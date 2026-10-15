#pragma once
// Seams of the DLSS Neural Rendering series (A1-A12, B1-B9, E4), included by ext/Seams.h. Catalog groups `nr`,
// `restrack` (A1/A4 rows) and `dx11bridge` (OnNgxInit/OnNgxShutdownBegin). Bodies: ext/nr/NrSeams.cpp.

#include "ext/ExtFeatures.h"

#include <OptiTypes.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <vulkan/vulkan.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ext/nr/NgxResourceBinding.h"

struct NVSDK_NGX_Resource_VK;

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

// Native Vulkan evaluate (B9). The token carries what the after-call and the Color restore need; `bridged` backends
// (`*_on12`) are evaluated by IFeature_VkwDx12 on D3D12, so NR does not run on the Vulkan side for them.
struct VkNrToken
{
    bool handled = false;
    bool rayReconstruction = false;
    bool bridged = false;
    NVSDK_NGX_Resource_VK* nrColor = nullptr; // the NR-edited colour that replaced Parameter_Color, if any
    void* originalColor = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;

    VkNrToken() = default;
    VkNrToken(const VkNrToken&) = delete;
    VkNrToken& operator=(const VkNrToken&) = delete;
    VkNrToken(VkNrToken&& other) noexcept;
    ~VkNrToken(); // puts Parameter_Color back when NR replaced it
};

// Before `deviceContext->Evaluate`. Asked of the live feature's backend rather than of the config: the config is what
// was requested, the feature is what is actually running.
VkNrToken VkEvalBefore(VkCommandBuffer cmd, NVSDK_NGX_Parameter* params, VkInstance inst, VkPhysicalDevice pd,
                       VkDevice dev, Upscaler backend);
// After the evaluate. Nothing for a failed upscale, a bridged backend or when the model already ran before the upscale.
void VkEvalAfter(VkNrToken& token, bool upscaleOk, VkCommandBuffer cmd, NVSDK_NGX_Parameter* params, VkInstance inst,
                 VkPhysicalDevice pd, VkDevice dev);

// vkCreateDevice: appends the NVX extensions the model needs when NR is on (B9) and keeps the merged name list alive
// for the create call; LogResult reports the outcome. A no-op scope while NR is off.
class VkDeviceExtScope
{
  public:
    VkDeviceExtScope(PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd, VkDeviceCreateInfo& info);
    void LogResult(VkResult result) const;

  private:
    bool _on = false;
    const VkDeviceCreateInfo* _info = nullptr;
    std::vector<const char*> _names;
};

// DLSSFeature::ProcessInitParams: the six Hint_Render_Preset_* equal to NV_PRESET_LATEST (the legacy UI "Latest"
// sentinel, not an NGX preset) are mapped to the runtime default. SR only.
void NormalizeDlssPresets(NVSDK_NGX_Parameter* params);

// NvApiHooks.cpp hkNvAPI_QueryInterface, final return: records the CUDA-kernel entry points NR's hybrid path
// intercepts and hands back its wrappers; every other id passes through.
void* WrapNvapi(unsigned int id, void* fn);

// Util::FindFilePath: residual-only FG (NR + DeferredDLSS + ResidualFG) needs the official FG runtime even with
// full-game FG off.
bool NeedsDlssgRuntime(const std::filesystem::path& fileName);

// Streamline_Proxy.h LoadStreamline: Streamline can initialise NGX before the upscaler does, so the NR runtime's folder
// goes into the first search list (a later NGX initialisation cannot repair it).
void AppendNgxSearchPaths(const std::filesystem::path& exePath, std::vector<std::wstring>& paths);
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

namespace ext::restrack
{
// ResTrack_dx12.cpp hkCreateUnorderedAccessView, before the call (A4 exposure scan). One relaxed load while NR was
// never enabled this session.
void NoteUavSlow(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc);
inline void NoteUav(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc)
{
    if (Active(F::Nr))
        NoteUavSlow(resource, desc);
}

// D3D12_Hooks.cpp tail of hkCreateCommittedResource / hkCreatePlacedResource (A4). Every game thread, bursts of
// thousands: lock-free on the miss path. `resource` is the call's ppvResource; only a resource IID is looked at.
void NoteResourceCreatedSlow(const D3D12_RESOURCE_DESC* desc, REFIID riid, void* resource);
inline void NoteResourceCreated(HRESULT hr, const D3D12_RESOURCE_DESC* desc, REFIID riid, void** resource)
{
    if (Active(F::Nr) && SUCCEEDED(hr) && resource != nullptr)
        NoteResourceCreatedSlow(desc, riid, *resource);
}

// D3D12_Hooks.cpp HookToDevice: NR wants the resource-creation hooks even without the UE atomics spoof.
bool WantsResourceCreateHooks();

// NVNGX_DLSS_Dx12.cpp NVSDK_NGX_D3D12_ReleaseFeature, before any resource is freed: drops the exposure scan's pins
// (the Cyberpunk device-removal fix).
void OnFeatureRelease();
} // namespace ext::restrack
