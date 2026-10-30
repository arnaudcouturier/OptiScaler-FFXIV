#pragma once
// Developer draw diagnostic, pure text side: the CSV formats, the frame marker rule, the caps and the run folder naming.
// No D3D, no file, no clock, so a unit test checks the format and the marker ordering without a device, and the reader of a
// file cannot drift from its writer.
//
// Files under <folder of the DLL>\OptiScaler\draw-diag\<run>\ :
//   draws.csv  one row per draw of each captured frame, then one `#` comment line per frame with the marker ordinals (byte cap)
//   cb.csv     one row per constant-buffer read (once a second while a capture runs)
// Nothing in them is a path, a user name or a machine name: frames, slots, CRC32s, reflected shader names, formats and sizes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace ext::ffxiv::diag
{
inline constexpr int kFormatVersion = 1;
inline constexpr const char* kRootFolder = "draw-diag";
inline constexpr const char* kDrawsFile = "draws.csv";
inline constexpr const char* kCbFile = "cb.csv";
inline constexpr size_t kKeepRuns = 5; // the newest runs kept next to each other, like the other developer probes

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

struct ConstantRead
{
    uint64_t frame = 0;
    double seconds = 0.0; // since the capture started
    uint32_t psCrc = 0;
    bool hasCrc = false;
    std::string name; // the reflected constant-buffer name
    uint32_t slot = 0, bytes = 0;
    float position[3] {}, direction[3] {}, diffuse[3] {};
};

inline std::string CbHeaderText()
{
    return "frame,time_s,ps_crc,cb,slot,bytes,pos_x,pos_y,pos_z,dir_x,dir_y,dir_z,diffuse_r,diffuse_g,diffuse_b\n";
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
