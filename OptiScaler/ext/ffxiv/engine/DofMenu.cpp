#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/engine/Dof.h"
#include "ext/game/Game.h"

#include <imgui/imgui.h>

#include <string>

// "Depth of field (FFXIV)" card (tab FFXIV, the engine-pass group; position: ext/core/MenuLayout.h). Every control
// publishes to the render thread (Publish), so a switch turned on in a running session arms the stage and a slider
// reaches the next tone map: nothing here needs a restart.
namespace ext::ffxiv::dof::menu
{
namespace
{
// Same condition as the engine pass overrides card: the FFXIV build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Depth of field (FFXIV)");

    auto& cfg = ext::Cfg().ffxiv;
    const bool on = cfg.depthOfField.value_or_default();
    bool enabled = on;
    bool changed = false;
    if (ImGui::Checkbox("Depth of field (experimental)", &enabled))
    {
        cfg.depthOfField = enabled;
        changed = true;
    }
    ext::menu::Tooltip("Experimental: the visible effect is not verified in game yet.\n\n"
                       "This fork's own depth of field, at the tone-map chain's last stage: the scene is packed with an\n"
                       "analytic circle of confusion (from the camera's own depth and the focus distance, no lens\n"
                       "disassembly needed), blurred on a disc at a quarter of the render window split near and far,\n"
                       "and merged back before the game's own tone map reads the picture. The game's own scene is only\n"
                       "read, never written, and every frame this cannot measure is left exactly as the chain left it, with\n"
                       "one log line for the reason. It stands down for the session after sixty-four of its own draws in a\n"
                       "row are left alone. It is inactive while the engine's scene HDR is on, because that path replaces\n"
                       "the tone map this stage draws before. In a frame where the game's own depth of field draws\n"
                       "(cutscenes, some camera modes, the graphics option) this stage leaves the frame to it, in both looks.\n\n"
                       "The Look decides what it does; see its tooltip.");
    ImGui::BeginDisabled(!on);
    const Mode look = ParseMode(cfg.dofMode.value_or_default());
    int lookIndex = look == Mode::Cinematic ? 1 : 0;
    const char* looks[] { "Distance (gameplay: the far background only)", "Cinematic (a focus plane, near and far)" };
    if (ImGui::Combo("Look", &lookIndex, looks, 2))
    {
        cfg.dofMode = std::string(lookIndex == 1 ? "cinematic" : "distance");
        changed = true;
    }
    ext::menu::Tooltip("Distance (the default) is the gameplay look. Nothing nearer than the start distance is touched, so your\n"
                       "character and everything around it stay sharp: the start is the larger of twice the focus distance\n"
                       "and 20 metres (40 m for a subject at 20 m). Beyond it the background softens along the thin-lens\n"
                       "curve, reaching its full softness at eight times the start (160 m for a character at 8 m), with a\n"
                       "blur radius capped at 0.75 percent of the window height and at most 25 percent of the blurred\n"
                       "picture mixed in, so the castle and the sea are visibly softer, not destroyed. The near side never\n"
                       "blurs. The auto focus follows the centre of the screen smoothly (the sky and a glance past the\n"
                       "character are ignored); Strength scales the mix, Blur reach does not apply.\n\n"
                       "Cinematic is the first release's look, kept as it was: a sharp band around the focus plane (from\n"
                       "about 0.79 to 1.27 times the focus distance), the near side capped at a quarter of the full defocus\n"
                       "and everything behind the band blurred more and more up to the full reach at every angle. It is a\n"
                       "screenshot look: with a focus on a near subject the whole background is blurred. Strength, Blur\n"
                       "reach and Taps all apply to it.");
    float strength = cfg.dofStrength.value_or_default();
    if (ImGui::SliderFloat("Strength", &strength, kMinStrength, kMaxStrength, "%.2f"))
    {
        cfg.dofStrength = ClampStrength(strength);
        changed = true;
    }
    ext::menu::Tooltip("How much of the blur the merge mixes in, 0 to 1. 0.00 is off: the frame costs one test per\n"
                       "draw, nothing else. The default 0.80 is a visible softness without hazing the whole frame. In the\n"
                       "Distance look the mix is this value times 0.25 (1.00 mixes 25 percent of the blurred picture at the far limit).");
    float aperture = cfg.dofAperture.value_or_default();
    ImGui::BeginDisabled(look == Mode::Distance);
    if (ImGui::SliderFloat("Blur reach", &aperture, kMinAperture, kMaxAperture, "%.3f", ImGuiSliderFlags_Logarithmic))
    {
        cfg.dofAperture = ClampAperture(aperture);
        changed = true;
    }
    ImGui::EndDisabled();
    ext::menu::Tooltip("Cinematic look only (the Distance look has its own capped radius). How far the blur reaches, as a fraction\n"
                       "of the window's height: the same number reads as the same circle at 1440p and at 1080p. 0.050 spans 72\n"
                       "pixels of a 1440p render at the deepest defocus; the key's own floor 0.005 is sub-pixel and turns the\n"
                       "cinematic look off.");
    int steps = cfg.dofSteps.value_or_default();
    if (ImGui::SliderInt("Taps", &steps, kMinSteps, kMaxSteps))
    {
        cfg.dofSteps = int32_t(ClampSteps(steps));
        changed = true;
    }
    ext::menu::Tooltip("The disc's taps, 16 to 48. More taps are a smoother disc's edge and cost more on the GPU at a\n"
                       "quarter of the render size; 24 is the default disc.");
    int focusMode = cfg.dofFocusMode.value_or_default() == "manual" ? 1 : 0;
    const char* modes[] { "Auto (the view distance under the screen centre)", "Manual distance" };
    if (ImGui::Combo("Focus", &focusMode, modes, 2))
    {
        cfg.dofFocusMode = std::string(focusMode == 1 ? "manual" : "auto");
        changed = true;
    }
    ext::menu::Tooltip("What the focus plane is. Auto follows the depth the camera looks at (its own centre pixel,\n"
                       "reconstructed through the game's own camera), which is the classic follow-focus behaviour; the\n"
                       "Distance look smooths it over time and ignores the sky. Manual pins the plane to a distance in\n"
                       "metres, for when the centre of the frame is not what should be sharp.");
    if (focusMode == 1)
    {
        float distance = cfg.dofFocusDistance.value_or_default();
        if (ImGui::SliderFloat("Focus distance", &distance, kMinFocusDistance, kMaxFocusDistance, "%.1f m",
                               ImGuiSliderFlags_Logarithmic))
        {
            cfg.dofFocusDistance = ClampFocusDistance(distance);
            changed = true;
        }
        ext::menu::Tooltip("The manual focus plane's distance from the camera, in metres - the same metres the game's\n"
                           "own fog profile reads.");
    }
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
        state = "off for this session: the stage was left alone (see the log)";
    else
    {
        CardCounts numbers;
        numbers.frames = status.frames;
        numbers.runs = status.runs;
        numbers.declined = status.declined;
        numbers.skipped = status.skipped;
        numbers.noDepth = status.noDepth;
        numbers.noCamera = status.noCamera;
        numbers.refusals = status.refusals;
        numbers.armed = status.armed;
        numbers.strength = status.params.strength;
        numbers.aperture = status.params.aperture;
        numbers.steps = status.params.steps;
        numbers.focusMode = status.params.focusMode;
        numbers.mode = status.params.mode;
        numbers.windowWidth = status.windowWidth;
        numbers.windowHeight = status.windowHeight;
        numbers.quarterWidth = status.quarterWidth;
        numbers.quarterHeight = status.quarterHeight;
        state = FormatCard(numbers);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", state.c_str());
    ImGui::PopStyleColor();
}

const ext::menu::AutoRegister registration { { "ffxiv_dof", "FFXIV", "Depth of field (FFXIV)", Draw, "ffxiv_engine_passes", Visible } };
} // namespace
} // namespace ext::ffxiv::dof::menu
