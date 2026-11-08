#pragma once
// Engine pass overrides (ext/ffxiv/engine): the seam's interface.
//
// The module recognises one of the engine's own draws by the names its pixel shader declares (PassIndex.h), patches one of that draw's
// constant buffers for exactly that draw (CbPatch.h) and puts the game's own binding back. Its first knob scales the per-probe
// `skyVisibilityScale` of the ambient probe table the deferred composite reads, which is the engine's own multiplier on how much sky light a
// probe contributes: 1.0 is exactly the picture the artists baked. The second knob forces the wetness amount and the dryness blend the engine
// hands every material draw, which is what makes a dry stone plaza gloss over: -1.0 is the engine's own value and runs nothing, and the engine's
// rain-exposure mask still decides where the ground is sheltered.
//
// The seam costs one relaxed load per draw and one per pixel-shader bind while the key is off, and nothing else. The index is filled whatever
// the key says, so a switch turned on in a running session finds the shaders the game created while it loaded.

#include "ext/ffxiv/engine/EnginePassRules.h"

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11PixelShader;

namespace ext::ffxiv::engine
{
// The game's own draw call, behind a function pointer so one implementation serves the product and the tests.
using DrawFn = void (*)(void* user);

// The product entry of the draw seam: classifies the draw the seam tracked, runs the patch for it and always calls the game's draw exactly
// once.
void OnGameDraw(ID3D11DeviceContext* context, DrawFn draw, void* user);

// The pixel-shader seam: remembers which of the indexed shaders the game has bound, so a draw costs one integer load instead of a
// PSGetShader. `instances` other than 0 is a class-instance override, which is never one of these draws.
void OnPixelShaderSet(ID3D11PixelShader* shader, uint32_t instances);

// The shader-creation hook: one byte scan, then the reflection of a shader that names one of the buffers. Filled whatever the key says; one
// log line per session while it is on.
void OnShaderCreated(ID3D11PixelShader* shader, const void* code, size_t size, uint32_t crc);

// Whether the module wants to see this draw at all: the key is on (which the knob at 1.0 switches off) and the module has not switched itself
// off for the session.
bool Armed();

// Marks this module's own GPU work (a pass that draws through the same hook), so the draw seam never classifies, swaps or counts those draws.
void SetOwnWork(bool on);

// The parameters this session runs with (the keys, clamped).
Params CurrentParams();

// What the card shows: whether the draw path is armed, whether every knob is at the engine's own value, and what the session has done so far.
struct Status
{
    bool armed = false;      // the key is on, at least one knob is off the engine's value, and the module has not switched itself off
    bool identity = false;   // every knob is at the engine's own value: today's picture, so nothing is patched
    uint32_t runs = 0;       // composite draws the ambient knob patched
    uint32_t wetnessRuns = 0;  // wetness patches the wetness knob ran (one per frame and buffer, not one per draw)
    uint32_t wetnessSwaps = 0; // material draws that bound the patched wetness buffer
    uint32_t declined = 0;   // draws of either knob left alone, and why: the log
    uint32_t indexed = 0;    // bytecodes the shader index stores
    uint32_t indexRefused = 0; // new bytecodes the index's cap refused (their draws are left alone, fail open)
};
Status CurrentStatus();

// The number of `Decline` values (None .. Exception, TooManyBuffers included), so a caller can carry one count per reason without a guess at the table's length.
inline constexpr uint32_t kDeclineReasonCount = 12;
static_assert(uint32_t(Decline::None) == 0 && uint32_t(Decline::Exception) + 1 == kDeclineReasonCount,
              "kDeclineReasonCount must cover every engine::Decline value");

// What one capture records about this module, so a session can be read without a log: the switch, the status the card shows, one count per
// decline reason, and the facts of the last draw that was patched (the buffer's size, the composite's target and the draw's viewport).
struct Diagnostics
{
    bool key = false;      // the [FFXIV] EnginePassOverrides key is on
    bool armed = false;    // the draw path can patch a draw now (the key is on, the knob is not at 1.0, and the module did not stand down)
    bool identity = false; // the knob is at 1.0: today's picture
    uint32_t runs = 0;
    uint32_t declined = 0;
    uint32_t declines[kDeclineReasonCount] {}; // one count per Decline, indexed by the enum's value; none for Decline::None
    uint32_t indexed = 0;                      // bytecodes the shader index stores at the time of the capture
    uint32_t indexRefused = 0;                 // new bytecodes the index's cap refused (a session that outgrew it)
    bool haveRun = false;                      // a draw has been patched: the facts below are from the latest one
    uint32_t boundBytes = 0;                   // the ambient buffer's byte width at that draw
    uint32_t targetWidth = 0, targetHeight = 0; // the composite's render target
    float window[4] {};                         // that draw's viewport: x, y, width, height
};
Diagnostics CurrentDiagnostics();

// Publishes the card's values to the render thread: the parameters and the feature bit, plus a re-arm after the module switched itself off.
// Called after every menu change and at the end of a config load.
void Publish(bool on);

// Releases the device objects while the device is still alive (the engine's teardown path).
void Release();

// Marks one of this module's own GPU passes: the draw seam passes every draw it makes through unclassified, and the pixel-shader seam keeps the
// anchors it learned from the game instead of overwriting them with the pass's own shaders (the game's shader comes back with the device-context
// state swap, not with another PSSetShader, so a tracker that saw the pass would stay wrong until the game bound a new one). Nested scopes
// restore the previous state, so two modules' passes cannot clear each other's guard.
class OwnWork
{
  public:
    OwnWork();
    ~OwnWork();
    OwnWork(const OwnWork&) = delete;
    OwnWork& operator=(const OwnWork&) = delete;

  private:
    bool _previous = false; // the guard the enclosing scope had, put back by the destructor
};

// The draw hook's wrapper: runs the game's draw through this module's patch when the draw is one of its own, and the game's draw alone
// otherwise. Header only, so the seam needs no link dependency on the module.
template <class F> inline void BoundDraw(ID3D11DeviceContext* context, F& draw)
{
    struct Thunk
    {
        static void Call(void* user) { (*static_cast<F*>(user))(); }
    };
    OnGameDraw(context, &Thunk::Call, &draw);
}
} // namespace ext::ffxiv::engine
