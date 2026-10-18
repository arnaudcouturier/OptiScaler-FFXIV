#pragma once
// Config keys of the character preview feature (CharaView). Names and sections are the ones used by the build this
// feature was reconstructed from, so ini files move between builds (docs/RECONSTRUCTION-NICKLE.md, CharaView).
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
    CustomOptional<bool> previewNr { false };           // [FFXIV] PreviewNR: Neural Rendering for character previews (new work, not in Nickle's build)
    CustomOptional<uint32_t> previewNrLingerSeconds { 5 }; // [FFXIV] PreviewNRLingerSeconds: how long a preview's NR model outlives its window
    CustomOptional<bool> previewNrLate { false };       // [FFXIV] PreviewNRLate: show the previous frame's NR result (no GPU wait)

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
    }
};
} // namespace ext::ffxiv::chara
