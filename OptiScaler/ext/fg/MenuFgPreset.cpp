#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuWidgets.h"
#include "ext/fg/FgInternal.h"
#include "ext/fg/dlssg/PresetLogRelay.h"
#include "ext/fg/dlssg/PresetStatus.h"
#include "ext/hudless/HudlessEngine.h"

#include <Config.h>

#include <menu/menu_common.h>

// The DLSS-G render preset section of the card `fg_unlocks` (tab "Frame Gen"): the choice of [DLSSG] RenderPreset, switched live
// by ext/fg/dlssg/PresetSeams.cpp, and what NVIDIA's runtime says about it. Drawn by DrawFgUnlocks (MenuFgUnlocks.cpp), so no card
// of its own and no entry in the layout table. A preset is shown as "in use" only when the runtime confirmed it (PresetLog.h).

namespace
{
using namespace ext::fg::preset;

const char* UirText(Uir uir)
{
    switch (uir)
    {
    case Uir::Disabled:
        return "UI recomposition off";
    case Uir::Enabled:
        return "UI recomposition on";
    default:
        return nullptr;
    }
}

// The runtime's reply can reach us: the log relay is attached and info lines pass the log level.
bool ReplyReadable()
{
    return LogRelayState() == RelayState::Attached && Config::Instance()->LogLevel.value_or_default() <= 2;
}

void DrawReadout(const Status& status)
{
    FlushCandidateLines(); // unrecognised preset lines go to the log from here as well as from the dispatch tick
    auto& readout = ReadoutState();
    const auto view = readout.Snapshot();

    ImGui::TextDisabled("What NVIDIA's runtime reports");
    ImGui::Text("Driver queried the setting: %s", view.queried ? "yes" : "no");
    ext::menu::Tooltip("Seen by the driver-settings hook: the runtime asked the driver for its preset at start-up. 'no' means it\n"
                       "never did (an old runtime, or the request was never needed); a request then only works as a live change.");
    if (status.switches > 0)
    {
        ImGui::Text("Runtime read the new request after the last change: %s", view.readSinceRequest ? "yes" : "no");
        ext::menu::Tooltip("'no' after a change can mean that another copy of the runtime than the one that was hooked is in use.");
    }

    if (view.lastLine.empty())
        ImGui::TextWrapped("NVIDIA preset line: none yet (it appears after the first request other than Auto)");
    else
        ImGui::TextWrapped("NVIDIA preset line: %s", view.lastLine.c_str());
    if (!view.lastLine.empty())
        ImGui::TextDisabled("%s", view.confirmed ? "confirmed: the line has a known format" : "not confirmed: the wording is not one this version knows");

    if (view.hasReport)
        ImGui::Text("Runtime report: preset %llu, driver held a preset %d, applied %d", static_cast<unsigned long long>(view.value),
                    view.requested, view.applied);
    if (const char* uir = UirText(view.uir))
        ImGui::Text("Runtime: %s", uir);
    if (view.blockedByFlags)
        ImGui::TextWrapped("The runtime ignores presets because of private flags set in the driver.");

    switch (LogRelayState())
    {
    case RelayState::Lost:
        ImGui::TextWrapped("The log was reopened after a change of the log settings: NVIDIA's preset line cannot be shown until the next "
                           "start.");
        break;
    case RelayState::NotAttached:
        ImGui::TextWrapped("NVIDIA's preset line cannot be shown: the log is not active.");
        break;
    case RelayState::AsyncLogging:
        ImGui::TextWrapped("NVIDIA's preset line cannot be shown with asynchronous logging (LogAsync).");
        break;
    default:
        if (Config::Instance()->LogLevel.value_or_default() > 2)
            ImGui::TextWrapped("NVIDIA's preset line is not shown at this log level: set LogLevel to Trace, Debug or Info.");
        break;
    }
}
} // namespace

namespace ext::fg
{
void DrawRenderPresetSection()
{
    auto& cfg = ext::Cfg().fg;
    const auto status = GetStatus();

    ext::menu::SectionTitle("DLSS-G render preset");

    const bool off = status.unavailable != Unavailable::None;
    ImGui::BeginDisabled(off);
    int preset = cfg.renderPreset.value_or_default();
    if (preset < kAuto || preset > kPresetC)
        preset = kAuto;
    if (ImGui::Combo("DLSS-G render preset", &preset, "Auto\0Preset A\0Preset B\0Preset C\0"))
        cfg.renderPreset = preset;
    ImGui::EndDisabled();
    ext::menu::Tooltip("Render preset of NVIDIA's frame generation, changed while the game runs: frame generation is switched off for a\n"
                       "moment, its resources are released and it resumes a few frames later with fresh history.\n"
                       "Auto leaves the choice to the driver and the runtime. A, B and C are answered inside this process only;\n"
                       "no NVIDIA profile is written. Not every runtime build acts on all three: the lines below show what it reports.");

    if (off)
    {
        ImGui::TextWrapped("%s", UnavailableText(status.unavailable));
        return;
    }
    if (status.configured == kPresetC || status.active == kPresetC)
        ImGui::TextDisabled("Preset C: the 310.9.1.0 runtime ignores it (it reports applied=0); other builds may differ.");

    const auto confirmed = ReadoutState().Confirm(status.active);
    if (status.restartNeeded)
    {
        ImGui::TextWrapped("Restart needed: %s. %s stays in use until the next start, then the saved choice applies.",
                           FailText(status.fail), Name(status.active));
        if (!status.moduleText.empty() && status.fail == SwitchFail::Prepare)
            ImGui::TextWrapped("%s", status.moduleText.c_str());
    }
    else if (status.configured != status.active)
        ImGui::TextWrapped("%s is chosen; the change is made when frame generation runs next.", Name(status.configured));
    else
        ImGui::TextWrapped("%s%s", StatusSentence(status.active, confirmed.effect, ReplyReadable()).c_str(),
                           status.switches > 0 && status.active == kAuto ? " (changed while the game ran)" : "");

    // HUD-less recomposition and the presets: A turns the runtime's own UI recomposition off (ui_free mode is not affected)
    const bool involved =
        status.configured == kPresetA || status.active == kPresetA || status.configured == kPresetC || status.active == kPresetC;
    if (ext::hudless::QueryRecomposition().enabled && involved)
    {
        if (UiRecompositionPaused())
            ImGui::TextWrapped("UI recomposition of NVIDIA's runtime is paused by this preset: with the HUD-less recomposition mode the "
                               "interface is no longer kept out of the generated frames. Preset B or Auto brings it back.");
        else if (status.active == kPresetC)
            ImGui::TextWrapped("Preset C: UI recomposition stays on until the runtime confirms that it acts on C and turns it off.");
    }

    DrawReadout(status);
}
} // namespace ext::fg
