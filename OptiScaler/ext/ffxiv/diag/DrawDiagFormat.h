#pragma once
// Developer draw diagnostic, pure text side: the CSV formats, the frame marker rule, the caps and the run folder naming.
// No D3D, no file, no clock, so a unit test checks the format and the marker ordering without a device, and the reader of a
// file cannot drift from its writer.
//
// Files under <folder of the DLL>\OptiScaler\draw-diag\<run>\ :
//   draws.csv  one row per draw of each captured frame, then one `#` comment line per frame with the marker ordinals (byte cap)
//   cb.csv     one row per constant-buffer read (once a second while the diagnostic is armed)
// Nothing in them is a path, a user name or a machine name: frames, slots, CRC32s, reflected shader names, formats and sizes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "ShaderBindings.h" // diag::ResourceBaseName: the reflected names carry the effect compiler's type suffix

namespace ext::ffxiv::diag
{
inline constexpr int kFormatVersion = 2;
inline constexpr const char* kRootFolder = "draw-diag";
inline constexpr const char* kDrawsFile = "draws.csv";
inline constexpr const char* kCbFile = "cb.csv";
inline constexpr const char* kShadersFile = "shaders.txt"; // the fixture list: crc, size and the reflected names of each kept shader
inline constexpr const char* kShaderFolder = "shaders";    // and their bytecode, one <crc>.dxbc per kept shader
inline constexpr size_t kKeepRuns = 5; // the newest runs kept next to each other, like the other developer probes

// The fixture dump: pixel shaders whose reflection names one of the textures or constant buffers the fork identifies draws by. Bounded, so a
// capture cannot fill the disk: the game has a few dozen such shaders and each blob is a few kilobytes.
inline constexpr size_t kMaxFixtureShaders = 64;
inline constexpr size_t kMaxCommonFixtureShaders = 16; // kept for a common name only (see IsKeyFixtureShader)
inline constexpr size_t kMaxLightFixtures = 16;        // the light draw's own shape (see IsLightDrawFixture), on top of kMaxFixtureShaders
inline constexpr size_t kMaxFixtureBytes = 4ull << 20;

// The names a shader has to declare one of to be worth keeping. They are the ones the draw diagnostic and the contact shadow pass look up, so
// a fixture set holds every shape either of them classifies by.
inline constexpr uint32_t kFixtureMask = 1u << 0;         // g_SamplerShadow
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
    if (base == "g_SamplerShadow")
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

// Bounds. A frame of this game is about two thousand draws and a capture is a handful of frames.
inline constexpr uint64_t kDrawsBytes = 8ull << 20;
inline constexpr uint64_t kCbBytes = 64ull << 10;
inline constexpr uint32_t kMaxRowsPerFrame = 6000;
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

// The capture's frame end, in the draw stream.
//
// A frame used to be closed only at the D3D11-on-D3D12 bridge's Present entry (the fork's only Present hook). A session that upscales
// on the game's own D3D11 device has no bridge swapchain at all - the benchmark log line "Failed to get ID3D12CommandQueue from
// pDevice, creating Dx11 swapchain!" is that case - so no Present ever arrived, no frame was ever closed, and a capture with both keys
// set wrote nothing. The rule below needs no Present: the game's tone map draw runs exactly once per frame, so the draws from just
// after one of them to just after the next are one frame's worth of work, whatever presents the picture.
struct FrameSplitter
{
    bool toneSeen = false; // the frame in progress already carries a tone map draw

    void Reset() { toneSeen = false; }

    // Whether this draw closes the frame in progress. Called AFTER the draw's own row was collected, so the tone map draw is the frame's
    // last row and its `tone` marker carries the last ordinal: a captured frame runs from just after one tone map to just after the next.
    // `drawsInFrame` = rows collected for the frame in progress; a shorter frame is a loading screen or a replay and is never split.
    bool NoteDraw(uint8_t stage, uint32_t drawsInFrame)
    {
        if (stage != kStageToneMap)
            return false;
        if (!toneSeen)
        {
            toneSeen = true;
            return false;
        }
        if (drawsInFrame < kMinDrawsPerFrame)
            return false;
        toneSeen = false;
        return true;
    }
};

// Row markers. A tone map and the frame's first UI draw are properties of a draw; the upscale boundary happens between two
// draws, so it is carried by the row of the draw that follows it.
inline constexpr uint8_t kMarkerTone = 1;
inline constexpr uint8_t kMarkerUpscale = 2;
inline constexpr uint8_t kMarkerFirstUi = 4;

inline constexpr const char* kTruncatedLine = "# TRUNCATED: byte cap of this file reached, later rows were dropped\n";

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

// One draw, as the hook collected it. The variable-length parts arrive already formatted (slot:name:format:size|...), so this
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
    std::string srvs;     // "0:g_SamplerDepth:0x2C:3840x2160|3:-:0x0A:1920x1080"
    std::string cbs;      // "0:g_CommonParameter:64|2:g_LightParam:400"  (bytes = the buffer's own size)
    std::string rtv;      // "0:0x0A:3840x2160|1:0x0B:3840x2160"
    std::string dsv;      // "0x2C:3840x2160" or "-"
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

// The camera constant buffer of the same draw, read beside the light: the view matrix at +0x00 (64 B) is what tells the sun
// direction's space apart. A direction in world space stays the same while the view matrix turns; a direction in view space turns
// with it. `kCameraParamMatrixBytes` is the whole first matrix, and the row-major 4x4 of the game's constant buffer is what the
// columns carry.
inline constexpr uint32_t kCameraParamViewAt = 0x00;
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
};

inline std::string CbHeaderText()
{
    std::string text = "frame,time_s,ps_crc,cb,slot,bytes,pos_x,pos_y,pos_z,dir_x,dir_y,dir_z,diffuse_r,diffuse_g,diffuse_b";
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            text += ",view_" + std::to_string(row) + std::to_string(column);
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
    text += '\n';
    return text;
}

// Of the folder names next to a new run (the new run included), the oldest beyond `keep`. Names that are not run ids are never
// listed: the diagnostic only ever deletes what it created.
inline std::string FormatRunId(const std::tm& t)
{
    char text[32];
    snprintf(text, sizeof text, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return text;
}

inline bool IsRunId(std::string_view s)
{
    if (s.size() != 15 || s[8] != '-')
        return false;
    for (size_t i = 0; i < s.size(); ++i)
        if (i != 8 && (s[i] < '0' || s[i] > '9'))
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
