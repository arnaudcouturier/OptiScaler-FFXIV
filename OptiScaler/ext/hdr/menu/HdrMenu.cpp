#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/game/Game.h"
#include "ext/hdr/Hdr10.h"
#include "ext/hdr/screenshot/HdrScreenshot.h"
#include "ext/hdr/scene/NrSceneInput.h"

#include <imgui/imgui.h>

namespace
{
template <class Setting> void ResetHdr(const char* id, Setting& setting)
{
    ImGui::SameLine();
    ImGui::PushID(id);
    if (ImGui::SmallButton("Reset"))
        setting = std::nullopt;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Restore this setting to its default. Save Settings to keep the reset.");
    ImGui::PopID();
}

void* ScreenshotKeyOption() { return &ext::Cfg().hdr.screenshotKey; }
void ScreenshotPressed(ext::menu::Ctx&) { ext::hdr::screenshot::Request(); }

void DrawHdrCard(ext::menu::Ctx& ctx)
{
    auto& hdr = ext::Cfg().hdr;
    auto& nr = ext::Cfg().nr;
    ext::menu::SectionTitle("FFXIV HDR10 output");
    ImGui::TextDisabled("(experimental) HDR10 output for the FFXIV build match.");

    bool enabled = hdr.ffxivHdr.value_or_default();
    if (ImGui::Checkbox("Enable OptiScaler HDR (restart)", &enabled))
        hdr.ffxivHdr = enabled;
    ResetHdr("FfxivHDR", hdr.ffxivHdr);

    int mode = hdr.mode.value_or_default() == 1 ? 1 : 0;
    const char* modes[] = { "SDR expansion (original)", "RenoDX scene HDR (experimental)" };
    if (ImGui::Combo("HDR method", &mode, modes, 2))
        hdr.mode = mode;
    ResetHdr("FfxivHDRMode", hdr.mode);

    ImGui::BeginDisabled(mode != 1);
    bool reshadeHighlights = hdr.reshadeHighlights.value_or_default();
    if (ImGui::Checkbox("Preserve HDR highlights through ReShade (experimental)", &reshadeHighlights))
        hdr.reshadeHighlights = reshadeHighlights;
    ResetHdr("FfxivHDRReShadeHighlights", hdr.reshadeHighlights);
    ext::menu::Tooltip("Uses captured scene highlight brightness with the final SDR effect colours.\n"
                       "Relaxes colour-difference rejection only where both images are bright, and preserves post-effect colour ratios.\n"
                       "SDR ReShade input is unchanged. Overlapping bright HUD elements may also brighten. Detail clipped by a preset cannot be recovered.\n"
                       "Applies immediately; save settings to keep your choice.");

    bool dlssSceneInput = hdr.dlssSceneInput.value_or_default();
    if (ImGui::Checkbox("Use scene HDR input for DLSS (experimental, restart)", &dlssSceneInput))
        hdr.dlssSceneInput = dlssSceneInput;
    ResetHdr("FfxivHDRDLSSSceneInput", hdr.dlssSceneInput);
    ext::menu::Tooltip("Feeds linear FP16 scene highlights through DLSS w/Dx12, then restores SDR for ReShade.\n"
                       "Changes take effect when the DLSS feature is recreated; restart for a consistent comparison.\n"
                       "Missing same-frame captures use linear SDR. Existing SDR expansion and NR input controls remain independent.");

    bool nrSceneInput = hdr.nrSceneInput.value_or_default();
    if (ImGui::Checkbox("Use scene HDR input for NR (experimental)", &nrSceneInput))
        hdr.nrSceneInput = nrSceneInput;
    ResetHdr("FfxivHDRNRSceneInput", hdr.nrSceneInput);
    ext::menu::Tooltip("Feeds preserved linear scene brightness through NR's reversible HDR proxy. Supports ordinary NR before upscaling, PreSR and alternating NR anchors.\n"
                       "The private DLSS residual carrier and ReShade stay SDR. Exposure is already applied; NR normalization is 1.\n"
                       "Missing or incompatible scene data uses SDR NR and resets affected history. Save Settings to retain your choice.");
    if (nrSceneInput)
    {
        auto status = ext::hdr::nrSceneState.load(std::memory_order_relaxed);
        if (!nr.enabled.value_or_default())
            status = ext::nr::SceneState::NRDisabled;
        else if (mode != 1 || !ext::hdr::Active())
            status = ext::nr::SceneState::HDRInactive;
        else if (!nr.runBeforeSr.value_or_default() && !nr.deferredDlss.value_or_default())
            status = ext::nr::SceneState::RequiresBeforeSR;
        ImGui::TextWrapped("NR scene input: %s", ext::hdr::NrSceneText(status));
    }
    ImGui::EndDisabled();

    ImGui::TextWrapped("Scene HDR preserves a parallel FP16 scene through RenoDX shaders. ReShade keeps SDR input; NR can optionally use the captured scene. Final output remains 10-bit PQ for DLSS-G compatibility. Missing scene data falls back to paper-white SDR, without invented highlights.");
    ImGui::TextWrapped("%s", ext::hdr::Status().c_str());
    ImGui::TextWrapped("Requires Windows HDR. Disable RTX HDR and Auto HDR. Supports DLSS-G, XeFG, or FG off. The original expansion mode applies its curve to the HUD too. XeFG extracts the HUD from the HDR image; separate SDR UI overlays and HUD comparison are bypassed.");

    float peak = hdr.peak.value_or_default();
    if (ImGui::SliderFloat("Peak brightness (nits)", &peak, 400, 4000, "%.0f"))
        hdr.peak = peak;
    ResetHdr("FfxivHDRPeak", hdr.peak);
    float paper = hdr.paper.value_or_default();
    if (ImGui::SliderFloat("Paper white (nits)", &paper, 80, 400, "%.0f"))
        hdr.paper = paper;
    ResetHdr("FfxivHDRPaper", hdr.paper);

    int screenshotFormat = hdr.screenshotFormat.value_or_default() == 1 ? 1 : 0;
    const char* screenshotFormats[] = { "SDR PNG (sharing)", "HDR PNG" };
    if (ImGui::Combo("Screenshot format", &screenshotFormat, screenshotFormats, 2))
        hdr.screenshotFormat = screenshotFormat;
    ResetHdr("FfxivHDRScreenshotFormat", hdr.screenshotFormat);
    ext::menu::Tooltip("Saves one PNG to game/OptiScaler/Screenshots while OptiHDR is active. SDR preserves the image before OptiHDR, including ReShade. HDR captures OptiHDR output and needs a compatible viewer.\n"
                       "The separate Companion nameplate window is not captured. FFXIV's original screenshot function is unchanged.");

    ImGui::BeginDisabled(screenshotFormat != 1);
    bool screenshotMenu = hdr.screenshotMenu.value_or_default();
    if (ImGui::Checkbox("Include OptiScaler menu", &screenshotMenu))
        hdr.screenshotMenu = screenshotMenu;
    ResetHdr("FfxivHDRScreenshotMenu", hdr.screenshotMenu);
    ext::menu::Tooltip("HDR PNG: capture the presented image after drawing the visible OptiScaler menu, including its HDR colours. Useful for settings comparisons.\n"
                       "With the menu hidden, screenshots use the normal clean capture. Requires the overlay menu. SDR PNG captures remain unchanged. The separate Companion window is not captured.");
    bool screenshotIccOnly = hdr.screenshotIccOnly.value_or_default();
    if (ImGui::Checkbox("HDR PNG: ICC only (sharing)", &screenshotIccOnly))
        hdr.screenshotIccOnly = screenshotIccOnly;
    ResetHdr("FfxivHDRScreenshotIccOnly", hdr.screenshotIccOnly);
    ext::menu::Tooltip("Embeds a BT.2100 PQ ICC profile and omits the outer PNG cICP chunk. HDR pixels are unchanged.\n"
                       "Experimental compatibility option for sharing; Discord support must be checked on the receiving device.\n"
                       "Windows Photos and Explorer may display this differently. Uses a CC0 PQ profile, not Apple's exact profile.\n"
                       "SDR screenshots and the live game are unaffected.");
    ImGui::EndDisabled();
    ImGui::TextWrapped("Optional screenshot key: one key; Escape cancels, Backspace unbinds. Save Settings to keep your choice.");
    ImGui::TextWrapped("%s", ext::hdr::screenshot::Status().c_str());

    float expansion = hdr.expansion.value_or_default();
    if (ImGui::SliderFloat("Highlight expansion", &expansion, 0, 1, "%.2f"))
        hdr.expansion = expansion;
    ResetHdr("FfxivHDRExpansion", hdr.expansion);
    ext::menu::Tooltip("In SDR expansion mode, controls highlight expansion for the complete image.\n"
                       "In scene HDR mode, maps SDR-authored UI and changed effect pixels into HDR; captured world highlights retain their own scene brightness.\n"
                       "Applies immediately. No additional DLSS evaluation is needed.");

    float contrast = hdr.contrast.value_or_default();
    if (ImGui::SliderFloat("Contrast", &contrast, 0.5f, 1.5f, "%.2fx"))
        hdr.contrast = contrast;
    ResetHdr("FfxivHDRContrast", hdr.contrast);
    ext::menu::Tooltip("Applies immediately. 1.00 preserves the original contrast.\n"
                       "Higher values deepen shadows and lift highlights around middle grey.\n"
                       "Black, white and the HDR peak limit are preserved. Applies to the game HUD too.");

    float saturation = hdr.saturation.value_or_default();
    if (ImGui::SliderFloat("Saturation##OptiHDR", &saturation, 0.0f, 2.0f, "%.2fx"))
        hdr.saturation = saturation;
    ResetHdr("FfxivHDRSaturation", hdr.saturation);
    ext::menu::Tooltip("Applies immediately. 1.00 is unchanged; 0 removes colour.\n"
                       "Adjusts colour strength while preserving luminance and respecting the HDR output range.");

    float vibrance = hdr.vibrance.value_or_default();
    if (ImGui::SliderFloat("Vibrance##OptiHDR", &vibrance, -1.0f, 1.0f, "%.2f"))
        hdr.vibrance = vibrance;
    ResetHdr("FfxivHDRVibrance", hdr.vibrance);
    ext::menu::Tooltip("Applies immediately. 0 is unchanged. Positive values favour muted colours; negative values soften them.\n"
                       "Both colour controls also affect the game HUD.");

    if (ctx.config->LoadReShade.value_or_default())
        ImGui::TextWrapped("ReShade compatibility: SDR presets run before OptiHDR. Their colour adjustments combine with these controls; detail clipped by a preset cannot be restored.");
    if (mode == 0)
        ImGui::TextWrapped("0 preserves SDR brightness at paper white; 1 expands white highlights to the peak. Black stays black. This cannot recover detail already clipped by the game.");
    else
        ImGui::TextWrapped("Scene HDR uses actual pre-clipping scene brightness. SDR ReShade presets remain compatible; strong spatial effects or changed overlays can suppress the HDR residual in affected pixels. In-game validation is required.");
}

bool HdrCardVisible(const ext::menu::Ctx&) { return ext::game::Current().hdr != nullptr; }

struct HdrMenuRegistration
{
    HdrMenuRegistration()
    {
        ext::menu::Register({ "ffxiv_hdr", "FFXIV", "FFXIV HDR10 output", DrawHdrCard, "fg_selection",
                              HdrCardVisible });
        ext::menu::RegisterShortcut({ "OptiHDR screenshot", 15, ScreenshotKeyOption, "OptiHDR screenshot",
                                      ScreenshotPressed });
    }
} registration;
} // namespace
