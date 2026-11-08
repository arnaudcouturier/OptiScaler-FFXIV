#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/engine/Gi.h"
#include "ext/game/Game.h"

#include "ext/ffxiv/meter/MeterCard.h"
#include <imgui/imgui.h>

#include <string>

// "Light bounce (FFXIV)" card (tab FFXIV, the engine-pass group; position: ext/core/MenuLayout.h). Every control publishes to the render
// thread (Publish), so a switch turned on in a running session arms the stage and a slider reaches the next tone map: nothing here needs a
// restart, and the composite stamps are written whatever the key says.
namespace ext::ffxiv::engine::gi::menu
{
namespace
{
// Same condition as the engine pass overrides card: the FFXIV build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Light bounce (FFXIV)");

    auto& cfg = ext::Cfg().ffxiv;
    const bool on = cfg.gi.value_or_default();
    bool enabled = on;
    bool changed = false;
    if (ImGui::Checkbox("Light bounce (experimental)", &enabled))
    {
        cfg.gi = enabled;
        changed = true;
    }
    ext::menu::Tooltip("Experimental: the visible effect is not verified in game yet.\n\n"
                       "The tone-map chain's own light bounce after the volumetric god rays: at a half of the render size, K\n"
                       "cosine-distributed rays leave every pixel's G-buffer normal, each ray is marched against the game's own view\n"
                       "positions up to a range key, and a hit sends back its own linear scene value (the scene's stored texel squared,\n"
                       "the surface's own colour left in, so a warm floor sends warm light). Rays that find no hit add zero. The estimate\n"
                       "is smoothed once (a dense 5x5 binomial stencil that averages every pixel the pass traced, zeros included) and added\n"
                       "to a copy of the lit scene that the tone map reads for that one draw, bounded by the frame's own white like the\n"
                       "other added-light stages.\n"
                       "The game's own scene is only read, never written, and every frame this cannot measure is left exactly as the chain\n"
                       "left it, with one log line for the reason. It stands down for the session after sixty-four of its own declines in a\n"
                       "row are left alone. It is inactive while the engine's scene HDR is on, because that path replaces the tone map this\n"
                       "stage draws before.\n\n"
                       "Judge it where geometry meets under bright sun: the bounce reads as colour carried onto a wall that no direct light\n"
                       "reaches.");
    ImGui::BeginDisabled(!on);
    int rays = cfg.giRays.value_or_default();
    if (ImGui::SliderInt("Rays", &rays, kMinRays, kMaxRays))
    {
        cfg.giRays = int32_t(ClampRays(rays));
        changed = true;
    }
    ext::menu::Tooltip("The rays one pixel casts, 1 to 8. More rays fill the hemisphere the corner sees; the march runs at a half of the\n"
                       "render size by default.");
    int steps = cfg.giSteps.value_or_default();
    if (ImGui::SliderInt("Steps", &steps, kMinSteps, kMaxSteps))
    {
        cfg.giSteps = int32_t(ClampSteps(steps));
        changed = true;
    }
    ext::menu::Tooltip("The march's step count, 8 to 32. More steps confirm a nearer crossing; the cost is one read per step and per ray.");
    float range = cfg.giRange.value_or_default();
    if (ImGui::SliderFloat("Range (metres)", &range, kMinRange, kMaxRange, "%.1f"))
    {
        cfg.giRange = ClampRange(range);
        changed = true;
    }
    ext::menu::Tooltip("How far from the pixel a ray reaches, in metres of world. Rays past the range carry nothing; small radii keep the\n"
                       "bounce local (tight corners, no ghosting across the scene).");
    float strength = cfg.giStrength.value_or_default();
    if (ImGui::SliderFloat("Strength", &strength, kMinStrength, kMaxStrength, "%.2f"))
    {
        cfg.giStrength = ClampStrength(strength);
        changed = true;
    }
    ext::menu::Tooltip("How much of the bounce the pass adds, 0 to 1. 0.00 turns the stage off (the frame costs one test per draw, nothing\n"
                       "else); 1.00 is the full diffuse bounce (a surface under a uniform environment reflects it whole), and\n"
                       "the default 0.55 is the calibrated amount: a lamp-lit wall bleeds visibly onto its neighbours while a flat open area\n"
                       "stays under 1 %. The addition is bounded by each pixel's room to the white the pass reads, so no key pushes a channel\n"
                       "past that white.");
    int resolution = cfg.giResolution.value_or_default() == "quarter" ? 1 : 0;
    const char* resolutions[] { "Half of the render size", "A quarter of the render size" };
    if (ImGui::Combo("Resolution", &resolution, resolutions, 2))
    {
        cfg.giResolution = std::string(resolution == 1 ? "quarter" : "half");
        changed = true;
    }
    ext::menu::Tooltip("The pass's own resolution. Half is the default; a quarter halves the march's cost and gives a coarser estimate\n"
                       "(the same 5x5 stencil smooths it).");
    ImGui::EndDisabled();
    if (changed)
        Publish(); // the render thread's copy of the keys and the feature bit, from the next frame

    const Status status = CurrentStatus();
    std::string state;
    if (!on)
        state = "off";
    else if (status.hdrInactive)
        state = "inactive while scene HDR is on: that path replaces the tone map this stage draws before";
    else if (status.strength <= kMinStrength)
        state = "off at strength 0.00: the master switch is on and the stage adds nothing";
    else if (!status.armed)
        state = "off for this session: the stage was left alone (see the log)";
    else
    {
        CardNumbers numbers;
        numbers.strength = status.strength;
        numbers.frames = status.frames;
        numbers.runs = status.runs;
        numbers.held = status.held;
        numbers.declined = status.declined;
        numbers.skipped = status.skipped;
        numbers.noGBuffer = status.noGBuffer;
        numbers.noDepth = status.noDepth;
        numbers.noCamera = status.noCamera;
        numbers.refused = status.refused;
        numbers.windowWidth = status.windowWidth;
        numbers.windowHeight = status.windowHeight;
        numbers.giWidth = status.giWidth;
        numbers.giHeight = status.giHeight;
        state = FormatCard(numbers);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", state.c_str());
    ImGui::PopStyleColor();
    ext::ffxiv::meter::DrawCardLine(ext::ffxiv::meter::StageId::Gi); // the effect meter's "ran X %, visible Y %" (ext/ffxiv/meter)
}

const ext::menu::AutoRegister registration { { "ffxiv_gi", "FFXIV", "Light bounce (FFXIV)", Draw, "ffxiv_engine_passes", Visible } };
} // namespace
} // namespace ext::ffxiv::engine::gi::menu
