# Code and seam names per feature

Material moved out of [FEATURES.md](../FEATURES.md) on 2026-10-23: the file lists and the seam lists of each entry, copied unchanged; internal names are renamed or removed. The full list of seams, with host files, is [SEAMS-catalog.md](../SEAMS-catalog.md); the rules are in [SEAMS.md](../SEAMS.md).

## DLSS Neural Rendering with pre-SR multipass

**Code.**

- `OptiScaler/ext/nr/pass/DlssNr_Dx12.cpp`: the Direct3D 12 pass (compute shaders in `ext/nr/shaders/`, compiled at build time).
- `OptiScaler/ext/nr/presr/`: pre-SR placement, pass profiles, cadence, trail guard, split frame work.
- `OptiScaler/ext/nr/exposure/DlssNr_ExposureScan.cpp`: exposure scan.
- `OptiScaler/ext/nr/NrResampler_Dx12.cpp`: the NR-owned resampler used for `WorkingScale`.
- `OptiScaler/ext/nr/presets/NrUserPresets.h`: named user presets.
- `OptiScaler/ext/nr/forwarder/dlssnr_forwarder.cpp`: the `nvngx.dll_dlssnr.dll` forwarder (second project in `OptiScaler.sln`). The odd file name is deliberate: NVIDIA's model only answers a caller whose module path contains `nvngx.dll`.

**Seams.** nr: `EvalBefore`, `EvalAfter`, `PreSrScope`, `NormalizeDlssPresets`, `WrapNvapi`, `NeedsDlssgRuntime`, `AppendNgxSearchPaths`. restrack: `OnCommandListReset`, `WantsCommandListReset`, `NoteUav`, `NoteResourceCreated`, `WantsResourceCreateHooks`, `OnDeviceHooked`, `ReleaseQueueHook`, `OnFeatureRelease`. dx11bridge: `OnNgxInit`, `OnNgxShutdownBegin`. menu: `MergeBoxes`, `PollShortcuts`, `HandleShortcuts`, `KeybindRows`, `DrawOverlays`, `WantsFrame`, `ShaderTimeRows`. gpu: `ResetCommands`, `ExecuteCommands`.

## NR model version picker

Code: `OptiScaler/ext/nr/runtime/` (`NrRuntimeCatalog.h`, `NrRuntimeScan.h`, `NrRuntimeSelection.h`, `NrRuntimeSwitch.h`, `NrRuntimeCore.h`, `NrRuntime.cpp`, `NrRuntimeText.h`), `ext/nr/pass/DlssNr_Dx12.cpp` (`RuntimeHost`, `PollRuntime`), `ext/ffxiv/chara/CharaNr.cpp` (`SyncRuntime`, `NrPreviewRuntime.h`).

Seams: nr `AppendNgxSearchPaths` (body changed, no new seam).

## NR native path and ViT capture

**Code.** `OptiScaler/ext/nr/native/` (`DlssNrNative.cpp`, `DlssNrModuleProfiles.h`, `DlssNrVitCapture.h`, `DlssNrVitReuse.h`), `OptiScaler/ext/nr/hybrid/`. The capture summariser is `tools/nr-vit-capture-summary.ps1`; how to profile a new model file is in [nr-vit-reuse-profile-runbook.md](../nr-vit-reuse-profile-runbook.md).

**Seams.** nr: `WrapNvapi`, `AppendNgxSearchPaths`.

## AMD HIP pre-SR

Code: `OptiScaler/ext/nr/amd/` (`AmdBridge.cpp`, `AmdPreSr.cpp`, shaders).

Seams: nr `PreSrScope` (the AMD edit is chosen inside it), restrack `OnDeviceHooked`.

## Vulkan NR path

Code: `OptiScaler/ext/nr/vk/`, `OptiScaler/ext/nr/NrResampler_Vk.cpp`.

Seams: nr `VkEvalBefore`, `VkEvalAfter`, `VkDeviceExtScope`.

## OptiHDR

- Code: `ext/hdr/Hdr10.cpp`, `PresentHdr.cpp`, `BridgeHdr.cpp`, `FgHdr.cpp`, `ext/hdr/menu/` (menu layer composited into the HDR output), `ext/hdr/shaders/Hdr10.hlsl`.

- Seams: present `PlanDx12Presenter`, `PatchPresenterDesc`, `LogPlainPresenter`. dx11bridge `ConfigureOutput`, `OnDestroy`, `OutputFormat`, `OnResizeDone`, `ConvertCopySource`, `SetColorSpace`. fg `SetResourcePre`, `SetResourcePost`, `NormalizeFormat`, `DlssgTagHudless`, `XefgPrepareHudless`, `AdjustConstants`. menu `AdjustFgOutputOptions`, `UseSdrUiTarget`, `AdjustTheme`. gpu `ResetCommands`, `ExecuteCommands`. hdr `OverlayRtvFormat`, `OverlayPrepare`, `OverlayScope`, `OverlayRelease`.

## RenoDX scene HDR

- Code: `ext/hdr/scene/` (`FfxivSceneHdr.h`, `SceneHooks.cpp`, `DlssSceneInput.h`, `NrSceneInput.h`), `ext/hdr/DlssHdr.cpp`, `ext/hdr/shaders/renodx/` (RenoDX shaders; licence and notice shipped as `Licenses/RenoDX_*`).

- Seams: hdr `DlssExt`, `AdjustDlssCreateFlags`, `RestoreDlssCreateFlags`, `DlssEvalScope`, `BridgeInitScope`, `BridgeEvalScope`. dx11bridge `PresentScope`, `BeforeSharedCopy`. fg `SceneRawHudless`.

## HDR PNG capture

- Code: `ext/hdr/screenshot/`.

- Seams: dx11bridge `PrepareScreenshot`; the menu variant (`ScreenshotMenu`) is taken inside `hdr::OverlayScope` with no extra upstream line.

## DLSS-G multi frame generation unlock, RTX 40

- Code: `ext/fg/dlssg/MfgUnlock.cpp`, `AdaTemporal.h`, `DlssgSeams.cpp`; `ext/fg/streamline/StreamlineSeams.cpp`.

- Seams: streamline `MfgUnlockTick`, `MfgCeilingStale`, `RaiseMfgCeiling`, `OnDlssgModuleLoad`, `WarnMfgOverrideIgnored`. fg `AdjustMfgMax`, `AdjustDlssgOptions`, `AdjustReflexOptions`, `AfterDlssgSetOptions`, `OnActivated`.

## DLSS-G MFG loader for RTX 30

Code: `ext/fg/dlssg/AmpereMfgLoader.cpp`.

Seams: streamline `OnLoadStreamline`.

## DLSS-G render preset

Code: `ext/fg/dlssg/Preset*.h|.cpp` (`PresetX64.h` is the instruction decoder of the guards), `ext/fg/MenuFgPreset.cpp`.

Seams: fg `PresetSwitchTick`; the driver-settings hook calls `ext::fg::preset::OnDrsGetSetting` (upstream: `nvapi/NvApiHooks.cpp` +2 lines, `framegen/dlssg/DLSSG_Dx12.cpp` +2 lines).

## Dynamic MFG on RTX 50

Code: `ext/fg/MenuFgStatus.cpp`, `ext/fg/dlssg/DlssgSeams.cpp`.

Seams: menu `ObservedMultiplier`; fg `AfterDlssgSetOptions`, `AdjustReflexOptions`.

## Dynamic MFG on RTX 40

Code: `ext/fg/DynamicMfg.h` (selector), `DynamicMfgControl.h` (gating, kill-switch, self-test), `DynamicMfgGate.h`, `DynamicMfgGlue.h` (the per-frame tick and the guard, tested with fakes), `DynamicMfgSeams.cpp`, `MenuAutoMfg.cpp`.

Seams: fg `AdjustDlssgOptions`, `AfterDlssgSetOptions`, `OnActivated` (no new seam).

## Pacing, markers and hardening

Code: `ext/fg/` (`FrameInterval.cpp`, `GuideMatcher.cpp`, `GuideSeams.cpp`, `MarkerSeams.cpp`, `Dx12Sc.cpp`), `ext/gpu/`.

Seams: present `FrameInterval`; fg `MarkersBeforePresent`, `MarkersNextFrame`, `OnUpscaleStart`, `GuideAge`, `DispatchResetHistory`, `PostDispatch`, `ForceConstantsReset`, `SubmitSC`, `PrepareSC`, `ReleaseGuides`, `OnDeactivating`; gpu `ResetCommands`, `ExecuteCommands`.

## XeFG and XeMFG

**Code.** `ext/fg/xefg/` (`XeFGUnlock.h`, `XeFGDynamic.h`, `XeFGPacing.h`, `XeFGCamera.h`, `XeFGHdr.h`, `XefgSeams.cpp`).

**Seams.** menu `XeFgExtras`; fg `XefgSelectFactor`, `XefgFrameTime`, `XefgNoteFrameTime`, `XefgBuildCamera`, `OnXefgProviderLoaded`, `XefgPrepareHudless`; dx11bridge `BeforeReleaseFeature`; config `SaveXeFG`.

## HUD-less frame generation

**Code.** `ext/hudless/` (engine, ring of shared textures, mask and paste shaders, provider adapters), `ext/ffxiv/ui/` (finds the first UI draw in FFXIV by shader fingerprint and snapshots the scene there).

**Seams.** hudless `PreInteropCopy`, `UiFreeFrame`, `PostInteropCopy`, `OnReleaseInterop`; present `BeforeOverlay`; fg `AdjustDlssgOptions`; menu `ObservedMultiplier` (counters).

## ReShade built-in add-on

**Code.** `ext/reshade/` (the public ReShade API headers are vendored unmodified in `ext/reshade/third_party/reshade`; licence notice shipped as `Licenses/ReShade_ATTRIBUTION.txt`).

**Seams.** reshade `OnReshadeLoaded`.

## Automatic ReShade scene depth

**Code.** `ext/reshade/ReShadeDepth*.{h,cpp}` (facade `ReShadeDepth`, evaluate engine `ReShadeDepthEval`, binder `ReShadeDepthCore`, copy pass `ReShadeDepthCopy` with `ext/reshade/shaders/depth_crop.hlsl`, G-buffer remap pass `ReShadeGBufferCopy` with `ext/reshade/shaders/gbuffer_remap.hlsl` (`ReShadeGBuffer.shaders.props`), live backend `ReShadeDepthBackend`/`ReShadeDepthRaw`, event callbacks `ReShadeDepthEvents`, late registration `ReShadeDepthLate`, pure decisions `ReShadeDepthGlue.h`, menu card `ReShadeDepthMenu`), `ext/reshade/ReShadeRegistrationState.*`.

**Seams.** reshade `OnEvaluated` (one line in `NVNGX_DLSS_Dx11.cpp`). `ext::reshade::depth::OnNgxShutdown()` is called from the existing seam bodies `dx11bridge::OnNgxShutdownBegin` and `dx11bridge::BeforeReleaseFeature`. Config: `FfxivConfig::automaticReShadeDepth` with a load hook (no seam). The ReShade events are subscribed in `ReShadeDepthEvents.cpp` (five at load, begin and finish from the `init_effect_runtime` callback); no new seam in upstream code.

## Window mode

**Code.** `ext/window/`: `WindowRules.h` (pure rules: border rule, DLSS-G off gate, thread rule, defer verdict), `WindowDeferral.h` (the Idle/Pending/Replaying machine), `WindowProbe.h` (the line format), `WindowController.h` (decisions, driven through an `Env` interface so tests use fakes), `WindowWin32.cpp` (window reads and the extended-style call), `WindowModule.cpp` (live `Env`, entry points), `WindowConfig.h`, `WindowMode.h`, `WindowSeams.h`.

**Seams.** None in upstream files. The module is called from the existing fork seam bodies `dx11bridge::OnResizeDone` and `dx11bridge::PresentScope` (`ext/hdr/BridgeHdr.cpp`) and from a ReShade add-on event (`ext/reshade/ReShadeEvents.cpp`).

## Native quality and forced quality

Code: `ext/ffxiv/quality/` (`FfxivNativeQualityHooks.h`, `NativeQualityRetry.h`, `NativeQualityTransaction.h`, `QualityProvider.cpp`), `ext/upscaler/ForcedQuality.h`, `ext/upscaler/UpscalerSeams.cpp`.

Seams: upscaler `EffectivePerfQuality`, `OnOptimalSettings`, `ApplyForcedQuality`.

## Render geometry

Code: `ext/ffxiv/geometry/`, `ext/upscaler/RenderGeometry.h`.

Seams: upscaler `ResolveRenderGeometry`, `CorrectRenderSubrect`.

## Forced-quality colour scaling

Code: `ext/dx11bridge/ColorScaler_Dx12.cpp`, `ext/dx11bridge/BridgeState.cpp`.

Seams: dx11bridge `PrepareUpscalerColor`.

## GPU submission ownership

Code: `ext/gpu/`.

Seams: gpu `ResetCommands`, `ExecuteCommands`; restrack `OnCommandListReset`, `WantsCommandListReset`; dx11bridge `ShutdownNgx`, `RedirectShutdown1`, `BeforeReleaseFeature`.

## Quality change without the stall

Code: `ext/dx11bridge/NgxLifecycle.cpp`, `NgxFeatureTable.h`, `ReleaseSequence.h`, `ReleaseDrain.h`, `ext/gpu/DrainResult.h`, `GpuQueue.cpp` (`DrainQueueEx`), `ext/upscaler/NgxThreadTrace*.h`.

Seams: dx11bridge `BeforeReleaseFeature` (now returns a three-way result), `ShouldSkipReleaseSleep`, `SetCurrentFeature`, `ClearCurrentFeatureIf`; upstream: `inputs/NVNGX_DLSS_Dx11.cpp` (+13 -13: the table type, one locked iteration, the evaluate pin, the detach in release, the sleep condition); the thread lines are two calls in `ext/upscaler/UpscalerSeams.cpp` and `ext/Seams.cpp`.

## Lighting scan

Code: `ext/ffxiv/lighting/`.

Seams: game `OnD3D11DeviceHooked`, `OnD3D11Unhook`; dx11bridge `OnUpscaleBoundary`.

## Composition swapchain hook

Code: `ext/present/CompositionSwapchain.cpp`, `CompositionPassThrough.h`, `CompositionWindow.h`.

Seams: present `HookFactoryComposition`, `ResolveCompositionHandle`; core `HooksBypassed`. Inline marker `// ext:` comments in `wrapped_swapchain.cpp/.h` (the lines that use `_composition`) and in `hooks/DxgiFactory_Hooks.cpp/.h`.

## Character preview (CharaView)

**Where.** `OptiScaler/ext/ffxiv/chara/` (with `shaders/`), hooks inside `ext/ffxiv/lighting/FfxivLightingCapture.h`, the per-frame callback through `dx11bridge::AfterSharedCopy` in `ext/Seams.cpp`, the game build entry in `ext/ffxiv/GameBuild.h`, ini fragments `ext/ini/FFXIV.ini` and `ext/ini/HDR.ini`.

**Seams.** None in upstream files.

## Frame pacing trace

Code: `ext/present/` (`FramePacingCore.h`, `FramePacingTrace.cpp`, `FramePacingConfig.h`, `HiddenPresentPolicy.h`, `PresentSeams.h`).

Seams: present `PacingScope`, `HiddenPresent`, `NativeBegin`/`NativeEnd`, `FgDispatchBegin`/`FgDispatchEnd`, `RecordSample`. Upstream lines: 3 (`wrapped/wrapped_swapchain.cpp`, `hooks/FG_Hooks.cpp`, `with_dx12/dx11_with_dx12_sc.cpp`).

## Bridge flush after signal

Code: `ext/bridge/` (`BridgeSeams.h` is the cached key and the seam, `BridgeConfig.h` the `[Bridge] FlushAfterSignal` key, `BridgeSeams.cpp` the one startup line, called from `ext/core/Banner.cpp`).

Seams: bridge `FlushAfterSignal`. Upstream lines: 2 in `with_dx12/dx11_with_dx12.cpp` (the `#include "ext/Seams.h"` and the call in `SyncDx11ToDx12`, right after the accepted fence `Signal`); upstream's own commented-out `Flush()` stays.

## Automatic mipmap bias, bind-time mode

**Code:** `ext/upscaler/MipBind.h` (target, eligibility, add/replace detector, gate, texts), `ext/upscaler/MipBindEngine.h` (sampler table, variants, bind path), `ext/upscaler/MipBindControl.h` (controller and install plan), `ext/upscaler/MipBindHooks.cpp` (attaches it to the game).

Seams:** no new seam; `OnD3D11DeviceHooked`, `OnD3D11Unhook` and `OnUpscaleBoundary` also reach the bind-time module, and the first upscaler create reaches it through `mipbias::OnFeatureCreate`.

## Automatic mipmap bias, create-time mode

**Code:** `ext/upscaler/MipBias.h` (formula, source policy, log and menu texts, tested in `tests/ext/upscaler/mip_bias_*`), `ext/upscaler/MipBias.cpp` (glue), `ext/upscaler/menu/MipBiasMenu.cpp`.

Seams:** none added; the existing upscaler seam `ApplyForcedQuality` also hands every feature create to the automatic mip bias.

## Menu style (Ember)

**Code.** `ext/menu/EmberPalette.h` (the palette and metrics as plain data, no ImGui types), `ext/menu/MenuStyleSeams.cpp` (copies the palette into the ImGuiStyle), `ext/menu/MenuStyle.h` and `MenuStyleConfig.h` (the key).

**Seams.** menu `ApplyStyle`.

## F2, F3, F4

F2: `ext/nr/shaders/dlssnr.hlsl`. F3: `ext/nr/` (config, menu, presets), `ext/ini/DlssNr.ini`. F4: `ext/hdr/screenshot/` (`MenuCapture.h`), `ext/ini/HDR.ini`.

Seams: none added by F2 or F3; F4 uses the existing `dx11bridge::PrepareScreenshot` (it returns early while a menu capture is wanted) and `hdr::OverlayScope`.

## Menu layout

Code: `ext/core/` (`SourceTracker.h`, `MenuLayout.h`, `MenuManifest.h`, `MenuRegistry.*`).

Seams: menu `SourceTracker::Claim` (the tab loop) and `FfxivTabVisible` (one `tabs[]` row).
