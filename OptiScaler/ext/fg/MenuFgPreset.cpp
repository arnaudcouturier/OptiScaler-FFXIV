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
// of its own and no entry in the layout table.

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
        ImGui::Text("Runtime read the new request after the last change: %s", view.readSinceRequest ? "yes" : "no");

    if (view.lastLine.empty())
        ImGui::TextWrapped("NVIDIA preset line: none yet (it appears after the first request other than Auto)");
    else
        ImGui::TextWrapped("NVIDIA preset line: %s", view.lastLine.c_str());
    if (!view.lastLine.empty())
        ImGui::TextDisabled("%s", view.confirmed ? "confirmed: the line has a known format" : "not confirmed: the wording is not one this version knows");

    if (view.hasReport)
    {
        ImGui::Text("Runtime: requested %d, applied %d, value %llu", view.requested, view.applied,
                    static_cast<unsigned long long>(view.value));
        if (view.requested > 0 && view.applied == 0)
            ImGui::TextWrapped("The runtime did not apply this preset (applied=0): this build of NVIDIA's runtime may not act on it.");
    }
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
                       "moment, its resources are released and it resumes on the next frame with fresh history.\n"
                       "Auto leaves the choice to the driver and the runtime. A, B and C are answered inside this process only;\n"
                       "no NVIDIA profile is written. Not every runtime build acts on all three: the lines below show what it reports.");

    if (off)
    {
        ImGui::TextWrapped("%s", UnavailableText(status.unavailable));
        return;
    }

    if (status.restartNeeded)
    {
        ImGui::TextWrapped("Restart needed: %s. %s stays in use until the next start, then the saved choice applies.",
                           FailText(status.fail), Name(status.active));
        if (!status.moduleText.empty() && status.fail == SwitchFail::Prepare)
            ImGui::TextWrapped("%s", status.moduleText.c_str());
    }
    else if (status.configured != status.active)
        ImGui::TextWrapped("%s is chosen; it applies when frame generation runs next.", Name(status.configured));
    else
        ImGui::Text("In use: %s%s", Name(status.active), status.switches > 0 ? " (changed while the game ran)" : "");

    // HUD-less recomposition and Preset A/C: the runtime's own UI recomposition is paused (ui_free mode is not affected)
    if (ext::hudless::QueryRecomposition().enabled &&
        (PausesUiRecomposition(status.configured) || PausesUiRecomposition(status.active)))
        ImGui::TextWrapped("Preset A and C turn off NVIDIA's UI recomposition: with the HUD-less recomposition mode the interface is "
                           "no longer kept out of the generated frames. Preset B or Auto brings it back.");

    DrawReadout(status);
}
} // namespace ext::fg
