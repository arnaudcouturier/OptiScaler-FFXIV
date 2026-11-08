#pragma once
// Developer draw diagnostic, pure text side: the CSV formats, the frame marker rule, the caps and the run folder naming.
// No D3D, no file, no clock, so a unit test checks the format and the marker ordering without a device, and the reader of a
// file cannot drift from its writer.
//
// Files under <folder of the DLL>\OptiScaler\draw-diag\<run>\ :
//   draws.csv  one row per draw of each captured frame, then one `#` comment line per frame with the marker ordinals (byte cap)
//   cb.csv     one row per constant-buffer read (once a second, at the light pass that writes the frame's light buffers): the light's
//              position, direction and colour, and the same draw's camera view and view-projection
//   views.txt  the SRV description of the four identifying textures, one line per name per capture
//   frames.csv one row per diagnostic sample: SSR and puddle run/decline deltas, served counts, wetness and shape/tone pairing tokens
//              sample_time_token is FrameCount at the callback; module tokens are recorded separately because present paths can advance first
// Nothing in them is a path, a user name or a machine name: frames, slots, CRC32s, reflected shader names, formats, sizes and resource
// pointers.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ShaderBindings.h" // diag::ResourceBaseName: the reflected names carry the effect compiler's type suffix

namespace ext::ffxiv::diag
{
inline constexpr int kFormatVersion = 3;
inline constexpr const char* kRootFolder = "draw-diag";
inline constexpr const char* kDrawsFile = "draws.csv";
inline constexpr const char* kCbFile = "cb.csv";
inline constexpr const char* kViewsFile = "views.txt"; // the SRV description of four identifying textures, once per capture
inline constexpr const char* kFramesFile = "frames.csv"; // bounded per-output-frame SSR/puddle telemetry during a capture
inline constexpr const char* kShadersFile = "shaders.txt"; // the fixture list: crc, size and the reflected names of each kept shader
inline constexpr const char* kShaderFolder = "shaders";    // and their bytecode, one <crc>.dxbc per kept shader
inline constexpr size_t kKeepRuns = 8; // the newest runs kept next to each other; one session can start kMaxCapturesPerSession captures

// The fixture dump: pixel shaders whose reflection names one of the textures or constant buffers the fork identifies draws by. Bounded, so a
// capture cannot fill the disk: the game has a few dozen such shaders and each blob is a few kilobytes.
inline constexpr size_t kMaxFixtureShaders = 64;
inline constexpr size_t kMaxCommonFixtureShaders = 16; // kept for a common name only (see IsKeyFixtureShader)
inline constexpr size_t kMaxLightFixtures = 16;        // the light draw's own shape (see IsLightDrawFixture), on top of kMaxFixtureShaders
inline constexpr size_t kMaxFixtureBytes = 4ull << 20;

// The names a shader has to declare one of to be worth keeping. They are the ones the draw diagnostic and the contact shadow pass look up, so
// a fixture set holds every shape either of them classifies by. The sun mask answers to two names: `g_SamplerShadow` in the build the mask
// writers were measured in, and `g_SamplerShadowMask` in this game build's own sun light draw.
inline constexpr uint32_t kFixtureMask = 1u << 0;         // g_SamplerShadow / g_SamplerShadowMask
inline constexpr uint32_t kFixtureDepth = 1u << 1;        // g_SamplerDepth
inline constexpr uint32_t kFixtureLight = 1u << 2;        // g_LightParam
inline constexpr uint32_t kFixtureCamera = 1u << 3;       // g_CameraParameter
inline constexpr uint32_t kFixtureViewPosition = 1u << 4; // g_SamplerViewPosition
inline constexpr uint32_t kFixtureOcclusion = 1u << 5;    // g_SamplerOcclusion
inline constexpr uint32_t kFixtureAmbient = 1u << 6;      // g_AmbientParamArray

// Which of those names one reflected binding is, 0 for any other name. The engine's compiler writes the effect variables' type suffix into
// the reflection (`g_SamplerShadow.T`), so the name is compared by its variable name (diag::ResourceBaseName): comparing the raw one would
// give nothing for every texture of the game and the dump would stay empty.
inline constexpr uint32_t FixtureBitsOf(std::string_view name)
{
    const std::string_view base = ResourceBaseName(name);
    if (base == "g_SamplerShadow" || base == "g_SamplerShadowMask")
        return kFixtureMask;
    if (base == "g_SamplerDepth")
        return kFixtureDepth;
    if (base == "g_LightParam")
        return kFixtureLight;
    if (base == "g_CameraParameter")
        return kFixtureCamera;
    if (base == "g_SamplerViewPosition")
        return kFixtureViewPosition;
    if (base == "g_SamplerOcclusion")
        return kFixtureOcclusion;
    if (base == "g_AmbientParamArray")
        return kFixtureAmbient;
    return 0;
}

// A shader worth keeping even when the bound is contended: it declares the sun mask, or the light draw's own pair (the depth together with
// the light parameters), or a buffer only the lighting passes read. g_CameraParameter alone is what every ordinary geometry pass declares, so
// those are kept behind the key shapes instead of filling the bound with geometry and hiding the light draw.
inline constexpr bool IsKeyFixtureShader(uint32_t bits) noexcept
{
    return (bits & (kFixtureMask | kFixtureViewPosition | kFixtureOcclusion | kFixtureAmbient)) != 0 ||
           ((bits & kFixtureDepth) != 0 && (bits & kFixtureLight) != 0);
}

// The shape a lighting feature is looking for: the screen-space mask, or the depth together with the light parameters. The ambient-occlusion
// passes declare the occlusion buffer, which makes them key shapes too, and a session creates dozens of them before the sun light draw
// arrives; a bound those passes share would be full by then. So this shape keeps slots of its own (kMaxLightFixtures) on top of the bound.
inline constexpr bool IsLightDrawFixture(uint32_t bits) noexcept
{
    return (bits & kFixtureMask) != 0 || ((bits & kFixtureDepth) != 0 && (bits & kFixtureLight) != 0);
}

// Whether one more shader is kept, given what the dump holds (`kept` in all, `common` for a common name only, `light` for the light draw's
// shape) and its byte budget. The light draw's shape is admitted up to its own bound first: a session's ambient-occlusion shaders are key
// shapes too, and with one shared bound they pushed the sun light draw out of the dump the run was made for.
inline constexpr bool AdmitFixture(uint32_t bits, size_t kept, size_t common, size_t light, size_t bytes, size_t size) noexcept
{
    if (bits == 0 || bytes + size > kMaxFixtureBytes || kept >= kMaxFixtureShaders + kMaxLightFixtures)
        return false;
    if (IsLightDrawFixture(bits))
        return light < kMaxLightFixtures;
    return kept < kMaxFixtureShaders && (IsKeyFixtureShader(bits) || common < kMaxCommonFixtureShaders);
}

// ---- the shaders kept from creation ------------------------------------------------------------------------------------
// The passes a knob is judged by are created while the game loads, before any capture can be armed, so a capture-only fixture list never
// holds them. Their bytecode is kept for the session as soon as the shader is created: the deferred composite (the ambient probe table), the
// wetness pass (its buffer and the two textures it reads), the hierarchical-Z pass, and the glare chain's parameters. Bounded and first come,
// because a shape this list did not expect must not grow it without limit.
inline constexpr size_t kMaxCreationFixtures = 32;
inline constexpr uint64_t kMaxCreationFixtureBytes = 2ull << 20;

// One bit per reflected name that makes a shader worth keeping from creation. The shape rule below is what qualifies (`IsCreationFixture`).
enum CreationNameBits : uint32_t
{
    kKeepAmbientParam = 1u << 0,     // g_AmbientParamArray
    kKeepWetnessParam = 1u << 1,     // g_WetnessParameter
    kKeepGbuffer3 = 1u << 2,         // g_SamplerGBuffer3
    kKeepViewPosition = 1u << 3,     // g_SamplerViewPosition
    kKeepHierarchicalZ = 1u << 4,    // g_SamplerHierarchicalZ
    kKeepGlareParam = 1u << 5,       // cGlareParam
    kKeepBrightPassParam = 1u << 6,  // cBrightPassParam
    kKeepMergeWeight = 1u << 7,      // cMergeWeight
    kKeepGaussianBlurParam = 1u << 8, // cGaussianBlurParam
    kKeepSoftFocusParam = 1u << 9,   // cSoftFocusParam
    kKeepSunParam = 1u << 10,        // cSunParam: the sun sprite (and, through the radial blurs, the ghost discs)
    kKeepHaloParam = 1u << 11,       // cHaloParam: the ring
    kKeepRadialBlurParam = 1u << 12, // cRadialBlurParam: the ghost step of each radial-blur pass
    kKeepShadowMask = 1u << 13,      // g_SamplerShadowMask: this game build's sun light draw declares it while the game loads
};

// The wetness pass's three names, the glare chain's five parameters, and the flare family's three parameters, as masks of the bits above.
inline constexpr uint32_t kCreationWetness = kKeepWetnessParam | kKeepGbuffer3 | kKeepViewPosition;
inline constexpr uint32_t kCreationGlare =
    kKeepGlareParam | kKeepBrightPassParam | kKeepMergeWeight | kKeepGaussianBlurParam | kKeepSoftFocusParam;
inline constexpr uint32_t kCreationFlare = kKeepSunParam | kKeepHaloParam | kKeepRadialBlurParam;

// What one reflected name contributes, 0 for any other name. The comparison strips the effect compiler's `.T`/`.S` suffix, like the
// in-capture fixture rule does, because a texture is only ever named `g_SamplerGBuffer3.T` in the reflection.
inline constexpr uint32_t CreationNameBitsOf(std::string_view name)
{
    const std::string_view base = ResourceBaseName(name);
    if (base == "g_AmbientParamArray")
        return kKeepAmbientParam;
    if (base == "g_WetnessParameter")
        return kKeepWetnessParam;
    if (base == "g_SamplerGBuffer3")
        return kKeepGbuffer3;
    if (base == "g_SamplerViewPosition")
        return kKeepViewPosition;
    if (base == "g_SamplerHierarchicalZ")
        return kKeepHierarchicalZ;
    if (base == "g_SamplerShadowMask")
        return kKeepShadowMask;
    if (base == "cGlareParam")
        return kKeepGlareParam;
    if (base == "cBrightPassParam")
        return kKeepBrightPassParam;
    if (base == "cMergeWeight")
        return kKeepMergeWeight;
    if (base == "cGaussianBlurParam")
        return kKeepGaussianBlurParam;
    if (base == "cSoftFocusParam")
        return kKeepSoftFocusParam;
    if (base == "cSunParam")
        return kKeepSunParam;
    if (base == "cHaloParam")
        return kKeepHaloParam;
    if (base == "cRadialBlurParam")
        return kKeepRadialBlurParam;
    return 0;
}

// Whether the names a shader declares make it one of the shapes above: the ambient table alone, the hierarchical-Z alone, the sun mask alone (the
// light draw that reads it is created while the game loads, so a capture-only list never holds it), any glare parameter, any flare parameter, or
// all three names of the wetness pass (one of them alone is a G-buffer reader the list does not need).
inline constexpr bool IsCreationFixture(uint32_t names) noexcept
{
    return (names & kKeepAmbientParam) != 0 || (names & kKeepHierarchicalZ) != 0 || (names & kKeepShadowMask) != 0 ||
           (names & kCreationGlare) != 0 || (names & kCreationFlare) != 0 || (names & kCreationWetness) == kCreationWetness;
}

// Whether one more shader is kept from creation, given what the list holds and its byte bound. First come: a shader whose shape qualifies
// after the bound is reached is not kept, and the session says so once.
inline constexpr bool AdmitCreationFixture(uint32_t names, size_t kept, uint64_t bytes, size_t size) noexcept
{
    return IsCreationFixture(names) && kept < kMaxCreationFixtures && bytes + size <= kMaxCreationFixtureBytes;
}

// The byte scan that runs before the reflection parse at shader creation: the names above are the only ones this list can use, and most of
// the game's shaders hold none of them, so one pass over the bytes drops those before any parse. The names arrive as the effect compiler
// writes them (`g_SamplerGBuffer3.T`), so the plain name is a prefix of what the blob holds.
inline bool MentionsCreationFixtureName(const void* code, size_t size) noexcept
{
    if (code == nullptr)
        return false;
    const auto* bytes = static_cast<const unsigned char*>(code);
    for (size_t i = 0; i < size; ++i)
    {
        // Every name starts with `g_` or `c`, and the byte after a `g` is always `_`: two byte compares before any string compare.
        if (bytes[i] == 'c')
        {
            for (const std::string_view name : { std::string_view("cGlareParam"), std::string_view("cBrightPassParam"),
                                                 std::string_view("cMergeWeight"), std::string_view("cGaussianBlurParam"),
                                                 std::string_view("cSoftFocusParam"), std::string_view("cSunParam"),
                                                 std::string_view("cHaloParam"), std::string_view("cRadialBlurParam") })
                if (name.size() <= size - i && std::memcmp(bytes + i, name.data(), name.size()) == 0)
                    return true;
        }
        else if (bytes[i] == 'g' && i + 1 < size && bytes[i + 1] == '_')
        {
            for (const std::string_view name : { std::string_view("g_AmbientParamArray"), std::string_view("g_WetnessParameter"),
                                                 std::string_view("g_SamplerGBuffer3"), std::string_view("g_SamplerViewPosition"),
                                                 std::string_view("g_SamplerHierarchicalZ"), std::string_view("g_SamplerShadowMask") })
                if (name.size() <= size - i && std::memcmp(bytes + i, name.data(), name.size()) == 0)
                    return true;
        }
    }
    return false;
}

// The list shaders.txt starts with: the CRC, the blob size, where the copy was kept (creation = when the shader was created, before any
// capture; capture = while the capture ran) and the reflected names with their bind points. Old format-3 folders carry the two-column head
// without the origin, which is what a reader treats as `capture`.
inline std::string FixtureListHeader()
{
    return "# FFXIV draw diagnostics, shader fixtures, format " + std::to_string(kFormatVersion) +
           "\n# crc,size,origin,name=bindPoint,..."
           "\n# origin: creation = kept when the shader was created, before any capture; capture = kept while a capture ran\n";
}

inline std::string FixtureListLine(uint32_t crc, size_t size, bool fromCreation, const std::vector<std::string>& names)
{
    char head[64];
    snprintf(head, sizeof head, "%08X,%zu,%s", crc, size, fromCreation ? "creation" : "capture");
    std::string line = head;
    for (const std::string& name : names)
        line += "," + name;
    return line + "\n";
}

// Bounds. A live frame of this game is about 7,500 draws (a window of 8,824 was measured when a window still spanned two frames), so the
// row cap sits above one frame with room for a heavier one, and the byte cap covers a whole capture's rows: a format-3 row is about 150 to
// 200 bytes, and 12,000 rows of a frame fit well under a quarter of it.
inline constexpr uint64_t kDrawsBytes = 32ull << 20;
inline constexpr uint64_t kCbBytes = 64ull << 10;
inline constexpr uint32_t kMaxRowsPerFrame = 12000;
inline constexpr uint32_t kMinDrawsPerFrame = 32; // fewer draws than this is a loading or logo frame and is not counted
inline constexpr int32_t kDefaultFrames = 3;
inline constexpr int32_t kMaxFrames = 16;
inline constexpr size_t kMaxReflectedShaders = 20000; // bounded: names are interned, a session sees about 33k pixel shaders
inline constexpr size_t kMaxBindingsTotal = 262144;
inline constexpr size_t kMaxSlots = 16; // pixel shader resource slots read per draw

// Shader stages of the game's frame the diagnostic knows by name (ext/ffxiv/ui/UiShaderCatalog.h); 0 = not a known stage.
inline constexpr uint8_t kStageNone = 0;
inline constexpr uint8_t kStageToneMap = 1;
inline constexpr uint8_t kStageUi = 6;

// How a capture starts ([FFXIV] DrawDiagnosticsFrames' siblings DrawDiagnosticsDelaySeconds and DrawDiagnosticsKey).
//
// Immediate collects from the config load, which is the opening frames of the game's session. Delay waits that many seconds first,
// so the capture lands on a gameplay frame (particles, effects, the character in the scene) instead of the loading and logo frames.
// Key waits for a key press instead of a time. The key wins when both keys are set: a fixed delay and a press are alternatives, and
// the one that is harder to mistake for an accident is the press.
inline constexpr int32_t kMaxDelaySeconds = 3600;    // one hour; a longer value is clamped
inline constexpr uint32_t kNoKey = 0;                // "off"
inline constexpr uint32_t kWaitDrawsPerPoll = 64;    // draws between two key polls while the diagnostic waits
inline constexpr double kSampleSeconds = 1.0;        // how often the constant buffers are read

enum class Trigger : uint8_t
{
    Immediate = 0,
    Delay = 1,
    Key = 2,
};

struct ArmPlan
{
    Trigger trigger = Trigger::Immediate;
    double delaySeconds = 0.0;
    uint32_t key = kNoKey; // Windows virtual-key code while trigger == Trigger::Key
};

// A key name as GetAsyncKeyState takes it: one letter (A..Z), one digit (0..9), F1..F24, or anything else for "off". The names are
// the ones a person writes in an ini; the function is here, not in the runtime, so the parser is covered by a unit test.
constexpr uint32_t ParseKeyName(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    if (text.size() == 1)
    {
        const char c = text[0];
        if (c >= 'a' && c <= 'z')
            return uint32_t(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z')
            return uint32_t(c);
        if (c >= '0' && c <= '9')
            return uint32_t(c);
        return kNoKey;
    }
    if (text.size() >= 2 && text.size() <= 3 && (text[0] == 'f' || text[0] == 'F'))
    {
        uint32_t number = 0;
        for (size_t i = 1; i < text.size(); ++i)
        {
            if (text[i] < '0' || text[i] > '9')
                return kNoKey;
            number = number * 10 + uint32_t(text[i] - '0');
        }
        if (number >= 1 && number <= 24)
            return 0x70 + number - 1; // VK_F1
    }
    return kNoKey; // "off" and every name this build does not know
}

// The arming rule of the two keys: the delay is clamped to 0..kMaxDelaySeconds, and a key that parses wins over a delay.
constexpr ArmPlan PlanArm(int32_t delaySeconds, std::string_view keyName) noexcept
{
    ArmPlan plan;
    plan.key = ParseKeyName(keyName);
    if (plan.key != kNoKey)
    {
        plan.trigger = Trigger::Key;
        return plan;
    }
    const int32_t delay = delaySeconds < 0 ? 0 : (delaySeconds > kMaxDelaySeconds ? kMaxDelaySeconds : delaySeconds);
    plan.delaySeconds = double(delay);
    plan.trigger = delay > 0 ? Trigger::Delay : Trigger::Immediate;
    return plan;
}

// The capture session's re-arm rule ([FFXIV] DrawDiagnosticsKey). One press starts one capture; a capture lasts about fifty milliseconds,
// so the press that started it is usually still held when the capture ends, and without the release requirement that same press would start
// the next capture immediately. So a press is spent when it starts a capture, and only a release arms the next one. A session starts at
// most kMaxCapturesPerSession captures: each one is a whole set of files, and a stuck key must not fill the disk.
inline constexpr uint32_t kMaxCapturesPerSession = 8;

struct CaptureArm
{
    bool pressSpent = false; // the press that started the last capture has not come back up since
    uint32_t captures = 0;   // captures this session started

    bool Exhausted() const { return captures >= kMaxCapturesPerSession; }

    // One poll of the key while the diagnostic waits. True = start a capture now.
    bool Poll(bool keyDown)
    {
        if (Exhausted())
            return false;
        if (!keyDown)
        {
            pressSpent = false; // the press that started the last capture is over
            return false;
        }
        if (pressSpent)
            return false;
        ++captures;
        pressSpent = true;
        return true;
    }
};

// The capture's frame end, in the draw stream.
//
// A frame used to be closed only at the D3D11-on-D3D12 bridge's Present entry (the fork's only Present hook). A session that upscales
// on the game's own D3D11 device has no bridge swapchain at all - the benchmark log line "Failed to get ID3D12CommandQueue from
// pDevice, creating Dx11 swapchain!" is that case - so no Present ever arrived, no frame was ever closed, and a capture with both keys
// set wrote nothing. The rule below needs no Present: the game's tone map draw runs exactly once per frame, so the draws from just
// after one of them to just after the next are one frame's worth of work, whatever presents the picture.
//
// The tone map that closes a window is also the boundary of the next one, so it stays armed: the window after it ends at the NEXT tone
// map. A splitter that reset its armed state at a close made every second window span two game frames (measured windows of 8,824 and
// 15,079 draws against a live frame of about 7,500).

// What one draw did to the split.
inline constexpr uint8_t kSplitArm = 1;   // the tone map that opens the first window: the rows before it are a partial frame
inline constexpr uint8_t kSplitClose = 2; // the tone map that closes the window in progress

struct FrameSplitter
{
    bool toneSeen = false; // the window in progress already carries a tone map draw
    bool fresh = true;     // no frame has been closed yet: the first tone map must drop the partial one

    // The bridge Present closed the frame in progress at the engine's own frame end. The next window starts there, so its tone map only
    // arms it: the rows after a Present are a whole frame already and must not be dropped as a partial one.
    void Reset()
    {
        toneSeen = false;
        fresh = false;
    }

    // What this draw does to the window, as kSplit* bits (0 = nothing). Called AFTER the draw's own row was collected, so the tone map
    // draw is the window's last row and its `tone` marker carries the last ordinal: a captured window runs from just after one tone map
    // to just after the next. `drawsInFrame` = rows collected for the window in progress; a shorter frame is a loading screen or a replay
    // and is never split.
    uint8_t NoteDraw(uint8_t stage, uint32_t drawsInFrame)
    {
        if (stage != kStageToneMap)
            return 0;
        if (!toneSeen)
        {
            toneSeen = true;
            if (!fresh)
                return 0;
            fresh = false;
            return kSplitArm;
        }
        if (drawsInFrame < kMinDrawsPerFrame)
            return 0;
        return kSplitClose;
    }
};

// Row markers. A tone map and the frame's first UI draw are properties of a draw; the upscale boundary happens between two
// draws, so it is carried by the row of the draw that follows it.
inline constexpr uint8_t kMarkerTone = 1;
inline constexpr uint8_t kMarkerUpscale = 2;
inline constexpr uint8_t kMarkerFirstUi = 4;

inline constexpr const char* kTruncatedLine = "# TRUNCATED: byte cap of this file reached, later rows were dropped\n";

// The self-check trailer of each file. A reader compares it with the rows it parsed: a file cut off by a failed copy, a full disk or a
// capped write ends without the line or with the wrong count, which is the failure that hid a lost part of an earlier format for weeks.
inline std::string DrawsTrailerText(uint64_t rows, uint64_t frames, uint64_t omitted)
{
    return "# rows " + std::to_string(rows) + " frames " + std::to_string(frames) + " omitted " + std::to_string(omitted) + "\n";
}

inline std::string RowsTrailerText(uint64_t rows) { return "# rows " + std::to_string(rows) + "\n"; }

// What one frame contributes to draws.csv's trailer. A frame whose text the byte cap refused is not in the file, so it contributes neither
// its rows nor its frame line: the trailer has to agree with the rows a reader parses, or a capped file looks like a cut-off copy.
struct FrameTally
{
    uint64_t rows = 0, omitted = 0;
    bool frame = false; // the frame line was appended
};

inline constexpr FrameTally TallyFrame(uint32_t draws, uint32_t omitted, bool appended) noexcept
{
    if (!appended)
        return {};
    return FrameTally { draws >= omitted ? uint64_t(draws - omitted) : 0, uint64_t(omitted), true };
}

// Which rows carry which marker, for one frame. The rule is the whole point of the type: the tone map and the first UI draw
// are marked once per frame on their own row, and a boundary is marked on the next row, never twice (two boundaries before the
// next draw keep the first one, which is the one the draw order is read against).
struct MarkerPlan
{
    bool toneSeen = false, uiSeen = false, upscalePending = false;
    uint32_t toneOrdinal = 0, uiOrdinal = 0, upscaleAfter = 0; // 0 = not in this frame

    void Reset() { *this = MarkerPlan {}; }

    uint8_t NoteDraw(uint8_t stage, uint32_t ordinal)
    {
        uint8_t bits = 0;
        if (upscalePending)
        {
            bits |= kMarkerUpscale;
            upscalePending = false;
        }
        if (stage == kStageToneMap && !toneSeen)
        {
            toneSeen = true;
            toneOrdinal = ordinal;
            bits |= kMarkerTone;
        }
        if (stage == kStageUi && !uiSeen)
        {
            uiSeen = true;
            uiOrdinal = ordinal;
            bits |= kMarkerFirstUi;
        }
        return bits;
    }

    // `lastOrdinal` = the ordinal of the last draw of the frame (0 when the frame has none yet).
    void NoteUpscale(uint32_t lastOrdinal)
    {
        if (upscalePending)
            return;
        upscalePending = true;
        upscaleAfter = lastOrdinal;
    }

    // The frame's comment line, written after its rows. `omitted` = rows the per-frame row cap dropped.
    std::string Line(uint64_t frame, uint32_t draws, uint32_t omitted = 0) const
    {
        char text[192];
        snprintf(text, sizeof text, "# frame %llu draws %u tone %u ui1 %u upscale_after %u", static_cast<unsigned long long>(frame), draws,
                 toneOrdinal, uiOrdinal, upscaleAfter);
        std::string line = text;
        if (omitted != 0)
            line += " omitted " + std::to_string(omitted);
        return line + "\n";
    }
};

namespace detail
{
// A float without trailing noise: 0.5 stays "0.5", 1920 stays "1920".
inline std::string Number(float v)
{
    char text[32];
    snprintf(text, sizeof text, "%.6g", double(v));
    return text;
}

inline std::string Hex32(uint32_t v)
{
    char text[16];
    snprintf(text, sizeof text, "0x%08X", v);
    return text;
}
} // namespace detail

// One draw, as the hook collected it. The variable-length parts arrive already formatted (slot:name:format:size:pointer|...), so this
// function only joins and formats numbers.
struct DrawRow
{
    uint64_t frame = 0;
    uint32_t ordinal = 0;
    const char* api = "";
    bool gameContext = true; // the draw came from the context the lighting hooks latched onto
    uint32_t count = 0, instances = 0, start = 0;
    int32_t baseVertex = 0;
    uint8_t stage = kStageNone;
    uint32_t psCrc = 0;
    bool hasCrc = false;
    uint8_t markers = 0;
    std::string srvs;     // "0:g_SamplerDepth:0x2C:3840x2160:1a2b3c4d5e6f|3:-:0x0A:1920x1080:0"
    std::string cbs;      // "0:g_CommonParameter:64:0|2:g_LightParam:400:2a3b4c5d6e7f"  (bytes = the buffer's own size)
    std::string rtv;      // "0:0x0A:3840x2160:3a4b5c6d7e8f|1:0x0B:3840x2160:4a5b6c7d8e9f"
    std::string dsv;      // "0x2C:3840x2160:5a6b7c8d9eaf" or "-"
    std::string viewport; // "1:3840x2160+0+0"
    std::string blend;    // "0/5/1/1/5/1/1/15" (enable/src/dst/op/srcA/dstA/opA/write mask)
    std::string depth;    // "1/1/4/0" (enable/write/func/stencil)
};

inline std::string DrawsHeaderText()
{
    return "frame,ordinal,api,context,count,instances,start,base,stage,ps_crc,srv,cb,rtv,dsv,viewport,blend,depth,markers\n";
}

inline std::string MarkerText(uint8_t markers)
{
    std::string text;
    const auto add = [&text](const char* name)
    {
        if (!text.empty())
            text += '+';
        text += name;
    };
    if (markers & kMarkerTone)
        add("tone");
    if (markers & kMarkerUpscale)
        add("upscale");
    if (markers & kMarkerFirstUi)
        add("ui1");
    return text;
}

// A resource pointer as a reader compares it: lowercase hex, no 0x, no padding, "0" for none. The same texture has the same text in a
// target column and in the slot of a later draw that reads it, so a target and its readers can be joined without a symbol table.
inline std::string PointerText(const void* pointer)
{
    char text[32];
    snprintf(text, sizeof text, "%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pointer)));
    return text;
}

// One entry of the draw row's variable-length columns, in the format-3 shape (the pointer is the resource behind the view, or the
// constant buffer itself); `name` is "-" when the shader declares no binding for the slot.
inline std::string SrvEntryText(uint32_t slot, std::string_view name, std::string_view format, std::string_view size, const void* pointer)
{
    return std::to_string(slot) + ":" + std::string(name) + ":" + std::string(format) + ":" + std::string(size) + ":" +
           PointerText(pointer);
}

inline std::string CbEntryText(uint32_t slot, std::string_view name, uint32_t bytes, const void* pointer)
{
    return std::to_string(slot) + ":" + std::string(name) + ":" + std::to_string(bytes) + ":" + PointerText(pointer);
}

inline std::string TargetEntryText(uint32_t slot, std::string_view format, std::string_view size, const void* pointer)
{
    return std::to_string(slot) + ":" + std::string(format) + ":" + std::string(size) + ":" + PointerText(pointer);
}

inline std::string DepthEntryText(std::string_view format, std::string_view size, const void* pointer)
{
    return std::string(format) + ":" + std::string(size) + ":" + PointerText(pointer);
}

// ---- views.txt: the SRV description of the textures the lighting and reflection work has to identify ----------------------------------
// A texture is named by its variable in the reflection (`g_SamplerReflectionArray.T`), so the list is compared by base name. The first draw
// of a capture that binds one of these writes one line: what the view says about itself (dimension, mip count, format, array size or cube
// count) and what the texture behind it is, which tells a real array or cube from a 1x1 dummy.
inline constexpr const char* kViewNames[] { "g_SamplerReflectionArray", "g_SamplerReflectionMap", "g_SamplerHierarchicalZ",
                                            "g_SamplerShadow" };
inline constexpr size_t kViewNameCount = sizeof(kViewNames) / sizeof(kViewNames[0]);

// The index of `name` in the list, or -1. Compared by the variable's name: the engine's compiler appends `.T` to a texture.
inline int32_t ViewNameIndex(std::string_view name)
{
    const std::string_view base = ResourceBaseName(name);
    for (size_t i = 0; i < kViewNameCount; ++i)
        if (base == kViewNames[i])
            return static_cast<int32_t>(i);
    return -1;
}

// The name of a D3D11_SRV_DIMENSION value (the numbers are the API's own), for the views file.
inline std::string SrvDimensionText(uint32_t dimension)
{
    switch (dimension)
    {
    case 0: return "UNKNOWN";
    case 1: return "BUFFER";
    case 2: return "TEXTURE1D";
    case 3: return "TEXTURE1DARRAY";
    case 4: return "TEXTURE2D";
    case 5: return "TEXTURE2DARRAY";
    case 6: return "TEXTURE2DMS";
    case 7: return "TEXTURE2DMSARRAY";
    case 8: return "TEXTURE3D";
    case 9: return "TEXTURECUBE";
    case 10: return "TEXTURECUBEARRAY";
    case 11: return "BUFFEREX";
    default: return "dim" + std::to_string(dimension);
    }
}

struct ViewSample
{
    uint32_t ordinal = 0;  // the draw that first bound the texture
    std::string name;      // the variable's name, without the compiler's type suffix
    uint32_t viewDimension = 0, viewMips = 0, arraySize = 0, cubes = 0;
    std::string viewFormat;
    uint32_t width = 0, height = 0, textureArray = 0, textureMips = 0;
    std::string textureFormat;
    const void* pointer = nullptr;
};

inline std::string ViewsFileHeader()
{
    return "# FFXIV draw diagnostics, views, format " + std::to_string(kFormatVersion) + "\n";
}

inline std::string ViewsHeaderText()
{
    return "name,ordinal,view_dimension,view_mips,view_format,array_size,cubes,width,height,texture_array,texture_mips,texture_format,"
           "pointer\n";
}

inline std::string ViewSampleText(const ViewSample& sample)
{
    std::string text = sample.name + "," + std::to_string(sample.ordinal) + "," + SrvDimensionText(sample.viewDimension) + "," +
                       std::to_string(sample.viewMips) + "," + sample.viewFormat + "," + std::to_string(sample.arraySize) + "," +
                       std::to_string(sample.cubes) + "," + std::to_string(sample.width) + "," + std::to_string(sample.height) + "," +
                       std::to_string(sample.textureArray) + "," + std::to_string(sample.textureMips) + "," + sample.textureFormat + "," +
                       PointerText(sample.pointer);
    return text + '\n';
}

inline std::string DrawsRowText(const DrawRow& row)
{
    char head[192];
    snprintf(head, sizeof head, "%llu,%u,%s,%s,%u,%u,%u,%d,%u,", static_cast<unsigned long long>(row.frame), row.ordinal,
             row.api != nullptr ? row.api : "", row.gameContext ? "game" : "other", row.count, row.instances, row.start, row.baseVertex,
             uint32_t(row.stage));
    std::string text = head;
    text += row.hasCrc ? detail::Hex32(row.psCrc) : std::string("-");
    text += ',';
    text += row.srvs;
    text += ',';
    text += row.cbs;
    text += ',';
    text += row.rtv;
    text += ',';
    text += row.dsv.empty() ? std::string("-") : row.dsv;
    text += ',';
    text += row.viewport.empty() ? std::string("-") : row.viewport;
    text += ',';
    text += row.blend.empty() ? std::string("-") : row.blend;
    text += ',';
    text += row.depth.empty() ? std::string("-") : row.depth;
    text += ',';
    text += MarkerText(row.markers);
    text += '\n';
    return text;
}

// One read of a named constant buffer at the offsets the lighting passes carry: position at 0x00, direction at 0x10, diffuse
// colour at 0x20 (three floats each).
inline constexpr uint32_t kLightParamBytes = 0x30;
inline constexpr uint32_t kLightParamPositionAt = 0x00;
inline constexpr uint32_t kLightParamDirectionAt = 0x10;
inline constexpr uint32_t kLightParamDiffuseAt = 0x20;

// The camera constant buffer of the same draw, read beside the light: the view matrix at +0x00 (64 B), the view-projection at +0x060 (64 B) and
// the projection at +0x120 (64 B). The light's direction is in view space - a capture shows it turning with the view matrix while the frame's
// directional-shadow axis stays put - so the projection is what the module projects it through, and the view's second row is the world's up in
// view space, which is how the engine.txt sun line says whether that sun is above the horizon. A direction in world space stays the same while the
// view matrix turns; a direction in view space turns with it. The view-projection's first three columns carry the view's rotation and its w row is
// the projection's -1 row, so a row that is not of unit length (or a matrix that is the view or the identity) is visible here instead of only in a
// session's log. `kCameraParamMatrixBytes` is one whole matrix; the buffer's own layout puts the inverse view's row 0 at +0x030, which is why the
// second matrix starts at +0x060 and not at +0x040, and the projection at +0x120.
inline constexpr uint32_t kCameraParamViewAt = 0x00;
inline constexpr uint32_t kCameraParamViewProjectionAt = 0x060;
inline constexpr uint32_t kCameraParamProjectionAt = 0x120;
inline constexpr uint32_t kCameraParamMatrixBytes = 0x40;

struct ConstantRead
{
    uint64_t frame = 0;    // 0 = read while the diagnostic was still waiting for its capture to start
    double seconds = 0.0;  // since the config load armed the diagnostic
    uint32_t psCrc = 0;
    bool hasCrc = false;
    std::string name; // the reflected constant-buffer name
    uint32_t slot = 0, bytes = 0;
    float position[3] {}, direction[3] {}, diffuse[3] {};
    bool hasView = false;
    float view[16] {}; // the view matrix of the same draw, row-major as the game stores it
    bool hasViewProjection = false;
    float viewProjection[16] {}; // and the view-projection that follows it, row-major too: the frame's camera, for a cut's own measurement
    bool hasProjection = false;
    float projection[16] {}; // and the projection at +0x120: what the light's view-space direction is projected through
};

inline std::string CbHeaderText()
{
    std::string text = "frame,time_s,ps_crc,cb,slot,bytes,pos_x,pos_y,pos_z,dir_x,dir_y,dir_z,diffuse_r,diffuse_g,diffuse_b";
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            text += ",view_" + std::to_string(row) + std::to_string(column);
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            text += ",vp_" + std::to_string(row) + std::to_string(column);
    return text + '\n';
}

inline std::string CbRowText(const ConstantRead& read)
{
    char head[192];
    snprintf(head, sizeof head, "%llu,%.3f,%s,%s,%u,%u", static_cast<unsigned long long>(read.frame), read.seconds,
             read.hasCrc ? detail::Hex32(read.psCrc).c_str() : "-", read.name.c_str(), read.slot, read.bytes);
    std::string text = head;
    for (const float* triple : { read.position, read.direction, read.diffuse })
        for (int i = 0; i < 3; ++i)
        {
            text += ',';
            text += detail::Number(triple[i]);
        }
    for (int i = 0; i < 16; ++i)
    {
        text += ',';
        text += read.hasView ? detail::Number(read.view[i]) : std::string("-");
    }
    for (int i = 0; i < 16; ++i)
    {
        text += ',';
        text += read.hasViewProjection ? detail::Number(read.viewProjection[i]) : std::string("-");
    }
    text += '\n';
    return text;
}

// ---- cbdump.csv: the bytes of the named constant buffers the lighting and post passes carry ------------------------------------------
// One row per (sample, named buffer) while a capture runs: the first min(bytes, kCbDumpHexMax) bytes in hex, with the buffer's pointer, so
// "one shared object per frame" becomes a measurement instead of an assumption. The list is fixed and small; each name is read at most
// once a second and only while the capture collects, because each read is one staging copy and one map stall.
inline constexpr const char* kCbDumpFile = "cbdump.csv";
inline constexpr uint32_t kCbDumpHexMax = 16384;
inline constexpr uint64_t kCbDumpBytes = 4ull << 20;
inline constexpr const char* kCbDumpNames[] {
    "g_WetnessParameter",          // 32 B, the engine's own wetness value (the ground's gloss comes from it)
    "g_AmbientParamArray",         // the ambient probe array, bound at 16,384 B
    "g_DirectionalShadowParameter",
    "g_ReflectionParameter",
    "g_BGAmbientParameter",
    "g_CameraParameter",           // 0x160 B the shaders read: the view's rows and the projection an offline march needs
    "cBrightPassParam",
    "cMergeWeight",
    "cGlareParam",
    "cToneMapParam",
    "cCommonTexParam",
    "cFilterForDark",
    "cDarkFilterParam",
    "cFogParam",
    "cHaloParam",       // 64 B as bound: the ring (its own colour, radius, band widths and the sun's screen UV)
    "cSunParam",        // 128 B as bound: the sun sprite's falloffs, colour and the sun's screen UV
    "cRadialBlurParam", // 16 B: one per radial-blur pass - the zoom centre, the decay and the step the ghosts march by
    "g_ShadowBiasParameter", // 16 B: the engine shadow block's normal-offset bias (the offset's width and the dot capacity that
                             // scales it), bound beside the atlas in the same draws; the god-rays march needs its own bias
};
inline constexpr size_t kCbDumpNameCount = sizeof(kCbDumpNames) / sizeof(kCbDumpNames[0]);

// The index of `name` in the dump list, or -1. Compared by the variable's name: a constant buffer keeps its plain name, and the base-name
// comparison costs nothing and still finds it if a future compiler adds a suffix.
inline int32_t CbDumpNameIndex(std::string_view name)
{
    const std::string_view base = ResourceBaseName(name);
    for (size_t i = 0; i < kCbDumpNameCount; ++i)
        if (base == kCbDumpNames[i])
            return static_cast<int32_t>(i);
    return -1;
}

// ---- one named buffer, several instances per second ------------------------------------------------------------------
// The cadence above answers once a second for a *name*. The directional shadow parameter breaks that: one frame binds several
// different 128-byte instances of the name (the shadow mask's assembly draws bind one per cascade, and the sun's own light draw
// binds one of them again - the session-4 rows hold two different pointers in one second). One row a second would carry one
// cascade's matrix; the cadence below is its own: each *pointer* answers once per window too, and the ring holds the distinct
// pointers a window saw (a bounded copy each, so a session with a reader storm pays for a handful of staging buffers).
inline constexpr size_t kCbDumpPointerRing = 8;

struct CbDumpPointerCadence
{
    const void* who[kCbDumpPointerRing] {}; // the last pointer each slot served (null: empty)
    int64_t at[kCbDumpPointerRing] {};      // the time of that read (0: this slot has read nothing yet)
};

// How many pointers of `baseName` one window samples: the shadow parameter's per-cascade instances, one a second for every
// other buffer. The rule is pure, so a test pins the two names' answers.
inline constexpr uint32_t CbDumpPointersPerSecond(std::string_view baseName) noexcept
{
    return baseName == "g_DirectionalShadowParameter" ? 4u : 1u;
}

// The slot that belongs to `pointer`, or -1 when the ring holds none of it.
inline int32_t CbDumpPointerIndex(const CbDumpPointerCadence& cadence, const void* pointer) noexcept
{
    if (pointer == nullptr)
        return -1;
    for (size_t i = 0; i < kCbDumpPointerRing; ++i)
        if (cadence.who[i] == pointer)
            return static_cast<int32_t>(i);
    return -1;
}

// Admits the pointer's row this tick window: the ring knows it and its window has passed, or it was never read. `windowTicks`
// is the window in the caller's tick units (the runtime converts its seconds; this rule stays clock-free). Full windows (more
// distinct pointers per window than the ring holds) evict the oldest slot when the content is recorded, so a name with more
// instances than slots reads a rotation of them, which is what a bounded rule promises and what a session below the ring's own
// size never sees.
inline bool CbDumpPointerDue(const CbDumpPointerCadence& cadence, const void* pointer, int64_t now, int64_t windowTicks) noexcept
{
    const int32_t index = CbDumpPointerIndex(cadence, pointer);
    if (index < 0)
        return true; // a pointer the ring has never read: the first read of it is always due
    return cadence.at[size_t(index)] == 0 || now - cadence.at[size_t(index)] >= windowTicks;
}

// Records the pointer's read: its own slot when the ring has it, else the first empty slot, else the oldest read's. Returns
// the slot the record went to, for the caller's log. Empty slots are the ones whose `at` is 0, so a record that lands in a
// fresh ring fills the low slots in order.
inline size_t CbDumpPointerRecord(CbDumpPointerCadence& cadence, const void* pointer, int64_t now) noexcept
{
    if (pointer == nullptr)
        return 0; // an unknown buffer: nothing to remember, the slot is not looked at again
    const int32_t index = CbDumpPointerIndex(cadence, pointer);
    size_t slot = index >= 0 ? size_t(index) : kCbDumpPointerRing - 1;
    if (index < 0)
    {
        size_t oldest = size_t(-1);
        for (size_t i = 0; i < kCbDumpPointerRing; ++i)
        {
            if (cadence.who[i] == nullptr)
            {
                slot = i;
                break;
            }
            if (oldest == size_t(-1) || cadence.at[i] < cadence.at[oldest])
                oldest = i;
        }
        if (index < 0 && cadence.who[slot] != nullptr)
            slot = oldest == size_t(-1) ? slot : oldest;
    }
    cadence.who[slot] = pointer;
    cadence.at[slot] = now;
    return slot;
}

// Lowercase hex of a byte range (CRLF-terminated comment lines stay out of it: the column is one field of a CSV row).
inline std::string HexBytes(const unsigned char* bytes, size_t count)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    if (bytes == nullptr)
        return text;
    text.reserve(count * 2);
    for (size_t i = 0; i < count; ++i)
    {
        text += kDigits[bytes[i] >> 4];
        text += kDigits[bytes[i] & 0x0F];
    }
    return text;
}

struct CbDumpRow
{
    uint64_t frame = 0;
    double seconds = 0.0;
    uint32_t psCrc = 0;
    bool hasCrc = false;
    std::string name;
    uint32_t slot = 0, bytes = 0;
    const void* pointer = nullptr;
    std::vector<unsigned char> data; // the first min(bytes, kCbDumpHexMax) bytes that were read
};

inline std::string CbDumpHeaderText() { return "frame,time_s,ps_crc,name,slot,bytes,ptr,hex\n"; }

inline std::string CbDumpRowText(const CbDumpRow& row)
{
    char head[192];
    snprintf(head, sizeof head, "%llu,%.3f,%s,%s,%u,%u,%s,", static_cast<unsigned long long>(row.frame), row.seconds,
             row.hasCrc ? detail::Hex32(row.psCrc).c_str() : "-", row.name.c_str(), row.slot, row.bytes,
             PointerText(row.pointer).c_str());
    return std::string(head) + HexBytes(row.data.data(), row.data.size()) + '\n';
}

// The cap rule of cbdump.csv: a row that would pass the byte cap is not written, and the file then ends with the truncated line.
inline bool CbDumpFits(size_t used, size_t row) { return used + row <= kCbDumpBytes; }

// ---- the engine pass overrides' counters (engine.txt) ---------------------------------------------------------------------
// One line per field, so a session's engine pass overrides can be read from the capture without a log: the switch, the status the module's card
// shows, one count per decline reason and the facts of the last patched draw, then the same counts for this capture's own window (the
// difference from a baseline taken when the capture armed), so a second capture of one session answers for its own frames. "module: off" is
// the whole file when the key is off.
inline constexpr const char* kEngineFile = "engine.txt";

// One capture's share of a session counter: the count since the baseline the capture took when it armed. A counter that went backwards (the
// module's state was rebuilt) counts from zero again, so a window never reports a wrapped number.
inline constexpr uint32_t WindowCounter(uint32_t current, uint32_t baseline) noexcept
{
    return current >= baseline ? current - baseline : current;
}

struct EngineStatus
{
    bool keyOn = false;      // the [FFXIV] EnginePassOverrides key is on
    bool armed = false;      // a draw can be patched now
    bool identity = false;   // the knob is at 1.0: today's picture
    uint32_t runs = 0;       // composite draws patched
    uint32_t declined = 0;   // composite draws left alone
    std::vector<std::pair<std::string, uint32_t>> declines; // one entry per reason with a nonzero count
    uint32_t indexed = 0;    // bytecodes the shader index stores
    uint32_t indexRefused = 0; // new bytecodes the index's cap refused (their draws are left alone)
    // The sun the light pass last reported, projected into its own window by the diagnostic: a capture's answer to whether an F9 could have
    // the engine's flare (the halo ring and the ghost discs) in frame. Computed from the same light draw's g_LightParam and camera, so it does
    // not depend on any card being on. `sunOutside` is in window widths/heights, 0 inside it.
    bool haveSun = false;
    bool sunBehind = false;
    float sunX = 0.0f, sunY = 0.0f, sunOutside = 0.0f;
    bool haveWindow = false; // the window lines below are real: a baseline was taken when this capture armed
    uint32_t windowRuns = 0;     // composite draws patched since the baseline
    uint32_t windowDeclined = 0; // composite draws left alone since the baseline
    std::vector<std::pair<std::string, uint32_t>> windowDeclines; // the same, per reason
    bool haveRun = false;    // the last-run facts below are real
    uint32_t boundBytes = 0, targetWidth = 0, targetHeight = 0;
    float window[4] { 0.0f, 0.0f, 0.0f, 0.0f };
};

inline std::string EngineStatusHeader()
{
    return "# FFXIV draw diagnostics, engine pass overrides, format " + std::to_string(kFormatVersion) + "\n";
}

inline std::string EngineStatusText(const EngineStatus& status)
{
    std::string text = EngineStatusHeader();
    text += std::string("module: ") + (status.keyOn ? "on" : "off") + "\n";
    // The sun's own line comes before the key's early return: whether the frame could show the engine's flare is a fact about the game's frame,
    // not about this card, and it stays in the file whether or not the light shafts card (or this one) is on.
    if (!status.haveSun)
        text += "sun: -\n";
    else if (status.sunBehind)
        text += "sun: behind the camera\n";
    else
        text += "sun: " + detail::Number(status.sunX) + "x" + detail::Number(status.sunY) +
                (status.sunOutside > 0.0f ? ", " + detail::Number(status.sunOutside) + " windows outside" : std::string(", in the window")) +
                "\n";
    if (!status.keyOn)
        return text; // the module is off: there is nothing else it could report
    text += std::string("armed: ") + (status.armed ? "yes" : "no") + "\n";
    text += std::string("identity: ") + (status.identity ? "yes" : "no") + "\n";
    text += "runs: " + std::to_string(status.runs) + "\n";
    text += "declined: " + std::to_string(status.declined) + "\n";
    for (const auto& [reason, count] : status.declines)
        if (count != 0)
            text += "decline: " + reason + " = " + std::to_string(count) + "\n";
    text += "index: " + std::to_string(status.indexed) + " bytecodes, " + std::to_string(status.indexRefused) + " refused\n";
    if (status.haveWindow)
    {
        text += "window runs: " + std::to_string(status.windowRuns) + "\n";
        text += "window declined: " + std::to_string(status.windowDeclined) + "\n";
        for (const auto& [reason, count] : status.windowDeclines)
            if (count != 0)
                text += "window decline: " + reason + " = " + std::to_string(count) + "\n";
    }
    if (!status.haveRun)
        return text + "last bound bytes: -\nlast target: -\nlast window: -\n";
    text += "last bound bytes: " + std::to_string(status.boundBytes) + "\n";
    text += "last target: " + std::to_string(status.targetWidth) + "x" + std::to_string(status.targetHeight) + "\n";
    text += "last window: " + detail::Number(status.window[0]) + "," + detail::Number(status.window[1]) + "," +
            detail::Number(status.window[2]) + "x" + detail::Number(status.window[3]) + "\n";
    return text;
}

// ---- the screen-space reflections' counters (the second block of engine.txt) --------------------------------------------
// The reflections answer for draws the engine pass overrides never patch, so their counters are a block of their own after that one. The shape is
// the same - one line per field, then one line per reason that happened, then this capture's own window - so a session reads from a capture,
// and not from a log, which of the module's gates stopped the water draws: the card off, an unarmed module, a frame it ran, the readers it
// served, or the reasons it passed draws on for before its first decision.
struct SsrStatus
{
    bool keyOn = false;      // the [FFXIV] ScreenSpaceReflections card is on
    bool armed = false;      // a water draw can run now: the key, a blend off zero and no switch-off
    uint32_t runs = 0;       // passes run this session
    uint32_t readers = 0;    // water draws that bound this module's texture this session
    uint32_t declined = 0;   // water draws left alone this session, and why
    std::vector<std::pair<std::string, uint32_t>> declines;  // one entry per decline reason with a nonzero count
    std::vector<std::pair<std::string, uint32_t>> passedOn;  // one entry per passed-on reason with a nonzero count
    // The stamp and the view of the last water draw the gate passed on, in the module's own wording; empty until one was read.
    std::string lastWaterDraw;
    bool haveWindow = false; // the window lines below are real: a baseline was taken when this capture armed
    uint32_t windowRuns = 0;
    uint32_t windowReaders = 0;
    uint32_t windowDeclined = 0;
    std::vector<std::pair<std::string, uint32_t>> windowDeclines;
    std::vector<std::pair<std::string, uint32_t>> windowPassedOn;
};

inline std::string SsrStatusText(const SsrStatus& status)
{
    std::string text;
    text += std::string("ssr module: ") + (status.keyOn ? "on" : "off") + "\n";
    if (!status.keyOn)
        return text; // the card is off: the file's own answer is that the module was never in play
    text += std::string("ssr armed: ") + (status.armed ? "yes" : "no") + "\n";
    text += "ssr runs: " + std::to_string(status.runs) + "\n";
    text += "ssr readers: " + std::to_string(status.readers) + "\n";
    text += "ssr declined: " + std::to_string(status.declined) + "\n";
    for (const auto& [reason, count] : status.declines)
        if (count != 0)
            text += "ssr decline: " + reason + " = " + std::to_string(count) + "\n";
    for (const auto& [reason, count] : status.passedOn)
        if (count != 0)
            text += "ssr passed on: " + reason + " = " + std::to_string(count) + "\n";
    // The slot and the view of the last water draw the gate passed on: the capture's own answer to "which slot did the module's stamp name",
    // which no earlier capture could say. A session compares the map slot with the slot its rows print the planar map at. The line is this
    // capture's: the module keeps one read for the session, so a window that passed no water draw on leaves it at `-`.
    text += "ssr last water draw: " + (status.lastWaterDraw.empty() ? std::string("-") : status.lastWaterDraw) + "\n";
    if (status.haveWindow)
    {
        text += "ssr window runs: " + std::to_string(status.windowRuns) + "\n";
        text += "ssr window readers: " + std::to_string(status.windowReaders) + "\n";
        text += "ssr window declined: " + std::to_string(status.windowDeclined) + "\n";
        for (const auto& [reason, count] : status.windowDeclines)
            if (count != 0)
                text += "ssr window decline: " + reason + " = " + std::to_string(count) + "\n";
        for (const auto& [reason, count] : status.windowPassedOn)
            if (count != 0)
                text += "ssr window passed on: " + reason + " = " + std::to_string(count) + "\n";
    }
    return text;
}

// ---- the one-frame texture dump ([FFXIV] DrawDiagnosticsDump = frame, ini only, off by default) -------------------------------------
// One frame's targets, read back after they were drawn: the wetness pass's target, the five G-buffer targets, the two light buffers, the
// hierarchical-Z (screen-space reflection) draw's target, the scene the tone map reads, the view-position buffer the water and the composite
// draws sample, the tone map's target and the unidentified FP16 copy that follows the tone map. The rules below are the pure part; the D3D side
// is in the runtime.
inline constexpr const char* kDumpFolder = "dump";
inline constexpr const char* kDumpFactsFile = "facts.txt";
inline constexpr size_t kMaxDumpTextures = 20;
// The view-position buffer's variable name as each pass spells it. The composite and the wetness pass declare `g_SamplerViewPosition`; the water
// shaders declare `g_SamplerVPosition` and bind the same resource (a capture of the water draws at t0 and the composite at t10 read one pointer).
// The dump identifies the buffer by either name, so it does not depend on which of the two passes a capture happened to name.
inline constexpr const char* kViewPositionNames[] { "g_SamplerViewPosition", "g_SamplerVPosition" };
inline constexpr size_t kViewPositionNameCount = sizeof(kViewPositionNames) / sizeof(kViewPositionNames[0]);
inline constexpr bool IsViewPositionName(std::string_view name) noexcept
{
    for (size_t i = 0; i < kViewPositionNameCount; ++i)
        if (name == kViewPositionNames[i])
            return true;
    return false;
}
// The dump's byte budget ([FFXIV] DrawDiagnosticsDumpBudgetMiB): 64 to 4096 MiB, and 256 MiB is today's bound. At 4K the whole set (the wetness
// target, the five G-buffer targets, the two light buffers, the hierarchical-Z target, the scene, the view position, the tone map and the copy) is
// about 514 MiB - the same set without the view position is 451 MiB and a 4K view position is 63 MiB - and about 546 MiB when a 33 MiB SSR target
// is written too, so the bound is a key instead of a constant and a session that wants the whole set of a 4K frame sets 1024 MiB. The shadow atlas
// (g_SamplerShadow) adds its plane on top - 32 MiB at 2048x8192 R16 - and is the first item a contended bound displaces (DumpRank::Atlas). The
// water gate's four files ride below it: the transparency-light pair as the water draws bind it (two R11G11B10 targets, about 33 MiB each at 4K),
// the planar reflection map and the composite's occlusion (about 8 MiB each at 1080p) - a 2048 MiB bound carries the whole set with them.
inline constexpr int32_t kMinDumpBudgetMiB = 64;
inline constexpr int32_t kMaxDumpBudgetMiB = 4096;
inline constexpr int32_t kDefaultDumpBudgetMiB = 256;
constexpr uint64_t DumpBudgetBytes(int32_t mib) noexcept
{
    const int32_t clamped = mib < kMinDumpBudgetMiB ? kMinDumpBudgetMiB : (mib > kMaxDumpBudgetMiB ? kMaxDumpBudgetMiB : mib);
    return uint64_t(clamped) << 20;
}
inline constexpr uint64_t kMaxDumpBytes = DumpBudgetBytes(kDefaultDumpBudgetMiB); // the default budget: 256 MiB, today's bound

// What the dump takes, in the order it takes it when the byte bound is contended.
enum class DumpRank : uint8_t
{
    Wetness = 0,       // the pass that writes the ground's wetness value
    Gbuffer = 1,       // the five targets of the frame's G-buffer fill
    Light = 2,         // the two light buffers as the light draws sample them
    Ssr = 3,           // the target of the hierarchical-Z (screen-space reflection) draw
    Scene = 4,         // t0 of the tone map
    ViewPosition = 5,  // the view-position buffer (g_SamplerViewPosition / g_SamplerVPosition): every pixel's view-space position, the
                       // reflections pass's ray origin
    Rest = 6,          // the tone-map target and the post-tone-map copy
    Atlas = 7,         // the cascade shadow atlas as its readers bind it (g_SamplerShadow): the whole plane, one subresource copy of
                       // about 32 MiB at 2048x8192 - the god-rays march's shadow source, so a session can read the texels its own
                       // compare sees. Lowest priority: a tight byte bound displaces it first, before any lighting target
    WaterLights = 8,   // the transparency-light pair the frame's water draws sample under the light names (a different resource pair than
                       // the composite's, PS-FRESH-PUDDLE-OCEAN.md): the engine's own shading of the water, and the top candidate the
                       // puddles' definitive water gate can be built from. Below the atlas: a tight bound displaces it first
    WaterGate = 9,     // the planar reflection map at the water draws and the composite's own g_SamplerOcclusion: the other two
                       // candidates the water gate can be read from (the map's alpha traffic, the occlusion's channels)
};

// The key: "frame" turns the one-frame dump on, anything else leaves it off, which is today's behaviour and what every other value means.
inline constexpr bool DumpEnabled(std::string_view value) noexcept { return value == "frame"; }

// The light pass that writes the frame's light buffers: exactly two render targets, both R11G11B10_FLOAT and both of one size, written without
// blending. It is the draw whose g_LightParam carries the directional light's own parameters, and it is the one the light shafts pass copies
// that buffer from; the other draws that declare the same name are another pass - the wetness volumes write one R8 target with blending on, and
// every local light accumulates into the same pair with blending on. A constant-buffer sample taken at whichever draw bound the name first
// therefore measured a different light, which is why the light's direction was read with the wrong space for a whole session.
inline constexpr uint32_t kLightBufferFormat = 26;    // DXGI_FORMAT_R11G11B10_FLOAT
inline constexpr uint32_t kLightBufferTargets = 2;    // the diffuse and the specular light buffer

// `format` is the first target's, `sameSizeAndFormat` says every target is that format and that size.
inline constexpr bool IsLightPassShape(uint32_t targets, uint32_t format, bool sameSizeAndFormat, bool blended) noexcept
{
    return targets == kLightBufferTargets && sameSizeAndFormat && format == kLightBufferFormat && !blended;
}

// The wetness pass's shape: its pixel shader declares the wetness buffer and the two textures the ground pass reads, and it writes exactly
// one target. Identified by those names and not by a bytecode CRC: a CRC is a fact of one build, the names are what the game ships.
inline constexpr bool IsWetnessPassShape(bool wetnessParam, bool gbuffer3, bool viewPosition, uint32_t rtvCount) noexcept
{
    return wetnessParam && gbuffer3 && viewPosition && rtvCount == 1;
}

// The G-buffer fill of a frame: one draw with five render targets.
inline constexpr bool IsGbufferShape(uint32_t rtvCount) noexcept { return rtvCount == 5; }

// A texture of the tone map's size class, which is what the post-tone-map copy is identified by (the "2560-class" is measured, not
// hard-coded: the class is "the same size as the tone map's t0").
inline constexpr bool SameTextureSize(uint32_t width, uint32_t height, uint32_t otherWidth, uint32_t otherHeight) noexcept
{
    return width != 0 && height != 0 && width == otherWidth && height == otherHeight;
}

// The FP16 formats a copy of the scene texture can use (DXGI_FORMAT values: R16G16B16A16_FLOAT 10, R16G16_FLOAT 34, R16_FLOAT 54).
inline constexpr bool IsFp16Format(uint32_t format) noexcept { return format == 10 || format == 34 || format == 54; }

// The bound of the dump: at most kMaxDumpTextures files and `bound` bytes of pixels (the configured budget, 256 MiB by default). The files
// are written in DumpRank order, so a texture that would pass the bound is skipped with a line in facts.txt instead of taking the place of one
// that matters more.
inline constexpr bool DumpFits(uint32_t written, uint64_t bytes, uint64_t addBytes, uint64_t bound = kMaxDumpBytes) noexcept
{
    return written < kMaxDumpTextures && bytes + addBytes <= bound;
}

// One texture the dump holds against that bound, as the release rule sees it: its priority and the packed bytes it keeps.
struct DumpHold
{
    DumpRank rank = DumpRank::Rest;
    uint64_t bytes = 0;
    bool released = false; // set by DumpMakeRoom: this texture gives its place away
};

// Whether a held texture may give its place to a newcomer: only one of a strictly lower priority (a larger DumpRank number). A texture of
// the same rank keeps its first-come place, so the order of the twenty places never depends on which identification came last.
inline constexpr bool DumpYieldsTo(DumpRank held, DumpRank newcomer) noexcept
{
    return uint8_t(held) > uint8_t(newcomer);
}

// The byte bound with priority: a texture that does not fit on its own releases the held textures of a lower priority, lowest first, until
// it does, so the wetness target identified after the G-buffer and the light buffers still has a place in a 4K frame. Returns the bytes the
// set freed and marks those holds `released`; each becomes one facts.txt line naming the texture that took its place. Nothing is released
// while only equal or higher priorities are held, and the caller then refuses the newcomer.
inline uint64_t DumpMakeRoom(DumpHold* held, size_t count, DumpRank newcomer, uint64_t newcomerBytes, uint64_t heldBytes,
                             uint64_t bound = kMaxDumpBytes)
{
    // A texture larger than the whole bound can never fit, so nothing is released for it and the caller refuses it.
    if (newcomerBytes > bound)
        return 0;
    uint64_t freed = 0; // always a part of heldBytes, so the subtraction below stays above zero
    while (heldBytes - freed + newcomerBytes > bound)
    {
        size_t pick = count;
        for (size_t i = 0; i < count; ++i)
            if (!held[i].released && held[i].bytes != 0 && DumpYieldsTo(held[i].rank, newcomer) &&
                (pick == count || uint8_t(held[i].rank) > uint8_t(held[pick].rank)))
                pick = i;
        if (pick == count)
            break;
        held[pick].released = true;
        freed += held[pick].bytes;
    }
    return freed;
}

// The one-frame dump reads one whole window: nothing is identified while the splitter still waits for the tone map that opens the first
// window (the draws before it belong to a frame whose beginning was never seen). The test is the splitter's `fresh`, not its `toneSeen`:
// a bridge Present clears the tone state on every frame, so a guard on it would identify nothing in a bridged session.
inline constexpr bool DumpIdentifies(bool splitterFresh) noexcept { return !splitterFresh; }

// Whether one identification is added at all: the dump has to be armed and unwritten, the resource has to exist, the name must not be held
// yet, and the twenty written places must not be full. A name is held at most once per capture, whether its line said the file was written
// or why it was skipped, so the wetness pass, the light draws and the five-target draw repeating inside one frame add one line each.
inline constexpr bool DumpAdds(bool active, bool written, const void* resource, bool held, size_t writtenCount) noexcept
{
    return active && !written && resource != nullptr && !held && writtenCount < kMaxDumpTextures;
}

// The draws of one capture are numbered by the capture's own counter, which neither a frame close nor the partial-frame drop restarts. The
// row ordinal starts at 1 at every frame, so it cannot express "a later draw": a target identified at a window's last draw would be copied
// in the middle of the next window, or never.
inline constexpr bool DumpCopyDue(bool copied, bool noted, uint64_t identifiedSequence, uint64_t nowSequence) noexcept
{
    return !copied && !noted && identifiedSequence < nowSequence;
}

// The post-tone-map copy is identified by a draw after the tone map's own, in the same per-capture order.
inline constexpr bool DumpAfterTone(uint64_t toneSequence, uint64_t nowSequence) noexcept { return nowSequence > toneSequence; }

// The G-buffer targets are the one rank that is not complete at the draw that identifies it: the five-target draw is one draw inside the
// geometry pass, so the copy waits for a later draw that samples the target. Every other resource is complete at its identifying draw and
// is copied at the next one.
inline constexpr bool DumpWaitsForSampling(DumpRank rank) noexcept { return rank == DumpRank::Gbuffer; }

// The sampling match of a G-buffer target: the index of the slot that binds `target`, -1 when none does. The comparison is by the
// resource behind the view, so a second view of the same texture counts, and a null slot matches nothing.
inline int32_t DumpSampledSlot(const void* const* resources, size_t count, const void* target) noexcept
{
    if (target == nullptr)
        return -1;
    for (size_t i = 0; i < count; ++i)
        if (resources[i] != nullptr && resources[i] == target)
            return static_cast<int32_t>(i);
    return -1;
}

// The staging copies are bounded when a texture is identified, not only when the files are written: the copies are taken over the frame
// after the one they belong to, and twenty textures of a 4K frame would be about a gigabyte of staging memory.
inline constexpr bool DumpIdentFits(uint64_t heldBytes, uint64_t addBytes, uint64_t bound = kMaxDumpBytes) noexcept
{
    return heldBytes + addBytes <= bound;
}

// Whether the frame that identified the dump's tone map is over, which is when the dump can be written: either a later frame was closed, or
// the tone-map frame itself was closed at a bridge Present. That Present runs after the frame's last draw, so the frame is whole there even
// though no later one has ended; a two-frame capture in a session with a bridge ends on exactly that Present and would otherwise lose the
// dump it just collected. Both writers of the dump ask this one rule.
inline constexpr bool DumpFrameOver(uint64_t framesClosed, uint32_t toneFrame, bool toneFrameAtPresent) noexcept
{
    return toneFrame != 0 && (framesClosed > toneFrame || (framesClosed == toneFrame && toneFrameAtPresent));
}

// What the end of a capture does with one copy that is still pending.
enum class DumpEnd : uint8_t
{
    Skip = 0,  // a G-buffer target no later draw sampled: a copy now would hold a later frame's content under this frame's name
    Copy = 1,  // the frame is over for this texture and the copy is safe
    Leave = 2, // identified at the draw that is finishing the capture: the game has not written it yet
};

// The end of a capture writes the dump too, with the game's context. A G-buffer target is the one rank that is not complete at the end of
// its frame, so it is skipped (facts.txt says `not sampled`) instead of copied after the frame it belongs to. Every other resource may be
// copied once the draw that identified it has run - the bridge Present has already run it - while the tone-map split writes the dump from
// the closing tone map's own draw hook, before that draw, so there the identifying draw is still left out.
inline constexpr DumpEnd DumpAtEnd(DumpRank rank, uint64_t identifiedSequence, uint64_t nowSequence, bool toneFrameAtPresent) noexcept
{
    if (DumpWaitsForSampling(rank))
        return DumpEnd::Skip;
    if (toneFrameAtPresent || identifiedSequence < nowSequence)
        return DumpEnd::Copy;
    return DumpEnd::Leave;
}

struct DumpFact
{
    std::string name; // the file is <name>.bin
    std::string format;
    uint32_t width = 0, height = 0, rowPitch = 0, ordinal = 0; // the draw that identified the resource
    uint64_t bytes = 0;                                       // the packed bytes written
    const void* pointer = nullptr;
};

inline std::string DumpFactsHeader()
{
    return "# FFXIV draw diagnostics, one frame dump, format " + std::to_string(kFormatVersion) +
           "\n# wetness.bin: the wetness pass target; gbuffer0.bin..gbuffer4.bin: the five-target draw; light_diffuse.bin and\n"
           "# light_specular.bin: the light buffers as the light draws sample them; ssr.bin: the target of the hierarchical-Z draw;\n"
           "# scene.bin: t0 of the tone map; vposition.bin: the view-position buffer the water (g_SamplerVPosition) and the composite\n"
           "# and wetness pass (g_SamplerViewPosition) sample; tonemap.bin: its target;\n"
           "# copyps.bin: the post-tone-map FP16 copy; atlas.bin: the cascade shadow atlas, copied from the game's own SRV\n"
           "# file,format,size,bytes,row_pitch,draw_ordinal,pointer\n";
}

// One line of facts.txt for a file that was written: the packed size, the row pitch the readback had (the file itself is packed), the
// draw that identified the resource and its pointer.
inline std::string DumpFactText(const DumpFact& fact)
{
    return fact.name + ".bin," + fact.format + "," + std::to_string(fact.width) + "x" + std::to_string(fact.height) + "," +
           std::to_string(fact.bytes) + "," + std::to_string(fact.rowPitch) + "," + std::to_string(fact.ordinal) + "," +
           PointerText(fact.pointer) + "\n";
}

// A texture the dump could not take, with the reason (multisampled, a format it cannot write, the bound, a failed copy or map).
inline std::string DumpSkipText(std::string_view name, std::string_view reason)
{
    return "# skipped " + std::string(name) + ".bin: " + std::string(reason) + "\n";
}

// Of the folder names next to a new run (the new run included), the oldest beyond `keep`. Names that are not run ids are never
// listed: the diagnostic only ever deletes what it created.
inline std::string FormatRunId(const std::tm& t)
{
    char text[32];
    snprintf(text, sizeof text, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return text;
}

// The folder of one capture: the run id of the moment, and from the second capture of a session on the capture's index, because run ids
// have one-second resolution and two captures of one session can land in the same second.
inline std::string CaptureFolderName(const std::tm& t, uint32_t capture)
{
    std::string id = FormatRunId(t);
    if (capture > 1)
        id += "-" + std::to_string(capture);
    return id;
}

inline bool IsRunId(std::string_view s)
{
    if (s.size() < 15 || s[8] != '-')
        return false;
    for (size_t i = 0; i < 15; ++i)
        if (i != 8 && (s[i] < '0' || s[i] > '9'))
            return false;
    if (s.size() == 15)
        return true;
    if (s.size() < 17 || s.size() > 18 || s[15] != '-') // "-<capture index>"
        return false;
    for (size_t i = 16; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

inline std::vector<std::string> RunsToPrune(std::vector<std::string> names, size_t keep = kKeepRuns)
{
    names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& n) { return !IsRunId(n); }), names.end());
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    if (names.size() <= keep)
        return {};
    return std::vector<std::string>(names.begin(), names.end() - static_cast<std::ptrdiff_t>(keep));
}
} // namespace ext::ffxiv::diag
