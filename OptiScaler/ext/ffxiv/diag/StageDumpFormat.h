#pragma once
// The per-stage image dump: the pure rules (no D3D, no state). The draw diagnostic's one-frame dump (`DrawDiagnosticsDump = frame`) also writes,
// for the capture frame only, what every stage of this fork did to the picture: the image each stage received, the image it produced, and one
// status line per stage. The runtime is in FfxivDrawDiag.cpp and the stages' entry points are in StageDump.h; this file holds what both agree on
// and what a host test can drive without a device: the stage registry, the file names, the window crop, the open rule (which draws belong to the
// capture frame), the merge rule of a stage's outcomes and the text of facts.txt's stage lines.
//
// Nothing here changes a stage's rendering: a stage reports AFTER its own pass returned, and the copy reads the stage's own output.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace ext::ffxiv::diag
{
// ---- the registry --------------------------------------------------------------------------------------------------------------------
// The order is the number in the file name (stage-<NN>-<name>-in/out.bin, NN = index + 1). The pre-light stages come first, in the order
// of the draws that reach them in a frame; the tone-map chain follows in the order its stages run: the light shafts, the puddles' tone
// stage, the god-rays march, the light bounce and depth of field (each composes the next inside its own entry), then the flare card.
enum class StageId : uint8_t
{
    ContactShadows = 0, // relight/ContactShadowsApply.cpp: the sun mask the contact term is combined into (the copy the light draw samples)
    ShadowLift,         // shadows/ShadowsDispatch.cpp: the mask copy the shadow-mapping lift draws (own route), or the lift ridden on the contact pass
    Ao,                 // engine/AoApply.cpp: the occlusion texture the readers sample
    Gloss,              // engine/GlossPuddlesApply.cpp: G-buffer 1 as the gloss pass rewrites it
    Ssr,                // engine/SsrApply.cpp: the planar reflection map the water draws sample
    Shafts,             // engine/ShaftsDraw.cpp: the light shafts, the first stage of the tone-map chain
    PuddleTone,         // engine/PuddleToneDraw.cpp: the puddles' tone-map stage
    Godrays,            // engine/GodraysDraw.cpp: the volumetric march
    Gi,                 // engine/GiDraw.cpp: the screen-space light bounce
    Dof,                // engine/DofDraw.cpp: depth of field, the chain's last member before t0
    Flare,              // engine/FlareDraw.cpp: the flare card patches the engine's own constants and makes no image: status only
    Atmosphere,         // engine/AtmoDraw.cpp: the atmosphere, the chain's stage between the light bounce and depth of field (APPENDED: stage-12, so 10-dof and 11-flare keep their numbers)
    NightGrade,         // enginelight/EngineLightDraw.cpp: the exposure hold (ENG2), the chain's stage between the light bounce and the atmosphere, in the slot of the retired night grade (stage-13, so the atmosphere keeps 12; the slot keeps the name "night-grade" in the file names)
    Count
};
inline constexpr size_t kStageCount = size_t(StageId::Count);

// Which part of the capture frame a stage belongs to: the draws before the tone map, or the tone-map chain at the tone map itself.
enum class StageGroup : uint8_t
{
    PreLight = 1,
    Chain = 2
};

enum class StagePort : uint8_t
{
    In = 0,
    Out = 1
};

struct StageInfo
{
    const char* name; // the file stem: stage-<NN>-<name>-in.bin
    StageGroup group;
    bool images; // false: the stage makes no image, only its status line is written
};

inline constexpr StageInfo kStageTable[kStageCount] {
    { "contact-shadows", StageGroup::PreLight, true }, { "shadow-lift", StageGroup::PreLight, true },
    { "ao", StageGroup::PreLight, true },              { "gloss", StageGroup::PreLight, true },
    { "ssr", StageGroup::PreLight, true },             { "shafts", StageGroup::Chain, true },
    { "puddle-tone", StageGroup::Chain, true },        { "godrays", StageGroup::Chain, true },
    { "gi", StageGroup::Chain, true },                 { "dof", StageGroup::Chain, true },
    { "flare", StageGroup::Chain, false },
    { "atmosphere", StageGroup::Chain, true },
    { "night-grade", StageGroup::Chain, true },
};

constexpr const StageInfo& StageInfoOf(StageId id) noexcept { return kStageTable[size_t(id)]; }
constexpr uint32_t StageNumber(StageId id) noexcept { return uint32_t(id) + 1; }

// The bits of the open mask the draw diagnostic publishes: a stage reports only while the bit of its group is set.
inline constexpr uint32_t kStagePreBit = 1;
inline constexpr uint32_t kStageChainBit = 2;
constexpr uint32_t StageGroupBit(StageGroup group) noexcept { return group == StageGroup::Chain ? kStageChainBit : kStagePreBit; }
constexpr uint32_t StageBitOf(StageId id) noexcept { return StageGroupBit(StageInfoOf(id).group); }

// "stage-06-shafts-out": the dump item's name, whose file is <name>.bin and whose facts.txt line carries the same text.
inline std::string StageImageName(StageId id, StagePort port)
{
    char number[8];
    std::snprintf(number, sizeof number, "%02u", StageNumber(id));
    return std::string("stage-") + number + "-" + StageInfoOf(id).name + (port == StagePort::In ? "-in" : "-out");
}

// ---- which draws belong to the capture frame ------------------------------------------------------------------------------------------
// The capture frame is the one the one-frame dump reads: the window that follows the splitter's arming, up to and including its tone map.
// Before that window (the splitter still waits for the first tone map) nothing is a stage image of the capture frame; the pre-light stages
// of the window report until its tone map has been seen; the chain's stages run at the tone map itself, after the draw diagnostic closed
// the frame, and report until the frame is over (the next close or the bridge Present). `toneFrameSet` = the dump identified the window's
// tone map; `toneFrameOver` = DumpFrameOver for that frame.
struct StageOpenState
{
    bool dumpActive = false;  // [FFXIV] DrawDiagnosticsDump = frame and the session's allowance holds
    bool dumpWritten = false; // this capture's dump was written already
    bool capturing = false;   // the capture collects draws
    bool splitFresh = true;   // no window opened yet (the partial frame the capture started in)
    bool toneFrameSet = false;
    bool toneFrameOver = false;
};

constexpr uint32_t StageOpenMask(const StageOpenState& s) noexcept
{
    if (!s.dumpActive || s.dumpWritten || !s.capturing || s.splitFresh)
        return 0;
    if (!s.toneFrameSet)
        return kStagePreBit;
    return s.toneFrameOver ? 0 : kStageChainBit;
}

// ---- the crop --------------------------------------------------------------------------------------------------------------------------
// Every image is cropped to the stage's render window (the engine renders the picture at a half-pixel viewport origin, so the origin is
// floored the way the gloss pass floors it) and clamped to the texture. A window the stage does not know (zero size) takes the whole texture.
struct StageBox
{
    uint32_t left = 0, top = 0, right = 0, bottom = 0;
    bool valid = false;
    bool whole = false; // the window was unknown: the whole texture
    constexpr uint32_t Width() const noexcept { return right - left; }
    constexpr uint32_t Height() const noexcept { return bottom - top; }
};

inline StageBox StageCrop(float x, float y, float width, float height, uint32_t textureWidth, uint32_t textureHeight) noexcept
{
    StageBox box;
    if (textureWidth == 0 || textureHeight == 0)
        return box;
    if (!(width >= 1.0f) || !(height >= 1.0f))
    {
        box.right = textureWidth;
        box.bottom = textureHeight;
        box.valid = true;
        box.whole = true;
        return box;
    }
    const float left = std::floor(x < 0.0f ? 0.0f : x);
    const float top = std::floor(y < 0.0f ? 0.0f : y);
    if (left >= float(textureWidth) || top >= float(textureHeight))
        return box;
    box.left = uint32_t(left);
    box.top = uint32_t(top);
    const float right = left + std::floor(width + 0.5f);
    const float bottom = top + std::floor(height + 0.5f);
    box.right = right > float(textureWidth) ? textureWidth : uint32_t(right);
    box.bottom = bottom > float(textureHeight) ? textureHeight : uint32_t(bottom);
    box.valid = box.right > box.left && box.bottom > box.top;
    return box;
}

// The stage images have a place count of their own (the dump's twenty-four places are pinned); the bytes share the dump's budget.
inline constexpr size_t kMaxStageImages = 24;
constexpr bool StageCountFits(size_t held) noexcept { return held < kMaxStageImages; }

// ---- a stage's outcomes in the capture frame -------------------------------------------------------------------------------------------
// Several draws of one frame can reach a stage (the contact shadows' mask, the occlusion's readers and the reflections' water draws run more
// than once), and a later "already answered this frame" must not overwrite "ran": the outcomes merge by this precedence.
enum class StageOutcome : uint8_t
{
    None = 0,
    Declined = 1, // the stage looked at the frame and left it alone, with its own reason
    Combined = 2, // the stage's work rode on another stage's pass (the shadow-mapping lift on the contact pass's combine)
    Held = 3,     // the stage bound its last whole answer: an earlier frame's picture, neither a run nor a decline
    Ran = 4
};

constexpr StageOutcome StageMerge(StageOutcome current, StageOutcome incoming) noexcept
{
    return uint8_t(incoming) > uint8_t(current) ? incoming : current;
}

struct StageRecord
{
    StageOutcome outcome = StageOutcome::None;
    uint32_t ran = 0, held = 0, declined = 0, combined = 0; // how many times the capture frame reached the stage with each outcome
    std::string reason;                                     // the newest reason of the strongest outcome that carries one
    uint32_t ordinal = 0;                                   // the frame position (the draw ordinal) of the stage's first report
    bool images[2] { false, false };                        // an in / out image was written for it (or named as another file's again)
    bool attempted[2] { false, false };                     // the first run's in / out picture was asked for: later runs of the frame add no more
};

constexpr const char* StageOutcomeName(StageOutcome outcome) noexcept
{
    switch (outcome)
    {
    case StageOutcome::Ran: return "ran";
    case StageOutcome::Held: return "held";
    case StageOutcome::Combined: return "combined";
    case StageOutcome::Declined: return "declined";
    default: return "not reached this frame";
    }
}

// Notes one report into the record. The reason is kept for the outcomes that carry one and only while that outcome is the record's strongest.
inline void StageNote(StageRecord& record, StageOutcome outcome, std::string_view reason, uint32_t ordinal)
{
    if (outcome == StageOutcome::None)
        return;
    if (record.outcome == StageOutcome::None)
        record.ordinal = ordinal;
    switch (outcome)
    {
    case StageOutcome::Ran: ++record.ran; break;
    case StageOutcome::Held: ++record.held; break;
    case StageOutcome::Combined: ++record.combined; break;
    case StageOutcome::Declined: ++record.declined; break;
    default: break;
    }
    const StageOutcome merged = StageMerge(record.outcome, outcome);
    if (merged == outcome && !reason.empty())
        record.reason = std::string(reason);
    else if (merged != record.outcome)
        record.reason.clear();
    record.outcome = merged;
}

// The one line of facts.txt a stage gets, whether or not it ran: "# stage 06 shafts: ran | frame position 5123 | values ...". A line starts
// with '#', so the dump's parsers skip it. `armed` is the stage's own armed state at the capture frame, for the stage nothing reached.
inline std::string StageStatusLine(StageId id, const StageRecord& record, bool armed, std::string_view values)
{
    char number[8];
    std::snprintf(number, sizeof number, "%02u", StageNumber(id));
    std::string text = std::string("# stage ") + number + " " + StageInfoOf(id).name + ": ";
    switch (record.outcome)
    {
    case StageOutcome::Ran:
        text += "ran";
        if (record.ran > 1)
            text += " (" + std::to_string(record.ran) + " times)";
        break;
    case StageOutcome::Held:
        text += "held: the stage's last whole answer (an earlier frame's picture) was bound";
        break;
    case StageOutcome::Combined:
        text += "combined: " + (record.reason.empty() ? std::string("ridden on another stage's pass") : record.reason);
        break;
    case StageOutcome::Declined:
        text += "declined: " + (record.reason.empty() ? std::string("no reason given") : record.reason);
        if (record.declined > 1)
            text += " (" + std::to_string(record.declined) + " times)";
        break;
    default: text += armed ? "not reached this frame" : "not armed"; break;
    }
    if (record.outcome != StageOutcome::None)
        text += " | frame position " + std::to_string(record.ordinal);
    text += " | images ";
    text += !StageInfoOf(id).images ? "none (this stage makes no image)"
            : (record.images[0] || record.images[1])
                ? std::string(record.images[0] ? "in" : "") + (record.images[0] && record.images[1] ? "+" : "") + (record.images[1] ? "out" : "")
                : "none";
    if (!values.empty())
        text += " | values " + std::string(values);
    text += "\n";
    return text;
}

// ---- the files -------------------------------------------------------------------------------------------------------------------------
// One written image: the dump's own CSV line is written by the existing writer (so every parser of facts.txt reads it unchanged); this
// comment follows it with what the CSV cannot carry: the stage, the port, the format the view sampled it in (the mask is a typeless texture
// sampled through a typed view), the window the stage reported and the box that was copied.
inline std::string StageImageNote(std::string_view name, StageId id, StagePort port, std::string_view viewFormat, float windowX, float windowY,
                                  float windowWidth, float windowHeight, const StageBox& box, uint32_t textureWidth, uint32_t textureHeight)
{
    char text[512];
    std::snprintf(text, sizeof text,
                  "# stage-image %.*s stage=%s port=%s view_format=%.*s window=%g,%g,%g,%g box=%u,%u,%u,%u texture=%ux%u%s\n", int(name.size()),
                  name.data(), StageInfoOf(id).name, port == StagePort::In ? "in" : "out", int(viewFormat.size()), viewFormat.data(),
                  double(windowX), double(windowY), double(windowWidth), double(windowHeight), box.left, box.top, box.right, box.bottom,
                  textureWidth, textureHeight, box.whole ? " window_unknown=1" : "");
    return text;
}

// The comment that follows a reflections A/B picture (ssr-ab-before / ssr-ab-engine / ssr-ab-ours): the draw it was made at (its frame position),
// the pixel shader's CRC, the window the draw worked in, the box that was copied and the texture it was cut from. One pattern for all three.
inline std::string AbImageNote(std::string_view name, uint32_t ordinal, uint32_t psCrc, std::string_view format, float windowX, float windowY,
                               float windowWidth, float windowHeight, const StageBox& box, uint32_t textureWidth, uint32_t textureHeight)
{
    char text[512];
    std::snprintf(text, sizeof text,
                  "# ssr-ab-image %.*s frame_position=%u ps_crc=0x%08X format=%.*s window=%g,%g,%g,%g box=%u,%u,%u,%u texture=%ux%u%s\n",
                  int(name.size()), name.data(), ordinal, psCrc, int(format.size()), format.data(), double(windowX), double(windowY),
                  double(windowWidth), double(windowHeight), box.left, box.top, box.right, box.bottom, textureWidth, textureHeight,
                  box.whole ? " window_unknown=1" : "");
    return text;
}

// An input that is another stage's output (or the chain's first input again): no second file, a reference the analysis tool resolves.
inline std::string StageReferenceNote(std::string_view name, std::string_view sameAs)
{
    return "# stage-image " + std::string(name) + " same_as=" + std::string(sameAs) + "\n";
}

// A stage image the dump could not take (the budget, the format, the copy): a skipped file in the dump's own shape, so the readers that know
// "# skipped" list it with its reason.
inline std::string StageSkipNote(std::string_view name, std::string_view reason)
{
    return "# skipped " + std::string(name) + ".bin: " + std::string(reason) + "\n";
}

// ---- the capture frame's measured numbers (the late values) -------------------------------------------------------------------------------------
// The stage's key values are read once where the capture window opens, before the chain's stages have run, so a number the stage measures at its
// own run (the frame's white point and fade, the sun's pixel, the tint, a depth-of-field focus) reaches the stage line through the late hooks
// (StageDump.h) and is appended to the snapshot. The line's format does not change: more words in the same `| values` text.
struct StageFrameValues
{
    bool haveWhite = false;
    float white = 0.0f, fade = 1.0f;
    bool haveSun = false;
    float sunX = 0.0f, sunY = 0.0f; // the sun in window pixels
    bool haveTint = false;
    float tint[3] { 0.0f, 0.0f, 0.0f };
};

inline std::string FormatFrameValues(const StageFrameValues& values)
{
    std::string text;
    char piece[96];
    const auto add = [&text, &piece] {
        if (!text.empty())
            text += ' ';
        text += piece;
    };
    if (values.haveWhite)
    {
        std::snprintf(piece, sizeof piece, "white=%.3f fade=%.2f", double(values.white), double(values.fade));
        add();
    }
    if (values.haveSun)
    {
        std::snprintf(piece, sizeof piece, "sunPx=%.1f,%.1f", double(values.sunX), double(values.sunY));
        add();
    }
    if (values.haveTint)
    {
        std::snprintf(piece, sizeof piece, "tint=%.2f,%.2f,%.2f", double(values.tint[0]), double(values.tint[1]), double(values.tint[2]));
        add();
    }
    return text;
}

// The snapshot of the keys and the late numbers, one space between.
inline std::string StageValuesWithLate(std::string_view snapshot, std::string_view late)
{
    if (late.empty())
        return std::string(snapshot);
    if (snapshot.empty())
        return std::string(late);
    return std::string(snapshot) + " " + std::string(late);
}

// The comment that follows a depth file's facts line: the same attributes the stage images carry, so one pattern reads both. The depth is cropped
// to the window the fog post's draw worked in.
inline std::string DepthImageNote(std::string_view name, std::string_view viewFormat, float windowX, float windowY, float windowWidth, float windowHeight,
                                  const StageBox& box, uint32_t textureWidth, uint32_t textureHeight)
{
    char text[512];
    std::snprintf(text, sizeof text, "# depth-image %.*s view_format=%.*s window=%g,%g,%g,%g box=%u,%u,%u,%u texture=%ux%u%s\n", int(name.size()), name.data(),
                  int(viewFormat.size()), viewFormat.data(), double(windowX), double(windowY), double(windowWidth), double(windowHeight), box.left, box.top,
                  box.right, box.bottom, textureWidth, textureHeight, box.whole ? " window_unknown=1" : "");
    return text;
}

inline const char* StageFactsLegend()
{
    return "# the stage images: stage-<NN>-<name>-in.bin / -out.bin are the picture a stage received and the picture it produced, cropped to its\n"
           "# render window in the texture's own format (a '# stage-image' line after each file names the stage, the port, the view format\n"
           "# and the box; 'same_as' means the input is the named file again); one '# stage NN name:' line per stage says ran / held /\n"
           "# combined / declined: <reason> / not armed / not reached this frame\n";
}
} // namespace ext::ffxiv::diag
