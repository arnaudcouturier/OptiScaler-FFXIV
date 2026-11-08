#pragma once
// Depth of field, Route B (ext/ffxiv/engine/DofDraw.cpp): the module's interface.
//
// The module hangs off the engine pass override seam: the seam already hands every one of the game's draws to
// ext::ffxiv::engine::OnGameDraw, and that is where the calls into this module live. It recognises the three of the
// frame's draws the march already names - the light draws (the camera buffer the CoC's metres reconstruct through), the
// engine's fog post (the scene depth and the pixel-to-NDC row the pack walks) and the tone map (cCommonTexParam, one
// FP16 target at the scene's size, its t0 that scene) - and it packs, blurs and merges the tone-map chain's output at a
// tone map: the pack + the disc blur + the merge, then the game's tone map reads this stage's copy of the scene at t0.
//
// The stage is the tone-map chain's LAST member (the order: shafts, puddles, march, light bounce, depth of field, t0). It hangs
// inside the light bounce's entries, one link further down than the bounce hangs inside the march's: the bounce's chained entry hands
// it the bounce's answer, or on every exit where the bounce has none the texture the bounce was handed (the march's answer, or what the
// march was handed), so it answers with the march and the bounce both off; the bounce's owner entry hands it the owner turn the same
// way. The stage's answer is what t0 reads; when no earlier stage took the tone map and this module is on, it is the chain's fallback
// owner. The owner-return rule holds here: true only when this stage drew the game's call. Every
// frame it cannot measure is left exactly as the chain left it (the chained decline leaves the chain's last good picture
// at t0 - the hold stands; the owner decline leaves the game's own binding, returns false and the caller draws the game's call once), with
// one log line for the reason. Sixty-four counted refusals in a row switch it off for the session. It is inactive while
// the engine's scene HDR is on, and it refuses a character-preview replay of a game draw.
//
// The stamping contract: the stage's decline paths never touch the chain's shared state - the composite's capture stamp
// is the puddle stage's file-local state, unreachable from this module's code - and the stage's answered-tracking is its
// own rendered-frame counter (dofFrame), never the shared stamp.

#include "ext/ffxiv/engine/DofRules.h"
#include "ext/ffxiv/engine/ShaftsRules.h" // ToneMapFacts, the chain's common currency

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace ext::ffxiv::dof
{
// The game's own draw call, behind a function pointer so the module never calls the seam's lambda twice (the march's own
// DrawFn shape; declared here so this header needs no link dependency on that module).
using DrawFn = void (*)(void*);

// The one call in the engine module's draw path: captures the camera at the light draws, the depth and the NDC row at
// the fog post, the scene at the composite, and runs the stage as the fallback owner at a tone map none of the earlier
// stages took. Returns true when this module drew the game's draw itself.
bool OnSeamDraw(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn draw, void* user);

// The chained role, behind the light bounce's stage: the entry the bounce's chained entry calls with the chain's current output (its own
// answer, or on every exit where it has none the texture it was handed). A texture comes back for the caller to bind at t0; null means this
// stage has nothing for this frame, and the caller's own texture - the chain's last good picture - stays at t0 (the hold stands). Answers at
// most one tone map a frame, by this stage's own counter (the stamping contract's own).
ID3D11ShaderResourceView* ChainScene(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene);

// The fallback-owner role, behind the light bounce's stage: makes its own scene copy of the game's scene, runs its three draws, and binds its
// output at t0 for the one draw, putting the game's own binding back afterwards - also when that draw throws. True ONLY when it drew the
// game's draw itself; a false (off, already answered, declined) obliges the caller to draw the game's call once.
bool ToneMapOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user);

// The engine's own depth of field (its weight pass, which writes the result back into the scene the tone map reads): stamped at shader creation and noted
// at the bind seam; a frame it drew in is left to the engine (the stage declines it, Decline::EngineDof).
void NoteShader(void* shader, const void* code, size_t size);
void OnPixelShaderSet(const void* shader, uint32_t instances);

// Whether the module wants to see a draw at all: the key is on, the strength and the aperture are above zero, the
// engine's scene HDR path is off (read live, so switching it never leaves the module armed and inert), and the module
// has not stood down.
bool Armed();

// The engine's per-frame sample of the scene HDR state, the march's own pattern: the HDR path takes every game draw
// before the draw seam hands this module one, so the sample is taken on a pixel-shader bind, once an output frame.
void SampleSceneHdr();

// What the card shows: the switch, the tuning, and what the session has done so far.
struct Status
{
    bool armed = false;       // the key is on, the strength and the aperture are above zero, the scene HDR is off, and the module has not stood down
    bool hdrInactive = false; // the key is on but the engine's scene HDR path replaces the tone map this stage draws before
    Params params;
    uint64_t frames = 0;
    uint32_t runs = 0;      // tone-map draws this stage composited into
    uint32_t declined = 0;
    uint32_t skipped = 0;   // frames left as they were without counting: the two numbers below are its whole decomposition
    uint32_t noDepth = 0;
    uint32_t noCamera = 0;
    uint32_t refusals = 0;  // the camera refusals (a build mismatch), counted toward the stand-down
    uint32_t windowWidth = 0, windowHeight = 0;
    uint32_t quarterWidth = 0, quarterHeight = 0;
};
Status CurrentStatus();

// Publishes the card's values to the render thread: the parameters and the feature bit, plus a re-arm after the module
// stood itself down.
void Publish();

// The config hook (set through FfxivConfig.h's pointer, so ExtConfig.cpp keeps no link dependency on this file).
void OnConfigLoaded(bool on);

// Releases the device objects while the device is still alive (the engine's teardown path).
void Release();
} // namespace ext::ffxiv::dof
