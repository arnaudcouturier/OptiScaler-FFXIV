#pragma once
// Seams of the DLSS Neural Rendering series (A1-A12, B1-B9, E4), included by ext/Seams.h. Catalog groups `nr`,
// `restrack` (A1/A4 rows) and `dx11bridge` (OnNgxInit/OnNgxShutdownBegin). Bodies: ext/nr/NrSeams.cpp.

#include "ext/ExtFeatures.h"

#include <OptiTypes.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>

#include <memory>
#include <optional>

#include "ext/nr/NgxResourceBinding.h"

namespace ext::nr
{
// What one upscaler evaluate hands to NR (SEAMS-catalog `nr`, EvalBefore/EvalAfter). Four host sites: the native NGX
// D3D12 entry point (passthrough and OptiScaler path), the D3D11-on-D3D12 bridge and the Vulkan-on-D3D12 bridge.
struct NrEvalCtx
{
    enum class Site
    {
        Native,     // NVSDK_NGX_D3D12_EvaluateFeature, no timing queue, no epoch
        Dx11Bridge, // IFeature_Dx11wDx12::Evaluate
        VkBridge    // IFeature_VkwDx12::Evaluate
    };

    Site site = Site::Native;
    ID3D12GraphicsCommandList* cmd = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    ID3D12CommandQueue* queue = nullptr; // queue the list executes on (bridges), else null
    unsigned long long frame = 0;        // submission epoch (bridges), else 0
    NVSDK_NGX_Feature feature = NVSDK_NGX_Feature_SuperSampling;
    unsigned int guideWidth = 0, guideHeight = 0; // C4: extent of the colour before the forced-quality scale

    // Bridge flavour: the feature is the one the upscaler stands for (DLSSD = ray reconstruction).
    static NrEvalCtx Bridge(Site site, ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* params,
                            ID3D12CommandQueue* queue, unsigned long long frame, Upscaler upscaler,
                            unsigned int guideWidth = 0, unsigned int guideHeight = 0)
    {
        return { site,
                 cmd,
                 params,
                 queue,
                 frame,
                 upscaler == Upscaler::DLSSD ? NVSDK_NGX_Feature_RayReconstruction : NVSDK_NGX_Feature_SuperSampling,
                 guideWidth,
                 guideHeight };
    }
};

// Before the upscale (run-before-SR placement, deferred residual SR). Native: SR and RR only.
void EvalBefore(NrEvalCtx& ctx);
// After the upscale; `ok` = the upscale succeeded. Native: SR/RR only (frame generation handles can reach the same
// branch, filtering on the parameter block alone would run the model twice a frame). Bridges also log, once per
// bridge, that the hand-off was reached.
void EvalAfter(NrEvalCtx& ctx, bool ok);

// IFeature_Dx12::Evaluate: owns the Output/Color parameter swap of one evaluate. The constructor points Output at
// `outputTarget` (the first shader pass' input) and, when NR runs before SR on an AMD device, replaces Color with the
// AMD pre-SR edit; the destructor restores both, on every exit (the reference build's RestoreOutput/RestoreColor).
class PreSrScope
{
  public:
    PreSrScope(ID3D12Device* device, Upscaler upscaler, int featureFlags, ID3D12GraphicsCommandList* cmd,
               NVSDK_NGX_Parameter* params, ID3D12Resource* outputTarget, ID3D12CommandQueue* timingQueue);
    ~PreSrScope();
    PreSrScope(const PreSrScope&) = delete;
    PreSrScope& operator=(const PreSrScope&) = delete;

  private:
    std::optional<NgxResourceBinding::Binding<NVSDK_NGX_Parameter>> _output;
    std::optional<NgxResourceBinding::Binding<NVSDK_NGX_Parameter>> _color;
    bool _colorChanged = false;
};
} // namespace ext::nr

namespace ext::dx11bridge
{
// NVNGX_DLSS_Dx11.cpp NVSDK_NGX_D3D11_Init_Ext, before `return Success`: NR resumes after a bridge re-initialisation
// (a no-op unless OnNgxShutdownBegin suspended it).
void OnNgxInit();
// NVNGX_DLSS_Dx11.cpp NVSDK_NGX_D3D11_Shutdown, first statement after `shutdown = true;`: while the D3D11-on-D3D12
// bridge is up, NR stops new work and retires its PreSR generations before the driver's NGX shutdown.
void OnNgxShutdownBegin();
} // namespace ext::dx11bridge
