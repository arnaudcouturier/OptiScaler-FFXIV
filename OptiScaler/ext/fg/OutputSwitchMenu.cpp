#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/fg/outswitch/OutputSwitch.h"

#include <menu/menu_common.h>

// Card `fg_output_switch` (tab "Frame Gen", after the automatic multiplier; position: ext/core/MenuLayout.h): the master
// key of the live output switch and the state of its last attempt.
//
// The card reads the switch's status every frame (atomics only, StatusBox.h) and the key from the config, so nothing here is
// cached: what the player sees is the state the present thread last published.
namespace
{
bool OutputSwitchVisible(const ext::menu::Ctx& c)
{
    (void) c;
    return true;
}

void DrawOutputSwitchCard(ext::menu::Ctx& c)
{
    (void) c;
    ext::menu::SectionTitle("Live frame generation output switch"); // the card title, as the manifest test requires

    auto& cfg = ext::Cfg().fg;

    bool enabled = cfg.outputHotSwap.value_or_default();
    if (ImGui::Checkbox("Live output switch (experimental)", &enabled))
        cfg.outputHotSwap = enabled;
    ext::menu::Tooltip("Move the chain that carries frame generation to the newly configured output while the game runs, "
                       "instead of asking for a restart.\n"
                       "Only starts from a run that already has FSR FG or XeFG selected with the upscaler as its input; a run "
                       "with frame generation off has no input path to switch to, and a DLSS-G output is not switched live. "
                       "From there it moves between FSR FG, XeFG "
                       "and plain presentation in all six directions; they were verified on the official benchmark, in runs "
                       "that booted with FSR FG and in runs that booted with XeFG, not yet in a game session.\n"
                       "The new chain is judged by its first presents: one that fails is put back to plain presentation and that "
                       "target is remembered until the game restarts. A session allows a few switches.\n"
                       "Off (default) is today's behaviour: the configured output applies on the next start.");

    const ext::fg::outswitch::Status status = ext::fg::outswitch::MenuStatus();
    ImGui::TextWrapped("Configured output: %s, active output: %s",
                       ext::fg::outswitch::TargetText(status.target),
                       ext::fg::outswitch::TargetText(status.active));

    switch (status.state)
    {
    case ext::fg::outswitch::State::Switching:
        ImGui::TextWrapped("Switching...");
        break;
    case ext::fg::outswitch::State::Probation:
        ImGui::TextWrapped("On the new chain, being judged by its presents (%u so far).", status.probationPresents);
        break;
    case ext::fg::outswitch::State::Refused:
        ImGui::TextWrapped("Refused: %s.", ext::fg::outswitch::FailText(status.reason));
        break;
    case ext::fg::outswitch::State::RolledBack:
        ImGui::TextWrapped("Rolled back to plain presentation: %s.", ext::fg::outswitch::FailText(status.reason));
        if (ImGui::Button("Try again"))
            ext::fg::outswitch::TryAgainTarget(status.target);
        break;
    case ext::fg::outswitch::State::Limp:
        ImGui::TextWrapped("No chain on the window: %s. Retrying plain presentation for a few seconds.",
                           ext::fg::outswitch::FailText(status.reason));
        break;
    case ext::fg::outswitch::State::Dead:
        ImGui::TextWrapped("Stopped for this session: %s. Restart the game to try again.",
                           ext::fg::outswitch::FailText(status.reason));
        break;
    default:
        if (enabled)
            ImGui::TextWrapped("Waiting for a configured output that differs from the active one.");
        break;
    }

    if (status.switchesLeft != 0 || status.state == ext::fg::outswitch::State::Probation)
        ImGui::TextWrapped("Switches left this session: %u. Last switch took %u ms.", status.switchesLeft,
                           status.lastHitchMs);
}

ext::menu::AutoRegister reg { { "fg_output_switch", "Frame Gen", "Live frame generation output switch",
                                DrawOutputSwitchCard, "fg_auto_mfg", OutputSwitchVisible } };
} // namespace
