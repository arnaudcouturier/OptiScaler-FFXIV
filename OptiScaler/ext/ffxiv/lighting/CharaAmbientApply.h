#pragma once
// The lighting tab's chara ambient (ext/ffxiv/lighting): the seam's interface.
//
// The module recognises a draw of the game's character shading family by the name its pixel shader declares (`g_AmbientParam`, the shared
// name table's chara row) and scales that draw's own ambient block - one float field, the block's reciprocal scale - for exactly that draw,
// keyed on the shadow mapping module's day/night curve. A character draw is a shader that declares the customize block beside the ambient
// block (the world's materials declare the ambient block alone). The curve's single number is the frame's own g_LightParam colour at the SUN
// light draw (the contact path's own name test), copied into a small staging ring at that draw and read no-wait a frame or two later (the
// readback-ring pattern the shadow mapping module and the light shafts both use).
//
// The seam costs one relaxed load per draw and one per pixel-shader bind while the key is off, and nothing else: a draw whose shader declares
// neither the chara block nor the light draw's parameters is passed on at that one load, without a state query. The recognition itself is the
// shared seen-table pattern: the shader object is stamped once at creation and the draw path answers from that table, with the object's own
// stamp catching a recycled address.

#include "ext/ffxiv/lighting/CharaAmbientRules.h"

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11PixelShader;

namespace ext::ffxiv::lighting::chara
{
// Called every draw of the frame (inside the capture hook chain, the same depth the engine pass overrides patch at). `draw` runs the game's
// draw call; a draw this module patches runs it once with its block's patched copy bound, and the game's own buffer comes back afterwards,
// also when the draw throws.
void OnGameDraw(ID3D11DeviceContext* context, void (*draw)(void*), void* user);

// One shader creation. Stamps the shader object with the slot its bytecode declares the chara ambient block at (and, for the curve's capture,
// the slot it declares the light draw's parameters at), whatever the key says, because the game creates its shaders while it loads.
void OnShaderCreated(ID3D11PixelShader* shader, const void* code, size_t size, uint32_t crc);

// The pixel-shader bind hook: tracks the bound shader's stamp so a draw costs one integer load, never a state query.
void OnPixelShaderSet(ID3D11PixelShader* shader, uint32_t instances);

// The seam's wrapper, in the hook chain's own shape: runs the game's draw through this module's patch when it is one of its own, and the
// game's draw alone otherwise. Header only, like the engine's and the contact shadows'.
template <class F> inline void BoundDraw(ID3D11DeviceContext* context, F& draw)
{
    struct Thunk
    {
        static void Call(void* user) { (*static_cast<F*>(user))(); }
    };
    OnGameDraw(context, &Thunk::Call, &draw);
}

// What the card shows: the switch, the keys, and what the session has done so far.
struct Status
{
    bool key = false;      // the [FFXIV] CharaAmbient key
    bool identity = false; // the lift is at 1.0: today's picture, so nothing runs
    bool armed = false;    // the key is on and the lift is off the engine's own value (the curve decides per draw)
    bool off = false;      // the session's stand-down: sixty-four of the knob's draws in a row were left alone
    float lift = kCharaLiftDefault;
    bool haveReading = false; // the curve's ring has landed a light colour at least once
    float curve = 0.0f;       // the newest blend's curve value, 0..1
    float lastScale = 1.0f;   // the newest blend's effective multiplier
    uint32_t runs = 0;        // chara draws patched
    uint32_t served = 0;      // binds answered from the cached copy
    uint32_t declined = 0;
};

Status CurrentStatus();

// The curve's reading as the capture ring holds it, for the card's status lines (the card asks the modules it assembles).
bool HaveColourReading();
float LastCurve();

// Publishes the card's values to the render thread: the keys and the feature bit. Called after every menu change and at the end of a config load.
void Publish(bool on);

// Releases the device objects while the device is still alive (the capture hook chain's teardown).
void Release();
} // namespace ext::ffxiv::lighting::chara
