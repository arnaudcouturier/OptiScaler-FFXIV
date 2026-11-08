#pragma once
// Engine pass overrides: the constant-buffer patch primitive (ext/ffxiv/engine/shaders/CbScale.hlsl).
//
// Two modes, both applied to exactly one draw, and both leaving the game's own buffer bound afterwards:
//
//  - the GPU scale and set. The game's buffer is copied GPU side into an owned constant buffer (`src`), one compute dispatch copies every
//    float4 into an owned raw buffer (`work`), multiplies the float4s a scale-entry table addresses and/or replaces the components a
//    set-entry table masks, and the result is copied into a second owned constant buffer (`dst`) that is bound for that one draw. Three
//    buffers, because D3D11 forbids D3D11_BIND_CONSTANT_BUFFER on a buffer that also carries another bind flag (and a compute shader cannot
//    write a constant buffer at all), so the buffer the dispatch writes can never be the one the draw reads. Two 16 KB copies and one
//    1,024-lane dispatch cost far less than a millisecond.
//
//    The set kind exists for the engine's wetness: the game binds one 32-byte buffer for every material draw of a frame, and the knob replaces
//    two floats of it (the wetness amount and the dryness blend that follows it) for all of them. The caller runs the patch once per frame and
//    binds the cached result for the frame's later draws (EnginePassRules.h's FrameCache).
//
//  - the CPU writer, for a patch whose target is a handful of floats the caller already knows: an owned DYNAMIC buffer per (name, size),
//    written with Map(WRITE_DISCARD) at most once per frame and swapped in per draw. The equality gate is the caller's: it passes the
//    engine's value it read and whether its target differs, and a frame whose target equals the engine's value swaps nothing.
//
// Nothing of the game is written and no game buffer is ever mapped: the GPU mode reads the game's bytes only through CopyResource, and the
// CPU mode never sees them. A failed step returns null and the caller draws the game's own call, unchanged.
//
// Threads: a patch runs on the render thread, while the switch and its keys are published from the menu (the render thread too) and from a
// config load, which can run on the DLL's start-up thread. The public entries therefore serialise on one small mutex, so a release can never
// clear the buffers under a dispatch in flight; the draw path outside this class keeps its own state atomic.

#include "ext/ffxiv/engine/EnginePassRules.h"

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <atomic>
#include <string>
#include <vector>
#include <mutex>

namespace ext::ffxiv::engine
{
// One CPU-written patch target: the caller's values for this frame, and whether they differ from what it read.
struct CpuWriteRequest
{
    const char* name = nullptr;  // identifies the buffer: one owned buffer per (name, byteWidth)
    uint32_t byteWidth = 0;      // a multiple of 16, at most the largest constant buffer the engine binds
    const void* bytes = nullptr; // the values to write; `byteWidth` bytes
    uint64_t frame = 0;          // the caller's frame token: a change writes once, not once per draw
    bool equal = false;          // the caller's target equals the value it last read: nothing to change, so nothing is swapped
};

// The swap of one constant-buffer slot: the game's buffer is read, the replacement is bound, and the game's buffer is put back when the
// guard goes out of scope - also when the draw throws. A guard built with no replacement binds nothing and reports false, which is how a
// caller tells "the game's binding is untouched" from "the patch is in place".
class BoundConstantBuffer
{
  public:
    BoundConstantBuffer(ID3D11DeviceContext* context, uint32_t slot, ID3D11Buffer* replacement);
    ~BoundConstantBuffer();
    BoundConstantBuffer(const BoundConstantBuffer&) = delete;
    BoundConstantBuffer& operator=(const BoundConstantBuffer&) = delete;

    bool Bound() const noexcept { return _bound; }

  private:
    ID3D11DeviceContext* _context = nullptr;
    uint32_t _slot = 0;
    ID3D11Buffer* _original = nullptr;
    bool _bound = false;
};

class CbPatch
{
  public:
    // The GPU scale for one draw. `game` is the buffer the draw bound at `byteWidth` bytes; the entries are checked against that size before
    // anything is dispatched. Returns the buffer to bind for the draw, or null when nothing may be bound (`Failure()` says why).
    ID3D11Buffer* Scale(ID3D11DeviceContext* context, ID3D11Buffer* game, uint32_t byteWidth, const ScaleEntry* entries, uint32_t count);

    // The GPU set for one draw, the same three buffers and the same one dispatch: the components a set entry's mask addresses are replaced with
    // the entry's bits, and every other component is copied exactly as the game wrote it. Returns the buffer to bind for the draw, or null when
    // nothing may be bound (`Failure()` says why).
    ID3D11Buffer* Set(ID3D11DeviceContext* context, ID3D11Buffer* game, uint32_t byteWidth, const SetEntry* entries, uint32_t count);

    // The CPU-written mode: returns the buffer to bind for this frame's draws, or null when the request asks for no swap (its target equals
    // the engine's value) or the buffer could not be created.
    ID3D11Buffer* Write(ID3D11DeviceContext* context, const CpuWriteRequest& request);

    // Forgets every device object. Called while the device is still alive: at the end of the session, on a config reload that switches the
    // key off, and on the engine's teardown path.
    void Release();

    // Why the last run failed, for the log; null while the primitive is healthy.
    const char* Failure() const noexcept { return _failure; }

    // The GPU time of the patch work (both copies and the dispatch), measured with a timestamp pair that is read a run or two later, never
    // waited for. `HasGpuMilliseconds` is false until the first measurement has landed.
    bool HasGpuMilliseconds() const noexcept { return _haveGpuMs; }
    double GpuMilliseconds() const noexcept { return _gpuMs; }

    // How many scales, sets and writes this instance has issued, so a test pins that a frame patches its buffer once, not once per draw.
    uint32_t Scales() const noexcept { return _scales; }
    uint32_t Sets() const noexcept { return _sets; }
    uint32_t Writes() const noexcept { return _writes; }
    uint32_t SizesCached() const noexcept { return uint32_t(_sizes.size()); }

    // Bumped whenever the owned buffers go away (a device change, a release, or the size cache evicting an entry), so a caller that keeps a
    // pointer to a patched buffer can tell that it is no longer this instance's. One relaxed load; never holds the mutex.
    uint32_t Generation() const noexcept { return _generation.load(std::memory_order_relaxed); }

  private:
    // One byte size's three buffers: the copy of the game's buffer, the dispatch's output, and what the draw reads.
    struct SizeCache
    {
        uint32_t byteWidth = 0;
        Microsoft::WRL::ComPtr<ID3D11Buffer> source;
        Microsoft::WRL::ComPtr<ID3D11Buffer> work;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> workView;
        Microsoft::WRL::ComPtr<ID3D11Buffer> destination;
    };

    // One CPU-written target: one DYNAMIC buffer per (name, byteWidth), with the frame it was last written in. `written` separates a buffer
    // that has never been written from one written in frame 0, which the frame token alone cannot tell apart.
    struct CpuSlot
    {
        std::string name;
        uint32_t byteWidth = 0;
        uint64_t frame = 0;
        bool written = false;
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    };

    bool Init(ID3D11Device* device);
    void ReleaseLocked();
    SizeCache* EnsureSize(ID3D11Device* device, uint32_t byteWidth);
    bool UploadParams(ID3D11DeviceContext* context, uint32_t byteWidth, const ScaleEntry* scaleEntries, uint32_t scaleCount,
                      const SetEntry* setEntries, uint32_t setCount);
    // The one dispatch: the copy in, the scale and set tables, the copy out. Both public entries go through it, so the three buffers, the
    // saved compute state and the timestamp pair exist once.
    ID3D11Buffer* Patch(ID3D11DeviceContext* context, ID3D11Buffer* game, uint32_t byteWidth, const ScaleEntry* scaleEntries,
                        uint32_t scaleCount, const SetEntry* setEntries, uint32_t setCount);
    bool BeginGpuTiming(ID3D11DeviceContext* context);
    void EndGpuTiming(ID3D11DeviceContext* context);

    std::mutex _mutex; // serialises the public entries: a release waits for a patch in flight and vice versa
    std::atomic<uint32_t> _generation { 0 }; // bumped under _mutex whenever the owned buffers go away (Generation)
    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> _shader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _params; // the scale table: one DYNAMIC buffer, rewritten per patched draw
    Microsoft::WRL::ComPtr<ID3D11Query> _disjoint, _begin, _end;
    bool _timingPending = false;
    bool _haveGpuMs = false;
    double _gpuMs = 0.0;
    std::vector<SizeCache> _sizes;
    std::vector<CpuSlot> _slots;
    uint32_t _scales = 0;
    uint32_t _sets = 0;
    uint32_t _writes = 0;
    const char* _failure = nullptr;
};
} // namespace ext::ffxiv::engine
