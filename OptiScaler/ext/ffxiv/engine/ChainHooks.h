#pragma once
// The tone-map chain's stage hooks (ext/ffxiv/engine): the march is the chain's last member, and the two earlier stages call it through
// these pointers so the chain needs no link dependency between the modules. The pointers are null until the march's own object file is
// linked and its static initialisers run, which is the same answer the product gives with the march's key off - and the reason the
// puddle's and the shafts' own tests link without the march and run the chain unchanged.
//
// The hooks carry no state of their own: the march's draw path owns the frame's bookkeeping, and these are the two entries the chain's
// callers reach it by. Neither writes the chain's shared state - the composite's capture stamp is the puddle stage's own file-local
// state, and the march's decline paths never touch it (the stamping contract the landing review walks).

#include "ext/ffxiv/engine/ShaftsRules.h" // ToneMapFacts, the chain's common currency

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace ext::ffxiv::engine
{
// The game's own draw call, behind a function pointer (the same shape the shafts' own DrawFn has; declared here so this header needs no
// link dependency on that module).
using ChainDrawFn = void (*)(void*);

// The march chained on the chain's current output: runs its own pass on the texture (its own copy of it, the march, the composite) and
// returns its own texture for the caller to bind at t0, or null when it has nothing for this frame - in which case the texture stays as
// it is and the chain's hold stands (the chained decline never falls through to the engine's texel and never binds an unlit scene).
using ChainSceneFn = ID3D11ShaderResourceView* (*)(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts,
                                                   ID3D11ShaderResourceView* scene);

// The march as the chain's fallback owner: makes its own scene copy of the game's scene, runs its pass, binds its output at t0 for the
// one draw and puts the game's own binding back, also when that draw throws. True when it drew the game's draw itself. The owner-return rule
// of the chain: a fallback owner answers true ONLY if it drew, and a false obliges the caller to draw the game's call itself on the binding
// it already has (an owner that is off, already answered the frame, or declined returns false). Every owner hook added after this one
// follows it.
using ToneOwnerFn = bool (*)(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene,
                             ChainDrawFn draw, void* user);

// Set by the march's draw path at start-up; null in every build that does not link it (the tests, and the march being off).
inline ChainSceneFn chainMarchScene = nullptr;
inline ToneOwnerFn toneMapMarchOwner = nullptr;

// ---- The stage after the march: the screen-space light bounce (GiDraw.cpp). The names and the shape below are the contract a stage after it
// (depth of field) hangs on; it repeats the march's own pattern one link down.
//
// The march is off by default and returns early in six places, so a later stage is reachable only if every one of those exits hands it a
// turn. Where the light bounce is asked (GodraysDraw.cpp):
//   chainGiScene(context, facts, scene) - the texture hook, behind chainMarchScene's entry. Asked (1) with the march's own answer, inside the
//     march, once, when the march answered (the stage composes on the march's picture); (2) with the texture the march's chained entry was
//     given, on every exit where the march has no answer (its key is off, it answered the frame already, it declined, it threw). It returns
//     its own texture for the caller to bind at t0, or null, and a null leaves the texture it was handed standing (the hold stands). It
//     never binds, draws or touches the chain's capture stamp, and has its own one-answer gate on its own rendered-frame counter.
//   toneMapGiOwner(context, facts, scene, draw, user) - the owner hook, behind toneMapMarchOwner. Asked with the game's scene at both early
//     exits of the march's owner entry (key off, frame already answered) and after the march's own owner role declined (outside that role's
//     try, so a throw after the stage drew cannot make the march draw the game's call again). The owner-return rule above holds: true ONLY
//     if the stage drew, and a false obliges the caller to draw the game's call once on the binding it already has.
//   The seam's own entry is not a hook: the march's seam entry returns false at its two early exits and the engine pass dispatch calls
//     gi::OnSeamDraw right behind it (captures at the composite and the fog post, and the tone map's owner role).
// A stage after this one repeats the pattern: its texture hook is asked with this stage's answer (inside the stage, where it composes) and
// with the texture this stage was handed on every exit where this stage has no answer; its owner hook is asked at this stage's owner exits
// the same way, and an owner that is off or declined returns false.
inline ChainSceneFn chainGiScene = nullptr;
inline ToneOwnerFn toneMapGiOwner = nullptr;

// ---- The chain's order after the light bounce, AS BUILT: light bounce -> night grade -> atmosphere -> depth of field -> t0 (the darkening before the in-scatter,
// so a lamp's halo is not darkened; depth of field last). The light bounce names none of the stages behind it: it asks the two dispatchers at the bottom of this
// file (chainAfterGiScene / toneMapAfterGiOwner) where it used to ask depth of field, and the dispatchers know the order. Each stage behind the bounce hands the
// next LINKED stage the texture it made, or - on every exit where it has no answer - the texture it was handed; the owner hooks follow the same order and the
// owner-return rule above (true ONLY if the stage drew; a false obliges the caller to draw the game's call once).

// ---- The night grade (NightGradeDraw.cpp, L1): the stage right behind the light bounce.
//   chainNightScene(context, facts, scene) - the texture hook. Asked (through chainAfterGiScene) from the light bounce's chained entry (1) with the bounce's own
//     answer, inside the bounce, once, when the bounce answered; (2) with the texture the bounce's chained entry was given on every exit where the bounce has no
//     answer. It returns its own texture for the caller to bind at t0 or null (the hold stands), composes the NEXT stage (chainAfterNightScene: the atmosphere,
//     else depth of field) onto its own answer inside itself, and hands the next stage the texture it was given on every exit where it has no answer of its own
//     (its key is off, the night curve's weight is 0 and the engine's own picture stands, it answered the frame already, it declined, it threw).
//   toneMapNightOwner(context, facts, scene, draw, user) - the owner hook, with the owner-return rule: true ONLY if the stage drew; every exit where this stage
//     does not draw asks toneMapAfterNightOwner (the atmosphere's turn, else depth of field's).
//   The seam's own entry is not a hook: the engine pass dispatch calls nightgrade::OnSeamDraw right behind gi::OnSeamDraw.
inline ChainSceneFn chainNightScene = nullptr;
inline ToneOwnerFn toneMapNightOwner = nullptr;

// ---- The atmosphere (AtmoDraw.cpp): the stage behind the night grade. It repeats the same pattern one link down and asks depth of field on every exit it has.
//   chainAtmoScene(context, facts, scene) - the texture hook. Asked (through chainAfterNightScene, or through chainAfterGiScene when the night grade is not
//     linked) with the previous stage's answer inside that stage, once, when it answered, and with the texture that stage was handed on every exit where it has
//     none. It returns its own texture for the caller to bind at t0 or null (the hold stands), composes depth of field onto its own answer inside itself
//     (chainDofScene), and hands depth of field the texture it was given on every exit where it has no answer of its own (its key is off, it answered the frame
//     already, it declined, it threw). It never binds, draws or touches the chain's capture stamp.
//   toneMapAtmoOwner(context, facts, scene, draw, user) - the owner hook, with the owner-return rule above: true ONLY if the stage drew, and a false obliges the
//     caller to draw the game's call once on the binding it already has. Every exit where this stage does not draw asks toneMapDofOwner.
//   The seam's own entry is not a hook: the engine pass dispatch calls atmo::OnSeamDraw right behind nightgrade::OnSeamDraw (captures at the light draws, the fog
//     post and the composite, and the tone map's owner role).
inline ChainSceneFn chainAtmoScene = nullptr;
inline ToneOwnerFn toneMapAtmoOwner = nullptr;

// ---- Depth of field (DofDraw.cpp), the chain's last member before t0. It repeats the same pattern one link further.
//   chainDofScene(context, facts, scene) - the texture hook. Asked by the stage in front of it (the atmosphere; the night grade or the light bounce when the
//     stages between are not linked) (1) with that stage's own answer, inside the stage, once, when it answered (depth of field composes on that picture, in both
//     roles, before any bind); (2) with the texture that stage was handed on every exit where it has no answer (its key is off, its gate says the frame was
//     answered, it declined, it threw) - and that texture is the previous answering stage's picture, else what the march was handed. It returns its own texture
//     for the caller to bind at t0, or null, and a null leaves the texture it was handed standing (the hold stands). It never binds, draws or touches the chain's
//     capture stamp, and has its own one-answer gate on its own rendered-frame counter.
//   toneMapDofOwner(context, facts, scene, draw, user) - the owner hook. Asked with the game's scene at the early exits and the declines of the stage in front of
//     it (outside that role's try, so a throw after the stage drew cannot make the stage in front draw the game's call again). The owner-return rule above
//     holds: true ONLY if the stage drew, and a false obliges the caller to draw the game's call once on the binding it already has.
//   The seam's own entry is not a hook: the engine pass dispatch calls dof::OnSeamDraw right behind atmo::OnSeamDraw (captures at the fog post and the light
//     draws, and the tone map's owner role).
inline ChainSceneFn chainDofScene = nullptr;
inline ToneOwnerFn toneMapDofOwner = nullptr;

// ---- The two dispatchers: the only places the order is written.

// What the night grade hands the chain on: the atmosphere when it is linked, else depth of field (so a build, or a test, that links the night grade without the
// atmosphere reaches depth of field exactly as it would without the atmosphere). Null when neither is linked.
inline ID3D11ShaderResourceView* chainAfterNightScene(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene)
{
    if (chainAtmoScene != nullptr)
        return chainAtmoScene(context, facts, scene);
    if (chainDofScene != nullptr)
        return chainDofScene(context, facts, scene);
    return nullptr;
}

// The owner turn the night grade hands on: the atmosphere's when linked, else depth of field's, else false (the caller draws the game's call once).
inline bool toneMapAfterNightOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, ChainDrawFn draw, void* user)
{
    if (toneMapAtmoOwner != nullptr)
        return toneMapAtmoOwner(context, facts, scene, draw, user);
    if (toneMapDofOwner != nullptr)
        return toneMapDofOwner(context, facts, scene, draw, user);
    return false;
}

// What the light bounce hands the chain on: the night grade when it is linked, else what the night grade would hand on (the atmosphere, else depth of field), so
// a build, or a test, that links the bounce without the stages behind it reaches the next linked stage exactly as it would without them. Null when none is linked.
inline ID3D11ShaderResourceView* chainAfterGiScene(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene)
{
    if (chainNightScene != nullptr)
        return chainNightScene(context, facts, scene);
    return chainAfterNightScene(context, facts, scene);
}

// The owner turn the light bounce hands on: the night grade's when linked, else the one after it, else false (the caller draws the game's call once).
inline bool toneMapAfterGiOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, ChainDrawFn draw, void* user)
{
    if (toneMapNightOwner != nullptr)
        return toneMapNightOwner(context, facts, scene, draw, user);
    return toneMapAfterNightOwner(context, facts, scene, draw, user);
}
} // namespace ext::ffxiv::engine
