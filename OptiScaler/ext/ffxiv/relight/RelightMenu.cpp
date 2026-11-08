#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/relight/ContactShadowsRules.h"
#include "ext/ffxiv/relight/Inject.h"
#include "ext/game/Game.h"

#include <imgui/imgui.h>

#include <cstdio>

// "Contact shadows (FFXIV)" card (tab FFXIV, after the FFXIV HDR10 card; position: ext/core/MenuLayout.h). Every control publishes its
// value to the render thread (Publish), so a switch turned on in a running session arms the pass and the sliders reach the next sun light
// draw: the reflection index is filled whatever the key says, because the game creates its light shaders while it loads. Nothing here needs
// a restart. The two keys that say how the game reports its sun vector are ini only: they exist for a build where the measurement has to be
// redone, not for a player to guess at.
namespace ext::ffxiv::relight::menu
{
namespace
{
// Same condition as the character preview card: the FFXIV build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Contact shadows (FFXIV)");

    auto& cfg = ext::Cfg().ffxiv;
    const bool on = cfg.contactShadows.value_or_default();
    bool shadows = on;
    bool changed = false;
    if (ImGui::Checkbox("Contact shadows (experimental)", &shadows))
    {
        cfg.contactShadows = shadows;
        changed = true;
    }
    ext::menu::Tooltip("Experimental: the effect is not verified in game. On the one benchmark frame that was dumped, the term mostly did\n"
                       "not land on contact edges (3.0 % of its texels were on one), so treat what you see as a test, not as a feature.\n\n"
                       "Short shadows where a character or a prop meets the ground, added to the game's own sun shadow before the\n"
                       "lighting pass runs (so before DLSS, the tone map and the UI). The game's cascade shadow map cannot resolve\n"
                       "those few centimetres; this marches half a metre along the sun direction over the depth buffer and multiplies\n"
                       "the result into a copy of the engine's shade mask, which only that one draw samples.\n\n"
                       "Off by default. Nothing changes on a screen the pass cannot measure (a new game build whose shader layout it\n"
                       "does not recognise, a resolution it has not seen): it then leaves every draw alone and says so once in the log.\n"
                       "It also stands down for the session after sixteen refused light draws in a row.");

    float length = cfg.contactShadowsLength.value_or_default();
    ImGui::BeginDisabled(!on);
    if (ImGui::SliderFloat("Length (metres)", &length, kMinLength, kMaxLength, "%.2f m"))
    {
        cfg.contactShadowsLength = ClampLength(length);
        changed = true;
    }
    ext::menu::Tooltip("How far from the surface the ray looks for an occluder. Long enough to catch a foot or a crate edge, short\n"
                       "enough to stay inside the engine's own shadows: half a metre is the default.");
    float strength = cfg.contactShadowsStrength.value_or_default();
    if (ImGui::SliderFloat("Strength", &strength, kMinStrength, kMaxStrength, "%.2f"))
    {
        cfg.contactShadowsStrength = ClampStrength(strength);
        changed = true;
    }
    ext::menu::Tooltip("How much of the sun's light the contact occlusion takes away: 0.5 is half. The engine's mask is 8-bit, so the\n"
                       "term lands on whole levels of 1/255 and a very low strength is easy to miss.");
    int steps = cfg.contactShadowsSteps.value_or_default();
    if (ImGui::SliderInt("Steps", &steps, int(kMinSteps), int(kMaxSteps)))
    {
        cfg.contactShadowsSteps = int32_t(ClampSteps(steps));
        changed = true;
    }
    ext::menu::Tooltip("Steps of the march, each at most one pixel long, so a thin occluder is never stepped over. More steps reach further from\n"
                       "the surface up close and cost more; 32 is the default and the range is 4 to 64.");
    ImGui::EndDisabled();
    if (changed)
        Publish(shadows); // the render thread's copy of the keys and the feature bit, from the frame after this

    const Params current = CurrentParams();
    const Status status = CurrentStatus();
    // The counters are the answer a session reads before its picture: a pass that never ran because the rule recognised a mask of another
    // shape, and a mask whose format or size says why. The last classified light draw's mask is carried with its own verdict. One pass serves
    // one light draw, so the pass count is not printed a second time as a "served" count: the line says what one pass is.
    char counters[320];
    if (status.haveInspection)
        std::snprintf(counters, sizeof counters,
                      "%u pass(es), one per light draw served, %u left alone (%u of them of another shape); the last classified light draw's mask "
                      "is %ux%u in format %u at slot %u (%s)",
                      status.runs, status.declined, status.shapes, status.maskWidth, status.maskHeight, status.maskFormat,
                      status.maskSlot, DeclineName(status.lastDecline));
    else if (status.indexed > 0)
        std::snprintf(counters, sizeof counters,
                      "%u pass(es), one per light draw served, %u left alone, %u shader(s) indexed, no light draw has been classified yet",
                      status.runs, status.declined, status.indexed);
    else
        std::snprintf(counters, sizeof counters,
                      "%u pass(es), one per light draw served, %u left alone, no shader of this build's light pass was recognised",
                      status.runs, status.declined);
    char state[512];
    if (!on)
        std::snprintf(state, sizeof state, "off");
    else if (!status.armed)
        std::snprintf(state, sizeof state, "off for this session: the last light draws were left alone (see the log); %s", counters);
    else if (status.runs > 0)
        std::snprintf(state, sizeof state, "on: up to %.2f m, strength %.2f, %u steps; %s", double(current.length), double(current.strength),
                      current.steps, counters);
    else
        std::snprintf(state, sizeof state, "on: up to %.2f m, strength %.2f, %u steps; waiting for the frame's sun light draw; %s",
                      double(current.length), double(current.strength), current.steps, counters);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", state);
    ImGui::PopStyleColor();
}

const ext::menu::AutoRegister registration { { "ffxiv_contact_shadows", "FFXIV", "Contact shadows (FFXIV)", Draw, "output_scaling",
                                               Visible } };
} // namespace
} // namespace ext::ffxiv::relight::menu
