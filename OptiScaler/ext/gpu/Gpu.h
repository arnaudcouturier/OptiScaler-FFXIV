#pragma once
// C6 GPU submission ownership (docs/SEAMS.md section 5, catalog group `gpu`).
//
// Features that keep GPU objects alive across command-list submissions (NR descriptor/model ownership, the HDR
// conversion pool, the AMD PreSR bridge) register one `ext::gpu::Owner` each. Every command list the game or the
// bridges submit then passes through the owners, so an owner can pin what a list references until the GPU is done.
//   * Game submissions: the ExecuteCommandLists hook in QueueHook.cpp (ext/gpu owns the detour, always detached).
//   * Bridge / FG submissions that may run before general resource tracking is installed: ext::gpu::ExecuteCommands.
//   * Reset notifications: the single upstream hkReset (ext::restrack::OnCommandListReset) and ext::gpu::ResetCommands.
// With no owner registered every entry point is the plain D3D12 call (one relaxed load and one branch).

#include "ext/ExtFeatures.h"
#include "ext/gpu/DrainResult.h"
#include "ext/gpu/GpuLifetime.h"
#include "ext/gpu/GpuSubmission.h"

#include <atomic>
#include <d3d12.h>

namespace ext::gpu
{
// Callback table of one owner. Every pointer may be null. `ctx` and the owner's state must stay valid for the whole
// process: slots are never recycled, SetOwnerEnabled(false) only stops new calls (a call already in flight may finish).
// Callbacks run on game threads (recording threads for `reset`, submitting threads for the others) and must not
// block on the thread that is submitting.
struct Owner
{
    void* ctx = nullptr;
    // Capture the recording identity BEFORE the real ExecuteCommandLists. May throw: the table's `quarantine` is then
    // called with the same arguments and the submission proceeds without a token.
    GpuSubmission (*begin)(void* ctx, UINT count, ID3D12CommandList* const* lists) = nullptr;
    // Allocation-free failure path of `begin` (see GpuLifetime::QuarantineSubmission).
    void (*quarantine)(void* ctx, UINT count, ID3D12CommandList* const* lists) = nullptr;
    // A command list was Reset successfully (via the hkReset detour or ext::gpu::ResetCommands).
    void (*reset)(void* ctx, ID3D12CommandList* commands) = nullptr;
    // Index of a list in the batch that must be executed alone (AMD PreSR pending list), or -1.
    int (*isolate)(void* ctx, UINT count, ID3D12CommandList* const* lists) = nullptr;
    // Around each actual ExecuteCommandLists call (after isolation splitting), before `begin` / after the call.
    void (*before)(void* ctx, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) = nullptr;
    void (*after)(void* ctx, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) = nullptr;
};

constexpr int kMaxOwners = 8;

// Returns a handle >= 0 (the owner starts enabled), or -1 when the table is full. Thread-safe.
int RegisterOwner(const Owner& owner);
void SetOwnerEnabled(int handle, bool enabled);

// True while at least one owner is enabled. Hot path: one relaxed load.
inline bool SubmissionOwnersActive() { return ext::Active(ext::F::Gpu); }
namespace detail
{
inline std::atomic<int> g_resetObservers { 0 }; // enabled owners with a `reset` callback
}
// True while at least one enabled owner wants Reset notifications (=> the Reset hook must be installed).
inline bool OwnersWantReset() { return detail::g_resetObservers.load(std::memory_order_relaxed) > 0; }
// True when the ExecuteCommandLists detour is installed (QueueHook.cpp).
bool QueueHookInstalled();

namespace detail
{
using ExecuteFn = void (*)(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
void NotifyReset(ID3D12CommandList* commands);
// Splits around an isolated list, then runs each batch through the owners with `execute` as the real call.
void ExecuteWithIsolation(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn execute);
void ExecuteThroughOwners(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
} // namespace detail

// Replaces `list->Reset(allocator, nullptr)` on bridge/FG-owned lists: Reset, then every owner is told about it.
inline HRESULT ResetCommands(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator)
{
    const HRESULT hr = list->Reset(allocator, nullptr);
    if (SUCCEEDED(hr) && SubmissionOwnersActive())
        detail::NotifyReset(list);
    return hr;
}

// Replaces `queue->ExecuteCommandLists(count, lists)` on bridge/FG-owned lists.
inline void ExecuteCommands(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (!SubmissionOwnersActive())
    {
        queue->ExecuteCommandLists(count, lists);
        return;
    }
    detail::ExecuteThroughOwners(queue, count, lists);
}

// RAII bracket for a bridge-owned recording: Reset through the owners, Submit() closes and executes through them.
// A scope that is destroyed without Submit() closes the list, so an abandoned recording never stays open.
class Scope
{
  public:
    Scope(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator)
        : _list(list), _result(ResetCommands(list, allocator))
    {
        _open = SUCCEEDED(_result);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope()
    {
        if (_open)
            _list->Close();
    }

    HRESULT Result() const { return _result; }
    explicit operator bool() const { return SUCCEEDED(_result); }

    // Close + ExecuteCommands. Returns the Close result; the list is not executed when Close fails.
    HRESULT Submit(ID3D12CommandQueue* queue)
    {
        if (!_open)
            return _result;
        _open = false;
        const HRESULT hr = _list->Close();
        if (SUCCEEDED(hr))
        {
            ID3D12CommandList* lists[] = { _list };
            ExecuteCommands(queue, 1, lists);
        }
        return hr;
    }

  private:
    ID3D12GraphicsCommandList* _list;
    HRESULT _result;
    bool _open = false;
};

// Blocks until `queue` has drained (5 s), tolerating device removal. False = could not prove idle.
// Used on upscaler release so nothing the queue still runs references the feature being destroyed.
bool DrainQueue(ID3D12CommandQueue* queue);
// Same wait, but says what it proved (DrainResult.h): a null queue is Absent and a removed device DeviceLost, neither
// is a drain. DrainQueue above is DrainedOk of this.
QueueDrain DrainQueueEx(ID3D12CommandQueue* queue);

// Ordered submit with a monotonic completion fence (the C6 half of ext::fg::SubmitSC, SEAMS-catalog `fg`).
struct OrderedFence
{
    UINT64 value = 0;     // last value handed out
    bool failed = false;  // a signal failed: the caller stops submitting
};
// Executes one closed list on `queue`, then signals `fence` with ++state.value and stores that value in
// `allocatorValue` (the allocator may be reset once the fence reaches it). On a failed Signal sets state.failed,
// logs and returns false.
bool ExecuteOrdered(ID3D12CommandQueue* queue, ID3D12CommandList* list, ID3D12Fence* fence, OrderedFence& state,
                    UINT64& allocatorValue);
} // namespace ext::gpu
