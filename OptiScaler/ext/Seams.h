#pragma once
// The only fork header upstream files may include (docs/SEAMS.md section 1).
// Every seam is a free function in `ext::<group>`; the catalog is docs/SEAMS-catalog.md.
// Hot-path seams are inline wrappers testing a feature bit, see ExtFeatures.h.

#include "ext/ExtFeatures.h"
#include "ext/bridge/BridgeSeams.h"
#include "ext/core/CustomTabPins.h"
#include "ext/core/HooksBypass.h"
#include "ext/core/MenuRegistry.h"
#include "ext/dx11bridge/BridgeExt.h"
#include "ext/dx11bridge/BridgeState.h"
#include "ext/dx11bridge/NgxFeatureTable.h"
#include "ext/dx11bridge/ReleaseDrain.h"
#include "ext/fg/Dx12Ext.h"
#include "ext/fg/FgSeams.h"
#include "ext/fgrec/FgRecSeams.h"
#include "ext/hdr/HdrSeams.h"
#include "ext/gpu/Gpu.h"
#include "ext/gpu/Restrack.h"
#include "ext/hudless/HudlessSeams.h"
#include "ext/menu/MenuStyleSeams.h"
#include "ext/nr/NrSeams.h"
#include "ext/present/PresentSeams.h"
#include "ext/reshade/ReShadeSeams.h"
#include "ext/status/StatusFile.h"
#include "ext/upscaler/UpscalerSeams.h"

#include <cstddef>

// CSimpleIniA is a typedef of a template, it cannot be forward declared
#include <SimpleIni.h>

// Upstream classes named by the seams below (the full types come with the host file's own includes)
class IFeature;
class IFeature_Dx11;
class IFeature_Dx11wDx12;
class IFGFeature_Dx12;
class Dx11wDx12SC;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11Texture2D;
struct NVSDK_NGX_Parameter;
// ID3D12Device/ID3D12GraphicsCommandList/ID3D12Resource are already complete by this point (NrSeams.h above pulls
// in d3d12.h), so PrepareUpscalerColor's signature below can simply name them.

namespace ext::fg::outswitch
{
struct HookSlot; // ext/fg/outswitch/HookRebind.h
}

namespace ext
{
// Friend of the upstream classes whose private state the fork has to reach (`friend struct ext::Access;`).
// It is the only fork code that names private upstream members. Accessors are added next to the seam that needs
// them; the definitions live in Seams.cpp. (The menu seam needs none: it is a template, see ext/core/MenuRegistry.h.)
struct Access
{
    // framegen/IFGFeature_Dx12.h (friend line). C6: close the unsubmitted UI/SC lists, drop the frame resources and
    // Deactivate, so nothing queued references an upscaler that is about to be destroyed (ext::dx11bridge::
    // BeforeReleaseFeature). Not a seam: ext-internal.
    static void CancelPendingUpscalerWork(IFGFeature_Dx12& fg);

    // hooks/FG_Hooks.h (friend line). The live output switch's rebind needs the ten (original, body) pairs upstream's
    // HookFGSwapchain attaches; `o_*` and `hk*` are private statics, so this is the only way to name them. The definition
    // is ext/fg/outswitch/HookAccess.h (it needs the complete class), and the caller treats a returned 0 as "no set".
    static size_t FgHookSlots(ext::fg::outswitch::HookSlot* out, size_t maxOut);
    // hooks/FG_Hooks.h (same friend line). True while the present slot holds a Detours trampoline: the rehook's proof that
    // the class the switch created really is the hooked one. Defined in ext/fg/outswitch/HookAccess.h.
    static bool FgPresentHooked();

    // hooks/DxgiFactory_Hooks.h and hooks/DxgiFactory_WrappedCalls.h (their friend lines). The plain chain a live output
    // switch creates goes through the real factory entry with the FG swapchain hook and the parent wrapping off, exactly as
    // boot's fallback does. `wrappedFactory` is the WrappedIDXGIFactory7 the captured factory answered as, or null; the chain
    // comes back with its creation reference. Defined in ext/fg/outswitch/FactoryGuards.cpp.
    static long CreatePlainSwapChain(void* factory, void* wrappedFactory, void* device, void* desc, void** chain);
    // The same two friend lines. A provider chain the switch creates goes through upstream's FGHooks::CreateSwapChain with
    // the factory's FG swapchain creation skipped, as boot's own call is: the library creates its real chain through the
    // hooked factory, and that inner create must not run the provider's create again. Same file.
    static long CreateProviderSwapChain(void* factory, void* device, void* desc, void** chain);

    // resource_tracking/ResTrack_dx12.h (friend line). C6, ext-internal (ext::restrack::EnsureHooks): attach upstream's
    // own hkReset to ResTrack_Dx12::o_Reset. AttachResetHook is a no-op returning true when o_Reset is already set;
    // `target` is slot 10 of a real ID3D12GraphicsCommandList vtable. Failure leaves o_Reset null.
    static bool ResetHookAttached();
    static bool AttachResetHook(void* target);

    // upscalers/IFeature.h (friend line). C2/C3: the free functions ext::upscaler::CorrectRenderSubrect /
    // ResolveRenderGeometry need to call the protected GetRenderResolution again, on an arbitrary feature (the
    // D3D11/Vulkan bridges' inner dx12Feature, not necessarily `this`). Not a seam: ext-internal, no declaration is
    // added to IFeature.h beyond the friend line.
    static void GetRenderResolution(IFeature& feature, const NVSDK_NGX_Parameter* params, unsigned int& w,
                                     unsigned int& h);

    // upscalers/IFeature.h (friend line, already granted). C4: ext::dx11bridge::PrepareUpscalerColor needs the
    // D3D11-on-D3D12 device (static across every feature, upstream's own _dx11on12Device) to create/update its
    // lazily-created forced-quality colour scaler. Not a seam: ext-internal.
    static ID3D12Device* Dx11On12Device();

    // framegen/IFGFeature_Dx12.h and the derived FG classes (friend lines). The FG fork code names their protected
    // state through this nested class only; it is defined in ext/fg/Dx12Access.h, which upstream files never include.
    struct FgDx12;

    // with_dx12/dx11_with_dx12_sc.h (friend line). The fork's D3D11-bridge code names its private state through this
    // nested class only; defined in ext/dx11bridge/BridgeAccess.h, which upstream files never include.
    struct Bridge;

    // with_dx12/dx11_with_dx12_sc.h and framegen/IFGFeature_Dx12.h (friend lines). The HUD-less engine (S10) names the
    // bridge's interop state and the frame generator's UI fence through this nested class only; defined in
    // ext/hudless/HudlessAccess.h, which upstream files never include.
    struct Hudless;
};

// ini load/save hooks in Config.cpp (state: ext/core/ExtConfig)
namespace config
{
void Load(CSimpleIniA& ini);    // end of Config::Reload, after upstream read its keys
void Save(CSimpleIniA& ini);    // Config::SaveIni, before ini.SaveFile
void SaveXeFG(CSimpleIniA& ini); // Config::SaveXeFG, before ini.SaveFile
} // namespace config

// menu_common.cpp (state: ext/core/MenuRegistry). ext::menu::MergeBoxes(base, count) is a template declared in
// ext/core/MenuRegistry.h: `base` is upstream's static MenuBox array, the result is `base` itself while no fork card is
// registered, otherwise a merged array (count is updated).

// core services (state: ext/core)
namespace core
{
void Banner(); // once, right after upstream logs its version line
} // namespace core

// ext::gpu (command-list Reset/Execute wrappers, owners, Scope) is declared in ext/gpu/Gpu.h and ext::restrack
// (OnCommandListReset, WantsCommandListReset, OnDeviceHooked, ReleaseQueueHook) in ext/gpu/Restrack.h; both are
// included above.

// NGX shutdown / feature release on the D3D11 entry points (C6). State: ext/dx11bridge/NgxLifecycle.cpp
namespace dx11bridge
{
// C5/B6: the bridge passes its original input color and feature flags; dispatch stays inside the fork seam. `parameters` is the
// bridge's own non-const parameter block: the lighting scan only reads it, the FFXIV jitter correction (default off) writes
// Jitter.Offset.X/Y into it (ext/ffxiv/jitter/FfxivJitter.h).
void OnUpscaleBoundary(ID3D11DeviceContext* context, ID3D11Resource* color,
                       NVSDK_NGX_Parameter* parameters);
// _CopyDx11BackBufferToShared, right after the backbuffer copy (before the source texture is released): C7
// per-frame callback (GPU timer copy-in close, character preview frame count). `target` is the shared
// copy's destination texture, the same image the copy just wrote.
void AfterSharedCopy(ID3D11DeviceContext* context, ID3D11Texture2D* target);
// NVSDK_NGX_D3D11_Shutdown, replaces the legacy proxy Shutdown call. True = handled (`out` holds the result): the
// game isolates NGX shutdown (ext::game INgxLifecycle) and the bridge is up, so only the device-specific Shutdown1 runs
// and Streamline's live DX12 NGX instance survives. False = upstream calls the legacy Shutdown.
bool ShutdownNgx(ID3D11Device* device, NVSDK_NGX_Result& out);
// NVSDK_NGX_D3D11_Shutdown1, first statement. True = the caller must `return NVSDK_NGX_D3D11_Shutdown();`.
// `recordedDevice` is the translation unit's D3D11Device, set to `inDevice` when handled.
bool RedirectShutdown1(ID3D11Device* inDevice, ID3D11Device*& recordedDevice);
// NVSDK_NGX_D3D11_ReleaseFeature, before the feature is reset. Failed -> return FAIL_PlatformError. Drained = the GPU
// work that could reference the feature is proven finished, so the caller may skip its 500 ms sleep when
// SkipReleaseSleep(result) says so; NotApplicable = no proof, nothing changes (ext/dx11bridge/ReleaseDrain.h).
ReleaseDrain BeforeReleaseFeature(IFeature_Dx11& feature, unsigned handleId, bool shuttingDown);
// NVSDK_NGX_D3D11_ReleaseFeature, in the condition of the 500 ms sleep before the reset (non-shutdown path only): true
// = skip it. Only for ReleaseDrain::Drained with [NgxRelease] SkipReleaseSleep on (default); the sleep after the reset
// (Dx11DelayedInit) is not part of this.
bool ShouldSkipReleaseSleep(ReleaseDrain drain);
// NVSDK_NGX_D3D11_CreateFeature / EvaluateFeature: `State::currentFeature = feature` under the narrow table lock
// (ext/dx11bridge/NgxFeatureTable.h). ClearCurrentFeatureIf (ReleaseFeature): clears it only when it still is `feature`.
void SetCurrentFeature(IFeature* feature);
void ClearCurrentFeatureIf(IFeature* feature);

// IFeature_Dx11wDx12.cpp Evaluate, before binding NVSDK_NGX_Parameter_Color (replaces the unconditional
// dx11Color.Dx12Resource bind). C4's forced-quality colour scale: Lanczos3-downscales the display-sized colour
// input to the live render size when game::Current().quality->Live() is false, game::Current().geometry says the
// colour is really allocated at display size (IRenderGeometry::ColorIsDisplayAllocation, which also carries the
// RenderSizeFromWidthHeight gate -- the oracle's single !Available() gate covers both the geometry correction and
// this scale, so this call and C2/C3's CorrectRenderSubrect stay coupled to the same config flag), and
// GameQuirk::ScaleDisplayColorForForcedQuality is set. Every other combination (no FFXIV match, native quality
// live, quirk off, flag off, render already equals the allocation, or the game's subrect doesn't describe the full
// allocation) is a no-op returning `color` unchanged. `guideSrcWidth`/`guideSrcHeight` are zeroed by this call and
// set to the ORIGINAL (unscaled) extent only on the scaled path, for the caller's NrEvalCtx guide-extent fields.
// nullptr = the scale itself failed; the caller must bail the evaluate (same severity as "Can't process Dx11
// textures!").
ID3D12Resource* PrepareUpscalerColor(IFeature_Dx11wDx12& feature, ID3D12GraphicsCommandList* cmd,
                                     NVSDK_NGX_Parameter* params, ID3D12Resource* color, unsigned int& guideSrcWidth,
                                     unsigned int& guideSrcHeight);
} // namespace dx11bridge

namespace game
{
void OnD3D11DeviceHooked(ID3D11Device* device);
void OnD3D11Unhook();
} // namespace game

// ext::bridge ([Bridge] keys, ext/bridge/BridgeSeams.h) is included above: `FlushAfterSignal()` is one relaxed atomic
// load and is called by the D3D11 -> D3D12 hand-over (with_dx12/dx11_with_dx12.cpp).

// ext::upscaler (EffectivePerfQuality, OnOptimalSettings, LastQualityAnswer, ApplyForcedQuality) is declared in
// ext/upscaler/UpscalerSeams.h, included above.

// Groups of later series. Declarations are added here together with the SEAMS-catalog.md row.
namespace nr {}
namespace streamline {}
namespace hdr {}
namespace hudless {}
namespace game {}
} // namespace ext
