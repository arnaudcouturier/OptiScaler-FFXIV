#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/engine/GlossPuddlesApply.h"
#include "ext/ffxiv/engine/PuddleTone.h"
#include "ext/game/Game.h"

#include <imgui/imgui.h>

#include <cstdio>
#include <string>

// "Puddles (FFXIV)" card (tab FFXIV, after the ambient occlusion card; position: ext/core/MenuLayout.h). Every control publishes its value to the
// render thread (Publish), so a switch turned on in a running session arms the draw path and a slider reaches the next frame: the shader stamps are
// written whatever the key says, because the game creates its shaders while it loads. Nothing here needs a restart.
namespace ext::ffxiv::engine::gloss::menu
{
namespace
{
// Same condition as the fork's other FFXIV cards: the build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Puddles (FFXIV)");

    auto& cfg = ext::Cfg().ffxiv;
    bool on = cfg.puddles.value_or_default();
    bool changed = false;
    if (ImGui::Checkbox("Puddles (experimental)", &on))
    {
        cfg.puddles = on;
        changed = true;
    }
    ext::menu::Tooltip("Experimental: the visible effect is not verified in game yet.\n\n"
                       "Rain makes the ground glossy; this fork adds the standing water, in two stages that read one noise field anchored to the\n"
                       "world. Once a frame, before the light passes, it copies one of the engine's own G-buffer targets and replaces the gloss\n"
                       "channel of the ground where a puddle is. At the tone-map draw it then darkens the pool's lit colour and adds a marched\n"
                       "reflection of the scene to it, with water's Fresnel: the mirror is the picture the frame has already drawn, not the\n"
                       "engine's 256-square environment probe. The engine's own wetness amount ramps both stages up with the rain and down as it\n"
                       "dries, so there is no timer here and nothing is remembered between frames.\n\n"
                       "Which channel of which target carries the gloss is measured now: the material pack (target 1) carries it in its alpha, the only\n"
                       "continuous gloss weight the engine's composite reads. Its red is a selector the composite and the wetness pass compare against\n"
                       "1.0, so that one pair is refused and nothing runs. Nothing of the game is written and no shader of the game is replaced. A frame\n"
                       "this pass cannot serve keeps the engine's target and is skipped (one log line says why, with what that draw bound); only a\n"
                       "frame it cannot do the work for at all counts, and sixty-four of those in a row switch it off for the session.");

    float coverage = cfg.puddlesCoverage.value_or_default();
    float scale = cfg.puddlesScale.value_or_default();
    float softness = cfg.puddlesSoftness.value_or_default();
    float wet = cfg.puddlesGlossWet.value_or_default();
    int target = int(cfg.puddlesGlossTarget.value_or_default()) - 1;
    const Channel channel = ParseChannel(cfg.puddlesGlossChannel.value_or_default());
    int channelIndex = int(channel);
    ImGui::BeginDisabled(!on);
    if (ImGui::SliderFloat("Coverage", &coverage, kMinCoverage, kMaxCoverage, "%.2f"))
    {
        cfg.puddlesCoverage = ClampKey(coverage, kMinCoverage, kMaxCoverage, kCoverageDefault);
        changed = true;
    }
    ext::menu::Tooltip("How much of the ground that faces up becomes puddle at full rain. 0.42 puts about a third of it in pools (measured on the\n"
                       "shader's own field over one full 64-cell period): the rest stays damp with the engine's own sheen, which is what makes the pools\n"
                       "read as pools. 0.00 runs nothing at all.");
    if (ImGui::SliderFloat("Scale (m)", &scale, kMinScale, kMaxScale, "%.2f"))
    {
        cfg.puddlesScale = ClampKey(scale, kMinScale, kMaxScale, kScaleDefault);
        changed = true;
    }
    ext::menu::Tooltip("The size of one patch of the noise field, in metres: a pool is a few cells across, so 6.00 puts the median pool at about\n"
                       "four metres across and the largest tenth near twelve (measured on a 60 m patch). Larger values make broad sheets,\n"
                       "smaller ones a wet grain. The field is anchored to the world, so it stays where it is while the camera moves.");
    if (ImGui::SliderFloat("Edge softness", &softness, kMinSoftness, kMaxSoftness, "%.2f"))
    {
        cfg.puddlesSoftness = ClampKey(softness, kMinSoftness, kMaxSoftness, kSoftnessDefault);
        changed = true;
    }
    ext::menu::Tooltip("The half-width of a puddle's edge, in noise units. 0.03 gives the sub-metre edge the reference's stone has (0.8 m measured\n"
                       "at the default scale); near 0.00 the puddles get a hard outline, which also shows the 8 bits the engine's target is stored in.");
    if (ImGui::SliderFloat("Wet value", &wet, kMinWet, kMaxWet, "%.2f"))
    {
        cfg.puddlesGlossWet = ClampKey(wet, kMinWet, kMaxWet, kWetDefault);
        changed = true;
    }
    ext::menu::Tooltip("The channel value of a mirror-wet surface: 1.00 if the engine's gloss channel is high on a wet surface, 0.00 if it is\n"
                       "the roughness that is high. Whichever way round it is, the puddles take this value and the dry ground keeps the engine's.\n"
                       "Inside the pool it is also what a mirrored ray that found nothing falls back to: the engine's own probe reflection, sharpened.");
    const char* targets[] { "1: material pack", "2" };
    if (ImGui::Combo("G-buffer target", &target, targets, IM_ARRAYSIZE(targets)))
    {
        cfg.puddlesGlossTarget = int32_t(ClampTarget(target + 1));
        changed = true;
    }
    ext::menu::Tooltip("Which of the engine's G-buffer targets carries the channel to shape. 1 is the material pack, whose alpha is the gloss the\n"
                       "engine's composite reads. Only the engine's 8-bit targets can be shaped: 3 is a 16-bit target and 4 is the world-space normal\n"
                       "this pass reads to decide what faces up, so neither is offered (an ini value of either binds the engine's own target and says\n"
                       "so in the log).\n"
                       "The pools are kept off ground the engine renders dry, and that is read from the engine's gloss in G-buffer 1: with target 2 there\n"
                       "is no engine gloss to read, so both stages draw no pool (one log line says so).");
    const char* channels[] { "r", "g", "b", "a" };
    if (ImGui::Combo("Channel", &channelIndex, channels, IM_ARRAYSIZE(channels)))
    {
        cfg.puddlesGlossChannel = std::string(ChannelName(Channel(channelIndex)));
        changed = true;
    }
    ext::menu::Tooltip("Which channel of that target is replaced. `a` is the material pack's gloss. The other three are the engine's own bits,\n"
                       "untouched. `r` on target 1 is the one pair this pass refuses: the engine reads that byte as a selector, so writing it tints\n"
                       "the ground and nothing runs.");

    ImGui::SeparatorText("At the tone map");
    bool reflection = cfg.puddlesReflection.value_or_default();
    float strength = cfg.puddlesReflectionStrength.value_or_default();
    float darkening = cfg.puddlesDarkening.value_or_default();
    float ripple = cfg.puddlesRipple.value_or_default();
    int steps = int(cfg.puddlesReflectionSteps.value_or_default());
    if (ImGui::Checkbox("Reflection", &reflection))
    {
        cfg.puddlesReflection = reflection;
        changed = true;
    }
    ext::menu::Tooltip("The mirrored reflection of the scene at the tone-map draw: each pool texel marches the frame's own view positions and adds\n"
                       "what it hits with water's Fresnel. Judge it from the reference's own pose - level, at eye height, the fountain six to ten\n"
                       "metres ahead: below about three metres the Fresnel makes the mirror invisible and the darkening carries the pool. Off is the\n"
                       "darkening alone, which is the A/B to judge the pool's shape before the mirror. The pass remembers nothing between frames, so it\n"
                       "cannot ghost on its own; it is one sample of the scene per texel, so an upscaler or frame generation may shimmer or smear the\n"
                       "hit's edge.");
    if (ImGui::SliderFloat("Reflection strength", &strength, kMinReflectionStrength, kMaxReflectionStrength, "%.2f"))
    {
        cfg.puddlesReflectionStrength = ClampKey(strength, kMinReflectionStrength, kMaxReflectionStrength, kReflectionStrengthDefault);
        changed = true;
    }
    ext::menu::Tooltip("How much of the Fresnel reflection the marched colour is added with. 0.00 is the darkening alone; 1.00 is the scene's own\n"
                       "colour at full Fresnel, a perfect mirror where the angle is right.");
    if (ImGui::SliderFloat("Darkening", &darkening, kMinDarkening, kMaxDarkening, "%.2f"))
    {
        cfg.puddlesDarkening = ClampKey(darkening, kMinDarkening, kMaxDarkening, kDarkeningDefault);
        changed = true;
    }
    ext::menu::Tooltip("How much of the lit colour a full puddle takes away. This is what makes the ground read as water instead of varnish, and the\n"
                       "part that has to work before the mirror does: 0.45 keeps a little over half of the colour inside a pool. It is also the risky\n"
                       "half of the look at night: a night stone sits near 0.10, so a full pool at 0.45 lands near 0.05 and reads as a hole where the\n"
                       "mirror misses.");
    if (ImGui::SliderFloat("Ripple", &ripple, kMinRipple, kMaxRipple, "%.2f"))
    {
        cfg.puddlesRipple = ClampKey(ripple, kMinRipple, kMaxRipple, kRippleDefault);
        changed = true;
    }
    ext::menu::Tooltip("How much a fixed ripple field tilts the mirror's normal. The slope comes from the same world-anchored noise as the pool, so it\n"
                       "is identical in every frame and the pool's shape cannot crawl; 0.00 is a flat mirror of the stone joints.");
    if (ImGui::SliderInt("March steps", &steps, int(kMinSteps), int(kMaxSteps)))
    {
        cfg.puddlesReflectionSteps = int32_t(ClampSteps(steps));
        changed = true;
    }
    ext::menu::Tooltip("How many taps the mirrored reflection's march takes: the one setting that changes what the tone-map stage costs on the GPU,\n"
                       "roughly in proportion. A pool's mirror is wide and smooth, so the default is half of the water reflection's.");
    float reach = cfg.puddlesReach.value_or_default();
    if (ImGui::SliderFloat("Reach (m)", &reach, kMinReach, kMaxReach, "%.1f"))
    {
        cfg.puddlesReach = ClampReach(reach);
        changed = true;
    }
    ext::menu::Tooltip("How far from the camera a pool may sit. The G-buffer beneath a forward draw - the ocean, the lakes, the rivers - is the\n"
                       "up-facing ground under the water, so past the reach the stage keeps the engine's own texel instead of painting pools onto the\n"
                       "water. The measured water blotches sat between 25 and 40 m; the beach pools a camera reads sit inside 24 m. 0 disables the gate.");
    ImGui::EndDisabled();
    if (changed)
        Publish(on); // the render thread's copy of the keys and the feature bit, from the frame after this

    const Status status = CurrentStatus();
    const ToneStatus tone = CurrentToneStatus();
    const Wetness wetness = CurrentWetness();
    const float rainAmount = LastRainAmount();
    const float activeKnob = WetnessKnobFor(cfg.enginePassOverrides.value_or_default(), cfg.wetnessOverride.value_or_default());
    char state[400];
    if (!status.key)
        std::snprintf(state, sizeof state, "off");
    else if (status.identity)
        std::snprintf(state, sizeof state, "on, but %s, so the engine's own target is bound unchanged",
                      !TargetServable(CurrentParams().target)
                          ? "this pass cannot shape the key's G-buffer target"
                          : (!ChannelServable(CurrentParams().target, CurrentParams().channel)
                                 ? "the key's channel of this target is one the pass must not write"
                                 : "the coverage is at 0"));
    else if (status.armed && !GateReadable(CurrentParams().target))
        std::snprintf(state, sizeof state, "on, but G-buffer target %u carries no engine gloss, so both stages draw no pool (target 1 does)",
                      CurrentParams().target);
    else if (status.armed)
    {
        if (!wetness.known)
            std::snprintf(state, sizeof state, "on: waiting for the frame's weather reading");
        else if (!WetEnough(wetness.known, wetness.amount))
            std::snprintf(state, sizeof state, "on (dry weather, waiting for rain): %u gloss passes, %u tone-map passes this session",
                          status.runs, tone.runs);
        else
        {
            const char* weatherState = rainAmount > 0.0f ? "raining, splash rings active"
                                                         : (activeKnob >= 0.0f ? "wetness override active, still pools" : "drying after rain, still pools");
            std::snprintf(state, sizeof state,
                          "on (%s, wetness %.2f): %u gloss passes, %u draws served (%u in the last frame), %u tone-map passes, %u readers left "
                          "alone so far%s",
                          weatherState, double(wetness.amount), status.runs, status.readers, status.servedLastFrame, tone.runs, status.declined,
                          ToneIdentity(CurrentParams()) ? ", the tone stage is at its identity" : "");
        }
    }
    else
        std::snprintf(state, sizeof state, "off for this session: the readers were left alone (see the log)");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", state);
    ImGui::PopStyleColor();
}

const ext::menu::AutoRegister registration { { "ffxiv_puddles", "FFXIV", "Puddles (FFXIV)", Draw, "output_scaling", Visible } };
} // namespace
} // namespace ext::ffxiv::engine::gloss::menu
