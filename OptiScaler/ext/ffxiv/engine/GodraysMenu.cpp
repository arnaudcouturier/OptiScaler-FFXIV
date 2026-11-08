#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/engine/Godrays.h"
#include "ext/game/Game.h"

#include "ext/ffxiv/meter/MeterCard.h"
#include <imgui/imgui.h>

#include <string>

// "God rays: volumetric (FFXIV)" card (tab FFXIV, the engine-pass group; position: ext/core/MenuLayout.h). Every control publishes to the
// render thread (Publish), so a switch turned on in a running session arms the march and a slider reaches the next tone map: nothing here
// needs a restart, and the shader index is filled whatever the key says.
namespace ext::ffxiv::godrays::menu
{
namespace
{
// Same condition as the engine pass overrides card: the FFXIV build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("God rays: volumetric (FFXIV)");

    auto& cfg = ext::Cfg().ffxiv;
    const bool on = cfg.godraysVolumetric.value_or_default();
    bool enabled = on;
    bool changed = false;
    if (ImGui::Checkbox("God rays: volumetric (experimental)", &enabled))
    {
        cfg.godraysVolumetric = enabled;
        changed = true;
    }
    ext::menu::Tooltip("Experimental: the visible effect is not verified in game yet.\n\n"
                       "A per-pixel ray march through the air in front of each pixel, at a quarter of the render size: each sample's shadow\n"
                       "is read from the game's own cascade shadow atlas (all four cascades, the map the ground shadows are built from), the\n"
                       "medium from the game's own live fog state, scattered around the shadow light and summed with extinction, and the\n"
                       "result is added to a copy of the lit scene (the sky too, up to its own 99th percentile) that the tone map reads for\n"
                       "that one draw - bounded by the frame's own white, like the radial light shafts beside it. The game's own scene is\n"
                       "only read, never written, and every frame this cannot measure is left exactly as the chain of effects left it, with\n"
                       "one log line for the reason. It stands down for the session after sixty-four of its own draws in a row are left\n"
                       "alone. It is inactive while the engine's scene HDR is on, because that path replaces the tone map this march draws\n"
                       "before.\n\n"
                       "Judge it with the sun low in the frame, behind trees or a roof edge: the beams follow the engine's shadow light,\n"
                       "which can sit above the visible sun's disk, and there are none when that light is behind or beside the camera or dim\n"
                       "(the log says which).");
    ImGui::BeginDisabled(!on);
    int steps = cfg.godraysSteps.value_or_default();
    if (ImGui::SliderInt("Steps", &steps, kMinSteps, kMaxSteps))
    {
        cfg.godraysSteps = int32_t(ClampSteps(steps));
        changed = true;
    }
    ext::menu::Tooltip("The march's sample count, 16 to 96. More samples are smoother and cost more on the GPU; the march runs at a\n"
                       "quarter of the render size by default.");
    int resolution = cfg.godraysResolution.value_or_default() == "half" ? 1 : 0;
    const char* resolutions[] { "Quarter of the render size", "Half of the render size" };
    if (ImGui::Combo("Resolution", &resolution, resolutions, 2))
    {
        cfg.godraysResolution = std::string(resolution == 1 ? "half" : "quarter");
        changed = true;
    }
    ext::menu::Tooltip("The march's own resolution. Quarter is the default and the cheap one; half doubles the taps for a softer term.");
    float fogBoost = cfg.godraysFogBoost.value_or_default();
    if (ImGui::SliderFloat("Fog density", &fogBoost, kMinBoost, kMaxBoost, "%.2f"))
    {
        cfg.godraysFogBoost = ClampBoost(fogBoost);
        changed = true;
    }
    ext::menu::Tooltip("A multiplier on the density the game's own fog profile gives, 0 to 2, on top of the Density slider. 1.00 keeps the\n"
                       "engine's own profile (its shape, its fall-off with height).");
    float heightLift = cfg.godraysHeightLift.value_or_default();
    if (ImGui::SliderFloat("Height lift", &heightLift, kMinBoost, kMaxBoost, "%.2f"))
    {
        cfg.godraysHeightLift = ClampBoost(heightLift);
        changed = true;
    }
    ext::menu::Tooltip("A multiplier on the height falloff of the game's own fog profile, 0 to 2. 1.00 is the engine's own shape; 0.00\n"
                       "flattens it to a uniform haze; 2.00 doubles the height gradient.");
    float intensity = cfg.godraysIntensity.value_or_default();
    if (ImGui::SliderFloat("Intensity", &intensity, kMinIntensity, kMaxIntensity, "%.2f"))
    {
        cfg.godraysIntensity = ClampIntensity(intensity);
        changed = true;
    }
    ext::menu::Tooltip("How much light the march adds, 0 to 2, as a share of the white the pass reads (the tone map's exposure\n"
                       "multiplier's reciprocal, held between 0.15 and the measured 0.838). 0.00 turns the march off (the frame costs one\n"
                       "test per draw, nothing else); each pixel's room to that white bounds the addition, so no key pushes a channel past\n"
                       "the white the pass reads.");
    int colour = 0;
    const std::string colourText = cfg.godraysColor.value_or_default();
    if (ParseColour(colourText) == Colour::White)
        colour = 1;
    else if (ParseColour(colourText) == Colour::EngineFog)
        colour = 2;
    const char* colours[] { "Auto (the sun's own colour)", "White (a neutral beam)", "Engine fog (the game's own fog colour)" };
    if (ImGui::Combo("Colour", &colour, colours, 3))
    {
        cfg.godraysColor = std::string(colour == 1 ? "white" : (colour == 2 ? "enginefog" : "auto"));
        changed = true;
    }
    ext::menu::Tooltip("Which colour the beams take. Auto is the sun's own colour: the engine's colour for that light when it reports one,\n"
                       "and otherwise the sky's colour around the sun's screen position (only sky pixels are sampled, so a tree in front of\n"
                       "the sun cannot tint the beams). White is a neutral beam tinted only by the intensity key - the way to tell a colour\n"
                       "problem from a geometry problem. Engine fog is the game's own fog colour rule: the colour it fogs that very pixel\n"
                       "with, built from its own sky texture and weight LUT.");
    int softness = cfg.godraysShadowSoftness.value_or_default();
    if (ImGui::SliderInt("Shadow softness", &softness, kMinSoftness, kMaxSoftness))
    {
        cfg.godraysShadowSoftness = int32_t(ClampSoftness(softness));
        changed = true;
    }
    ext::menu::Tooltip("The shadow compare's taps: 0 is a point compare (the cheapest), 1 samples two texels at a cascade's boundary and 2\n"
                       "samples four. A cascade's atlas texel spans a world footprint the march reads per sample, and this is the knob that\n"
                       "softens the banding that footprint can show - at the cost of the same multiple in atlas reads.");
    bool jitter = cfg.godraysJitter.value_or_default();
    if (ImGui::Checkbox("Jitter", &jitter))
    {
        cfg.godraysJitter = jitter;
        changed = true;
    }
    ext::menu::Tooltip("The per-pixel offset of the march's samples (the radial shafts' own hash, a function of the pixel alone). Off makes\n"
                       "a still scene perfectly still and shows the march's own lattice; on trades a little noise for smoother beams, which\n"
                       "the upscaler's temporal accumulation then cleans.");
    float phaseBoost = cfg.godraysPhaseBoost.value_or_default();
    if (ImGui::SliderFloat("Phase", &phaseBoost, kMinBoost, kMaxBoost, "%.2f"))
    {
        cfg.godraysPhaseBoost = ClampBoost(phaseBoost);
        changed = true;
    }
    ext::menu::Tooltip("Narrows or widens the glow around the shadow light, 0 to 2. 1.00 is neutral (the Anisotropy slider as it is); higher\n"
                       "concentrates the beams toward the light, lower spreads them; 0.00 is an even glow in every direction.");
    float density = cfg.godraysDensity.value_or_default();
    if (ImGui::SliderFloat("Density", &density, kMinDensity, kMaxDensity, "%.1f"))
    {
        cfg.godraysDensity = ClampDensity(density);
        changed = true;
    }
    ext::menu::Tooltip("How dense the air is for the beams, 0 to 200, in units of 0.001 per metre at the camera's height (20 is 0.02 per\n"
                       "metre). The game's own fog is about 0.001 per metre there, far too thin to show a beam, so this is an artistic\n"
                       "constant: higher makes the beams stronger and darkens the air behind them.");
    float anisotropy = cfg.godraysAnisotropy.value_or_default();
    if (ImGui::SliderFloat("Anisotropy", &anisotropy, kMinAnisotropy, kMaxAnisotropy, "%.2f"))
    {
        cfg.godraysAnisotropy = ClampAnisotropy(anisotropy);
        changed = true;
    }
    ext::menu::Tooltip("How narrow the glow around the shadow light is, 0 to 0.95 (the Henyey-Greenstein g). 0.00 spreads the light evenly\n"
                       "in every direction; 0.80 keeps it within about 25 degrees of the light.");
    ImGui::EndDisabled();
    if (changed)
        Publish(); // the render thread's copy of the keys and the feature bit, from the next frame

    const Status status = CurrentStatus();
    std::string state;
    if (!on)
        state = "off";
    else if (status.hdrInactive)
        state = "inactive while scene HDR is on: that path replaces the tone map this march draws before";
    else if (status.intensity <= kIntensityEpsilon)
        state = "off at intensity 0.00: the master switch is on and the march adds nothing";
    else if (!status.armed)
        state = "off for this session: the march was left alone (see the log)";
    else
    {
        CardNumbers numbers;
        numbers.intensity = status.intensity;
        numbers.frames = status.frames;
        numbers.runs = status.runs;
        numbers.held = status.held;
        numbers.declined = status.declined;
        numbers.skipped = status.skipped;
        numbers.noAtlas = status.noAtlas;
        numbers.noFog = status.noFog;
        numbers.noLight = status.noLight;
        numbers.refused = status.refused;
        numbers.reads = status.reads;
        numbers.notReady = status.notReady;
        numbers.windowWidth = status.windowWidth;
        numbers.windowHeight = status.windowHeight;
        numbers.marchWidth = status.marchWidth;
        numbers.marchHeight = status.marchHeight;
        state = FormatCard(numbers);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", state.c_str());
    ImGui::PopStyleColor();
    ext::ffxiv::meter::DrawCardLine(ext::ffxiv::meter::StageId::Godrays); // the effect meter's "ran X %, visible Y %" (ext/ffxiv/meter)
}

const ext::menu::AutoRegister registration { { "ffxiv_godrays", "FFXIV", "God rays: volumetric (FFXIV)", Draw, "ffxiv_engine_passes", Visible } };
} // namespace
} // namespace ext::ffxiv::godrays::menu
