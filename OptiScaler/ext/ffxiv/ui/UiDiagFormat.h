#pragma once
// S10 package A: file formats of the D1 diagnostics (docs/HUDLESS.md section 12) as pure text functions.
//
// No D3D, no pch, no clock: tests/ext/ffxiv/ui builds this standalone and round-trips shaders.csv and frames.txt.
// Everything the diagnostics write goes through the formatters here, so a reader (the analysis scripts, the tests) and the
// writer cannot drift apart.
//
// shaders.csv : header line, then one row per distinct pixel shader (key = bytecode CRC32; shaders created before the hook
//               have no bytecode, so one row per pointer: id "pre#N", crc/bytes unknown).
// frames.txt  : '#' comment lines, then records: a `F ...` line followed by indented `d`/`p` lines.
// All numbers are decimal unless they start with 0x. Format enum values are the numeric DXGI_FORMAT values.

#include "ext/ffxiv/ui/UiBoundaryRules.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace ext::ffxiv::ui::dfmt
{
inline constexpr int kFormatVersion = 1;
inline constexpr size_t kMaxRtSet = 8;       // distinct render-target shapes kept per shader
inline constexpr size_t kMaxListedDraws = 48; // tagged draws listed per frame record
inline constexpr size_t kMaxPresentDraws = 8; // draws inside the bridge Present listed per frame record
inline constexpr size_t kKeepRuns = 5;

// ---- names ---------------------------------------------------------------------------------------------------------

inline const char* DxgiFormatName(uint32_t f)
{
    switch (f)
    {
    case 0: return "UNKNOWN";
    case 2: return "R32G32B32A32_FLOAT";
    case 9: return "R16G16B16A16_TYPELESS";
    case 10: return "R16G16B16A16_FLOAT";
    case 11: return "R16G16B16A16_UNORM";
    case 23: return "R10G10B10A2_TYPELESS";
    case 24: return "R10G10B10A2_UNORM";
    case 26: return "R11G11B10_FLOAT";
    case 27: return "R8G8B8A8_TYPELESS";
    case 28: return "R8G8B8A8_UNORM";
    case 29: return "R8G8B8A8_UNORM_SRGB";
    case 34: return "R16G16_FLOAT";
    case 40: return "D32_FLOAT";
    case 41: return "R32_FLOAT";
    case 45: return "D24_UNORM_S8_UINT";
    case 61: return "R8_UNORM";
    case 87: return "B8G8R8A8_UNORM";
    case 88: return "B8G8R8X8_UNORM";
    case 90: return "B8G8R8A8_TYPELESS";
    case 91: return "B8G8R8A8_UNORM_SRGB";
    default: return nullptr;
    }
}

inline std::string FormatName(uint32_t f)
{
    if (const char* n = DxgiFormatName(f))
        return n;
    return "fmt" + std::to_string(f);
}

inline uint32_t FormatFromName(std::string_view s)
{
    for (uint32_t f = 0; f < 256; ++f)
        if (const char* n = DxgiFormatName(f); n && s == n)
            return f;
    if (s.substr(0, 3) == "fmt")
    {
        uint32_t v = 0;
        std::from_chars(s.data() + 3, s.data() + s.size(), v);
        return v;
    }
    return 0;
}

// Draw kind text: "-" untagged, S1..S5 scene/post stage, UI, ?? (shader created before the hook).
inline std::string StageName(uint8_t stage)
{
    if (stage == kStageNone)
        return "-";
    if (stage >= 1 && stage <= 5)
        return "S" + std::to_string(stage);
    if (stage == kUiStage)
        return "UI";
    return "??";
}

inline uint8_t StageFromName(std::string_view s)
{
    if (s == "-")
        return kStageNone;
    if (s == "UI")
        return kUiStage;
    if (s.size() == 2 && s[0] == 'S' && s[1] >= '1' && s[1] <= '5')
        return static_cast<uint8_t>(s[1] - '0');
    return kStageUnknown;
}

// ---- render target description -------------------------------------------------------------------------------------

enum class RtKind : uint8_t
{
    None,             // no render target bound (or not looked up)
    Backbuffer,       // the resource of RTV 0 is the swapchain backbuffer (pointer identity)
    SameAsBackbuffer, // another texture with the backbuffer's size and format
    Other,
};

inline const char* RtKindName(RtKind k)
{
    switch (k)
    {
    case RtKind::Backbuffer: return "BB";
    case RtKind::SameAsBackbuffer: return "BBLIKE";
    case RtKind::Other: return "OTHER";
    default: return "NONE";
    }
}

inline RtKind RtKindFromName(std::string_view s)
{
    if (s == "BB")
        return RtKind::Backbuffer;
    if (s == "BBLIKE")
        return RtKind::SameAsBackbuffer;
    if (s == "OTHER")
        return RtKind::Other;
    return RtKind::None;
}

struct RtShape
{
    uint32_t width = 0, height = 0, format = 0;

    bool operator==(const RtShape&) const = default;
};

inline std::string ShapeText(const RtShape& s) { return std::to_string(s.width) + "x" + std::to_string(s.height) + "/" + FormatName(s.format); }

inline bool ParseShape(std::string_view s, RtShape& out)
{
    const auto x = s.find('x');
    const auto slash = s.find('/');
    if (x == std::string_view::npos || slash == std::string_view::npos || slash < x)
        return false;
    uint32_t w = 0, h = 0;
    if (std::from_chars(s.data(), s.data() + x, w).ec != std::errc {} ||
        std::from_chars(s.data() + x + 1, s.data() + slash, h).ec != std::errc {})
        return false;
    out = { w, h, FormatFromName(s.substr(slash + 1)) };
    return true;
}

// ---- small text helpers --------------------------------------------------------------------------------------------

inline std::vector<std::string_view> Split(std::string_view s, char sep)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (true)
    {
        const size_t at = s.find(sep, start);
        if (at == std::string_view::npos)
        {
            parts.push_back(s.substr(start));
            return parts;
        }
        parts.push_back(s.substr(start, at - start));
        start = at + 1;
    }
}

template <class T> inline bool ParseNumber(std::string_view s, T& out, int base = 10)
{
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        s.remove_prefix(2);
        base = 16;
    }
    T v {};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (r.ec != std::errc {} || r.ptr != s.data() + s.size())
        return false;
    out = v;
    return true;
}

inline std::string Hex32(uint32_t v)
{
    char b[16];
    snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

// ---- shaders.csv ---------------------------------------------------------------------------------------------------

struct ShaderRow
{
    std::string id;            // "0xAABBCCDD" (CRC32 of the bytecode) or "pre#N"
    bool hasCrc = false;       // false: created before the hook, no bytecode seen
    uint32_t crc = 0;
    uint32_t bytes = 0;        // DXBC size
    uint8_t stage = kStageNone; // kStageNone, 1..5, kUiStage, kStageUnknown
    bool ui = false;           // member of the UI catalog (the UI tag)
    bool preHook = false;      // first seen at a draw without having passed through TagShader
    uint8_t tagAgree = 0;      // 0 not checked, 1 same stage as FfxivSceneHdr's own tag, 2 different
    uint32_t instances = 0;    // shader objects created with this bytecode
    uint64_t draws = 0;
    uint64_t firstFrame = 0;   // Present counter value of the first draw (0 = never drawn)
    std::vector<RtShape> rtSet;
    uint32_t rtOverflow = 0;   // distinct shapes beyond kMaxRtSet that were not kept
    uint64_t rtSamples = 0;    // draws whose render target was looked up
    uint64_t bbSamples = 0;    // of those, draws into the backbuffer

    bool operator==(const ShaderRow&) const = default;

    double BackbufferShare() const { return rtSamples ? 100.0 * double(bbSamples) / double(rtSamples) : 0.0; }
};

inline constexpr const char* kShadersCsvHeader =
    "id,crc32,dxbc_bytes,stage,ui,pre_hook,tag_agree,instances,draws,first_frame,rt_set,rt_overflow,rt_samples,bb_samples,bb_share_pct";

inline std::string ShaderCsvRow(const ShaderRow& r)
{
    std::string rt;
    for (size_t i = 0; i < r.rtSet.size(); ++i)
    {
        if (i)
            rt += '|';
        rt += ShapeText(r.rtSet[i]);
    }
    char share[32];
    snprintf(share, sizeof share, "%.1f", r.BackbufferShare());
    std::string line = r.id + ',' + (r.hasCrc ? Hex32(r.crc) : std::string("?")) + ',' +
                       (r.hasCrc ? std::to_string(r.bytes) : std::string("?")) + ',' +
                       (r.stage == kStageUnknown ? std::string("?") : std::to_string(r.stage)) + ',' +
                       (r.ui ? "1" : "0") + ',' + (r.preHook ? "1" : "0") + ',' + std::to_string(r.tagAgree) + ',' +
                       std::to_string(r.instances) + ',' + std::to_string(r.draws) + ',' + std::to_string(r.firstFrame) +
                       ',' + (rt.empty() ? std::string("-") : rt) + ',' + std::to_string(r.rtOverflow) + ',' +
                       std::to_string(r.rtSamples) + ',' + std::to_string(r.bbSamples) + ',' + share;
    return line;
}

inline bool ParseShaderCsvRow(std::string_view line, ShaderRow& out)
{
    const auto f = Split(line, ',');
    if (f.size() != 15)
        return false;
    ShaderRow r;
    r.id = std::string(f[0]);
    r.hasCrc = f[1] != "?";
    if (r.hasCrc && (!ParseNumber(f[1], r.crc) || !ParseNumber(f[2], r.bytes)))
        return false;
    if (f[3] == "?")
        r.stage = kStageUnknown;
    else
    {
        unsigned st = 0;
        if (!ParseNumber(f[3], st))
            return false;
        r.stage = static_cast<uint8_t>(st);
    }
    unsigned agree = 0;
    if (!ParseNumber(f[6], agree))
        return false;
    r.ui = f[4] == "1";
    r.preHook = f[5] == "1";
    r.tagAgree = static_cast<uint8_t>(agree);
    if (!ParseNumber(f[7], r.instances) || !ParseNumber(f[8], r.draws) || !ParseNumber(f[9], r.firstFrame) ||
        !ParseNumber(f[11], r.rtOverflow) || !ParseNumber(f[12], r.rtSamples) || !ParseNumber(f[13], r.bbSamples))
        return false;
    if (f[10] != "-")
        for (auto part : Split(f[10], '|'))
        {
            RtShape s;
            if (!ParseShape(part, s))
                return false;
            r.rtSet.push_back(s);
        }
    out = std::move(r);
    return true;
}

inline std::string ShadersCsv(const std::vector<ShaderRow>& rows)
{
    std::string out = std::string("# S10 D1 shaders.csv, format ") + std::to_string(kFormatVersion) +
                      ". stage: 0 none, 1..5 scene/post, 6 UI only, ? unknown (created before the hook).\n"
                      "# tag_agree: 0 not checked, 1 our stage equals FfxivSceneHdr's own tag, 2 different. rt_set: render-target shapes\n"
                      "# drawn into (sampled draws only, see rt_samples). bb_share_pct = bb_samples / rt_samples.\n";
    out += kShadersCsvHeader;
    out += '\n';
    for (const auto& r : rows)
    {
        out += ShaderCsvRow(r);
        out += '\n';
    }
    return out;
}

inline std::vector<ShaderRow> ParseShadersCsv(std::string_view text)
{
    std::vector<ShaderRow> rows;
    for (auto line : Split(text, '\n'))
    {
        if (line.empty() || line[0] == '#' || line == kShadersCsvHeader)
            continue;
        ShaderRow r;
        if (ParseShaderCsvRow(line, r))
            rows.push_back(std::move(r));
    }
    return rows;
}

// ---- frames.txt ----------------------------------------------------------------------------------------------------

struct DrawLine
{
    uint32_t ordinal = 0;
    uint8_t stage = kStageNone;
    RtKind rt = RtKind::None;
    RtShape shape {};
    bool r1 = false, r2 = false, r2any = false, r3 = false; // this draw is the final candidate of that rule

    bool operator==(const DrawLine&) const = default;
};

struct PresentDrawLine // a draw issued while the bridge's Present was running (ReShade inner, Dalamud, HDR/NR/Companion work)
{
    uint32_t seq = 0;
    std::string id;     // shader id as in shaders.csv
    uint8_t stage = kStageNone;
    RtKind rt = RtKind::None;
    RtShape shape {};

    bool operator==(const PresentDrawLine&) const = default;
};

struct FrameRecord
{
    uint64_t frame = 0;
    FrameSummary summary {};
    Candidates candidates {};
    uint32_t presentDraws = 0;
    uint32_t omittedDraws = 0;    // tagged/listed-worthy draws beyond kMaxListedDraws
    uint32_t omittedPresent = 0;
    std::string reason;           // why this frame was logged: first600 | every100 | anomaly:<what>
    std::vector<DrawLine> draws;
    std::vector<PresentDrawLine> present;

    bool operator==(const FrameRecord&) const = default;
};

inline std::string FrameRecordText(const FrameRecord& r)
{
    const auto& s = r.summary;
    const auto& c = r.candidates;
    char head[512];
    snprintf(head, sizeof head,
             "F frame=%llu why=%s draws=%u present_draws=%u scene=%d s1=%u s2=%u s3=%u s4=%u s5=%u ui=%u unknown=%u "
             "r1=%u r2=%u r2any=%u r3=%u moves=%u r1_sup=%u r2_sup=%u ui_before_scene=%u first_ui=%u last_scene=%u last_s5=%u "
             "omitted=%u omitted_present=%u\n",
             static_cast<unsigned long long>(r.frame), r.reason.empty() ? "-" : r.reason.c_str(), s.draws, r.presentDraws,
             s.sawSceneStage ? 1 : 0, s.stageDraws[1], s.stageDraws[2], s.stageDraws[3], s.stageDraws[4], s.stageDraws[5],
             s.uiDraws, s.unknownDraws, c.r1, c.r2, c.r2any, c.r3, s.candidateMoves, s.r1Supersessions, s.r2Supersessions,
             s.uiBeforeLastScene, s.firstUi, s.lastSceneStage, s.lastStage5, r.omittedDraws, r.omittedPresent);
    std::string out = head;
    for (const auto& d : r.draws)
    {
        out += "  d " + std::to_string(d.ordinal) + ' ' + StageName(d.stage) + ' ' + RtKindName(d.rt) + ' ' +
               ShapeText(d.shape);
        if (d.r1)
            out += " R1";
        if (d.r2)
            out += " R2";
        if (d.r2any)
            out += " R2any";
        if (d.r3)
            out += " R3";
        out += '\n';
    }
    for (const auto& p : r.present)
        out += "  p " + std::to_string(p.seq) + ' ' + p.id + ' ' + StageName(p.stage) + ' ' + RtKindName(p.rt) + ' ' +
               ShapeText(p.shape) + '\n';
    return out;
}

inline bool ParseFrameHead(std::string_view line, FrameRecord& r)
{
    const auto parts = Split(line, ' ');
    if (parts.size() < 2 || parts[0] != "F")
        return false;
    auto& s = r.summary;
    auto& c = r.candidates;
    for (size_t i = 1; i < parts.size(); ++i)
    {
        const auto kv = parts[i];
        const auto eq = kv.find('=');
        if (eq == std::string_view::npos)
            return false;
        const auto k = kv.substr(0, eq);
        const auto v = kv.substr(eq + 1);
        bool ok = true;
        unsigned scene = 0;
        if (k == "frame") ok = ParseNumber(v, r.frame);
        else if (k == "why") r.reason = v == "-" ? "" : std::string(v);
        else if (k == "draws") ok = ParseNumber(v, s.draws);
        else if (k == "present_draws") ok = ParseNumber(v, r.presentDraws);
        else if (k == "scene") { ok = ParseNumber(v, scene); s.sawSceneStage = scene != 0; }
        else if (k == "s1") ok = ParseNumber(v, s.stageDraws[1]);
        else if (k == "s2") ok = ParseNumber(v, s.stageDraws[2]);
        else if (k == "s3") ok = ParseNumber(v, s.stageDraws[3]);
        else if (k == "s4") ok = ParseNumber(v, s.stageDraws[4]);
        else if (k == "s5") ok = ParseNumber(v, s.stageDraws[5]);
        else if (k == "ui") ok = ParseNumber(v, s.uiDraws);
        else if (k == "unknown") ok = ParseNumber(v, s.unknownDraws);
        else if (k == "r1") ok = ParseNumber(v, c.r1);
        else if (k == "r2") ok = ParseNumber(v, c.r2);
        else if (k == "r2any") ok = ParseNumber(v, c.r2any);
        else if (k == "r3") ok = ParseNumber(v, c.r3);
        else if (k == "moves") ok = ParseNumber(v, s.candidateMoves);
        else if (k == "r1_sup") ok = ParseNumber(v, s.r1Supersessions);
        else if (k == "r2_sup") ok = ParseNumber(v, s.r2Supersessions);
        else if (k == "ui_before_scene") ok = ParseNumber(v, s.uiBeforeLastScene);
        else if (k == "first_ui") ok = ParseNumber(v, s.firstUi);
        else if (k == "last_scene") ok = ParseNumber(v, s.lastSceneStage);
        else if (k == "last_s5") ok = ParseNumber(v, s.lastStage5);
        else if (k == "omitted") ok = ParseNumber(v, r.omittedDraws);
        else if (k == "omitted_present") ok = ParseNumber(v, r.omittedPresent);
        else ok = false;
        if (!ok)
            return false;
    }
    s.sceneStageDraws = s.stageDraws[1] + s.stageDraws[2] + s.stageDraws[3] + s.stageDraws[4] + s.stageDraws[5];
    return true;
}

inline std::vector<FrameRecord> ParseFrames(std::string_view text)
{
    std::vector<FrameRecord> records;
    for (auto line : Split(text, '\n'))
    {
        if (line.empty() || line[0] == '#')
            continue;
        if (line[0] == 'F')
        {
            FrameRecord r;
            if (ParseFrameHead(line, r))
                records.push_back(std::move(r));
            continue;
        }
        if (records.empty())
            continue;
        const auto parts = Split(line.substr(line.find_first_not_of(' ')), ' ');
        if (parts.size() >= 5 && parts[0] == "d")
        {
            DrawLine d;
            if (!ParseNumber(parts[1], d.ordinal) || !ParseShape(parts[4], d.shape))
                continue;
            d.stage = StageFromName(parts[2]);
            d.rt = RtKindFromName(parts[3]);
            for (size_t i = 5; i < parts.size(); ++i)
            {
                if (parts[i] == "R1") d.r1 = true;
                else if (parts[i] == "R2") d.r2 = true;
                else if (parts[i] == "R2any") d.r2any = true;
                else if (parts[i] == "R3") d.r3 = true;
            }
            records.back().draws.push_back(d);
        }
        else if (parts.size() == 6 && parts[0] == "p")
        {
            PresentDrawLine p;
            if (!ParseNumber(parts[1], p.seq) || !ParseShape(parts[5], p.shape))
                continue;
            p.id = std::string(parts[2]);
            p.stage = StageFromName(parts[3]);
            p.rt = RtKindFromName(parts[4]);
            records.back().present.push_back(std::move(p));
        }
    }
    return records;
}

// Which frames are logged: the first 600, then every 100th.
inline bool LogThisFrame(uint64_t frame) { return frame <= 600 || frame % 100 == 0; }

// ---- cost.txt ------------------------------------------------------------------------------------------------------

struct Rolling
{
    uint64_t count = 0;
    double sum = 0, min = 0, max = 0;

    void Add(double v)
    {
        if (count == 0 || v < min)
            min = v;
        if (count == 0 || v > max)
            max = v;
        sum += v;
        ++count;
    }
    double Mean() const { return count ? sum / double(count) : 0.0; }
    void Reset() { *this = Rolling {}; }
};

inline std::string RollingText(const char* name, const Rolling& r)
{
    char b[160];
    snprintf(b, sizeof b, "%s n=%llu mean=%.2f min=%.2f max=%.2f", name, static_cast<unsigned long long>(r.count), r.Mean(),
             r.min, r.max);
    return b;
}

// ---- run folder ----------------------------------------------------------------------------------------------------

// YYYYMMDD-HHMMSS, local time of the first armed Present. Sorts chronologically as a string.
inline std::string FormatRunId(const std::tm& t)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
             t.tm_sec);
    return buf;
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

// Of the folder names next to a new run (the new run included), the oldest beyond `keep`. Names that are not run ids are
// never listed: D1 only ever deletes what it created.
inline std::vector<std::string> RunsToPrune(std::vector<std::string> names, size_t keep = kKeepRuns)
{
    names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& n) { return !IsRunId(n); }), names.end());
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    if (names.size() <= keep)
        return {};
    return std::vector<std::string>(names.begin(), names.end() - static_cast<std::ptrdiff_t>(keep));
}

// ---- output budget -------------------------------------------------------------------------------------------------

// A per-file byte cap with a TRUNCATED marker. Take(n) says whether n more bytes may be written; the first refusal asks
// for the marker line once.
struct ByteBudget
{
    uint64_t limit = 0;
    uint64_t used = 0;
    bool truncated = false;

    bool Take(uint64_t n)
    {
        if (truncated || used + n > limit)
        {
            truncated = true;
            return false;
        }
        used += n;
        return true;
    }
};

inline constexpr const char* kTruncatedLine = "# TRUNCATED: byte budget of this file reached, later records were dropped\n";

// ---- snapshot raw container ----------------------------------------------------------------------------------------

// "UIDS" raw snapshot: 32-byte little-endian header, then `height` rows of `rowPitch` bytes. Used when the pixel format
// has no 8-bit PNG path (FP16, R10G10B10A2, ...). Header: magic 'U','I','D','S'; u32 version=1; u32 width; u32 height;
// u32 dxgiFormat; u32 rowPitch; u32 frame; u32 reserved=0.
struct RawHeader
{
    uint32_t magic = 0x53444955; // "UIDS" little endian
    uint32_t version = 1;
    uint32_t width = 0, height = 0, format = 0, rowPitch = 0, frame = 0, reserved = 0;
};
static_assert(sizeof(RawHeader) == 32);
} // namespace ext::ffxiv::ui::dfmt
