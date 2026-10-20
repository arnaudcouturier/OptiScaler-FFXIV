#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/dx11bridge/ReleaseSequence.h"
#include "ext/game/Game.h"
#include "ext/upscaler/NgxThreadTrace.h"

#include <chrono>

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

bool ShouldSkipReleaseSleep(ReleaseDrain drain)
{
    // shuttingDown is false here by construction: the caller only asks on the non-shutdown path.
    return SkipReleaseSleep(drain, ext::Cfg().ngxRelease.skipReleaseSleep.value_or_default(), false);
}

void SetCurrentFeature(IFeature* feature)
{
    TableGuard lock; // only the pointer store happens under the lock
    State::Instance().currentFeature = feature;
}

void ClearCurrentFeatureIf(IFeature* feature)
{
    TableGuard lock;
    if (State::Instance().currentFeature == feature)
        State::Instance().currentFeature = nullptr;
}

namespace
{
// Production side of ReleaseSequence.h: the real queues. DrainFg also cancels the FG's pending work first, under its mutex.
struct ReleaseOps
{
    IFGFeature_Dx12* fg = nullptr;
    bool fgRequired = false;
    ID3D12CommandQueue* fgQueue = nullptr;

    gpu::QueueDrain DrainFg()
    {
        OwnedLockGuard guard(fg->Mutex, 5);
        ext::Access::CancelPendingUpscalerWork(*fg);
        fgQueue = fg->GetCommandQueue();
        return gpu::DrainQueueEx(fgQueue);
    }

    gpu::QueueDrain DrainUpscale(gpu::QueueDrain fgResult)
    {
        auto upscaleQueue = WithDx12::GetD3D12CommandQueue();
        // One queue serving both roles was waited for once, above.
        if (fgRequired && upscaleQueue == fgQueue)
            return fgResult;
        return gpu::DrainQueueEx(upscaleQueue);
    }
};
} // namespace

ReleaseDrain BeforeReleaseFeature(IFeature_Dx11& feature, unsigned handleId, bool shuttingDown)
{
    // Before the early return: the thread of every release is wanted in the log, whatever the game.
    ext::upscaler::LogReleaseThread(handleId);

    if (!IsolatesNgx() || !feature.IsWithDx12())
        return ReleaseDrain::NotApplicable;

    const auto started = std::chrono::steady_clock::now();
    auto fg = State::Instance().currentFG;

    ReleaseOps ops;
    ops.fg = fg;
    ops.fgRequired = &feature == State::Instance().currentFeature && fg != nullptr &&
                     State::Instance().activeFgInput == FGInput::Upscaler;

    // The XeFG context rebuild below runs after the drain and tears down GPU state the drain does not cover, so a release that
    // will do it is not proven: it keeps the old flow in full (sleep kept, handle never closed).
    const bool xefgRebuild = !shuttingDown && State::Instance().activeFgOutput == FGOutput::XeFG &&
                             ext::Cfg().fg.xefgResetOnUpscalerChange.value_or_default() &&
                             &feature == State::Instance().currentFeature && fg != nullptr &&
                             State::Instance().activeFgInput == FGInput::Upscaler;

    ReleasePlan plan;
    plan.handleId = handleId;
    plan.fgRequired = ops.fgRequired;
    // Closing the handle and waiting for the evaluates in flight is what makes skipping the sleep safe; with the key off
    // (or on shutdown, where nothing is skipped anyway) the flow is the old one.
    plan.gate = !shuttingDown && !xefgRebuild && ext::Cfg().ngxRelease.skipReleaseSleep.value_or_default();

    const auto outcome = PrepareRelease(Registry(), plan, ops);
    if (outcome.threw)
        LOG_ERROR("FFXIV upscaler release: exception while draining the queues");
    const auto result = outcome.result;
    const double waitedMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (result == ReleaseDrain::Failed)
    {
        LOG_WARN("FFXIV upscaler release: handle {} result=failed, the release is refused", handleId);
        return result;
    }

    // D3: the upscaler is rebuilt from here and the caller then sleeps 500 ms. XeFG keeps running straight across that
    // hole: CancelPendingUpscalerWork only switches it off and back on, so the provider's presentation timing history
    // survives a large discontinuity. It can then settle onto the display's fixed refresh grid and stay there, which
    // defeats VRR until some later disruption happens to shake it loose - the reason a quality or NR change flips
    // variable refresh on and off at random.
    //
    // Upstream OptiScaler destroys the FG context on this event instead, so frame generation restarts with a clean
    // history. Do the same, after the queues above have drained: XeFG_Dx12 recreates the context on the next frame when
    // _fgContext is null.
    if (xefgRebuild)
    {
        OwnedLockGuard guard(fg->Mutex, 5);
        LOG_INFO("FFXIV upscaler release: rebuilding FG context so pacing restarts clean");
        State::Instance().fgChanged = true;
        fg->DestroyFGContext();
    }

    LOG_INFO("FFXIV upscaler release: handle {} result={} (fg queue required={}, fg={}, upscale={}, handle {}), waited {:.2f} ms, sleep {}",
             handleId, Name(result), plan.fgRequired, (int) outcome.fg, (int) outcome.upscale, Name(outcome.quiet), waitedMs,
             shuttingDown ? "n/a (shutdown)" : (ShouldSkipReleaseSleep(result) ? "skipped" : "kept (500 ms)"));
    return result;
}
} // namespace ext::dx11bridge
