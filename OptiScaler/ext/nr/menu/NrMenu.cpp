#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/nr/DlssNr.h"
#include "ext/nr/NrHotkeys.h"
#include "ext/nr/NrModelSize.h"
#include "ext/nr/exposure/DlssNr_ExposureScan.h"
#include "ext/nr/feature/DlssNrFeature_Dx12.h"

#include <Config.h>
#include <menu/menu_common.h>

#include <imgui/ImGuiNotify.hpp>
#include <imgui/imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>
#include <string>

// What the NR series puts into upstream's menu (docs/SEAMS.md 3.6, catalog group `menu`):
//   * the card `dlss_nr` (tab Upscaling, A5/A6/A12; the body is DlssNr_Menu.cpp)
//   * the toggle shortcut (A5): key `[DlssNr] ToggleKey`, Keybinds slot 14, toast
//   * the model-resolution shortcuts: keys `[DlssNr] ScaleUpKey`/`ScaleDownKey`, Keybinds slots 16/17, toast
//   * the enlargement shortcut: key `[DlssNr] TransferToggleKey`, Keybinds slot 18, toast
//   * the overlays: comparison labels (A6) and the exposure-scan lamp (A4)
//   * the "Neural Rendering" row of the per-shader GPU times (A1)
// Was inline in menu_common.cpp (U5 rows 2, 3, 12, 13, 15, 29, 30, 32, 34-36, 39, 40).

namespace
{
// ---- card ------------------------------------------------------------------------------------------------------
void DrawNrCard(ext::menu::Ctx& ctx) { DlssNr::RenderMenu(ctx.menuResScale); }

// ---- toggle shortcut -------------------------------------------------------------------------------------------
void* ToggleKeyOption() { return &ext::Cfg().nr.toggleKey; }

void OnTogglePressed(ext::menu::Ctx&)
{
    auto& nr = ext::Cfg().nr;

    nr.enabled = !nr.enabled.value_or_default();
    LOG_DEBUG("Neural Rendering toggle key pressed, setting DlssNrEnabled to {}", nr.enabled.value_or_default());

    // Starts the per-resource exposure scan the same frame (the F::Nr bit only latches on the ini and on an evaluate)
    if (nr.enabled.value_or_default())
        ext::SetActive(ext::F::Nr, true);

    ImGuiToast toast { ImGuiToastType::Info, 2000 };
    toast.setTitle("DLSS Neural Rendering");
    toast.setContent(nr.enabled.value_or_default() ? "On" : "Off");
    ImGui::InsertNotification(toast);
}

// ---- model-resolution and enlargement shortcuts ----------------------------------------------------------------------
void* ScaleUpKeyOption() { return &ext::Cfg().nr.scaleUpKey; }
void* ScaleDownKeyOption() { return &ext::Cfg().nr.scaleDownKey; }
void* TransferToggleKeyOption() { return &ext::Cfg().nr.transferToggleKey; }

void ShowHotkeyToast(const char* title, const std::string& content)
{
    ImGuiToast toast { ImGuiToastType::Info, 2000 };
    toast.setTitle(title);
    toast.setContent(content.c_str());
    ImGui::InsertNotification(toast);
}

// Whether a per-axis key currently overrides the uniform Model resolution: exactly the values the pass uses
// instead of it (a zero, negative or non-finite key follows the uniform one). The scale keys only step the
// uniform WorkingScale and never touch the per-axis keys, so the toast says when one of them wins.
bool PerAxisOverrides()
{
    const auto& nr = ext::Cfg().nr;
    const float x = nr.workingScaleX.value_or_default(), y = nr.workingScaleY.value_or_default();
    return (std::isfinite(x) && x > 0.0f) || (std::isfinite(y) && y > 0.0f);
}

// Shortcuts are polled on every present, menu open or closed, and their handlers run in the shortcut pass of the menu
// frame: the same render-thread pass that commits the Model resolution slider.
// One press steps the uniform Model resolution by 5% through the same rule the slider commits, so the hotkey and
// the slider write identical values. The pass rebuilds the model once after its 500 ms settle, exactly like after
// a slider change: there is no second debounce path here.
void StepModelResolution(bool up)
{
    auto& nr = ext::Cfg().nr;

    nr.workingScale = DlssNr::Hotkeys::StepWorkingScale(nr.workingScale.value_or_default(), up);
    LOG_DEBUG("NR model resolution key pressed, setting WorkingScale to {}", nr.workingScale.value_or_default());

    const int percent = (int) std::lround(nr.workingScale.value_or_default() * 100.0f);
    std::string content = std::format("Model resolution {}%", percent);
    // The pass ignores the per-axis keys while the anamorphic refusal latch holds (NrModelSize.h): say "override"
    // only when the keys are really in force, judged against the keys after this press like the pass will be.
    const DlssNr::AnamorphicLatch toastLatch = DlssNr::AnamorphicLatchState();
    if (PerAxisOverrides() &&
        !DlssNr::AnamorphicLatchHolds(toastLatch, nr.workingScaleX.value_or_default(),
                                      nr.workingScaleY.value_or_default(), nr.workingScale.value_or_default()))
        content += " (per-axis keys override)";
    ShowHotkeyToast("DLSS Neural Rendering", content);
}

void OnScaleUpPressed(ext::menu::Ctx&) { StepModelResolution(true); }
void OnScaleDownPressed(ext::menu::Ctx&) { StepModelResolution(false); }

void OnTransferTogglePressed(ext::menu::Ctx&)
{
    auto& nr = ext::Cfg().nr;

    // The same numbers the Enlargement combo is greyed from (the governor's cap included), through the same
    // function, so the key is a no-op exactly while the combo is greyed.
    const DlssNr::AxisScales scales = DlssNr::ShownEffectiveScales(nr.workingScale.value_or_default());
    const bool toggleable = DlssNr::Hotkeys::TransferToggleable(scales.x, scales.y);
    if (!toggleable)
        return;
    nr.transfer = DlssNr::Hotkeys::ToggleTransfer(nr.transfer.value_or_default(), toggleable);
    LOG_DEBUG("NR enlargement key pressed, setting Transfer to {}", nr.transfer.value_or_default());

    ShowHotkeyToast("DLSS Neural Rendering",
                     nr.transfer.value_or_default() == 1 ? "Enlargement: Matched residual" : "Enlargement: Classic");
}

// ---- overlays --------------------------------------------------------------------------------------------------

// Labels for the comparison views.
//
// Drawn straight onto the foreground draw list, not as ImGui windows -- the last attempt made them
// draggable windows and the clamping fought the split. Here each label is clipped to its own side of
// the comparison, so in the wipe the moving split reveals and hides it exactly as it does the
// pictures, and there is nothing to drag. Both wipe labels sit in the same top-left corner, each
// clipped to its side, so whichever picture currently owns that corner is the one whose label shows.
void RenderNrCompareTags()
{
    auto& nr = ext::Cfg().nr;

    const uint32_t mode = nr.compare.value_or_default();

    if (mode == 0 || !nr.compareTags.value_or_default())
        return;

    const ImVec2 screen = ImGui::GetIO().DisplaySize;

    if (screen.x < 1.0f || screen.y < 1.0f)
        return;

    const bool swap = nr.compareSwap.value_or_default();
    const float split = mode == 1 ? 0.5f : std::clamp(nr.compareSplit.value_or_default(), 0.0f, 1.0f);
    const float splitX = split * screen.x;

    const float scale = std::clamp(nr.tagScale.value_or_default(), 0.5f, 5.0f);

    // The left side is the untouched frame unless swapped -- matching the shader's
    // showOriginal = (uv.x < split) != swap.
    const char* leftText = swap ? "DLSS NR : ON" : "DLSS NR : OFF";
    const char* rightText = swap ? "DLSS NR : OFF" : "DLSS NR : ON";

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize() * scale;
    const float margin = 10.0f * scale;

    // Both labels flank the divider along the top: the left picture's label is right-aligned just
    // left of the split, the right picture's is left-aligned just right of it. Each is clipped to its
    // own side, so in the wipe the split reveals and hides them along with the images.
    auto drawTag = [&](const char* text, float x, ImVec2 clipMin, ImVec2 clipMax)
    {
        const ImVec2 size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);

        // Never let a label run off the visible frame as it grows.
        x = std::min(std::max(x, 0.0f), screen.x - size.x);
        float y = std::min(margin, screen.y - size.y - margin);
        y = std::max(y, 0.0f);

        dl->PushClipRect(clipMin, clipMax, true);
        dl->AddText(font, fontSize, ImVec2(x + 2.0f, y + 2.0f), IM_COL32(0, 0, 0, 210), text);
        dl->AddText(font, fontSize, ImVec2(x, y), IM_COL32(255, 255, 255, 255), text);
        dl->PopClipRect();
    };

    const ImVec2 leftSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, leftText);

    // Left picture's label: right edge a margin in from the split. Right picture's: left edge a margin
    // out from the split.
    drawTag(leftText, splitX - margin - leftSize.x, ImVec2(0.0f, 0.0f), ImVec2(splitX, screen.y));
    drawTag(rightText, splitX + margin, ImVec2(splitX, 0.0f), ImVec2(screen.x, screen.y));
}

// The lamp, and only the lamp.
//
// Red for dark, green for full light, with its reading beside it. No status sentence: the whole
// point of a light meter is that it is read at a glance while playing, and a paragraph in the corner
// of somebody's game is not that. Everything wordy lives in the menu, which is where someone has
// already decided to stop and read.
//
// Drawn only when its own setting is on. An overlay that appears because a scan happens to be
// running is an overlay nobody asked for.
void RenderExposureScanIndicator(float alpha)
{
    using DlssNr::ExposureScan::Verdict;

    auto& nr = ext::Cfg().nr;

    if (!nr.scanMeter.value_or_default())
        return;

    if (DlssNr::ExposureScan::Where() == Verdict::Off)
        return;

    int which = 0;
    float low = 0.0f, high = 0.0f;
    const float now = DlssNr::ExposureScan::BestValue(&which, &low, &high);

    // Nothing found yet, or no range to place it in: a dim lamp, which says "watching, no reading"
    // without saying it in words.
    const bool reading = now > 0.0f && high > low;

    float lit = 0.0f;

    if (reading)
    {
        // An exposure falls as the scene brightens, so the value reads backwards unless the buffer
        // holds the reciprocal -- the same question the anchor asks, answered from the same setting,
        // because a lamp contradicting the picture would be worse than no lamp.
        lit = (high - now) / (high - low);

        if (nr.scanInverted.value_or_default())
            lit = 1.0f - lit;

        lit = lit < 0.0f ? 0.0f : (lit > 1.0f ? 1.0f : lit);
    }

    // Red to amber to green. A straight red-to-green fade passes through a muddy brown at the
    // midpoint, and the midpoint is where most of a session is spent.
    const ImVec4 dark(0.90f, 0.22f, 0.20f, 1.0f);
    const ImVec4 mid(0.95f, 0.75f, 0.20f, 1.0f);
    const ImVec4 bright(0.35f, 0.88f, 0.38f, 1.0f);
    const ImVec4 idle(0.45f, 0.45f, 0.45f, 1.0f);

    ImVec4 lamp = idle;

    if (reading)
    {
        const float t = lit < 0.5f ? lit * 2.0f : (lit - 0.5f) * 2.0f;
        const ImVec4& a = lit < 0.5f ? dark : mid;
        const ImVec4& b = lit < 0.5f ? mid : bright;
        lamp = ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 12.0f, vp->WorkPos.y + 12.0f), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(alpha);

    if (ImGui::Begin("DlssNrExposureScan", nullptr,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove))
    {
        const float r = ImGui::GetFontSize() * 0.38f;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 centre(at.x + r, at.y + ImGui::GetTextLineHeight() * 0.5f);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddCircleFilled(centre, r, ImGui::GetColorU32(lamp), 20);
        draw->AddCircle(centre, r, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.6f)), 20, 1.5f);

        ImGui::Dummy(ImVec2(r * 2.0f + 6.0f, ImGui::GetTextLineHeight()));
        ImGui::SameLine();

        if (reading)
            ImGui::TextColored(lamp, "%3.0f%%  %.5f", lit * 100.0f, now);
        else
            ImGui::TextColored(idle, "--");
    }

    ImGui::End();
}

void DrawNrOverlays(ext::menu::Ctx& ctx)
{
    RenderNrCompareTags();
    RenderExposureScanIndicator(ctx.config->FpsOverlayAlpha.value_or_default());
}

// The menu frame is built also when the lamp or the comparison labels are on, because they are drawn while the menu is
// closed. The lamp is tied to its own setting and nothing else: an overlay that appears because a scan is running,
// rather than because someone asked for it, is an overlay nobody asked for.
bool NrWantsFrame()
{
    auto& nr = ext::Cfg().nr;

    const bool scanIndicator =
        nr.scanMeter.value_or_default() && DlssNr::ExposureScan::Where() != DlssNr::ExposureScan::Verdict::Off;

    return scanIndicator || (nr.compare.value_or_default() != 0 && nr.compareTags.value_or_default());
}

// ---- per-shader GPU time row -----------------------------------------------------------------------------------
std::optional<double> NrGpuTime() { return DlssNr::LastGpuTime(); }

// ---- registration (static initialisation; the registries are function-local statics) ---------------------------
struct NrMenuRegistration
{
    NrMenuRegistration()
    {
        ext::menu::Register({ "dlss_nr", "Upscaling", "DLSS Neural Rendering", DrawNrCard, "upscaler" });
        ext::menu::RegisterShortcut({ "Neural Rendering", 14, ToggleKeyOption,
                                      "Neural Rendering key pressed, will be toggling the pass", OnTogglePressed });
        ext::menu::RegisterShortcut({ "NR model resolution up", 16, ScaleUpKeyOption,
                                      "NR model resolution up key pressed", OnScaleUpPressed });
        ext::menu::RegisterShortcut({ "NR model resolution down", 17, ScaleDownKeyOption,
                                      "NR model resolution down key pressed", OnScaleDownPressed });
        ext::menu::RegisterShortcut({ "NR enlargement mode", 18, TransferToggleKeyOption,
                                      "NR enlargement toggle key pressed", OnTransferTogglePressed });
        ext::menu::RegisterOverlay({ DrawNrOverlays, NrWantsFrame });
        ext::menu::RegisterTimeRow({ "Neural Rendering", NrGpuTime });
    }
} registration;
} // namespace
