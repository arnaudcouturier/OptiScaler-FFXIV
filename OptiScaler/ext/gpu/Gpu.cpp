#include "pch.h"

#include "ext/gpu/Gpu.h"

#include <atomic>
#include <mutex>

// Owner registry and the Reset/Execute paths that run through it. No logging and no hook code in here, so the
// registry can be linked into the standalone WARP tests (tests/ext/gpu).

namespace ext::gpu
{
namespace
{
struct Slot
{
    Owner owner {};
    bool used = false;                // guarded by g_registryMutex
    std::atomic<bool> enabled { false }; // published with release after `owner` is written
};

Slot g_slots[kMaxOwners];
std::mutex g_registryMutex;

// Recompute the aggregate state from the enabled slots. Caller holds g_registryMutex.
void RefreshLocked()
{
    int active = 0;
    int observers = 0;
    for (auto& slot : g_slots)
        if (slot.used && slot.enabled.load(std::memory_order_relaxed))
        {
            ++active;
            if (slot.owner.reset != nullptr)
                ++observers;
        }
    detail::g_resetObservers.store(observers, std::memory_order_relaxed);
    ext::SetActive(ext::F::Gpu, active > 0);
}

template <typename Fn> void ForEachEnabled(Fn&& fn)
{
    for (auto& slot : g_slots)
        if (slot.enabled.load(std::memory_order_acquire))
            fn(slot.owner);
}

void ExecuteBatch(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    queue->ExecuteCommandLists(count, lists);
}
} // namespace

int RegisterOwner(const Owner& owner)
{
    std::lock_guard lock(g_registryMutex);
    for (int i = 0; i < kMaxOwners; ++i)
    {
        if (g_slots[i].used)
            continue;
        g_slots[i].owner = owner;
        g_slots[i].used = true;
        g_slots[i].enabled.store(true, std::memory_order_release);
        RefreshLocked();
        return i;
    }
    return -1;
}

void SetOwnerEnabled(int handle, bool enabled)
{
    if (handle < 0 || handle >= kMaxOwners)
        return;
    std::lock_guard lock(g_registryMutex);
    if (!g_slots[handle].used)
        return;
    g_slots[handle].enabled.store(enabled, std::memory_order_release);
    RefreshLocked();
}

namespace detail
{
void NotifyReset(ID3D12CommandList* commands)
{
    ForEachEnabled(
        [&](const Owner& owner)
        {
            if (owner.reset != nullptr)
                owner.reset(owner.ctx, commands);
        });
}

// One real ExecuteCommandLists call surrounded by every owner's before/begin ... after/complete (the order the
// reference build's hkExecuteCommandLists used: AMD Submitting, NR begin, HDR begin, execute, AMD Submitted, completes).
static void ExecuteBatchThroughOwners(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists,
                                      ExecuteFn execute)
{
    GpuSubmission tokens[kMaxOwners];
    ForEachEnabled(
        [&](const Owner& owner)
        {
            if (owner.before != nullptr)
                owner.before(owner.ctx, queue, count, lists);
        });
    int index = 0;
    ForEachEnabled(
        [&](const Owner& owner)
        {
            const int mine = index++;
            if (owner.begin == nullptr)
                return;
            try
            {
                tokens[mine] = owner.begin(owner.ctx, count, lists);
            }
            catch (...)
            {
                if (owner.quarantine != nullptr)
                    owner.quarantine(owner.ctx, count, lists);
            }
        });
    execute(queue, count, lists);
    ForEachEnabled(
        [&](const Owner& owner)
        {
            if (owner.after != nullptr)
                owner.after(owner.ctx, queue, count, lists);
        });
    for (auto& token : tokens)
        token.CompleteNoThrow(queue);
}

void ExecuteWithIsolation(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn execute)
{
    int pending = -1;
    if (count > 1)
        ForEachEnabled(
            [&](const Owner& owner)
            {
                if (pending < 0 && owner.isolate != nullptr)
                    pending = owner.isolate(owner.ctx, count, lists);
            });
    if (count > 1 && pending >= 0)
    {
        if (pending > 0)
            ExecuteBatchThroughOwners(queue, static_cast<UINT>(pending), lists, execute);
        ExecuteBatchThroughOwners(queue, 1, lists + pending, execute);
        const UINT remaining = count - static_cast<UINT>(pending) - 1;
        if (remaining > 0)
            ExecuteBatchThroughOwners(queue, remaining, lists + pending + 1, execute);
        return;
    }
    ExecuteBatchThroughOwners(queue, count, lists, execute);
}

void ExecuteThroughOwners(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    // The detour already does all of this for a queue it covers; going through it again would double every token.
    if (QueueHookInstalled())
    {
        queue->ExecuteCommandLists(count, lists);
        return;
    }
    ExecuteWithIsolation(queue, count, lists, ExecuteBatch);
}
} // namespace detail
} // namespace ext::gpu
