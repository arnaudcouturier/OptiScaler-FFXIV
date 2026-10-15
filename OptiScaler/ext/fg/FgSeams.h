#pragma once
// Seams of the frame-generation group (SEAMS-catalog groups `fg` and `streamline`), included by ext/Seams.h.
// Per-object state: ext::fg::Dx12Ext (IFGFeature_Dx12::_ext), ext/fg/Dx12Ext.h. Implementations: ext/fg/*.cpp.

#include <d3d12.h>
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

class IFGFeature_Dx12;
struct Dx12Resource;

// ---------------------------------------------------------------------------------------------------------------
// Streamline (D1, D4). Implementation: ext/fg/streamline/*.cpp. Hosts: proxies/Streamline_Proxy.h (LoadStreamline),
// hooks/Streamline_Hooks.cpp (hkslDLSSGSetOptions, hkslDLSSGGetState), hooks/LibraryLoad_Hooks.cpp (LoadLibraryCheckW).
namespace ext::streamline
{
// D4: first statement of StreamlineProxy::LoadStreamline: Ampere MFG companion setup (guarded inside, runs on every call;
// must precede the Streamline module load and the `_dll != nullptr` early-out).
void OnLoadStreamline();
// D1: apply the nvngx_dlssg.dll patches when the provider is loaded (cheap flag check when the unlock is off).
void MfgUnlockTick();
// D1: the populate-once condition of the cached Streamline frame-count ceiling (State::dlssgMfgMax): true while it is
// unset or an unlock raised the ceiling after it was cached, and the answer is not provisional any more.
bool MfgCeilingStale(const std::optional<int>& cached);
// D1: raises `numFramesToGenerateMax` to the unlocked ceiling when a Streamline wrapper reports a lower one. Returns the
// (possibly raised) value. Must stay inside the struct-version-2+ branch of hkslDLSSGGetState.
uint32_t RaiseMfgCeiling(uint32_t& numFramesToGenerateMax);
// D1: LoadLibraryCheckW, nvngx_dlssg.dll branch. True = handled: `module` is what LoadLibraryCheckW returns (nullptr when
// the load failed). The provider is loaded through the loader-level entry and patched before NGX caches capabilities.
bool OnDlssgModuleLoad(const std::wstring& normalizedPath, LPCWSTR fullPath, HMODULE& module);
// D5: hkslDLSSGSetOptions, Streamline older than 2.7.1 (no multi frame generation). Once-only warning when the user set an
// interpolation count override that therefore cannot apply.
void WarnMfgOverrideIgnored(uint32_t major, uint32_t minor, uint32_t patch);
} // namespace ext::streamline

namespace ext::fg
{
// --- C6 (Dx12Sc.cpp)
// DLSSG_Dx12, XeFG_Dx12, FSRFG_Dx12 `Present`: close and submit the SC list in order (monotonic completion fence).
// False = the submission failed (further SC rendering is off): `return false` from Present.
bool SubmitSC(IFGFeature_Dx12* fg, UINT index);
// IFGFeature_Dx12::GetSCCommandList, after the stale lists were discarded: leaves the list at `index` open and ready
// (failed-submission and null guards, allocator fence wait, resets). False -> return nullptr.
bool PrepareSC(IFGFeature_Dx12* fg, UINT index);

// D1: DLSSG_Common::QueryState, the usable maximum: max(driver ceiling, unlocked ceiling).
int AdjustMfgMax(int driverMax);
} // namespace ext::fg

namespace ext::present
{
// D5, D1: FrameLimit::sleep. Minimum time (ns) between two frames for the FramerateLimit cap, or nullopt when there is
// no valid cap. With NVIDIA FG the cap counts generated frames, so the interval doubles. Implementation: FrameInterval.cpp.
std::optional<uint64_t> FrameInterval(float fpsCap, bool fgActive);
} // namespace ext::present

// ---------------------------------------------------------------------------------------------------------------
// DLSS-G options / markers (D2). Hosts: framegen/dlssg/DLSSG_Common.cpp (SetOptions), hooks/FG_Hooks.cpp (FGPresent),
// inputs/FG/Upscaler_Inputs_Dx11wDx12.cpp (UpscaleStart), menu/menu_common.cpp (DLSSG card).
class IFGFeature;

namespace sl
{
struct DLSSGOptions;
struct ReflexOptions;
struct ViewportHandle;
enum class Result;
} // namespace sl

namespace ext::fg
{
// DLSSG_Common::SetOptions, after the base options are built and before slDLSSGSetOptions: UI recomposition and the
// hud-less/ui buffer formats (Dx12 output only, nvngx_dlssg reads them at feature creation; needs the hudless source,
// off until C11) and the Vulkan menu interlock (all outputs).
void AdjustDlssgOptions(IFGFeature* fg, sl::DLSSGOptions& options);
// Right after slDLSSGSetOptions: remembers its result/mode and samples the observed multiplier on the existing
// Streamline thread (GetState is not thread safe and consumes presentation telemetry, so never from ImGui).
void AfterDlssgSetOptions(IFGFeature* fg, const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options,
                          sl::Result setOptionsResult);
// Reflex options built next to the DLSS-G ones: in dynamic mode Streamline sees the same output cap as the user, so
// dynamic MFG can account for it.
void AdjustReflexOptions(const sl::DLSSGOptions& options, sl::ReflexOptions& reflex);

// UpscalerInputsDx11wDx12::UpscaleStart, right after StartNewFrame: the game module's per-upscale hook
// (ext::game::IFgMarkers, null for every game but FFXIV).
void OnUpscaleStart(IFGFeature* fg);
// FGHooks::FGPresent. Before the Streamline frame token is fetched / after ReflexSleep (frame = next frame count).
void MarkersBeforePresent(uint32_t frameId);
void MarkersNextFrame(uint64_t frame);
} // namespace ext::fg

namespace ext::menu
{
// DLSSG card, inside the Force Dynamic MFG block: "Current DMFG: Nx (observed)" or the waiting text, with its tooltip.
void ObservedMultiplier(IFGFeature* dlssg);
} // namespace ext::menu

// ---------------------------------------------------------------------------------------------------------------
// XeFG extras (D3) and the activation hook (D3, E3). Hosts: framegen/dlssg/DLSSG_Common.cpp (Activate), framegen/xefg/
// XeFG_Dx12.cpp (Activate, Dispatch), proxies/XeFG_Proxy.h (HookXeFG),
// menu/menu_common.cpp (XeFG card). Implementation: ext/fg/Activation.cpp, ext/fg/xefg/XefgSeams.cpp.
namespace ext::fg
{
// XeFG_Dx12::Activate (before it marks itself active: the provider pacing restarts clean, XeFGPacing::RequestRecovery)
// and DLSSG_Common::Activate (after: the DLSS-G presentation telemetry window restarts, E3).
void OnActivated(IFGFeature* fg);

// XeFGProxy::HookXeFG, right after the provider module is bound and before the first call into it: the built-in MFG
// unlock and pacing patches (XeFGUnlock::Apply). A failure leaves the provider as Intel shipped it.
void OnXefgProviderLoaded(HMODULE provider);

// What XefgSelectFactor decided, for the "XeFG factor" log of the caller.
struct XefgFactorInfo
{
    bool dynamic = false;
    float target = 0.0f;
};
// XeFG_Dx12::Dispatch, interpolation-count block: the factor (generated frames per real frame) to request. `current` is
// the factor in use, `maxInterpolation` what the provider supports. Applies the configured count, dynamic MFG (target
// capped by FramerateLimit) and the range clamp. Does not write FGXeFGInterpolationCount back (SEAMS 7 item 14).
int XefgSelectFactor(IFGFeature_Dx12* fg, int current, int maxInterpolation, XefgFactorInfo& info);
// XeFG_Dx12::Dispatch, frame time: `inputFrameTime` is the frame time the host supplied (ms). A game module with its own
// markers prefers fresh pacing telemetry; others only use it when the input is missing. Falls back to the Opti time.
double XefgFrameTime(double inputFrameTime);
// XeFG_Dx12::Dispatch, after the FTInput selection: discards samples outside [0, 250] ms (sets them to 0 and, unless
// FGSkipReset, `resetHistory`), then feeds the pacing hook with what the provider gets.
void XefgNoteFrameTime(float& frameRenderTime, bool& resetHistory);
// XeFG_Dx12::Dispatch, view matrix: validated world-to-view (nonsingular stationary view when the pose is unknown),
// writes 16 floats. Returns whether the pose was usable. `guideAge` is only for the periodic fallback log.
bool XefgBuildCamera(float* viewMatrix, const float* position, const float* right, const float* up,
                     const float* forward, uint64_t frameCount, int guideAge);
} // namespace ext::fg

namespace ext::menu
{
// XeFG card, right after the Frame Gen slider: Dynamic XeMFG, target FPS, current XeMFG label and Extra Pacing.
void XeFgExtras(IFGFeature* fgOutput);
} // namespace ext::menu

// ---------------------------------------------------------------------------------------------------------------
// Provider-neutral presentation-guide matching and scene-paired history (B5, C5; ext/fg/GuideMatcher.cpp,
// GuideSeams.cpp). DLSSG_Dx12 and XeFG_Dx12 hosts. All of it is inert unless ext::game::Current().fgMarkers is set.
namespace ext::fg
{
enum class Provider
{
    DlssG,
    XeFg
};

// Age of the NR guides paired with the frame being dispatched, and the scene frame it maps to (willDispatchFrame - age).
struct GuideInfo
{
    int age = -1;
    uint64_t sceneFrame = 0;
};

// SetResource, after the frame index is known. Depth/Velocity inputs are replaced by the scene-matched snapshot taken
// `age` frames ago (`matched` is the storage, `input` is repointed at it). False = skip the input (warm-up, gap, resize).
// OptiHDR (C8) adds its own handling of HudlessColor and UIColor inputs here (ext/hdr/FgHdr.cpp: raw scene capture, UI tag
// drop, PQ HUD-less capture); `matched` is the storage for whichever of the two replaces `input`.
bool SetResourcePre(IFGFeature_Dx12* fg, int fIndex, Dx12Resource*& input, Dx12Resource& matched, Provider provider);
// SetResource, after the per-frame resource record took the input's fields: Depth/Velocity never keep a copy.
void SetResourcePost(const Dx12Resource& input, Dx12Resource& frameResource);

// Dispatch, before the motion vectors are fetched: the guide age and scene frame of `willDispatchFrame`.
GuideInfo GuideAge(IFGFeature_Dx12* fg, int fIndex, uint64_t willDispatchFrame);
// Dispatch, before the constants: true when the history is discontinuous (DLSS-G: also makes the next constants reset;
// XeFG: OR it into resetHistory). Replaces DlssgPreConstants/XefgResetHistory.
bool DispatchResetHistory(IFGFeature_Dx12* fg, const GuideInfo& guide, uint64_t frame, Provider provider);
// Dispatch, after the constants were accepted: commit (or invalidate) the history. Replaces DlssgPostConstants/XefgPostDispatch.
void PostDispatch(IFGFeature_Dx12* fg, const GuideInfo& guide, uint64_t frame, bool discontinuity, Provider provider);

// ReleaseObjects (DLSSG_Dx12, XeFG_Dx12): collect and retire the guide snapshots (fence refs). Unfinished copies are
// kept through shutdown on purpose.
void ReleaseGuides(IFGFeature_Dx12* fg);
// DLSSG_Common::Deactivate / XeFG_Dx12::Deactivate, first statement: invalidate history, guide snapshots and the observed
// multiplier.
void OnDeactivating(IFGFeature* fg);
// DLSSG_Common::SetConstants: true while the next constants must carry reset = true (history discontinuity).
bool ForceConstantsReset(IFGFeature* fg);
} // namespace ext::fg

// ---------------------------------------------------------------------------------------------------------------
// OptiHDR in the FG path (C8, C10; implementation ext/hdr/FgHdr.cpp). Hosts: framegen/dlssg/DLSSG_Dx12.cpp and
// framegen/xefg/XeFG_Dx12.cpp (SetResource), inputs/FG/Upscaler_Inputs_Dx11wDx12.cpp (UpscaleStart). All of it is
// inert while ext::hdr::Active() is false (and the game has no ISceneHdr).
struct FG_Constants;
struct _xefg_swapchain_d3d12_resource_data_t;

namespace sl
{
struct Resource;
struct ResourceTag;
} // namespace sl

namespace ext::fg
{
// SetResource, after the lastFormat bookkeeping: the HUD-less format the loop guard compares. R10G10B10A2 while
// OptiHDR converts the HUD-less input (the tag is PQ), `format` otherwise.
DXGI_FORMAT NormalizeFormat(DXGI_FORMAT format);
// SetResource, right after the Streamline resource lifecycle is chosen (DLSS-G): converts a not yet PQ-encoded HUD-less
// colour to the PQ presentation image on the capture list and tags it eOnlyValidNow. False = the conversion failed, FG
// was deactivated (refusing a mismatched input): the caller returns false.
bool DlssgTagHudless(IFGFeature_Dx12* fg, Dx12Resource& fResource, sl::Resource& resource, sl::ResourceTag& tag);
// SetResource, XeFG, after GetResourceData: the same conversion through the SDK's immediate copy path. False = FG was
// deactivated, the caller returns false.
bool XefgPrepareHudless(IFGFeature_Dx12* fg, Dx12Resource& fResource, int fIndex,
                        _xefg_swapchain_d3d12_resource_data_t& resourceParam);
// SetResource, after the resource state was recorded: true while the raw (SDR) HUD-less capture of scene HDR mode is
// only held; the PQ colour and the tag are produced after the interop fence (`_noHudless = true; return true`).
bool SceneRawHudless(const Dx12Resource& input);
// UpscalerInputsDx11wDx12::UpscaleStart: OptiHDR output also needs the FG HDR flag.
void AdjustConstants(FG_Constants& constants);
} // namespace ext::fg
