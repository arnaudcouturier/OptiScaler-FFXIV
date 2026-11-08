#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/engine/EnginePass.h" // the seam's arming chain and its card's counters
#include "ext/hdr/scene/FfxivSceneHdr.h" // the scene-HDR state line (the shafts rule the module follows)
#include "ext/ffxiv/shadows/ShadowsHooks.h"
#include "ext/ffxiv/shadows/ShadowsRules.h" // the clamp and identity rules the sliders behave by
#include "ext/game/Game.h"

#include "ext/ffxiv/meter/MeterCard.h"
#include <imgui/imgui.h>

#include <string>

// "Shadow mapping (FFXIV)" card (tab FFXIV, right after the engine pass overrides card; position:
// ext/core/MenuLayout.h). The switch and the three tuning keys publish to the module through the shadow mapping
// hook pointer (ext/ffxiv/shadows/ShadowsDispatch.cpp OnConfigLoaded), which re-asks the engine seam's feature bit,
// so a switch turned on in a running session arms both levers and a slider reaches the next frame. The identity
// answer to every default: the module copies nothing, patches nothing, draws nothing.
namespace ext::ffxiv::shadows::menu
{
namespace
{
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void PublishAll()
{
    ext::ffxiv::shadows::OnConfigLoaded(ext::Cfg().ffxiv.shadowMapping.value_or_default());
}

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Shadow mapping (FFXIV)");

    auto& cfg = ext::Cfg().ffxiv;
    const bool on = cfg.shadowMapping.value_or_default();
    bool enabled = on;
    bool changed = false;
    if (ImGui::Checkbox("Shadow mapping (experimental)", &enabled))
    {
        cfg.shadowMapping = enabled;
        changed = true;
    }
    ext::menu::Tooltip("Experimental: the visible effect is retuned by an in-game session, not yet verified in game.\n\n"
                       "Two levers at the game's own deferred light, both at their idle by default:\n\n"
                       "1. The engine's screen-space sun/shadow mask - the R8 texture the light pass squares and\n"
                       "multiplies (the squared mask is the shadow's own visibility term) - gets a lifted shadowed\n"
                       "floor at the light draws. The key applies to the value the square shares, so it reads\n"
                       "linearly in the picture, and a lift of 0 is the engine's own mask byte for byte. An optional\n"
                       "tent feathers the lifted mask's edges. The lift shares one copy per frame with the contact\n"
                       "shadows pass when that feature is armed and runs its own copy at the light draws when it is\n"
                       "not. Before DLSS, before the tone map, before the UI.\n\n"
                       "2. The deferred composite's ambient probe table gets a night lift: the header's global\n"
                       "sky-SH scale - the night term the shipped ambient knob never moves - and the per-probe SH\n"
                       "reciprocal scale, in ONE patch dispatch on the composite draw (the same place the engine\n"
                       "pass overrides knobs patch, so the two compose).\n\n"
                       "Both levers key on the visible sun's elevation as the fog post states it (no lift while the sun\n"
                       "is above 1 degree below the horizon, full by 7 degrees below it); when no fog sun has landed\n"
                       "recently they fall back to the main view's light colour (g_LightParam's m_DiffuseColor, thresholds\n"
                       "provisional). Every state the module cannot measure is left\n"
                       "exactly as it was, with one log line for the reason. Standing rules: no game resource is ever\n"
                       "written, nothing waits on the GPU, and the engine's scene HDR leaves the module inert (that\n"
                       "path replaces the tone chain this module draws before).");
    if (changed)
        PublishAll();
    ImGui::BeginDisabled(!on);
    float lift = cfg.shadowLift.value_or_default();
    if (ImGui::SliderFloat("Shadow lift", &lift, ext::ffxiv::shadows::kMinLift, ext::ffxiv::shadows::kMaxLift, "%.2f"))
    {
        cfg.shadowLift = ext::ffxiv::shadows::ClampLift(lift);
        changed = true;
    }
    ext::menu::Tooltip("How much of the mask's unlit depth the light term gives back, 0 to 0.9 as the share the m^2\n"
                       "remap adds. 0.00 is the engine's own mask (and runs nothing at all); the night curve scales\n"
                       "this key, so a day frame is the engine's own picture whatever it says.");
    float softness = cfg.shadowEdgeSoftness.value_or_default();
    if (ImGui::SliderFloat("Edge softness", &softness, ext::ffxiv::shadows::kMinSoftness, ext::ffxiv::shadows::kMaxSoftness,
                           "%.1f px"))
    {
        cfg.shadowEdgeSoftness = ext::ffxiv::shadows::ClampSoftness(softness);
        changed = true;
    }
    ext::menu::Tooltip("How far the lifted mask's edges feather, 0 to 2 mask pixels: the tent's reach over the\n"
                       "lifted copy (a reach of at most one pixel reads the 3x3 kernel, two the 5x5 one). 0.0 keeps\n"
                       "the mask's own edges; a lift of 0 is the byte-exact passthrough whatever this says - the\n"
                       "softness feathers the lift only.");
    float ambient = cfg.nightAmbientLift.value_or_default();
    if (ImGui::SliderFloat("Night ambient lift", &ambient, ext::ffxiv::shadows::kMinAmbientLift, ext::ffxiv::shadows::kMaxAmbientLift,
                           "%.2f"))
    {
        cfg.nightAmbientLift = ext::ffxiv::shadows::ClampAmbientLift(ambient);
        changed = true;
    }
    ext::menu::Tooltip("The night multiplier on the composite's ambient table, 0.25 to 4.0: the header's global\n"
                       "sky-SH scale (the term the shipped ambient knob never moves - the night's dominant fill) and\n"
                       "the per-probe SH reciprocal scale. 1.00 is the engine's own picture and runs nothing at all;\n"
                       "above 1.0 brightens the night's fill, below 1.0 darkens it. The shipped ambient knob's scale\n"
                       "composes with this one (one dispatch, the multipliers multiply). The night curve applies it too.");
    bool curveToggle = cfg.nightCurveToggle.value_or_default();
    if (ImGui::Checkbox("Key on the visible sun (fog sun, else the light colour)", &curveToggle))
    {
        cfg.nightCurveToggle = curveToggle;
        changed = true;
    }
    ext::menu::Tooltip("The day/night curve that scales both keys: the visible sun's elevation from the fog post (the\n"
                       "capture ring reads it without the CPU ever waiting), falling back to the frame's own g_LightParam\n"
                       "colour when no fog sun has landed recently. Full effect at night, zero in day, a smoothstep\n"
                       "between 1 and 7 degrees below the horizon. A dim, moon-coloured light with the fog row's body\n"
                       "1 to 7 degrees or more ABOVE the horizon is the moon: night too. A bright light is day whatever\n"
                       "the fog row says. ON is correct for\n"
                       "a game session; OFF pins the full effect at all hours, the debugging switch that makes an\n"
                       "A/B at noon possible before the capture has run.");
    if (changed)
        PublishAll();
    ImGui::EndDisabled();

    // The state line: the module's own counters (the mask route's draws, the light colour copies and reads that landed, the
    // composite dispatches that carried the ambient entries), not the seam's "asks". One line per change of state in the log; the
    // card repeats what a session reads first. A key at its idle value says so, and says nothing runs.
    const ext::ffxiv::shadows::Status status = ext::ffxiv::shadows::CurrentStatus();
    ext::ffxiv::shadows::StateFacts facts;
    facts.keyOn = on;
    facts.sceneHdr = ext::Cfg().hdr.ffxivHdr.value_or_default() && ext::hdr::FfxivSceneHdr::Requested();
    facts.liftKeyOff = status.liftKeyOff;
    facts.ambientKeyOff = status.ambientKeyOff;
    facts.standDown = status.standDown;
    facts.routeDraws = status.routeDrawsSeen;
    facts.routeRuns = status.routeRuns;
    facts.colourCopies = status.colourCopies;
    facts.colourReads = status.colourReads;
    facts.colourNotReady = status.colourNotReady;
    facts.ambientPatched = ext::ffxiv::engine::CurrentStatus().shadowsRuns;
    facts.curve = status.curve;
    facts.haveReading = status.haveReading;
    const std::string state = ext::ffxiv::shadows::FormatStateLine(facts);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", state.c_str());
    ImGui::PopStyleColor();
    ext::ffxiv::meter::DrawCardLine(ext::ffxiv::meter::StageId::ShadowLift); // the effect meter's "ran X %, visible Y %" (ext/ffxiv/meter)
}

const ext::menu::AutoRegister registration { { "ffxiv_shadow_mapping", "FFXIV", "Shadow mapping (FFXIV)", Draw,
                                              "ffxiv_engine_passes", Visible } };
} // namespace
} // namespace ext::ffxiv::shadows::menu
