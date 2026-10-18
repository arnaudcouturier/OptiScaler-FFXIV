#pragma once
// Which module handles a draw of the character preview feature. Pure: the dispatcher passes facts in and acts on the answer,
// so every rule is tested without a device.
//
//   PreviewDLSS (or PreviewNR) on and a path able to run -> the isolated DLSS / NR plumbing, never the AA
//   PreviewAA on    -> spatial AA for the draw that shows the equipment preview image, also when PreviewDLSS is on but its path
//                      cannot run (NGX not initialised, the path latched after a failure). The reference build left that draw
//                      native; falling back to the AA instead is a deliberate product decision of this fork.
//   otherwise       -> the native draw
//
// The draw API does not take part in the decision. The game issues the equipment display draw as an indexed draw, not as a
// plain Draw, and any other API (instanced, indirect) is just as possible: the AA pass replaces a shader resource and runs
// the game's own draw call, so the call type is irrelevant to it. The parameter exists so that this stays tested.

#include "ext/ffxiv/chara/CharaDraw.h"
#include "ext/ffxiv/chara/CharaTags.h"

namespace ext::ffxiv::chara
{
enum class Route : uint8_t
{
    Native,
    Dlss,
    Enhance,
};

// `dlssActive` is "the SR path or preview NR can do work now" (dlss::CanRun(): wanted, not latched, gate open). It is NOT "the plumbing
// is alive": the plumbing also stays alive while a latched or switched-off path drains its slots, and those draws must not be routed
// to a path that will leave them native. `nrWanted` ([FFXIV] PreviewNR) offers the draws to that plumbing also when PreviewDLSS is off.
// An active NR path takes the draw before the spatial AA (NR replaces the native image the AA would filter); when no path can run the
// AA is used.
constexpr Route ChooseRoute(bool dlssWanted, bool dlssActive, bool aaWanted, uint32_t roles, DrawApi /*api*/, bool nrWanted = false)
{
    if ((dlssWanted || nrWanted) && dlssActive)
        return Route::Dlss;
    if (aaWanted && (roles & RoleEquipDisplay) != 0)
        return Route::Enhance;
    return Route::Native;
}

// A draw with the equipment display shader on a deferred context is only counted (the menu says why nothing happened).
constexpr bool CountsAsDeferredEquip(bool aaWanted, uint32_t roles, DrawApi /*api*/)
{
    return aaWanted && (roles & RoleEquipDisplay) != 0;
}
} // namespace ext::ffxiv::chara
