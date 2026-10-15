#include "pch.h"

#include "ext/gpu/Gpu.h"
#include "ext/gpu/Restrack.h"

#include <Util.h>
#include "ext/Seams.h"

#include <detours/detours.h>
#include <mutex>
#include <wrl/client.h>

// The fork's ExecuteCommandLists detour (queue vtable[10]) and the ext::restrack entry points.
//
// Owned by ext/gpu (docs/SEAMS.md section 5): installed only while submission owners are active, a pass-through
// trampoline while none is enabled, detached exactly once from ResTrack_Dx12::ReleaseDeviceHooks / ReleaseHooks
// through ext::restrack::ReleaseQueueHook. The Reset side is the one upstream hkReset (ResTrack_Dx12::EnsureResetHook).

namespace
{
using PFN_ExecuteCommandLists = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue* This, UINT NumCommandLists,
                                                         ID3D12CommandList* const* ppCommandLists);

PFN_ExecuteCommandLists o_ExecuteCommandLists = nullptr;
std::atomic<bool> g_queueHookInstalled { false };
std::mutex g_hookMutex;

void CallOriginal(ID3D12CommandQueue* This, UINT count, ID3D12CommandList* const* lists)
{
    o_ExecuteCommandLists(This, count, lists);
}

void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* This, UINT NumCommandLists,
                                             ID3D12CommandList* const* ppCommandLists)
{
    // Fast path: no owner enabled -> the original, nothing else.
    if (!ext::gpu::SubmissionOwnersActive())
    {
        o_ExecuteCommandLists(This, NumCommandLists, ppCommandLists);
        return;
    }
    ext::gpu::detail::ExecuteWithIsolation(This, NumCommandLists, ppCommandLists, CallOriginal);
}

// Caller holds g_hookMutex
void HookToQueueLocked(ID3D12Device* device)
{
    if (g_queueHookInstalled.load(std::memory_order_relaxed))
        return;

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

    auto hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));

    if (hr == S_OK)
    {
        ID3D12CommandQueue* realQueue = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, queue, (IUnknown**) &realQueue))
            realQueue = queue;

        // Get the vtable pointer
        PVOID* pVTable = *(PVOID**) realQueue;

        o_ExecuteCommandLists = (PFN_ExecuteCommandLists) pVTable[10];

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (o_ExecuteCommandLists != nullptr)
            DetourAttach(&(PVOID&) o_ExecuteCommandLists, hkExecuteCommandLists);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook CommandList methods: {:X}", detourResult);
            o_ExecuteCommandLists = nullptr;
        }
        else
            g_queueHookInstalled.store(o_ExecuteCommandLists != nullptr, std::memory_order_release);

        queue->Release();
    }
}
} // namespace

namespace ext::gpu
{
bool QueueHookInstalled() { return g_queueHookInstalled.load(std::memory_order_acquire); }
} // namespace ext::gpu

namespace ext::restrack
{
bool EnsureHooks(ID3D12Device* device)
{
    if (device == nullptr)
        return false;

    bool ok = true;
    std::lock_guard lock(g_hookMutex);

    if (gpu::SubmissionOwnersActive())
    {
        HookToQueueLocked(device);
        ok = g_queueHookInstalled.load(std::memory_order_relaxed);
    }

    // The same hkReset upstream attaches for persistent bindings, through ext::Access; no-op when o_Reset is set
    if (WantsCommandListReset() && !ext::Access::ResetHookAttached())
    {
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* commands = nullptr;
        if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
        {
            if (SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                                    IID_PPV_ARGS(&commands))))
            {
                ID3D12GraphicsCommandList* real = nullptr;
                if (!Util::CheckForRealObject(__FUNCTION__, commands, (IUnknown**) &real))
                    real = commands;

                ok = ext::Access::AttachResetHook((*(PVOID**) real)[10]) && ok;
                commands->Close();
                commands->Release();
            }
            allocator->Release();
        }
    }

    return ok;
}

void OnDeviceHooked(ID3D12Device* device)
{
    // Nothing is hooked on behalf of owners that are not there: upstream behaviour is unchanged until one registers.
    if (gpu::SubmissionOwnersActive() || WantsCommandListReset())
        EnsureHooks(device);
}

void ReleaseQueueHook()
{
    std::lock_guard lock(g_hookMutex);
    if (!g_queueHookInstalled.load(std::memory_order_relaxed))
        return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&) o_ExecuteCommandLists, hkExecuteCommandLists);
    auto detourResult = DetourTransactionCommit();

    if (detourResult != NO_ERROR)
        LOG_ERROR("Failed to unhook the ExecuteCommandLists queue hook: {:X}", detourResult);
    else
    {
        // Detours has pointed o_ExecuteCommandLists back at the original: a submission still inside the hook completes.
        g_queueHookInstalled.store(false, std::memory_order_release);
    }
}
} // namespace ext::restrack
