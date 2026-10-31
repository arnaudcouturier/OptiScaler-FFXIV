#include "pch.h"

#include "ext/ExtFeatures.h"
#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/fg/dlssg/PresetStatus.h"
#include "ext/game/Game.h"
#include "ext/hudless/HudlessConfig.h"
#include "ext/hudless/HudlessEngine.h"
#include "ext/hudless/HudlessMenuLog.h"
#include "ext/hudless/PlateMode.h"
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
        // The guard in force: its limit, where the release sits, and why a present falls back while it is tripped (docs/HUDLESS.md 5).
        ImGui::TextDisabled("%s", ext::hudless::GuardStatusLine(status.guardLimit, status.coverageTripped, status.guardPolicy).c_str());
        // Hidden while the blind A/B of the interval runs: the line would tell the player which block they are in.
        if (status.coverageTripped && status.probeInterval > 1 && ext::Cfg().hudless.ProbeAbSeconds() <= 0)
            ImGui::TextDisabled("Measuring on one frame in %d while falling back", status.probeInterval);
        // The HUD's own flagged area the limit has to let through, so the user has a number to set the guard against before the
        // warning ever fires, and a pointer to the warning itself (whose full text is next to the guard slider in the fold).
        if (status.hudCoverage >= 0.f)
            ImGui::TextDisabled("HUD's own flagged area: %.1f%% (low fifth of %llu readbacks)", 100.0 * status.hudCoverage,
                                (unsigned long long) status.hudCoverageSamples);
        ImGui::TextDisabled("UI paste: %llu built, %llu drawn, %llu skipped", (unsigned long long) status.paste.produced,
                            (unsigned long long) status.paste.pasted, (unsigned long long) status.paste.skipped);
    }

    // The guard-limit warning, one short pointer line here: the numbers and what to set are in the Advanced fold, next to the guard
    // slider the warning is about.
    if (status.guardAdvisoryWarn)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
        ImGui::TextWrapped("The coverage guard limit cannot release over the HUD's own flagged area: Advanced, below the guard "
                           "slider, has the numbers and the value to set.");
        ImGui::PopStyleColor();
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

    const ext::hudless::Status status = ext::hudless::GetStatus();
    DrawStatus(status);

    // One log line per slider change that settled (HudlessMenuLog.h): a session log then shows what the user set and when. The
    // flush runs before the fold's early return, so a value that settled while the fold (or the card) was closed is written on the
    // next frame of the card instead of waiting for the fold to be opened again.
    static ext::hudless::HudlessSliderLogs sliderLogs;
    for (const std::string& line : sliderLogs.Due(GetTickCount64()))
        LOG_INFO("{}", line);
    static ext::hudless::SliderSettleLog probeLog; // [HudLess] ProbeInterval, same settle rule
    if (probeLog.Due(GetTickCount64()))
        LOG_INFO("HudLess: ProbeInterval set to {} from the menu", static_cast<int>(probeLog.Take()));

    static const char* kModes[] = { "Off", "Recomposition (DLSS-G recomposes the UI)", "UI-free frame generation (experimental)" };
    int selected = static_cast<int>(current);
    if (ImGui::Combo("Mode", &selected, kModes, 3) && selected != static_cast<int>(current))
    {
        const Mode chosen = static_cast<Mode>(selected);
        ext::hudless::SetMode(chosen);
        LOG_INFO("HudLess: mode set to {} from the menu", ext::hudless::ModeName(chosen));
    }
    ext::menu::Tooltip("Off: frame generation interpolates the whole frame, UI included (today's behaviour).\n"
                       "Recomposition: DLSS-G gets a hud-less and a UI image and recomposes the UI itself; it still "
                       "interpolates the UI layer. Switching to or from it recreates DLSS-G (frame generation restarts for a moment). "
                       "FSR-FG and XeFG only receive the tags (untested).\n"
                       "UI-free: frame generation runs on the UI-free image and the UI is pasted back over every output "
                       "frame at the real frame rate. DLSS-G only. Experimental: it has a known micro-stutter in motion, with "
                       "DLSS-G preset Auto and preset C alike. Recomposition with preset Auto showed no micro-stutter.\n"
                       "Any doubt (no UI boundary, a size or format mismatch, a busy GPU slot, a fade) shows the normal "
                       "frame instead; the status line says why.");

    // ui_free is the mode with the known micro-stutter; the recommendation is recomposition with the DLSS-G preset on Auto.
    if (current == Mode::UiFree)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
        ImGui::TextWrapped("UI-free is experimental and micro-stutters in motion. Recomposition with DLSS-G preset Auto does not.");
        ImGui::PopStyleColor();
    }

    if (current == Mode::Recomposition || (selected == int(Mode::Recomposition)))
        ImGui::TextWrapped("Switching to or from recomposition recreates DLSS-G: frame generation restarts for a moment.");
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
    ext::menu::Tooltip("Debug: tints the pasted UI area so the mask is visible (UI-free mode). While the coverage guard is "
                       "tripped the tinted pixels are the newest real frame's, and the draw replaces the output there: that region "
                       "runs at the real frame rate instead of the generated one, with the judder the fallback already has.");

    // Native nameplates: the game's own nameplate draws are taken into a plate-only image and drawn back over every
    // output frame. Snapshot first, bit second: the presenting threads and the engine read the snapshot, so the bit may
    // only say "on" once the key says so, and the menu never clears it (the engine clears it when the last hidden plate
    // image is gone and re-reads this snapshot, so a key turned back on in that instant ends with the bit back on).
    if (ImGui::TreeNode("Native nameplates (experimental)"))
    {
        auto publish = [&cfg]
        {
            const ext::hudless::PlateSnapshot snapshot = cfg.PlateSettings();
            ext::hudless::PublishPlateSettings(snapshot);
            if (snapshot.interp != ext::hudless::PlateInterp::Off)
                ext::SetActive(ext::F::Plate, true);
        };

        static constexpr const char* kInterpNames[] = { "off", "endpoint", "on" };
        int interp = static_cast<int>(cfg.GetPlateInterp());
        if (ImGui::Combo("Interpolate nameplates", &interp, "Off\0Endpoint\0On\0"))
        {
            cfg.plateInterp = std::string(kInterpNames[interp]);
            publish();
        }
        ext::menu::Tooltip("Experimental, and never tried in a game: only a session can say whether the game's plate draws really have the state the rule expects.\n"
                           "The game's nameplates move once per game frame; frame generation shows that step on every output.\n"
                           "Off: nameplates stay native (today's behaviour).\n"
                           "Endpoint: the plate draws are taken out of the game's frame and drawn back over every output "
                           "unchanged, which proves the whole path with no visual change.\n"
                           "On: reserved for per-output shifting. This build has no plate tracker, so On draws exactly what "
                           "Endpoint draws and moves nothing; the two keys below have no effect yet.\n"
                           "Needs DLSS-G with a multiplier of 2 or more and an SDR output; any doubt keeps the plates native.");

        static constexpr const char* kPhaseNames[] = { "auto", "0", "1" };
        const int parsed = ext::hudless::ParsePlatePhase(cfg.platePhaseOffset.value_or_default());
        int phase = parsed == ext::hudless::kPlatePhaseAuto ? 0 : parsed + 1;
        if (ImGui::Combo("Shift order", &phase, "Auto\0Constant 0\0Constant 1\0"))
        {
            cfg.platePhaseOffset = std::string(kPhaseNames[phase]);
            publish();
        }
        ext::menu::Tooltip("Which output of a batch shows the older plate position. Auto assumes the generated frames come "
                           "first and the real frame last. No effect in this build (no plate tracker yet).");

        int step = cfg.GetPlateStep();
        if (ImGui::SliderInt("Largest shift (px per frame)", &step, 0, ext::HudlessConfig::kPlateStepMax))
        {
            cfg.plateMaxStep = step;
            publish();
        }
        ext::menu::Tooltip("The biggest distance a nameplate may be moved between two outputs, in output pixels. A plate "
                           "that moves further than this is drawn unshifted for that frame instead of jumping. "
                           "No effect in this build (no plate tracker yet). Default 24.");

        ImGui::TreePop();
    }

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
    {
        cfg.maskThreshold = threshold;
        sliderLogs.maskThreshold.Observe(cfg.Threshold(), GetTickCount64());
    }
    ext::menu::Tooltip("Largest per-channel difference (0..1) between the final frame and the UI-free frame that still "
                       "counts as the same pixel. Default 0.008.");

    int dilation = static_cast<int>(cfg.Dilation());
    if (ImGui::SliderInt("Mask dilation (px)", &dilation, 0, ext::HudlessConfig::kDilationMax))
    {
        cfg.maskDilation = dilation;
        sliderLogs.maskDilation.Observe(static_cast<float>(cfg.Dilation()), GetTickCount64());
    }
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

    // [HudLess] AnchorRule: where the UI-free image is taken on a frame whose first UI draw goes to an off-screen target (docs/HUDLESS.md
    // section 10). Live: the boundary reads it at every present and logs each change.
    static constexpr const char* kAnchorNames[] = { "r1b", "early" };
    int anchor = static_cast<int>(cfg.GetAnchorRule());
    if (ImGui::Combo("UI-free capture point", &anchor, "First HUD draw (default)\0Early (experimental)\0"))
        cfg.anchorRule = std::string(kAnchorNames[anchor]);
    ext::menu::Tooltip("Where the UI-free image is taken on frames whose first interface draw goes to an off-screen target (a small "
                       "interface target the game draws before the HUD in the world, probably the minimap). In a city such frames "
                       "made the detected UI area cover most of the screen.\n"
                       "First HUD draw: at the first interface draw into the screen image (the behaviour before this setting).\n"
                       "Early: at that off-screen draw, right after the scene, before the off-screen block and the draws that follow "
                       "it. Frames whose first interface draw is on the screen image do not change, and a frame with ReShade presets "
                       "to apply at the UI boundary keeps the first HUD draw.\n"
                       "Both HUD-less modes use this image. In recomposition mode no coverage guard checks it, so a wrong early image goes to DLSS-G as it is (in UI-free mode the guard falls back).\n"
                       "Experimental: unit-tested, not tried in a game yet. Applies at once; not changed by Reset advanced. Save "
                       "Settings to keep it.");

    // The coverage guard is its own row of the fold, under its own heading: it is a second 0..1 slider and was mistaken for the mask
    // threshold in a play session, and a limit below the HUD's own coverage keeps ui_free off for a whole session.
    ImGui::Spacing();
    ext::menu::SectionTitle("Coverage guard");
    ImGui::TextDisabled("Separate from the Mask threshold above: this one does not change the mask, it decides when to stop using it.");

    float guard = cfg.CoverageLimit();
    if (ImGui::SliderFloat("Coverage guard limit", &guard, 0.f, 1.f, guard <= 0.f ? "off" : "%.2f"))
    {
        cfg.coverageGuard = guard;
        sliderLogs.coverageGuard.Observe(cfg.CoverageLimit(), GetTickCount64());
    }
    ext::menu::Tooltip("Fall back to normal frame generation when more than this share of the screen is flagged as UI for "
                       "five frames in a row (full-screen fades, a preset that runs before the capture); retries after "
                       "120 frames. 0 = off. Default 0.60 (Reset advanced restores it).\n"
                       "The guard only comes back at or below three quarters of this value, so a limit has to stay well above "
                       "the HUD's own flagged area (shown in the status); the warning below names a limit that releases.");

    const ext::hudless::GuardAdvisory advisory =
        ext::hudless::EvaluateGuardAdvisory(cfg.CoverageLimit(), status.hudCoverage, status.hudCoverageSamples);
    // The guard exists in ui_free only: in the other modes the limit changes nothing, so the warning stays silent there.
    if (status.mode == Mode::UiFree)
    {
        if (const std::string warning = ext::hudless::GuardAdvisoryMenuLine(advisory); !warning.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", warning.c_str());
            ImGui::PopStyleColor();
        }
    }

    // While tripped the guard keeps measuring on one frame in N only (docs/HUDLESS.md 5): the per-frame probe could make frame generation
    // stutter during the fallback.
    int probe = cfg.ProbeInterval();
    if (ImGui::SliderInt("Probe one frame in N while falling back", &probe, 1, ext::hudless::kProbeIntervalMax))
    {
        cfg.probeInterval = probe;
        probeLog.Observe(static_cast<float>(cfg.ProbeInterval()), GetTickCount64());
    }
    ext::menu::Tooltip("While the coverage guard is tripped (falling back), build the UI-free capture and its mask on only one frame in "
                       "this many; the other frames are plain frame generation with nothing extra. The guard then comes back after about "
                       "30 x N frames of a clean scene instead of 30. 1 = every frame (the old behaviour). Building them on every frame "
                       "is the suspected cause of a faint stutter felt while falling back (with DLSS-G render preset C; felt in play, "
                       "not measured).\n"
                       "Experimental: unit-tested, not tried in a game yet. Not changed by Reset advanced.");

    // [DLSSG] UIRecomposition: unset = on (the engine treats an unset key as on); read when the DLSS-G feature is created.
    auto& recompositionSetting = ext::Cfg().fg.uiRecomposition;
    int recomposition = !recompositionSetting.has_value() ? 0 : (recompositionSetting.value() ? 1 : 2);
    if (ImGui::Combo("UI recomposition", &recomposition, "Auto (on)\0On\0Off\0"))
        recompositionSetting = recomposition == 0 ? std::optional<bool> {} : std::optional<bool>(recomposition == 1);
    ext::menu::Tooltip("Recomposition mode only: whether DLSS-G gets a separate UI path and recomposes the UI itself "
                       "(Auto = on). Off: DLSS-G treats the tagged hud-less and UI images as hints only and still "
                       "interpolates the UI.\n"
                       "A change recreates DLSS-G at once (frame generation restarts for a moment). Save Settings to keep it.");

    if (ImGui::Button("Reset advanced"))
    {
        cfg.maskThreshold = ext::HudlessConfig::kThresholdDefault;
        cfg.maskDilation = 1;
        cfg.pasteCleanup = true;
        cfg.steadyTiming = false;
        cfg.coverageGuard = ext::HudlessConfig::kGuardDefault;
        // One line with the values it restored, and a drag that had not settled yet is dropped with them: the log must not show a
        // value the Reset already replaced.
        for (const std::string& line : sliderLogs.Reset(cfg.Threshold(), static_cast<int>(cfg.Dilation()), cfg.CoverageLimit()))
            LOG_INFO("{}", line);
    }
    ext::menu::Tooltip("Restore the advanced defaults above: Mask threshold 0.008, dilation 1, clean-up on, steady timing off, "
                       "and the Coverage guard back to 0.60 (the guard is reset with them). Save Settings to keep it.");

    ImGui::TreePop();
}

ext::menu::AutoRegister reg { { "fg_hudless", "FFXIV", "HUD-less (UI-free frame generation)", DrawHudless, "fg_selection",
                                Visible } };
} // namespace
