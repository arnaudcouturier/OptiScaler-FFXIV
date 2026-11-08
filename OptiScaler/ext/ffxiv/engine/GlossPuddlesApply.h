#pragma once
// Puddles (ext/ffxiv/engine): the seam's interface.
//
// The module recognises a reader of one of the engine's own G-buffer targets by the name its pixel shader declares (the four G-buffer rows of
// the shared name table, so the draw path recognises it with the anchors it already loads) and by the object it binds there: the frame's first
// complete reader records which texture that is, and only draws that bind the same object are served. Before the frame's first such draw it
// copies that texture into a texture of the engine's own size and format and shapes the gloss channel on the up-facing ground where the engine's
// own wetness is above zero; every draw of the frame that binds the engine's texture binds this module's for that one call instead, and the
// game's binding comes back afterwards.
//
// The pass runs once per frame, not once per reader: a frame makes hundreds of bindings of one G-buffer target (the light draws, the composite
// and every forward draw), and a per-draw pass could not be paid for.
//
// The seam costs one relaxed load per draw and one per pixel-shader bind while the key is off, and nothing else: a draw whose shader does not
// declare one of those names is passed on at that one load, without a state query.

#include "ext/ffxiv/engine/EnginePassRules.h" // the anchors word a reader draw is classified by
#include "ext/ffxiv/engine/GlossPuddlesRules.h"

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceChild;
struct ID3D11DeviceContext;
struct ID3D11PixelShader;
struct ID3D11ShaderResourceView;

namespace ext::ffxiv::engine::gloss
{
// One shader creation. Stamps the slots a shader declares the depth, the camera table and the four G-buffer targets at on the shader object
// itself, so the frame's first reader reads them with one call and no parse of its own: one byte scan for the four target names, then the DXBC
// reflection of a shader that reads one of them. Called whatever the key says, because the game creates its shaders while it loads; nothing is
// remembered anywhere else.
void NoteShader(ID3D11PixelShader* shader, const void* code, size_t size);

// The swap of one reader draw's target slot, in place while the game's draw runs: the game's own view is put back when the guard goes out of
// scope, also when the draw throws. A guard built with nothing bound reports false and restores nothing.
//
// The caller makes the swap (detail::SwapSlot) and hands the guard the view it read, which the guard releases after restoring it; the constructor
// is public for that reason, so the swap and the restore can be driven on a device of their own.
class BoundReader
{
  public:
    BoundReader() = default;
    // A guard built with no view to restore binds nothing and restores nothing, which is how the caller of a swap that did not happen says so.
    BoundReader(ID3D11DeviceContext* context, uint32_t slot, ID3D11ShaderResourceView* original)
        : _context(original != nullptr ? context : nullptr), _slot(slot), _original(original)
    {
    }
    BoundReader(const BoundReader&) = delete;
    BoundReader& operator=(const BoundReader&) = delete;
    // Moving hands the game's binding (and the reference this guard owns) to the new guard, so a caller can return it by value.
    BoundReader(BoundReader&& other) noexcept : _context(other._context), _slot(other._slot), _original(other._original)
    {
        other._context = nullptr;
        other._original = nullptr;
    }
    BoundReader& operator=(BoundReader&&) = delete;
    ~BoundReader();

    bool Bound() const noexcept { return _context != nullptr; }

  private:
    ID3D11DeviceContext* _context = nullptr;
    uint32_t _slot = 0;
    ID3D11ShaderResourceView* _original = nullptr; // the game's own view, owned by this guard until it restores it
};

// The draw seam's entry: for a draw whose shader reads a G-buffer target, the frame's pass (at the first reader that can serve the frame) and the
// swap of that one draw's slot. The same call also notes the engine's wetness amount for the frame when the draw binds it: one GPU-side copy of
// 32 bytes per frame, from whichever material draw comes first in it, and - while the wetness knob is armed - the knob's forced amount written
// over the same buffer, because that is what the engine's own draws read. A draw this module does not serve binds nothing and restores nothing.
BoundReader SwapReader(ID3D11DeviceContext* context, engine::Anchors anchors);

// The swap itself, in one place and with nothing around it (GlossPuddlesSwap.cpp): binds `replacement` at `slot` and answers the view the game
// had there, with one reference owned by the caller, or null when nothing was bound. SwapReader is the product's only caller; a device test
// drives it directly, together with the guard, to pin the restore.
namespace detail
{
// The stamp this module writes on a shader object: the packed word GlossPuddlesRules.h's PackInputs makes from the slots that shader declares the
// depth, the camera table and the four G-buffer targets at. It is the only thing this module remembers about a shader, and it lives on the
// object, so no table of shaders can lose it however many the game creates. A packed word of 0 is not written, and a shader that was never
// stamped reads 0.
bool StampShader(ID3D11DeviceChild* shader, uint64_t packed);
uint64_t ReadStamp(ID3D11DeviceChild* shader);

ID3D11ShaderResourceView* SwapSlot(ID3D11DeviceContext* context, uint32_t slot, ID3D11ShaderResourceView* replacement);
} // namespace detail

// Whether the draw path should see a draw at all: the key is on, the coverage is above zero, and the module did not switch itself off. Either stage
// is enough: the tone-map stage has its own identity (the darkening at zero with the reflection off), so a session that turned the gloss write's
// target off still gets the pool's colour.
bool Armed();

// What the tone-map stage gates on: the wetness amount the gloss pass wrote into its own buffer, and whether the CPU knows it yet. With the wetness
// knob armed the amount is the knob's and is always known; without it, the engine's own amount arrives a frame behind (the pass's no-wait map) and
// is not known until the first copy lands. The shader reads the buffer either way - this is only what decides whether the stage is worth running at
// all, so a stale or unread figure can only skip a frame, never shape one wrongly.
struct Wetness
{
    float amount = 0.0f;
    bool known = false;
    float engine = 0.0f;       // the engine's own amount, whatever the knob forces: the rain signal reads this
    bool engineKnown = false;
};
Wetness CurrentWetness();

// The pass's own wetness buffer, which both stages read: the tone-map stage binds it so the two gate on one value.
ID3D11ShaderResourceView* WetnessResource();

// The wetness gate's one latch (GlossPuddlesRules.h's WetEnoughDwell), which both stages read: it is answered on the
// frame's wetness copy - the first draw of the frame that binds the engine's buffer - so the pool's two answers
// cannot disagree inside a frame, and so it is the same bit whether it is read at a reader draw or at the tone map.
bool WetLatched();

// Whether the gloss pass's camera layout check rejected the scene: the tone stage's hold answers a transient copy
// window (the last good answer keeps the ramp continuous), but a REJECTED scene is another scene - the hold's pools
// would freeze over it, so the answer falls back to the engine's picture until the new scene shapes.
bool GlossCameraRejected();

// Whether the composite of the frame `token` names reads the gloss stage's own shape (its own, or the hold that
// answered a refused attempt): the tone-map stage pairs its answer with that frame, so a mid-ramp skip holds the last
// answer instead of painting the engine's own unshaped picture. The token is the output-frame counter the draw path
// reads (PresentSeams.h), the same figure the frame cache and the tone stage's capture carry.
bool ShapeOnFrame(uint64_t token);

// The parameters this session runs with (the keys, clamped).
Params CurrentParams();

// What the card and a capture show.
struct Status
{
    bool key = false;      // the [FFXIV] Puddles key is on
    bool identity = false; // the coverage is at zero: today's picture, so nothing runs
    bool armed = false;    // the pass can run now (the key is on, the coverage above zero, no switch-off)
    uint32_t runs = 0;     // passes run
    uint32_t readers = 0;  // reader draws that bound this module's texture
    uint32_t declined = 0; // readers left alone, and why: the log
    uint32_t servedLastFrame = 0; // reader draws the last finished frame served: a session's own answer to "how many readers are named"
    bool wetnessKnown = false;    // the engine's wetness amount has been copied at least once this session
};
Status CurrentStatus();

// The number of `Decline` values, so a caller can carry one count per reason.
inline constexpr uint32_t kDeclineCount = kDeclineReasonCount;

// What one capture records about this module: the switch, the status, one count per decline reason, and the facts of the last pass.
struct Diagnostics
{
    bool key = false;
    bool identity = false;
    bool armed = false;
    uint32_t runs = 0;
    uint32_t readers = 0;
    uint32_t declined = 0;
    uint32_t servedLastFrame = 0;
    uint32_t declines[kDeclineCount] {};
    bool haveRun = false;
    uint32_t targetWidth = 0, targetHeight = 0; // the G-buffer target the pass shaped
    uint32_t targetIndex = 0;                   // the target key, as the shader's constants see it (0..3)
    uint32_t channel = 0;                       // the channel key, as the shader's constants see it (0..3)
    float window[4] {};                         // the render window, in target texels
    bool wetnessKnown = false;
};
Diagnostics CurrentDiagnostics();

// Read-only current-frame sample for the bounded draw diagnostic CSV.
struct FrameObservation
{
    uint64_t token = 0, wetnessToken = 0, shapeToken = 0;
    bool hasToken = false, tried = false, valid = false, wetnessKnown = false, wetLatched = false, shapePresent = false;
    float wetness = 0.0f;
    uint32_t served = 0, runs = 0, declined = 0;
};
FrameObservation CurrentFrameObservation();

// Publishes the card's values to the render thread: the parameters, the frame memory and the feature bit. Called after every menu change and at
// the end of a config load.
void Publish(bool on);

// Releases the device objects while the device is still alive (the engine's teardown path).
void Release();
} // namespace ext::ffxiv::engine::gloss
