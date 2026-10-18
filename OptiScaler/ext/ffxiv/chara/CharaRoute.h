#pragma once
// Which module handles a draw of the character preview feature. Pure: the dispatcher passes facts in and acts on the answer,
// so every rule is tested without a device.
//
//   PreviewDLSS (or PreviewNR) on and a path able to run -> the isolated DLSS / NR plumbing. The AA still takes the equipment display
//                      draw when the plumbing leaves it alone (no result for this draw yet) unless PreviewDLSS is on and its
//                      super-resolution path runs (FallBackToAa)
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
// to a path that will leave them native. `nrUsable` is "preview NR is wanted AND can run now" (nr::NrUsable, dlss::NrCanRun()), not the
// bare config key: with the key on but NR latched, unavailable or only draining its models, the draw goes to the spatial AA.
// An active NR path takes the draw before the spatial AA (NR replaces the native image the AA would filter); when no path can run the
// AA is used. A draw that the plumbing took the route for but did not run is offered to the AA afterwards (FallBackToAa).
constexpr Route ChooseRoute(bool dlssWanted, bool dlssActive, bool aaWanted, uint32_t roles, DrawApi /*api*/, bool nrUsable = false)
{
    if ((dlssWanted || nrUsable) && dlssActive)
        return Route::Dlss;
    if (aaWanted && (roles & RoleEquipDisplay) != 0)
        return Route::Enhance;
    return Route::Native;
}

// The route was Dlss and the module left the draw alone (dlss::Around returned false: it never called the draw). With PreviewAA on and the
// equipment display draw, the spatial AA takes it unless PreviewDLSS is on AND its super-resolution path can run (`srUsable`,
// dlss::SrCanRun()). So the AA is used while PreviewDLSS is off (preview NR is usable but has no result for this draw: the first renders,
// a render that was skipped, a finisher that was rejected) and while the PreviewDLSS pass cannot run (NGX not initialised, latched).
// While the super-resolution path runs the equipment draw stays native, as the specification says (its result is not ready yet). The
// native draw still runs exactly once: the AA runs it, or the dispatcher does when the AA declines too.
constexpr bool FallBackToAa(Route route, bool handled, bool dlssWanted, bool srUsable, bool aaWanted, uint32_t roles)
{
    const bool superResolutionRuns = dlssWanted && srUsable;
    return route == Route::Dlss && !handled && !superResolutionRuns && aaWanted && (roles & RoleEquipDisplay) != 0;
}

// A draw with the equipment display shader on a deferred context is only counted (the menu says why nothing happened).
constexpr bool CountsAsDeferredEquip(bool aaWanted, uint32_t roles, DrawApi /*api*/)
{
    return aaWanted && (roles & RoleEquipDisplay) != 0;
}
} // namespace ext::ffxiv::chara
