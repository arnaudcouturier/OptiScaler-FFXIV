#pragma once
// HDR config keys (docs/SEAMS.md section 3.4). Owner: C8, C9, C10. Section [HDR], key names identical to the reference
// build (upstream's own [HDR] keys ForceHDR/UseHDR10/SkipColorSpace stay in Config.h). Use:
// ext::Cfg().hdr.ffxivHdr.value_or_default(). Retired keys HDR.MenuNits and HDR.ScreenshotHotkey are deleted on save.

#include <Config.h>

namespace ext::config
{
class Binder;
}

namespace ext
{
struct HdrConfig
{
    // C8: OptiHDR, HDR10 output through the DX11 bridge presenter. Restart after changing.
    CustomOptional<bool> ffxivHdr { false }; // FfxivHDR
    // C8/C10: 0 SDR expansion, 1 RenoDX scene HDR
    CustomOptional<int> mode { 0 }; // Mode
    // C10: preserve measured scene highlights through ReShade's grading
    CustomOptional<bool> reshadeHighlights { false }; // ReShadeHighlights
    // C10: NR runs on the linear HDR scene
    CustomOptional<bool> nrSceneInput { false }; // NRSceneInput
    // C10: DLSS (D3D12 path of the bridge) runs on the HDR scene. Restart after changing.
    CustomOptional<bool> dlssSceneInput { false }; // DLSSSceneInput
    // C8: output mapping
    CustomOptional<float> peak { 1100.0f };      // PeakNits
    CustomOptional<float> paper { 203.0f };      // PaperWhiteNits
    CustomOptional<float> expansion { 0.5f };    // HighlightExpansion
    CustomOptional<float> contrast { 1.0f };     // Contrast
    CustomOptional<float> saturation { 1.0f };   // Saturation
    CustomOptional<float> vibrance { 0.0f };     // Vibrance
    // C9: HDR PNG / ICC screenshot
    CustomOptional<int> screenshotKey { UnboundKey };      // ScreenshotKey (saved as hex when > 0)
    CustomOptional<bool> screenshotIccOnly { false };      // ScreenshotIccOnly
    CustomOptional<int> screenshotFormat { 0 };            // ScreenshotFormat: 0 SDR PNG, 1 HDR PNG
    // F4 (Nickle's "Include OptiScaler menu"): HDR PNG taken after the visible menu was drawn onto the image
    CustomOptional<bool> screenshotMenu { false };         // ScreenshotMenu

    void Bind(config::Binder& b);
    void PostLoad();               // end of config::Load: installs the NR scene hooks (once)
    void Retire(CSimpleIniA& ini); // config::Save: drops the retired keys
};
} // namespace ext
