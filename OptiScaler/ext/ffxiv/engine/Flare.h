#pragma once
// Lens flare: the flare family's own card (ext/ffxiv/engine/FlareDraw.cpp): the module's interface.
//
// The card hangs off the engine pass override seam: the seam already hands every one of the game's draws to ext::ffxiv::engine::OnGameDraw,
// and that is where the calls into this module live. It recognises the family's four draws by the names their shaders declare (the ring's
// cHaloParam, the sun's cSunParam, each radial-blur pass's cRadialBlurParam, and the veil by its sToneMap-only, no-constant-buffer
// reflection split by the target's size) and, per part, either patches one buffer's one float4 for exactly that draw (the GPU scale, which
// leaves every other component's bytes untouched) or skips the veil's add. A draw the card asks nothing of is the game's own call, and the
// two parts' gates are the same equality gate the other knobs of the seam have: a scale at the engine's own value emits no entry at all.
//
// Sixty-four counted refusals in a row stand the card down for the session. A character-preview replay of a game draw is never patched and
// never skipped - the replay must reproduce the game's own picture to the byte. Nothing here writes a game resource and every binding the
// patch takes is put back; the patch runs on the game's own immediate context between its binds.
//
// The card is the flare part-B slice (LENS-1); the in-frame capture (the flare study's FLARE-1) is its acceptance gate, not a prerequisite.

#include "ext/ffxiv/engine/FlareRules.h"

#include <cstdint>

struct ID3D11DeviceContext;

namespace ext::ffxiv::flare
{
// The game's own draw call, behind a function pointer so the module never calls the seam's lambda twice (the same shape the shafts' own
// DrawFn has; declared here so this header needs no link dependency on that module).
using DrawFn = void (*)(void*);

// The shader-creation hook, called from the engine module's own (ext/ffxiv/engine/EnginePass.cpp) whatever the keys say: a switch turned on
// later finds the family's shaders. One byte scan; the reflection of a draw that names one of the four names is parsed once per shader object.
void NoteShader(void* shader, const void* code, size_t size);

// The pixel-shader-bind hook, called from the engine module's own while the card's feature bit is on: the stamp of the bound shader, so a draw
// costs one relaxed load. An `instances` other than 0 is a class-instance override, which is never one of these draws.
void OnPixelShaderSet(const void* shader, uint32_t instances);

// The one call in the engine module's draw path, before the seam's pass-through: patches one buffer's float4 (the ring, the sun, one ghost
// pass), skips the veil's add when its part is asked off, and hands every other draw to the game's own call below. Returns true when this
// module made the draw's decision itself (the skip), false - the usual - when the game's draw still runs through the seam.
bool OnSeamDraw(ID3D11DeviceContext* context, DrawFn draw, void* user);

// Whether the card wants to see a draw at all: the key is on, a part is off the engine's own picture, and the card has not stood down.
bool Armed();

// What the card shows: the switch, the parts and scales, and what the session has done so far.
struct Status
{
    bool enabled = false;    // the [FFXIV] FlareCard key
    bool armed = false;      // the key is on, a part is off the engine's own picture, and the card has not stood down
    float ring = kDefaultPartScale;
    float sun = kDefaultPartScale;
    float ghost = kDefaultPartScale;
    uint32_t parts = kFlarePartsAll;
    uint32_t ringRuns = 0;   // ring draws patched
    uint32_t sunRuns = 0;    // sun draws patched
    uint32_t ghostRuns = 0;  // radial-blur draws patched
    uint32_t veilSkips = 0;  // veil adds skipped
    uint32_t passes = 0;     // flare draws seen and left as the game drew them (the scale at the engine's own value, or the veil on)
    uint32_t declined = 0;   // draws the card asked for and was refused
};
Status CurrentStatus();

// Publishes the card's values to the render thread: the parameters, the arming, the feature bit, and a re-arm after the card stood itself
// down. Called after every menu change and at the end of a config load.
void Publish();

// The config hook (set through FfxivConfig.h's pointer, so ExtConfig.cpp keeps no link dependency on this file).
void OnConfigLoaded(bool on);

// Releases the device objects while the device is still alive (the engine's teardown path).
void Release();
} // namespace ext::ffxiv::flare
