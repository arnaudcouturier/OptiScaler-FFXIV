#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/nr/DlssNr.h"
#include "ext/nr/amd/AmdBridge.h"
#include "ext/nr/exposure/DlssNr_ExposureScan.h"

#include <with_dx12/with_dx12.h>

// Bodies of the NR seams (docs/SEAMS-catalog.md groups nr, restrack, dx11bridge). Everything here is the glue the
// upstream host files used to carry inline; the logic it calls lives in the rest of ext/nr.

namespace ext::nr
{
namespace
{
bool IsUpscalerFeature(NVSDK_NGX_Feature feature)
{
    return feature == NVSDK_NGX_Feature_SuperSampling || feature == NVSDK_NGX_Feature_RayReconstruction;
}
} // namespace

void EvalBefore(NrEvalCtx& ctx)
{
    // The native entry point sees every NGX feature; only the upscaler evaluates are NR's.
    if (ctx.site == NrEvalCtx::Site::Native && !IsUpscalerFeature(ctx.feature))
        return;

    // The NR feature bit gates the per-resource hot paths (exposure scan). It follows the user's switch at runtime.
    if (!Active(F::Nr) && Cfg().nr.enabled.value_or_default())
        SetActive(F::Nr, true);

    DlssNr::EvaluateBeforeUpscale(ctx.cmd, ctx.params, ctx.queue, ctx.frame,
                                  ctx.feature == NVSDK_NGX_Feature_RayReconstruction, ctx.guideWidth,
                                  ctx.guideHeight);
}

void EvalAfter(NrEvalCtx& ctx, bool ok)
{
    const bool rayReconstruction = ctx.feature == NVSDK_NGX_Feature_RayReconstruction;

    if (ctx.site == NrEvalCtx::Site::Native)
    {
        // Neural Rendering runs over what the upscaler just wrote, on the same list, so frame generation
        // interpolates from enhanced frames and the model still costs one run per rendered frame. The feature
        // check is the point: frame generation is handed depth and motion vectors too, and its handle can reach
        // here, so filtering on the parameter block alone would run the model twice a frame.
        if (ok && IsUpscalerFeature(ctx.feature))
            DlssNr::EvaluateAfterUpscale(ctx.cmd, ctx.params, nullptr, rayReconstruction);
        return;
    }

    // Bridge: at this moment the block carries the D3D12 copies of every input, the list is still recording, and the
    // model's edit lands on the D3D12 output before it is copied back to the game's texture. This one call is what
    // makes the pass work in D3D11 and Vulkan games, whatever upscaler carried it here.
    const bool enabled = Cfg().nr.enabled.value_or_default();
    const bool dx11 = ctx.site == NrEvalCtx::Site::Dx11Bridge;
    static bool reportedDx11 = false;
    static bool reportedVk = false;
    bool& reported = dx11 ? reportedDx11 : reportedVk;

    if (!reported)
    {
        reported = true;
        LOG_INFO("DLSS-NR: the {} bridge reached the hand-off (upscale ok: {}, enabled: {})",
                 dx11 ? "D3D11" : "Vulkan", ok, enabled);
    }

    if (ok && enabled)
        DlssNr::EvaluateAfterUpscale(ctx.cmd, ctx.params, ctx.queue, rayReconstruction, ctx.frame, ctx.guideWidth,
                                     ctx.guideHeight);
}

PreSrScope::PreSrScope(ID3D12Device* device, Upscaler upscaler, int featureFlags, ID3D12GraphicsCommandList* cmd,
                       NVSDK_NGX_Parameter* params, ID3D12Resource* outputTarget, ID3D12CommandQueue* timingQueue)
{
    // Upscaler will write to the first active shader, or just output.
    _output.emplace(params, NVSDK_NGX_Parameter_Output);
    _output->Set(outputTarget);

    const NrConfig& nr = Cfg().nr;

    if (nr.enabled.value_or_default() && nr.runBeforeSr.value_or_default() && upscaler != Upscaler::DLSSD &&
        DlssNr::AmdBridge::CanUse(device) && DlssNr::AmdBridge::EnsureHooks(device))
    {
        _color.emplace(params, NVSDK_NGX_Parameter_Color);

        if (auto* edited = DlssNr::AmdBridge::Prepare(cmd, params, timingQueue, featureFlags, timingQueue != nullptr))
        {
            if (nr.applyModel.value_or_default())
            {
                _colorChanged = true;
                _color->Set(edited);
            }
        }
    }
}

PreSrScope::~PreSrScope()
{
    if (_colorChanged)
        _color->Restore();
    _output->Restore();
}
} // namespace ext::nr

namespace ext::dx11bridge
{
void OnNgxInit() { DlssNr::ResumeAfterBridgeInit(); }

void OnNgxShutdownBegin()
{
    // The oracle tested the FFXIV exe name here; the bridge-active test is what the call is about.
    if (WithDx12::IsInited())
        DlssNr::SuspendForBridgeShutdown();
}
} // namespace ext::dx11bridge

namespace ext::restrack
{
void NoteUavSlow(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc)
{
    DlssNr::ExposureScan::NoteUav(resource, desc);
}

void NoteResourceCreatedSlow(const D3D12_RESOURCE_DESC* desc, REFIID riid, void* resource)
{
    // The reference build cast the out pointer blindly; only the resource interfaces are known to be one.
    if (resource == nullptr || desc == nullptr)
        return;

    if (riid != __uuidof(ID3D12Resource) && riid != __uuidof(ID3D12Resource1) && riid != __uuidof(ID3D12Resource2))
        return;

    DlssNr::ExposureScan::NoteResource(desc, static_cast<ID3D12Resource*>(resource));
}

// The scan needs the game's resources, and a game without the UE atomics spoof had none of these hooks installed.
bool WantsResourceCreateHooks() { return ext::Cfg().nr.enabled.value_or_default(); }

void OnFeatureRelease() { DlssNr::ExposureScan::ReleaseTrackedResources(); }
} // namespace ext::restrack
