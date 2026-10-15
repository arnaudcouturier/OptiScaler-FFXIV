#include "pch.h"

#include "ext/Seams.h"
#include "ext/game/Game.h"

#include <Config.h>
#include <OwnedMutex.h>
#include <State.h>
#include <framegen/IFGFeature_Dx12.h>
#include <proxies/NVNGX_Proxy.h>
#include <upscalers/IFeature_Dx11.h>
#include <with_dx12/with_dx12.h>

// C6: NGX shutdown / feature release on the D3D11 entry points (SEAMS-catalog group dx11bridge). Everything is gated
// on ext::game INgxLifecycle::IsolatesShutdown, which only a game module that shares NGX with a live DX12 Streamline
// instance provides; for every other game each function returns the upstream-default answer.

namespace
{
bool IsolatesNgx()
{
    auto* ngx = ext::game::Current().ngx;
    return ngx != nullptr && ngx->IsolatesShutdown();
}
} // namespace

namespace ext::dx11bridge
{
bool ShutdownNgx(ID3D11Device* device, NVSDK_NGX_Result& out)
{
    if (!IsolatesNgx() || !WithDx12::IsInited())
        return false;

    // Legacy Shutdown destroys NGX on ALL devices, including Streamline's live DX12
    // FG instance. Keep that instance alive for its remaining presents and cleanup.
    if (device != nullptr && NVNGXProxy::D3D11_Shutdown1() != nullptr)
    {
        unsigned int remainingInstances = 0;
        LOG_INFO("FFXIV device-specific NGX shutdown: calling driver with DX11 device {:X}", (size_t) device);
        out = NVNGXProxy::D3D11_Shutdown1()(device, &remainingInstances);
        LOG_INFO("FFXIV device-specific NGX shutdown: DX11 device {:X}, result {:X}, remaining={}", (size_t) device,
                 (UINT) out, remainingInstances);
    }
    else
        LOG_ERROR("FFXIV NGX shutdown deferred: device-specific shutdown unavailable; DX12 still alive");

    return true;
}

bool RedirectShutdown1(ID3D11Device* inDevice, ID3D11Device*& recordedDevice)
{
    // A null Shutdown1 device has the same global effect as legacy Shutdown.
    // Route the game through the guarded path using its recorded DX11 device.
    if (!IsolatesNgx() || !WithDx12::IsInited())
        return false;

    if (inDevice != nullptr)
        recordedDevice = inDevice;
    return true;
}

bool BeforeReleaseFeature(IFeature_Dx11& feature, [[maybe_unused]] bool shuttingDown)
{
    if (!IsolatesNgx() || !feature.IsWithDx12())
        return true;

    auto fg = State::Instance().currentFG;
    ID3D12CommandQueue* fgQueue = nullptr;
    if (&feature == State::Instance().currentFeature && fg != nullptr &&
        State::Instance().activeFgInput == FGInput::Upscaler)
    {
        OwnedLockGuard guard(fg->Mutex, 5);
        ext::Access::CancelPendingUpscalerWork(*fg);
        fgQueue = fg->GetCommandQueue();
        if (!gpu::DrainQueue(fgQueue))
            return false;
    }
    auto upscaleQueue = WithDx12::GetD3D12CommandQueue();
    if (upscaleQueue != fgQueue && !gpu::DrainQueue(upscaleQueue))
        return false;

    // ext: D3 (series S5) adds the XeFG context rebuild here: with `!shuttingDown`, activeFgOutput == XeFG and
    // FGXeFGResetOnUpscalerChange, under fg->Mutex: log "FFXIV upscaler release: rebuilding FG context so pacing
    // restarts clean", State::fgChanged = true, fg->DestroyFGContext(). It needs the D3 config key, so it is not here.
    return true;
}
} // namespace ext::dx11bridge
