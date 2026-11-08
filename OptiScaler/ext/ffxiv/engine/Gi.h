#pragma once
// Screen-space light bounce (ext/ffxiv/engine/GiDraw.cpp): the module's interface.
//
// The module hangs off the engine pass override seam: the seam already hands every one of the game's draws to
// ext::ffxiv::engine::OnGameDraw, and that is where the calls into this module live. It recognises three of the frame's draws by the anchors
// the seam's own word carries - the deferred composite (the ambient probe table's pair, whose single target the frame's lit scene is), the
// engine fog post (the depth the rays march), and the tone map (its fallback owner when no earlier stage took the draw) - and it composes its
// own bounce light into a copy of the scene the tone-map chain has produced until then.
//
// The stage is the tone-map chain's member after the volumetric march: the march's stage hands the chain's current output to the hook pointer
// this module registers (ChainHooks.h's chainGiScene), and the stage hands its own texture back for the caller to bind at t0 - or null, and
// the chain's hold stands exactly as the march's does. When no earlier stage and the march answered, this stage is the chain's fallback owner
// (the chain's toneMapGiOwner hook seam: it makes its own scene copy, runs its three draws, and binds its output at t0 for the one draw).
// Every frame it cannot measure is left exactly as the chain left it (the chained decline leaves the chain's last good picture at t0 - the
// hold stands; the owner decline leaves the game's own binding), with one log line for the reason. Sixty-four counted refusals in a row
// switch it off for the session. It is inactive while the engine's scene HDR is on, and it refuses a character-preview replay of a game draw.
//
// The stamping contract: this stage's decline paths never touch the chain's shared state - the composite's capture stamp is the puddle stage's
// own file-local state, unreachable from this module's code, and the stage's answered-tracking is its own rendered-frame counter.

#include "ext/ffxiv/engine/GiRules.h"

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11DeviceChild;
struct ID3D11ShaderResourceView;

namespace ext::ffxiv::engine::gi
{
// The game's own draw call, behind a function pointer so the module never calls the seam's lambda twice (the march's own shape, and the chain
// hooks' ChainDrawFn is the same one type).
using DrawFn = void (*)(void* user);

// The one call in the engine module's draw path: keeps this frame's composite views against the scene it wrote, keeps the fog post's depth
// beside it, and runs the pass as the fallback owner at a tone map that no earlier stage took. Returns true when this module drew the game's
// draw itself.
bool OnSeamDraw(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn draw, void* user);

// The chained role, behind the march's stage: the entry the march's chain functions call with the chain's current output. A texture comes
// back for the caller to bind at t0; null means this stage has nothing for this frame, and the caller's own texture - the chain's last good
// picture - stays at t0 (the hold stands). Runs at most one tone map a frame, by this stage's own counter (the stamping contract's own).
ID3D11ShaderResourceView* ChainScene(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene);

// The fallback-owner role, behind the march's stage: makes its own scene copy of the game's scene, runs its three draws, and binds its output
// at t0 for the one draw, putting the game's own binding back afterwards - also when that draw throws. True when it drew the game's draw
// itself.
bool ToneMapOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user);

// Whether the module wants to see a draw at all: the key is on, the strength is above zero, the engine's scene HDR path is off (read live, so
// switching it never leaves the module armed and inert), and the module has not stood down.
bool Armed();

// The engine's per-frame sample of the scene HDR state, the march's own pattern: the HDR path takes every game draw before the draw seam hands
// this stage one, so the sample is taken on a pixel-shader bind, once an output frame.
void SampleSceneHdr();

// The composite-shader stamp: the slots a shader declares of the pass's own texture family (the normal target, the albedo carrier, the view
// positions and the camera table), packed once at creation and read at the frame's composite - the occlusion pass's stamp pattern, one word on
// the shader object itself so nothing has to be looked up later. A shader that declares none of the four answers 0, and takes no stamp.
void NoteShader(ID3D11DeviceChild* shader, const void* code, size_t size);

// What the card shows: the switch, the tuning, and what the session has done so far.
struct Status
{
    bool enabled = false;     // the [FFXIV] Gi key
    bool armed = false;       // the key is on, the strength is above zero, the engine's scene HDR is off, and the module has not stood down
    bool hdrInactive = false; // the key is on but the engine's scene HDR path replaces the tone chain this stage draws before
    float strength = kDefaultStrength;
    uint64_t frames = 0;
    uint32_t runs = 0;      // tone-map draws this stage composited into
    uint32_t held = 0;      // of those, the frames drawn with the held state (this frame's own read had not landed)
    uint32_t declined = 0;
    uint32_t skipped = 0;   // frames left as they were without counting: the two numbers below are its whole decomposition
    uint32_t noGBuffer = 0;
    uint32_t noDepth = 0;
    uint32_t noCamera = 0;
    uint32_t refused = 0;   // the camera refusals (a build mismatch), counted toward the stand-down
    uint32_t windowWidth = 0, windowHeight = 0;
    uint32_t giWidth = 0, giHeight = 0;
};
Status CurrentStatus();

// Publishes the card's values to the render thread: the parameters and the feature bit, plus a re-arm after the module stood itself down.
void Publish();

// The config hook (set through FfxivConfig.h's pointer, so ExtConfig.cpp keeps no link dependency on this file).
void OnConfigLoaded(bool on);

// Releases the device objects while the device is still alive (the engine's teardown path).
void Release();
} // namespace ext::ffxiv::engine::gi
