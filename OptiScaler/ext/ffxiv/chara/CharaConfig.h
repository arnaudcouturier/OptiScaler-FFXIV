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
    CustomOptional<bool> previewNrTrace { false };      // [HDR] PreviewNRTrace: trace log lines for preview NR work

    template <class Binder> void Bind(Binder& b)
    {
        b.Add("FFXIV", "PreviewDLSS", previewDlss);
        b.Add("FFXIV", "PreviewAA", previewAA);
        b.Add("FFXIV", "PreviewUpscale", previewUpscale);
        b.Add("FFXIV", "PreviewGuideCapture", previewGuideCapture);
        b.Add("HDR", "PreviewNRTrace", previewNrTrace);
    }
};
} // namespace ext::ffxiv::chara
