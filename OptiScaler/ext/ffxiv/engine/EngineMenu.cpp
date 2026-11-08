#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/engine/EnginePass.h"
#include "ext/game/Game.h"

#include <imgui/imgui.h>

#include <cstdio>

// "Engine pass overrides (FFXIV)" card (tab FFXIV, after the contact shadow card; position: ext/core/MenuLayout.h). Every control publishes
// its value to the render thread (Publish), so a switch turned on in a running session arms the draw path and a slider reaches the next draw of
// its pass: the shader index is filled whatever the key says, because the game creates its shaders while it loads. Nothing here needs a
// restart.
namespace ext::ffxiv::engine::menu
{
namespace
{
// Same condition as the character preview card: the FFXIV build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Engine pass overrides (FFXIV)");

    auto& cfg = ext::Cfg().ffxiv;
    const bool on = cfg.enginePassOverrides.value_or_default();
    bool overrides = on;
    bool changed = false;
    if (ImGui::Checkbox("Engine pass overrides (experimental)", &overrides))
    {
        cfg.enginePassOverrides = overrides;
        changed = true;
    }
    ext::menu::Tooltip("Experimental: the visible effect is not verified in game yet.\n\n"
                       "Lets this fork change one of the engine's own rendering parameters for a single draw, without touching the game's\n"
                       "memory: the draw is recognised by the parameter buffers its shader declares, the buffer is copied on the GPU, scaled or\n"
                       "set, bound for that one draw and the game's own buffer is put back. Every screen the module cannot measure is left\n"
                       "exactly as it was, with one log line for the reason, and it stands down for the session after sixty-four of one knob's\n"
                       "draws in a row are refused.\n\n"
                       "The first knob is the ambient sky visibility: how much of the sky's light each of the 64 ambient probes adds to the\n"
                       "scene. Lower values darken the flat sky-lit fill of a night without touching the artists' baked values, the direct\n"
                       "light or the shadows. 1.00 is the engine's own picture and runs nothing at all.\n\n"
                       "The second knob forces the engine's wetness, the value that makes the ground draw wet and glossy; a dry stone plaza is\n"
                       "where its effect can be judged. -1.00 is the engine's own value and runs nothing at all.");
    float scale = cfg.ambientSkyVisibilityScale.value_or_default();
    ImGui::BeginDisabled(!on);
    if (ImGui::SliderFloat("Ambient sky visibility", &scale, kMinAmbientScale, kMaxAmbientScale, "%.2f"))
    {
        cfg.ambientSkyVisibilityScale = ClampAmbientScale(scale);
        changed = true;
    }
    ext::menu::Tooltip("A multiplier on every ambient probe's sky visibility, 0 to 2. 1.00 is the vanilla picture (and the pass does not run);\n"
                       "0.70 is a visible darkening of the sky-lit fill; 0 removes the sky's contribution entirely, which is far beyond what the\n"
                       "artists intended and a stretch of a comparison, not a setting to play with.");
    float wetness = cfg.wetnessOverride.value_or_default();
    if (ImGui::SliderFloat("Wetness override (experimental)", &wetness, kMinWetnessOverride, kMaxWetnessOverride, "%.2f"))
    {
        cfg.wetnessOverride = ClampWetnessOverride(wetness);
        changed = true;
    }
    ext::menu::Tooltip("The engine's wetness: how wet the ground is drawn. -1.00 is the engine's own value from the weather and runs nothing at\n"
                       "all; 0.00 forces dry ground and 1.00 full wetness whatever the weather is, which is what a dry stone plaza needs to show\n"
                       "puddles and gloss. Grass has no gloss response: judge it on stone. The engine's own rain-exposure mask still decides where\n"
                       "the ground is sheltered, so the gloss appears under open sky and not under a canopy.");
    ImGui::EndDisabled();
    if (changed)
        Publish(overrides); // the render thread's copy of the keys and the feature bit, from the frame after this

    const Status status = CurrentStatus();
    // The shader index's own state is appended to the card's line when its cap refused a bytecode: a session then reads why a shader created
    // late (a water material, the composite of a new zone) is not patched, instead of seeing only "left alone".
    char index[96];
    if (status.indexRefused != 0)
        std::snprintf(index, sizeof index, "; the shader index refused %u bytecode(s) at its cap", status.indexRefused);
    else
        index[0] = '\0';
    char state[400];
    if (!on)
        std::snprintf(state, sizeof state, "off");
    else if (status.identity)
        std::snprintf(state, sizeof state, "on, but both sliders are at the engine's own values, so nothing is patched%s", index);
    else if (status.armed)
        std::snprintf(state, sizeof state,
                      "on: ambient %.2f on %u composite draws, wetness %.2f on %u patches over %u material draws; %u left alone so far%s",
                      double(CurrentParams().ambientScale), status.runs,
                      double(WetnessAmount(CurrentParams().wetnessOverride)), status.wetnessRuns, status.wetnessSwaps, status.declined, index);
    else
        std::snprintf(state, sizeof state, "off for this session: the draws were left alone (see the log)%s", index);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", state);
    ImGui::PopStyleColor();
}

const ext::menu::AutoRegister registration { { "ffxiv_engine_passes", "FFXIV", "Engine pass overrides (FFXIV)", Draw, "output_scaling",
                                               Visible } };
} // namespace
} // namespace ext::ffxiv::engine::menu
