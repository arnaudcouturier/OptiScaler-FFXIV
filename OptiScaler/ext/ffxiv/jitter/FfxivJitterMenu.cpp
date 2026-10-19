#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/game/Game.h"

#include <imgui/imgui.h>

// "Game jitter (FFXIV)" card (tab Upscaling, right after Output Scaling): the two [FFXIV] jitter keys as live switches.
// The hook (FfxivJitterProbe.cpp) reads both keys at every DLSS evaluate, so a tick applies at once; like every other menu
// setting it is stored in the ini only when the settings are saved. Nothing else lives here: the decisions are in
// FfxivJitterCore.h (tested).
namespace ext::ffxiv::jitter::menu
{
namespace
{
// Same condition as the character preview card: the FFXIV build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Game jitter (FFXIV)");

    auto& cfg = ext::Cfg().ffxiv;

    bool probe = cfg.jitterProbe.value_or_default();
    if (ImGui::Checkbox("Log the game's jitter", &probe))
        cfg.jitterProbe = probe;
    ext::menu::Tooltip("Read-only. Writes a line to the log every few seconds that compares the jitter the game\n"
                       "reports to DLSS with the jitter it really applied to the picture. Changes nothing.");

    bool correct = cfg.jitterCorrection.value_or_default();
    if (ImGui::Checkbox("Correct the game's jitter (experimental)", &correct))
        cfg.jitterCorrection = correct;
    ext::menu::Tooltip("Hands DLSS the jitter the game really applied instead of the one it reports, when they differ\n"
                       "(the jitter probe showed a constant factor of 0.6 in this game). Applies at once.\n\n"
                       "One write feeds super resolution, frame generation, NR and the Jitter Count readout: to compare,\n"
                       "look at the picture with frame generation off first. If something looks wrong, untick it.");

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("The log line \"Jitter correction: on; totals since the start of the session ...\" shows how many "
                       "evaluates were replaced and how many passed through unchanged.");
    ImGui::PopStyleColor();
}

const ext::menu::AutoRegister registration { { "ffxiv_jitter", "FFXIV", "Game jitter (FFXIV)", Draw, "output_scaling",
                                                 Visible } };
} // namespace
} // namespace ext::ffxiv::jitter::menu
