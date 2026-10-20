#include "pch.h"

#include "ext/gpu/Gpu.h"

#include <wrl/client.h>

// Queue helpers of the C6 base that log (kept out of Gpu.cpp, which the standalone tests link).

namespace ext::gpu
{
QueueDrain DrainQueueEx(ID3D12CommandQueue* queue)
{
    if (queue == nullptr)
        return QueueDrain::Absent;
    FenceOutcome outcome;
    DWORD waited = WAIT_FAILED;
    HRESULT result = E_FAIL;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
        return ClassifyFence(outcome); // setupOk stays false: Failed
    // Once removed, this device can no longer execute work referencing the feature.
    outcome.removedBefore = FAILED(device->GetDeviceRemovedReason());
    if (outcome.removedBefore)
        return ClassifyFence(outcome);
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return ClassifyFence(outcome);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr)
        return ClassifyFence(outcome);
    outcome.setupOk = true;
    result = queue->Signal(fence.Get(), 1);
    if (SUCCEEDED(result))
        result = fence->SetEventOnCompletion(1, event);
    outcome.signalOk = SUCCEEDED(result);
    waited = outcome.signalOk ? WaitForSingleObject(event, 5000) : WAIT_FAILED;
    CloseHandle(event);
    outcome.waitSignaled = waited == WAIT_OBJECT_0;
    outcome.removedAfter = FAILED(device->GetDeviceRemovedReason());
    const auto drain = ClassifyFence(outcome);
    LOG_INFO("FFXIV upscaler release: queue {:X} drained={}, wait={:X}, result={:X}", (size_t) queue,
             drain == QueueDrain::Drained, waited, (UINT) result);
    return drain;
}

bool DrainQueue(ID3D12CommandQueue* queue) { return DrainedOk(DrainQueueEx(queue)); }

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
