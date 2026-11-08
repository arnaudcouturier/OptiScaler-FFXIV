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

namespace ext::ffxiv::diag
{
// The render window a stage reports with its images: x, y, width, height of the viewport it worked in (all zero when it has none).
struct StageWindow
{
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
};

using StageImageFn = void (*)(StageId id, StagePort port, ID3D11DeviceContext* context, ID3D11ShaderResourceView* image,
                              const StageWindow& window);
using StageStatusFn = void (*)(StageId id, StageOutcome outcome, const char* reason);
using StageArmedFn = bool (*)();                // the stage's own armed state, asked at the capture frame's write
using StageValuesFn = std::string (*)();        // the stage's key values as one line of "name=value" words
using StageLateFn = void (*)(StageId id, const char* text);                      // numbers the stage measured at its run, appended to its values text
using StageProbeFormatFn = std::string (*)(const float* values, size_t count);   // how a probed texel's floats read as words
using StageProbeFn = void (*)(StageId id, ID3D11DeviceContext* context, ID3D11ShaderResourceView* texel, StageProbeFormatFn format);

// Installs the two entry points above into the stages' inline calls: called once from the draw diagnostic's own static initialiser.
void InstallStageHooks();

namespace detail
{
inline std::atomic<uint32_t> g_stageOpen { 0 }; // kStagePreBit | kStageChainBit while a capture frame is open
inline StageImageFn g_stageImage = nullptr;
inline StageStatusFn g_stageStatus = nullptr;
inline StageArmedFn g_stageArmed[kStageCount] {};
inline StageValuesFn g_stageValues[kStageCount] {};
inline StageLateFn g_stageLate = nullptr;
inline StageProbeFn g_stageProbe = nullptr;
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
    if (!StageOpen(id) || detail::g_stageStatus == nullptr)
        return;
    detail::g_stageStatus(id, outcome, reason);
}

// A stage that ran: its input and its output, in that order, and the run itself. The stage after it has not been handed the answer yet.
inline void StageRan(StageId id, ID3D11DeviceContext* context, ID3D11ShaderResourceView* in, ID3D11ShaderResourceView* out,
                     const StageWindow& window)
{
    if (!StageOpen(id))
        return;
    StageImage(id, StagePort::In, context, in, window);
    StageImage(id, StagePort::Out, context, out, window);
    StageReport(id, StageOutcome::Ran);
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

// Registers a stage's armed probe and its values text (called once from the stage's own static initialiser: two stores, no allocation).
inline void StageRegister(StageId id, StageArmedFn armed, StageValuesFn values)
{
    detail::g_stageArmed[size_t(id)] = armed;
    detail::g_stageValues[size_t(id)] = values;
}
} // namespace ext::ffxiv::diag
