#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/nr/DlssNr.h"
#include <State.h>
#include "ext/nr/amd/AmdBridge.h"
#include "ext/nr/exposure/DlssNr_ExposureScan.h"
#include "ext/nr/vk/DlssNrFeature_Vk.h"
#include "ext/nr/vk/DlssNr_VkExtensions.h"

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

namespace ext::nr
{
namespace
{
// A backend ending in _on12 does its work on a D3D12 device. (The reference build's list left DLSS_on12 out because
// no Vulkan provider hands that variant out yet.)
bool IsOn12(Upscaler backend)
{
    return backend == Upscaler::XeSS_on12 || backend == Upscaler::FSR21_on12 || backend == Upscaler::FSR22_on12 ||
           backend == Upscaler::FFX_on12 || backend == Upscaler::DLSS_on12;
}
} // namespace

VkNrToken::VkNrToken(VkNrToken&& other) noexcept
    : handled(other.handled), rayReconstruction(other.rayReconstruction), bridged(other.bridged),
      nrColor(other.nrColor), originalColor(other.originalColor), params(other.params)
{
    other.nrColor = nullptr;
}

VkNrToken::~VkNrToken()
{
    if (nrColor)
        params->Set(NVSDK_NGX_Parameter_Color, originalColor);
}

VkNrToken VkEvalBefore(VkCommandBuffer cmd, NVSDK_NGX_Parameter* params, VkInstance inst, VkPhysicalDevice pd,
                       VkDevice dev, Upscaler backend)
{
    VkNrToken token;
    token.params = params;
    token.bridged = IsOn12(backend);
    token.rayReconstruction = backend == Upscaler::DLSSD;
    params->Get(NVSDK_NGX_Parameter_Color, &token.originalColor);

    // A bridged backend is run by IFeature_VkwDx12 on the D3D12 command list; running the model here as well would
    // evaluate it twice a frame, the second pass composing over a frame the first already edited.
    if (!token.bridged)
        token.nrColor = DlssNr::EvaluateBeforeUpscaleVk(cmd, params, inst, pd, dev, token.handled,
                                                        token.rayReconstruction);
    if (token.nrColor)
        params->Set(NVSDK_NGX_Parameter_Color, (void*) token.nrColor);

    return token;
}

void VkEvalAfter(VkNrToken& token, bool upscaleOk, VkCommandBuffer cmd, NVSDK_NGX_Parameter* params, VkInstance inst,
                 VkPhysicalDevice pd, VkDevice dev)
{
    // The model exports a complete Vulkan surface and vkCreateDevice already adds the two NVX extensions NGX needs,
    // so there is no D3D12 bridge involved on the native path.
    if (upscaleOk && !token.bridged)
        DlssNr::EvaluateAfterUpscaleVk(cmd, params, inst, pd, dev, token.rayReconstruction, token.handled);
}

VkDeviceExtScope::VkDeviceExtScope(PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd, VkDeviceCreateInfo& info)
    : _info(&info)
{
    // Neural Rendering on Vulkan without a D3D12 bridge, or the reason it cannot be.
    //
    // The model needs two NVIDIA vendor extensions to load its kernels, a game never asks for them, and a device's
    // extension list cannot be changed after creation. This is the only moment it can be arranged. Reported either
    // way: if the answer is no, the log says so here rather than leaving a create failure three layers down to be
    // explained.
    //
    // Only appended when the feature is switched on, and only what the physical device already offers -- asking for
    // an extension a driver does not have makes vkCreateDevice fail and the game not start.
    _on = Cfg().nr.enabled.value_or_default();

    if (!_on)
        return;

    const auto supported = DlssNr::VkExt::SupportedDeviceExtensions(gipa, State::Instance().VulkanInstance, pd);

    _names.assign(info.ppEnabledExtensionNames, info.ppEnabledExtensionNames + info.enabledExtensionCount);

    std::string present, added, missing;

    for (const char* want : DlssNr::VkExt::kDevice)
    {
        const bool already = DlssNr::VkExt::ListHas(info.ppEnabledExtensionNames, info.enabledExtensionCount, want);

        if (already)
            present += std::string(present.empty() ? "" : ", ") + want;
        else if (!DlssNr::VkExt::Contains(supported, want))
            missing += std::string(missing.empty() ? "" : ", ") + want;
        else
        {
            _names.push_back(want);
            added += std::string(added.empty() ? "" : ", ") + want;
        }
    }

    LOG_INFO("DLSS-NR Vulkan: device offers {} extensions. game already enabled: [{}]. added here: "
             "[{}]. NOT AVAILABLE: [{}]",
             supported.size(), present.empty() ? "none" : present, added.empty() ? "none" : added,
             missing.empty() ? "none" : missing);

    if (!missing.empty())
        LOG_WARN("DLSS-NR Vulkan: the native path is not possible on this device -- the model's kernels "
                 "cannot be loaded without the extensions listed as NOT AVAILABLE");

    if (!added.empty())
    {
        info.ppEnabledExtensionNames = _names.data();
        info.enabledExtensionCount = (uint32_t) _names.size();
    }
}

void VkDeviceExtScope::LogResult(VkResult result) const
{
    if (_on)
        LOG_INFO("DLSS-NR Vulkan: vkCreateDevice returned {} with {} extensions requested", (int) result,
                 _info->enabledExtensionCount);
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
