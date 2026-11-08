#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/enginelight/EngineLight.h"
#include "ext/game/Game.h"

#include "ext/ffxiv/meter/MeterCard.h"
#include <imgui/imgui.h>

#include <format>
#include <string>

// "Engine light (FFXIV)" card (tab FFXIV, the engine-pass group; position: ext/core/MenuLayout.h). Every control publishes to the render thread (Publish), so a switch turned on in
// a running session arms the module and a slider reaches the next frame: nothing here needs a restart. The "engine's own light" box is the live A/B: every arm shows the engine's own
// value (every factor 1, no compensation) while it is ticked, and the exposure latch keeps following the engine meanwhile, so unticking it puts the look back on a fresh latch.
namespace ext::ffxiv::enginelight::menu
{
namespace
{
// Same condition as the other engine-pass cards: the FFXIV build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

bool Slider(const char* label, CustomOptional<float>& key, float lo, float hi, const char* format = "%.2f")
{
    float value = key.value_or_default();
    if (ImGui::SliderFloat(label, &value, lo, hi, format))
    {
        key = ClampOr(value, lo, hi, key.value_or_default());
        return true;
    }
    return false;
}

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Engine light (FFXIV)");

    auto& cfg = ext::Cfg().ffxiv;
    bool changed = false;

    bool master = cfg.engineLight.value_or_default();
    if (ImGui::Checkbox("Engine light (experimental)", &master))
    {
        cfg.engineLight = master;
        changed = true;
    }
    ext::menu::Tooltip("Experimental: the visible effect is judged in game.\n\n"
                       "Changes the light the ENGINE computes at night, at the source: the moon, the ambient, the water's own copies of both, and the point lights (lamps). The\n"
                       "engine's eye adaptation would raise its exposure when the night gets darker and undo most of it, so an exposure hold rescales the picture back to the\n"
                       "exposure the engine had on its own light. Everything is weighted by the shared night curve (the visible sun's height and the sun draw's light colour), so a\n"
                       "daylight frame is the engine's own picture byte for byte, and the controls fade in over a second at night, after the engine's exposure has settled.\n\n"
                       "It asks the Shadow mapping module to keep the light-colour feed running while it is on, whatever Shadow lift and Night ambient lift say. It is inactive\n"
                       "while the engine's scene HDR is on.");

    ImGui::BeginDisabled(!master);
    bool reference = cfg.engineLightReferenceAB.value_or_default();
    if (ImGui::Checkbox("Show the engine's own light (A/B)", &reference))
    {
        cfg.engineLightReferenceAB = reference;
        changed = true;
    }
    ext::menu::Tooltip("Ticked: every arm shows the engine's own value for as long as the box is ticked (every factor 1, no exposure compensation), so one launch compares the\n"
                       "two looks. The exposure latch keeps following the engine meanwhile: unticking the box is a fresh latch on the engine's own settled exposure.");

    changed |= Slider("Moon", cfg.engineLightMoon, kMoonMin, kMoonMax);
    ext::menu::Tooltip("The moon's colour multiplier at night (the directional light's diffuse and specular colour). 1.00 is the engine's own; the default 0.10 is the measured\n"
                       "distance to the reference look (low-to-medium confidence).");
    changed |= Slider("Ambient", cfg.engineLightAmbient, kAmbientMin, kAmbientMax);
    ext::menu::Tooltip("The ambient (the spherical-harmonics probe table) multiplier. 1.00 is the engine's own. While Engine light is on this arm owns the composite ambient:\n"
                       "Shadow mapping's Night ambient lift yields (no double application).");
    changed |= Slider("Water: moon", cfg.engineLightWaterMoon, kWaterMoonMin, kWaterMoonMax);
    ext::menu::Tooltip("The water pass's own moon term. It REPLACES the Moon slider on the water pass - they do not stack: at 1.00 the water keeps the engine's own moon\n"
                       "whatever the Moon slider says.");
    changed |= Slider("Water: ambient", cfg.engineLightWaterAmbient, kWaterAmbientMin, kWaterAmbientMax);
    ext::menu::Tooltip("The water's own ambient block. 1.00 is the engine's own.");
    changed |= Slider("Lamps", cfg.engineLightLamps, kLampMin, kLampMax);
    ext::menu::Tooltip("The point lights' contribution multiplier (every lamp volume in both light passes). 1.00 is the engine's own; applied through the blend factor of the\n"
                       "lamp draws, so it needs no knowledge of their constants.");

    if (ImGui::TreeNode("Tints"))
    {
        changed |= Slider("Moon tint R", cfg.engineLightMoonTintR, kTintMin, kTintMax);
        changed |= Slider("Moon tint G", cfg.engineLightMoonTintG, kTintMin, kTintMax);
        changed |= Slider("Moon tint B", cfg.engineLightMoonTintB, kTintMin, kTintMax);
        changed |= Slider("Ambient tint R", cfg.engineLightAmbientTintR, kTintMin, kTintMax);
        changed |= Slider("Ambient tint G", cfg.engineLightAmbientTintG, kTintMin, kTintMax);
        changed |= Slider("Ambient tint B", cfg.engineLightAmbientTintB, kTintMin, kTintMax);
        changed |= Slider("Water tint R", cfg.engineLightWaterTintR, kTintMin, kTintMax);
        changed |= Slider("Water tint G", cfg.engineLightWaterTintG, kTintMin, kTintMax);
        changed |= Slider("Water tint B", cfg.engineLightWaterTintB, kTintMin, kTintMax);
        ext::menu::Tooltip("Per-channel multipliers on the moon colour, the ambient and the water's ambient block (toward neutral at 1.00).");
        ImGui::TreePop();
    }

    static const char* const kModes[] { "Off (the engine adapts freely)", "Auto hold", "Manual factor" };
    int mode = ClampMode(cfg.engineLightExposure.value_or_default());
    if (ImGui::Combo("Exposure", &mode, kModes, 3))
    {
        cfg.engineLightExposure = mode;
        changed = true;
    }
    ext::menu::Tooltip("Off: the engine's eye adaptation runs freely (it takes back part of the darkening).\n"
                       "Auto hold: the picture keeps the exposure the engine had on its own light, latched after it has settled (a few seconds when the night starts), rescaled at the tone\n"
                       "map by s_hold / s_now. A/B and master toggles re-latch for free.\n"
                       "Manual factor: a fixed multiplier on the scene, eased in with the night weight.");
    if (mode == kExposureManual)
        changed |= Slider("Manual factor", cfg.engineLightExposureManual, kManualMin, kManualMax);
    ImGui::EndDisabled();
    if (changed)
        Publish(); // the render thread's copy of the keys and the feature bit, from the next frame

    const Status status = CurrentStatus();
    std::string state;
    if (!master)
        state = "off";
    else if (status.hdrInactive)
        state = "inactive while scene HDR is on: that path replaces the draws this module patches";
    else
    {
        state = std::format("{} - weight {:.2f} (night curve {:.2f}, {}); lamps x{:.2f}", PhaseName(status.phase), double(status.weight), double(status.rawTarget), status.arm,
                            double(status.lampFactor));
        if (status.params.referenceAB)
            state += " [showing the engine's own light]";
        state += status.haveS ? std::format("\nexposure s {:.3f} (y {:.3f})", double(status.s), double(status.y)) : std::string("\nexposure not read yet");
        state += status.haveHold ? std::format(", hold {:.3f}{}", double(status.sHold), status.forcedLatch ? " (latched before the engine had settled)" : "") : std::string(", hold not latched yet");
        state += std::format(", compensation x{:.3f}", double(status.gain));
        if (status.standDown)
            state += "\nthe exposure stage stood itself down for this session (see the log); the lamps and the light arms go on";
        else
            state += status.haveGpuMs ? std::format("; hold pass {:.3f} ms GPU", status.gpuMs) : std::string("; hold pass GPU time not read yet");
        if (status.haveTable)
            state += std::format("\nluminance table [128, 512, 896] {:.3f} {:.3f} {:.3f}", double(status.tableLow), double(status.tableMid), double(status.tableHigh));
        state += std::format("\nlamp draws scaled {}", status.lampsScaled);
        state += "\nNight ambient lift (Shadow mapping) is overridden while Engine light is on: the composite ambient belongs to the Ambient arm above";
        if (publishCbFamily == nullptr)
            state += "\nthe moon / ambient / water patches are not part of this build: only the lamps and the exposure act";
        if (!status.haveReading)
            state += "\nno night reading yet (it needs the sun draw's light colour or the fog sun: see Shadow mapping)";
        else if (status.feedMasterOff)
            state += "\nShadow mapping's master switch is off: the night curve is fed anyway";
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", state.c_str());
    ImGui::PopStyleColor();
    ext::ffxiv::meter::DrawCardLine(ext::ffxiv::meter::StageId::NightGrade); // the effect meter's "ran X %, visible Y %" (ext/ffxiv/meter)
}

const ext::menu::AutoRegister registration { { "ffxiv_engine_light", "FFXIV", "Engine light (FFXIV)", Draw, "ffxiv_engine_passes", Visible } };
} // namespace
} // namespace ext::ffxiv::enginelight::menu
