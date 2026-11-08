#pragma once
// Light shafts before the tone map: the module's interface (ext/ffxiv/engine/ShaftsDraw.cpp).
//
// The module hangs off the engine pass override seam: the seam already hands every one of the game's draws to
// ext::ffxiv::engine::OnGameDraw, and that is where the one call into this module lives. It recognises three of the frame's draws by the names
// their pixel shaders declare plus their own shape - the light draws (g_LightParam, and the sun's own draw by its light-buffer pair written
// without blending), the deferred composite (the ambient probe table, whose single target is the frame's lit scene) and the tone map
// (cCommonTexParam, one FP16 target at the scene's size, its t0 is that scene) - and it composites the shafts into its own copy of the scene for
// the tone map's one draw.
//
// Everything it decides fails open: a frame it cannot measure leaves the game's draw exactly as it was, with one log line for the reason, and a
// frame without a sun in it (no sun draw, its bytes not arrived, a sun behind the camera) is skipped without counting. Sixty-four counted
// refusals in a row switch it off for the session.

#include "ext/ffxiv/engine/ShaftsRules.h"

#include <cstdint>

struct ID3D11DeviceContext;

namespace ext::ffxiv::shafts
{
// The game's own draw call, behind a function pointer so the module never calls the seam's lambda twice.
using DrawFn = void (*)(void* user);

// The one call in the engine module's draw path: records the frame's sun light draw and the composite's scene, runs the pass at the tone-map
// draw and always runs the game's draw exactly once. Returns true when this module drew the game's draw itself.
bool OnEngineDraw(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn draw, void* user);

// Whether the module wants to see a draw at all: the key is on, the intensity is above zero, the engine's scene HDR path is off (read live, so
// switching it never leaves the module armed and inert), and the module has not stood down.
bool Armed();

// The engine's per-frame sample of the scene HDR state. The engine's HDR path takes every game draw before the draw seam hands this module one,
// so the sample is taken on a pixel-shader bind, once an output frame: it is what writes the one line when HDR comes on while the shafts key is
// untouched, and what lets the pass start again on the frame it goes off.
void SampleSceneHdr();

// What the card shows: the switch, the tuning, and what the session has done so far (including the sun it last projected, which is what a
// session checks first when the shafts point the wrong way).
struct Status
{
    bool enabled = false;  // the [FFXIV] LightShafts key
    bool armed = false;    // the key is on, the intensity is above zero, the engine's scene HDR is off, and the module has not stood down
    bool hdrInactive = false; // the key is on but the engine's scene HDR path replaces the tone chain this pass draws before
    float intensity = kDefaultIntensity;
    uint64_t frames = 0;   // the module's own count of rendered frames the card's share is over
    uint32_t runs = 0;     // tone-map draws this pass composited into
    uint32_t held = 0;     // of those, the frames drawn with the last accepted sun (this frame's own read had not landed)
    uint32_t declined = 0; // tone-map draws left alone, and why: the log
    uint32_t skipped = 0;  // frames left as they were without counting: the five numbers below, which are its whole decomposition
    uint32_t noDepth = 0;     // ... no sun among the frame's light draws (none had the sun draw's shape, or its light was a lamp)
    uint32_t noSun = 0;       // ... the light's bytes had not arrived, or they are not a sun this pass can project
    uint32_t offScreen = 0;   // ... the sun projected behind the camera or outside the viewport margin
    uint32_t belowHorizon = 0; // ... the frame's light was refused as the sun's mirror (the direction pointed below the horizon)
    uint32_t notTheSun = 0;   // ... the light's direction is not the one the frame's directional shadow is built from
    uint32_t reads = 0;     // times the frame's light and camera bytes were read back
    uint32_t notReady = 0;  // read-backs the GPU had not finished (never a refusal)
    uint32_t refused = 0;   // read-back lights the frame's own shadow disagreed with; a refused read never draws, so a frame drawn from a held sun
                            // is not this count
    bool haveSun = false;  // a light draw of the frame carried a sun this module projected
    float sunDirection[3] {}; // the direction the engine reported for it (world space, as the light draws report it)
    float sunDiffuse[3] {};   // and the colour it reported (the live build reports zero, and the pass takes the sky's colour then)
    float sunScreen[2] {};    // the sun's position in the last window it was projected into
    uint32_t windowWidth = 0, windowHeight = 0; // that window
};

Status CurrentStatus();

// Publishes the card's values to the render thread and re-arms the module after it stood down. Called after every menu change.
void Publish();

// The config load: stores the key, reads the tuning keys and arms the seam's feature bit.
void OnConfigLoaded(bool on);

// Releases the pass's device objects while the device is still alive (the engine's teardown path).
void Release();
} // namespace ext::ffxiv::shafts
