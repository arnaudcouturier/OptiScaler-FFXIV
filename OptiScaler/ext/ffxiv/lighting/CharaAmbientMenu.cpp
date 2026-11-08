#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/engine/EnginePass.h" // the engine's ambient knob, the same seam's shipped card
#include "ext/ffxiv/engine/Godrays.h"    // the march, the lighting tab's air-light stage
#include "ext/ffxiv/engine/Shafts.h"     // the light shafts card, its sibling
#include "ext/ffxiv/lighting/CharaAmbientApply.h"
#include "ext/ffxiv/shadows/ShadowsHooks.h" // the shadow mapping module's night ambient lift, the seams' shared curve
#include "ext/ffxiv/shadows/ShadowsRules.h"
#include "ext/game/Game.h"

#include <imgui/imgui.h>

#include <string>

// "Lighting (FFXIV)" card (tab FFXIV, right after the shadow mapping card; position: ext/core/MenuLayout.h). One card group listing the
// lighting levers this fork ships, with status lines and the one rule they share: the light shafts stay inactive while the engine's scene HDR
// is on. The card's own controls are the chara ambient - the character-at-night knob - and everything else it shows is the status of levers
// with their own cards, so no card's keys are duplicated here and every master arming is the one its own card carries.
namespace ext::ffxiv::lighting::chara::menu
{
namespace
{
// Same condition as the engine pass overrides card: the FFXIV build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void PublishAll()
{
    ext::ffxiv::lighting::chara::Publish(ext::Cfg().ffxiv.charaAmbient.value_or_default());
}

// The levers' one-line states, as their own cards show them. A lever that is off reads "off"; a lever on and at its idle reads "at the engine's
// own picture"; a lever armed names its counters the way its own card would.
std::string AmbientKnobLine(const ext::ffxiv::engine::Status& engine)
{
    if (!engine.armed && engine.identity)
        return "ambient sky visibility (the engine pass overrides card): off";
    if (engine.identity)
        return "ambient sky visibility (the engine pass overrides card): at the engine's own picture";
    return std::format("ambient sky visibility (the engine pass overrides card): armed, {} ambient patch(es) ran ({} wetness patch(es) on {} binds), {} draw(s) left alone",
                       engine.runs, engine.wetnessRuns, engine.wetnessSwaps, engine.declined);
}

std::string NightLiftLine()
{
    auto& cfg = ext::Cfg().ffxiv;
    if (!cfg.shadowMapping.value_or_default())
        return "night ambient lift (the shadow mapping card): off";
    const bool liftAsk = !ext::ffxiv::shadows::IsAmbientIdentity(ext::ffxiv::shadows::ClampAmbientLift(cfg.nightAmbientLift.value_or_default()));
    if (!liftAsk)
        return "night ambient lift (the shadow mapping card): on at the engine's own picture";
    const bool wants = ext::ffxiv::shadows::Hooks::ambientWants != nullptr && ext::ffxiv::shadows::Hooks::ambientWants();
    if (wants)
        return "night ambient lift (the shadow mapping card): armed on the composite draw";
    return "night ambient lift (the shadow mapping card): on, waiting for the seam to offer the composite draw";
}

std::string ShaftsLine(const ext::ffxiv::shafts::Status& shafts)
{
    if (!shafts.enabled)
        return "light shafts (the light shafts card): off";
    if (shafts.hdrInactive)
        return "light shafts (the light shafts card): inactive while scene HDR is on";
    if (!shafts.armed)
        return "light shafts (the light shafts card): off for this session (see the log)";
    return std::format("light shafts (the light shafts card): armed, {} composite(s) this pass added to, {} declining (see the log)",
                       shafts.runs, shafts.declined);
}

std::string GodraysLine(const ext::ffxiv::godrays::Status& march)
{
    if (!march.enabled)
        return "the godrays march (the godrays card): off";
    if (march.hdrInactive)
        return "the godrays march (the godrays card): inactive while scene HDR is on";
    if (!march.armed)
        return "the godrays march (the godrays card): off for this session (see the log)";
    return std::format("the godrays march (the godrays card): armed, {} tone-map stage(s) this march composited into, {} declined",
                       march.runs, march.declined);
}

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Lighting (FFXIV)");

    auto& cfg = ext::Cfg().ffxiv;
    const bool on = cfg.charaAmbient.value_or_default();
    bool enabled = on;
    bool changed = false;
    if (ImGui::Checkbox("Chara ambient lift (experimental)", &enabled))
    {
        cfg.charaAmbient = enabled;
        changed = true;
    }
    ext::menu::Tooltip("Experimental: the visible effect is not verified in game yet.\n\n"
                       "The game's own character draws bind one ambient block of their own (g_AmbientParam, a 160-byte table: the block's sky\n"
                       "SH rows, its colour, and a scalar - the block's reciprocal scale - that every ambient term the character's shader reads\n"
                       "is multiplied with). The lift scales that scalar for exactly those draws: at night, characters read their own local\n"
                       "fill; in day the engine's own block comes back byte for byte. A character draw is a shader that declares the\n"
                       "customize block beside the ambient block; the world's materials declare the ambient block alone and stay as they are.\n\n"
                       "The lift keys on the frame's own light colour, the same day/night curve the shadow mapping card's levers use (the\n"
                       "colour is read no-wait from the sun's light draw; its two luminance thresholds are provisional until the\n"
                       "five-stop capture runs). Every state the module cannot measure is left exactly as it was, with one log line for the\n"
                       "reason. It stands down for the session after sixty-four of its own draws in a row are left alone. Nothing of the game\n"
                       "is written and no buffer of the game is ever mapped; the game's own binding comes back after every draw.");
    ImGui::BeginDisabled(!on);
    float lift = cfg.charaAmbientLift.value_or_default();
    if (ImGui::SliderFloat("Chara ambient lift", &lift, kMinCharaLift, kMaxCharaLift, "%.2f"))
    {
        cfg.charaAmbientLift = ClampCharaLift(lift);
        changed = true;
    }
    ext::menu::Tooltip("The night multiplier on the character ambient block's reciprocal scale, 0.5 to 2.0. 1.00 is the engine's own picture\n"
                       "and runs nothing at all; below 1.00 dims the characters' own ambient fill (the engine's notion of the local light),\n"
                       "above 1.00 lifts it. The block's colour bytes and every term beside the scalar keep their bits, so the knob never\n"
                       "tints the characters on its own. The day/night curve applies this key too.");
    ImGui::EndDisabled();
    if (changed)
        PublishAll();

    // The card's own status line, the state a session reads first.
    const Status status = CurrentStatus();
    std::string state;
    if (!on)
        state = "chara ambient: off";
    else if (status.identity)
        state = "chara ambient: on at the engine's own picture (the lift is 1.00; nothing runs)";
    else if (status.off)
        state = "chara ambient: off for this session (see the log)";
    else if (!status.haveReading)
        state = "chara ambient: armed, the curve is still waiting for the frame's own light colour (the engine's own picture until then)";
    else
        state = std::format("chara ambient: armed, last lift {:.3f} under curve {:.3f} ({} patch(es) ran, {} bind(s) served from the cache, "
                            "{} declined)",
                            double(status.lastScale), double(status.curve), status.runs, status.served, status.declined);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", state.c_str());
    ImGui::PopStyleColor();

    // The assembly: the levers of the lighting look, each with its own card and its own keys; the rule they share stated once, the way the
    // light shafts card states it.
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", AmbientKnobLine(ext::ffxiv::engine::CurrentStatus()).c_str());
    ImGui::TextWrapped("%s", NightLiftLine().c_str());
    ImGui::TextWrapped("%s", ShaftsLine(ext::ffxiv::shafts::CurrentStatus()).c_str());
    ImGui::TextWrapped("%s", GodraysLine(ext::ffxiv::godrays::CurrentStatus()).c_str());
    ImGui::TextWrapped("Shared rule: the light shafts (and the march chained after them) stay inactive while the engine's scene HDR is on - "
                       "that path replaces the tone chain those passes draw before, and it comes back when the HDR path does.");
    ImGui::TextWrapped("The curve keys on the frame's own g_LightParam colour, its two threshold values are provisional until the five-stop "
                       "capture protocol runs; the ini defaults stay as they are until then.");
    ImGui::PopStyleColor();
}

const ext::menu::AutoRegister registration { { "ffxiv_lighting", "FFXIV", "Lighting (FFXIV)", Draw, "ffxiv_shadow_mapping", Visible } };
} // namespace
} // namespace ext::ffxiv::lighting::chara::menu
