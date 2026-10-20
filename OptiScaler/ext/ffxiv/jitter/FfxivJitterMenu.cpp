#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/game/Game.h"

#include <imgui/imgui.h>

// "Game jitter (FFXIV)" card (tab FFXIV, after the FFXIV HDR10 card; position: ext/core/MenuLayout.h): the two [FFXIV] jitter keys as live switches.
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
    ext::menu::Tooltip("Learns how much of the jitter the game reports it really applies (it measured 0.6 in this game),\n"
                       "then hands DLSS the reported jitter scaled by that ratio. Applies at once; the log line\n"
                       "\"Jitter correction: calibrated ratio ...\" says when it is ready.\n\n"
                       "One write feeds super resolution, frame generation, NR and the Jitter Count readout: to compare,\n"
                       "look at the picture with frame generation off first. If something looks wrong, untick it.");

    float scale = cfg.jitterScaleOverride.value_or_default();
    ImGui::BeginDisabled(!correct);
    if (ImGui::SliderFloat("Manual jitter scale (0 = learn it)", &scale, 0.0f, 1.0f, scale > 0.0f ? "%.2f" : "Off"))
        cfg.jitterScaleOverride = scale;
    ImGui::EndDisabled();
    ext::menu::Tooltip("Experiment. Hands DLSS a fixed fraction of the jitter the game reports instead of the ratio the correction learns.\n"
                       "Try 1.0 (changes nothing), 0.8, 0.6 and 0.5 one after the other and compare the picture. Valid values are 0.25 to 1.0,\n"
                       "0 learns the ratio, any other value is ignored. It is only used while the correction switch above is ticked.\n"
                       "Applies at once. Look at the picture with frame generation off first.");

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("The log line \"Jitter correction: on; totals since the start of the session ...\" shows how many evaluates were scaled and how many passed through unchanged, and the calibrated ratio.");
    ImGui::PopStyleColor();
}

const ext::menu::AutoRegister registration { { "ffxiv_jitter", "FFXIV", "Game jitter (FFXIV)", Draw, "output_scaling",
                                                 Visible } };
} // namespace
} // namespace ext::ffxiv::jitter::menu
