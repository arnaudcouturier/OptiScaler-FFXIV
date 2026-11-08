#pragma once
// Engine light controls (ENG2, ext/ffxiv/enginelight/EngineLightDraw.cpp): the module's interface.
//
// What it is. The lamp arm (EngineLightLamps.h), the exposure hold at the tone map, the night weight and the keys / card for the whole slice. It replaces the retired night
// grade (a luminance-shaped darkening the user rejected) and takes over its place in the tone-map chain: right behind the light bounce, ahead of the atmosphere and depth of
// field (the order as built is the god-rays march, the light bounce, THIS, the atmosphere, depth of field; ChainHooks.h writes it in its two dispatchers). The feature bit
// (ext::F::NightGrade) and the F9 stage (stage 13, "night-grade" in the file names) keep the slot's names.
//
// The weight. The shared night curve (the Shadow mapping module's hook, evaluated with its toggle forced) smoothed in wall-clock time, gated by the controller
// (EngineLightRules.h): the controls wait for the engine's exposure to settle on its own light, latch it, then ramp in. This module asks for the curve's feed itself, while it
// is armed, whatever any other key says (the retired grade's feed ran only on its own request, which starved a lamps-only configuration).
//
// Publishing. Every change of the keys or of the applied weight is handed to writer 1's cb-patch family through the contract's PublishCbFamily (null-checked) and to the lamp
// arm as one factor.

#include "ext/ffxiv/engine/ShaftsRules.h" // ToneMapFacts, the chain's common currency
#include "ext/ffxiv/enginelight/EngineLightLamps.h"
#include "ext/ffxiv/enginelight/EngineLightRules.h"

#include <cstddef>
#include <cstdint>
#include <string>

struct ID3D11DeviceChild;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace ext::ffxiv::enginelight
{
// The one call in the engine module's draw path: the frame tick at the composite and, at a tone map none of the earlier stages took, the fallback owner. Returns true when
// this module drew the game's draw itself.
bool OnSeamDraw(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn draw, void* user);

// The chained role, behind the light bounce's stage: a texture comes back for the caller to bind at t0; null means this stage has nothing for this frame (the hold is the
// identity, declined) and the caller's own texture stays. Every null hands the stage after this one the texture this call was given.
ID3D11ShaderResourceView* ChainScene(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene);

// The fallback-owner role: scales its own copy of the game's scene and binds it at t0 for the one draw, putting the game's own binding back afterwards - also when that draw
// throws. True ONLY when it drew the game's draw itself; a false obliges the caller to draw the game's call once (the chain's owner-return rule).
bool ToneMapOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user);

// Whether the module wants to see a draw at all: the master is on, something would change, it has not stood down, and the scene HDR path is off.
bool Armed();

// The engine's per-frame sample of the scene HDR state (the stages' own pattern), on a pixel-shader bind.
void SampleSceneHdr();

// The composite's stamp: the slot its g_CommonParameter sits at, written once at shader creation (the AO / light bounce pattern). Only a shader that declares both the
// ambient table and g_CommonParameter takes it.
void NoteShader(ID3D11DeviceChild* shader, const void* code, size_t size);

// The keys, as the config's load or the card publishes them. A slider move never resets the weight or the latch; the master going off keeps the latch following the engine.
void Configure(const Params& params);

// Device-side release (the engine seam's Release chain).
void Release();

// Host tests only: a clock (ticks and their frequency) in place of the wall clock. Null restores the wall clock.
void SetClockForTest(int64_t (*clock)(), int64_t frequency);

// What the card shows.
struct Status
{
    bool configured = false;
    bool armed = false;
    bool hdrInactive = false;
    bool standDown = false;       // the exposure stage stood itself down for the session (the lamps and the cb family go on)
    bool haveReading = false;     // the night curve has an input (a fog sun or a light colour)
    bool feedMasterOff = false;   // the curve's feed runs without the shadow module's master key
    Params params;
    Phase phase = Phase::Idle;
    float weight = 0.0f;          // the applied weight (what the lamps and the cb family use)
    float rawTarget = 0.0f;       // the night curve's answer
    const char* arm = "none";
    bool haveS = false;
    float s = 0.0f, y = 0.0f;     // the newest valid read of the engine's exposure
    bool haveHold = false;
    float sHold = 0.0f;
    bool forcedLatch = false;
    float gain = 1.0f;            // the compensation the stage applies now (CPU view of it)
    float lampFactor = 1.0f;
    bool haveTable = false;
    float tableLow = 0.0f, tableMid = 0.0f, tableHigh = 0.0f;
    uint64_t frames = 0;
    uint32_t runs = 0;
    uint32_t declined = 0;
    bool haveGpuMs = false;
    double gpuMs = 0.0;
    uint64_t lampsScaled = 0;     // volume draws the lamp arm scaled, since the start
};
Status CurrentStatus();

// The keys as the config holds them, handed to Configure (EngineLightConfig.cpp): the card calls Publish after a change, the config load calls OnConfigLoaded.
Params ParamsFromConfig();
void Publish();
void OnConfigLoaded(bool master);

// One line of "name=value" words for the stage dump's line and the log.
std::string StageValues();
} // namespace ext::ffxiv::enginelight
