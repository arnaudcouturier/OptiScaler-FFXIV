#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/engine/AoApply.h"
#include "ext/game/Game.h"

#include "ext/ffxiv/meter/MeterCard.h"
#include <imgui/imgui.h>

#include <cstdio>
#include <string>

// "Ambient occlusion (FFXIV)" card (tab FFXIV, after the engine pass overrides card; position: ext/core/MenuLayout.h). Every control publishes
// its value to the render thread (Publish), so a switch turned on in a running session arms the draw path and a slider reaches the next frame:
// the shader index is filled whatever the key says, because the game creates its shaders while it loads. Nothing here needs a restart.
namespace ext::ffxiv::engine::ao::menu
{
namespace
{
// Same condition as the fork's other FFXIV cards: the build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Ambient occlusion (FFXIV)");

    auto& cfg = ext::Cfg().ffxiv;
    bool on = cfg.ambientOcclusion.value_or_default();
    bool changed = false;
    if (ImGui::Checkbox("Ambient occlusion (experimental)", &on))
    {
        cfg.ambientOcclusion = on;
        changed = true;
    }
    ext::menu::Tooltip("Experimental: tried in game in one session, and the user confirmed the pass works; the fixed picture and the\n"
                       "pause have not been judged again.\n\n"
                       "This fork computes its own ambient occlusion from the game's depth and its G-buffer normal, once a frame, from the\n"
                       "reader that completes the pass's three inputs onward, into a texture of the engine's own size, and every later draw of\n"
                       "that frame that would sample the engine's screen-space occlusion samples this one for that single call instead. Nothing\n"
                       "of the game is written and no shader of the game is replaced: the game's own texture is bound again after the draw.\n\n"
                       "The darkening sits in the corners a screen-space search can see: where a wall meets a floor, under a table, behind a\n"
                       "pillar. It is not a shadow: the engine's own shadows and light are untouched. A screen this module cannot measure\n"
                       "keeps the engine's texture (one log line says why, with what that frame collected), it counts the reader draws it\n"
                       "served in the last frame, and after sixty-four frames in a row left alone (one count a frame, at its first wait or its\n"
                       "refusal) it pauses: it then inspects every reader of one frame about 2.7 seconds later (120 frames at 45 frames a second)\n"
                       "and re-arms at the first frame it can serve, so a menu or a loading screen cannot keep it off. Strength or blend at 0 runs\n"
                       "nothing at all.");
    float radius = cfg.ambientOcclusionRadius.value_or_default();
    float strength = cfg.ambientOcclusionStrength.value_or_default();
    float blend = cfg.ambientOcclusionBlend.value_or_default();
    int quality = int(ParseQuality(cfg.ambientOcclusionQuality.value_or_default()));
    ImGui::BeginDisabled(!on);
    if (ImGui::SliderFloat("Radius (m)", &radius, kMinRadius, kMaxRadius, "%.2f"))
    {
        cfg.ambientOcclusionRadius = ClampRadius(radius);
        changed = true;
    }
    ext::menu::Tooltip("How far around a pixel the search looks, in metres of world. A larger radius darkens more of a room (a whole corner,\n"
                       "not just its edge) and costs nothing extra on the GPU: only the picture changes. 1.00 m is a doorway's worth.");
    if (ImGui::SliderFloat("Strength", &strength, kMinStrength, kMaxStrength, "%.2f"))
    {
        cfg.ambientOcclusionStrength = ClampStrength(strength);
        changed = true;
    }
    ext::menu::Tooltip("How much of the computed occlusion is applied. 1.00 is the algorithm's own answer, above 1 exaggerates it, and 0.00 runs\n"
                       "nothing at all: the engine's own texture stays bound.");
    if (ImGui::SliderFloat("Blend with the engine", &blend, kMinBlend, kMaxBlend, "%.2f"))
    {
        cfg.ambientOcclusionBlend = ClampBlend(blend);
        changed = true;
    }
    ext::menu::Tooltip("How much of this fork's occlusion is applied on top of the engine's own (the two are multiplied). 1.00 applies it\n"
                       "in full, 0.50 applies half of its darkening, and 0.00 runs nothing: the engine's picture stays as it is. Lower it\n"
                       "first if the picture looks too dark overall.");
    const char* qualities[] { "low", "medium", "high" };
    if (ImGui::Combo("Quality", &quality, qualities, IM_ARRAYSIZE(qualities)))
    {
        cfg.ambientOcclusionQuality = std::string(QualityName(quality == 0   ? Quality::Low
                                                             : quality == 2 ? Quality::High
                                                                            : Quality::Medium));
        changed = true;
    }
    ext::menu::Tooltip("Depth taps a direction: 4, 8 or 16. This is the one setting that changes what the pass costs on the GPU, roughly in\n"
                       "proportion. medium is the default.");
    ImGui::EndDisabled();
    if (changed)
        Publish(on); // the render thread's copy of the keys and the feature bit, from the frame after this

    const Status status = CurrentStatus();
    char state[400];
    if (!status.key)
        std::snprintf(state, sizeof state, "off");
    else if (status.identity)
        std::snprintf(state, sizeof state, "on, but the strength or the blend is at 0, so the engine's own occlusion is bound unchanged");
    else if (status.armed)
        std::snprintf(state, sizeof state,
                      "on: %u passes, %u draws served with this fork's occlusion (%u in the last frame), %u readers left alone so far",
                      status.runs, status.readers, status.servedLastFrame, status.declined);
    else
        std::snprintf(state, sizeof state,
                      "paused after sixty-four frames in a row left alone (one count a frame, at its first wait or its "
                      "refusal); it inspects every reader of one frame again about 2.7 seconds later (%u passes, %u draws served so far) - "
                      "see the log for the reason those frames were left alone",
                      status.runs, status.readers);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", state);
    ImGui::PopStyleColor();
    ext::ffxiv::meter::DrawCardLine(ext::ffxiv::meter::StageId::Ao); // the effect meter's "ran X %, visible Y %" (ext/ffxiv/meter)
}

const ext::menu::AutoRegister registration { { "ffxiv_ambient_occlusion", "FFXIV", "Ambient occlusion (FFXIV)", Draw, "output_scaling",
                                               Visible } };
} // namespace
} // namespace ext::ffxiv::engine::ao::menu
