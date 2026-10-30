#pragma once
// Config keys of the character preview feature (CharaView). Names and sections are the ones used by the build this
// feature was reconstructed from, so ini files move between builds (docs/RECONSTRUCTION-REFERENCE.md, CharaView).
//
// Depends on <Config.h> only: the config tests compile ExtConfig.cpp against stub headers. `Bind` is a template on the
// binder so this header does not need the binder's definition; ext::FfxivConfig::Bind (ext/core/ExtConfig.cpp) calls it.

#include <Config.h>

namespace ext::ffxiv::chara
{
struct CharaConfig
{
    CustomOptional<bool> previewDlss { false };         // [FFXIV] PreviewDLSS: isolated DLSS for character previews
    CustomOptional<bool> previewAA { false };           // [FFXIV] PreviewAA: spatial AA of the equipment preview
    CustomOptional<bool> previewUpscale { true };       // [FFXIV] PreviewUpscale: the AA output is 2x
    CustomOptional<bool> previewGuideCapture { false }; // [FFXIV] PreviewGuideCapture: diagnostics for the DLSS path
    CustomOptional<bool> previewDebugTint { false };    // [FFXIV] PreviewDebugTint: tints the processed preview image (debug aid)
    CustomOptional<bool> previewNrTrace { false };      // [HDR] PreviewNRTrace: trace log lines for preview NR work
    CustomOptional<bool> previewNr { false };           // [FFXIV] PreviewNR: Neural Rendering for character previews (new work, not in the collaborator's build)
    CustomOptional<uint32_t> previewNrLingerSeconds { 5 }; // [FFXIV] PreviewNRLingerSeconds: how long a preview's NR model outlives its window
    CustomOptional<bool> previewNrLate { false };       // [FFXIV] PreviewNRLate: show the previous frame's NR result (no GPU wait)
    CustomOptional<bool> previewNrBeforeSr { false };   // [FFXIV] PreviewNRBeforeSR: run NR before the DLSS evaluate instead of after it (experimental; needs PreviewDLSS and PreviewNR)
    CustomOptional<bool> previewProbe { false };        // [FFXIV] PreviewProbe: debug probe that traces how the preview reaches the screen
    CustomOptional<bool> previewDebugFlood { false };   // [FFXIV] PreviewDebugFlood: replaces the sampled preview by solid magenta (debug aid)
    CustomOptional<bool> previewReplayWideShape { false }; // [FFXIV] PreviewReplayWideShape: the motion replay also takes a camera block that is a view into a larger buffer
    CustomOptional<bool> previewBudget { false };      // [FFXIV] PreviewBudget: only PreviewMaxWindows preview windows are processed
    CustomOptional<uint32_t> previewMaxWindows { 2 };  // [FFXIV] PreviewMaxWindows: how many preview windows are processed (1 to 8, read while PreviewBudget is on)

    template <class Binder> void Bind(Binder& b)
    {
        b.Add("FFXIV", "PreviewDLSS", previewDlss);
        b.Add("FFXIV", "PreviewAA", previewAA);
        b.Add("FFXIV", "PreviewUpscale", previewUpscale);
        b.Add("FFXIV", "PreviewGuideCapture", previewGuideCapture);
        b.Add("FFXIV", "PreviewDebugTint", previewDebugTint);
        b.Add("HDR", "PreviewNRTrace", previewNrTrace);
        b.Add("FFXIV", "PreviewNR", previewNr);
        b.Add("FFXIV", "PreviewNRLingerSeconds", previewNrLingerSeconds);
        b.Add("FFXIV", "PreviewNRLate", previewNrLate);
        b.Add("FFXIV", "PreviewNRBeforeSR", previewNrBeforeSr);
        b.Add("FFXIV", "PreviewProbe", previewProbe);
        b.Add("FFXIV", "PreviewDebugFlood", previewDebugFlood);
        b.Add("FFXIV", "PreviewReplayWideShape", previewReplayWideShape);
        b.Add("FFXIV", "PreviewBudget", previewBudget);
        b.Add("FFXIV", "PreviewMaxWindows", previewMaxWindows);
    }
};
} // namespace ext::ffxiv::chara
