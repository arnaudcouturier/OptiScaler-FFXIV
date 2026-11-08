#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/engine/Flare.h"
#include "ext/game/Game.h"

#include <imgui/imgui.h>

#include <string>

// "Lens flare (FFXIV)" card (tab FFXIV, position set by ext/core/MenuLayout.h). Every control publishes to the render thread (Publish), so
// a switch turned on in a running session arms the card and a slider reaches the next flare draw: nothing here needs a restart, and the
// family's stamps are built while the game creates its shaders whatever the key says.
//
// The card is the family's own look, dials only: the ring's colour, the sun sprite's colour, the ghost chain's step, and the three parts'
// checkboxes (a part unchecked is removed from the picture; the veil's add is then skipped, because its colour rides its own vertex data).
// The switch and every default are today's picture.
namespace ext::ffxiv::flare::menu
{
namespace
{
// Same condition as the engine pass overrides card: the FFXIV build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Lens flare (FFXIV)");

    auto& cfg = ext::Cfg().ffxiv;
    const bool on = cfg.flareCard.value_or_default();
    bool enabled = on;
    bool changed = false;
    if (ImGui::Checkbox("Lens flare", &enabled))
    {
        cfg.flareCard = enabled;
        changed = true;
    }
    ext::menu::Tooltip("The engine's own lens flare, with each part's amount and presence in your hands.\n\n"
                       "The family is the game's own: the ring is one draw of its HDR pass (its colour row is scaled), the sun and the\n"
                       "ghost orbs are its sprite chain (the sprite's colour row, and the step the ghosts march by), and the veil\n"
                       "removes by skipping its add, because its colour and bias ride its own four vertices' data.\n"
                       "Nothing of the game's own buffers is ever written: they are copied, and the copy is what the patched draw reads. A\n"
                       "frame at the engine's own values makes no patch at all, so it is the game's own picture to the bit. Every frame the\n"
                       "card cannot serve is left exactly as it was, with one log line for the reason.\n\n"
                       "Think of the four parts: the ring around the sun, the sun sprite itself, the ghost orbs marching from it, and\n"
                       "the soft screen-centre veil.");
    ImGui::BeginDisabled(!on);
    float ring = cfg.flareRingScale.value_or_default();
    if (ImGui::SliderFloat("Ring", &ring, flare::kMinPartScale, flare::kMaxPartScale, "%.2f"))
    {
        cfg.flareRingScale = flare::ClampScale(ring);
        changed = true;
    }
    ext::menu::Tooltip("The ring's colour, 0 to 2, as a share of what the engine put in its constant buffer for this frame. 1.00 is the\n"
                       "engine's own picture; 0.00 removes the ring and nothing else (a patch, never a skip: the ring's own draw writes\n"
                       "the buffer the HDR scene reads, so a skipped draw would leave the last frame's content in it).");
    float sun = cfg.flareSunScale.value_or_default();
    if (ImGui::SliderFloat("Sun + orbs", &sun, flare::kMinPartScale, flare::kMaxPartScale, "%.2f"))
    {
        cfg.flareSunScale = flare::ClampScale(sun);
        changed = true;
    }
    ext::menu::Tooltip("The sun sprite's colour, 0 to 2. The three radial blurs copy the sprite, so this key also carries the ghost orbs\n"
                       "and the veil empties with them; 0.00 removes the whole of it and leaves the game's own bloom where it was - the\n"
                       "glare chain's weight is the bloom's own, not the flare's.");
    float ghost = cfg.flareGhostScale.value_or_default();
    if (ImGui::SliderFloat("Ghost step", &ghost, flare::kMinPartScale, flare::kMaxPartScale, "%.2f"))
    {
        cfg.flareGhostScale = flare::ClampScale(ghost);
        changed = true;
    }
    ext::menu::Tooltip("The step each radial-blur pass installs on its own 16-byte buffer, which is how far each copy keeps the sun's\n"
                       "pixels marching outward per pass: 0.00 collapses the three copies into one disc - the ghosts merge into the\n"
                       "sprite while the sprite itself is untouched - and 2.00 spreads them twice as far.");
    bool ringPart = (cfg.flareParts.value_or_default() & flare::kPartRing) != 0;
    if (ImGui::Checkbox("Ring part on", &ringPart))
    {
        cfg.flareParts = int32_t((cfg.flareParts.value_or_default() & ~flare::kPartRing) | (ringPart ? flare::kPartRing : 0u));
        changed = true;
    }
    ext::menu::Tooltip("The ring's part, on or off. Off removes the ring only, at scale 0; the sun and the ghost orbs keep the values\n"
                       "they were at.");
    bool sunPart = (cfg.flareParts.value_or_default() & flare::kPartSunOrbs) != 0;
    if (ImGui::Checkbox("Sun + orbs part on", &sunPart))
    {
        cfg.flareParts = int32_t((cfg.flareParts.value_or_default() & ~flare::kPartSunOrbs) | (sunPart ? flare::kPartSunOrbs : 0u));
        changed = true;
    }
    ext::menu::Tooltip("The sun-and-orbs part, on or off. Off asks the sun's own patch at 0 and leaves the ghost step unanswered - the\n"
                       "orbs die with the sprite they are copies of, and the veil empties with its 1x1 input.");
    bool veilPart = (cfg.flareParts.value_or_default() & flare::kPartVeil) != 0;
    if (ImGui::Checkbox("Veil part on", &veilPart))
    {
        cfg.flareParts = int32_t((cfg.flareParts.value_or_default() & ~flare::kPartVeil) | (veilPart ? flare::kPartVeil : 0u));
        changed = true;
    }
    ext::menu::Tooltip("The veil's part, on or off. Off skips the veil's add for its draw and nothing else: the draw runs between the\n"
                       "merge that wrote the glare buffer and the bloom's add that follows, so its absence removes the veil alone. The\n"
                       "veil is never patched, because its own colour and bias live in its four vertices' data.");
    ImGui::EndDisabled();
    if (changed)
        Publish(); // the render thread's copy of the keys and the feature bit, from the next frame

    const Status status = CurrentStatus();
    std::string state;
    if (!on)
        state = "off";
    else if (!status.armed)
        state = "off for this session: the card was left alone (see the log)";
    else
    {
        // The card's own word: the parts and their values, the gates, and the counts.
        const bool veilAskedOn = (status.parts & flare::kPartVeil) != 0;
        state = std::format("ring x{:.2f}, sun x{:.2f}, ghost step x{:.2f}, parts 0x{:x}{}\n"
                            "patches: {} ring, {} sun, {} ghosts, {} veil skips; {} draws passed at the engine's own values, "
                            "{} refused",
                            double(status.ring), double(status.sun), double(status.ghost), status.parts,
                            veilAskedOn ? "" : " (the veil's add skipped)",
                            status.ringRuns, status.sunRuns, status.ghostRuns, status.veilSkips, status.passes, status.declined);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", state.c_str());
    ImGui::PopStyleColor();
}

const ext::menu::AutoRegister registration { { "ffxiv_lens_flare", "FFXIV", "Lens flare (FFXIV)", Draw, "ffxiv_engine_passes", Visible } };
} // namespace
} // namespace ext::ffxiv::flare::menu
