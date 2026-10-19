#include "pch.h"

#include "ext/Seams.h"

#include "ext/game/Game.h"
#include "ext/ffxiv/lighting/FfxivLighting.h"
#include "ext/ffxiv/lighting/FfxivLightingCapture.h"
#include "ext/ffxiv/companion/Companion.h"
#include "ext/ffxiv/companion/CompanionCallbackMap.h"
#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/jitter/FfxivJitter.h"

#include <atomic>

namespace ext::game
{
void OnD3D11DeviceHooked(ID3D11Device* device)
{
    if (device == nullptr)
        return;

    // Diagnostic for a code-review finding (not a fix): this seam fires on every device that reaches
    // HookToDeviceLocal, matching the oracle's own placement (D3D11_Hooks.cpp, before the "already hooked"
    // guard) -- so whichever device gets here first is the one lighting's Install latches onto. If anything
    // other than the game's own render device (an overlay's capability-probe device, Dalamud, ReShade) wins
    // that race, lighting's native capture binds to the wrong device and the real game device's later call
    // here is a silent no-op. Phase 4: confirm in-game that exactly one "lighting device hooked" line appears
    // per session and that the logged context matches the game's real device.
    if (Current().lighting != nullptr)
    {
        LOG_INFO("FFXIV lighting: device {:#x} reached OnD3D11DeviceHooked (already latched: {})", (size_t) device,
                 ext::ffxiv::lighting::FfxivLightingCapture::installed.load());
    }

    if (Current().hdr != nullptr)
    {
        // Companion has no dedicated Game interface slot (unlike lighting); gate on the same hdr != nullptr
        // idiom Companion.cpp's own menu-card visibility uses. Set once: the six mapped pointers never change
        // across devices, and re-assigning the destination struct on every call is needless, non-atomic churn
        // while the lighting draw hooks may already be reading it.
        static std::atomic<bool> companionCallbacksSet {false};
        if (!companionCallbacksSet.exchange(true))
        {
            // Callbacks must be set before the lighting and companion hooks fire (docs/SEAMS-catalog.md):
            // this runs before both the lighting dispatch below (which installs FfxivLightingCapture's hooks)
            // and ext::ffxiv::companion::Install further below.
            ext::ffxiv::lighting::FfxivLightingCapture::SetCompanionCallbacks(
                ext::ffxiv::companion::MapCaptureCallbacks<ext::ffxiv::lighting::FfxivLightingCapture::CompanionCallbacks>(
                    ext::ffxiv::companion::GetCaptureCallbacks()));
        }
    }

    if (Current().lighting != nullptr)
    {
        ext::ffxiv::lighting::OnDeviceHooked(device);
        LOG_INFO("FFXIV lighting: latched onto device {:#x}, context {:#x}, installed={}", (size_t) device,
                 (size_t) ext::ffxiv::lighting::FfxivLightingCapture::context,
                 ext::ffxiv::lighting::FfxivLightingCapture::installed.load());
    }

    if (Current().hdr != nullptr)
        ext::ffxiv::companion::Install(device);
}

void OnD3D11Unhook()
{
    if (Current().lighting != nullptr)
        ext::ffxiv::lighting::OnUnhook();

    if (Current().hdr != nullptr)
        ext::ffxiv::companion::Unhook();
}
} // namespace ext::game

namespace ext::dx11bridge
{
void OnUpscaleBoundary(ID3D11DeviceContext* context, ID3D11Resource* color,
                       NVSDK_NGX_Parameter* parameters)
{
    if (auto* lighting = game::Current().lighting)
    {
        unsigned int flags = 0;
        const bool haveFlags = parameters != nullptr &&
                               parameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags) ==
                                   NVSDK_NGX_Result_Success;
        lighting->OnUpscaleBoundary(context, color,
                                    !haveFlags || (flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0);
    }

    // FFXIV game-jitter probe/correction ([FFXIV] JitterProbe, JitterCorrection; both off by default): ext/ffxiv/jitter/FfxivJitter.h.
    ext::ffxiv::jitter::OnUpscaleBoundary(parameters);
}

void AfterSharedCopy(ID3D11DeviceContext* context, ID3D11Texture2D* target)
{
    if (game::Current().hdr != nullptr)
        ext::ffxiv::companion::AfterSharedCopy(context, target);

    // Once per presented frame: the character preview feature only counts frames here (the draw thread consumes it).
    if (game::Current().lighting != nullptr)
        ext::ffxiv::chara::OnFrameEnd();
}
} // namespace ext::dx11bridge

#include <framegen/IFGFeature_Dx12.h>
#include <resource_tracking/ResTrack_dx12.h>
#include <upscalers/IFeature.h>

#include <detours/detours.h>

// ext::Access definitions: the only fork code that names private upstream members (docs/SEAMS.md section 3.3).
// When upstream renames a member, this file is where the build breaks.
namespace ext
{
void Access::GetRenderResolution(IFeature& feature, const NVSDK_NGX_Parameter* params, unsigned int& w,
                                  unsigned int& h)
{
    feature.GetRenderResolution(params, &w, &h);
}

ID3D12Device* Access::Dx11On12Device()
{
    return IFeature::_dx11on12Device;
}

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
