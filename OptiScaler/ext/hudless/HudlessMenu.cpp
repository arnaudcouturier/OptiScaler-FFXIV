#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/fg/dlssg/PresetStatus.h"
#include "ext/game/Game.h"
#include "ext/hudless/HudlessConfig.h"
#include "ext/hudless/HudlessEngine.h"
#include "ext/reshade/ReShadeBoundary.h"

#include <menu/menu_common.h>

// Card `fg_hudless` (tab "FFXIV"), docs/HUDLESS.md section 11: a status line (state, reason, counters), the mode
// selector and an "Advanced" fold with the tuning keys. Replaces the reference build's REST HUD box, whose checkbox the
// missing ReShade add-on kept greyed out; the old [FrameGen] ExternalUI* keys are gone.

namespace
{
using ext::hudless::Mode;

bool Visible(const ext::menu::Ctx&) { return ext::game::Current().hudless != nullptr; }

void DrawStatus(const ext::hudless::Status& status)
{
    const auto line = ext::hudless::StatusLine(status);
    ImGui::TextWrapped("Status: %s", line.c_str());
    if (status.mode == Mode::Off)
        return;

    const double share = status.presents == 0 ? 0.0 : 100.0 * double(status.captured) / double(status.presents);
    ImGui::TextDisabled("%llu presents, %llu used the hud-less image (%.1f%%); UI boundary: %s",
                        (unsigned long long) status.presents, (unsigned long long) status.captured, share,
                        status.boundary.c_str());

    if (status.lastReason != ext::hudless::Reason::None)
        ImGui::TextDisabled("Last present: %s", ext::hudless::ReasonText(status.lastReason));

    if (status.mode == Mode::UiFree)
    {
        if (status.coverage >= 0.f)
            ImGui::TextDisabled("Flagged UI area: %.1f%% of the screen%s", 100.0 * status.coverage,
                                status.coverageTripped ? " (over the guard, falling back)" : "");
        else if (status.coverageUnknown)
            ImGui::TextDisabled("Flagged UI area: unknown (no readback has completed)");
        ImGui::TextDisabled("UI paste: %llu built, %llu drawn, %llu skipped", (unsigned long long) status.paste.produced,
                            (unsigned long long) status.paste.pasted, (unsigned long long) status.paste.skipped);
    }

    // Fallback tally, only the reasons that happened
    std::string tally;
    for (size_t i = 1; i < ext::hudless::kReasonCount; ++i)
    {
        if (status.reasons[i] == 0)
            continue;
        if (!tally.empty())
            tally += ", ";
        tally += std::format("{} {}", ext::hudless::ReasonName(static_cast<ext::hudless::Reason>(i)), status.reasons[i]);
    }
    if (!tally.empty())
        ImGui::TextDisabled("Fallbacks: %s", tally.c_str());
}

void DrawHudless(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("HUD-less (UI-free frame generation)");

    auto& cfg = ext::Cfg().hudless;
    const Mode current = ext::hudless::CurrentMode();

    DrawStatus(ext::hudless::GetStatus());

    static const char* kModes[] = { "Off", "Recomposition (DLSS-G recomposes the UI)", "UI-free frame generation" };
    int selected = static_cast<int>(current);
    if (ImGui::Combo("Mode", &selected, kModes, 3) && selected != static_cast<int>(current))
    {
        const Mode chosen = static_cast<Mode>(selected);
        ext::hudless::SetMode(chosen);
        LOG_INFO("HudLess: mode set to {} from the menu", ext::hudless::ModeName(chosen));
    }
    ext::menu::Tooltip("Off: frame generation interpolates the whole frame, UI included (today's behaviour).\n"
                       "Recomposition: DLSS-G gets a hud-less and a UI image and recomposes the UI itself; it still "
                       "interpolates the UI layer. DLSS-G reads this when it is created: applies after a game restart. "
                       "FSR-FG and XeFG only receive the tags (untested).\n"
                       "UI-free: frame generation runs on the UI-free image and the UI is pasted back over every output "
                       "frame at the real frame rate. DLSS-G only.\n"
                       "Any doubt (no UI boundary, a size or format mismatch, a busy GPU slot, a fade) shows the normal "
                       "frame instead; the status line says why.");

    if (current == Mode::Recomposition || (selected == int(Mode::Recomposition)))
        ImGui::TextWrapped("Recomposition applies after a game restart (DLSS-G reads it when the feature is created).");

    // A DLSS-G render preset that takes the recomposition away (A, C): the same sentence as in the log and next to the preset selector.
    if (const auto note = ext::fg::preset::UiRecompositionMenuNote(); !note.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
        ImGui::TextWrapped("%s", note.c_str());
        ImGui::PopStyleColor();
    }

    bool tint = cfg.showDetectedUi.value_or_default();
    if (ImGui::Checkbox("Show detected UI (HUD-less)", &tint))
        cfg.showDetectedUi = tint;
    ext::menu::Tooltip("Debug: tints the pasted UI area so the mask is visible (UI-free mode).");

    // [HudLess] ReShadePresets: auto | on | off; the order of the combo is the order of ext::reshade::PresetsMode.
    static constexpr const char* kPresetNames[] = { "auto", "on", "off" };
    int preset = static_cast<int>(ext::reshade::ParsePresetsMode(ext::Cfg().reshade.presets.value_or_default()));
    if (ImGui::Combo("ReShade presets at the UI boundary", &preset, "Auto\0On\0Off\0"))
        ext::Cfg().reshade.presets = std::string(kPresetNames[preset]);
    ext::menu::Tooltip("Apply your ReShade presets in place at the UI boundary, so the UI-free image carries the effects and "
                       "the HUD stays unfiltered. Only used in UI-free mode (needs ReShade with the add-on).\n"
                       "Auto: when ReShade registered a runtime and at least one technique is enabled.\n"
                       "On: always try (the technique count is not trusted).\n"
                       "Off: never apply presets here.\n"
                       "Switching to Off applies at once.\n"
                       "Switching it on again after the game started with Off applies after a restart (the ReShade runtime tracking is only installed at start when presets are not Off).\n"
                       "Save Settings to keep it.");

    if (!ImGui::TreeNode("Advanced"))
        return;

    float threshold = cfg.Threshold();
    if (ImGui::SliderFloat("Mask threshold", &threshold, ext::HudlessConfig::kThresholdMin, ext::HudlessConfig::kThresholdMax, "%.3f",
                           ImGuiSliderFlags_Logarithmic))
        cfg.maskThreshold = threshold;
    ext::menu::Tooltip("Largest per-channel difference (0..1) between the final frame and the UI-free frame that still "
                       "counts as the same pixel. Default 0.008.");

    int dilation = static_cast<int>(cfg.Dilation());
    if (ImGui::SliderInt("Mask dilation (px)", &dilation, 0, ext::HudlessConfig::kDilationMax))
        cfg.maskDilation = dilation;
    ext::menu::Tooltip("Grow the detected UI area by this many pixels. Default 1.");

    bool cleanup = cfg.pasteCleanup.value_or_default();
    if (ImGui::Checkbox("Clear UI that just closed", &cleanup))
        cfg.pasteCleanup = cleanup;
    ext::menu::Tooltip("Also paste where the previous frame had UI and this one does not, so the ghost of a closed window "
                       "does not linger (not needed on a UI-free base).");

    bool steady = cfg.steadyTiming.value_or_default();
    if (ImGui::Checkbox("Steady UI timing", &steady))
        cfg.steadyTiming = steady;
    ext::menu::Tooltip("Paste the UI of one real frame behind: steadier world-anchored UI such as nameplates, about one "
                       "base frame of extra UI latency.");

    float guard = cfg.CoverageLimit();
    if (ImGui::SliderFloat("Coverage guard", &guard, 0.f, 1.f, guard <= 0.f ? "off" : "%.2f"))
        cfg.coverageGuard = guard;
    ext::menu::Tooltip("Fall back to normal frame generation when more than this share of the screen is flagged as UI for "
                       "five frames in a row (full-screen fades, a preset that runs before the capture); retries after "
                       "120 frames. 0 = off. Default 0.60.");

    // [DLSSG] UIRecomposition: unset = on (the engine treats an unset key as on); read when the DLSS-G feature is created.
    auto& recompositionSetting = ext::Cfg().fg.uiRecomposition;
    int recomposition = !recompositionSetting.has_value() ? 0 : (recompositionSetting.value() ? 1 : 2);
    if (ImGui::Combo("UI recomposition (restart)", &recomposition, "Auto (on)\0On\0Off\0"))
        recompositionSetting = recomposition == 0 ? std::optional<bool> {} : std::optional<bool>(recomposition == 1);
    ext::menu::Tooltip("Recomposition mode only: whether DLSS-G gets a separate UI path and recomposes the UI itself "
                       "(Auto = on). Off: DLSS-G treats the tagged hud-less and UI images as hints only and still "
                       "interpolates the UI.\n"
                       "DLSS-G reads this when it is created: applies after a restart. Save Settings to keep it.");

    if (ImGui::Button("Reset advanced"))
    {
        cfg.maskThreshold = ext::HudlessConfig::kThresholdDefault;
        cfg.maskDilation = 1;
        cfg.pasteCleanup = true;
        cfg.steadyTiming = false;
        cfg.coverageGuard = ext::HudlessConfig::kGuardDefault;
    }

    ImGui::TreePop();
}

ext::menu::AutoRegister reg { { "fg_hudless", "FFXIV", "HUD-less (UI-free frame generation)", DrawHudless, "fg_selection",
                                Visible } };
} // namespace
