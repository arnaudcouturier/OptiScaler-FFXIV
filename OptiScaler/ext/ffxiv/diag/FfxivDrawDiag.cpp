#include "pch.h"

#include "ext/ffxiv/diag/DrawDiag.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/diag/DrawDiagFormat.h"
#include "ext/ffxiv/diag/ShaderBindings.h"
#include "ext/ffxiv/lighting/FfxivLightingCapture.h" // the game's own context (the hooks' identity for "the game is drawing")
#include "ext/ffxiv/ui/PlateDraws.h"                 // the draw arguments the draw hooks already noted
#include "ext/ffxiv/ui/UiShaderCatalog.h"

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

// The bytecode CRC the creation hook stamps on a shader object, so a draw can name its pixel shader without seeing its bytecode.
constexpr GUID kCrcTag { 0x2d7b41c8, 0x9e04, 0x4a1d, { 0x8a, 0x3c, 0x1b, 0x6e, 0xd2, 0x57, 0x40, 0x91 } };

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
    uint32_t type = 0; // ext::ffxiv::diag::kBind*
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
    uint32_t omittedInFrame = 0;
    uint32_t framesCaptured = 0;
    uint32_t waitTicks = 0; // draws since the last key poll while waiting for the capture to start
    FrameSplitter split;
    std::string frameRows;
    MarkerPlan markers;
    std::string rows; // draws.csv body
    std::string cbs;  // cb.csv body
    bool truncated = false, truncatedCbs = false;
    uint64_t rowsWritten = 0, cbRows = 0, shadersWithoutReflection = 0;
    int64_t clockZero = 0;
    int64_t lastReadQpc = 0;
    Staging lightStaging, cameraStaging;

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

    // Reflection table: appended to by shader creation on any thread, read by the draw path under the same mutex. Kept for the session
    // (freeing it would race a creation that outlives the capture); bounded by the two caps below.
    std::mutex mutex;
    std::vector<std::string> names;
    std::unordered_map<std::string, uint32_t> nameIds;
    std::unordered_map<uint32_t, Reflection> byCrc;
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

// Creates OptiScaler\draw-diag\<run>\ and keeps the newest runs, deleting only folders it named itself.
fs::path MakeRunFolder()
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
    const std::string id = FormatRunId(local);
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

// The binding a shader declares at `slot`, or null.
const char* NameAt(const State& s, const Reflection* reflection, uint32_t type, uint32_t slot)
{
    if (reflection == nullptr)
        return nullptr;
    for (const Bound& b : reflection->binds)
    {
        const ShaderBinding probe { s.names[b.name].c_str(), static_cast<uint32_t>(s.names[b.name].size()), b.type, b.bindPoint, b.bindCount };
        if (BindingCovers(probe, type, slot))
            return probe.name;
    }
    return nullptr;
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

bool TextureSize(ID3D11Resource* resource, uint32_t* width, uint32_t* height)
{
    if (resource == nullptr)
        return false;
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return false;
    D3D11_TEXTURE2D_DESC desc {};
    static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
    *width = desc.Width;
    *height = desc.Height;
    return true;
}

std::string SizeText(uint32_t width, uint32_t height) { return std::to_string(width) + "x" + std::to_string(height); }

// One number per field, in the D3D11 enum order: enable, src, dst, op, srcAlpha, dstAlpha, opAlpha, write mask. With no blend state
// object bound D3D11 uses its default, which is blending off and every channel written.
std::string BlendText(ID3D11DeviceContext* c)
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

// Every bound render target as "slot:format:size|..." in the render target column, and the depth view as "format:size" in `depth`.
std::string RenderTargetText(ID3D11DeviceContext* c, std::string& depth)
{
    ID3D11RenderTargetView* views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] {};
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, views, &dsv);
    std::string text;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        if (views[i] == nullptr)
            continue;
        D3D11_RENDER_TARGET_VIEW_DESC desc {};
        views[i]->GetDesc(&desc);
        ComPtr<ID3D11Resource> resource;
        views[i]->GetResource(&resource);
        uint32_t width = 0, height = 0;
        const bool sized = TextureSize(resource.Get(), &width, &height);
        if (!text.empty())
            text += '|';
        text += std::to_string(i) + ":" + FormatText(uint32_t(desc.Format)) + ":" + (sized ? SizeText(width, height) : "?");
        views[i]->Release();
    }
    if (dsv != nullptr)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc {};
        dsv->GetDesc(&desc);
        ComPtr<ID3D11Resource> resource;
        dsv->GetResource(&resource);
        uint32_t width = 0, height = 0;
        const bool sized = TextureSize(resource.Get(), &width, &height);
        depth = FormatText(uint32_t(desc.Format)) + ":" + (sized ? SizeText(width, height) : "?");
        dsv->Release();
    }
    return text;
}

// ---- the named constant buffers of the lighting passes ----------------------------------------------------------------

// Reads the light parameters and the camera view matrix of one draw: position, direction and diffuse colour at the offsets
// DrawDiagFormat.h documents, and the view matrix rows the space of the direction is read against. One staging copy and one map per
// buffer, at most once a second, on the render thread. `frame` = the frame the read belongs to (0 while the diagnostic waits).
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

    // The camera buffer is read from the same draw, so the direction and the matrix always belong to one frame's state.
    if (camera != nullptr)
        if (const unsigned char* view = MapConstant(c, s.cameraStaging, camera, kCameraParamViewAt + kCameraParamMatrixBytes);
            view != nullptr)
        {
            std::memcpy(read.view, view + kCameraParamViewAt, sizeof read.view);
            read.hasView = true;
            UnmapConstant(c, s.cameraStaging);
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

// ---- files -----------------------------------------------------------------------------------------------------------

void AppendDraws(State& s, const std::string& text)
{
    if (s.truncated)
        return;
    if (s.rows.size() + text.size() > kDrawsBytes)
    {
        s.rows += kTruncatedLine;
        s.truncated = true;
        return;
    }
    s.rows += text;
}

void Finish()
{
    State& s = S();
    s.phase.store(kDone, std::memory_order_relaxed);
    ext::SetActive(ext::F::DrawDiag, false); // the draw path is back to one relaxed load
    const fs::path dir = MakeRunFolder();
    if (dir.empty())
    {
        LOG_WARN("Draw diagnostics: the capture could not be written");
        return;
    }
    const std::string draws = "# FFXIV draw diagnostics, format " + std::to_string(kFormatVersion) + "\n" + DrawsHeaderText() + s.rows;
    const std::string cbs = "# FFXIV draw diagnostics, format " + std::to_string(kFormatVersion) + "\n" + CbHeaderText() + s.cbs;
    if (!WriteFile(dir / kDrawsFile, draws) || !WriteFile(dir / kCbFile, cbs))
    {
        LOG_WARN("Draw diagnostics: a write to the run folder failed");
        return;
    }
    LOG_INFO("Draw diagnostics: {} frame(s), {} draw row(s), {} constant-buffer read(s) written to {}", s.framesCaptured, s.rowsWritten,
             s.cbRows, dir.string());
    if (s.truncated || s.truncatedCbs)
        LOG_WARN("Draw diagnostics: the byte cap of a file was reached, later rows were dropped");
    if (!s.fixtures.empty())
    {
        // The fixture shaders: one file per shader plus a list of what each one declares, so a reflection rule can be replayed offline on
        // the blob the game really handed to CreatePixelShader.
        const fs::path folder = dir / kShaderFolder;
        std::error_code ec;
        fs::create_directories(folder, ec);
        std::string list = "# FFXIV draw diagnostics, shader fixtures, format " + std::to_string(kFormatVersion) +
                           "\n# crc,size,name=bindPoint,...\n";
        for (const State::Fixture& fixture : s.fixtures)
        {
            char head[64];
            snprintf(head, sizeof head, "%08X,%zu", fixture.crc, fixture.code.size());
            std::string line = head;
            for (const std::string& name : fixture.names)
                line += "," + name;
            list += line + "\n";
            if (!ec)
                WriteFile(folder / (detail::Hex32(fixture.crc) + ".dxbc"), fixture.code);
        }
        if (!ec && !WriteFile(dir / kShadersFile, list))
            LOG_WARN("Draw diagnostics: the shader fixtures could not be listed");
        else
            LOG_INFO("Draw diagnostics: {} shader fixture(s) written", s.fixtures.size());
    }
    if (s.shadersWithoutReflection != 0)
        LOG_INFO("Draw diagnostics: {} draw(s) named a pixel shader whose reflection was not recorded (created before the key was on)",
                 s.shadersWithoutReflection);
}

// ---- one draw --------------------------------------------------------------------------------------------------------

struct CbSlot
{
    ComPtr<ID3D11Buffer> buffer;
    uint32_t bytes = 0;
};

// Closes the frame in progress: its rows, its marker line, the counters and the splitter. Called from the tone map draw (the frame
// split, which needs no Present) and from the bridge Present entry when a session has one.
void CloseFrame()
{
    State& s = S();
    const uint64_t frame = s.frames + 1;
    if (s.drawsInFrame >= kMinDrawsPerFrame) // shorter frames are loading screens, the title and the like
    {
        std::string text = std::move(s.frameRows);
        text += s.markers.Line(frame, s.drawsInFrame, s.omittedInFrame);
        AppendDraws(s, text);
        ++s.framesCaptured;
    }
    s.frameRows.clear();
    s.frames = frame;
    s.drawsInFrame = 0;
    s.omittedInFrame = 0;
    s.markers.Reset();
    s.split.Reset();
}

// The pixel shader's bytecode CRC, as the creation hook stamped it on the object (0 when it has none).
uint32_t ShaderCrc(ID3D11DeviceContext* c)
{
    ComPtr<ID3D11PixelShader> shader;
    c->PSGetShader(&shader, nullptr, nullptr);
    uint32_t crc = 0;
    if (shader)
    {
        UINT bytes = sizeof crc;
        if (FAILED(shader->GetPrivateData(kCrcTag, &bytes, &crc)) || bytes != sizeof crc)
            crc = 0;
    }
    return crc;
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

// While the diagnostic waits for its capture (a delay or a key), it still samples the light and camera constants once a second - the
// measurement of the sun direction's space needs a series over a moving camera - and checks every kWaitDrawsPerPoll draws whether it
// is time to start. Nothing else happens per draw on this path: no render target, viewport, blend or depth query.
void WaitTick(ID3D11DeviceContext* c);

// Starts the collection, once, from the render thread (the config load may run inside DllMain, where nothing may log). `clockZero`
// stays at the config load even for a delayed capture, so a cb.csv row says when in the session it was read.
void StartCollecting()
{
    State& s = S();
    s.phase.store(kCapturing, std::memory_order_relaxed);
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
    if (!s.announced)
        Announce(s);

    DrawRow row;
    row.frame = s.frames + 1;
    row.ordinal = ++s.drawsInFrame;
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
    struct SrvSlot
    {
        bool bound = false;
        uint32_t format = 0, width = 0, height = 0;
    } srv[kMaxSlots] {};
    c->PSGetShaderResources(0, kMaxSlots, views);
    for (size_t i = 0; i < kMaxSlots; ++i)
    {
        if (views[i] == nullptr)
            continue;
        ComPtr<ID3D11Resource> resource;
        views[i]->GetResource(&resource);
        D3D11_SHADER_RESOURCE_VIEW_DESC desc {};
        views[i]->GetDesc(&desc);
        srv[i].bound = true;
        srv[i].format = uint32_t(desc.Format);
        TextureSize(resource.Get(), &srv[i].width, &srv[i].height);
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

    row.rtv = RenderTargetText(c, row.dsv);
    row.viewport = ViewportText(c);
    row.blend = BlendText(c);
    row.depth = DepthText(c);

    // The names, from the reflection table. Every D3D query above is already done, so the mutex is never held across a D3D call.
    // The names of the two constant buffers are copied out here: the interned names live in a vector that shader creation still grows on
    // other threads, so a pointer into it must not outlive this block.
    std::string lightName;
    uint32_t lightSlot = 0;
    uint32_t cameraSlot = 0;
    bool hasCamera = false;
    {
        std::lock_guard lock(s.mutex);
        const Reflection* reflection = nullptr;
        if (row.hasCrc)
        {
            const auto it = s.byCrc.find(crc);
            if (it != s.byCrc.end())
                reflection = &it->second;
            else
                ++s.shadersWithoutReflection;
        }
        for (size_t i = 0; i < kMaxSlots; ++i)
        {
            if (!srv[i].bound)
                continue;
            const char* name = NameAt(s, reflection, kBindTexture, static_cast<uint32_t>(i));
            if (!row.srvs.empty())
                row.srvs += '|';
            row.srvs += std::to_string(i) + ":" + (name != nullptr ? name : "-") + ":" + FormatText(srv[i].format) + ":" +
                        (srv[i].width != 0 || srv[i].height != 0 ? SizeText(srv[i].width, srv[i].height) : "?");
        }
        for (size_t i = 0; i < 8; ++i)
        {
            if (!cb[i].buffer)
                continue;
            const char* name = NameAt(s, reflection, kBindConstantBuffer, static_cast<uint32_t>(i));
            if (!row.cbs.empty())
                row.cbs += '|';
            row.cbs += std::to_string(i) + ":" + (name != nullptr ? name : "-") + ":" + std::to_string(cb[i].bytes);
            if (name == nullptr)
                continue;
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
    }

    // The named constant buffers of the lighting and camera passes: at most once a second, one staging copy and one map each.
    if (!lightName.empty() && SampleDue(s))
        ReadConstants(c, s.frames + 1, cb[lightSlot].buffer.Get(), lightSlot, hasCamera ? cb[cameraSlot].buffer.Get() : nullptr, crc,
                      row.hasCrc);

    // Markers are noted for every draw, whether or not the row below is kept, so a capped frame still reports its ordinals.
    row.markers = s.markers.NoteDraw(row.stage, row.ordinal);
    if (s.drawsInFrame > kMaxRowsPerFrame)
        ++s.omittedInFrame;
    else
    {
        s.frameRows += DrawsRowText(row);
        ++s.rowsWritten;
    }
    // The frame end, in the draw stream: the tone map draw closes the frame in progress, so a capture completes in a session that
    // has no bridge Present at all (the reason a benchmark run with both keys set once wrote nothing).
    if (s.split.NoteDraw(row.stage, s.drawsInFrame))
    {
        CloseFrame();
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
    const bool start = s.arm.trigger == Trigger::Key ? KeyDown(s.arm.key)
                                                      : SecondsBetween(s.clockZero, Qpc()) >= s.arm.delaySeconds;
    if (start)
    {
        StartCollecting();
        return;
    }
    if (!SampleDue(s))
        return;
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
            const auto it = s.byCrc.find(crc);
            const Reflection* reflection = crc != 0 && it != s.byCrc.end() ? &it->second : nullptr;
            for (size_t i = 0; i < 8; ++i)
            {
                if (!cb[i].buffer)
                    continue;
                const char* name = NameAt(s, reflection, kBindConstantBuffer, static_cast<uint32_t>(i));
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
        CloseFrame();
        if (s.framesCaptured >= s.wantedFrames)
            Finish();
    }
    catch (...)
    {
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
    if (!Capturing() || shader == nullptr || crc == 0) // one relaxed load per created shader
        return;
    try
    {
        // The stamp goes on the object first: a shader whose reflection is skipped (the caps below) still names its CRC in a row.
        shader->SetPrivateData(kCrcTag, sizeof crc, &crc);
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
            reflection.binds.push_back({ Intern(s, parsed[i].name, parsed[i].nameLength), parsed[i].type, parsed[i].bindPoint,
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
    // The clock of cb.csv starts here, so the first captured frame already has a sane "seconds into the capture" (its draws run before the
    // first Present). A delayed capture keeps this zero: its rows then say how long after the config load they were read. Reading the
    // performance counter is safe from the config load.
    s.clockZero = Qpc();
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
