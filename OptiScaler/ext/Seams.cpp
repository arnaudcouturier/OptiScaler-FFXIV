#include "pch.h"

#include "ext/Seams.h"

#include <framegen/IFGFeature_Dx12.h>
#include <resource_tracking/ResTrack_dx12.h>

#include <detours/detours.h>

// ext::Access definitions: the only fork code that names private upstream members (docs/SEAMS.md section 3.3).
// When upstream renames a member, this file is where the build breaks.
namespace ext
{
void Access::CancelPendingUpscalerWork(IFGFeature_Dx12& fg)
{
    // The input feature can disappear before its final Present (FFXIV exit/scene changes).
    // These lists may reference its shaders/resources. Close without submitting them.
    UINT cancelled = 0;
    for (UINT i = 0; i < BUFFER_COUNT; ++i)
    {
        if (fg._uiCommandListResetted[i])
        {
            fg._uiCommandList[i]->Close();
            fg._uiCommandListResetted[i] = false;
            fg._uiAllocatorFenceValues[i] = 0; // reserved value was never submitted
            ++cancelled;
        }
        if (fg._scCommandListResetted[i])
        {
            fg._scCommandList[i]->Close();
            fg._scCommandListResetted[i] = false;
            ++cancelled;
        }
        std::unique_lock lock(fg._resourceMutex[i]);
        fg._frameResources[i].clear();
        fg._resourceReady[i].clear();
    }
    // XeFG/FSR Deactivate can submit the current UI list. Cancel first so no
    // queued work can reference the upscaler that is about to be destroyed.
    fg.Deactivate();
    fg._lastDispatchedFrame = fg._frameCount;
    fg._waitingNewFrameData = true;
    LOG_INFO("FFXIV upscaler release: cancelled {} pending FG command lists", cancelled);
}

bool Access::ResetHookAttached() { return ResTrack_Dx12::o_Reset != nullptr; }

bool Access::AttachResetHook(void* target)
{
    if (ResTrack_Dx12::o_Reset != nullptr)
        return true;
    if (target == nullptr)
        return false;

    ResTrack_Dx12::o_Reset = reinterpret_cast<decltype(ResTrack_Dx12::o_Reset)>(target);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) ResTrack_Dx12::o_Reset, ResTrack_Dx12::hkReset);
    auto detourResult = DetourTransactionCommit();

    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to hook CommandList Reset: {:X}", detourResult);
        ResTrack_Dx12::o_Reset = nullptr;
        return false;
    }
    return true;
}
} // namespace ext
