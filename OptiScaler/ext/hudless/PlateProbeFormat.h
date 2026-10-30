#pragma once
// Developer probes, pure text side (docs/HUDLESS.md, "Developer probes"): the CSV and summary formats, the rule R0 that flags a
// draw as a nameplate candidate, the frame sampling rule, the byte caps, the per-shader table and the run folder naming.
// No I/O, no D3D, no clock: tests/ext/hudless builds this header standalone, so the writer and the readers cannot drift apart.
//
// Files under <game>\OptiScaler\plate-probe\<run>\ :
//   outputs.csv       one row per output present (appended in batches at flush; byte cap kOutputsBytes)
//   draws.csv         one row per recorded UI draw (frames 1..600, then every 100th; byte caps kEarlyDrawBytes / kWorldDrawBytes)
//   shaders-plate.csv per pixel shader counts, rewritten at frames 120, 600 and every 1800
// The summary line of the output probe goes to the log every 10 seconds.

#include "ext/hudless/PlateProbeAccounting.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ext::hudless::plate
{
inline constexpr int kFormatVersion = 3; // 3: draws.csv lost its packet columns (the probe reads no packets); 2: outputs.csv gained ticks_since_post
inline constexpr size_t kKeepRuns = 5;

// ---- caps ----------------------------------------------------------------------------------------------------------
inline constexpr uint64_t kOutputsBytes = 48ull << 20;   // outputs.csv; the summary line keeps running after the cap
inline constexpr uint64_t kEarlyDrawBytes = 6ull << 20;  // draws.csv rows of frames 1..600 (title and loading screens)
inline constexpr uint64_t kWorldDrawBytes = 40ull << 20; // draws.csv rows of the sampled frames after 600 (in the world)
inline constexpr size_t kMaxDrawRowsPerFrame = 400;      // rows of one frame in draws.csv
inline constexpr size_t kMaxPriorityRowsPerFrame = 1200; // the higher limit for rule r0 rows, so the rows the probe is about are not the ones dropped
inline constexpr size_t kMaxShaderRows = 5000;
inline constexpr uint64_t kEarlyFrames = 600;

inline constexpr const char* kTruncatedLine = "# TRUNCATED: byte cap of this file reached, later rows were dropped\n";

struct ByteBudget
{
    uint64_t limit = 0;
    uint64_t used = 0;
    bool truncated = false;

    // Whether `n` more bytes may be written; the first refusal is the one that asks for the marker line.
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

// The draw rows of the early frames and of the world frames have a cap each, so the title screens cannot use up the room the
// in-world samples need.
struct DrawBudget
{
    ByteBudget early { kEarlyDrawBytes };
    ByteBudget world { kWorldDrawBytes };

    bool Take(uint64_t frame, uint64_t bytes) { return (frame <= kEarlyFrames ? early : world).Take(bytes); }
};

// The row cap of one frame of draws.csv. `kept` counts every row written in the frame, normal or priority. A normal row is admitted while
// fewer than kMaxDrawRowsPerFrame rows are kept; a priority row (a rule r0 candidate: the rows the
// probe exists for, and nameplates are drawn late in a frame) while fewer than kMaxPriorityRowsPerFrame are kept. Whatever is refused is counted,
// per frame here and per run by the caller, so a frame that lost rows says so (a missing r0 row is then not read as "r0 never fires").
struct RowCap
{
    size_t kept = 0;
    uint64_t dropped = 0;         // rows refused in this frame, normal and priority
    uint64_t priorityDropped = 0; // of them, priority rows (the second limit was reached)

    bool Admit(bool priority)
    {
        if (kept < (priority ? kMaxPriorityRowsPerFrame : kMaxDrawRowsPerFrame))
        {
            ++kept;
            return true;
        }
        ++dropped;
        if (priority)
            ++priorityDropped;
        return false;
    }

    bool Capped() const { return dropped != 0; }
    void NextFrame() { *this = RowCap(); }
};

// The comment rows draws.csv gets after the rows of a frame that lost some (an analysis tool skips every line that starts with '#').
inline std::string CapMarker(uint64_t frame, const RowCap& cap)
{
    if (!cap.Capped())
        return {};
    std::string text = "# frame " + std::to_string(frame) + " capped at " + std::to_string(kMaxDrawRowsPerFrame) + " rows, " +
                       std::to_string(cap.dropped) + " draws dropped\n";
    if (cap.priorityDropped)
        text += "# frame " + std::to_string(frame) + " priority limit " + std::to_string(kMaxPriorityRowsPerFrame) + " reached, " +
                std::to_string(cap.priorityDropped) + " r0 draws dropped\n";
    return text;
}

// Which frames get draw rows: the first 600, then every 100th (the same schedule as frames.txt).
inline bool LogThisFrame(uint64_t frame) { return frame >= 1 && (frame <= kEarlyFrames || frame % 100 == 0); }

// shaders-plate.csv is rewritten at frames 120, 600 and every 1800.
inline bool RewriteShadersThisFrame(uint64_t frame) { return frame == 120 || frame == 600 || (frame != 0 && frame % 1800 == 0); }

// The frame classes of the per-shader counts: 0 = frames 1..600 (title and loading), 1 = the rest (in the world).
inline int FrameClass(uint64_t frame) { return frame <= kEarlyFrames ? 0 : 1; }

// ---- csv text ------------------------------------------------------------------------------------------------------
// A field that holds a comma, a quote or a line break is quoted, with quotes doubled. Everything else is written as it is.
inline std::string CsvField(std::string_view text)
{
    if (text.find_first_of(",\"\r\n") == std::string_view::npos)
        return std::string(text);
    std::string out = "\"";
    for (char c : text)
    {
        if (c == '"')
            out += '"';
        out += c;
    }
    out += '"';
    return out;
}

namespace detail
{
inline std::string Num(uint64_t v) { return std::to_string(v); }
inline std::string Ms(int64_t ticks, double ticksPerMs)
{
    char b[48];
    snprintf(b, sizeof b, "%.3f", ticksPerMs > 0 ? double(ticks) / ticksPerMs : 0.0);
    return b;
}
inline std::string Hex(uint64_t v)
{
    char b[24];
    snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}
template <class Map> inline std::string MapText(const Map& m, size_t limit = 8)
{
    std::string out = "{";
    size_t n = 0;
    for (const auto& [key, count] : m)
    {
        if (n)
            out += ", ";
        if (n == limit)
        {
            out += "...";
            break;
        }
        out += std::to_string(key) + ":" + std::to_string(count);
        ++n;
    }
    return out + "}";
}
} // namespace detail

// ---- outputs.csv ---------------------------------------------------------------------------------------------------
inline constexpr const char* kOutputsHeader =
    "qpc,t_ms,thread,back_buffer,window,batch,index,multiplier,lag_posts,lag_ms,since_prev_ms,ticks_since_post";

inline std::string OutputsHeaderText()
{
    return std::string("# plate probe outputs.csv, format ") + std::to_string(kFormatVersion) +
           ". One row per output present, in QPC order. t_ms: milliseconds since the first Post of the run. window: the real frame whose\n"
           "# Post opened the window the output arrived in. batch/index: the real frame the output belongs to by order and its position\n"
           "# in it (0 = none). multiplier: outputs that frame was dispatched for (0 = unknown). lag_posts: real frames that were posted\n"
           "# between that frame's Post and this output. -1 = no batch. ticks_since_post: raw QPC ticks from the Post that opened the window\n"
           "# to this output (-1 = before the first Post).\n" +
           kOutputsHeader + "\n";
}

inline std::string OutputsRow(const OutputRecord& r, uint64_t qpcZero, double ticksPerMs)
{
    const int64_t since = r.qpc >= qpcZero ? static_cast<int64_t>(r.qpc - qpcZero) : 0;
    std::string line = detail::Num(r.qpc) + ',' + detail::Ms(since, ticksPerMs) + ',' + detail::Num(r.thread) + ',' +
                       std::to_string(r.backBuffer) + ',' + detail::Num(r.window) + ',' + detail::Num(r.batch) + ',' +
                       detail::Num(r.indexInBatch) + ',' + detail::Num(r.multiplier) + ',' + std::to_string(r.lagPosts) + ',';
    line += r.lagTicks >= 0 ? detail::Ms(r.lagTicks, ticksPerMs) : std::string("-1");
    line += ',' + detail::Ms(r.sinceLastTicks, ticksPerMs) + ',' + std::to_string(r.ticksSincePost) + '\n';
    return line;
}

// ---- packet association counters -----------------------------------------------------------------------------------
// What happened to the logged draw rows when the draw probe looked for the game packets behind them (queue key, vertex box), and to the
// draw commands the association saw being enqueued. A row is exactly one of: not an indexed draw (no packet can sit behind it), with a
// packet, without any packet record (no batch of the game was being submitted for it, or the frame was not one the probe reads), or
// rejected (a record existed but could not be trusted). Pure data: the runtime keeps atomics and copies them here for the summary line.
enum class RowOutcome : uint8_t
{
    NotIndexed = 0, // not an indexed draw
    Associated,     // a record was bound to the draw and it pointed at least at one packet
    NoPacket,       // no record: nothing was bound to the draw, or the record pointed at no packet
    Stale,          // a record was found but it was older than the age limit
    Changed,        // the command bytes at bind time differed from the ones recorded at enqueue time (the pool reused the address)
    Mismatch,       // the draw's index count, start index or base vertex differed from the record's command
};

// Why an associated row has no vertex box (exactly one reason per such row, checked in this order).
enum class BoxReason : uint8_t
{
    Ok = 0,    // the row has a box
    NoRun,     // no vertex bytes were copied for the draw, or what was copied does not fit the vertices the draw uses
    Untagged,  // the draw's input layout carries no position tag (only the layouts of the three nameplate vertex shaders do)
    Strides,   // the strides of the packets, of the renderer's slot and of the bound vertex buffer do not agree, or the layout does not fit them
    NonFinite, // every used vertex has a position that is not a finite number
};

struct AssocStats
{
    uint64_t rows = 0, notIndexed = 0, associated = 0, noPacket = 0;
    uint64_t stale = 0, changed = 0, mismatch = 0;                                    // rejected rows by reason
    uint64_t boxes = 0, noBox = 0;                                                    // associated rows with and without a vertex box
    uint64_t noBoxNoRun = 0, noBoxUntagged = 0, noBoxStrides = 0, noBoxNonFinite = 0; // noBox by reason (they add up to noBox)
    uint64_t made = 0, refusedCapacity = 0, refusedRead = 0, refusedBusy = 0;         // enqueue side: records made, and refusals by reason
    uint64_t unmapped = 0;                                                            // records whose indices fell in no known packet
    uint64_t rowsCapped = 0, framesCapped = 0, priorityCapped = 0; // draws.csv row cap: rows refused, frames that refused some, priority rows among the refused
    uint64_t batchReads = 0, batchCut = 0, headerCap = 0;          // batch calls whose packets were read, cut by the time budget, refused for too many headers

    uint64_t Rejected() const { return stale + changed + mismatch; }
    uint64_t NoBoxReasons() const { return noBoxNoRun + noBoxUntagged + noBoxStrides + noBoxNonFinite; }
    bool Any() const
    {
        return rows || made || refusedCapacity || refusedRead || refusedBusy || unmapped || rowsCapped || batchReads || batchCut || headerCap;
    }

    // one logged row; `reason` only matters for an associated row
    void NoteRow(RowOutcome outcome, BoxReason reason)
    {
        ++rows;
        switch (outcome)
        {
        case RowOutcome::NotIndexed: ++notIndexed; break;
        case RowOutcome::Associated:
            ++associated;
            if (reason == BoxReason::Ok)
            {
                ++boxes;
                break;
            }
            ++noBox;
            switch (reason)
            {
            case BoxReason::NoRun: ++noBoxNoRun; break;
            case BoxReason::Untagged: ++noBoxUntagged; break;
            case BoxReason::Strides: ++noBoxStrides; break;
            default: ++noBoxNonFinite; break;
            }
            break;
        case RowOutcome::NoPacket: ++noPacket; break;
        case RowOutcome::Stale: ++stale; break;
        case RowOutcome::Changed: ++changed; break;
        case RowOutcome::Mismatch: ++mismatch; break;
        }
    }

    // a frame of draws.csv that refused rows (`dropped` includes `priorityDropped`)
    void NoteCappedFrame(uint64_t dropped, uint64_t priorityDropped)
    {
        rowsCapped += dropped;
        priorityCapped += priorityDropped;
        ++framesCapped;
    }
};

// The tail of the summary line (docs/HUDLESS.md 12.1). Every key is always printed, zero or not, so a reader can match it as it is.
inline std::string AssocText(const AssocStats& a)
{
    using detail::Num;
    return ", draw rows " + Num(a.rows) + " (not indexed " + Num(a.notIndexed) + ", with packet " + Num(a.associated) + ", no packet " +
           Num(a.noPacket) + ", rejected " + Num(a.Rejected()) + " {stale:" + Num(a.stale) + ", changed:" + Num(a.changed) +
           ", mismatch:" + Num(a.mismatch) + "}), box from vertices " + Num(a.boxes) + ", without box " + Num(a.noBox) + " {untagged:" +
           Num(a.noBoxUntagged) + ", strides:" + Num(a.noBoxStrides) + ", norun:" + Num(a.noBoxNoRun) + ", nonfinite:" + Num(a.noBoxNonFinite) +
           "}, records made " + Num(a.made) + ", refused {capacity:" + Num(a.refusedCapacity) + ", read:" + Num(a.refusedRead) + ", busy:" +
           Num(a.refusedBusy) + "}, indices outside packets " + Num(a.unmapped) + ", rows capped " + Num(a.rowsCapped) + " (frames " +
           Num(a.framesCapped) + "), priority rows lost " + Num(a.priorityCapped) + ", batch cut " + Num(a.batchCut) + " of " +
           Num(a.batchReads) + ", header cap hit " + Num(a.headerCap);
}

// ---- the summary line ----------------------------------------------------------------------------------------------
struct SummaryExtras
{
    uint64_t ringDrops = 0;   // events the ring had no room for (the probe's own loss, never a pacer drop)
    uint64_t mergedLate = 0;  // events that came in below what an earlier flush had already released
    uint64_t reordered = 0;   // events pushed out of QPC order (producers racing)
    double ticksPerMs = 0;
    AssocStats assoc;         // the packet association of the draw probe (appended at the end of the line)
};

inline std::string SummaryLine(const Summary& s, const SummaryExtras& x)
{
    using detail::Num;
    char percent[96];
    if (s.windowsKnown)
        snprintf(percent, sizeof percent, "%.1f%% (%llu of %llu judged, %llu settling, %llu without M)", s.Percent(),
                 static_cast<unsigned long long>(s.windowsEqual), static_cast<unsigned long long>(s.windowsKnown),
                 static_cast<unsigned long long>(s.windowsTransition), static_cast<unsigned long long>(s.windowsUnknown));
    else
        snprintf(percent, sizeof percent, "n/a (0 judged, %llu settling, %llu without M)",
                 static_cast<unsigned long long>(s.windowsTransition), static_cast<unsigned long long>(s.windowsUnknown));

    // the cumulative lag keeps the worst moment of the whole run; the interval is the time since the previous summary line
    const std::string lag = std::to_string(s.maxLagPosts) + " posts, " + detail::Ms(s.maxLagTicks, x.ticksPerMs) + " ms, " + Num(s.lateOutputs) +
                            " late outputs (this interval: " + std::to_string(s.interval.maxLagPosts) + " posts, " +
                            detail::Ms(s.interval.maxLagTicks, x.ticksPerMs) + " ms, " + Num(s.interval.lateOutputs) + " late)";

    const std::string order = "first-output lag " + detail::MapText(s.firstLag) + " (this interval " + detail::MapText(s.interval.firstLag) +
                              "), pushed out of order " + Num(x.reordered) + ", late events " + Num(x.mergedLate);

    char options[16];
    snprintf(options, sizeof options, "%.2f", s.posts ? double(s.optionsEvents) / double(s.posts) : 0.0);

    // is the multiplier noted for the right frame: the share of Options events pushed from their Post's thread, and how long after the Post they came
    // (read the mean delay against the mean Post interval: a delay close to one interval means the multiplier is noted a frame late)
    char sameThread[64];
    if (s.optionsMatched)
        snprintf(sameThread, sizeof sameThread, "%.1f%% (%llu of %llu)", 100.0 * double(s.optionsSameThread) / double(s.optionsMatched),
                 static_cast<unsigned long long>(s.optionsSameThread), static_cast<unsigned long long>(s.optionsMatched));
    else
        snprintf(sameThread, sizeof sameThread, "n/a (0 of 0)");
    const int64_t meanDelay = s.optionsMatched ? static_cast<int64_t>(s.optionsDelayTicks / s.optionsMatched) : 0;
    const int64_t meanInterval = s.postIntervals ? static_cast<int64_t>(s.postIntervalTicks / s.postIntervals) : 0;
    const std::string phase = "options orphaned " + Num(s.orphanOptions) + ", options on the Post's thread " + sameThread +
                              ", options delay after Post mean " + detail::Ms(meanDelay, x.ticksPerMs) + " ms, max " +
                              detail::Ms(s.optionsDelayMaxTicks, x.ticksPerMs) + " ms (Post interval mean " +
                              detail::Ms(meanInterval, x.ticksPerMs) + " ms)";

    return "Plate probe: outputs " + Num(s.outputs) + ", batches " + Num(s.windows) + ", batches with count == M " + percent +
           ", count histogram " + detail::MapText(s.countHistogram, 12) + ", threads " + detail::MapText(s.threads) +
           ", max lag (outputs seen after the next Post) " + lag + ", order check " + order + ", posts " + Num(s.posts) +
           ", options per Post " + options + ", " + phase + ", resets " + Num(s.resets) + ", probe drops " + Num(x.ringDrops) +
           AssocText(x.assoc);
}

// ---- rule R0 -------------------------------------------------------------------------------------------------------
// The working rule for a nameplate draw: depth tested with GREATER_EQUAL, no depth write, no stencil, drawn into the backbuffer or a
// texture shaped like it. Pure function of recorded state; the caller resolves a missing depth-stencil state object to the D3D11
// default before asking (which is LESS with writes on, so never a candidate).
enum class RtClass : uint8_t
{
    None = 0,
    Backbuffer = 1,
    BackbufferLike = 2,
    Other = 3,
};

inline const char* RtName(RtClass c)
{
    switch (c)
    {
    case RtClass::Backbuffer: return "BB";
    case RtClass::BackbufferLike: return "BBLIKE";
    case RtClass::Other: return "OTHER";
    default: return "NONE";
    }
}

inline constexpr uint32_t kCompareLess = 2;
inline constexpr uint32_t kCompareGreaterEqual = 7;

struct DepthState
{
    bool dsvBound = false;       // a depth-stencil view is bound
    bool depthEnable = true;
    bool depthWrite = true;      // the depth write mask is not ZERO
    uint32_t depthFunc = kCompareLess;
    bool stencilEnable = false;
};

// D3D11's default depth-stencil state, used when none is bound.
inline DepthState DefaultDepthState(bool dsvBound)
{
    DepthState d;
    d.dsvBound = dsvBound;
    return d;
}

inline bool IsCandidate(const DepthState& d, RtClass rt)
{
    return d.dsvBound && d.depthEnable && d.depthFunc == kCompareGreaterEqual && !d.depthWrite && !d.stencilEnable &&
           (rt == RtClass::Backbuffer || rt == RtClass::BackbufferLike);
}

// ---- draws.csv -----------------------------------------------------------------------------------------------------
struct DrawRow
{
    uint64_t frame = 0;             // Present counter
    uint32_t draw = 0;              // ordinal of the draw among the frame's counted draws
    std::string vs = "-", ps = "-"; // 0xCRC32 of the bytecode, pre#N for a shader created before the hook, - unknown
    int stage = -1;                 // catalog stage of the pixel shader, -1 unknown
    char kind = 'I';                // D draw, I indexed, N instanced, X indexed instanced, Y indirect
    uint32_t indexCount = 0, instanceCount = 0, startIndex = 0;
    int32_t baseVertex = 0;
    uint32_t topology = 0, stride = 0;
    RtClass rt = RtClass::None;
    uint32_t rtWidth = 0, rtHeight = 0, rtFormat = 0;
    uint32_t vpWidth = 0, vpHeight = 0;
    DepthState depth;
    bool blend = false;
    uint32_t srcBlend = 0, dstBlend = 0, blendOp = 0, srcAlpha = 0, dstAlpha = 0;
    std::string srvs = "-";         // bound pixel shader textures: id/WxH/format, '|' between slots
};

inline constexpr const char* kDrawsHeader =
    "frame,draw,vs,ps,stage,kind,index_count,instance_count,start_index,base_vertex,topology,stride,rt,rt_w,"
    "rt_h,rt_fmt,vp_w,vp_h,dsv,depth,depth_write,depth_func,stencil,blend,src,dst,op,src_a,dst_a,srvs,r0";
inline constexpr size_t kDrawsColumns = 31;

inline std::string DrawsHeaderText()
{
    return std::string("# plate probe draws.csv, format ") + std::to_string(kFormatVersion) +
           ". One row per recorded draw: every draw of a logged frame (frames 1..600, then every 100th) that is a UI-stage draw or comes\n"
           "# after the first UI draw of the frame, plus on any frame every draw that satisfies r0. The probe reads no game packets:\n"
           "# there are no packet columns (no batch, queue key, vertex box or interception marker). rt: BB backbuffer, BBLIKE same size\n"
           "# and format, OTHER, NONE. depth_func: D3D11 comparison (2 LESS, 4 LESS_EQUAL, 5 GREATER, 7 GREATER_EQUAL, 8 ALWAYS).\n"
           "# depth_write 1 = writes on. src/dst/op/src_a/dst_a: blend factors of render target 0. r0 = 1: depth tested\n"
           "# GREATER_EQUAL, no depth write, no stencil, drawn into the backbuffer or a texture of its size and format. A line that starts\n"
           "# with # between the rows is a marker, not a row: '# frame N capped at 400 rows, M draws dropped' says that frame lost M draws to the row cap.\n" +
           kDrawsHeader + "\n";
}

inline std::string DrawsRow(const DrawRow& r)
{
    using detail::Num;
    std::string line = Num(r.frame) + ',' + Num(r.draw) + ',' + CsvField(r.vs) + ',' + CsvField(r.ps) + ',' +
                       std::to_string(r.stage) + ',' + r.kind + ',' + Num(r.indexCount) + ',' + Num(r.instanceCount) + ',' +
                       Num(r.startIndex) + ',' + std::to_string(r.baseVertex) + ',' + Num(r.topology) + ',' + Num(r.stride) + ',' +
                       RtName(r.rt) + ',' + Num(r.rtWidth) + ',' + Num(r.rtHeight) + ',' + Num(r.rtFormat) + ',' + Num(r.vpWidth) + ',' +
                       Num(r.vpHeight) + ',' + (r.depth.dsvBound ? '1' : '0') + ',' + (r.depth.depthEnable ? '1' : '0') + ',' +
                       (r.depth.depthWrite ? '1' : '0') + ',' + Num(r.depth.depthFunc) + ',' + (r.depth.stencilEnable ? '1' : '0') + ',' +
                       (r.blend ? '1' : '0') + ',' + Num(r.srcBlend) + ',' + Num(r.dstBlend) + ',' + Num(r.blendOp) + ',' +
                       Num(r.srcAlpha) + ',' + Num(r.dstAlpha) + ',' + CsvField(r.srvs) + ',' +
                       (IsCandidate(r.depth, r.rt) ? '1' : '0') + '\n';
    return line;
}

// ---- shaders-plate.csv ---------------------------------------------------------------------------------------------
struct ShaderPlateRow
{
    std::string ps;
    std::string vsSet = "-"; // up to four vertex shaders seen with it (sampled), '|' between, ... when more
    int stage = -1;
    uint64_t firstFrame = 0;
    uint64_t earlyDraws = 0, earlyR0 = 0;     // frames 1..600
    uint64_t worldDraws = 0, worldR0 = 0;     // frames after 600
    uint64_t worldFrames = 0, worldR0Frames = 0;
    uint64_t worldMax = 0, worldR0Max = 0;    // most draws / r0 draws in one world frame
    uint64_t rt[4] = { 0, 0, 0, 0 };          // draws by render target class: NONE, BB, BBLIKE, OTHER
};

inline constexpr const char* kShadersHeader =
    "ps,vs_set,stage,first_frame,early_draws,early_r0,world_draws,world_r0,world_frames,world_r0_frames,world_max_per_frame,"
    "world_r0_max_per_frame,rt_none,rt_bb,rt_bblike,rt_other";

inline std::string ShadersHeaderText()
{
    return std::string("# plate probe shaders-plate.csv, format ") + std::to_string(kFormatVersion) +
           ". Every draw of the game, counted per pixel shader. early = frames 1..600 (title and loading screens), world = later frames.\n"
           "# *_r0 counts the draws that satisfy rule r0 (see draws.csv). world_frames: frames with at least one draw of the shader;\n"
           "# world_r0_frames: frames with at least one r0 draw. Compare a run with nameplates off against one with them on: the world_r0\n"
           "# columns of the nameplate shaders should differ, the others should not. vs_set is sampled (logged and r0 draws only). rt_* count the\n"
           "# draws whose target was looked up (UI-phase draws and draws that pass the depth part of r0); every other draw counts as rt_none.\n" +
           kShadersHeader + "\n";
}

inline std::string ShadersRow(const ShaderPlateRow& r)
{
    using detail::Num;
    return CsvField(r.ps) + ',' + CsvField(r.vsSet) + ',' + std::to_string(r.stage) + ',' + Num(r.firstFrame) + ',' + Num(r.earlyDraws) + ',' +
           Num(r.earlyR0) + ',' + Num(r.worldDraws) + ',' + Num(r.worldR0) + ',' + Num(r.worldFrames) + ',' + Num(r.worldR0Frames) + ',' +
           Num(r.worldMax) + ',' + Num(r.worldR0Max) + ',' + Num(r.rt[0]) + ',' + Num(r.rt[1]) + ',' + Num(r.rt[2]) + ',' + Num(r.rt[3]) + '\n';
}

// Per-shader accumulation. The caller keeps its own pointer -> index cache for the hot path and calls Find() once per new shader.
class ShaderTable
{
  public:
    size_t Find(const std::string& ps, int stage)
    {
        auto it = _index.find(ps);
        if (it != _index.end())
            return it->second;
        ShaderPlateRow row;
        row.ps = ps;
        row.stage = stage;
        _rows.push_back(Entry { row, 0, 0, 0 });
        _index[ps] = _rows.size() - 1;
        return _rows.size() - 1;
    }

    void Add(size_t index, uint64_t frame, bool r0, RtClass rt)
    {
        if (index >= _rows.size())
            return;
        Entry& e = _rows[index];
        if (e.row.firstFrame == 0)
            e.row.firstFrame = frame;
        if (e.curFrame != frame)
        {
            Roll(e);
            e.curFrame = frame;
        }
        ++e.curDraws;
        if (r0)
            ++e.curR0;
        ++e.row.rt[static_cast<size_t>(rt)];
        if (FrameClass(frame) == 0)
        {
            ++e.row.earlyDraws;
            if (r0)
                ++e.row.earlyR0;
        }
        else
        {
            ++e.row.worldDraws;
            if (r0)
                ++e.row.worldR0;
        }
    }

    void NoteVertexShader(size_t index, const std::string& vs)
    {
        if (index >= _rows.size() || vs.empty() || vs == "-")
            return;
        Entry& e = _rows[index];
        for (const auto& v : e.vs)
            if (v == vs)
                return;
        if (e.vs.size() < 4)
            e.vs.push_back(vs);
        else
            e.vsMore = true;
    }

    // Rows sorted by world_r0, world_draws (descending), then name; at most kMaxShaderRows. Counters of the frame in progress are included.
    std::vector<ShaderPlateRow> Rows() const
    {
        std::vector<ShaderPlateRow> out;
        out.reserve(_rows.size());
        for (Entry e : _rows)
        {
            Roll(e);
            std::string set;
            for (size_t i = 0; i < e.vs.size(); ++i)
                set += (i ? "|" : "") + e.vs[i];
            if (e.vsMore)
                set += "|...";
            e.row.vsSet = set.empty() ? "-" : set;
            out.push_back(e.row);
        }
        std::sort(out.begin(), out.end(),
                  [](const ShaderPlateRow& a, const ShaderPlateRow& b)
                  {
                      if (a.worldR0 != b.worldR0)
                          return a.worldR0 > b.worldR0;
                      if (a.worldDraws != b.worldDraws)
                          return a.worldDraws > b.worldDraws;
                      return a.ps < b.ps;
                  });
        if (out.size() > kMaxShaderRows)
            out.resize(kMaxShaderRows);
        return out;
    }

    std::string Csv() const
    {
        std::string text = ShadersHeaderText();
        const bool truncated = _rows.size() > kMaxShaderRows;
        for (const auto& r : Rows())
            text += ShadersRow(r);
        if (truncated)
            text += kTruncatedLine;
        return text;
    }

    size_t Size() const { return _rows.size(); }

  private:
    struct Entry
    {
        ShaderPlateRow row;
        uint64_t curFrame = 0;
        uint64_t curDraws = 0;
        uint64_t curR0 = 0;
        std::vector<std::string> vs;
        bool vsMore = false;
    };

    // Closes the per-frame counters of an entry into the per-frame statistics.
    static void Roll(Entry& e)
    {
        if (e.curFrame != 0 && FrameClass(e.curFrame) == 1)
        {
            ++e.row.worldFrames;
            if (e.curR0 > 0)
                ++e.row.worldR0Frames;
            e.row.worldMax = std::max(e.row.worldMax, e.curDraws);
            e.row.worldR0Max = std::max(e.row.worldR0Max, e.curR0);
        }
        e.curDraws = 0;
        e.curR0 = 0;
    }

    std::vector<Entry> _rows;
    std::map<std::string, size_t> _index;
};

// ---- run folder ----------------------------------------------------------------------------------------------------
// YYYYMMDD-HHMMSS, local time of the first record. Sorts chronologically as a string.
inline std::string FormatRunId(const std::tm& t)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
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

// Of the folder names next to a new run (the new run included), the oldest beyond `keep`. Names that are not run ids are never
// listed: the probe only ever deletes what it created.
inline std::vector<std::string> RunsToPrune(std::vector<std::string> names, size_t keep = kKeepRuns)
{
    names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& n) { return !IsRunId(n); }), names.end());
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    if (names.size() <= keep)
        return {};
    return std::vector<std::string>(names.begin(), names.end() - static_cast<std::ptrdiff_t>(keep));
}
} // namespace ext::hudless::plate
