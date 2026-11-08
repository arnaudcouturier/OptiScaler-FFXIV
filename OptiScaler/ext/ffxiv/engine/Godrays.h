#pragma once
// God rays: the volumetric march (ext/ffxiv/engine/GodraysDraw.cpp): the module's interface.
//
// The module hangs off the engine pass override seam: the seam already hands every one of the game's draws to
// ext::ffxiv::engine::OnGameDraw, and that is where the calls into this module live. It recognises three of the frame's draws by the
// names their pixel shaders declare plus their own shape - the engine's fog post (cFogParam, and none of the material family's fog
// buffer), the shadow atlas' assembly draws (they declare g_LightParam, so they arrive as light draws), and the tone map (cCommonTexParam,
// one FP16 target at the scene's size, its t0 is that scene) - and it marches the frame's air along the shadow light's direction into its
// own copy of the scene for the tone map's one draw.
//
// The march is the tone-map chain's last member: the radial shafts own the draw when they are on and hand their output to the puddles'
// stage, which hands the chain's output to this march; when neither earlier stage answered, the march is the fallback owner and makes its
// own scene copy. Every frame it cannot measure is left exactly as the chain left it (the chained decline leaves the chain's last good
// picture at t0 - the hold stands; the owner decline leaves the game's own binding), with one log line for the reason. Sixty-four counted
// refusals in a row switch it off for the session. It is inactive while the engine's scene HDR is on, and it refuses a character-preview
// replay of a game draw.
//
// The stamping contract: the march's decline paths never touch the chain's shared state - the composite's capture stamp is the puddle
// stage's own file-local state, unreachable from this module's code, and the march's own answered-tracking is its own rendered-frame
// counter.

#include "ext/ffxiv/engine/GodraysRules.h"

#include <cstdint>

struct ID3D11DeviceChild;
struct ID3D11DeviceContext;

namespace ext::ffxiv::godrays
{
// The game's own draw call, behind a function pointer so the module never calls the seam's lambda twice (the same shape the shafts' own
// DrawFn has; declared here so this header needs no link dependency on that module).
using DrawFn = void (*)(void*);

// The one call in the engine module's draw path: captures the fog state at the fog post, the cascades and the atlas at the assembly
// draws, and runs the march as the fallback owner at a tone map neither earlier stage took. Returns true when this module drew the game's
// draw itself.
bool OnSeamDraw(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn draw, void* user);

// Whether the march wants to see a draw at all: the key is on, the intensity is above zero, the engine's scene HDR path is off (read
// live, so switching it never leaves the module armed and inert), and the module has not stood down.
bool MarchArmed();

// Whether the draw seam has anything to do for this module: the march is armed, or shadow mapping's night curve asked for the fog sun (FogSun.h) and
// the fog post's sun row is being copied with the march off. The march's own branches (the light and composite captures, the tone map) are behind
// MarchArmed(): with the fog sun alone, the tone map is the game's own.
bool Armed();

// The fog sun flag changed: re-evaluates the feature bit (registered with FogSun.h at start-up; not for callers).
void OnFogSunWantedChanged();

// The engine's per-frame sample of the scene HDR state, the radial's own pattern: the HDR path takes every game draw before the draw seam
// hands this module one, so the sample is taken on a pixel-shader bind, once an output frame.
void SampleSceneHdr();

// The fog-post shader's stamp: the constant-buffer slot its own reflection names cExpHeightFogParam at, written once on the shader object
// at creation (the light bounce's composite stamp pattern) and read at the fog post's draw to pick the height profile by that slot and not
// by the shape of the bytes. A shader that does not declare both the fog constants and the profile takes no stamp.
void NoteShader(ID3D11DeviceChild* shader, const void* code, size_t size);

// What the card shows: the switch, the tuning, and what the session has done so far.
struct Status
{
    bool enabled = false;     // the [FFXIV] GodraysVolumetric key
    bool armed = false;       // the key is on, the intensity is above zero, the engine's scene HDR is off, and the module has not stood down
    bool hdrInactive = false; // the key is on but the engine's scene HDR path replaces the tone chain this march draws before
    float intensity = kDefaultIntensity;
    uint64_t frames = 0;
    uint32_t runs = 0;      // tone-map draws this march composited into
    uint32_t held = 0;      // of those, the frames drawn with the last accepted state (this frame's own read had not landed)
    uint32_t declined = 0;
    uint32_t skipped = 0;   // frames left as they were without counting: the three numbers below are its whole decomposition
    uint32_t noAtlas = 0;
    uint32_t noFog = 0;
    uint32_t noLight = 0;
    uint32_t refused = 0;   // the camera refusals (a build mismatch), counted toward the stand-down
    uint32_t reads = 0;
    uint32_t notReady = 0;
    uint32_t windowWidth = 0, windowHeight = 0;
    uint32_t marchWidth = 0, marchHeight = 0;
};
Status CurrentStatus();

// Publishes the card's values to the render thread: the parameters and the feature bit, plus a re-arm after the module stood itself down.
void Publish();

// The config hook (set through FfxivConfig.h's pointer, so ExtConfig.cpp keeps no link dependency on this file).
void OnConfigLoaded(bool on);

// Releases the device objects while the device is still alive (the engine's teardown path).
void Release();
} // namespace ext::ffxiv::godrays
