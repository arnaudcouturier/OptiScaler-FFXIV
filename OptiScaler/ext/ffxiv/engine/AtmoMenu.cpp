#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/engine/Atmo.h"
#include "ext/game/Game.h"

#include "ext/ffxiv/meter/MeterCard.h"
#include <imgui/imgui.h>

#include <string>

// "Atmosphere (FFXIV)" card (tab FFXIV, the engine-pass group; position: ext/core/MenuLayout.h). Every control publishes to the render thread (Publish), so a
// switch turned on in a running session arms the stage and a slider reaches the next tone map: nothing here needs a restart.
namespace ext::ffxiv::engine::atmo::menu
{
namespace
{
// Same condition as the engine pass overrides card: the FFXIV build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Atmosphere (FFXIV)");

    auto& cfg = ext::Cfg().ffxiv;
    const bool on = cfg.atmosphere.value_or_default();
    bool enabled = on;
    bool changed = false;
    if (ImGui::Checkbox("Atmosphere (experimental)", &enabled))
    {
        cfg.atmosphere = enabled;
        changed = true;
    }
    ext::menu::Tooltip("Experimental: the visible effect is not verified in game yet.\n\n"
                       "Aerial perspective: distant surfaces sink into the air in front of them, the way they do on a hazy day. The air is an\n"
                       "exponential height medium (thickest at your own height, thinner above), each surface keeps what its distance lets\n"
                       "through and takes the rest from the air's own light: the engine's fog colour (it matches the horizon sky) and, toward\n"
                       "the sun, a Henyey-Greenstein glow. Sky pixels and everything nearer than the start distance are left exactly as they\n"
                       "are, and the engine's own fog is respected: when the weather already fogs the air, this adds less.\n\n"
                       "It runs at the tone map, after the engine's exposure measure (so eye adaptation does not undo it), between the light\n"
                       "bounce and depth of field. The game's own scene is only read, never written, and every frame this cannot measure is\n"
                       "left exactly as the chain left it, with one log line for the reason. It needs no sun: without one it runs on the\n"
                       "ambient light alone. It stands down for the session after sixty-four of its own draws in a row are left alone, and\n"
                       "it is inactive while the engine's scene HDR is on, because that path replaces the tone map this stage draws before.");
    ImGui::BeginDisabled(!on);
    float density = cfg.atmosphereDensity.value_or_default();
    if (ImGui::SliderFloat("Density", &density, kMinDensity, kMaxDensity, "%.2f /km", ImGuiSliderFlags_Logarithmic))
    {
        cfg.atmosphereDensity = ClampDensity(density);
        changed = true;
    }
    ext::menu::Tooltip("How thick the air is at your own height, per kilometre. 0 is off: the frame costs one test per draw, nothing else.\n"
                       "At the default 1.5 per km (and the 50 m start) a surface 200 m away keeps about 80 percent of its own light, one 500 m away about 51 percent.");
    float falloff = cfg.atmosphereHeightFalloff.value_or_default();
    if (ImGui::SliderFloat("Height falloff", &falloff, kMinFalloff, kMaxFalloff, "%.4f /m"))
    {
        cfg.atmosphereHeightFalloff = ClampFalloff(falloff);
        changed = true;
    }
    ext::menu::Tooltip("How fast the air thins with height above you, per metre: 0.01 is a scale height of 100 m. 0 is a homogeneous haze.\n"
                       "A ray that looks up leaves the thick air sooner and so fogs less; one that looks down into a valley fogs more.");
    float start = cfg.atmosphereStart.value_or_default();
    if (ImGui::SliderFloat("Start distance", &start, kMinStart, kMaxStart, "%.0f m"))
    {
        cfg.atmosphereStart = ClampStart(start);
        changed = true;
    }
    ext::menu::Tooltip("Nothing nearer than this is fogged, whatever the density, so your character and the first metres around you stay clear.");
    float phaseG = cfg.atmospherePhaseG.value_or_default();
    if (ImGui::SliderFloat("Sun glow width (g)", &phaseG, kMinPhaseG, kMaxPhaseG, "%.2f"))
    {
        cfg.atmospherePhaseG = ClampPhaseG(phaseG);
        changed = true;
    }
    ext::menu::Tooltip("The sun lobe's anisotropy (Henyey-Greenstein g): 0 spreads the sun's light over the whole sky, 0.7 is a glow that falls to a\n"
                       "tenth at 30 degrees from the sun, 0.95 a tight one. The glow shows on distant surfaces; the sky itself is not touched.");
    float sun = cfg.atmosphereSunStrength.value_or_default();
    if (ImGui::SliderFloat("Sun strength", &sun, kMinSunStrength, kMaxSunStrength, "%.2f"))
    {
        cfg.atmosphereSunStrength = ClampSunStrength(sun);
        changed = true;
    }
    ext::menu::Tooltip("How much of the frame's white the sun's glow adds, at the sun, where the air is thick. 0 takes the sun's glow away.");
    float sky = cfg.atmosphereSkyStrength.value_or_default();
    if (ImGui::SliderFloat("Sky strength", &sky, kMinSkyStrength, kMaxSkyStrength, "%.2f"))
    {
        cfg.atmosphereSkyStrength = ClampSkyStrength(sky);
        changed = true;
    }
    ext::menu::Tooltip("The ambient light of the air, as a multiple of the engine's own fog colour: 1.0 fades distant surfaces to the horizon's colour.");
    float neutral = cfg.atmosphereNeutral.value_or_default();
    if (ImGui::SliderFloat("Haze neutral", &neutral, kMinNeutral, kMaxNeutral, "%.2f"))
    {
        cfg.atmosphereNeutral = ClampNeutral(neutral);
        changed = true;
    }
    ext::menu::Tooltip("How much of the haze's colour is replaced by a grey of the same brightness: 0 keeps the engine's own fog colour (the cyan of a clear sky),\n"
                       "1.0 is a pure neutral. The far haze of the reference look is a bright near-neutral white.");
    float night = cfg.atmosphereNightDensity.value_or_default();
    if (ImGui::SliderFloat("Night density", &night, kMinNightDensity, kMaxNightDensity, "%.2f"))
    {
        cfg.atmosphereNightDensity = ClampNightDensity(night);
        changed = true;
    }
    ext::menu::Tooltip("The share of the density that stays when the light is the moon's (the light draw's own luminance decides how much of the day\n"
                       "is left): 1.0 keeps all of it, 0 takes the haze away at night.");
    ImGui::EndDisabled();
    if (changed)
        Publish(); // the render thread's copy of the keys and the feature bit, from the next frame

    const Status status = CurrentStatus();
    std::string state;
    if (!on)
        state = "off";
    else if (status.hdrInactive)
        state = "inactive while scene HDR is on: that path replaces the tone map this stage draws before";
    else if (!status.armed)
        state = "off for this session: the stage was left alone (see the log), or the medium is empty";
    else
    {
        CardCounts numbers;
        numbers.frames = status.frames;
        numbers.runs = status.runs;
        numbers.declined = status.declined;
        numbers.skips = status.skips;
        numbers.armed = status.armed;
        numbers.params = status.params;
        state = FormatCard(numbers);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", state.c_str());
    ImGui::PopStyleColor();
    ext::ffxiv::meter::DrawCardLine(ext::ffxiv::meter::StageId::Atmosphere); // the effect meter's "ran X %, visible Y %" (ext/ffxiv/meter)
}

const ext::menu::AutoRegister registration { { "ffxiv_atmosphere", "FFXIV", "Atmosphere (FFXIV)", Draw, "ffxiv_engine_passes", Visible } };
} // namespace
} // namespace ext::ffxiv::engine::atmo::menu
