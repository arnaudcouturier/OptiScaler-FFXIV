#pragma once
// Puddles, the tone-map stage: the module's interface (ext/ffxiv/engine/PuddleToneDraw.cpp, PuddleTonePass.{h,cpp},
// ext/ffxiv/engine/shaders/PuddleReflection.hlsl).
//
// The stage is the second half of the puddles: the gloss pass shapes the G-buffer where a pool is, and this one, at the frame's tone-map draw,
// darkens the pool's lit colour and adds a marched mirror of the scene to it. It reaches the tone map through the same draw seam as the light
// shafts, and the two are ordered: the shafts' own pass runs first (it adds air light to the scene), and this stage then runs on the shafts' output
// so the mirror reflects the lit scene - one module owns the tone map's t0, one copy serves both, and the sibling call in the draw seam is the
// fallback for a frame the shafts left alone. The scene HDR path never hands the seam a tone-map draw at all, which is why the chained order needs no
// second HDR rule; the fallback asks the same key itself.
//
// What it costs while it is off: one call per draw of the seam (one bit test), and nothing else. While it is on: one capture per frame at the
// composite that binds the frame's view positions, one small copy of the camera table, one draw into a texture of the scene's size whose window's
// ring is the only part copied, and one draw, at the tone map.

#include "ext/ffxiv/engine/EnginePass.h"      // DrawFn
#include "ext/ffxiv/engine/GlossPuddlesRules.h"
#include "ext/ffxiv/engine/ShaftsRules.h"     // the tone map's measured shape, which the shafts hand over with their output

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace ext::ffxiv::engine::gloss
{
// Whether this stage wants to see a draw at all: the card's switch, a coverage above zero, and a tuning that is not the frame's own picture (the
// darkening at zero with the reflection off is today's picture, so nothing is copied and nothing is drawn).
bool ToneWants(const Params& params);

// The seam's entry for this stage, called right after the light shafts' own. It records the frame's composite inputs on the draw that binds them
// and runs the stage at the frame's tone map, where it returns true when it drew that draw itself. Every other draw, and every refusal, returns
// false so the engine module runs the game's draw unchanged. A tone map the shafts' branch already answered through the chain below is not answered
// twice: the capture the chain spent is stamped, so this call returns false for it.
bool OnToneStageDraw(ID3D11DeviceContext* context, engine::Anchors anchors, engine::DrawFn draw, void* user);

// Whether the chained order has anything to do this frame: the card, the stage's own identity, the session, and a composite of this frame that no
// tone map has answered yet. The light shafts' tone-map branch asks this before its own copy is bound at t0; it is one test per tone map and no
// device work at all.
bool ToneArmedForChain();

// The chained run: the light shafts have already added their air light to `scene`, and this stage runs on that output, before the shafts bind t0. It
// returns the view the tone map should read (this module's own texture, the shafts' scene with the pool's colour in it), or null when the stage did
// nothing - no wetness, no captured inputs, a pass that could not run - in which case the caller keeps the shafts' own copy. `facts` is the tone
// map's shape as the shafts measured it at the draw.
ID3D11ShaderResourceView* OnShaftsScene(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene);

// The chained order's draw: the stage runs on `scene` when it has anything to do and the game's own draw then reads this module's texture, and when
// it does not the same draw reads `scene` - exactly one draw, and the game's own binding is back at t0 afterwards (also when the draw throws). This
// is the entry the light shafts call on the frames they leave as they were: with their card on, this stage's own fallback never sees the tone map,
// so without it a sun behind the camera or a rain-free sky would silently turn the pools off. Safe to call for a tone map the stage refused or a
// frame whose composite it has not seen: the draw is the only thing that happens.
void OnShaftsSceneDraw(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, engine::DrawFn draw,
                       void* user);

// Publishes the card's values to the render thread (called from the puddles module's own Publish, so one config load arms both stages).
void PublishTone(const Params& params);

// The raindrops' clock the last run of the stage used, in seconds from the module's first frame (0 before the first run). The pool's splash rings
// are the one thing in this pass that moves with real time, so a caller that has to reproduce the stage's own picture - the host test that compares
// the chained texture against a pass run on the same scene does exactly that - reads the instant from here instead of taking its own reading of a
// clock that moved in between.
float LastRippleSeconds();

// How strongly it was raining on the pools in that run, 0..1: the rings' amplitude, for the same caller.
float LastRainAmount();

// Gives the owned textures back, from the render thread only.
void ReleaseTone();

// What the card shows of this stage.
struct ToneStatus
{
    bool armed = false;    // the key, the coverage, a tuning that does something, and no switch-off
    bool identity = false; // the darkening is at zero with the reflection off: today's picture
    uint32_t runs = 0;
    uint32_t declined = 0;
    uint32_t held = 0;     // the frames answered with the last good output instead of a run (the hold): a session reads the ramp's skips here
    uint32_t captures = 0; // composites the frame's inputs were taken from
    bool statsKnown = false;
    float poolShare = 0.0f;
    float hitShare = 0.0f;
};
ToneStatus CurrentToneStatus();

// What one capture records about this stage, for engine.txt: the switch, the counts, one count per refusal reason and the last measurement.
struct ToneDiagnostics
{
    bool key = false;
    bool armed = false;
    bool identity = false;
    uint32_t runs = 0;
    uint32_t declined = 0;
    uint32_t declines[kToneDeclineReasonCount] {};
    bool statsKnown = false;
    float poolShare = 0.0f;
    float hitShare = 0.0f;
    bool haveRun = false;
    uint32_t sceneWidth = 0, sceneHeight = 0;
    float window[4] {};
};
ToneDiagnostics CurrentToneDiagnostics();

struct ToneFrameObservation
{
    uint64_t captureToken = 0;
    bool captureTaken = false, shapeWet = false;
    uint32_t runs = 0, declined = 0;
};
ToneFrameObservation CurrentToneFrameObservation();
} // namespace ext::ffxiv::engine::gloss
