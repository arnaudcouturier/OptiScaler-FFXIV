#pragma once
// Atmosphere (ext/ffxiv/engine/AtmoDraw.cpp): the module's interface.
//
// The module hangs off the engine pass override seam: the seam already hands every one of the game's draws to ext::ffxiv::engine::OnGameDraw, and that is
// where the calls into this module live. It recognises the frame's draws the march and the light bounce already name - the light draws (the camera buffer
// and, when the draw is the sun's, the light's direction and colour), the engine's fog post (cFogParam and the height profile, the scene depth) and the
// tone map (cCommonTexParam, one FP16 target at the scene's size, its t0 that scene) - and it mixes the tone-map chain's output toward the air in front of
// every surface at the tone map: aerial perspective in an exponential height medium with a Henyey-Greenstein lobe around the sun (AtmoRules.h has the
// maths and its references), into its own copy of the scene; the game's tone map then reads that copy at t0.
//
// The stage sits between the light bounce and depth of field: the chain's order is shafts, puddles, march, light bounce, night grade, THIS, depth of field, t0
// (the darkening before the in-scatter). It hangs behind the night grade (ChainHooks.h's chainAtmoScene / toneMapAtmoOwner, reached through the dispatchers
// chainAfterNightScene / chainAfterGiScene, which the light bounce asks where it asked depth of field before) and it asks depth of field on every exit it has,
// so with the atmosphere off the chain is exactly the chain without it. The owner-return rule holds: true only
// when this stage (with depth of field composed behind it) drew the game's call. Every frame it cannot measure is left exactly as the chain left it (the
// chained decline leaves the chain's last good picture at t0 - the hold stands; the owner decline leaves the game's own binding, returns false and the
// caller draws the game's call once), with one log line for the reason. Sixty-four counted refusals in a row switch it off for the session. It is
// inactive while the engine's scene HDR is on, and it refuses a character-preview replay of a game draw.
//
// It needs a depth, a camera and a fog state; it does NOT need a sun: the sun's direction (the last sun-class light the light draws reported, held in world
// space) and colour are optional, and a frame without them runs on the ambient term alone. The camera does not depend on a sun either (CAMFIX): it comes from the copy
// taken at the sun-shaped light draw when there is one, else from the copy taken at the frame's first light draw of any shape (AtmoRules.h FoldCameraSources); the sun
// the first copy carried stands for kMaxCameraAge frames after it and no longer.
//
// The stamping contract: the stage's decline paths never touch the chain's shared state - the composite's capture stamp is the puddle stage's file-local
// state, unreachable from this module's code - and the stage's answered-tracking is its own rendered-frame counter (atmoFrame), never the shared stamp.

#include "ext/ffxiv/engine/AtmoRules.h"
#include "ext/ffxiv/engine/EnginePassRules.h" // Anchors
#include "ext/ffxiv/engine/ShaftsRules.h"     // ToneMapFacts, the chain's common currency

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceChild;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace ext::ffxiv::engine::atmo
{
// The game's own draw call, behind a function pointer so the module never calls the seam's lambda twice (the march's own DrawFn shape; declared here so this
// header needs no link dependency on that module).
using DrawFn = void (*)(void*);

// The one call in the engine module's draw path: captures the camera and the sun at the light draws, the fog state and the depth at the fog post and the
// scene at the composite, and runs the stage as the fallback owner at a tone map none of the earlier stages took. Returns true when this module drew the
// game's draw itself.
bool OnSeamDraw(ID3D11DeviceContext* context, Anchors anchors, DrawFn draw, void* user);

// The chained role, behind the light bounce's stage (ChainHooks.h's chainAtmoScene): the entry the bounce calls with the chain's current output. A texture
// comes back for the caller to bind at t0 (this stage's answer with depth of field composed on it, or depth of field's answer when this stage has none);
// null means nothing has anything for this frame and the caller's own texture - the chain's last good picture - stays at t0 (the hold stands). Answers at
// most one tone map a frame, by this stage's own counter.
ID3D11ShaderResourceView* ChainScene(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene);

// The fallback-owner role (ChainHooks.h's toneMapAtmoOwner): makes its own scene copy, runs its draw, composes depth of field on it, and binds the result at
// t0 for the one draw, putting the game's own binding back afterwards - also when that draw throws. True ONLY when it drew the game's draw itself; every exit
// where it did not draw asks depth of field's owner role, whose answer is returned.
bool ToneMapOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user);

// The fog post's profile-slot stamp: the constant-buffer slot its own reflection names cExpHeightFogParam at, written once on the shader object at creation
// (this module's own tag: the stage never reads another module's stamp) and read at the fog post's draw to pick the height profile by that slot and not by
// the shape of the bytes.
void NoteShader(ID3D11DeviceChild* shader, const void* code, size_t size);

// Whether the module wants to see a draw at all: the key is on, there is a medium and some light for it, the engine's scene HDR path is off (read live, so
// switching it never leaves the module armed and inert), and the module has not stood down.
bool Armed();

// The engine's per-frame sample of the scene HDR state, the march's own pattern: the HDR path takes every game draw before the draw seam hands this module
// one, so the sample is taken on a pixel-shader bind, once an output frame.
void SampleSceneHdr();

// What the card shows: the switch, the tuning, and what the session has done so far.
struct Status
{
    bool armed = false;       // the key is on, there is a medium, the scene HDR is off, and the module has not stood down
    bool hdrInactive = false; // the key is on but the engine's scene HDR path replaces the tone map this stage draws before
    Params params;
    uint64_t frames = 0;
    uint32_t runs = 0;     // tone-map draws this stage mixed into
    uint32_t declined = 0;
    SkipCounts skips;
    float engineDensity = 0.0f; // the engine's own fog density at the camera, per metre (the newest fog state)
    float engineScale = 1.0f;
    float densityPerMetre = 0.0f; // what the last run used
    bool haveSun = false;
    uint32_t windowWidth = 0, windowHeight = 0;
};
Status CurrentStatus();

// Publishes the card's values to the render thread: the parameters and the feature bit, plus a re-arm after the module stood itself down.
void Publish();

// The config hook (set through FfxivConfig.h's pointer, so ExtConfig.cpp keeps no link dependency on this file).
void OnConfigLoaded(bool on);

// Releases the device objects while the device is still alive (the engine's teardown path).
void Release();
} // namespace ext::ffxiv::engine::atmo
