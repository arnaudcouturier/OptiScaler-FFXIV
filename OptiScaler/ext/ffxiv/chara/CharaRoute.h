#pragma once
// Which module handles a draw of the character preview feature. Pure: the dispatcher passes facts in and acts on the answer,
// so every rule is tested without a device.
//
//   PreviewDLSS on  -> the isolated DLSS path (when it is available), nothing else
//   PreviewAA on    -> spatial AA for the draw that shows the equipment preview image
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

constexpr Route ChooseRoute(bool dlssWanted, bool dlssActive, bool aaWanted, uint32_t roles, DrawApi /*api*/)
{
    if (dlssWanted)
        return dlssActive ? Route::Dlss : Route::Native;
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
