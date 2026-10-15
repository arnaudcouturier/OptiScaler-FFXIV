#include "pch.h"

#include "ext/fg/Dx12Access.h"

// C6: ownership of the SC (swapchain-side) command list of the frame-generation features. The lists are submitted in
// order with a monotonic completion fence, the allocator is only reset once that fence passed, and a failed submission
// disables further SC rendering for the object (ext::gpu::OrderedFence::failed).

namespace ext
{
bool Access::FgDx12::WaitForSCAllocator(IFGFeature_Dx12& fg, UINT index)
{
    if (Ext(fg).sc.failed || index >= BUFFER_COUNT)
        return false;
    const auto fenceValue = fg._scAllocatorFenceValues[index];
    if (fenceValue == 0)
        return true;

    if (fg._scFence == nullptr || fg._scFenceEvent == nullptr)
        return false;
    const auto completedValue = fg._scFence->GetCompletedValue();
    if (completedValue == UINT64_MAX)
        return false; // Device removal is not completion.
    if (completedValue >= fenceValue)
        return true;

    auto result = fg._scFence->SetEventOnCompletion(fenceValue, fg._scFenceEvent);
    if (FAILED(result))
    {
        LOG_ERROR("SC allocator fence SetEventOnCompletion failed. slot {}, fence {}, completed {}, result {:X}", index,
                  fenceValue, completedValue, (UINT) result);
        return false;
    }

    const auto waitResult = WaitForSingleObject(fg._scFenceEvent, 5000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LOG_ERROR("SC allocator fence wait failed. slot {}, fence {}, completed {}, waitResult {:X}", index, fenceValue,
                  fg._scFence->GetCompletedValue(), waitResult);
        return false;
    }

    const auto retiredValue = fg._scFence->GetCompletedValue();
    return retiredValue != UINT64_MAX && retiredValue >= fenceValue;
}

bool Access::FgDx12::PrepareSC(IFGFeature_Dx12& fg, UINT index)
{
    auto& state = Ext(fg).sc;
    if (state.failed)
        return false;
    if (index >= BUFFER_COUNT || !fg._scCommandAllocator[index] || !fg._scCommandList[index])
        return false;
    if (fg._scCommandListResetted[index])
        return true;

    if (!WaitForSCAllocator(fg, index))
        return false;

    auto result = fg._scCommandAllocator[index]->Reset();
    if (result != S_OK)
    {
        LOG_ERROR("_scCommandAllocator[{}]->Reset() error: {:X}", index, (UINT) result);
        return false;
    }

    result = fg._scCommandList[index]->Reset(fg._scCommandAllocator[index], nullptr);
    if (result != S_OK)
    {
        LOG_ERROR("_scCommandList[{}]->Reset() error: {:X}", index, (UINT) result);
        return false;
    }

    fg._scCommandListResetted[index] = true;
    return true;
}

bool Access::FgDx12::SubmitSC(IFGFeature_Dx12& fg, UINT index)
{
    auto& state = Ext(fg).sc;
    if (index >= BUFFER_COUNT || state.failed)
        return false;
    if (!fg._scCommandListResetted[index])
        return true;
    if (!fg._gameCommandQueue || !fg._scFence || !fg._scCommandList[index])
        return false;
    const auto closeResult = fg._scCommandList[index]->Close();
    fg._scCommandListResetted[index] = false;
    if (FAILED(closeResult))
    {
        state.failed = true;
        LOG_ERROR("SC command list close failed: {:X}", (UINT) closeResult);
        return false;
    }
    return gpu::ExecuteOrdered(fg._gameCommandQueue, fg._scCommandList[index], fg._scFence, state,
                               fg._scAllocatorFenceValues[index]);
}
} // namespace ext

namespace ext::fg
{
bool PrepareSC(IFGFeature_Dx12* fg, UINT index) { return fg != nullptr && Access::FgDx12::PrepareSC(*fg, index); }
bool SubmitSC(IFGFeature_Dx12* fg, UINT index) { return fg != nullptr && Access::FgDx12::SubmitSC(*fg, index); }
} // namespace ext::fg
