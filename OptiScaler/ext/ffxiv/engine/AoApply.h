#pragma once
// Ambient occlusion (ext/ffxiv/engine): the seam's interface.
//
// The module recognises a reader of the engine's own screen-space occlusion texture by the name its pixel shader declares (`g_SamplerOcclusion`,
// a row of the shared name table, so the draw path recognises it with the anchors it already loads). At the reader that completes the frame's
// three inputs it computes its own occlusion from the game's depth and G-buffer normal into a texture of the engine's own size and format, and
// every later draw of that frame that binds the engine's texture binds ours for that one call instead; the game's binding comes back
// afterwards. The readers of that frame that ran before the completing one keep the engine's texture: the pass cannot run at a draw the frame
// has already gone past, so such a frame is served from that reader onward and not before (AoRules.h says which frames that splits).
//
// The pass runs once per frame, not once per reader: the engine binds one occlusion texture for the whole frame, both light passes and every
// forward draw read it, and a per-draw pass could not be paid for. The frame's completing reader records which of the engine's textures it bound
// (the engine alternates between two from one frame to the next) and the frame's later readers are served from that memory.
//
// The seam costs one relaxed load per draw and one per pixel-shader bind while the key is off, and nothing else: a draw whose shader does not
// declare the name is passed on at that one load, without a state query.

#include "ext/ffxiv/engine/AoRules.h"
#include "ext/ffxiv/engine/EnginePassRules.h" // the anchors word a reader draw is classified by

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceChild;
struct ID3D11DeviceContext;
struct ID3D11PixelShader;
struct ID3D11ShaderResourceView;

namespace ext::ffxiv::engine::ao
{
// One shader creation. Stamps the slots a shader declares the depth, the G-buffer normal and the camera table at on the shader object itself,
// so the frame's first reader reads them with one call and no parse of its own: one byte scan for the occlusion name, then the DXBC reflection
// of a shader that reads that texture. Called whatever the key says, because the game creates its shaders while it loads; nothing is remembered
// anywhere else.
void NoteShader(ID3D11PixelShader* shader, const void* code, size_t size);

// The swap of one reader draw's occlusion slot, in place while the game's draw runs: the game's own view is put back when the guard goes out
// of scope, also when the draw throws. A guard built with nothing bound reports false and restores nothing.
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

// The draw seam's entry: for a draw whose shader reads the engine's occlusion texture, the frame's pass (at the first reader that can serve
// the frame) and the swap of that one draw's slot. A draw this module does not serve binds nothing and restores nothing.
BoundReader SwapReader(ID3D11DeviceContext* context, Anchors anchors);

// The swap itself, in one place and with nothing around it (AoSwap.cpp): binds `replacement` at `slot` and answers the view the game had there,
// with one reference owned by the caller, or null when nothing was bound. SwapReader is the product's only caller; a device test drives it
// directly, together with the guard, to pin the restore.
namespace detail
{
// The stamp this module writes on a shader object: the packed word AoRules.h's PackInputs makes from the slots that shader declares the depth,
// the G-buffer normal and the camera table at. It is the only thing this module remembers about a shader, and it lives on the object, so no table
// of shaders can lose it however many the game creates. A packed word of 0 is not written, and a shader that was never stamped reads 0.
bool StampShader(ID3D11DeviceChild* shader, uint32_t packed);
uint32_t ReadStamp(ID3D11DeviceChild* shader);

ID3D11ShaderResourceView* SwapSlot(ID3D11DeviceContext* context, uint32_t slot, ID3D11ShaderResourceView* replacement);
} // namespace detail

// Whether the draw path should see a draw at all: the key is on and the tuning is not the identity. A paused module keeps the draw path on -
// that is how its next look is reached - so the cost while paused is the key-off cost plus the pause's own frame check.
bool Armed();

// The parameters this session runs with (the keys, clamped).
Params CurrentParams();

// What the card and a capture show.
struct Status
{
    bool key = false;      // the [FFXIV] AmbientOcclusion key is on
    bool identity = false; // the strength or the blend is at zero: today's picture, so nothing runs
    bool paused = false;   // sixty-four frames in a row were left alone, so it paused; it inspects one frame's readers again a while later
    bool armed = false;    // the pass can run now (the key is on, the tuning is not the identity, and the module is not paused)
    uint32_t runs = 0;     // passes run
    uint32_t readers = 0;  // reader draws that bound this module's texture
    uint32_t declined = 0; // readers left alone, and why: the log
    uint32_t servedLastFrame = 0; // reader draws the last finished frame served: a session's own answer to "how many readers are named"
};
Status CurrentStatus();

// The number of `Decline` values, so a caller can carry one count per reason.
inline constexpr uint32_t kDeclineCount = kDeclineReasonCount;

// What one capture records about this module: the switch, the status, one count per decline reason, and the facts of the last pass.
struct Diagnostics
{
    bool key = false;
    bool identity = false;
    bool paused = false;
    bool armed = false;
    uint32_t runs = 0;
    uint32_t readers = 0;
    uint32_t declined = 0;
    uint32_t servedLastFrame = 0;
    uint32_t declines[kDeclineCount] {};
    bool haveRun = false;
    uint32_t sourceWidth = 0, sourceHeight = 0; // the depth and normal allocation
    uint32_t targetWidth = 0, targetHeight = 0; // the occlusion texture the pass replaced
    float window[4] {};                         // the render window, in depth texels
    bool haveGpuMilliseconds = false;
    double gpuMilliseconds = 0.0;
};
Diagnostics CurrentDiagnostics();

// Publishes the card's values to the render thread: the parameters, the frame memory and the feature bit. Called after every menu change and
// at the end of a config load.
void Publish(bool on);

// Releases the device objects while the device is still alive (the engine's teardown path).
void Release();
} // namespace ext::ffxiv::engine::ao
