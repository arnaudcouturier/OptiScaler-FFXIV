#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/engine/Shafts.h"
#include "ext/game/Game.h"

#include <imgui/imgui.h>

#include <string>

// "Light shafts (FFXIV)" card (tab FFXIV, after the engine pass overrides card; position: ext/core/MenuLayout.h). Every control publishes to
// the render thread (Publish), so a switch turned on in a running session arms the pass and a slider reaches the next tone map: nothing here
// needs a restart, and the shader index is filled whatever the key says.
namespace ext::ffxiv::shafts::menu
{
namespace
{
// Same condition as the engine pass overrides card: the FFXIV build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Light shafts (FFXIV)");

    auto& cfg = ext::Cfg().ffxiv;
    const bool on = cfg.lightShafts.value_or_default();
    bool enabled = on;
    bool changed = false;
    if (ImGui::Checkbox("Light shafts (experimental)", &enabled))
    {
        cfg.lightShafts = enabled;
        changed = true;
    }
    ext::menu::Tooltip("Experimental: the visible effect is not verified in game yet.\n\n"
                       "The sun's light is scattered through the frame: the depth buffer gives the sky, a radial blur carries it toward the\n"
                       "sun's position on screen, and the result is added to a copy of the lit scene that the tone map reads for that one\n"
                       "draw. The game's own scene is only read, never written, and every frame this cannot measure is left exactly as it\n"
                       "was, with one log line for the reason. It stands down for the session after sixty-four of its own draws in a row are\n"
                       "left alone. It is inactive while the engine's scene HDR is on, because that path replaces the tone map this pass\n"
                       "draws before.\n\n"
                       "Judge it with the sun low in the frame, behind trees or a roof edge: shafts are longest when the sun is near the\n"
                       "horizon and the frame has something to break it up.");
    ImGui::BeginDisabled(!on);
    float intensity = cfg.lightShaftsIntensity.value_or_default();
    if (ImGui::SliderFloat("Intensity", &intensity, kMinIntensity, kMaxIntensity, "%.2f"))
    {
        cfg.lightShaftsIntensity = ClampIntensity(intensity);
        changed = true;
    }
    ext::menu::Tooltip("How much light the shafts add, 0 to 2, as a share of the white the pass reads (the tone map's exposure multiplier's\n"
                       "reciprocal, held between 0.15 and the measured 0.838). 0.00 turns the pass off (the frame costs one test per draw, nothing\n"
                       "else); 1.00 is a clear glow around a low sun; 2.00 is the strongest the slider goes - each pixel's room to that white bounds\n"
                       "the addition, so no key pushes a channel past the white the pass reads. A wrong-high live exposure on a much darker frame\n"
                       "still over-lights it, and while the exposure has not landed the white is the captured measurement, 0.838.");
    float decay = cfg.lightShaftsDecay.value_or_default();
    if (ImGui::SliderFloat("Decay", &decay, kMinDecay, kMaxDecay, "%.3f"))
    {
        cfg.lightShaftsDecay = ClampDecay(decay);
        changed = true;
    }
    ext::menu::Tooltip("How fast a sample toward the sun fades, 0.80 to 1.00. Higher keeps the shafts reaching across the frame; lower keeps\n"
                       "them a glow around the sun's own pixels.");
    int samples = cfg.lightShaftsSamples.value_or_default();
    if (ImGui::SliderInt("Samples", &samples, kMinSamples, kMaxSamples))
    {
        cfg.lightShaftsSamples = int32_t(ClampSamples(samples));
        changed = true;
    }
    ext::menu::Tooltip("The blur's sample count, 16 to 96. More samples are smoother and cost more on the GPU; the pass runs at half the\n"
                       "render size.");
    float density = cfg.lightShaftsDensity.value_or_default();
    if (ImGui::SliderFloat("Length", &density, kMinDensity, kMaxDensity, "%.2f"))
    {
        cfg.lightShaftsDensity = ClampDensity(density);
        changed = true;
    }
    ext::menu::Tooltip("How far each sample reaches toward the sun, 0.10 to 1.50. Above 1.00 the blur samples past the sun, which spreads the\n"
                       "shafts over the whole frame.");
    int colour = ParseColour(cfg.lightShaftsColor.value_or_default()) == Colour::White ? 1 : 0;
    const char* colours[] { "Auto (the sun's own colour)", "White (a neutral shaft)" };
    if (ImGui::Combo("Colour", &colour, colours, 2))
    {
        cfg.lightShaftsColor = std::string(colour == 1 ? "white" : "auto");
        changed = true;
    }
    ext::menu::Tooltip("Which colour the shafts take. Auto is the sun's own colour: the engine's colour for that light when it reports one,\n"
                       "and otherwise the sky's colour around the sun's screen position (only sky pixels are sampled, so a tree in front of the\n"
                       "sun cannot tint the shafts), which is what today's build needs - its directional light reports a zero colour. A frame\n"
                       "with no sky around the sun adds nothing. White is a neutral shaft tinted only by the intensity key - the way to tell a\n"
                       "colour problem from a geometry problem: a white shaft in the wrong place is the projection, a coloured one is the\n"
                       "colour.");
    ImGui::EndDisabled();
    if (changed)
        Publish(); // the render thread's copy of the keys and the feature bit, from the next frame

    const Status status = CurrentStatus();
    std::string state;
    if (!on)
        state = "off";
    else if (status.hdrInactive)
        state = "inactive while scene HDR is on: that path replaces the tone map this pass draws before";
    else if (status.intensity <= kIntensityEpsilon)
        state = "off at intensity 0.00: the master switch is on and the pass adds nothing";
    else if (!status.armed)
        state = "off for this session: the pass was left alone (see the log)";
    else
    {
        CardNumbers numbers;
        numbers.intensity = status.intensity;
        numbers.frames = status.frames;
        numbers.runs = status.runs;
        numbers.held = status.held;
        numbers.declined = status.declined;
        numbers.skipped = status.skipped;
        numbers.noDepth = status.noDepth;
        numbers.noSun = status.noSun;
        numbers.offScreen = status.offScreen;
        numbers.belowHorizon = status.belowHorizon;
        numbers.notTheSun = status.notTheSun;
        numbers.reads = status.reads;
        numbers.notReady = status.notReady;
        numbers.refused = status.refused;
        numbers.sunX = status.sunScreen[0];
        numbers.sunY = status.sunScreen[1];
        numbers.windowWidth = status.windowWidth;
        numbers.windowHeight = status.windowHeight;
        state = FormatCard(numbers);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", state.c_str());
    ImGui::PopStyleColor();
}

const ext::menu::AutoRegister registration { { "ffxiv_light_shafts", "FFXIV", "Light shafts (FFXIV)", Draw, "ffxiv_engine_passes", Visible } };
} // namespace
} // namespace ext::ffxiv::shafts::menu
