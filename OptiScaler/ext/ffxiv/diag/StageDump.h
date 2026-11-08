#pragma once
// The per-stage image dump, stage side (the rules are in StageDumpFormat.h, the runtime in FfxivDrawDiag.cpp).
//
// A stage of this fork calls these AFTER its own pass returned and its explicit-state Guard has gone out of scope, so the copy the draw
// diagnostic queues is a plain CopySubresourceRegion on the context the pass ran on (it sets no pipeline state), and BEFORE the stage hands
// its answer to the next one (ComposeChainNext), which keeps the copies in chain order. Nothing here changes what a stage renders: the
// calls read the stage's own output.
//
// Cost when no capture is open: every entry point is ONE relaxed load of the open mask and a branch. The draw diagnostic sets the mask for
// the capture frame only; the function pointers stay null in every build that does not link the diagnostic (the host tests of the stages),
// which is the same answer as a closed mask.

#include "ext/ffxiv/diag/StageDumpFormat.h"

#include <atomic>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

namespace ext::ffxiv::diag
{
// The render window a stage reports with its images: x, y, width, height of the viewport it worked in (all zero when it has none).
struct StageWindow
{
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    float white = 0.0f; // the frame's white point as the stage read it (the effect meter's display terms); 0 = the stage has none of its own
};

using StageImageFn = void (*)(StageId id, StagePort port, ID3D11DeviceContext* context, ID3D11ShaderResourceView* image,
                              const StageWindow& window);
using StageStatusFn = void (*)(StageId id, StageOutcome outcome, const char* reason);
using StageArmedFn = bool (*)();                // the stage's own armed state, asked at the capture frame's write
using StageValuesFn = std::string (*)();        // the stage's key values as one line of "name=value" words
using StageLateFn = void (*)(StageId id, const char* text);                      // numbers the stage measured at its run, appended to its values text
using StageProbeFormatFn = std::string (*)(const float* values, size_t count);   // how a probed texel's floats read as words
using StageProbeFn = void (*)(StageId id, ID3D11DeviceContext* context, ID3D11ShaderResourceView* texel, StageProbeFormatFn format);
// The reflections' frame-exact A/B (engine/SsrAb.cpp): the capture's offer of a reader draw ("try this draw"), the A/B's answer that it kept one, a named
// picture of a texture the A/B holds (its private target, or the game's own after the real draw) and one comment line for facts.txt.
using AbTakeFn = bool (*)(uint32_t* framePosition, uint32_t* tryIndex); // true while no draw was kept and tries are left: the draw's frame position and try number
using AbAcceptFn = void (*)();                                          // the A/B kept the draw it was offered: no more offers this capture
using AbImageFn = void (*)(const char* name, ID3D11DeviceContext* context, ID3D11Texture2D* texture, const StageWindow& window, uint32_t psCrc);
using AbNoteFn = void (*)(const char* text);
// The effect meter's two entries (ext/ffxiv/meter/Meter.cpp). They are called on EVERY frame, not only on the capture frame: the meter counts what each stage did
// per rendered frame and measures its picture on a sampled frame. Null in every build that does not link the meter (the host tests of the stages): the same
// answer as a meter that is off, and the cost is one load of a null pointer.
using MeterRanFn = void (*)(StageId id, ID3D11DeviceContext* context, ID3D11ShaderResourceView* in, ID3D11ShaderResourceView* out, const StageWindow& window);
using MeterReportFn = void (*)(StageId id, StageOutcome outcome, const char* reason);

// Installs the two entry points above into the stages' inline calls: called once from the draw diagnostic's own static initialiser.
void InstallStageHooks();

// The engine light lamp arm (ENG2, ext/ffxiv/enginelight): one csv row per point-light volume draw of the capture frame, appended to lamplights.csv.
using LampRowSinkFn = void (*)(const std::string& csvRow);

namespace detail
{
inline LampRowSinkFn g_lampRow = nullptr; // set by the draw diagnostic; null in every build that does not link it
inline std::atomic<uint32_t> g_stageOpen { 0 }; // kStagePreBit | kStageChainBit while a capture frame is open
inline StageImageFn g_stageImage = nullptr;
inline StageStatusFn g_stageStatus = nullptr;
inline StageArmedFn g_stageArmed[kStageCount] {};
inline StageValuesFn g_stageValues[kStageCount] {};
inline StageLateFn g_stageLate = nullptr;
inline StageProbeFn g_stageProbe = nullptr;
inline AbTakeFn g_abTake = nullptr;
inline AbAcceptFn g_abAccept = nullptr;
inline AbImageFn g_abImage = nullptr;
inline AbNoteFn g_abNote = nullptr;
inline MeterRanFn g_meterRan = nullptr;
inline MeterReportFn g_meterReport = nullptr;
inline std::atomic<float> g_frameWhite { 0.0f }; // the white point an earlier stage noted this capture frame (0 = none)
} // namespace detail

// Whether the capture frame is open for this stage's group. ONE relaxed load.
inline bool StageOpen(StageId id) { return (detail::g_stageOpen.load(std::memory_order_relaxed) & StageBitOf(id)) != 0; }

// One image of a stage: its input (before it ran) or its output. `image` is the view the stage handed to / received from the next link.
inline void StageImage(StageId id, StagePort port, ID3D11DeviceContext* context, ID3D11ShaderResourceView* image, const StageWindow& window)
{
    if (!StageOpen(id) || detail::g_stageImage == nullptr || context == nullptr || image == nullptr)
        return;
    detail::g_stageImage(id, port, context, image, window);
}

// What happened to the capture frame at this stage. `reason` is the stage's own text (a decline's name), null for a run.
inline void StageReport(StageId id, StageOutcome outcome, const char* reason = nullptr)
{
    if (detail::g_meterReport != nullptr)
        detail::g_meterReport(id, outcome, reason); // every frame, before the capture's gate
    if (!StageOpen(id) || detail::g_stageStatus == nullptr)
        return;
    detail::g_stageStatus(id, outcome, reason);
}

// A stage that ran: its input and its output, in that order, and the run itself. The stage after it has not been handed the answer yet.
inline void StageRan(StageId id, ID3D11DeviceContext* context, ID3D11ShaderResourceView* in, ID3D11ShaderResourceView* out,
                     const StageWindow& window)
{
    if (detail::g_meterRan != nullptr)
        detail::g_meterRan(id, context, in, out, window); // every frame, before the capture's gate; measures only on its sampled frame
    if (!StageOpen(id))
        return;
    StageImage(id, StagePort::In, context, in, window);
    StageImage(id, StagePort::Out, context, out, window);
    StageReport(id, StageOutcome::Ran);
}

// A frame the stage looked at and left alone for a reason that never reaches the capture frame's status line (the night grade's weight 0 is reported in the capture
// only while the dump is open): the meter's count and nothing else. `reason` is a string literal or a text the caller already has; the meter keys it with its
// numbers folded.
inline void MeterSkipped(StageId id, const char* reason)
{
    if (detail::g_meterReport != nullptr)
        detail::g_meterReport(id, StageOutcome::Declined, reason);
}

// Numbers the stage measured at its run: free text for the stage's own line (the capture frame only; the caller builds the text only while the
// capture is open, with StageOpen). Several calls add words in order.
inline void StageNoteText(StageId id, const std::string& text)
{
    if (!StageOpen(id) || detail::g_stageLate == nullptr)
        return;
    detail::g_stageLate(id, text.c_str());
}

// The frame's white point and fade, the sun's pixel and the tint a stage used, for its line. The white point is also kept for the chain's later stages
// (depth of field has none of its own and names the one an earlier stage noted).
inline void StageNoteFrame(StageId id, const StageFrameValues& values)
{
    if (!StageOpen(id) || detail::g_stageLate == nullptr)
        return;
    if (values.haveWhite)
        detail::g_frameWhite.store(values.white, std::memory_order_relaxed);
    detail::g_stageLate(id, FormatFrameValues(values).c_str());
}

inline float StageFrameWhite() { return detail::g_frameWhite.load(std::memory_order_relaxed); }

// One texel of a texture the stage keeps on the GPU (a state it never reads back): on the capture frame the runtime copies it into its own staging
// texture and maps it where it writes the dump, then `format` turns its floats into words for the stage's line.
inline void StageProbe(StageId id, ID3D11DeviceContext* context, ID3D11ShaderResourceView* texel, StageProbeFormatFn format)
{
    if (!StageOpen(id) || detail::g_stageProbe == nullptr || context == nullptr || texel == nullptr || format == nullptr)
        return;
    detail::g_stageProbe(id, context, texel, format);
}

// ---- the reflections' A/B (engine/SsrAb.cpp) ---------------------------------------------------------------------------------------------------
// Each entry tests the reflections stage's open bit first, so a frame that is not the capture frame pays ONE relaxed load and a branch.

// True while the capture frame is open, the dump is armed, no draw was kept yet and the capture's tries are not used up: the caller's draw is then offered
// to the A/B (its frame position and its try number, from 1). Each true answer is one try.
inline bool AbTake(uint32_t* framePosition, uint32_t* tryIndex)
{
    if (!StageOpen(StageId::Ssr) || detail::g_abTake == nullptr)
        return false;
    return detail::g_abTake(framePosition, tryIndex);
}

// The A/B kept the draw it was offered: it is not offered another this capture.
inline void AbAccept()
{
    if (!StageOpen(StageId::Ssr) || detail::g_abAccept == nullptr)
        return;
    detail::g_abAccept();
}

// A picture of a texture the A/B holds, copied now (the copy is queued behind the draws made so far), cropped to the draw's window.
inline void AbImage(const char* name, ID3D11DeviceContext* context, ID3D11Texture2D* texture, const StageWindow& window, uint32_t psCrc)
{
    if (!StageOpen(StageId::Ssr) || detail::g_abImage == nullptr || context == nullptr || texture == nullptr)
        return;
    detail::g_abImage(name, context, texture, window, psCrc);
}

// One comment line for the capture's facts.txt (a decline's reason, or what the A/B did).
inline void AbNote(const char* text)
{
    if (!StageOpen(StageId::Ssr) || detail::g_abNote == nullptr || text == nullptr)
        return;
    detail::g_abNote(text);
}

// Registers a stage's armed probe and its values text (called once from the stage's own static initialiser: two stores, no allocation).
inline void StageRegister(StageId id, StageArmedFn armed, StageValuesFn values)
{
    detail::g_stageArmed[size_t(id)] = armed;
    detail::g_stageValues[size_t(id)] = values;
}
} // namespace ext::ffxiv::diag
