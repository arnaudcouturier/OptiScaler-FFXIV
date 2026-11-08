#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/engine/SsrApply.h"
#include "ext/game/Game.h"

#include <imgui/imgui.h>

#include <cstdio>

// "Screen-space reflections (FFXIV)" card (tab FFXIV, after the ambient occlusion card; position: ext/core/MenuLayout.h). Every control
// publishes its value to the render thread (Publish), so a switch turned on in a running session arms the draw path and a slider reaches the next
// frame: the shader stamp is written whatever the key says, because the game creates its water shaders while it loads. Nothing here needs a
// restart.
namespace ext::ffxiv::engine::ssr::menu
{
namespace
{
// Same condition as the fork's other FFXIV cards: the build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Screen-space reflections (FFXIV)");

    auto& cfg = ext::Cfg().ffxiv;
    bool on = cfg.screenSpaceReflections.value_or_default();
    bool changed = false;
    if (ImGui::Checkbox("Screen-space reflections (experimental)", &on))
    {
        cfg.screenSpaceReflections = on;
        changed = true;
    }
    ext::menu::Tooltip("Experimental: the visible effect is not verified in game yet.\n\n"
                       "This fork computes its own reflection of the scene for the water surfaces: once a frame, before the first water draw,\n"
                       "it marches the game's own view-position buffer against the scene the frame has already rendered, and every water draw of\n"
                       "that frame that would sample the engine's planar reflection map samples this one for that single call instead. Nothing of\n"
                       "the game is written and no shader of the game is replaced: the game's own texture is bound again after the draw.\n\n"
                       "Where the march finds nothing, and everywhere the engine's own half-resolution map has no answer, the engine's texel is\n"
                       "kept untouched, so a scene this module cannot measure keeps the picture it had (one log line says why, with what that draw\n"
                       "bound). It switches itself off for the session after sixty-four refused water draws in a row, and a blend of 0 runs nothing\n"
                       "at all.");
    int steps = cfg.screenSpaceReflectionsSteps.value_or_default();
    float thickness = cfg.screenSpaceReflectionsThickness.value_or_default();
    float blend = cfg.screenSpaceReflectionsBlend.value_or_default();
    float fade = cfg.screenSpaceReflectionsFade.value_or_default();
    ImGui::BeginDisabled(!on);
    if (ImGui::SliderInt("Steps", &steps, int(kMinSteps), int(kMaxSteps)))
    {
        cfg.screenSpaceReflectionsSteps = int32_t(ClampSteps(float(steps)));
        changed = true;
    }
    ext::menu::Tooltip("How many taps the ray march takes. This is the one setting that changes what the pass costs on the GPU, roughly in\n"
                       "proportion; 48 is the default. More steps find thinner and farther reflections, not brighter ones.");
    if (ImGui::SliderFloat("Thickness (m)", &thickness, kMinThickness, kMaxThickness, "%.2f"))
    {
        cfg.screenSpaceReflectionsThickness = ClampThickness(thickness);
        changed = true;
    }
    ext::menu::Tooltip("How deep behind a crossing ray a surface still counts as the one the ray hit, in metres of world. Too small and a thin\n"
                       "reflection (a railing, a mast) is missed; too large and a wall the ray passed a metre ago paints the water.");
    if (ImGui::SliderFloat("Blend with the engine", &blend, kMinBlend, kMaxBlend, "%.2f"))
    {
        cfg.screenSpaceReflectionsBlend = ClampBlend(blend);
        changed = true;
    }
    ext::menu::Tooltip("How much of this fork's reflection replaces the engine's own where the march found something. 1.00 is this one only,\n"
                       "0.50 mixes the two equally, and 0.00 runs nothing at all: the engine's own map stays bound.");
    if (ImGui::SliderFloat("Edge fade", &fade, kMinFade, kMaxFade, "%.2f"))
    {
        cfg.screenSpaceReflectionsFade = ClampFade(fade);
        changed = true;
    }
    ext::menu::Tooltip("The fraction of the picture at its edges over which a reflection is trusted less and less, so a hit that runs out of\n"
                       "screen fades out instead of ending at a hard line. 0.00 trusts a hit right up to the edge.");
    ImGui::EndDisabled();
    if (changed)
        Publish(on); // the render thread's copy of the keys and the feature bit, from the frame after this

    const Status status = CurrentStatus();
    char state[400];
    if (!status.key)
        std::snprintf(state, sizeof state, "off");
    else if (status.identity)
        std::snprintf(state, sizeof state, "on, but the blend is at 0, so the engine's own reflection map is bound unchanged");
    else if (status.armed)
        std::snprintf(state, sizeof state,
                      "on: %u passes, %u water draws served with this fork's reflection (%u in the last frame), %u draws left alone so far%s",
                      status.runs, status.readers, status.servedLastFrame, status.declined,
                      FormatGpuCost(status.haveGpuMilliseconds, status.gpuMilliseconds).c_str());
    else
        std::snprintf(state, sizeof state, "off for this session: the water draws were left alone (see the log)");
    // The reasons the seam passed draws on for, which are not refusals: the line a player needs when the card reads "on" and nothing changes,
    // because it says which gate the water draws stopped at (SsrRules.h's PassedOn).
    const std::string passedOn = FormatPassedOnCounts(status.passedOn);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", state);
    if (status.diagnostic)
    {
        static constexpr const char* modes[] = { "A SSR + swap", "B original SRV + swap", "C private map copy + swap", "D SSR, no native SRV setter", "E D with observers suppressed", "F D without SSR draw", "G F without timing queries", "H F with scene-target unbind omitted", "I bypass SSR pass", "J native-context compatibility pass", "K preparation only", "L pipeline only", "M combined without draw", "N swap only", "O target bindings only", "P other bindings only" };
        ImGui::TextWrapped("SSR live diagnostic: %s; runs %u, bypasses %u, matching engine-map copies %u, fullscreen draws submitted %u, timing begins/ends/GetData %u/%u/%u, OM UAV masks %02X→%02X (lost %02X), predicate %s, suppressed callbacks %llu; foreign-window native draws %llu, pipeline setters %u; F7 cycles A–P; Ctrl+F7 cycles L→N→O→P→I→L (from other modes, starts L); Shift+F7 toggles A/J",
                           modes[status.diagnosticMode], status.runs, status.diagnosticBypasses, status.diagnosticCopies,
                           status.diagnosticDraws, status.diagnosticTimingBegins, status.diagnosticTimingEnds,
                           status.diagnosticTimingGetDataCalls,
                           status.diagnosticUavMaskBeforeCopy, status.diagnosticUavMaskAfterCopy,
                           status.diagnosticUavLostMask, status.diagnosticPredicatePresent ? "present" : "absent",
                           static_cast<unsigned long long>(status.diagnosticSuppressedCallbacks), static_cast<unsigned long long>(status.diagnosticForeignDraws), status.diagnosticPipelineBinds);
        if (status.diagnosticMode == 9)
        {
            const char* route = status.diagnosticJCompatible && !status.diagnosticJPassCompleted
                ? "compatible context, but the full SSR pass did not complete"
                : status.diagnosticJCompatible && status.diagnosticJRouteChanged
                ? "VERIFIED: raw contexts differ and compatibility identities match; full SSR ran"
                : status.diagnosticJCompatible
                    ? "SSR applied; context route unchanged; route comparison NOT EXERCISED"
                    : "NOT EXERCISED: compatibility identity did not match; engine map retained";
            ImGui::TextWrapped("J route comparison: %s", route);
        }
        if (status.diagnosticMode >= 10)
            ImGui::TextWrapped("K-P preserve the engine reflection: SSR appearance/settings are inactive. Last-run copies/clears/maps/state swaps/draws/timing begins/ends/GetData/scene OM sets/pipeline setters: %u/%u/%u/%u/%u/%u/%u/%u/%u/%u; tracked OM before/after %d/%d (unknown=-1), native target same %s",
                               status.diagnosticOperations.copies, status.diagnosticOperations.clears, status.diagnosticOperations.maps,
                               status.diagnosticOperations.stateSwaps, status.diagnosticOperations.fullscreenDraws,
                               status.diagnosticOperations.timingBegins, status.diagnosticOperations.timingEnds,
                               status.diagnosticOperations.timingGetData, status.diagnosticOperations.sceneOmSets,
                               status.diagnosticOperations.pipelineBinds, status.diagnosticOperations.trackedOmBefore, status.diagnosticOperations.trackedOmAfter, status.diagnosticOperations.nativeTargetRestored ? "yes" : "no");
        if (!status.diagnosticCopyAvailable)
            ImGui::TextWrapped("Private engine-map copy unavailable. Mode C uses the original engine map and is not a valid C comparison.");
    }
    if (!passedOn.empty())
        ImGui::TextWrapped("draws passed on, which is not a refusal: %s", passedOn.c_str());
    ImGui::PopStyleColor();
}

const ext::menu::AutoRegister registration { { "ffxiv_screen_space_reflections", "FFXIV", "Screen-space reflections (FFXIV)", Draw,
                                               "ffxiv_ambient_occlusion", Visible } };
} // namespace
} // namespace ext::ffxiv::engine::ssr::menu
