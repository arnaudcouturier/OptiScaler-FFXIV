#pragma once
// Contact shadows before the sun light pass: the reflection index and the injection.
//
// The reflection index: the pixel shaders the game creates are parsed into the slots of the four names this feature binds by
// (g_SamplerShadow, g_SamplerDepth, g_LightParam, g_CameraParameter; the light draw binds no view-position buffer, so the
// position is reconstructed from the depth). The index is filled whatever the key says, so a switch turned on in a running session finds
// the game's light shader; nothing is logged while the key is off. A draw is a sun light draw when its shader is in the index and the
// runtime checks of ContactShadowsRules.h pass, so no shader CRC is compared and a recompile of the game's shaders does not break the
// feature.
//
// The injection: for exactly one draw the mask's binding is replaced by this pass's copy, and the game's own binding is put back
// afterwards - also when the pass threw, and also when the draw declined. Nothing else of the frame is touched: the pass runs in its own
// device-context state, the render targets and shaders it uses are its own, and the game's constant buffers are only ever read (copied GPU
// side, never mapped).

#include "ext/ffxiv/relight/ContactShadowsPass.h"
#include "ext/ffxiv/relight/ContactShadowsRules.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <unordered_map>

struct ID3D11DeviceContext;
struct ID3D11PixelShader;

namespace ext::ffxiv::relight
{
// The slots one pixel shader binds the five names at (ContactShadowsRules.h lists the names).
struct Layout
{
    bool ok = false;
    uint32_t found = 0; // which of the four names were seen (1 mask, 2 depth, 4 g_LightParam, 8 g_CameraParameter): for the log
    uint32_t maskSlot = 0;
    uint32_t depthSlot = 0;
    uint32_t lightParamSlot = 0;
    uint32_t cameraParamSlot = 0;
};

// The slots of a shader's bytecode. Pure (no device): it reads the RDEF chunk, so a test can call it on a shader compiled in the test.
// `ok` is false when any of the five names is missing, which is the "not a sun light draw" answer.
Layout LayoutOfShader(const void* code, size_t size);

// One shader whose draws were refused for their shape (it binds something other than the screen-space mask), with the time of that
// refusal: the memory is what keeps the cascade atlas writer's draws from paying the state queries on every shadow pass, and its age is
// what lets a re-created mask (a resolution change) be looked at again.
struct ShapeRefusal
{
    int64_t refusedQpc = 0;
};

// One session's state, so a test builds its own on a software device instead of reaching into the product singleton.
struct State
{
    Pass pass;
    Session session;
    LogGate log;
    Params params;
    // The last sun direction the shaders reported, for the "moves faster than the sun can" check.
    bool haveSun = false;
    float lastSun[3] {};
    int64_t lastSunQpc = 0;
    // When the one-time camera-layout check last ran: a light draw whose camera buffer it refused is looked at again
    // (ContactShadowsRules.h's kCameraCheckSeconds) rather than switching the feature off.
    int64_t lastCameraCheckQpc = 0;
    uint32_t runs = 0;
    uint32_t declined = 0;
    CostMeter cost;        // the on-cost sample of the draw hook
    BindCostMeter bindCost; // and of the pixel-shader seam that feeds it
    uint64_t drawsSeen = 0;
    uint64_t bindsSeen = 0;
    // The layout index of the pixel shader the game has bound, tracked by the PSSetShader seam: 0 when the bound shader is not a sun
    // light draw. The draw path reads this instead of asking the context for the shader (a PSGetShader per draw was 0.65 ms a frame).
    std::atomic<uint32_t> boundLayoutId { 0 };
    std::atomic<bool> inPass { false }; // true while this module's own pass sets its shaders, which must not overwrite the tracker
    // The one-frame dump of the developer check: the delay the key holds, when the session started, and whether it has been written.
    int32_t dumpDelaySeconds = 0;
    int64_t dumpStartQpc = 0;
    bool dumpWritten = false;
    // Layouts whose draws were refused for their shape, with when: filled and read on the render thread only.
    std::unordered_map<uint32_t, ShapeRefusal> refusedShapes;
    bool loggedOff = false; // the one line that says the module switched itself off for the session
    bool indexedLogged = false; // the one line that says the first sun light draw shader was indexed
    bool partialLogged = false; // the one line that reports a mask shader missing one of the other names
    Microsoft::WRL::ComPtr<ID3D11Buffer> readback; // the staging copy of the sun vector, read once per session
    Microsoft::WRL::ComPtr<ID3D11Buffer> cameraReadback; // the staging copy of the camera matrices, read once per session
};

// The game's own draw call, behind a function pointer so one implementation serves the product and a test.
using DrawFn = void (*)(void* user);

// Runs the pass for one draw and binds its copy for exactly that draw: the game's shader resource is restored before this returns, on
// every path. `layout` is the shader's reflection (null = the shader is not in the index). Returns the decline, or None when the copy was
// bound and the draw ran with it.
Decline ApplyDraw(ID3D11DeviceContext* context, State& state, const Layout* layout, DrawFn draw, void* user);

// The product entry (one guarded call in ext/ffxiv/lighting/FfxivLightingCapture.h's draw hook): takes the layout of the shader the seam
// tracked, logs a change of state with the budget of ContactShadowsRules.h, and calls ApplyDraw. Always calls the game's draw exactly once.
void OnGameDraw(ID3D11DeviceContext* context, DrawFn draw, void* user);

// The pixel-shader seam (the context's PSSetShader, ext/ffxiv/lighting/FfxivLightingCapture.h's OnSetPs): remembers which layout the game
// has bound, so a draw costs one integer load instead of a PSGetShader. `instances` other than 0 means the bound shader is a class-instance
// override, which is never a sun light draw: the tracker records 0. One relaxed load and one out-of-line call while the key is off, behind a
// PSSetShader the game makes about 0.7 times per draw.
void OnPixelShaderSet(ID3D11PixelShader* shader, uint32_t instances);

// Writes one frame's buffers for the developer check ([FFXIV] ContactShadowsDump): the engine's mask before the pass, the term, the mask
// copy, the depth, the camera bytes and the sun, into OptiScaler\contact-dump\<date-time>\. Called at the first sun light draw that runs
// after the delay, once per session; nothing is copied unless the key asks for it.
void DumpFrame(ID3D11DeviceContext* context, State& state, const Inputs& inputs, const Pass& pass);

// The shader creation hook (FfxivLightingCapture.h's CreateShader): one byte scan of the bytecode and, only for a shader that names the
// mask, an RDEF parse and a small private stamp on the shader object - whatever the key says, so the index is filled before the player can
// reach the card. No log line while the key is off.
void OnShaderCreated(ID3D11PixelShader* shader, const void* code, size_t size, uint32_t crc);

// Whether the module wants to see this draw at all: the key is on and the module has not switched itself off for the session.
bool Armed();

// The tuning parameters this session runs with (the keys, clamped).
Params CurrentParams();

// Publishes the card's values to the render thread: the tuning parameters and the feature bit, plus a re-arm when the switch is turned back
// on after the module switched itself off. The menu card calls this after every change, so a switch turned on in a running session arms the
// pass and the sliders reach the next draw (the reflection index is filled whatever the key says).
void Publish(bool on);

// Forgets every device object and the session's counters (a config reload that switches the key off).
void Reset();

// The draw hook seam's wrapper: runs the game's draw with the mask copy bound when the pass can run, and the game's draw alone otherwise.
template <class F> inline void BoundDraw(ID3D11DeviceContext* context, F& draw)
{
    struct Thunk
    {
        static void Call(void* user) { (*static_cast<F*>(user))(); }
    };
    OnGameDraw(context, &Thunk::Call, &draw);
}
} // namespace ext::ffxiv::relight
