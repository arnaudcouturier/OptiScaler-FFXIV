#include "pch.h"

#include "ext/ffxiv/diag/DrawDiag.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/diag/DrawDiagFormat.h"
#include "ext/ffxiv/diag/ShaderBindings.h"
#include "ext/ffxiv/diag/ShaderCrc.h" // the tag a draw's pixel shader is named by, shared with the modules that write a line about a draw
#include "ext/ffxiv/engine/EnginePass.h" // the engine pass overrides' counters, written to engine.txt
#include "ext/ffxiv/engine/GlossPuddlesApply.h" // the puddles' two stages, written to the same engine.txt
#include "ext/ffxiv/engine/PuddleTone.h"        // ... and the tone-map stage's last measurement of the pool and the march
#include "ext/ffxiv/engine/ShaftsRules.h"       // the pure sun projection engine.txt's sun line is computed with (it needs no card)
#include "ext/ffxiv/engine/SsrApply.h"    // the screen-space reflections' counters, written to the same engine.txt
#include "ext/ffxiv/lighting/FfxivLightingCapture.h" // the game's own context (the hooks' identity for "the game is drawing")
#include "ext/ffxiv/ui/PlateDraws.h"                 // the draw arguments the draw hooks already noted
#include "ext/ffxiv/ui/UiShaderCatalog.h"
#include "ext/present/PresentSeams.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Developer draw diagnostic (docs/FEATURES.md, "Developer diagnostics"). With [FFXIV] DrawDiagnostics = capture it writes what the game
// asks the GPU to draw into OptiScaler\draw-diag\<run>\ next to the DLL: one row per draw in draw order, with the pixel shader's CRC and
// stage, the reflected names and slots of the resources and constant buffers it binds, the render targets, the depth view, the viewport,
// the blend and depth state, and the markers of the tone map, the first UI draw and the upscale boundary. The column by column format is
// DrawDiagFormat.h.
//
// Log only: it reads D3D11 state, binds nothing, draws nothing and changes nothing. The only GPU work it adds is one staging copy of a
// constant buffer with one map, at most once a second and only while a capture runs.
//
// Cost with the key off: the one relaxed atomic load of Capturing() per draw (FfxivLightingCapture::Around) and one per Present.
//
// Threads: shader creation runs on whatever thread creates shaders and only appends to the reflection table under its own mutex; the draw,
// Present and evaluate hooks run on the render thread and share the frame state without a lock (the draw hooks and the Present entry are
// the same thread, which the draw hooks already assume). No D3D call is made while the reflection mutex is held.
namespace ext::ffxiv::diag
{
namespace
{
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

// The bytecode CRC the creation hook stamps on a shader object, so a draw can name its pixel shader without seeing its bytecode (the tag
// itself is diag::kShaderCrcTag: a module's own line names the same shader a capture would).

// The constant buffer the lighting passes carry, read back by name (its layout is in DrawDiagFormat.h: kLightParam*).
constexpr const char* kLightParamName = "g_LightParam";
// The camera buffer of the same draw: the view matrix beside the sun direction is what settles the direction's space.
constexpr const char* kCameraParamName = "g_CameraParameter";

enum Phase : uint8_t
{
    kIdle = 0,
    kWaiting = 1,   // armed with a delay or a key: the constant buffers are sampled, no draw row is collected
    kCapturing = 2, // the draw list is being collected
    kDone = 3,      // this session's capture is written; a config reload does not start another one
};

struct Bound
{
    uint32_t name = 0; // index into State::names
    BindKind kind = BindKind::Texture; // the kind the shader declares: what a slot is looked up by, and never compared by hand
    uint32_t bindPoint = 0;
    uint32_t bindCount = 1;
};

struct Reflection
{
    std::vector<Bound> binds;
};

int64_t Qpc()
{
    LARGE_INTEGER v {};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

// How many complete frames the key asks for, clamped to the range DrawDiagFormat.h documents.
uint32_t WantedFrames()
{
    const int32_t v = ext::Cfg().ffxiv.drawDiagnosticsFrames.value_or_default();
    return static_cast<uint32_t>(v < 1 ? 1 : (v > kMaxFrames ? kMaxFrames : v));
}

// When the capture starts, from the two keys beside the main one (DrawDiagFormat.h has the rules).
ArmPlan PlannedArm()
{
    return PlanArm(ext::Cfg().ffxiv.drawDiagnosticsDelaySeconds.value_or_default(),
                   ext::Cfg().ffxiv.drawDiagnosticsKey.value_or_default());
}

// A mapped staging copy of a game constant buffer. One copy and one map, only while a sample or a capture is due.
struct Staging
{
    ComPtr<ID3D11Buffer> buffer;
    UINT bytes = 0;
    D3D11_MAPPED_SUBRESOURCE mapped {};
    bool mappedOk = false;
};

// Copies `wanted` bytes of `source` into `staging` and maps it. Returns the mapped bytes, or null when the buffer is too small, the
// copy failed or the map failed; the caller unmaps with UnmapConstant in every case it mapped.
const unsigned char* MapConstant(ID3D11DeviceContext* c, Staging& staging, ID3D11Buffer* source, UINT wanted)
{
    staging.mappedOk = false;
    if (c == nullptr || source == nullptr || wanted == 0)
        return nullptr;
    D3D11_BUFFER_DESC desc {};
    source->GetDesc(&desc);
    if (desc.ByteWidth < wanted)
        return nullptr;
    if (!staging.buffer || staging.bytes != desc.ByteWidth)
    {
        ComPtr<ID3D11Device> device;
        c->GetDevice(&device);
        if (!device)
            return nullptr;
        D3D11_BUFFER_DESC create {};
        create.ByteWidth = desc.ByteWidth;
        create.Usage = D3D11_USAGE_STAGING;
        create.BindFlags = 0;
        create.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> created;
        if (FAILED(device->CreateBuffer(&create, nullptr, &created)))
            return nullptr;
        staging.buffer = created;
        staging.bytes = desc.ByteWidth;
    }
    c->CopyResource(staging.buffer.Get(), source);
    if (FAILED(c->Map(staging.buffer.Get(), 0, D3D11_MAP_READ, 0, &staging.mapped)) || staging.mapped.pData == nullptr)
        return nullptr;
    staging.mappedOk = true;
    return static_cast<const unsigned char*>(staging.mapped.pData);
}

void UnmapConstant(ID3D11DeviceContext* c, Staging& staging)
{
    if (staging.mappedOk && c != nullptr && staging.buffer)
        c->Unmap(staging.buffer.Get(), 0);
    staging.mappedOk = false;
}

// Whether the capture key is held down. Read with GetAsyncKeyState so the press does not need the game's window focused (a benchmark
// run has no way to focus a key into the game); only ever called while the diagnostic waits for it.
bool KeyDown(uint32_t key) { return (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0; }

int64_t QpcFrequency()
{
    LARGE_INTEGER f {};
    QueryPerformanceFrequency(&f);
    return f.QuadPart > 0 ? f.QuadPart : 1;
}

double SecondsBetween(int64_t from, int64_t to) { return from <= 0 ? 0.0 : double(to - from) / double(QpcFrequency()); }

struct State
{
    std::atomic<uint8_t> phase { kIdle };
    bool announced = false; // the "capturing" log line, written from the render thread (never from the config load)

    // Render thread only.
    ArmPlan arm;
    uint32_t wantedFrames = static_cast<uint32_t>(kDefaultFrames);
    uint64_t frames = 0; // frames closed since the capture started; the frame in progress is frames + 1
    uint32_t drawsInFrame = 0;
    // The capture's own draw counter: it keeps counting across a frame close and across the partial-frame drop, so "a later draw" has one
    // meaning within a capture (the row ordinal starts at 1 at every frame).
    uint64_t drawSequence = 0;
    uint32_t omittedInFrame = 0;
    uint32_t framesCaptured = 0;
    uint64_t framesWritten = 0; // frames whose text and frame line are in draws.csv (a frame the byte cap dropped is not counted)
    uint32_t waitTicks = 0; // draws since the last key poll while waiting for the capture to start
    FrameSplitter split;
    std::string frameRows;
    MarkerPlan markers;
    std::string rows; // draws.csv body
    std::string cbs;  // cb.csv body
    std::string cbdump; // cbdump.csv body: the named buffers' bytes, sampled at most once a second
    std::string framesCsv; // bounded per-output-frame SSR/puddle telemetry
    uint32_t frameTelemetryRows = 0;
    bool frameTelemetryTruncated = false;
    uint32_t previousSsrRuns = 0, previousSsrDeclines = 0;
    uint32_t previousGlossRuns = 0, previousGlossDeclines = 0;
    uint32_t previousToneRuns = 0, previousToneDeclines = 0;
    std::string views; // views.txt body: one line per identifying texture, once per capture
    bool viewSeen[kViewNameCount] {};
    bool truncated = false, truncatedCbs = false, truncatedCbDump = false;
    uint64_t rowsWritten = 0, cbRows = 0, cbDumpRows = 0, omittedTotal = 0, shadersWithoutReflection = 0;
    int64_t clockZero = 0;
    int64_t lastReadQpc = 0;
    // The last light pass the diagnostic read, in the form engine.txt's sun line needs: the direction the light draw reported (view space), the
    // projection the same draw's camera carried, and that draw's own window. It is filled whatever the light shafts card says, which is what the
    // line needed: that module only projects a sun while its own card runs. Arming a capture clears the read clock (`StartCollecting`), so the
    // capture's first light pass reads its own frame; a capture that holds no light pass keeps the waiting sample, and a sun a second old still
    // answers whether the engine's flare could be in frame.
    struct LightSample
    {
        bool have = false;
        float direction[3] {};
        float projection[16] {}; // m_ProjectionMatrix (+0x120): the light's direction is view space, so this alone places it
        float window[4] {};      // x, y, width, height of the light draw's viewport
    };
    LightSample lastLight;
    // The capture session's key: one press starts one capture, and a written capture needs a release before the next press counts.
    CaptureArm captureArm;
    fs::path runDir; // the run folder of the capture in progress, created on first use (the dump needs it at its frame's end)

    // The one-frame texture dump ([FFXIV] DrawDiagnosticsDump = frame): the resources identified in the first captured frame, the readback
    // copy of each, and what facts.txt says about them. `dumpWritten` stays set for the rest of the session: the dump is one frame of one
    // capture.
    struct DumpItem
    {
        std::string name;                // "wetness", "gbuffer0", ... ; the file is <name>.bin
        ComPtr<ID3D11Resource> resource; // identified at `sequence`; copied at a later draw, mapped at the dump's write
        ComPtr<ID3D11Texture2D> staging;
        std::string format;
        uint32_t width = 0, height = 0, bytesPerPixel = 0, rowPitch = 0;
        uint64_t bytes = 0;
        uint32_t ordinal = 0;  // the identifying draw's row ordinal, for facts.txt
        uint64_t sequence = 0; // the identifying draw in the capture's own order (the copy waits for a later one)
        uint8_t rank = 0;      // DumpRank
        bool copied = false;
        std::string note; // why the texture is not written (a skip decided while identifying, or a failed copy), for facts.txt
    };
    std::vector<DumpItem> dumpItems;
    uint64_t dumpHeldBytes = 0; // the bytes held for the staging copies, so identification stays under their bound
    bool dumpActive = false;
    bool dumpWritten = false;
    uint32_t dumpToneFrame = 0; // the frame whose tone map was identified (0 = not seen)
    bool dumpToneAtPresent = false; // that frame was closed at a bridge Present, so it is whole without a later frame
    uint64_t dumpToneSequence = 0;
    uint32_t dumpSceneWidth = 0, dumpSceneHeight = 0;
    int64_t cbDumpLast[kCbDumpNameCount] {}; // the last read of each named buffer, per name
    int64_t pointerWindowTicks = 0;          // the pointer cadence's window, converted from kSampleSeconds once a draw
    CbDumpPointerCadence cbDumpPointer[kCbDumpNameCount] {}; // the same cadence, per distinct pointer: the shadow parameter's
                                                             // cascade instances (CbDumpPointersPerSecond), which one name's
                                                             // row a second would fold into one
    uint64_t dumpBudget = kMaxDumpBytes;     // the one-frame dump's byte budget, from [FFXIV] DrawDiagnosticsDumpBudgetMiB
    Staging lightStaging, cameraStaging;
    std::vector<Staging> cbDumpStaging; // one staging buffer per distinct buffer size, reused for the session

    // The fixture dump: (crc, bytecode) of the pixel shaders whose reflection names one of the resources the fork identifies draws by
    // (DrawDiagFormat.h's FixtureBitsOf), kept for the run folder.
    struct Fixture
    {
        uint32_t crc = 0;
        std::vector<std::string> names;
        std::string code;
    };
    std::vector<Fixture> fixtures;
    size_t fixtureBytes = 0;
    size_t commonFixtures = 0; // the ones kept for a common name only, capped apart from the key shapes
    size_t lightFixtures = 0;  // the ones kept for the light draw's own shape, which keeps slots of its own

    // The shaders kept from creation (DrawDiagFormat.h's creation rules): the composite, the wetness pass, the hierarchical-Z and the glare
    // chain are created while the game loads, before any capture can be armed. Filled whatever the capture phase is, while the draw
    // diagnostic key is on, and written into every capture's shaders\ folder with `creation` as its origin. Session-wide and bounded.
    struct CreationFixture
    {
        uint32_t crc = 0;
        std::string code;
        std::vector<std::string> names;
    };
    std::mutex creationMutex;
    std::vector<CreationFixture> creationFixtures;
    uint64_t creationBytes = 0;
    bool creationBoundLogged = false;
    std::atomic<bool> creationOn { false }; // the draw diagnostic key is on: keep key shaders from creation, capture or not

    // The engine pass overrides' counters as they stood when the capture in progress armed: engine.txt's window lines are the difference
    // from this baseline, so a second capture of one session answers for its own window instead of the whole session. Taken where the phase
    // becomes `kCapturing`, before the first draw the capture collects.
    struct EngineBase
    {
        bool taken = false;
        uint32_t runs = 0;
        uint32_t declined = 0;
        uint32_t declines[ext::ffxiv::engine::kDeclineReasonCount] {};
    };
    EngineBase engineBase;

    // The screen-space reflections' counters at the same moment, for the second block of the same file: the module's own runs, readers,
    // declines and passed-on reasons, so a capture answers which of its gates stopped the water draws without a log.
    struct SsrBase
    {
        bool taken = false;
        uint32_t runs = 0;
        uint32_t readers = 0;
        uint32_t declined = 0;
        uint32_t declines[ext::ffxiv::engine::ssr::kDeclineReasonCount] {};
        uint32_t passedOn[ext::ffxiv::engine::ssr::kPassedOnCount] {};
    };
    SsrBase ssrBase;

    // The puddles' two stages at the same moment, for the last block of the same file: so a capture answers whether the mirror ran in its own
    // frames, not just whether the module was ever on.
    struct PuddleBase
    {
        bool taken = false;
        uint32_t toneRuns = 0;
    };
    PuddleBase puddleBase;

    // Reflection table: appended to by shader creation on any thread, read by the draw path under the same mutex. Kept for the session
    // (freeing it would race a creation that outlives the capture); bounded by the two caps below.
    std::mutex mutex;
    std::vector<std::string> names;
    std::unordered_map<std::string, uint32_t> nameIds;
    std::unordered_map<uint32_t, Reflection> byCrc;
    // The reflections of the shaders kept from creation, by CRC, under the same mutex and the same name pool. The table above is bounded and a
    // long session fills it (the game creates tens of thousands of pixel shaders), and the key passes it misses are exactly the ones created while
    // the game loads - the glare chain and the flare family among them. They are the shaders a capture has to name, so their reflections live in
    // their own bounded table instead of competing for the session's.
    std::unordered_map<uint32_t, Reflection> creationReflections;
    size_t bindings = 0;
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

fs::path OptiScalerDir()
{
    wchar_t path[32768] {};
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&OptiScalerDir), &module) ||
        !GetModuleFileNameW(module, path, 32768))
        return {};
    return fs::path(path).parent_path() / "OptiScaler";
}

// Creates OptiScaler\draw-diag\<run>\ and keeps the newest runs, deleting only folders it named itself. `capture` is the session's capture
// index: the first capture gets the plain run id, a later one of the same session the id with its index appended.
fs::path MakeRunFolder(uint32_t capture)
{
    const fs::path base = OptiScalerDir();
    if (base.empty())
    {
        LOG_WARN("Draw diagnostics: the OptiScaler folder could not be found");
        return {};
    }
    const fs::path root = base / kRootFolder;
    std::tm local {};
    const std::time_t now = std::time(nullptr);
    if (localtime_s(&local, &now) != 0)
    {
        LOG_WARN("Draw diagnostics: the local time could not be read");
        return {};
    }
    const std::string id = CaptureFolderName(local, capture);
    const fs::path dir = root / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec))
    {
        LOG_WARN("Draw diagnostics: cannot create the run folder: {}", ec.message());
        return {};
    }
    std::vector<std::string> names;
    ec.clear();
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code isDir;
        if (it->is_directory(isDir) && !isDir)
            names.push_back(it->path().filename().string());
    }
    if (!ec)
        for (const auto& old : RunsToPrune(names))
        {
            if (old == id)
                continue;
            std::error_code rm;
            fs::remove_all(root / old, rm);
        }
    return dir;
}

bool WriteFile(const fs::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush();
    return file.good();
}

bool WriteBytes(const fs::path& path, const void* data, size_t bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    file.flush();
    return file.good();
}

// The run folder of the capture in progress, created on first use: the one-frame dump needs it at its frame's end, the files at the end of
// the capture. The capture index names the folder of a re-armed capture.
fs::path RunFolder()
{
    State& s = S();
    if (s.runDir.empty())
        s.runDir = MakeRunFolder(s.captureArm.captures);
    return s.runDir;
}

// Takes the engine counters a capture's window is measured from: the counts as they stand the moment the capture arms, before it collects a
// draw. Called from both arm sites (the immediate arm in the config load and the delayed or key arm at a draw), so every capture has a
// baseline. It only reads the module's counters and atomics: no allocation, no D3D call and no log, so the config-load arm may run inside
// DllMain.
void TakeEngineBaseline(State& s)
{
    const ext::ffxiv::engine::Diagnostics diagnostics = ext::ffxiv::engine::CurrentDiagnostics();
    s.engineBase.taken = true;
    s.engineBase.runs = diagnostics.runs;
    s.engineBase.declined = diagnostics.declined;
    for (uint32_t reason = 0; reason < ext::ffxiv::engine::kDeclineReasonCount; ++reason)
        s.engineBase.declines[reason] = diagnostics.declines[reason];
}

// The puddles' tone-map runs at the same moment, for the last block of engine.txt.
void TakePuddleBaseline(State& s)
{
    s.puddleBase.taken = true;
    s.puddleBase.toneRuns = ext::ffxiv::engine::gloss::CurrentToneDiagnostics().runs;
}

// The screen-space reflections' counters at the same moment, for the second block of engine.txt: taken wherever the engine's baseline is, so the
// two windows of one file are the same draws.
void TakeSsrBaseline(State& s)
{
    const ext::ffxiv::engine::ssr::Diagnostics diagnostics = ext::ffxiv::engine::ssr::CurrentDiagnostics();
    s.ssrBase.taken = true;
    s.ssrBase.runs = diagnostics.runs;
    s.ssrBase.readers = diagnostics.readers;
    s.ssrBase.declined = diagnostics.declined;
    for (uint32_t reason = 0; reason < ext::ffxiv::engine::ssr::kDeclineReasonCount; ++reason)
        s.ssrBase.declines[reason] = diagnostics.declines[reason];
    for (uint32_t reason = 0; reason < ext::ffxiv::engine::ssr::kPassedOnCount; ++reason)
        s.ssrBase.passedOn[reason] = diagnostics.passedOn[reason];
}

// The puddles' counters as the last block of engine.txt carries them: both stages, each refusal reason named by the module's own text, the tone
// stage's last measurement of the pool and the march (only a capture takes one), and this capture's own share of the tone runs.
ext::ffxiv::engine::gloss::PuddleStatus PuddleStatusNow()
{
    const ext::ffxiv::engine::gloss::Diagnostics gloss = ext::ffxiv::engine::gloss::CurrentDiagnostics();
    const ext::ffxiv::engine::gloss::ToneDiagnostics tone = ext::ffxiv::engine::gloss::CurrentToneDiagnostics();
    ext::ffxiv::engine::gloss::PuddleStatus status;
    status.keyOn = gloss.key;
    status.glossArmed = gloss.armed;
    status.toneArmed = tone.armed;
    status.glossRuns = gloss.runs;
    status.glossReaders = gloss.readers;
    status.toneRuns = tone.runs;
    status.glossDeclined = gloss.declined;
    status.toneDeclined = tone.declined;
    for (uint32_t reason = 1; reason < ext::ffxiv::engine::gloss::kDeclineReasonCount; ++reason) // None is the run count, not a decline
        if (gloss.declines[reason] != 0)
            status.declines.emplace_back(std::string("gloss: ") + ext::ffxiv::engine::gloss::DeclineName(ext::ffxiv::engine::gloss::Decline(reason)),
                                         gloss.declines[reason]);
    for (uint32_t reason = 1; reason < ext::ffxiv::engine::gloss::kToneDeclineReasonCount; ++reason)
        if (tone.declines[reason] != 0)
            status.declines.emplace_back(std::string("tone: ") + ext::ffxiv::engine::gloss::ToneDeclineName(
                                                                     ext::ffxiv::engine::gloss::ToneDecline(reason)),
                                         tone.declines[reason]);
    status.statsKnown = tone.statsKnown;
    status.poolShare = tone.poolShare;
    status.hitShare = tone.hitShare;
    const State::PuddleBase& baseline = S().puddleBase;
    if (baseline.taken)
    {
        status.haveWindow = true;
        status.windowToneRuns = WindowCounter(tone.runs, baseline.toneRuns);
    }
    return status;
}

// The engine pass overrides' counters as engine.txt carries them: the module's own export, with each nonzero decline reason named by the
// module's own text. The key is what decides whether the file says anything else. The window lines are this capture's share of the session
// counters: the difference from the baseline `TakeEngineBaseline` took when it armed.
EngineStatus EngineStatusNow()
{
    const ext::ffxiv::engine::Diagnostics diagnostics = ext::ffxiv::engine::CurrentDiagnostics();
    EngineStatus status;
    status.keyOn = diagnostics.key;
    status.armed = diagnostics.armed;
    status.identity = diagnostics.identity;
    status.runs = diagnostics.runs;
    status.declined = diagnostics.declined;
    for (uint32_t reason = 1; reason < ext::ffxiv::engine::kDeclineReasonCount; ++reason) // None is the run count, not a decline
        if (diagnostics.declines[reason] != 0)
            status.declines.emplace_back(ext::ffxiv::engine::DeclineName(ext::ffxiv::engine::Decline(reason)), diagnostics.declines[reason]);
    const State::EngineBase& base = S().engineBase;
    if (base.taken)
    {
        status.haveWindow = true;
        status.windowRuns = WindowCounter(diagnostics.runs, base.runs);
        status.windowDeclined = WindowCounter(diagnostics.declined, base.declined);
        for (uint32_t reason = 1; reason < ext::ffxiv::engine::kDeclineReasonCount; ++reason)
            if (const uint32_t count = WindowCounter(diagnostics.declines[reason], base.declines[reason]); count != 0)
                status.windowDeclines.emplace_back(ext::ffxiv::engine::DeclineName(ext::ffxiv::engine::Decline(reason)), count);
    }
    status.haveRun = diagnostics.haveRun;
    status.boundBytes = diagnostics.boundBytes;
    status.targetWidth = diagnostics.targetWidth;
    status.targetHeight = diagnostics.targetHeight;
    status.indexed = diagnostics.indexed;
    status.indexRefused = diagnostics.indexRefused;
    // The frame's sun, projected here from the diagnostic's own last light read: the direction the light draw reported (already in view space),
    // through the projection the same draw's camera carried, into that draw's window. It does not depend on any other module's card being on (the
    // light shafts module only projects a sun while its own card runs), so the line is there in the state the engine's flare is captured in.
    const State::LightSample& light = S().lastLight;
    if (light.have)
    {
        const ext::ffxiv::shafts::Window window { light.window[0], light.window[1], light.window[2], light.window[3] };
        const ext::ffxiv::shafts::SunProjection sun = ext::ffxiv::shafts::ProjectSun(light.projection, light.direction, window);
        status.haveSun = sun.unit;
        status.sunBehind = sun.behind;
        status.sunX = sun.screenX;
        status.sunY = sun.screenY;
        status.sunOutside = sun.offscreen;
    }
    for (size_t i = 0; i < 4; ++i)
        status.window[i] = diagnostics.window[i];
    return status;
}

// The screen-space reflections' counters as the second block of engine.txt carries them: the module's own export, each nonzero reason named by
// the module's own text, and the same counts for this capture's window (the difference from the baseline taken when it armed).
SsrStatus SsrStatusNow()
{
    const ext::ffxiv::engine::ssr::Diagnostics diagnostics = ext::ffxiv::engine::ssr::CurrentDiagnostics();
    SsrStatus status;
    status.keyOn = diagnostics.key;
    status.armed = diagnostics.armed;
    status.runs = diagnostics.runs;
    status.readers = diagnostics.readers;
    status.declined = diagnostics.declined;
    for (uint32_t reason = 1; reason < ext::ffxiv::engine::ssr::kDeclineReasonCount; ++reason) // None is not a decline
        if (diagnostics.declines[reason] != 0)
            status.declines.emplace_back(ext::ffxiv::engine::ssr::DeclineName(ext::ffxiv::engine::ssr::Decline(reason)),
                                         diagnostics.declines[reason]);
    for (uint32_t reason = 0; reason < ext::ffxiv::engine::ssr::kPassedOnCount; ++reason)
        if (diagnostics.passedOn[reason] != 0)
            status.passedOn.emplace_back(ext::ffxiv::engine::ssr::PassedOnName(ext::ffxiv::engine::ssr::PassedOn(reason)),
                                         diagnostics.passedOn[reason]);
    const State::SsrBase& baseline = S().ssrBase;
    if (baseline.taken)
    {
        status.haveWindow = true;
        status.windowRuns = WindowCounter(diagnostics.runs, baseline.runs);
        status.windowReaders = WindowCounter(diagnostics.readers, baseline.readers);
        status.windowDeclined = WindowCounter(diagnostics.declined, baseline.declined);
        // The slot and the view of the last water draw the gate passed on, this capture's only: the module keeps one read for the session, so
        // the line answers for the capture's window when that window passed a water draw on (the count of the reason that stores the read, from
        // the baseline taken when it armed) and reads `-` otherwise - a second capture of one session must not print the first one's slot.
        constexpr uint32_t kReadPassedOn = uint32_t(ext::ffxiv::engine::ssr::PassedOn::OtherMap);
        if (WindowCounter(diagnostics.passedOn[kReadPassedOn], baseline.passedOn[kReadPassedOn]) > 0)
            status.lastWaterDraw = ext::ffxiv::engine::ssr::FormatReadBinding(diagnostics.read);
        for (uint32_t reason = 1; reason < ext::ffxiv::engine::ssr::kDeclineReasonCount; ++reason)
            if (const uint32_t count = WindowCounter(diagnostics.declines[reason], baseline.declines[reason]); count != 0)
                status.windowDeclines.emplace_back(ext::ffxiv::engine::ssr::DeclineName(ext::ffxiv::engine::ssr::Decline(reason)), count);
        for (uint32_t reason = 0; reason < ext::ffxiv::engine::ssr::kPassedOnCount; ++reason)
            if (const uint32_t count = WindowCounter(diagnostics.passedOn[reason], baseline.passedOn[reason]); count != 0)
                status.windowPassedOn.emplace_back(ext::ffxiv::engine::ssr::PassedOnName(ext::ffxiv::engine::ssr::PassedOn(reason)), count);
    }
    return status;
}

// ---- reflection table ------------------------------------------------------------------------------------------------

// Interns a name. The pool is bounded and a full pool simply stops new names, so a binding is then nameless rather than wrong.
uint32_t Intern(State& s, const char* name, uint32_t length)
{
    std::string text(name, length);
    const auto it = s.nameIds.find(text);
    if (it != s.nameIds.end())
        return it->second;
    const uint32_t id = static_cast<uint32_t>(s.names.size());
    s.names.push_back(text);
    s.nameIds.emplace(std::move(text), id);
    return id;
}

// The binding a shader declares at `slot`, or null. The kind is part of the question: a texture and its sampler share one variable name in
// the effect compiler's reflection, so a walk of the draw's slots that ignored the kind would name a texture after its sampler.
const char* NameAt(const State& s, const Reflection* reflection, BindKind kind, uint32_t slot)
{
    if (reflection == nullptr)
        return nullptr;
    for (const Bound& b : reflection->binds)
    {
        const ShaderBinding probe { s.names[b.name].c_str(), static_cast<uint32_t>(s.names[b.name].size()), uint32_t(b.kind), b.bindPoint,
                                    b.bindCount };
        if (BindingAnswers(probe, kind, slot))
            return probe.name;
    }
    return nullptr;
}

// The reflection a draw's pixel shader is described by, or null when the session never saw its bytecode. The session's table first (it is what a
// capture collects), then the shaders kept from creation: a long session fills the first, and the passes created while the game loads are the
// ones a capture needs to name. Called with the reflection mutex held.
const Reflection* ReflectionFor(const State& s, uint32_t crc)
{
    const auto it = s.byCrc.find(crc);
    if (it != s.byCrc.end())
        return &it->second;
    const auto creation = s.creationReflections.find(crc);
    return creation != s.creationReflections.end() ? &creation->second : nullptr;
}

// ---- text of the D3D state -------------------------------------------------------------------------------------------

const char* FormatName(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return "R16G16B16A16_UNORM";
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_R8G8_UNORM:
        return "R8G8_UNORM";
    case DXGI_FORMAT_R8_UNORM:
        return "R8_UNORM";
    case DXGI_FORMAT_R32_FLOAT:
        return "R32_FLOAT";
    case DXGI_FORMAT_R32_UINT:
        return "R32_UINT";
    case DXGI_FORMAT_R16_FLOAT:
        return "R16_FLOAT";
    case DXGI_FORMAT_R16G16_FLOAT:
        return "R16G16_FLOAT";
    case DXGI_FORMAT_R24G8_TYPELESS:
        return "R24G8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return "R24_UNORM_X8_TYPELESS";
    case DXGI_FORMAT_R32_TYPELESS:
        return "R32_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT:
        return "D32_FLOAT";
    case DXGI_FORMAT_BC1_UNORM:
        return "BC1_UNORM";
    case DXGI_FORMAT_BC7_UNORM:
        return "BC7_UNORM";
    default:
        return nullptr;
    }
}

// The format a reader compares: by name for the common ones, by number for everything else, so one column carries both.
std::string FormatText(uint32_t format)
{
    if (const char* name = FormatName(static_cast<DXGI_FORMAT>(format)))
        return name;
    return "fmt" + std::to_string(format);
}

// The description of the texture behind a view: the size column of a row and the views file both read it. A resource that is not a 2D
// texture yields an invalid result rather than a guess, and the column then says "?".
struct TextureInfo
{
    uint32_t width = 0, height = 0, arraySize = 0, mips = 0, format = 0, sampleCount = 1;
    bool valid = false;
};

TextureInfo ReadTextureInfo(ID3D11Resource* resource)
{
    TextureInfo info;
    if (resource == nullptr)
        return info;
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return info;
    D3D11_TEXTURE2D_DESC desc {};
    static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
    info.width = desc.Width;
    info.height = desc.Height;
    info.arraySize = desc.ArraySize;
    info.mips = desc.MipLevels;
    info.format = uint32_t(desc.Format);
    info.sampleCount = desc.SampleDesc.Count;
    info.valid = true;
    return info;
}

std::string SizeText(uint32_t width, uint32_t height) { return std::to_string(width) + "x" + std::to_string(height); }

// One number per field, in the D3D11 enum order: enable, src, dst, op, srcAlpha, dstAlpha, opAlpha, write mask. With no blend state
// object bound D3D11 uses its default, which is blending off and every channel written. `blends` answers the same question the first number
// does, for the light-buffer shape rule: a draw that blends into its targets is one of the local lights, not the frame's light pass.
std::string BlendText(ID3D11DeviceContext* c, bool& blends)
{
    ComPtr<ID3D11BlendState> state;
    FLOAT factor[4] {};
    UINT mask = 0;
    c->OMGetBlendState(&state, factor, &mask);
    D3D11_BLEND_DESC desc {};
    if (state)
        state->GetDesc(&desc);
    else
    {
        desc.RenderTarget[0].BlendEnable = FALSE;
        desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    const D3D11_RENDER_TARGET_BLEND_DESC& t = desc.RenderTarget[0];
    blends = desc.IndependentBlendEnable ? (desc.RenderTarget[0].BlendEnable || desc.RenderTarget[1].BlendEnable) != FALSE
                                         : desc.RenderTarget[0].BlendEnable != FALSE;
    char text[96];
    snprintf(text, sizeof text, "%u/%u/%u/%u/%u/%u/%u/%u", t.BlendEnable ? 1u : 0u, uint32_t(t.SrcBlend), uint32_t(t.DestBlend),
             uint32_t(t.BlendOp), uint32_t(t.SrcBlendAlpha), uint32_t(t.DestBlendAlpha), uint32_t(t.BlendOpAlpha),
             uint32_t(t.RenderTargetWriteMask));
    return text;
}

// enable, write mask (1 = writes depth), comparison, stencil. With no depth-stencil state object bound D3D11 uses its default: depth
// on, writes on, LESS, stencil off.
std::string DepthText(ID3D11DeviceContext* c)
{
    ComPtr<ID3D11DepthStencilState> state;
    UINT ref = 0;
    c->OMGetDepthStencilState(&state, &ref);
    D3D11_DEPTH_STENCIL_DESC desc {};
    if (state)
        state->GetDesc(&desc);
    else
    {
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        desc.DepthFunc = D3D11_COMPARISON_LESS;
        desc.StencilEnable = FALSE;
    }
    char text[64];
    snprintf(text, sizeof text, "%u/%u/%u/%u", desc.DepthEnable ? 1u : 0u,
             desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL ? 1u : 0u, uint32_t(desc.DepthFunc), desc.StencilEnable ? 1u : 0u);
    return text;
}

// All viewports of the draw, up to four: "count:w x h + x + y|...".
std::string ViewportText(ID3D11DeviceContext* c)
{
    D3D11_VIEWPORT viewports[4] {};
    UINT count = 4;
    c->RSGetViewports(&count, viewports);
    if (count == 0)
        return {};
    std::string text = std::to_string(count) + ":";
    for (UINT i = 0; i < count && i < 4; ++i)
    {
        if (i != 0)
            text += '|';
        text += detail::Number(viewports[i].Width);
        text += 'x';
        text += detail::Number(viewports[i].Height);
        text += '+';
        text += detail::Number(viewports[i].TopLeftX);
        text += '+';
        text += detail::Number(viewports[i].TopLeftY);
    }
    return text;
}

// Every bound render target as "slot:format:size:pointer|..." in the render target column, and the depth view as "format:size:pointer"
// in `depth`. The view's resource is held so a caller can name it (the pointer column) and copy it later (the one-frame dump).
struct TargetInfo
{
    ComPtr<ID3D11Resource> resource;
    uint32_t format = 0;
    TextureInfo texture;
};

struct DrawTargets
{
    TargetInfo rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    uint32_t bound = 0; // how many of the eight slots held a view
    TargetInfo dsv;
    bool hasDsv = false;
};

DrawTargets ReadTargets(ID3D11DeviceContext* c)
{
    DrawTargets targets;
    ID3D11RenderTargetView* views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] {};
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, views, &dsv);
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        if (views[i] == nullptr)
            continue;
        D3D11_RENDER_TARGET_VIEW_DESC desc {};
        views[i]->GetDesc(&desc);
        targets.rtv[i].format = uint32_t(desc.Format);
        views[i]->GetResource(&targets.rtv[i].resource);
        targets.rtv[i].texture = ReadTextureInfo(targets.rtv[i].resource.Get());
        ++targets.bound;
        views[i]->Release();
    }
    if (dsv != nullptr)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc {};
        dsv->GetDesc(&desc);
        targets.dsv.format = uint32_t(desc.Format);
        dsv->GetResource(&targets.dsv.resource);
        targets.dsv.texture = ReadTextureInfo(targets.dsv.resource.Get());
        targets.hasDsv = true;
        dsv->Release();
    }
    return targets;
}

std::string RenderTargetText(const DrawTargets& targets, std::string& depth)
{
    std::string text;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        if (!targets.rtv[i].resource)
            continue;
        const TextureInfo& texture = targets.rtv[i].texture;
        if (!text.empty())
            text += '|';
        text += TargetEntryText(i, FormatText(targets.rtv[i].format), texture.valid ? SizeText(texture.width, texture.height) : "?",
                                targets.rtv[i].resource.Get());
    }
    if (targets.hasDsv)
    {
        const TextureInfo& texture = targets.dsv.texture;
        depth = DepthEntryText(FormatText(targets.dsv.format), texture.valid ? SizeText(texture.width, texture.height) : "?",
                               targets.dsv.resource.Get());
    }
    return text;
}

// ---- the one-frame texture dump --------------------------------------------------------------------------------------

// The bytes per pixel of the formats a dump writes. A compressed or unfamiliar format gets 0 and is skipped with a line in facts.txt; a
// typeless format is written as it is, because that is the format the engine's targets have.
uint32_t DumpPixelBytes(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_A8_UNORM: return 1;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_B5G6R5_UNORM: return 2;
    case DXGI_FORMAT_D16_UNORM: return 2; // a 16-bit depth target, not the 32-bit group below
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_D32_FLOAT: return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32_FLOAT: return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return 16;
    default: return 0;
    }
}

// The bound texture slots of one draw, as the draw collected them.
struct SrvSlot
{
    bool bound = false;
    uint32_t format = 0, viewDimension = 0, viewMips = 0, arraySize = 0, cubes = 0;
    std::string formatText;
    TextureInfo texture;
    ComPtr<ID3D11Resource> resource;
};

bool DumpHeld(const State& s, std::string_view name)
{
    for (const State::DumpItem& item : s.dumpItems)
        if (item.name == name)
            return true;
    return false;
}

// Whether `name` is already held on `resource`: the water gate's light pair must not dump the texels the main light names already hold when a
// water draw binds the main pair under the same reflected names.
bool HoldsResource(const State& s, std::string_view name, ID3D11Resource* resource)
{
    if (resource == nullptr)
        return false;
    for (const State::DumpItem& item : s.dumpItems)
        if (item.name == name && item.resource.Get() == resource)
            return true;
    return false;
}

// The items that can still become a file. A skipped texture is held as an item too (with its reason), and it must not use up one of the
// twenty places the written files share.
size_t DumpCount(const State& s)
{
    size_t count = 0;
    for (const State::DumpItem& item : s.dumpItems)
        count += item.note.empty() ? 1 : 0;
    return count;
}

// Whether the shader declares `name`, compared by the variable's name (a texture arrives as `<name>.T`). The caller holds the mutex.
bool ReflectionHas(const State& s, const Reflection* reflection, std::string_view name)
{
    if (reflection == nullptr)
        return false;
    const std::string_view wanted = ResourceBaseName(name);
    for (const Bound& bound : reflection->binds)
        if (ResourceBaseName(s.names[bound.name]) == wanted)
            return true;
    return false;
}

// The reflected name of a bound SRV without the compiler's suffix, or an empty name when the shader declares none.
std::string_view SrvName(const State& s, const Reflection* reflection, size_t slot)
{
    const char* name = NameAt(s, reflection, BindKind::Texture, static_cast<uint32_t>(slot));
    return name != nullptr ? ResourceBaseName(name) : std::string_view {};
}

// Identifies one resource for the dump, from the description the draw's own state reads already took (`info`), so no D3D call is made here:
// the caller may hold the reflection mutex. Only the reference is kept; the copy happens at a later draw, because the hook runs before the
// draw, so a target read at the draw that writes it would still hold what was there before.
void DumpIdentify(State& s, std::string_view name, ID3D11Resource* resource, const TextureInfo& info, uint32_t ordinal, uint64_t sequence,
                  DumpRank rank)
{
    if (!DumpAdds(s.dumpActive, s.dumpWritten, resource, DumpHeld(s, name), DumpCount(s)))
        return;
    // A texture the dump cannot take is held as an item too, carrying the reason and the rank of its kind (so facts.txt keeps the priority
    // order between a written texture and a skip): its line is written once, and a later draw that identifies the same name does not add
    // it again.
    const auto skip = [&s, name, rank](std::string reason)
    {
        State::DumpItem item;
        item.name = std::string(name);
        item.note = std::move(reason);
        item.rank = uint8_t(rank);
        s.dumpItems.push_back(std::move(item));
    };
    if (!info.valid)
    {
        skip("not a 2D texture");
        return;
    }
    if (info.sampleCount > 1)
    {
        skip("multisampled");
        return;
    }
    const uint32_t bytesPerPixel = DumpPixelBytes(static_cast<DXGI_FORMAT>(info.format));
    if (bytesPerPixel == 0)
    {
        skip("format " + std::to_string(info.format) + " not written");
        return;
    }
    State::DumpItem item;
    item.name = std::string(name);
    item.resource = resource;
    item.format = FormatText(info.format);
    item.width = info.width;
    item.height = info.height;
    item.bytesPerPixel = bytesPerPixel;
    item.bytes = uint64_t(info.width) * info.height * bytesPerPixel;
    // The copies are bounded here, not only when the files are written: twenty staging textures of a 4K frame are about a gigabyte.
    // A texture of a higher priority gives the holds of a lower priority their place back first (facts.txt names the newcomer that took
    // it), so the wetness target is not displaced by the G-buffer and the light buffers merely because they came first.
    if (!DumpIdentFits(s.dumpHeldBytes, item.bytes, s.dumpBudget))
    {
        DumpHold holds[kMaxDumpTextures];
        size_t count = 0;
        for (const State::DumpItem& held : s.dumpItems)
            if (held.note.empty() && held.bytes != 0 && count < kMaxDumpTextures)
                holds[count++] = DumpHold { DumpRank(held.rank), held.bytes, false };
        const uint64_t freed = DumpMakeRoom(holds, count, rank, item.bytes, s.dumpHeldBytes, s.dumpBudget);
        size_t index = 0;
        for (State::DumpItem& held : s.dumpItems)
        {
            if (!held.note.empty() || held.bytes == 0)
                continue;
            if (index < count && holds[index].released)
            {
                held.note = "displaced by " + std::string(name);
                held.staging.Reset();
                held.resource.Reset();
            }
            ++index;
        }
        s.dumpHeldBytes -= freed;
    }
    if (!DumpIdentFits(s.dumpHeldBytes, item.bytes, s.dumpBudget))
    {
        skip("over the dump's bound");
        return;
    }
    item.ordinal = ordinal;
    item.sequence = sequence;
    item.rank = uint8_t(rank);
    s.dumpHeldBytes += item.bytes;
    s.dumpItems.push_back(std::move(item));
}

// Queues the staging copy of one identified resource: subresource 0 only, as the resource describes itself (a typeless format stays
// typeless). A failure is recorded in the item's note for facts.txt and does not stop the others; the caller gives the bytes back to the
// running total, because a texture without a copy can never become a file.
bool DumpCopyItem(ID3D11Device* device, ID3D11DeviceContext* c, State::DumpItem& item)
{
    ComPtr<ID3D11Texture2D> texture;
    item.resource.As(&texture);
    if (!texture)
    {
        item.note = "not a 2D texture";
        return false;
    }
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)) || !staging)
    {
        item.note = "the staging copy could not be created";
        return false;
    }
    c->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture.Get(), 0, nullptr);
    item.staging = staging;
    item.copied = true;
    return true;
}

// A copy that did not happen frees the bytes it held: the running total is what the bound is enforced against, and the texture's place
// must go to one that can still be read back.
void DumpReleaseBytes(State& s, const State::DumpItem& item)
{
    s.dumpHeldBytes = s.dumpHeldBytes > item.bytes ? s.dumpHeldBytes - item.bytes : 0;
}

// Copies the resources the dump identified before this draw, at the draw itself: the hook runs before the game's draw, so the copy holds
// what the previous draws wrote. The G-buffer targets are left to DumpCopySampled, because their pass is not over yet.
void DumpCopyPending(ID3D11DeviceContext* c, State& s, uint64_t sequence)
{
    if (!s.dumpActive || s.dumpWritten || s.dumpItems.empty() || c == nullptr)
        return;
    ComPtr<ID3D11Device> device;
    for (State::DumpItem& item : s.dumpItems)
    {
        if (!DumpCopyDue(item.copied, !item.note.empty(), item.sequence, sequence) || DumpWaitsForSampling(DumpRank(item.rank)))
            continue;
        if (!device)
        {
            c->GetDevice(&device);
            if (!device)
                return;
        }
        if (!DumpCopyItem(device.Get(), c, item))
            DumpReleaseBytes(s, item);
    }
}

// The G-buffer targets are copied at the first later draw that binds one of them as a sampled texture: that is the point the geometry pass
// that writes them is over. Called after the draw's texture slots were read and before the reflection mutex is taken, so it makes no D3D
// call under it.
void DumpCopySampled(ID3D11DeviceContext* c, State& s, const SrvSlot* srv, uint64_t sequence)
{
    if (!s.dumpActive || s.dumpWritten || s.dumpItems.empty() || c == nullptr)
        return;
    // The resources this draw binds in its texture slots, so the match itself is the same rule the unit test walks.
    const void* bound[kMaxSlots] {};
    for (size_t i = 0; i < kMaxSlots; ++i)
        bound[i] = srv[i].bound ? srv[i].resource.Get() : nullptr;
    ComPtr<ID3D11Device> device;
    for (State::DumpItem& item : s.dumpItems)
    {
        if (!DumpWaitsForSampling(DumpRank(item.rank)) || !DumpCopyDue(item.copied, !item.note.empty(), item.sequence, sequence))
            continue;
        if (DumpSampledSlot(bound, kMaxSlots, item.resource.Get()) < 0)
            continue;
        if (!device)
        {
            c->GetDevice(&device);
            if (!device)
                return;
        }
        if (!DumpCopyItem(device.Get(), c, item))
            DumpReleaseBytes(s, item);
    }
}

// Writes the dump: each staging copy is mapped, packed row by row with the readback's real RowPitch, and its .bin gets a line in facts.txt.
// The files are taken in DumpRank order, so the byte bound never gives the wetness target's place away to a later copy.
void DumpWrite(ID3D11DeviceContext* c, State& s)
{
    if (!s.dumpActive || s.dumpWritten || c == nullptr)
        return;
    s.dumpWritten = true;
    // The items move out of the state: every hold on a game texture, and every staging copy, is released whichever way this returns.
    std::vector<State::DumpItem> items = std::move(s.dumpItems);
    s.dumpItems.clear();
    s.dumpHeldBytes = 0;
    const fs::path run = RunFolder();
    if (run.empty())
    {
        LOG_WARN("Draw diagnostics: the one-frame dump was not written (the run folder)");
        return;
    }
    const fs::path dir = run / kDumpFolder;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
    {
        LOG_WARN("Draw diagnostics: the dump folder could not be created: {}", ec.message());
        return;
    }
    std::vector<State::DumpItem*> order;
    order.reserve(items.size());
    for (State::DumpItem& item : items)
        order.push_back(&item);
    std::stable_sort(order.begin(), order.end(), [](const State::DumpItem* a, const State::DumpItem* b) { return a->rank < b->rank; });
    std::string facts = DumpFactsHeader();
    uint32_t written = 0;
    uint64_t bytes = 0;
    for (State::DumpItem* item : order)
    {
        if (!item->copied || !item->staging)
        {
            facts += DumpSkipText(item->name, item->note.empty() ? "not copied" : item->note);
            continue;
        }
        if (!DumpFits(written, bytes, item->bytes, s.dumpBudget))
        {
            facts += DumpSkipText(item->name, "over the dump's bound");
            item->staging.Reset();
            item->resource.Reset();
            continue;
        }
        const void* pointer = item->resource.Get();
        // The packed image is allocated before the copy is mapped: an allocation failure must not leave the staging texture mapped.
        std::vector<unsigned char> pixels(size_t(item->width) * item->height * item->bytesPerPixel);
        D3D11_MAPPED_SUBRESOURCE map {};
        if (FAILED(c->Map(item->staging.Get(), 0, D3D11_MAP_READ, 0, &map)) || map.pData == nullptr)
        {
            facts += DumpSkipText(item->name, "the staging copy could not be mapped");
            item->staging.Reset();
            item->resource.Reset();
            continue;
        }
        item->rowPitch = map.RowPitch;
        const auto* rows = static_cast<const unsigned char*>(map.pData);
        for (uint32_t y = 0; y < item->height; ++y)
            std::memcpy(pixels.data() + size_t(y) * item->width * item->bytesPerPixel, rows + size_t(y) * map.RowPitch,
                        size_t(item->width) * item->bytesPerPixel);
        c->Unmap(item->staging.Get(), 0);
        item->staging.Reset();
        item->resource.Reset();
        if (!WriteBytes(dir / (item->name + ".bin"), pixels.data(), pixels.size()))
        {
            facts += DumpSkipText(item->name, "the write failed");
            continue;
        }
        DumpFact fact;
        fact.name = item->name;
        fact.format = item->format;
        fact.width = item->width;
        fact.height = item->height;
        fact.bytes = pixels.size();
        fact.rowPitch = item->rowPitch;
        fact.ordinal = item->ordinal;
        fact.pointer = pointer;
        facts += DumpFactText(fact);
        ++written;
        bytes += item->bytes;
    }
    if (!WriteFile(dir / kDumpFactsFile, facts))
        LOG_WARN("Draw diagnostics: the dump's facts.txt could not be written");
    else
        LOG_INFO("Draw diagnostics: one-frame dump of {} texture(s) written to {}", written, dir.string());
}

// Writes the dump once the frame that identified its tone map is over and its staging copies exist. A bridge Present has no context, so
// the next draw writes it; a capture of a single frame ends before its copies can be mapped, and the end of the capture says so.
void MaybeWriteDump(ID3D11DeviceContext* c, State& s)
{
    if (!s.dumpActive || s.dumpWritten || !DumpFrameOver(s.frames, s.dumpToneFrame, s.dumpToneAtPresent))
        return;
    DumpWrite(c, s);
}

// The one-frame dump at the end of a capture, with the game's own context: a bridge Present closes the frame with no context, so the draw
// path could not map the readbacks there and a two-frame capture would otherwise lose the dump that was just collected.
void FinishDump(State& s)
{
    if (!s.dumpActive || s.dumpWritten || !DumpFrameOver(s.frames, s.dumpToneFrame, s.dumpToneAtPresent))
        return;
    ID3D11DeviceContext* c = ext::ffxiv::lighting::FfxivLightingCapture::context;
    if (c == nullptr)
        return;
    ComPtr<ID3D11Device> device;
    // Every missing copy that may still be taken is queued now (what DumpAtEnd says: a bridge Present has already run the frame's last
    // draw, the tone-map split has not, and a G-buffer target is skipped because its frame is over).
    for (State::DumpItem& item : s.dumpItems)
    {
        if (item.copied || !item.note.empty())
            continue;
        switch (DumpAtEnd(DumpRank(item.rank), item.sequence, s.drawSequence, s.dumpToneAtPresent))
        {
        case DumpEnd::Skip:
            // A copy now would hold a later frame's content under the identifying draw's name.
            item.note = "not sampled";
            continue;
        case DumpEnd::Leave: continue;
        case DumpEnd::Copy: break;
        }
        if (!device)
        {
            c->GetDevice(&device);
            if (!device)
                break;
        }
        if (!DumpCopyItem(device.Get(), c, item))
            DumpReleaseBytes(s, item);
    }
    DumpWrite(c, s);
}

// What one draw contributes to the dump. Called with the reflected binding table in hand, because the wetness pass and the light draws are
// identified by the names their pixel shader declares and binds, not by a bytecode CRC. `sequence` is the draw in the capture's own order.
void DumpNoteDraw(State& s, const DrawRow& row, const DrawTargets& targets, const SrvSlot* srv, const Reflection* reflection,
                  uint64_t sequence)
{
    // Nothing is identified while the splitter still waits for the tone map that opens the first window: the draws before it are the
    // partial frame the capture started in, and their targets are only half written.
    if (!s.dumpActive || s.dumpWritten || !DumpIdentifies(s.split.fresh))
        return;
    // The G-buffer fill: the five targets of the frame's first five-target draw.
    if (IsGbufferShape(targets.bound) && !DumpHeld(s, "gbuffer0"))
    {
        uint32_t index = 0;
        for (uint32_t slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT && index < 5; ++slot)
        {
            if (!targets.rtv[slot].resource)
                continue;
            DumpIdentify(s, "gbuffer" + std::to_string(index), targets.rtv[slot].resource.Get(), targets.rtv[slot].texture, row.ordinal,
                         sequence, DumpRank::Gbuffer);
            ++index;
        }
    }
    // The light buffers as the first draws that sample them bind them, the view-position buffer the frame's water and composite draws sample (each
    // names it its own way, DrawDiagFormat.h's kViewPositionNames), and the target of the first draw that reads the hierarchical-Z map.
    for (size_t i = 0; i < kMaxSlots; ++i)
    {
        if (!srv[i].bound)
            continue;
        const std::string_view name = SrvName(s, reflection, i);
        if (name == "g_SamplerLightDiffuse" || name == "g_SamplerLightSpecular")
        {
            // The transparency-light pair the water draws sample under the light names is a DIFFERENT resource pair than
            // the composite's main pair (PS-FRESH-PUDDLE-OCEAN.md): the puddles' definitive water gate's top candidate.
            // The test sits ABOVE the plain light names - the same names, so a chain below them is unreachable - and it
            // asks for the main pair to be held already, so a first binder that is the main pair itself still identifies
            // the light files, whatever kind of draw bound it first.
            const bool waterPair = DumpHeld(s, "light_diffuse") && ReflectionHas(s, reflection, "g_SamplerReflectionMap") &&
                                   !HoldsResource(s, "light_diffuse", srv[i].resource.Get()) &&
                                   !HoldsResource(s, "light_specular", srv[i].resource.Get());
            if (waterPair)
                DumpIdentify(s, name == "g_SamplerLightDiffuse" ? "waterlights-diffuse" : "waterlights-specular",
                             srv[i].resource.Get(), srv[i].texture, row.ordinal, sequence, DumpRank::WaterLights);
            else if (name == "g_SamplerLightDiffuse")
                DumpIdentify(s, "light_diffuse", srv[i].resource.Get(), srv[i].texture, row.ordinal, sequence, DumpRank::Light);
            else
                DumpIdentify(s, "light_specular", srv[i].resource.Get(), srv[i].texture, row.ordinal, sequence, DumpRank::Light);
        }
        else if (name == "g_SamplerHierarchicalZ" && !DumpHeld(s, "ssr"))
        {
            // The braces matter: without them the view-position branch below would be this loop's `if`'s else, so the view-position buffer
            // would only ever be identified by a hierarchical-Z draw that binds no target at all, and its file would never be written.
            for (uint32_t slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++slot)
                if (targets.rtv[slot].resource)
                {
                    DumpIdentify(s, "ssr", targets.rtv[slot].resource.Get(), targets.rtv[slot].texture, row.ordinal, sequence,
                                 DumpRank::Ssr);
                    break;
                }
        }
        else if (IsViewPositionName(name))
            DumpIdentify(s, "vposition", srv[i].resource.Get(), srv[i].texture, row.ordinal, sequence, DumpRank::ViewPosition);
        else if (name == "g_SamplerShadow")
            DumpIdentify(s, "atlas", srv[i].resource.Get(), srv[i].texture, row.ordinal, sequence, DumpRank::Atlas);
        else if (name == "g_SamplerReflectionMap")
            // The water draws' own planar map: the puddles' definitive water gate reads its alpha traffic (PS-FRESH-PUDDLE-OCEAN.md's third
            // candidate). The water readers are the draws whose shader declares this name, which is also the gate the light-pair branch above
            // asks for before it names the transparency pair.
            DumpIdentify(s, "planar", srv[i].resource.Get(), srv[i].texture, row.ordinal, sequence, DumpRank::WaterGate);
    }
    // The composite's own occlusion: the puddles' water gate's second candidate (the occlusion's channels may carry the engine's own surface
    // class). Every light draw and every forward draw binds the same name, so the identification is scoped to the composite's own shape - the
    // ambient probe table beside the light buffers is what names the composite and nothing else.
    if (!DumpHeld(s, "occlusion") && ReflectionHas(s, reflection, "g_AmbientParamArray") &&
        ReflectionHas(s, reflection, "g_SamplerLightDiffuse"))
        for (size_t i = 0; i < kMaxSlots; ++i)
            if (srv[i].bound && SrvName(s, reflection, i) == "g_SamplerOcclusion")
            {
                DumpIdentify(s, "occlusion", srv[i].resource.Get(), srv[i].texture, row.ordinal, sequence, DumpRank::WaterGate);
                break;
            }
    // The wetness pass: it declares the wetness buffer and the two textures the ground pass reads, and it writes one target.
    if (!DumpHeld(s, "wetness") && IsWetnessPassShape(ReflectionHas(s, reflection, "g_WetnessParameter"),
                                                      ReflectionHas(s, reflection, "g_SamplerGBuffer3"),
                                                      ReflectionHas(s, reflection, "g_SamplerViewPosition"), targets.bound))
        for (uint32_t slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++slot)
            if (targets.rtv[slot].resource)
            {
                DumpIdentify(s, "wetness", targets.rtv[slot].resource.Get(), targets.rtv[slot].texture, row.ordinal, sequence,
                             DumpRank::Wetness);
                break;
            }
    // The tone map: t0 is the scene it reads and its first target is what it writes. The copy of the scene that follows it is identified
    // by the draws after this one.
    if (row.stage == kStageToneMap && s.dumpToneFrame == 0)
    {
        s.dumpToneFrame = static_cast<uint32_t>(row.frame);
        s.dumpToneSequence = sequence;
        if (srv[0].bound)
        {
            DumpIdentify(s, "scene", srv[0].resource.Get(), srv[0].texture, row.ordinal, sequence, DumpRank::Scene);
            s.dumpSceneWidth = srv[0].texture.width;
            s.dumpSceneHeight = srv[0].texture.height;
        }
        for (uint32_t slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++slot)
            if (targets.rtv[slot].resource)
            {
                DumpIdentify(s, "tonemap", targets.rtv[slot].resource.Get(), targets.rtv[slot].texture, row.ordinal, sequence,
                             DumpRank::Rest);
                if (s.dumpSceneWidth == 0)
                {
                    s.dumpSceneWidth = targets.rtv[slot].texture.width;
                    s.dumpSceneHeight = targets.rtv[slot].texture.height;
                }
                break;
            }
    }
    // The unidentified FP16 copy: the first draw after the tone map that reads one FP16 texture and writes a different FP16 target of the
    // same size as the scene the tone map read.
    if (s.dumpToneFrame != 0 && DumpAfterTone(s.dumpToneSequence, sequence) && !DumpHeld(s, "copyps") && s.dumpSceneWidth != 0)
    {
        bool reads = false;
        for (size_t i = 0; i < kMaxSlots && !reads; ++i)
            reads = srv[i].bound && IsFp16Format(srv[i].format) &&
                    SameTextureSize(srv[i].texture.width, srv[i].texture.height, s.dumpSceneWidth, s.dumpSceneHeight);
        if (!reads)
            return;
        for (uint32_t slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++slot)
        {
            const TargetInfo& target = targets.rtv[slot];
            if (!target.resource || !IsFp16Format(target.format) ||
                !SameTextureSize(target.texture.width, target.texture.height, s.dumpSceneWidth, s.dumpSceneHeight))
                continue;
            bool same = false;
            for (size_t i = 0; i < kMaxSlots && !same; ++i)
                same = srv[i].bound && srv[i].resource.Get() == target.resource.Get();
            if (same)
                continue;
            DumpIdentify(s, "copyps", target.resource.Get(), target.texture, row.ordinal, sequence, DumpRank::Rest);
            break;
        }
    }
}

// Whether the draw is the light pass that writes the frame's light buffers (DrawDiagFormat.h's IsLightPassShape), from the targets the diagnostic
// has already read and the blend state it has already queried: the draw the light shafts module copies g_LightParam from.
bool IsLightPass(const DrawTargets& targets, bool blends)
{
    uint32_t format = 0;
    uint32_t width = 0, height = 0;
    bool same = true;
    for (uint32_t slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++slot)
    {
        const TargetInfo& target = targets.rtv[slot];
        if (!target.resource)
            continue;
        if (format == 0)
        {
            format = target.format;
            width = target.texture.width;
            height = target.texture.height;
        }
        else if (target.format != format || target.texture.width != width || target.texture.height != height)
        {
            same = false;
        }
    }
    return IsLightPassShape(targets.bound, format, targets.bound != 0 && same, blends);
}

// ---- the named constant buffers of the lighting passes ----------------------------------------------------------------

// Reads the light parameters and the camera's two matrices of one draw: position, direction and diffuse colour at the offsets
// DrawDiagFormat.h documents, the view matrix rows the space of the direction is read against, and the view-projection that direction is
// projected through. One staging copy and one map per buffer, at most once a second, on the render thread. `frame` = the frame the read
// belongs to (0 while the diagnostic waits).
void ReadConstants(ID3D11DeviceContext* c, uint64_t frame, ID3D11Buffer* light, uint32_t lightSlot, ID3D11Buffer* camera, uint32_t crc,
                   bool hasCrc)
{
    State& s = S();
    if (light == nullptr)
        return;
    const unsigned char* bytes = MapConstant(c, s.lightStaging, light, kLightParamBytes);
    if (bytes == nullptr)
        return;
    ConstantRead read;
    read.frame = frame;
    read.seconds = SecondsBetween(s.clockZero, Qpc());
    read.psCrc = crc;
    read.hasCrc = hasCrc;
    read.name = kLightParamName;
    read.slot = lightSlot;
    D3D11_BUFFER_DESC desc {};
    light->GetDesc(&desc);
    read.bytes = desc.ByteWidth;
    std::memcpy(read.position, bytes + kLightParamPositionAt, sizeof read.position);
    std::memcpy(read.direction, bytes + kLightParamDirectionAt, sizeof read.direction);
    std::memcpy(read.diffuse, bytes + kLightParamDiffuseAt, sizeof read.diffuse);
    UnmapConstant(c, s.lightStaging);

    // The camera buffer is read from the same draw, so the direction and the matrices always belong to one frame's state. The view is the
    // buffer's first matrix, the view-projection its second and the projection its fourth at +0x120 (DrawDiagFormat.h), so one map of 0x160 bytes
    // covers them and a row's direction, view, view-projection and projection are always the same light pass's.
    if (camera != nullptr)
        if (const unsigned char* cameraBytes = MapConstant(c, s.cameraStaging, camera, kCameraParamProjectionAt + kCameraParamMatrixBytes);
            cameraBytes != nullptr)
        {
            std::memcpy(read.view, cameraBytes + kCameraParamViewAt, sizeof read.view);
            read.hasView = true;
            std::memcpy(read.viewProjection, cameraBytes + kCameraParamViewProjectionAt, sizeof read.viewProjection);
            read.hasViewProjection = true;
            std::memcpy(read.projection, cameraBytes + kCameraParamProjectionAt, sizeof read.projection);
            read.hasProjection = true;
            UnmapConstant(c, s.cameraStaging);
        }

    // The frame's sun for engine.txt, from this read: the direction the light draw reports (view space), through the projection the same draw's
    // camera carries, into the window this draw renders. Kept whether or not the light shafts card is on (that module only projects a sun while its
    // own card runs, which is the state the engine's flare is captured in) and whichever phase took the read - the sample belongs to the frame that
    // was drawn, not to a capture's window.
    if (read.hasProjection)
    {
        D3D11_VIEWPORT viewport {};
        UINT count = 1;
        c->RSGetViewports(&count, &viewport);
        State::LightSample& sample = s.lastLight;
        sample.have = true;
        std::memcpy(sample.direction, read.direction, sizeof sample.direction);
        std::memcpy(sample.projection, read.projection, sizeof sample.projection);
        sample.window[0] = viewport.TopLeftX;
        sample.window[1] = viewport.TopLeftY;
        sample.window[2] = viewport.Width;
        sample.window[3] = viewport.Height;
    }

    if (s.truncatedCbs)
        return;
    const std::string row = CbRowText(read);
    if (s.cbs.size() + row.size() > kCbBytes)
    {
        s.cbs += kTruncatedLine;
        s.truncatedCbs = true;
        return;
    }
    s.cbs += row;
    ++s.cbRows;
}

// One staging buffer per distinct constant-buffer size: the dump reads a dozen names a second, and allocating a copy for each would be the
// expensive part of it.
Staging& CbDumpStaging(State& s, UINT bytes)
{
    for (Staging& staging : s.cbDumpStaging)
        if (staging.bytes == bytes)
            return staging;
    s.cbDumpStaging.emplace_back();
    s.cbDumpStaging.back().bytes = bytes;
    return s.cbDumpStaging.back();
}

// Whether one named buffer of the dump list is due (at most once a second per name).
bool CbDumpDue(State& s, size_t index)
{
    const int64_t now = Qpc();
    if (s.cbDumpLast[index] != 0 && SecondsBetween(s.cbDumpLast[index], now) < kSampleSeconds)
        return false;
    s.cbDumpLast[index] = now;
    return true;
}

// One row of cbdump.csv: a staging copy of the buffer, its first min(bytes, kCbDumpHexMax) bytes in hex, and its pointer. Only while the
// capture collects; a buffer that cannot be copied or mapped skips its row and nothing else happens.
void ReadCbDump(ID3D11DeviceContext* c, State& s, uint64_t frame, ID3D11Buffer* buffer, uint32_t slot, const char* name, uint32_t crc,
                bool hasCrc)
{
    if (s.truncatedCbDump || buffer == nullptr)
        return;
    D3D11_BUFFER_DESC desc {};
    buffer->GetDesc(&desc);
    Staging& staging = CbDumpStaging(s, desc.ByteWidth);
    const unsigned char* bytes = MapConstant(c, staging, buffer, desc.ByteWidth);
    if (bytes == nullptr)
        return;
    CbDumpRow row;
    row.frame = frame;
    row.seconds = SecondsBetween(s.clockZero, Qpc());
    row.psCrc = crc;
    row.hasCrc = hasCrc;
    row.name = name;
    row.slot = slot;
    row.bytes = desc.ByteWidth;
    row.pointer = buffer;
    const size_t count = desc.ByteWidth < kCbDumpHexMax ? desc.ByteWidth : kCbDumpHexMax;
    row.data.assign(bytes, bytes + count);
    UnmapConstant(c, staging);
    const std::string text = CbDumpRowText(row);
    if (!CbDumpFits(s.cbdump.size(), text.size()))
    {
        s.cbdump += kTruncatedLine;
        s.truncatedCbDump = true;
        return;
    }
    s.cbdump += text;
    ++s.cbDumpRows;
}

// ---- files -----------------------------------------------------------------------------------------------------------

// Appends one frame's text to draws.csv, or refuses it at the byte cap. The return value is what the trailer counts: a frame that was not
// appended contributes nothing, and the TRUNCATED line says why.
bool AppendDraws(State& s, const std::string& text)
{
    if (s.truncated)
        return false;
    if (s.rows.size() + text.size() > kDrawsBytes)
    {
        s.rows += kTruncatedLine;
        s.truncated = true;
        return false;
    }
    s.rows += text;
    return true;
}

// The creation fixtures, copied under their mutex: shader creation can run while a capture is being written, and the files are written from
// the copy so no file of it is written while the mutex is held.
std::vector<State::CreationFixture> CreationFixtures()
{
    std::lock_guard lock(S().creationMutex);
    return S().creationFixtures;
}

// Writes the capture in progress: the run folder, the files and the fixtures.
void WriteRun()
{
    State& s = S();
    const fs::path dir = RunFolder();
    if (dir.empty())
    {
        LOG_WARN("Draw diagnostics: the capture could not be written");
        return;
    }
    const std::string draws = "# FFXIV draw diagnostics, format " + std::to_string(kFormatVersion) + "\n" + DrawsHeaderText() + s.rows +
                              DrawsTrailerText(s.rowsWritten, s.framesWritten, s.omittedTotal);
    const std::string cbs = "# FFXIV draw diagnostics, format " + std::to_string(kFormatVersion) + "\n" + CbHeaderText() + s.cbs +
                            RowsTrailerText(s.cbRows);
    const std::string cbdump = "# FFXIV draw diagnostics, format " + std::to_string(kFormatVersion) + "\n" + CbDumpHeaderText() + s.cbdump +
                               RowsTrailerText(s.cbDumpRows);
    const std::string frames = "# bounded feature telemetry; counts are deltas since the previous row; sample_time_token is FrameCount at this callback\n"
                               "sample_time_token,ssr_token,ssr_tried,ssr_valid,ssr_served,ssr_runs,ssr_declines,"
                               "puddle_gloss_token,puddle_gloss_tried,puddle_gloss_valid,puddle_gloss_served,puddle_gloss_runs,puddle_gloss_declines,"
                               "wetness_token,wetness_known,wetness,wet_latched,shape_token,shape_present,"
                               "tone_capture_token,tone_capture_taken,tone_shape_wet,tone_runs,tone_declines\n" + s.framesCsv +
                               (s.frameTelemetryTruncated ? "# telemetry truncated at the 16-sample cap\n" : std::string());
    const std::string views = ViewsFileHeader() + ViewsHeaderText() + s.views;
    if (!WriteFile(dir / kDrawsFile, draws) || !WriteFile(dir / kCbFile, cbs) || !WriteFile(dir / kCbDumpFile, cbdump) ||
        !WriteFile(dir / kViewsFile, views) || !WriteFile(dir / kFramesFile, frames))
    {
        LOG_WARN("Draw diagnostics: a write to the run folder failed");
        return;
    }
    // The engine pass overrides' counters: the same session facts the module's card shows, so a session is read from the capture instead of
    // from a log. `module: off` is the whole file when the key is off.
    if (!WriteFile(dir / kEngineFile, EngineStatusText(EngineStatusNow()) + SsrStatusText(SsrStatusNow()) + PuddleStatusText(PuddleStatusNow())))
        LOG_WARN("Draw diagnostics: engine.txt could not be written");
    LOG_INFO("Draw diagnostics: {} frame(s), {} draw row(s) ({} omitted), {} cb.csv read(s), {} cbdump.csv sample(s) written to {}",
             s.framesWritten, s.rowsWritten, s.omittedTotal, s.cbRows, s.cbDumpRows, dir.string());
    if (s.truncated || s.truncatedCbs || s.truncatedCbDump)
        LOG_WARN("Draw diagnostics: the byte cap of a file was reached, later rows were dropped");
    const std::vector<State::CreationFixture> creation = CreationFixtures();
    if (!s.fixtures.empty() || !creation.empty())
    {
        // The fixture shaders: one file per shader plus a list of what each one declares, so a reflection rule can be replayed offline on
        // the blob the game really handed to CreatePixelShader. The shaders kept from creation come first and are marked `creation`: they
        // were created while the game loaded, before this capture was armed. A blob kept in both lists is written once.
        const fs::path folder = dir / kShaderFolder;
        std::error_code ec;
        fs::create_directories(folder, ec);
        std::string list = FixtureListHeader();
        size_t listed = 0;
        for (const State::CreationFixture& fixture : creation)
        {
            list += FixtureListLine(fixture.crc, fixture.code.size(), true, fixture.names);
            if (!ec)
                WriteFile(folder / (detail::Hex32(fixture.crc) + ".dxbc"), fixture.code);
            ++listed;
        }
        for (const State::Fixture& fixture : s.fixtures)
        {
            bool alreadyKept = false;
            for (const State::CreationFixture& kept : creation)
                if (kept.crc == fixture.crc)
                    alreadyKept = true;
            if (alreadyKept)
                continue;
            list += FixtureListLine(fixture.crc, fixture.code.size(), false, fixture.names);
            if (!ec)
                WriteFile(folder / (detail::Hex32(fixture.crc) + ".dxbc"), fixture.code);
            ++listed;
        }
        if (!ec && !WriteFile(dir / kShadersFile, list))
            LOG_WARN("Draw diagnostics: the shader fixtures could not be listed");
        else
            LOG_INFO("Draw diagnostics: {} shader fixture(s) written ({} kept from creation)", listed, creation.size());
    }
    if (s.shadersWithoutReflection != 0)
        LOG_INFO("Draw diagnostics: {} draw(s) named a pixel shader whose reflection was not recorded (the session's table was full, or the shader "
                 "was created while the key was off)",
                 s.shadersWithoutReflection);
}

// The state one capture owns, back to its starting values for the next capture of the same session. The fixture list and the reflection
// table are session-wide, and the one-frame dump runs once per session, so neither is reset here.
void ResetForNextCapture(State& s)
{
    s.frameRows.clear();
    s.rows.clear();
    s.cbs.clear();
    s.cbdump.clear();
    s.framesCsv.clear();
    s.frameTelemetryRows = 0;
    s.frameTelemetryTruncated = false;
    s.previousSsrRuns = s.previousSsrDeclines = 0;
    s.previousGlossRuns = s.previousGlossDeclines = 0;
    s.previousToneRuns = s.previousToneDeclines = 0;
    s.views.clear();
    for (bool& seen : s.viewSeen)
        seen = false;
    for (int64_t& last : s.cbDumpLast)
        last = 0;
    for (CbDumpPointerCadence& cadence : s.cbDumpPointer)
        cadence = CbDumpPointerCadence {};
    s.pointerWindowTicks = 0;
    s.truncated = false;
    s.truncatedCbs = false;
    s.truncatedCbDump = false;
    s.frames = 0;
    s.framesCaptured = 0;
    s.framesWritten = 0;
    s.drawSequence = 0;
    s.drawsInFrame = 0;
    s.omittedInFrame = 0;
    s.rowsWritten = 0;
    s.cbRows = 0;
    s.cbDumpRows = 0;
    s.omittedTotal = 0;
    s.shadersWithoutReflection = 0;
    s.announced = false;
    s.waitTicks = 0;
    s.lastReadQpc = 0;
    s.split = FrameSplitter {};
    s.markers.Reset();
    // The run folder and the dump belong to the capture that ended; the dump runs once per session, so an unwritten one is not retried.
    s.runDir.clear();
    s.dumpItems.clear();
    s.dumpHeldBytes = 0;
    s.dumpActive = false;
    s.dumpToneFrame = 0;
    s.dumpToneAtPresent = false;
    s.dumpToneSequence = 0;
    s.dumpSceneWidth = 0;
    s.dumpSceneHeight = 0;
}

// Ends a capture. The files are written either way; a key-started capture then waits for its next press - the press that started the last
// one has to be released first, so one press never starts two captures - up to kMaxCapturesPerSession captures per session.
void Finish()
{
    State& s = S();
    s.phase.store(kDone, std::memory_order_relaxed);
    ext::SetActive(ext::F::DrawDiag, false); // the draw path is back to one relaxed load
    FinishDump(s);
    WriteRun();
    if (s.dumpActive && !s.dumpWritten)
        LOG_WARN("Draw diagnostics: the one-frame dump was not written (the frame that identified its tone map never ended, or there was "
                 "no context to read the copies back on)");
    // The dump holds references to game textures: they are released here whichever path ended the capture, before the session moves on.
    // A swapchain buffer held like this makes the next ResizeBuffers fail.
    s.dumpItems.clear();
    s.dumpHeldBytes = 0;
    if (s.arm.trigger != Trigger::Key || s.captureArm.Exhausted())
        return;
    ResetForNextCapture(s);
    s.phase.store(kWaiting, std::memory_order_relaxed);
    ext::SetActive(ext::F::DrawDiag, true); // set last: a draw never sees the bit without a capture behind it
    LOG_INFO("Draw diagnostics: capture {} of at most {} written; release the capture key and press it again for the next one",
             s.captureArm.captures, kMaxCapturesPerSession);
}

// ---- one draw --------------------------------------------------------------------------------------------------------

struct CbSlot
{
    ComPtr<ID3D11Buffer> buffer;
    uint32_t bytes = 0;
};

// The light draw's own 128-byte pixel-shader constant buffers, each with the name its reflection gives it. The shafts identity reads the row at
// +0x20 of the frame's `g_DirectionalShadowParameter`, and the sun draw binds a same-sized neighbour beside it (a captured sun draw puts one at
// slot 6), so the bytes of that draw itself are what says which slot holds the shadow and what the neighbour's numbers are. The fixed dump list's
// names land first at the rain volumes' earlier light draw of the frame, which is what a capture of the list alone shows; this dumps the draw that
// writes the frame's light buffers, once a second beside the light's own sample. A buffer that cannot be copied skips its row.
void DumpSunDrawConstants(ID3D11DeviceContext* c, State& s, const CbSlot (&cb)[8], const std::string (&names)[8], uint32_t crc, bool hasCrc)
{
    for (size_t i = 0; i < 8; ++i)
    {
        if (!cb[i].buffer || cb[i].bytes != 0x80)
            continue;
        try
        {
            ReadCbDump(c, s, s.frames + 1, cb[i].buffer.Get(), static_cast<uint32_t>(i),
                       names[i].empty() ? "128-byte constant buffer" : names[i].c_str(), crc, hasCrc);
        }
        catch (...)
        {
        }
    }
}

// Closes the frame in progress: its rows, its marker line and the counters. `keepTone` is set by the tone-map split: the tone map
// that closes a window also opens the next one, so the splitter keeps its armed state and only the bridge Present starts it over.
// Called from the tone map draw (the frame split, which needs no Present) and from the bridge Present entry when a session has one.
// `c` is the context the close was seen on; a Present has none, so a dump that is due is written by the next draw instead.
void CloseFrame(ID3D11DeviceContext* c, bool keepTone)
{
    State& s = S();
    const uint64_t frame = s.frames + 1;
    if (s.drawsInFrame >= kMinDrawsPerFrame) // shorter frames are loading screens, the title and the like
    {
        std::string text = std::move(s.frameRows);
        text += s.markers.Line(frame, s.drawsInFrame, s.omittedInFrame);
        const FrameTally tally = TallyFrame(s.drawsInFrame, s.omittedInFrame, AppendDraws(s, text));
        s.rowsWritten += tally.rows;
        s.omittedTotal += tally.omitted;
        if (tally.frame)
            ++s.framesWritten;
        ++s.framesCaptured;
    }
    s.frameRows.clear();
    s.frames = frame;
    // The frame that identified the dump's tone map is whole once a Present closed it: the dump can be written from here on, even when the
    // capture ends at that same Present (two frames, a session with a bridge).
    if (!keepTone && s.dumpToneFrame != 0 && frame == s.dumpToneFrame)
        s.dumpToneAtPresent = true;
    s.drawsInFrame = 0;
    s.omittedInFrame = 0;
    s.markers.Reset();
    if (!keepTone)
        s.split.Reset();
    MaybeWriteDump(c, s);
}

// The tone map that opens the first window also ends the partial frame the capture started in (the draws before it belong to a frame
// whose beginning was never seen). Its rows and markers are dropped so the first window is one whole frame, which is also what the
// one-frame texture dump reads.
void DropPartialFrame(State& s)
{
    s.frameRows.clear();
    s.drawsInFrame = 0;
    s.omittedInFrame = 0;
    s.markers.Reset();
}

// The pixel shader's bytecode CRC, as the creation hook stamped it on the object (0 when it has none).
uint32_t ShaderCrc(ID3D11DeviceContext* c)
{
    ComPtr<ID3D11PixelShader> shader;
    c->PSGetShader(&shader, nullptr, nullptr);
    return ReadShaderCrc(shader.Get());
}

// Whether the constant-buffer read of this draw is due (at most once a second).
bool SampleDue(State& s)
{
    const int64_t now = Qpc();
    if (s.lastReadQpc != 0 && SecondsBetween(s.lastReadQpc, now) < kSampleSeconds)
        return false;
    s.lastReadQpc = now;
    return true;
}

// Whether the second has passed, without spending it: a draw that is not the light pass leaves the slot open for the one that is.
bool SampleWanted(State& s)
{
    const int64_t now = Qpc();
    return s.lastReadQpc == 0 || SecondsBetween(s.lastReadQpc, now) >= kSampleSeconds;
}

void NoteSampled(State& s) { s.lastReadQpc = Qpc(); }

// While the diagnostic waits for its capture (a delay or a key), it still samples the light and camera constants once a second - the
// measurement of the sun direction's space needs a series over a moving camera - and checks every kWaitDrawsPerPoll draws whether it
// is time to start. Once a second, and only then, it also reads the draw's targets and blend state to tell the light pass that carries the
// light's own parameters from the passes that bind the same name; nothing else happens per draw on this path.
void WaitTick(ID3D11DeviceContext* c);

// Starts the collection, once, from the render thread (the config load may run inside DllMain, where nothing may log). `clockZero`
// stays at the config load even for a delayed capture, so a cb.csv row says when in the session it was read. The engine counters' baseline is
// taken here, after the phase's `kWaiting` draws and before this one's own patch, so the window holds exactly the draws the capture collects.
// The constant-buffer sample's clock is cleared here as well: a waiting diagnostic reads the light pass once a second, and a key capture can arm
// inside the second that read spent - without the reset the capture's first light pass is skipped and engine.txt's sun line carries the waiting
// frame's camera pose, which is a screen position this capture's frame does not have.
void StartCollecting()
{
    State& s = S();
    TakeEngineBaseline(s);
    TakeSsrBaseline(s);
    TakePuddleBaseline(s);
    s.lastReadQpc = 0;
    s.phase.store(kCapturing, std::memory_order_relaxed);
    ext::ffxiv::engine::ssr::OnDiagnosticCaptureStart();
    const auto ssr = ext::ffxiv::engine::ssr::CurrentFrameObservation();
    const auto gloss = ext::ffxiv::engine::gloss::CurrentFrameObservation();
    const auto tone = ext::ffxiv::engine::gloss::CurrentToneFrameObservation();
    s.previousSsrRuns = ssr.runs;
    s.previousSsrDeclines = ssr.declined;
    s.previousGlossRuns = gloss.runs;
    s.previousGlossDeclines = gloss.declined;
    s.previousToneRuns = tone.runs;
    s.previousToneDeclines = tone.declined;
}

void AppendFrameTelemetry(State& s)
{
    if (s.frameTelemetryRows >= static_cast<uint32_t>(kMaxFrames))
    {
        s.frameTelemetryTruncated = true;
        return;
    }
    const auto ssr = ext::ffxiv::engine::ssr::CurrentFrameObservation();
    const auto gloss = ext::ffxiv::engine::gloss::CurrentFrameObservation();
    const auto tone = ext::ffxiv::engine::gloss::CurrentToneFrameObservation();
    const uint64_t presentToken = ext::present::FrameCount();
    const auto delta = [](uint32_t value, uint32_t previous) { return value >= previous ? value - previous : value; };
    s.framesCsv += std::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{:.6f},{},{},{},{},{},{},{},{}\n", presentToken,
                               ssr.hasToken ? ssr.token : 0, ssr.tried ? 1 : 0, ssr.valid ? 1 : 0, ssr.served,
                               delta(ssr.runs, s.previousSsrRuns), delta(ssr.declined, s.previousSsrDeclines),
                               gloss.hasToken ? gloss.token : 0, gloss.tried ? 1 : 0, gloss.valid ? 1 : 0, gloss.served,
                               delta(gloss.runs, s.previousGlossRuns), delta(gloss.declined, s.previousGlossDeclines),
                               gloss.wetnessToken, gloss.wetnessKnown ? 1 : 0, double(gloss.wetness), gloss.wetLatched ? 1 : 0,
                               gloss.shapeToken, gloss.shapePresent ? 1 : 0, tone.captureToken, tone.captureTaken ? 1 : 0,
                               tone.shapeWet ? 1 : 0, delta(tone.runs, s.previousToneRuns), delta(tone.declined, s.previousToneDeclines));
    s.previousSsrRuns = ssr.runs;
    s.previousSsrDeclines = ssr.declined;
    s.previousGlossRuns = gloss.runs;
    s.previousGlossDeclines = gloss.declined;
    s.previousToneRuns = tone.runs;
    s.previousToneDeclines = tone.declined;
    ++s.frameTelemetryRows;
}

// The log line of the first collected draw: when the capture started and how many frames it writes.
void Announce(State& s)
{
    s.announced = true;
    const char* how = s.arm.trigger == Trigger::Key     ? "now (the capture key was pressed)"
                      : s.arm.trigger == Trigger::Delay ? "after the delay"
                                                        : "at the first draw";
    LOG_INFO("Draw diagnostics: capturing {} frame(s) {}; the files are written when the last frame ends", s.wantedFrames, how);
}

void Draw(ID3D11DeviceContext* c, const char* api)
{
    State& s = S();
    ext::ffxiv::engine::ssr::OnRenderCallback(c);
    if (!s.announced)
        Announce(s);

    DrawRow row;
    row.frame = s.frames + 1;
    row.ordinal = ++s.drawsInFrame;
    // The capture's own draw counter, which a frame close does not restart: the dump's "a later draw" rule is written on it, so a target
    // identified at a window's last draw is still copied in the next window.
    const uint64_t sequence = ++s.drawSequence;
    // The one-frame dump acts before the draw: the readback copies of what an earlier draw identified are queued here, so they hold that
    // draw's output, and the dump is written once its frame is over.
    DumpCopyPending(c, s, sequence);
    MaybeWriteDump(c, s);
    row.api = api != nullptr ? api : "";
    row.gameContext = c == ext::ffxiv::lighting::FfxivLightingCapture::context;
    const ext::ffxiv::ui::plate::DrawArgs args = ext::ffxiv::ui::plate::g_args;
    row.count = args.count;
    row.instances = args.instances;
    row.start = args.start;
    row.baseVertex = args.baseVertex;

    // The pixel shader, its bytecode CRC (stamped by the creation hook) and the catalog stage that CRC is known to be.
    const uint32_t crc = ShaderCrc(c);
    row.psCrc = crc;
    row.hasCrc = crc != 0;
    row.stage = ext::ffxiv::ui::StageOfCrc(crc);

    // The bound resources, by slot; the names are added below, from the reflection table.
    ID3D11ShaderResourceView* views[kMaxSlots] {};
    SrvSlot srv[kMaxSlots] {};
    c->PSGetShaderResources(0, kMaxSlots, views);
    for (size_t i = 0; i < kMaxSlots; ++i)
    {
        if (views[i] == nullptr)
            continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc {};
        views[i]->GetDesc(&desc);
        srv[i].bound = true;
        srv[i].format = uint32_t(desc.Format);
        srv[i].formatText = FormatText(srv[i].format);
        srv[i].viewDimension = uint32_t(desc.ViewDimension);
        srv[i].arraySize = 1;
        switch (desc.ViewDimension)
        {
        case D3D11_SRV_DIMENSION_TEXTURE1D: srv[i].viewMips = desc.Texture1D.MipLevels; break;
        case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
            srv[i].viewMips = desc.Texture1DArray.MipLevels;
            srv[i].arraySize = desc.Texture1DArray.ArraySize;
            break;
        case D3D11_SRV_DIMENSION_TEXTURE2D: srv[i].viewMips = desc.Texture2D.MipLevels; break;
        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
            srv[i].viewMips = desc.Texture2DArray.MipLevels;
            srv[i].arraySize = desc.Texture2DArray.ArraySize;
            break;
        case D3D11_SRV_DIMENSION_TEXTURE2DMS: break;
        case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY: srv[i].arraySize = desc.Texture2DMSArray.ArraySize; break;
        case D3D11_SRV_DIMENSION_TEXTURE3D: srv[i].viewMips = desc.Texture3D.MipLevels; break;
        case D3D11_SRV_DIMENSION_TEXTURECUBE:
            srv[i].viewMips = desc.TextureCube.MipLevels;
            srv[i].arraySize = 6;
            break;
        case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
            srv[i].viewMips = desc.TextureCubeArray.MipLevels;
            srv[i].cubes = desc.TextureCubeArray.NumCubes;
            srv[i].arraySize = desc.TextureCubeArray.NumCubes * 6;
            break;
        default: break;
        }
        views[i]->GetResource(&srv[i].resource);
        srv[i].texture = ReadTextureInfo(srv[i].resource.Get());
        views[i]->Release();
        views[i] = nullptr;
    }

    ID3D11Buffer* buffers[8] {};
    CbSlot cb[8] {};
    c->PSGetConstantBuffers(0, 8, buffers);
    for (size_t i = 0; i < 8; ++i)
    {
        if (buffers[i] == nullptr)
            continue;
        D3D11_BUFFER_DESC desc {};
        buffers[i]->GetDesc(&desc);
        cb[i].buffer.Attach(buffers[i]); // takes the reference the Get* call handed out
        cb[i].bytes = desc.ByteWidth;
    }

    const DrawTargets targets = ReadTargets(c);
    row.rtv = RenderTargetText(targets, row.dsv);
    row.viewport = ViewportText(c);
    bool blends = false;
    row.blend = BlendText(c, blends);
    row.depth = DepthText(c);

    // The G-buffer targets are copied at the first later draw that binds one of them as a sampled texture, which is the point the geometry
    // pass that writes them is over. The slots were read above; no D3D call is made under the reflection mutex below.
    DumpCopySampled(c, s, srv, sequence);

    // The names, from the reflection table. Every D3D query above is already done, so the mutex is never held across a D3D call.
    // The names of the two constant buffers are copied out here: the interned names live in a vector that shader creation still grows on
    // other threads, so a pointer into it must not outlive this block.
    std::string lightName;
    uint32_t lightSlot = 0;
    uint32_t cameraSlot = 0;
    bool hasCamera = false;
    int32_t dumpName[8] {}; // the cbdump.csv list index of each bound slot, -1 for a name not in it
    std::string cbName[8];  // the reflected name of each bound slot, for the sun draw's 128-byte dump below
    for (int32_t& index : dumpName)
        index = -1;
    {
        std::lock_guard lock(s.mutex);
        const Reflection* reflection = nullptr;
        if (row.hasCrc)
        {
            reflection = ReflectionFor(s, crc);
            if (reflection == nullptr)
                ++s.shadersWithoutReflection;
        }
        for (size_t i = 0; i < kMaxSlots; ++i)
        {
            if (!srv[i].bound)
                continue;
            const char* name = NameAt(s, reflection, BindKind::Texture, static_cast<uint32_t>(i));
            if (!row.srvs.empty())
                row.srvs += '|';
            row.srvs += SrvEntryText(static_cast<uint32_t>(i), name != nullptr ? name : "-", srv[i].formatText,
                                     srv[i].texture.valid ? SizeText(srv[i].texture.width, srv[i].texture.height) : "?",
                                     srv[i].resource.Get());
            if (name == nullptr)
                continue;
            const int32_t view = ViewNameIndex(name);
            if (view < 0 || s.viewSeen[view])
                continue;
            s.viewSeen[view] = true;
            ViewSample sample;
            sample.ordinal = row.ordinal;
            sample.name = std::string(ResourceBaseName(name));
            sample.viewDimension = srv[i].viewDimension;
            sample.viewMips = srv[i].viewMips;
            sample.arraySize = srv[i].arraySize;
            sample.cubes = srv[i].cubes;
            sample.viewFormat = srv[i].formatText;
            sample.width = srv[i].texture.width;
            sample.height = srv[i].texture.height;
            sample.textureArray = srv[i].texture.arraySize;
            sample.textureMips = srv[i].texture.mips;
            sample.textureFormat = FormatText(srv[i].texture.format);
            sample.pointer = srv[i].resource.Get();
            s.views += ViewSampleText(sample);
        }
        for (size_t i = 0; i < 8; ++i)
        {
            if (!cb[i].buffer)
                continue;
            const char* name = NameAt(s, reflection, BindKind::ConstantBuffer, static_cast<uint32_t>(i));
            if (!row.cbs.empty())
                row.cbs += '|';
            row.cbs += CbEntryText(static_cast<uint32_t>(i), name != nullptr ? name : "-", cb[i].bytes, cb[i].buffer.Get());
            if (name == nullptr)
                continue;
            dumpName[i] = CbDumpNameIndex(name);
            cbName[i] = name;
            if (std::strcmp(name, kLightParamName) == 0)
            {
                lightName = name;
                lightSlot = static_cast<uint32_t>(i);
            }
            else if (std::strcmp(name, kCameraParamName) == 0)
            {
                cameraSlot = static_cast<uint32_t>(i);
                hasCamera = true;
            }
        }
        // The one-frame dump's identification, from the same reflection table (the wetness pass is a shape of declared names, the light
        // buffers and the SSR target are bound names, and the G-buffer and tone map are shapes of the draw's own targets).
        DumpNoteDraw(s, row, targets, srv, reflection, sequence);
    }

    // The named constant buffers of the lighting and camera passes: at most once a second, one staging copy and one map each, and only at the
    // light pass that writes the frame's light buffers. Every other draw of the frame that declares g_LightParam is another pass (the wetness
    // volumes, a local light), and its buffer is another light's: a sample taken there answered a different direction and a different colour.
    if (!lightName.empty() && IsLightPass(targets, blends) && SampleDue(s))
    {
        ReadConstants(c, s.frames + 1, cb[lightSlot].buffer.Get(), lightSlot, hasCamera ? cb[cameraSlot].buffer.Get() : nullptr, crc,
                      row.hasCrc);
        // The draw's own 128-byte parameter buffers: the frame's `g_DirectionalShadowParameter` among them, so a capture of the sun draw itself
        // carries the row the identity reads and the bytes of the neighbour beside it.
        DumpSunDrawConstants(c, s, cb, cbName, crc, row.hasCrc);
    }

    // The named buffers of the byte dump, each at most once a second. Only the collecting path reads them: the waiting path never pays
    // for a copy and a map of a dozen buffers. The shadow parameter is the one name whose instances the window must not fold into
    // one row: its assembly draws bind one per cascade, so each pointer the shader read this window answers its own row
    // (CbDumpPointersPerSecond), by its ring slot and not by the name.
    for (size_t i = 0; i < 8; ++i)
    {
        if (dumpName[i] < 0)
            continue;
        const std::string_view name = kCbDumpNames[dumpName[i]];
        if (CbDumpPointersPerSecond(ResourceBaseName(name)) > 1)
        {
            if (s.pointerWindowTicks == 0)
                s.pointerWindowTicks = int64_t(double(QpcFrequency()) * kSampleSeconds);
            const int64_t now = Qpc();
            const void* pointer = cb[i].buffer.Get();
            const int32_t index = CbDumpPointerIndex(s.cbDumpPointer[dumpName[i]], pointer);
            if (index >= 0 && !CbDumpPointerDue(s.cbDumpPointer[dumpName[i]], pointer, now, s.pointerWindowTicks))
                continue;
            CbDumpPointerRecord(s.cbDumpPointer[dumpName[i]], pointer, now);
            try
            {
                ReadCbDump(c, s, s.frames + 1, cb[i].buffer.Get(), static_cast<uint32_t>(i), kCbDumpNames[dumpName[i]], crc,
                           row.hasCrc);
            }
            catch (...)
            {
            }
        }
        else if (!CbDumpDue(s, size_t(dumpName[i])))
            continue;
        else
        {
            try
            {
                ReadCbDump(c, s, s.frames + 1, cb[i].buffer.Get(), static_cast<uint32_t>(i), kCbDumpNames[dumpName[i]], crc,
                           row.hasCrc);
            }
            catch (...)
            {
            }
        }
    }

    // Markers are noted for every draw, whether or not the row below is kept, so a capped frame still reports its ordinals.
    row.markers = s.markers.NoteDraw(row.stage, row.ordinal);
    if (s.drawsInFrame > kMaxRowsPerFrame)
        ++s.omittedInFrame;
    else
        s.frameRows += DrawsRowText(row);
    // The frame end, in the draw stream: the tone map draw closes the window in progress, so a capture completes in a session that
    // has no bridge Present at all (the reason a benchmark run with both keys set once wrote nothing). The first tone map drops the
    // partial frame the capture started in; every later one closes a whole frame and arms the next.
    const uint8_t split = s.split.NoteDraw(row.stage, s.drawsInFrame);
    if ((split & kSplitArm) != 0)
        DropPartialFrame(s);
    if ((split & kSplitClose) != 0)
    {
        CloseFrame(c, true);
        if (s.framesCaptured >= s.wantedFrames)
            Finish();
    }
}

// ---- the frame -------------------------------------------------------------------------------------------------------

// Waiting for the capture to start (a delay or a key): the constants are sampled and the start is checked, and the draw list is not
// collected. `waitTicks` bounds both costs to one draw in kWaitDrawsPerPoll.
void WaitTick(ID3D11DeviceContext* c)
{
    State& s = S();
    if (++s.waitTicks < kWaitDrawsPerPoll)
        return;
    s.waitTicks = 0;
    const bool start = s.arm.trigger == Trigger::Key ? s.captureArm.Poll(KeyDown(s.arm.key))
                                                      : SecondsBetween(s.clockZero, Qpc()) >= s.arm.delaySeconds;
    if (start)
    {
        StartCollecting();
        return;
    }
    // The same rule the collecting path uses, and for the same reason: only the light pass that writes the frame's light buffers is read. The
    // shape is checked before the second's sample is spent, so a frame's worth of the wetness volumes and local lights costs one target read.
    if (!SampleWanted(s))
        return;
    {
        bool blends = false;
        BlendText(c, blends);
        if (!IsLightPass(ReadTargets(c), blends))
            return;
    }
    NoteSampled(s);
    try
    {
        const uint32_t crc = ShaderCrc(c);
        ID3D11Buffer* buffers[8] {};
        CbSlot cb[8] {};
        c->PSGetConstantBuffers(0, 8, buffers);
        for (size_t i = 0; i < 8; ++i)
        {
            if (buffers[i] == nullptr)
                continue;
            D3D11_BUFFER_DESC desc {};
            buffers[i]->GetDesc(&desc);
            cb[i].buffer.Attach(buffers[i]);
            cb[i].bytes = desc.ByteWidth;
        }
        // The names, from the reflection table (the mutex is never held across a D3D call).
        uint32_t lightSlot = 0, cameraSlot = 0;
        bool hasLight = false, hasCamera = false;
        {
            std::lock_guard lock(s.mutex);
            const Reflection* reflection = crc != 0 ? ReflectionFor(s, crc) : nullptr;
            for (size_t i = 0; i < 8; ++i)
            {
                if (!cb[i].buffer)
                    continue;
                const char* name = NameAt(s, reflection, BindKind::ConstantBuffer, static_cast<uint32_t>(i));
                if (name == nullptr)
                    continue;
                if (std::strcmp(name, kLightParamName) == 0)
                {
                    lightSlot = static_cast<uint32_t>(i);
                    hasLight = true;
                }
                else if (std::strcmp(name, kCameraParamName) == 0)
                {
                    cameraSlot = static_cast<uint32_t>(i);
                    hasCamera = true;
                }
            }
        }
        // `frame` 0: the read happened while the diagnostic was still waiting, so it does not belong to a captured frame.
        if (hasLight)
            ReadConstants(c, 0, cb[lightSlot].buffer.Get(), lightSlot, hasCamera ? cb[cameraSlot].buffer.Get() : nullptr, crc, crc != 0);
    }
    catch (...)
    {
    }
}

// The bridge Present entry, for the sessions that have one (frame generation, the HDR presenter, a DX12 upscaler). It closes the frame
// the draw hooks collected at the exact end of the engine's frame; sessions without a bridge rely on the tone map split above.
void Present()
{
    State& s = S();
    if (s.phase.load(std::memory_order_relaxed) != kCapturing)
        return;
    try
    {
        CloseFrame(nullptr, false);
        AppendFrameTelemetry(s);
        if (s.framesCaptured >= s.wantedFrames)
            Finish();
    }
    catch (...)
    {
    }
}
// Keeps the bytecode of a shader whose reflection declares one of the creation-fixture shapes (DrawDiagFormat.h's IsCreationFixture), and its
// reflected names and slots, bounded and first come. Runs at shader creation on whatever thread creates shaders, so it takes the creation mutex
// for the list and the reflection mutex for the interned names - never both at once, and no D3D call under either. The CRC is stamped on the
// object here too: a shader kept from creation can be created while no capture is collecting (the key is on, the capture is not yet armed, or one
// was already written), and a draw row can only name it through the stamp. One session line says when the bound was reached, because a shape the
// list did not expect must not fill it silently.
void KeepCreationFixture(ID3D11PixelShader* shader, const void* code, size_t size, uint32_t crc)
{
    State& s = S();
    ShaderBinding parsed[kMaxBindings];
    const size_t count = ParseBindings(code, size, parsed, kMaxBindings);
    if (count == 0)
        return;
    uint32_t names = 0;
    for (size_t i = 0; i < count; ++i)
        names |= CreationNameBitsOf(std::string_view(parsed[i].name, parsed[i].nameLength));
    if (!IsCreationFixture(names))
        return;
    State::CreationFixture fixture;
    fixture.crc = crc;
    for (size_t i = 0; i < count; ++i)
        fixture.names.push_back(std::string(parsed[i].name, parsed[i].nameLength) + "=" + std::to_string(parsed[i].bindPoint));
    bool loggedBound = false;
    bool kept = false;
    {
        std::lock_guard lock(s.creationMutex);
        for (const State::CreationFixture& keptFixture : s.creationFixtures)
            if (keptFixture.crc == crc)
                return; // one copy per bytecode, however often the game creates a shader object from it
        if (!AdmitCreationFixture(names, s.creationFixtures.size(), s.creationBytes, size))
        {
            loggedBound = !s.creationBoundLogged;
            s.creationBoundLogged = true;
        }
        else
        {
            fixture.code.assign(static_cast<const char*>(code), size);
            s.creationBytes += size;
            s.creationFixtures.push_back(std::move(fixture));
            kept = true;
        }
    }
    if (loggedBound)
        LOG_INFO("Draw diagnostics: the shaders kept from creation are at their bound ({} shader(s) or {} MiB); a later key shader is not "
                 "kept",
                 kMaxCreationFixtures, kMaxCreationFixtureBytes >> 20);
    if (!kept)
        return;
    // A draw names the shader by this stamp and its slots by the reflection below: both are kept for a shader created while no capture could
    // collect it, so a capture armed later still prints its names and samples its buffers.
    if (shader != nullptr)
        shader->SetPrivateData(kShaderCrcTag, sizeof crc, &crc);
    {
        std::lock_guard lock(s.mutex);
        if (s.creationReflections.find(crc) == s.creationReflections.end() && s.creationReflections.size() < kMaxCreationFixtures)
        {
            Reflection reflection;
            reflection.binds.reserve(count);
            for (size_t i = 0; i < count; ++i)
                reflection.binds.push_back({ Intern(s, parsed[i].name, parsed[i].nameLength), BindingKindOf(parsed[i]), parsed[i].bindPoint,
                                             parsed[i].bindCount });
            s.creationReflections.emplace(crc, std::move(reflection));
        }
    }
}
} // namespace

// ---- the hooks -------------------------------------------------------------------------------------------------------

void OnDraw(ID3D11DeviceContext* context, const char* api)
{
    const uint8_t phase = S().phase.load(std::memory_order_relaxed);
    if (context == nullptr || (phase != kCapturing && phase != kWaiting))
        return;
    try
    {
        if (phase == kCapturing)
            Draw(context, api);
        else
            WaitTick(context);
    }
    catch (...)
    {
    }
}

void OnPixelShaderCreated(ID3D11PixelShader* shader, const void* code, size_t size, uint32_t crc)
{
    const bool capturing = Capturing(); // one relaxed load per created shader, plus one while no capture is armed
    if (shader == nullptr || crc == 0 || (!capturing && !S().creationOn.load(std::memory_order_relaxed)))
        return; // the draw diagnostic is off: no scan, no parse, nothing kept
    try
    {
        if (capturing)
            shader->SetPrivateData(kShaderCrcTag, sizeof crc, &crc); // the stamp a draw row names its CRC by
        // The creation fixtures are kept whatever the capture phase is (the byte scan first, so the reflection parse runs only for the few
        // shaders that can be one of the shapes); the reflection table below is filled only while a capture runs, as before.
        if (code != nullptr && size >= 32 && MentionsCreationFixtureName(code, size))
            KeepCreationFixture(shader, code, size, crc);
        if (!capturing)
            return;
        State& s = S();
        std::lock_guard lock(s.mutex);
        if (s.byCrc.size() >= kMaxReflectedShaders || s.bindings >= kMaxBindingsTotal || s.byCrc.find(crc) != s.byCrc.end())
            return;
        ShaderBinding parsed[kMaxBindings];
        const size_t count = ParseBindings(code, size, parsed, kMaxBindings);
        if (count == 0)
            return;
        // A shader that names one of the fork's identifying resources is kept as a fixture (bounded, and only while a capture runs). The
        // shaders that declare the sun mask or the light draw's own pair are kept ahead of the geometry passes that only name the camera
        // buffer, so a bound that fills up cannot hide the draw a lighting feature is looking for.
        {
            uint32_t bits = 0;
            for (size_t i = 0; i < count; ++i)
                bits |= FixtureBitsOf(std::string_view(parsed[i].name, parsed[i].nameLength));
            const bool key = IsKeyFixtureShader(bits);
            const bool lightDraw = IsLightDrawFixture(bits);
            if (AdmitFixture(bits, s.fixtures.size(), s.commonFixtures, s.lightFixtures, s.fixtureBytes, size))
            {
                State::Fixture fixture;
                fixture.crc = crc;
                fixture.code.assign(static_cast<const char*>(code), size);
                for (size_t i = 0; i < count; ++i)
                    fixture.names.push_back(std::string(parsed[i].name, parsed[i].nameLength) + "=" + std::to_string(parsed[i].bindPoint));
                s.fixtureBytes += size;
                if (lightDraw)
                    ++s.lightFixtures;
                else if (!key)
                    ++s.commonFixtures;
                s.fixtures.push_back(std::move(fixture));
            }
        }
        Reflection reflection;
        reflection.binds.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            reflection.binds.push_back({ Intern(s, parsed[i].name, parsed[i].nameLength), BindingKindOf(parsed[i]), parsed[i].bindPoint,
                                         parsed[i].bindCount });
            ++s.bindings;
        }
        s.byCrc.emplace(crc, std::move(reflection));
    }
    catch (...)
    {
    }
}

void OnUpscaleBoundary()
{
    State& s = S();
    if (s.phase.load(std::memory_order_relaxed) != kCapturing)
        return;
    s.markers.NoteUpscale(s.drawsInFrame);
}

void OnPresent()
{
    if (S().phase.load(std::memory_order_relaxed) != kCapturing) // the present path pays one relaxed load while the key is off
        return;
    Present();
}

void OnConfigLoaded(bool capture)
{
    State& s = S();
    // The creation fixtures follow the key, not the capture: the shaders a diagnostic needs are created while the game loads, and a capture
    // armed later can only have them if they were kept then.
    s.creationOn.store(capture, std::memory_order_relaxed);
    if (!capture)
    {
        const uint8_t phase = s.phase.load(std::memory_order_relaxed);
        if (phase == kCapturing || phase == kWaiting)
        {
            s.phase.store(kIdle, std::memory_order_relaxed);
            ext::SetActive(ext::F::DrawDiag, false);
        }
        return;
    }
    if (s.phase.load(std::memory_order_relaxed) != kIdle)
        return; // a capture is running, or this session already wrote its one capture
    s.wantedFrames = WantedFrames();
    s.arm = PlannedArm();
    s.dumpActive = DumpEnabled(ext::Cfg().ffxiv.drawDiagnosticsDump.value_or_default());
    s.dumpBudget = DumpBudgetBytes(ext::Cfg().ffxiv.drawDiagnosticsDumpBudgetMiB.value_or_default());
    // The clock of cb.csv starts here, so the first captured frame already has a sane "seconds into the capture" (its draws run before the
    // first Present). A delayed capture keeps this zero: its rows then say how long after the config load they were read. Reading the
    // performance counter is safe from the config load.
    s.clockZero = Qpc();
    if (s.arm.trigger == Trigger::Immediate)
    {
        TakeEngineBaseline(s); // the window starts with the first draw, so its baseline is the counters as they stand now
        TakeSsrBaseline(s);
        TakePuddleBaseline(s);
    }
    s.phase.store(s.arm.trigger == Trigger::Immediate ? kCapturing : kWaiting, std::memory_order_relaxed);
    ext::SetActive(ext::F::DrawDiag, true); // set last: a draw never sees the bit without a capture behind it
}
} // namespace ext::ffxiv::diag

namespace
{
// The key reaches the diagnostic through the config's own hook pointer (ext::FfxivConfig::drawDiagnosticsOnLoaded), set when this DLL's
// static initialisers run, so ext/core/ExtConfig.cpp keeps no link dependency on this file.
struct RegisterDrawDiagnostics
{
    RegisterDrawDiagnostics() { ext::FfxivConfig::drawDiagnosticsOnLoaded = &ext::ffxiv::diag::OnConfigLoaded; }
} g_registerDrawDiagnostics;
} // namespace
