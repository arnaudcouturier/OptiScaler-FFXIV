#include "pch.h"

#include "ext/gpu/Gpu.h"

#include <wrl/client.h>

// Queue helpers of the C6 base that log (kept out of Gpu.cpp, which the standalone tests link).

namespace ext::gpu
{
bool DrainQueue(ID3D12CommandQueue* queue)
{
    if (queue == nullptr)
        return true;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
        return false;
    // Once removed, this device can no longer execute work referencing the feature.
    if (FAILED(device->GetDeviceRemovedReason()))
        return true;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return false;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr)
        return false;
    auto result = queue->Signal(fence.Get(), 1);
    if (SUCCEEDED(result))
        result = fence->SetEventOnCompletion(1, event);
    const auto waited = SUCCEEDED(result) ? WaitForSingleObject(event, 5000) : WAIT_FAILED;
    CloseHandle(event);
    const bool completed = waited == WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason());
    LOG_INFO("FFXIV upscaler release: queue {:X} drained={}, wait={:X}, result={:X}", (size_t) queue, completed, waited,
             (UINT) result);
    return completed;
}

bool ExecuteOrdered(ID3D12CommandQueue* queue, ID3D12CommandList* list, ID3D12Fence* fence, OrderedFence& state,
                    UINT64& allocatorValue)
{
    queue->ExecuteCommandLists(1, &list);
    // Assign values in submission order, not recording order.
    allocatorValue = ++state.value;
    if (FAILED(queue->Signal(fence, allocatorValue)))
    {
        state.failed = true;
        LOG_ERROR("SC allocator completion signal failed; further SC rendering disabled");
        return false;
    }
    return true;
}
} // namespace ext::gpu
