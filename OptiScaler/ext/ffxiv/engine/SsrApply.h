#pragma once
// Screen-space reflections (ext/ffxiv/engine): the seam's interface.
//
// The module recognises a sampler of the engine's planar reflection map by the name its pixel shader declares (`g_SamplerReflectionMap`, a row of
// the shared name table, so the draw path recognises it with the anchors it already loads). Before the frame's FIRST water draw - a shader that
// declares the map, the engine's view-position buffer, the camera table and one of the two water anchors - it marches the view position against
// the scene the frame had rendered into that draw's own render target, writes its own reflection into a texture of the engine's map's size and
// format, and every water draw of that frame that binds the engine's map binds ours for that one call instead; a draw that samples the map without
// the water anchors keeps the engine's texel. The game's binding comes back afterwards.
//
// The pass runs once per frame, not once per reader: the engine binds one planar map for the whole frame (the captured frames bound the same
// object throughout), the water material draws of a frame are a few dozen, and a per-draw pass could not be paid for. The frame's first water draw
// records which of the engine's maps it bound and the frame's later readers are served from that memory.
//
// The seam costs one relaxed load per draw and one per pixel-shader bind while the key is off, and nothing else: a draw whose shader does not
// declare the name is passed on at that one load, without a state query.
//
// Every draw the seam passes on before the frame's one attempt is counted and written once a session (SsrRules.h's `PassedOn`): a draw that
// keeps the engine's texel is not a refusal and never stands the module down, and a session in which the pass never ran reads which gate stopped
// it instead of showing an armed card and a silent log.
//
// Threads: the draw seam, the shader-creation hook, the pass and the frame memory are the render thread's - the thread the game draws the frame on.
// Publish is the other one: a menu change, or a config load that may run inside the loader lock. It stores the parameters and the key atomically and
// then rewrites the frame memory and gives the owned textures back, and those two are the render thread's, so a publish that landed while a draw was
// inside the pass would not be synchronised with it. That gap is the ambient occlusion pass's as well and is not measured here; a menu writes on
// the thread that draws the next frame, and a config load happens before the first one.

#include "ext/ffxiv/engine/EnginePassRules.h" // the anchors word a reader draw is classified by
#include "ext/ffxiv/engine/SsrRules.h"
#include "ext/ffxiv/engine/SsrPass.h"

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceChild;
struct ID3D11DeviceContext;
struct ID3D11PixelShader;
struct ID3D11ShaderResourceView;

namespace ext::ffxiv::engine::ssr
{
// One shader creation. Stamps the slots a shader declares the planar map, the view-position buffer and the camera table at on the shader object
// itself, plus whether it is a water surface, so the frame's first water draw reads them with one call and no parse of its own: one byte scan for
// the reflection name, then the DXBC reflection of a shader that samples that map. Called whatever the key says, because the game creates its
// shaders while it loads; nothing is remembered anywhere else.
void NoteShader(ID3D11PixelShader* shader, const void* code, size_t size);

// The swap of one water draw's reflection slot, in place while the game's draw runs: the game's own view is put back when the guard goes out of
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

// The draw seam's entry: for a draw whose shader samples the engine's planar reflection map, the frame's pass (at the first water draw that can
// serve the frame) and the swap of that one draw's slot. A draw this module does not serve binds nothing and restores nothing.
BoundReader SwapReader(ID3D11DeviceContext* context, Anchors anchors);

// The swap itself, in one place and with nothing around it (SsrSwap.cpp): binds `replacement` at `slot` and answers the view the game had there,
// with one reference owned by the caller, or null when nothing was bound. SwapReader is the product's only caller; a device test drives it
// directly, together with the guard, to pin the restore.
namespace detail
{
// The stamp this module writes on a shader object: the packed word SsrRules.h's PackSlots makes from the slots that shader declares. It is the
// only thing this module remembers about a shader, and it lives on the object, so no table of shaders can lose it however many the game creates.
// A packed word of 0 is not written, and a shader that was never stamped reads 0.
bool StampShader(ID3D11DeviceChild* shader, uint32_t packed);
uint32_t ReadStamp(ID3D11DeviceChild* shader);

ID3D11ShaderResourceView* SwapSlot(ID3D11DeviceContext* context, uint32_t slot, ID3D11ShaderResourceView* replacement);
} // namespace detail

// Whether the draw path should see a draw at all: the key is on, the blend is not at zero, and the module did not switch itself off.
bool Armed();

// The parameters this session runs with (the keys, clamped).
Params CurrentParams();

// What the card and a capture show.
struct Status
{
    bool key = false;      // the [FFXIV] ScreenSpaceReflections key is on
    bool identity = false; // the blend is at zero: today's picture, so nothing runs
    bool armed = false;    // the pass can run now (the key is on, the blend is not zero, no switch-off)
    uint32_t runs = 0;     // passes run
    uint32_t readers = 0;  // water draws that bound this module's texture
    uint32_t declined = 0; // draws left alone, and why: the log
    uint32_t servedLastFrame = 0; // water draws the last finished frame served: a session's own answer to "how many readers are named"
    uint32_t passedOn[kPassedOnCount] {}; // draws passed on before the frame's one attempt, one count per PassedOn reason
    bool haveGpuMilliseconds = false; // the pass's own GPU time, once a timestamp pair has landed
    double gpuMilliseconds = 0.0;
    bool diagnostic = false;
    bool diagnosticCopyAvailable = false;
    uint32_t diagnosticMode = 0;
    bool diagnosticJVerifiedDistinct = false;
    bool diagnosticJRouteChanged = false;
    bool diagnosticJCompatible = false;
    bool diagnosticJPassCompleted = false;
    DiagnosticOperations diagnosticOperations {};
    uint64_t diagnosticForeignDraws = 0;
    uint32_t diagnosticForeignDrawsLastRun = 0;
    uint32_t diagnosticWindowOwner = 0, diagnosticWindowCaller = 0;
    uint32_t diagnosticPipelineBinds = 0;
    uint32_t diagnosticCopies = 0;
    uint32_t diagnosticBypasses = 0; // classified first-reader attempts for mode I; no Pass::Run was called
    uint32_t diagnosticDraws = 0;
    uint32_t diagnosticTimingBegins = 0;
    uint32_t diagnosticTimingEnds = 0;
    uint32_t diagnosticTimingGetDataCalls = 0;
    uint8_t diagnosticUavMaskBeforeCopy = 0, diagnosticUavMaskAfterCopy = 0, diagnosticUavLostMask = 0;
    bool diagnosticPredicatePresent = false;
    uint64_t diagnosticSuppressedCallbacks = 0;
    bool diagnosticSuppressionActiveDuringPass = false;
};
Status CurrentStatus();

// The number of `Decline` values, so a caller can carry one count per reason.
inline constexpr uint32_t kDeclineCount = kDeclineReasonCount;

// What one capture records about this module: the switch, the status, one count per decline reason, one count per passed-on reason, and the facts
// of the last pass.
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
    uint32_t passedOn[kPassedOnCount] {};
    // The slot and the view the last water draw the gate passed on had read: the stamp's word, the slot it names, the view's dimension, array
    // size and format, and the texture behind it. `have` is false until a draw was passed on after reading a slot.
    ReadBinding read;
    bool haveRun = false;
    uint32_t sourceWidth = 0, sourceHeight = 0; // the scene allocation the march reads
    uint32_t targetWidth = 0, targetHeight = 0; // the planar map the pass replaced
    float window[4] {};                         // the render window, in scene texels
    bool haveGpuMilliseconds = false;
    double gpuMilliseconds = 0.0;
};
Diagnostics CurrentDiagnostics();

// Read-only current-frame sample for the bounded draw diagnostic CSV.
struct FrameObservation
{
    uint64_t token = 0;
    bool hasToken = false, tried = false, valid = false;
    uint32_t served = 0, runs = 0, declined = 0;
};
FrameObservation CurrentFrameObservation();

// Publishes the card's values to the render thread: the parameters, the frame memory and the feature bit. Called after every menu change and at
// the end of a config load.
void Publish(bool on);
void PublishDiagnostic(bool on);
// WARP fixture seam: selects the same binding policy as the F7 cycle without requiring a desktop key state.
void SetDiagnosticModeForTest(uint32_t mode);

// Releases the device objects while the device is still alive (the engine's teardown path).
void Release();
// Advances the opt-in pair's nonblocking staging readback from an ordinary render callback.
void OnRenderCallback(ID3D11DeviceContext* context);
// Starts a fresh pair for each draw-diagnostic capture, including repeated F8 captures.
void OnDiagnosticCaptureStart();
} // namespace ext::ffxiv::engine::ssr
