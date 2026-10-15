# OptiScaler-FFXIV seam catalog

The conflict surface of the fork. Contract and mechanics are in [SEAMS.md](SEAMS.md); this file lists every seam, its form, host and cost. On every nightly sync, review the host files below and nothing else. A seam that is added, removed or moved must update this file in the same commit.

Compiled from the five port ledgers (U1..U5, `phase3-ledger/`) with SEAMS.md section 5 applied wherever a ledger proposal conflicted.

## Conventions

- **Form:** `hook` = fire-and-forget call. `decision -> T` = upstream branches on the returned value; on any failure it returns the upstream-default answer. `inline-guarded` = inline wrapper in `Seams.h` that tests a feature bit or flag (relaxed atomic or `thread_local`) before an out-of-line call; cost when off is one load and one branch. `RAII` = scope object constructed by one upstream line.
- **Frequency:** `init`, `teardown`, `rare` (resize, colour-space change), `per-create` (per device, swapchain, feature or DLL load), `per-resource`, `per-cmdlist`, `per-frame`.
- **Owner IDs** are the FEATURE-INVENTORY IDs.
- **Naming:** group names as in SEAMS.md; PascalCase functions. Where ledgers named the same seam differently, the name below is the single one (see "Renames" at the end of this section).
- **Free functions over virtuals.** No virtual is added to an upstream class. Per-object state hangs off one `_ext` member (section C).
- **Hdr truth:** `ext::hdr::Active()` is an inline reader of nightly's `hdrOutputActive`/`outputColorSpace` (SEAMS 5). `isHdrActive` does not exist.
- **Count basis for the footprint:** added plus removed lines (numstat sum), the same basis as the 6.8k reference figure. Replacing an N-line block with one seam call therefore counts N+1.

### Renames (ledger name -> catalog name)

| Ledger name | Catalog name |
|---|---|
| U1 `ext::quality::EffectivePerfQuality`, `OnOptimalSettings` | `ext::upscaler::EffectivePerfQuality`, `OnOptimalSettings` |
| U1 `ext::dlssg::OnLoadStreamline` | `ext::streamline::OnLoadStreamline` |
| U1 `ext::xefg::OnProviderLoaded` | `ext::fg::OnXefgProviderLoaded` |
| U1 `ext::pacing::FrameInterval` | `ext::present::FrameInterval` |
| U1 `ext::hdr::MenuHdrFlag/MenuPixelShader` | `ext::hdr::MenuHdrFlag/MenuPixelShader` (contingent only) |
| U1 `ext::diag::LogFullscreenState`, U2 `OnFgActivated` log, U4 fullscreen logs | dropped (log-only) |
| U2/U3 `ExecuteUi`/`ResetUi`, U3 `ext::gpu::*`, U5 `ResetCopyList`/`ExecuteCopyList`, `Hdr10::ExecuteCommands/ResetCommands` | `ext::gpu::ExecuteCommands`, `ext::gpu::ResetCommands` |
| U2 `SetPresentationGuideDelay`, `ObservedMultiplier`, U3 `OnSetInitParameters` | folded into `ext::fg::OnUpscaleStart`, `ext::menu::FgStatusExtras`, `ext::upscaler::ApplyForcedQuality`/`ResolveRenderGeometry` |
| U2 `MatchGuide`, `XefgResetHistory`, `XefgOnActivate`, `XefgPostDispatch`, `DlssgPreConstants/PostConstants` | `SetResourcePre`, `DispatchResetHistory`, `OnActivated`, `PostDispatch`, `DispatchResetHistory`/`PostDispatch` |
| U3 `NrEvalBefore/After` and `NgxEvalBefore/After` | `ext::nr::EvalBefore`, `ext::nr::EvalAfter` |
| U3 `ext::nr::NeedsDlssgRuntime`, `WrapNvapi`, `AppendNgxSearchPaths` | unchanged (group nr) |
| U3 `ext::dx11bridge::InitFeature`, `ext::gpu::AllocatorRetired` | removed: both are `[UP]` (section a) |
| U3 `IGameScene` | split into `ext::game` interfaces (SEAMS 3.8) |
| U4 `ext::ffxiv::HooksBypassed` | `ext::core::HooksBypassed` |
| U4 `ext::ffxiv::OnD3D11DeviceHooked/OnD3D11Unhook` | `ext::game::OnD3D11DeviceHooked/OnD3D11Unhook` |
| U4 `ext::ffxiv::DynamicMarkers*` | `ext::fg::MarkersBeforePresent/MarkersNextFrame` |
| U4 `ext::fg::MfgUnlockTick/MfgCeilingPending/RaiseMfgCeiling/OnDlssgModuleLoad/ApplyMenuDlssgInterlock` | `ext::streamline::*` |
| U4 `ext::restrack::EnsureHooks` | ext-internal; the upstream function is `ResTrack_Dx12::EnsureResetHook` |
| U4/U5 `ResolveSizeToWindow`, `FinalizeSizedDesc` | dropped (E5 0x0 resolve, SEAMS 5) |
| U5 `ext::menu::OverlayHdr*` | `ext::hdr::Overlay*` |
| U5 `AppendBoxes` | `ext::menu::MergeBoxes` (SEAMS 3.6) |

## config

Fork keys live in per-feature structs bound by a declarative table; `ext::Cfg()` aggregates them. Section and key names are identical to today. `Config.h` loses every fork member (the ~460 `Config::Instance()->X` fork call sites exist only in fork code that moves to ext). Dropped keys (E1 `ProxyProbe`/`UseProxy`/`ScanAnchor*`, E2 `ProbeD3D11`, E6 `External`, dead `AdaBlackwellKernels`, migration-only `WhitePointFromExposure`) are not carried. `AutoCapture` becomes a real key (default false).

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::config::Load(CSimpleIni& ini)` | hook | `Config.cpp` `Config::Reload`, last statement | all ExtConfig owners (A*, B*, C*, D*) | init, each reload | After all upstream reads, so ext can post-validate upstream keys (`[XeFG] InterpolationCount` reset when > 7 stays out of upstream code). Ext carries its own copies of the `readBool/readInt/...` helpers; no helper is exposed. Must be reached from `LoadFromPath` too: S1 verifies it calls `Reload`, otherwise add the same call there. Ordering: `WhitePointSource` is derived after any legacy read; `Precision` assigned volatile, not `set_from_config`. |
| `ext::config::Save(CSimpleIni& ini)` | hook | `Config.cpp` `Config::SaveIni`, before `ini.SaveFile` | same | rare (user save) | Writes defaults as the member's own string; hex for key values > 0; retires `HDR.MenuNits`, `HDR.ScreenshotHotkey`. |
| `ext::config::SaveXeFG(CSimpleIni& ini)` | hook | `Config.cpp` `Config::SaveXeFG` | D3 | rare | SEAMS 3.4: fork XeFG keys must persist through the quick-save too. |

Not seams: `[LowLatency] Input/Output` and `[Vulkan] UseCopyForInputs/Output` persistence (upstream members, fork-added ini keys) are ExtConfig keys written by Load/Save. `Config::XeFGMaxInterpolations` moves to `ext/fg/FgConfig.h`. The ini fragments in `ext/ini/` are merged by `tools/package.ps1`.

## menu

`MenuRegistry` cards registered through `MergeBoxes`: `dlss_nr` (tab Upscaling, via `RenderCard`; `DlssNr::RenderMenu` loses its own CollapsingHeader), `forced_quality` (Upscaling, `after = "upscale_ratio"`'s predecessor), `ffxiv_hdr` (Frame Gen, visible when FFXIV), `fg_unlocks` (Frame Gen), `fg_hudless` (Frame Gen, visible when DLSS-G + OptiFG), `ffxiv_companion` (Compatibility, visible when FFXIV). Draw callbacks receive `ext::menu::Ctx`, built from `RenderMenuContext` through `ext::Access`.

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::menu::MergeBoxes(const MenuBox* base, size_t& count) -> const MenuBox*` | decision | `menu_common.cpp` `MenuCommon::GetMenuBoxes` (static array) | A5, A6, A12, C1, C5, C7, C8, C9, C10, C11, D1, D4 | init, once | Replaces the 2 NR/Companion box literals. Fixes the RenderCard/ScopedCard bypass. Registered card order uses `after`. |
| `ext::menu::XeFgExtras(Ctx&, IFGFeature* fg)` | hook | `menu_common.cpp` `RenderFrameGenerationRuntimeSettings`, XeFG card, between upstream controls | D3 | per-frame (menu open) | Dynamic MFG, target FPS, current XeMFG label, Extra Pacing. In-card because of `SameLine` layout. |
| `ext::menu::AdjustFgOutputOptions(OutputOptions&)` | hook | `menu_common.cpp` `RenderFrameGenerationSelection` | C8 | per-frame (menu open) | FSR FG output disabled while OptiHDR active. |
| `ext::menu::FgStatusExtras(Ctx&)` | hook | `menu_common.cpp` FG status/debug area | D2, C11 | per-frame (menu open) | "Current DMFG: Nx (observed)" and the HUD-less tagged/rejected counters. Replaces the `IFGFeature::GetObservedFrameMultiplier` virtual (ext reads its own state). |
| `ext::menu::PollShortcuts(Ctx&)` | hook | `menu_common.cpp` `UpdateManualInput` | A5, C9 | per-frame (input thread) | NR toggle key and HDR screenshot key polled with the registry's own `CheckShortcut`. |
| `ext::menu::HandleShortcuts(Ctx&)` | hook | `menu_common.cpp` `HandleMenuShortcuts` | A5 | per-frame | NR on/off flip and toast. Merge with PollShortcuts if both sit in one function after the sync. |
| `ext::menu::KeybindRows(Ctx&)` | hook | `menu_common.cpp` keybind list (`Keybind` rows) | A5, C9 | per-frame (menu open) | Keybind slot indices 14 (NR) and 15 (screenshot) are persisted: keep stable. Needs `Keybind` reachable through `ext::Access`. |
| `ext::menu::DrawOverlays(Ctx&)` | hook | `menu_common.cpp` `RenderMenu`, next to `RenderPerformanceOverlay` | A6, A4 | per-frame | NR compare tags plus exposure-scan lamp. One call replaces U5 rows 15 and 40. |
| `ext::menu::WantsFrame() -> bool` | decision | `menu_common.cpp` `BeginMenuFrameIfNeeded` (OR term) | A4, A6 | per-frame | Menu frame is built when the scan lamp or compare tags are on. Overlay Present must still draw when the menu is closed. |
| `ext::menu::ShaderTimeRows(Ctx&)` | hook | `menu_common.cpp` `RenderShaderTimes` (condition and table row) | A1 | per-frame (menu open) | Returns the "Neural Rendering N ms" row and feeds `hasExtra`. 2-3 call lines. |
| `ext::menu::AdjustUiTargetMode(UiTargetMode) -> UiTargetMode` | decision | `menu_common.cpp` `getUiTargetMode` | C8 | per-frame | Returns SDR while OptiHDR composites the menu. Replaces `LegacyMenuToneMapRequired`, the `MenuHdrCheck` re-indent and the notification term. See conflict 3 in section (e). |
| `ext::menu::AdjustTheme(ImGuiStyle&)` | hook | `menu_common.cpp` `ApplyThemeStyle`, end | C8 | per-create (theme apply) | Forces window/child/popup background alpha to 1 under OptiHDR. |
| `ext::menu::OnVulkanOverlay(bool created)` | hook (provisional) | `menu_overlay_vk.cpp` `CreateVulkanObjects`/`DestroyVulkanObjects` | D5, B9 | init, teardown | Sets the flag read by `ext::streamline::ApplyMenuDlssgInterlock`. Dropped (with `State::menuOverlayIsVulkan`) if S6 finds nightly's Vulkan DLSS-G menu path already covers it. `delayMenuRenderBy` is dropped regardless (write-only). |

Not carried (SEAMS 3.6, 5): cosmetic text and label changes, DLSS preset tooltip rewrite, `Keybind` reset-label parameter, `External` frame-gen checkbox (E6), add-on gate and wording (C11 add-on). Undecided minor items are in section (e).

## upscaler

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::upscaler::EffectivePerfQuality(int asked) -> NVSDK_NGX_PerfQuality_Value` | decision | `NVNGX_Parameter.cpp` DLSS and DLSSD `GetOptimalSettings` callbacks (2 sites) | C1, C5 | per-create (game query path) | Forces DLAA when native DLAA is required (via `game::Current().nativeQuality`), else applies `ForcePerfQuality`. The write-back to `PerfQualityValue` stays one inline line. |
| `ext::upscaler::OnOptimalSettings(q, rw, rh, dw, dh)` | hook | `NVNGX_Parameter.cpp` end of both callbacks (2 sites) | C1 | per-create | Records `LastQualityAnswer`; accessor lives in `ext/upscaler`, not in `NVNGX_Parameter.h`. |
| `ext::upscaler::ApplyForcedQuality(params, w, h, outW, outH, int& pq) -> bool` | decision | `IFeature.cpp` `IFeature::SetInitParameters` | C1, C5 | per-create | Applies the forced preset only when the create descends from the last answer (slack 32 px). Also performs the lazy `INativeQuality::EnsureHooks()` (replaces the separate `OnSetInitParameters`). Config key `ForcePerfQuality`. |
| `ext::upscaler::ResolveRenderGeometry(const NVSDK_NGX_Parameter*, unsigned& w, unsigned& h) -> bool` | decision | `IFeature.cpp` `IFeature::GetRenderResolution` | C2, C3, C5 | per-frame | `IRenderGeometry::ResolveRenderExtent`. C3 gate restored as designed in `adfbea9d` (both halves, default off; default-off behaviour equals the oracle). `EnsureHooks` is a cached pointer test here, never a mutex or string compare. The "Evaluate geometry" log dedupe is ext-side (no `IFeature.h` field). |
| `ext::upscaler::CorrectRenderSubrect(IFeature*, NVSDK_NGX_Parameter*)` | hook | `DLSSFeature.cpp` and `DLSSDFeature.cpp` `ProcessEvaluateParams`; `IFeature_Dx11wDx12.cpp` and `IFeature_VkwDx12.cpp` `Evaluate` | C2 | per-frame | Writes the corrected size back into `DLSS_Render_Subrect_Dimensions_*`. Free function (needs `ext::Access` on `IFeature`), so no declaration is added to `IFeature.h`. |

## dx11bridge

Group for the D3D11-to-D3D12 bridge (`Dx11wDx12SC`, `IFeature_Dx11wDx12`, `NVNGX_DLSS_Dx11.cpp`). Per-bridge state hangs off `Dx11wDx12SC::_ext` (BridgeExt) and `IFeature_Dx11wDx12::_ext` (BridgeState). The recorded-Init fix is a `[UP]` commit, not a seam.

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::dx11bridge::ConfigureOutput(Dx11wDx12SC&, IDXGISwapChain4* fg, DXGI_FORMAT orig) -> bool` | decision | `dx11_with_dx12_sc.cpp` ctor, after `_RefreshCachedSwapchainDesc` | C8 | init | Calls `Hdr10::Configure` when FFXIV HDR is on and the FG chain is R10G10B10A2; restores the format on failure. Result kept in `_ext`. |
| `ext::dx11bridge::OnDestroy(Dx11wDx12SC&)` | hook | `dx11_with_dx12_sc.cpp` dtor | C8, B6 | teardown | `Hdr10::Deactivate`, drain lighting-scan readbacks. Contract: never block, never release from DLL detach under loader lock. |
| `ext::dx11bridge::PresentScope` (RAII) | RAII | `dx11_with_dx12_sc.cpp` `Present`, after bypass early-return | C10 | per-frame | `EndFrame` of the scene-HDR frame guard at every exit. May also own state of PreInteropCopy/PostInteropCopy. |
| `ext::dx11bridge::PreInteropCopy(Dx11wDx12SC&, uint32_t slot)` | hook | `dx11_with_dx12_sc.cpp` `Present`, before `_WaitDx11ThenDx12` | C11 | per-frame | HudlessEngine P1: `HudlessSource::Take`, validate, normalise, `CopyResource` on the D3D11 immediate context before the interop signal. |
| `ext::dx11bridge::UiFreeFrame(Dx11wDx12SC&) -> bool` | decision | `dx11_with_dx12_sc.cpp` `Present`, after `_WaitDx11ThenDx12` | C11 | per-frame | P2: hud-less pending and paste ready. |
| `ext::dx11bridge::PostInteropCopy(Dx11wDx12SC&) -> bool needRecopy` | decision | `dx11_with_dx12_sc.cpp` `Present`, after `_WaitForInteropCopyOnPresentQueue` | C11 | per-frame | P4+P5: tag hud-less on the FG UI list, produce the paste. Upstream keeps `if (needRecopy) { copy; wait }` (3 inline lines). |
| `ext::dx11bridge::CopyScope` (RAII) | RAII | `dx11_with_dx12_sc.cpp` `_CopyDx11SharedToDx12FGBackBuffer` | C8, C9, C11 | per-frame | Created at the allocator reset; finishes screenshot submit after the execute. |
| `ext::dx11bridge::SelectCopySource(CopyScope&) -> CopySource` | decision | same function, source selection | C11, C8, C9 | per-frame | P3 FG-input swap, HDR convert of copy source, mode-1 FG input re-tag. Must stay inside this one function (cross-queue ordering relies on the DX11 wait). |
| `ext::dx11bridge::BeforeSharedCopy(ID3D11DeviceContext*)` | hook | `dx11_with_dx12_sc.cpp` `_CopyDx11BackBufferToShared`, before | C9 | per-frame | `HdrPreview::SealFrame`. |
| `ext::dx11bridge::AfterSharedCopy(ID3D11DeviceContext*, ID3D11Texture2D* dst)` | hook | same, after | C7 | per-frame | Companion source markers; the FFXIV test is behind `game::Current()`. |
| `ext::dx11bridge::OutputFormat(Dx11wDx12SC&, DXGI_FORMAT requested) -> DXGI_FORMAT` | decision | `ResizeBuffers`, `ResizeBuffers1` (FG resize format) | C8 | rare | R10G10B10A2 when HDR output. |
| `ext::dx11bridge::OnResizeDone(Dx11wDx12SC&) -> HRESULT` | decision | `ResizeBuffers`, `ResizeBuffers1`, after success | C8 | rare | Re-runs `Hdr10::Configure`; fails with `DXGI_ERROR_UNSUPPORTED`. |
| `ext::dx11bridge::SetColorSpace(Dx11wDx12SC&, DXGI_COLOR_SPACE_TYPE, HRESULT& out) -> bool handled` | decision | `dx11_with_dx12_sc.cpp` `SetColorSpace1` | C8 | rare | Swallows the game's call when OptiHDR owns the output. Otherwise fills nightly's `outputColorSpace`/`hdrOutputActive` (SEAMS 5; the bridge never filled them in the oracle). |
| `ext::dx11bridge::OnReleaseInterop(Dx11wDx12SC&)` | hook | `dx11_with_dx12_sc.cpp` `_ReleaseInteropBackBuffers` | C11 | rare (resize), teardown | P8: drain FG queue (2 s), release the hud-less ring; leaks on failure by design. |
| `ext::dx11bridge::PrepareUpscalerColor(IFeature_Dx11wDx12&, cmd, params, color, uint2& guideSrc) -> ID3D12Resource*` | decision | `IFeature_Dx11wDx12.cpp` `Evaluate`, before binding Color | C4 | per-frame | Lanczos3 scale of the display-sized colour to render size when `GameQuirk::ScaleDisplayColorForForcedQuality` and `IRenderGeometry::ColorIsDisplayAllocation`. nullptr = failure, upstream `break`s inline. |
| `ext::dx11bridge::OnUpscaleBoundary(ID3D11DeviceContext*, ID3D11Resource* color, const NVSDK_NGX_Parameter*)` | hook | `IFeature_Dx11wDx12.cpp` `Evaluate`, top | B6 | per-frame | `ISceneLighting::OnUpscaleBoundary`; one relaxed load when disarmed. |
| `ext::dx11bridge::OnNgxInit()` | hook | `NVNGX_DLSS_Dx11.cpp` `NVSDK_NGX_D3D11_Init_Ext` | A9, C6 | init | NR resume after bridge init. Not placed by S4 (an empty hook without NR): S2 adds the call with its body. |
| `ext::dx11bridge::OnNgxShutdownBegin()` | hook | `NVNGX_DLSS_Dx11.cpp` `NVSDK_NGX_D3D11_Shutdown` | A9, C6 | teardown | NR suspend; before the shutdown-state teardown. Not placed by S4 (see OnNgxInit): S2 adds it, as the first statement after `shutdown = true;`. |
| `ext::dx11bridge::ShutdownNgx(ID3D11Device*, NVSDK_NGX_Result& out) -> bool handled` | decision | `NVNGX_DLSS_Dx11.cpp` `NVSDK_NGX_D3D11_Shutdown` | C6 | teardown | `INgxLifecycle::IsolatesShutdown`: device-specific `Shutdown1` only, keeps Streamline's DX12 NGX alive. The `D3D11Device = nullptr` reorder and success-gated `SetDx11Inited(false)` stay inline. |
| `ext::dx11bridge::RedirectShutdown1(ID3D11Device* in, ID3D11Device*& recorded) -> bool` | decision | `NVNGX_DLSS_Dx11.cpp` `NVSDK_NGX_D3D11_Shutdown1`, first statement | C6 | teardown | A null-device `Shutdown1` has the same global effect as the legacy one. Signature refined: the file-local `D3D11Device` is passed by reference (the seam records the device). True -> caller returns `NVSDK_NGX_D3D11_Shutdown()`. |
| `ext::dx11bridge::BeforeReleaseFeature(IFeature_Dx11&, bool shuttingDown) -> bool ok` | decision | `NVNGX_DLSS_Dx11.cpp` `NVSDK_NGX_D3D11_ReleaseFeature` | C6, D3, D2 | per-create (feature release) | Cancel pending FG upscaler work under `fg->Mutex` (`ext::Access::CancelPendingUpscalerWork`, ext-internal; needs the `friend struct ext::Access;` line in `IFGFeature_Dx12.h`), drain FG and upscale queues (`ext::gpu::DrainQueue`), rebuild the XeFG context when `FGXeFGResetOnUpscalerChange`. false -> `FAIL_PlatformError`. **S4 status:** everything but the XeFG rebuild; S5 (D3) adds that block where marked in `ext/dx11bridge/NgxLifecycle.cpp`. |

## nr

Native DLSS Neural Rendering (A*, B*, E4 AMD PreSR). All internals live in `ext/nr`; these are the upstream attachment points.

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::nr::EvalBefore(NrEvalCtx&)` | hook | `NVNGX_DLSS_Dx12.cpp` `NVSDK_NGX_D3D12_EvaluateFeature` (native passthrough and OptiScaler path: 2 sites); `IFeature_Dx11wDx12.cpp` `Evaluate`; `IFeature_VkwDx12.cpp` `Evaluate` | A1, A9, B1, B8, B9, C4 | per-frame | `NrEvalCtx` (ext/nr/NrSeams.h) carries site (Native, Dx11Bridge, VkBridge), cmd, params, queue, frame, the NGX feature (SR/RR; bridges derive it from the upscaler) and the C4 guide extent. The SR/RR-only filter lives inside (native: both Before and After, so a DLSS-D/other handle never runs the model). Not gated on the NR switch: the NR internals still see the evaluate while NR is off (toggle reset, skip log), as in the oracle. `EvalBefore` latches `F::Nr` once NR was seen enabled (hot-path guard of the A4 scan seams). **S2b:** placed in `NVNGX_DLSS_Dx12.cpp` (2 sites). Bridges: see the S2b rows below as they land. |
| `ext::nr::EvalAfter(NrEvalCtx&, bool ok)` | hook | same four sites | A1, A9, B8, B9 | per-frame | Replaces `DlssNr::EvaluateAfterUpscale` and the one-shot hand-off log. `ProbeD3D11` (E2) is not called. |
| `ext::nr::VkEvalBefore(cmd, params, inst, pd, dev, Upscaler backend) -> VkNrToken` | decision | `NVNGX_DLSS_Vk.cpp` `NVSDK_NGX_VULKAN_EvaluateFeature`, before `deviceContext->Evaluate` | B9 | per-frame | Token carries `handled`, `rayReconstruction`, `bridged`, original and NR colour; destructor restores `Parameter_Color`. The bridged-backend list is an `IsOn12(Upscaler)` helper in ext (the oracle list omits `DLSS_on12`). |
| `ext::nr::VkEvalAfter(VkNrToken&, cmd, params, inst, pd, dev)` | hook | same, after a successful upscale | B9 | per-frame | Skipped when `token.bridged`. |
| `ext::nr::PreSrScope` (RAII) `(ID3D12Device*, Upscaler, int featureFlags, cmd, params, ID3D12Resource* outputTarget, ID3D12CommandQueue* timingQueue)` | RAII | `IFeature_Dx12.cpp` `Evaluate`, replaces the `Set(Output, currentTarget)` line | B1, B3, E4 | per-frame | Points Output at the first shader pass' input and, for run-before-SR on an AMD device, Color at the AMD pre-SR edit (`AmdBridge::CanUse/EnsureHooks/Prepare`, ApplyModel gating); restores both on every exit (replaces the `NgxResourceBinding` plumbing; Motion/Depth bindings are not carried). Arguments are values because `Device` is protected. `timingQueue` is a 1-line inline signature extension (`// ext:`, `IFeature_Dx12.h` + `.cpp`). |
| `ext::nr::NormalizeDlssPresets(NVSDK_NGX_Parameter*)` | hook | `DLSSFeature.cpp` `ProcessInitParams` | B1 | per-create | Maps the six `Hint_Render_Preset_*` equal to `NV_PRESET_LATEST`. SR only. |
| `ext::nr::WrapNvapi(NvAPI_ID, void* fn) -> void*` | decision | `NvApiHooks.cpp` `hkNvAPI_QueryInterface`, final return | A10, B2 | init (hundreds of calls) | Pass-through when NR/hybrid is off; recursive mutex only when on. The E6 early-return and KCD2 hunks are not carried. |
| `ext::nr::NeedsDlssgRuntime(const std::wstring& fileName) -> bool` | decision | `Util.cpp` `Util::FindFilePath` (`isDlssgOutput` term) | B7 | per-create (file lookup) | NR + DeferredDLSS + ResidualFG needs the official FG runtime. |
| `ext::nr::AppendNgxSearchPaths(std::vector<std::wstring>&)` | hook | `Streamline_Proxy.h` `LoadStreamline`, where the SL NGX path list is built | A1, D5 | init, once | Merges U1 hunks 3 and 4. |
| `ext::nr::AppendVkDeviceExtensions(VkPhysicalDevice, VkDeviceCreateInfo& local) -> ExtScope` | decision | `Vulkan_Hooks.cpp` `hkvkCreateDevice`, plus `ExtScope::LogResult(VkResult)` after the real call | B9 | per-create | `ExtScope` owns the vector so `ppEnabledExtensionNames` outlives the call; early-out when NR is off. The swapchain colour-space log (dead) is dropped. |

**S2a status (code is in `ext/nr`, nothing in upstream calls it yet; the seams above are S2b's).** Ext-internal hooks of the moved code, none of them an upstream seam:

| Hook | Where | Notes |
|---|---|---|
| `DlssNr::EnsureGpuHooks(ID3D12Device*)` | `ext/nr/pass/DlssNr_Dx12.cpp` | Registers NR's one `ext::gpu::Owner` (begin = `BeginGpuSubmission`, quarantine = `QuarantineGpuSubmission`, reset = `NotifyGpuReset`) on first call, then `ext::restrack::EnsureHooks(device)`. Called inside NR before it records; replaces `ResTrack_Dx12::HookLateNrQueue`. |
| `DlssNr::AmdBridge::EnsureHooks(ID3D12Device*)` | `ext/nr/amd/AmdBridge.cpp` | Registers the AMD pre-SR owner (isolate = `PendingListIndex`, before = `Submitting`, after = `Submitted`), then `ext::restrack::EnsureHooks`. S2b's `PreSrScope` calls it where `IFeature_Dx12::Evaluate` called `CanUse(Device) && HookLateNrQueue(Device)`. |
| `ext::nr::SetSceneHdrHooks(const SceneHdrHooks&)` | `ext/nr/NrHooks.h` | C10 (S6) fills it. With nothing registered NR runs on the SDR colour (= HDR.NRSceneInput / DLSSSceneInput off). |
| `ext::game::Current().nrQuirks` (`INrQuirks::BridgedNativeDx11`) and `.lighting` (`ISceneLighting::ConsumeHistoryReset(uint64_t& cursor)`) | `ext/game/GameInterfaces.h` | Replace the `gameExe == "ffxiv_dx11.exe"` tests inside NR; `ext/ffxiv` (S7) provides both. |

Not seams: `Shader_Dx12.cpp` `TranslateTypelessFormats` skip is solved by the NR call site passing `translateTypeless=false` (0 upstream lines; done in `DlssNr_Dx12::DispatchPass`, S2a). `OS_Dx12`/`OS_Vk` are not touched: NR owns `NrResampler_Dx12`/`NrResampler_Vk` (SEAMS 7 A8, S2a). `OS_Dx12`/`OS_Vk` scaler-override constructors are inline-marked `// ext: A8` (section d). The `dlssnr` forwarder DLL is the second project in `OptiScaler.sln`.

## fg

Provider-neutral frame-generation seams (DLSS-G, XeFG, FSR-FG) on `IFGFeature_Dx12`, `DLSSG_Common`, `Upscaler_Inputs_Dx11wDx12`, `FG_Hooks`. Per-object state: `IFGFeature_Dx12::_ext` (`ext::fg::Dx12Ext`, holding guide ring, `_sceneHistory`, SC fence state, DLSS-G observed multiplier and history, XeFG dynamic state). `DLSSG_Common` gets no member; ext reaches the derived object by `dynamic_cast` cached in `Dx12Ext`.

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::fg::OnActivated(IFGFeature*)` | hook | `DLSSG_Common.cpp` `Activate`; `XeFG_Dx12.cpp` `Activate` | D3, E3 | init | XeFG `RequestRecovery`; DLSS-G present stats reset. Fullscreen-state logging is dropped. |
| `ext::fg::OnDeactivating(IFGFeature*)` | hook | `DLSSG_Common.cpp` `Deactivate`; `XeFG_Dx12.cpp` `Deactivate` | B5, C5 | teardown | Invalidate history and guide snapshots, observed multiplier. |
| `ext::fg::AdjustMfgMax(int driverMax) -> int` | decision | `DLSSG_Common.cpp` `QueryState` | D1 | init | `max(driverMax, MfgUnlock::UnlockedMax())`. Must agree with `streamline::RaiseMfgCeiling`. |
| `ext::fg::AdjustDlssgOptions(DLSSG_Common*, sl::DLSSGOptions&, sl::ReflexOptions&)` | hook | `DLSSG_Common.cpp` `SetOptions` | D1, D2, C11 | per-frame | UI recomposition and hud-less/ui buffer formats, dynamic-MFG frame limit in the Reflex options, menu interlock (calls `streamline::ApplyMenuDlssgInterlock`). Logs on change only. The add-on module probe is gone. |
| `ext::fg::AfterDlssgSetOptions(DLSSG_Common*, const sl::DLSSGOptions&, sl::Result)` | hook | `DLSSG_Common.cpp` `SetOptions`, after | D2 | per-frame | Observed multiplier sampling; runs on the existing SL thread. |
| `ext::fg::ForceConstantsReset(DLSSG_Common*) -> bool` | decision | `DLSSG_Common.cpp` `SetConstants` (`reset` term) | B5, C5 | per-frame | State held in `Dx12Ext`, no `_forceConstantsReset` field upstream. |
| `ext::fg::GuideAge(IFGFeature_Dx12*, uint32_t idx) -> GuideInfo` | decision | `DLSSG_Dx12.cpp` and `XeFG_Dx12.cpp` `Dispatch` | B5 | per-frame | `age` and `sceneFrame`. FFXIV test is `game::Current().fgMarkers != nullptr`. |
| `ext::fg::DispatchResetHistory(IFGFeature_Dx12*, const GuideInfo&) -> bool` | decision | `DLSSG_Dx12.cpp` and `XeFG_Dx12.cpp` `Dispatch`, before constants | B5, C5 | per-frame | Replaces `DlssgPreConstants` and `XefgResetHistory`. |
| `ext::fg::PostDispatch(IFGFeature_Dx12*, bool ok, const GuideInfo&)` | hook | same, after constants/dispatch | B5, C5 | per-frame | Commit or invalidate history; replaces `DlssgPostConstants` and `XefgPostDispatch`. |
| `ext::fg::SetResourcePre(IFGFeature_Dx12*, Dx12Resource*& in, Dx12Resource& tmp) -> bool` | decision | `DLSSG_Dx12.cpp` and `XeFG_Dx12.cpp` `SetResource`, entry | B5, C8, C10 | per-resource | Guide matching for Depth/Velocity, HDR `sceneRaw` capture, UI tag drop under OptiHDR. false = skip as upstream would on its own `return false`. Merges `MatchGuide`. |
| `ext::fg::SetResourcePost(IFGFeature_Dx12*, ...)` | hook | same, after the copy | B5, C8 | per-resource | `hdrEncoded` flag copy; null the copy for encoded and Depth/Velocity. |
| `ext::fg::NormalizeFormat(DXGI_FORMAT) -> DXGI_FORMAT` | decision | `DLSSG_Dx12.cpp` and `XeFG_Dx12.cpp` `SetResource` (3 sites) | C8 | per-resource | R10G10B10A2 under OptiHDR. |
| `ext::fg::DlssgTagHudless(DLSSG_Dx12*, Dx12Resource&, sl::ResourceTag&)` | hook | `DLSSG_Dx12.cpp` `SetResource` tag | C8 | per-frame (HDR only) | Converts hud-less to PQ and tags `eOnlyValidNow`; deactivates FG on failure. |
| `ext::fg::XefgPrepareHudless(XeFG_Dx12*, ...) -> bool` | decision | `XeFG_Dx12.cpp` `SetResource` | C8 | per-frame (HDR only) | Immediate SDK copy path; failure deactivates FG. |
| `ext::fg::SubmitSC(IFGFeature_Dx12*, UINT idx) -> bool` | decision | `DLSSG_Dx12.cpp`, `XeFG_Dx12.cpp`, `FSRFG_Dx12.cpp` `Present` (3 sites) | C6, D5 | per-frame | Ordered SC list submit with monotonic `_scFenceValue`; false = `return false`. Net -10 lines per site upstream. Fence state in `Dx12Ext`. **S4:** the execute/signal half exists as `ext::gpu::ExecuteOrdered` (its `ext::gpu::OrderedFence {value, failed}` replaces `_scFenceValue`/`_scSubmissionFailed` inside `Dx12Ext`); the wrapper itself, `Dx12Ext` and the SC wait are S5. |
| `ext::fg::PrepareSC(IFGFeature_Dx12*, UINT idx) -> bool` | decision | `IFGFeature_Dx12.cpp` `GetSCCommandList` | C6, D5 | per-frame | Replaces the 7 inline early-outs/waits/null-checks; tied to `SubmitSC` (stale lists are discarded, not executed). |
| `ext::fg::ReleaseGuides(IFGFeature_Dx12*)` | hook | `DLSSG_Dx12.cpp` and `XeFG_Dx12.cpp` `ReleaseObjects` | B5 | teardown | Collect and retire guide snapshots (fence refs); intentional leak of unfinished copies at shutdown. |
| `ext::fg::XefgSelectFactor(XeFG_Dx12*, int current, int supported) -> int` | decision | `XeFG_Dx12.cpp` `Dispatch`, interpolation-count block | D3 | per-frame | Clamp, dynamic MFG, target capped by `FramerateLimit`. Behaviour change vs nightly: no longer writes `FGXeFGInterpolationCount` back to max. |
| `ext::fg::XefgFrameTime(XeFG_Dx12*, double inputFt) -> double` | decision | `XeFG_Dx12.cpp` `Dispatch` | D3, B5 | per-frame | FFXIV prefers paced time; others only when input is missing. |
| `ext::fg::XefgNoteFrameTime(XeFG_Dx12*, double ft, bool& reset)` | hook | `XeFG_Dx12.cpp` `Dispatch` | D3 | per-frame | Discards samples outside [0, 250] ms and feeds `XeFGPacing`. |
| `ext::fg::XefgBuildCamera(IFGFeature_Dx12*, ...) -> CameraPose` | decision | `XeFG_Dx12.cpp` `Dispatch` camera build | D3, B5 | per-frame | Validated view matrix with stationary fallback and non-mutating near/far (the matched B5 snapshot metadata must not be written). Alternative in (e). |
| `ext::fg::OnXefgProviderLoaded(HMODULE)` | hook | `XeFG_Proxy.h` after the provider module is bound | D3 | init, once | `XeFGUnlock::Apply`; runs before the first call into the provider. Failure leaves the provider untouched. |
| `ext::fg::AdjustConstants(FG_Constants&)` | hook | `Upscaler_Inputs_Dx11wDx12.cpp` `UpscaleStart` (constants) | C8 | per-frame | ORs `FG_Flags::Hdr` when OptiHDR is active. |
| `ext::fg::OnUpscaleStart(IFGFeature*, IFeature*)` | hook | `Upscaler_Inputs_Dx11wDx12.cpp` `UpscaleStart` | B5, D2, C5 | per-frame | `IFgMarkers::PresentationGuideDelay` into the guide ring, `IFgMarkers::OnUpscale`. Replaces `IFGFeature::SetPresentationGuideDelay` virtual. Cast to `IFGFeature_Dx12` is done once and cached. |
| `ext::fg::MarkersBeforePresent(uint32_t frameId)` | hook | `FG_Hooks.cpp` `FGPresent`, before `GetNewFrameToken` | D2, C5 | per-frame | `IFgMarkers::BeforePresent`; pointer test only when no game module. Raw-send function on the fork's Reflex path replaces the `bridgeMarkerCall` thread-local (SEAMS 5): no check in `hkNvAPI_D3D_SetLatencyMarker`. |
| `ext::fg::MarkersNextFrame(uint64_t frame)` | hook | `FG_Hooks.cpp` `FGPresent`, after `ReflexSleep` | D2, C5 | per-frame | `IFgMarkers::NextFrame`. |

Drop list for this group (SEAMS 5): E6 KCD2 waitable-object hunks, `External` early-returns, 64-bit `sleep` widening (18 `low_latency` files untouched), `IFGFeature.cpp` log tweak, `OnFgActivated` diagnostics.

## present

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::present::PlanDx12Presenter(HWND, bool skipFgSwapchainCreation) -> PresenterPlan{hdr, createFg}` | decision | `DxgiFactory_Hooks.cpp` `CreateSwapChain`, `CreateSwapChainForHwnd`; `DxgiFactory_WrappedCalls.cpp` same two (4 sites) | C8 | per-create | `Hdr10::Request(hwnd)` must stay side-effect free. Upstream ORs `plan.hdr` into its condition. S6 restores upstream's `activeFgInput != NvngxFG` term unless `git log -S` justifies its removal (SEAMS 5). |
| `ext::present::PatchPresenterDesc(DXGI_SWAP_CHAIN_DESC1&/DXGI_SWAP_CHAIN_DESC&, const PresenterPlan&)` | hook | same 4 sites | C8 | per-create | Sets R10G10B10A2. Overloads for both desc types. |
| `ext::present::HookFactoryComposition(IDXGIFactory2*)` | hook | `DxgiFactory_Hooks.cpp` `HookToFactory`, after `DetourTransactionCommit` | E5 | init, per factory | Vtable slot 24; ext opens its own transaction and owns `o_CreateSwapChainForComposition`; the hook function (62 lines) lives in `ext/present/CompositionSwapchain.cpp`. Only this hook is kept from E5. |
| `ext::present::ResolveCompositionHandle(HWND&) -> bool proceed` | decision | `wrapped_swapchain.cpp` `Present`, `Present1`; guarded by `if (_composition)` | E5 | per-frame (composition chains only) | One bool test elsewhere. |
| `ext::present::BeforeOverlay(IDXGISwapChain*, ID3D12CommandQueue*, bool isD3D11)` | hook | `wrapped_swapchain.cpp` `LocalPresent`, before `MenuOverlayDx::Present` | C11, C8 | per-frame (each real and generated frame) | General pre-overlay slot (SEAMS 5). HudlessEngine paste is the first user; one flag test when idle. |
| `ext::present::FrameInterval(uint32_t cap, bool fgActive) -> optional<ns>` | decision | `FrameLimit.cpp` `sleep` | D5, D1 | per-frame | NVIDIA FG cap counts generated frames; returns nullopt when cap is 0. The waitable-timer and clock fixes in the same file are `[UP]` (section a). Whole-file `[UP]` is the alternative (e). |

Inline (marked `// ext: E5`): `WrappedIDXGISwapChain4` `_composition` flag (ctor parameter `isComposition`, member, and `!_composition &&` in the two `OverrideVsync` conditions of `ResizeBuffers`/`ResizeBuffers1`). Not carried: E5 0x0-to-window resolve, `FinalizeSizedDesc`, "Overlay call!" log decorations, `_device2`, D6 (nightly `State::frameCount` is the epoch).

## restrack

Single Reset hook design (SEAMS 5): one upstream `hkReset`, installed when `persistentBindings` or `ext::restrack::WantsCommandListReset()`; late/ext install by `ext::restrack::EnsureHooks(ID3D12Device*)` in ext/gpu, which attaches the same `ResTrack_Dx12::hkReset` to `ResTrack_Dx12::o_Reset` through `ext::Access` (`o_Reset` became a class member; `friend struct ext::Access;` in `ResTrack_Dx12`). Upstream's own persistent-bindings attach in `HookCommandList` stays and is skipped (`ownReset`) when ext already attached; it now also requires `activeFgInput == Upscaler` before storing `o_Reset`, so a non-attached pointer is never stored. Replaces `HookLateNrQueue`; one `DetourDetach(o_Reset, hkReset)` at the existing places, reverse order of attach. The fork `ExecuteCommandLists` hook lives in `ext/gpu/QueueHook.cpp` (queue vtable[10], pass-through when `SubmissionOwnersActive()` is false), owned by ext/gpu and always detached, including from `ReleaseHooks` (fixes the oracle leak). Both hooks are installed lazily: nothing is hooked on behalf of subscribers until a feature registers an `ext::gpu::Owner` (see `gpu`), so with no subscriber the behaviour equals upstream. The inline seams are declared in `ext/gpu/Restrack.h` (included by `Seams.h`); `ext::restrack::EnsureHooks(ID3D12Device*)` is ext-internal (it replaces `HookLateNrQueue`): features call it after registering an owner and for devices ResTrack never hooked.

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::restrack::OnCommandListReset(ID3D12GraphicsCommandList*)` | inline-guarded | `ResTrack_dx12.cpp` `hkReset`, after a successful `o_Reset` | A1, C8 | per-cmdlist (many per frame, concurrent recording threads) | Calls the `reset` callback of every enabled `ext::gpu::Owner` (NR: `NotifyGpuReset`, HDR: `Hdr10::Reset`); callbacks must be lock-free or sharded. Guard: one relaxed load. `ResetBindingState` in the same hook is now guarded by `_bindingTrackingEnabled` (identical behaviour: no state can exist while it is false). |
| `ext::restrack::WantsCommandListReset() -> bool` | decision | `ResTrack_dx12.cpp` `ext::restrack::EnsureHooks` install condition | A1, C8 | init, per device | True while an enabled owner has a `reset` callback. Single `o_Reset` => single detach by the existing `o_Reset != nullptr` lines of `ReleaseDeviceHooks`/`ReleaseHooks`. |
| `ext::restrack::NoteUav(ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*)` | inline-guarded | `ResTrack_dx12.cpp` `hkCreateUnorderedAccessView` | A4 | per-resource (very hot, thousands per frame) | Relaxed-atomic-bool test in `Seams.h` before any call; never touches the resource unless armed. |
| `ext::restrack::NoteResourceCreated(const D3D12_RESOURCE_DESC*, REFIID, void* resource)` | inline-guarded | `D3D12_Hooks.cpp` tail of `hkCreateCommittedResource`, `hkCreatePlacedResource` (2 sites) | A4 | per-resource (any thread, bursts of thousands) | Keeps upstream's `auto hr = o_...; note; return hr;` shape. Checks `riid` and non-null before casting (the oracle cast blindly). Lock-free on the miss path. |
| `ext::restrack::WantsResourceCreateHooks() -> bool` | decision | `D3D12_Hooks.cpp` `HookToDevice` install block | A4 | init, per device | Hoisted into its own `if` so the spoof-only hooks stay gated by the spoof flag alone. |
| `ext::restrack::OnDeviceHooked(ID3D12Device*)` | hook | `ResTrack_dx12.cpp` `HookDevice`, last statement | A1, C8, E4, C6 | init, per device | `EnsureHooks` when an owner is active; no-op otherwise. Mutexed. |
| `ext::restrack::ReleaseQueueHook()` | hook | `ResTrack_dx12.cpp` `ReleaseDeviceHooks`, `ReleaseHooks` | A1, C8 | teardown | Own transaction before upstream's; the only detach of the fork queue detour. |
| `ext::restrack::OnFeatureRelease()` | hook | `NVNGX_DLSS_Dx12.cpp` `NVSDK_NGX_D3D12_ReleaseFeature`, before freeing resources | A4 | per-create (feature release) | Releases the pinned scan resources (Cyberpunk device-removal fix); must run before any teardown. |

## streamline

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::streamline::OnLoadStreamline()` | hook | `Streamline_Proxy.h` `LoadStreamline`, first statement | D4 | per-create (each call, guarded inside) | Ampere MFG loader setup; must precede the SL module load and the `_dll != nullptr` early-out. |
| `ext::streamline::MfgUnlockTick()` | hook | `Streamline_Hooks.cpp` `hkslDLSSGSetOptions`, `hkslDLSSGGetState` | D1, D4 | per-frame | `TryApply()`; atomic load when idle. |
| `ext::streamline::MfgCeilingPending(int currentMax) -> bool` | decision | same two functions, populate-once condition (duplicated upstream block, 2 sites) | D1, D4 | per-frame | True while an unlock raised the ceiling after the first populate. |
| `ext::streamline::RaiseMfgCeiling(uint32_t& numFramesToGenerateMax)` | hook | `hkslDLSSGGetState` after each `o_slDLSSGGetState` read; `hkslDLSSGSetOptions` (3 sites) | D1, D4 | per-frame | Must stay inside the struct v2 branch (v1 ends before the field). |
| `ext::streamline::OnDlssgModuleLoad(const std::filesystem::path&, LPCWSTR fullPath) -> HMODULE` | decision | `LibraryLoad_Hooks.cpp` `LoadLibraryCheckW`, `nvngx_dlssg.dll` branch | D1, D4 | per-create (DLL load, any thread) | nullptr = not handled. Loads through `NtdllProxy::LoadLibraryExW_Ldr` and applies `MfgUnlock::TryApply`; early-out is a filename compare plus `Watching()`. |
| `ext::streamline::ApplyMenuDlssgInterlock(sl::DLSSGOptions&, bool active)` | hook | `Streamline_Hooks.cpp` `hkslDLSSGSetOptions` (the `StreamlineHooks::applyMenuDlssgInterlock` function and its `.h` declaration move to ext) | B9, D5 | per-frame (menu open, Vulkan) | Forces DLSS-G off with `eRetainResourcesWhenOff` while the menu is visible. `delayMenuRenderBy` is dropped. Depends on `menu::OnVulkanOverlay` unless dropped (see menu). |

## gpu

C6 submission ownership (`GpuLifetime`, `GpuSubmission`, `ext/gpu/`). The wrappers replace every `Hdr10::ResetCommands/ExecuteCommands`; they are inline in `ext/gpu/Gpu.h` (included by `Seams.h`) and cost one relaxed load while no owner is registered.

**Owner registry.** A feature that keeps GPU objects alive across submissions (NR, the HDR conversion pool, the AMD PreSR bridge) calls `ext::gpu::RegisterOwner(const Owner&)` once at init and then `ext::restrack::EnsureHooks(device)`. `Owner` is a table of optional function pointers: `begin` (capture a `GpuSubmission` before the real execute), `quarantine` (failure path of `begin`), `reset` (a list was Reset), `isolate` (index of a list that must run alone, AMD PreSR pending list), `before`/`after` (around each actual execute). Slots are never recycled, `SetOwnerEnabled(handle, bool)` toggles. `SubmissionOwnersActive()` (inline, feature bit `F::Gpu`) and `OwnersWantReset()` are the fast-path tests. The detour and `ExecuteCommands` run every owner in registration order: all `before`, all `begin`, execute, all `after`, all `CompleteNoThrow` (the reference build's AMD Submitting / NR / HDR / AMD Submitted order). A throwing `begin` calls that owner's `quarantine` and the submission proceeds. When the detour covers the queue, `ExecuteCommands` only executes (the detour does the owners' work), so tokens are not doubled.

Ext-internal (not seams): `ext::gpu::Scope` (RAII over a bridge-owned recording: `Reset` through the owners, `Submit(queue)` = Close + `ExecuteCommands`, an abandoned scope closes the list; the SEAMS 7 item 1 line for the recorded-Init block), `ext::gpu::DrainQueue(queue)` (5 s fence drain, device-removal tolerant; was `WaitForFfxivReleaseQueue`), `ext::gpu::ExecuteOrdered(queue, list, fence, OrderedFence&, UINT64& allocatorValue)` (the C6 half of `ext::fg::SubmitSC`: execute, `++value`, signal, `failed` on a failed signal).

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::gpu::ResetCommands(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*) -> HRESULT` | decision | `IFGFeature_Dx12.cpp` `GetUICommandList`; `IFeature_Dx11wDx12.cpp` `ProcessDx11Textures`; `dx11_with_dx12_sc.cpp` `_CopyDx11SharedToDx12FGBackBuffer`; `DLSSG_Dx12.cpp`/`XeFG_Dx12.cpp` where present | C6, C8 | per-frame | `Reset`, then every owner's `reset` callback (the oracle's `Hdr10::Reset`: `ResetRecording` + pool collect). Inline, owner callbacks run on the calling thread. Name loses `Hdr10`. **Placed (S4):** `IFeature_Dx11wDx12.cpp` `ProcessDx11Textures` and `Init`. **Open (S5/S6):** `IFGFeature_Dx12.cpp` `GetUICommandList` (FG, U2 row 6), `dx11_with_dx12_sc.cpp` copy (C8, `_hdrOutput` only). |
| `ext::gpu::ExecuteCommands(ID3D12CommandQueue*, UINT n, ID3D12CommandList* const*)` | decision | `IFGFeature_Dx12.cpp` `SubmitUICommandList`; `DLSSG_Dx12.cpp` `Present`; `XeFG_Dx12.cpp` `Present`, `Deactivate`; `IFeature_Dx11wDx12.cpp` `Evaluate`; `dx11_with_dx12_sc.cpp` copy execute | C6, C8 | per-frame | Runs the owners around the execute (see above); the HDR pool's `begin` marks conversion packets submitted so they recycle. **Placed (S4):** `IFeature_Dx11wDx12.cpp` `Evaluate` and `Init`. **Open (S5/S6):** `IFGFeature_Dx12.cpp` `SubmitUICommandList`, `DLSSG_Dx12.cpp` `Present`, `XeFG_Dx12.cpp` `Present` + `Deactivate` (U2 rows 5/8/16, owner C8), `dx11_with_dx12_sc.cpp` copy execute (C8), `UiPaste` (C11, not in this repo yet). |

## hdr

OptiHDR (C8), HDR PNG + ICC (C9), RenoDX scene HDR (C10, NOTICE shipped), F3, F4.

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::hdr::Active() -> bool` | inline-guarded | `XeFG_Dx12.cpp` `Present` (2 guards); also used inside ext | C8 | per-frame | Reads nightly's `hdrOutputActive`/`outputColorSpace` plus the OptiHDR-enabled bit. Replaces `Hdr10::Active()` and `isHdrActive`. |
| `ext::hdr::BridgeInitScope` (RAII) | RAII | `IFeature_Dx11wDx12.cpp` `Init` around `dx12Feature->Init` (inside the `[UP]` recorded-Init block) | C10 | per-create | Sets `DlssSceneInput::bridgeInitialising` so `InitDLSS` sets IsHDR/AutoExposure. Render thread only. |
| `ext::hdr::BridgeEvalScope` (RAII) | RAII | `IFeature_Dx11wDx12.cpp` `Evaluate` around `dx12Feature->Evaluate` | C10 | per-frame | Publishes and clears `bridgeColour/bridgeOutput/bridgeColourState`; strictly scoped. |
| `ext::hdr::AdjustDlssCreateFlags(params, bool& sceneHdr) -> void` | decision | `DLSSFeature_Dx12.cpp` `InitDLSS` | C10 | per-create | Forces `IsHDR | AutoExposure` when bridge-initialising with FfxivHDR mode 1 and `DLSSSceneInput`; keeps the original flags in `_ext`. |
| `ext::hdr::RestoreDlssCreateFlags(params)` | hook | `DLSSFeature_Dx12.cpp` `InitDLSS`, after `CreateFeature` | C10 | per-create | |
| `ext::hdr::DlssEvalPrepare(DLSSFeature_Dx12*, device, cmd, params, w, h) -> bool` | decision | `DLSSFeature_Dx12.cpp` `EvaluateInternal`, before the native evaluate | C10 | per-frame | `ISceneHdr::Open` + `DlssSceneInput::Prepare`; substitutes colour/output/exposure; on failure disables the option and returns false (upstream returns false). |
| `ext::hdr::DlssEvalFinish(DLSSFeature_Dx12*, cmd, bool ok)` | hook | same, after | C10 | per-frame | Restores bindings and `Reset`, `DlssSceneInput::Finish`, `ISceneHdr::PublishReconstructed`. |
| `ext::hdr::OverlayRtvFormat(DXGI_FORMAT) -> DXGI_FORMAT` | decision | `menu_overlay_dx.cpp` overlay init, RTV format | C8 | init | R8G8B8A8 under OptiHDR so the menu is composited into an SDR target, then converted to PQ. |
| `ext::hdr::OverlayPrepare(...) -> bool draw` | decision | `menu_overlay_dx.cpp` per-frame prepare | C8 | per-frame | Computes bounds, ensures the layer, waits for retire on resize, false = skip (never draw SDR into HDR). |
| `ext::hdr::OverlayScope` (RAII) | RAII | `menu_overlay_dx.cpp` around `ImGui_ImplDX12_RenderDrawData` | C8 | per-frame | Begin/End of the HDR layer; the fence-ring `++g_overlaySubmissionCount` stays in the `[UP]` ring code at the same point. |
| `ext::hdr::OverlayRelease()` | hook | `menu_overlay_dx.cpp` `CleanupRenderTargetDx12` | C8 | teardown | |

Contingent (not counted; only if in-game Phase 4 shows the SDR-target composite is not visually equivalent, SEAMS 5): `ext::hdr::MenuHdrFlag(DXGI_FORMAT rtv) -> float` and `ext::hdr::MenuPixelShader() -> const char*` in `include/imgui/imgui_impl_dx12.cpp` (+ ~12 lines in the vendored file: root param, constant, shader replacement).

## hudless

`HudlessEngine` (shared-handle ring, UI-free decision, tag, paste produce) and `HudlessSource` (`Active()`, `Take() -> ref-counted texture, newest wins`). The ReShade add-on, `.fx` marker, `StatusV1`, `OptiScalerHudless.addon64` probe and `FrameGen.ExternalHudless` are dropped. Recomposition depends on `HudlessEngine::SourceActive()`.

**This group adds no upstream seam of its own.** Its entry points are reached through seams in other groups, so the conflict surface is already listed there:

| Engine point | Reached through | Notes |
|---|---|---|
| P1 take and stage | `ext::dx11bridge::PreInteropCopy` | D3D11 immediate context, before the interop signal |
| P2 UI-free decision | `ext::dx11bridge::UiFreeFrame` | CPU only |
| P3 FG-input swap | `ext::dx11bridge::SelectCopySource` | interop copy list |
| P4 tag, P5 paste produce | `ext::dx11bridge::PostInteropCopy` | FG UI list, FG queue |
| P6 paste draw | `ext::present::BeforeOverlay` | per output frame |
| P7 DLSS-G options | `ext::fg::AdjustDlssgOptions` | at creation |
| P8 teardown | `ext::dx11bridge::OnReleaseInterop` | resize and teardown |
| Source P0 | `ext/ffxiv/FfxivUiBoundary` (native capture), implements `HudlessSource` | no upstream hook |

## game

`ext::game::Current()` returns a struct of interface pointers selected once at init (null for every game except a matched FFXIV build). Seam bodies test a pointer, never a `gameExe` string. `ext/ffxiv/GameBuild.h` is the only file with FFXIV addresses; the offsets hard-coded inside `ProcessFfxivQualityRequest` move there too.

Upstream seams in this group:

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::game::OnD3D11DeviceHooked(ID3D11Device*)` | hook | `D3D11_Hooks.cpp` `HookToDeviceLocal`, after the null check and before the `o_CreateSamplerState` early return | B6, C7 | init, per D3D11 device | Must be idempotent (the early return only covers the first device). Dispatches to the FFXIV module (lighting capture install, Companion native install). |
| `ext::game::OnD3D11Unhook()` | hook | `D3D11_Hooks.cpp` `D3D11Hooks::Unhook`, before `ResTrack_Dx11::ReleaseHooks()` | B6, C7 | teardown | Ext detaches first (it hooks COM vtable slots installed after ResTrack_Dx11's); same-target detours are LIFO, S6 confirms no shared target. |

Interface contracts (implemented only by `ext/ffxiv/FfxivModule`; consumed by seams elsewhere, not hosted in upstream files):

| Interface | Methods | Consumed by seam | Owner IDs |
|---|---|---|---|
| `IRenderGeometry` | `ResolveRenderExtent`, `ColorIsDisplayAllocation` | `upscaler::ResolveRenderGeometry`, `dx11bridge::PrepareUpscalerColor` | C2, C3, C4 |
| `INativeQuality` | `EnsureHooks` (lazy, `call_once`), `Live`, `DlaaRequired` (per-frame cached; the oracle does two `ReadProcessMemory` per call), `Request(preset)` | `upscaler::ApplyForcedQuality`, `ResolveRenderGeometry`, `EffectivePerfQuality`, menu card `forced_quality` | C1, C5 |
| `ISceneLighting` | `OnUpscaleBoundary`, `ConsumeHistoryReset(uint64_t& cursor)` | `dx11bridge::OnUpscaleBoundary`, `fg::DispatchResetHistory`; NR (A3, PreSR) calls `ConsumeHistoryReset` directly | B6, A3 |
| `INrQuirks` | `BridgedNativeDx11` | NR internals: history reset on disable, split frame work, motion-onset sampling, exposure scan off (was `gameExe == "ffxiv_dx11.exe"`) | A1-A8, B5 |
| `ISceneHdr` | `Open`, `PublishReconstructed` | `hdr::DlssEvalPrepare/Finish`, `dx11bridge::SelectCopySource` | C10 |
| `IFgMarkers` | `PresentationGuideDelay`, `OnUpscale`, `BeforePresent`, `NextFrame` | `fg::OnUpscaleStart`, `GuideAge`, `MarkersBeforePresent`, `MarkersNextFrame` | B5, D2, C5 |
| `INgxLifecycle` | `IsolatesShutdown` | `dx11bridge::ShutdownNgx`, `RedirectShutdown1`, `BeforeReleaseFeature` | C6 |

## core

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::core::HooksBypassed() -> bool` | inline-guarded (`thread_local`) | `D3D11_Hooks.cpp` `hkD3D11CreateDevice`, `hkCreateSamplerState`; `Dxgi_Hooks.cpp` `hkCreateDXGIFactory`, `...1`, `...2`; `FG_Hooks.cpp` `hkFGPresent`, `hkFGPresent1` (7 sites) | C7 | per-frame (FG Present), per-create (device, factory), per-resource (sampler) | Replaces the six `FfxivNameplateLiveScope::active` guards. The Dxgi sites extend the existing Luma bypass condition (`creatingD3D12DeviceForLuma || HooksBypassed()`). Fork code that issues its own native draws wraps them in `ext::core::BypassScope`. |
| `ext::core::Banner()` | hook | `dllmain.cpp` once, after upstream's version line | INFRA | init | Logs `OptiScaler-FFXIV <tag> (<fork sha>) on upstream <upstream sha>`. Upstream's version line is not edited. |

Not seams: `ext::core::DetourLock` (shared mutex for the fork's own detour transactions; see (e) for sharing with upstream), `ExtConfig`/ini binding, `MenuRegistry` and widgets, build info.

## ffxiv-in-upstream

No seam names FFXIV. Everything that must stay FFXIV-visible in an upstream file is on the existing quirk extension point or the `printQuirks` convention, and is marked `// ext:`.

| Item | Form | Host file(s) | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ffxiv_dx11.exe` entry gains `.quirks = { ScaleDisplayColorForForcedQuality, ReleaseBackbufferOnce }` | upstream-ext (GameConfigs quirks table) | `misc/GameConfigs.cpp` | C4, D3, C6 | init | 1 line. `OverrideVsync=false` is upstream. |
| `GameQuirk::ScaleDisplayColorForForcedQuality`, `GameQuirk::ReleaseBackbufferOnce` | enum values, appended at the end of the enum (SEAMS 5; no shift of upstream values) | `misc/Quirks.h` | C4, D3 | compile-time | 2 lines. The second replaces the U4 string compare in `hkFGRelease`: the test becomes a quirk flag (`FG_Hooks.cpp`, ~5 lines inline) instead of a game interface. |
| `printQuirks` entries for both quirks | inline | `dllmain.cpp` | C4 | init | 4 lines (upstream convention: "add the new entry to printQuirks"). |

**Carried (S2 check, SEAMS 5):** `Kernel_Hooks.cpp` `!path.contains("nvngx.dll_")` in `hk_K32_GetFileAttributesW`/`hk_K32_CreateFileW` (+7 lines, `// ext: A1`). The forwarder keeps the name `nvngx.dll_dlssnr.dll` because NVIDIA's model checks that its caller's module path contains `nvngx.dll`; the nvngx spoof must therefore leave `nvngx.dll_` paths alone. Placed by S2b.

## Summary of seam counts

| Group | Seams | Of which inline-guarded / RAII |
|---|---|---|
| config | 3 | 0 |
| menu | 13 (1 provisional) | 0 |
| upscaler | 5 | 0 |
| dx11bridge | 21 | 2 RAII |
| nr | 10 | 1 RAII |
| fg | 26 | 0 |
| present | 6 | 0 |
| restrack | 8 | 3 inline-guarded |
| streamline | 6 | 0 |
| gpu | 2 | 0 |
| hdr | 11 (+2 contingent) | 1 inline-guarded, 3 RAII |
| hudless | 0 | 0 |
| game | 2 (+6 interface contracts) | 0 |
| core | 2 | 1 inline-guarded |
| ffxiv-in-upstream | 0 (3 inline items) | 0 |
| **Total** | **115** | |

# (a) `[UP]` fix list

Generic upstream bug fixes, each carried as a separate self-contained commit at the bottom of the stack (series S1), written as an upstream PR. SEAMS 5 says: unless the U6 check shows nightly already has it. Items marked "+" are identified by a ledger but not enumerated in SEAMS 5 line 160; S1 may downgrade them to `// ext:` inline.

| # | Fix | Upstream file(s) | Ledger reference | Approx. lines |
|---|---|---|---|---|
| 1 | Anisotropy: read `ModifyComparison`/`ModifyMinMax`, fall back to legacy `AFModify*` (upstream writes the new names and reads the old: round-trip bug) | `Config.cpp` | U1 Config.cpp hunk 8 | 6 |
| 2 | `FrameLimit` timer: `steady_clock` timestamp, `thread_local` waitable timer (RAII), `previous_frame_time` thread_local, early reset when cap is 0 | `misc/FrameLimit.cpp` | U1 FrameLimit hunks 1, 2 (non-interval part) | 20 |
| 3 | Vulkan spoofing DXVK/Proton re-entry: vendor and `dlssCapable` from `vkGetPhysicalDeviceProperties` of the device being created, no DXGI enumeration in `vkCreateInstance` | `spoofing/Vulkan_Spoofing.cpp` | U1 Vulkan_Spoofing hunks 1-4 | 53 |
| 4 | `PFN_D3D11_Shutdown1` typedef gains `unsigned int* OutRemainingInstances` and the non-FFXIV call passes it | `proxies/NVNGX_Proxy.h`, `inputs/NVNGX_DLSS_Dx11.cpp` | U1 NVNGX_Proxy; U3 NVNGX_DLSS_Dx11 H5b | 6 |
| 5 | D5 `ConcurrentInputSelection`: `activeInput`/`activeOutput`/`avaliableInputs` become a synchronized proxy type at their declaration (the ~44 call sites in `input_common.cpp` stay unchanged); tech mutex taken at the top of `update_low_latency_tech` with a lock-free read first; `alignas(16)` on the two `atomic<shared_ptr>` | `low_latency/input/input_common.h`, `input_common.cpp`, new `ConcurrentInputSelection.h` | U2 input_common.h/.cpp; SEAMS 5 | ~140 (new header 131; declarations and 3 `enum_name` call sites in `.cpp`) |
| 6 | `WaitForUIAllocator`: bounds check, missing fence is not "completed", `UINT64_MAX` (device removed) is failure | `framegen/IFGFeature_Dx12.cpp` | U2 IFGFeature_Dx12 #3 | 9 |
| 7 | HUD-less format-change loop: remember the new format and log once instead of resetting FG forever | `framegen/dlssg/DLSSG_Dx12.cpp` | U2 DLSSG_Dx12 #13 | 9 |
| 8 | XeFG copy-source barrier: use `incomingState`/`pResource` and `fResource->cmdList`, symmetrical restore | `framegen/xefg/XeFG_Dx12.cpp` | U2 XeFG_Dx12 #23 | 10 |
| 9 | XeFG `return false` after a failed `Deactivate` in `Dispatch` | `framegen/xefg/XeFG_Dx12.cpp` | U2 XeFG_Dx12 #12 | 1 |
| 10 | A11: `uavCount` is `D3D11_PS_CS_UAV_REGISTER_COUNT` on feature level < 11.1 (device error on slots > 8) | `upscalers/IFeature_Dx11.cpp` | U3 IFeature_Dx11 H1, H2 | 3 |
| 11 | A11: skip output-scaling/RCAS and ImGui after a failed upscale; restore `Output` unconditionally | `upscalers/IFeature_Dx11.cpp` | U3 IFeature_Dx11 H3, H4, H5a | 6 |
| 12 | A11: null the UAVs first when restoring compute-stage bindings (the batched-restore refactor is not carried) | `upscalers/IFeature_Dx11.cpp` | U3 IFeature_Dx11 H5b (reduced) | 6 |
| 13 | A11: Vulkan `RestoreOutput` RAII and `!paramDepth` in the RCAS early-out | `upscalers/IFeature_Vk.cpp` | U3 IFeature_Vk H1, H2 | 11 |
| 14 | Recorded Init for DLSS-on-12: reset allocator and list, run `dx12Feature->Init`, close, execute, signal, wait (weights upload was recorded on a closed list: posterised picture). Contains one `ext::hdr::BridgeInitScope` line added by the C10 series | `upscalers/IFeature_Dx11wDx12.cpp` | U3 IFeature_Dx11wDx12 H7a; SEAMS 5 | 65 |
| 15 | Same recorded Init for the Vulkan-on-D3D12 bridge, local fence | `upscalers/IFeature_VkwDx12.cpp` | U3 IFeature_VkwDx12 H2 | 67 |
| 16 | Per-API NGX init flags for DLSS SR (`_dlssInitedDx11/Dx12/Vk`; the single flag skipped `InitDx12` after a native-D3D11 init, Bannerlord) | `upscalers/dlss/DLSSFeature.h`, `DLSSFeature_Dx11.cpp`, `DLSSFeature_Dx12.cpp`, `DLSSFeature_Vk.cpp` | U3 A11 NGX init flag | 23 |
| 17 | Per-API NGX init flags for DLSS-D | `upscalers/dlssd/DLSSDFeature.h`, `DLSSDFeature_Dx11.cpp`, `_Dx12.cpp`, `_Vk.cpp` | U3 A11 NGX init flag | 23 |
| 18 | + Bridge `ProcessDx11Textures`: abort when the fence reports device removed or the allocator is still in flight before `Reset()` | `upscalers/IFeature_Dx11wDx12.cpp` | U3 IFeature_Dx11wDx12 H4, H5 | 13 |
| 19 | + Bridge interop copy fence hardening: `_copySubmissionFailed`, missing fence/event and `UINT64_MAX` detection in `_WaitForCopyAllocator`/`_WaitForCopyQueueIdle`/`_WaitForInteropCopyOnPresentQueue`, leave COM refs alive when the queue cannot be proven idle | `with_dx12/dx11_with_dx12_sc.cpp`, `.h` | U5 sc.cpp rows 17-19, 23-27, 29; sc.h H3 | 44 |
| 20 | Overlay fence ring: fence-based ring of allocators and ImGui upload buffers, recursive mutex, `ScopedResize`, try-lock Present (device-lost on busy GPUs with generated frames). Stats logging not carried. The two `ScopedResize` call lines in `dx11_with_dx12_sc.cpp` ship in this commit | `menu/menu_overlay_dx.cpp`, `menu_overlay_dx.h`, `with_dx12/dx11_with_dx12_sc.cpp` | U5 menu_overlay_dx rows 2-5, 7, 8, 10-15; commit `8cabe038` | 191 |
| 21 | Null `cmdList` guard in `HC_Dx12::Dispatch` and `RUI_Dx12::Dispatch` | `shaders/hudless_compare/HC_Dx12.cpp`, `shaders/render_ui/RUI_Dx12.cpp` | U5 HC1, RUI1 | 4 |
| 22 | D5 Reflex: copy the game's `NV_SET_SLEEP_MODE_PARAMS` into `effectiveParams` instead of overwriting `minimumIntervalUs` (DX and Vulkan) | `hooks/Reflex_Hooks.cpp` | U4 Reflex #1, #3 | 12 |
| 23 | D5 Reflex limiter: fall through the transition reset when markers stop; reset when the sleep device changes | `hooks/Reflex_Hooks.cpp` | U4 Reflex #5, #6 | 13 |
| 24 | D5 Streamline: one Detours transaction mutex around `Begin..Commit` in the hooking functions | `hooks/Streamline_Hooks.cpp` | U4 Streamline #2, #10, #12, #14-18 | 9 |
| 25 | D5 Streamline: `eOk` guards on `slDLSSGGetState` results, clamp via `set_volatile_value` instead of overwriting the ini value | `hooks/Streamline_Hooks.cpp` | U4 Streamline #3(c), #6, #8 | 8 |
| 26 | Vulkan DLSS-G menu interlock: while the menu is visible on a Vulkan swapchain/overlay, force DLSS-G `eOff` + `eRetainResourcesWhenOff` and frame count 0 (`State::menuOverlayIsVulkan` set in `menu_overlay_vk.cpp`). Not covered by nightly; dead `delayMenuRenderBy` not carried | `hooks/Streamline_Hooks.cpp`, `State.h`, `menu/menu_overlay_vk.cpp` | U4 Streamline; SEAMS 7 item 9; S1a | 24 |

Count: 26 rows (row 26 added by S1a; #16 skipped in S1b because nightly's DLSS-SR flag is per instance) (items 1-17 and 21-25 are enumerated or implied by SEAMS 5 line 160; 18-19 are additions). Approx. lines: 718 in total; the footprint table in (d) uses these numbers (rounded to ~715).

# (b) `ext::Access` needs

One `friend struct ext::Access;` per class. Members listed are what the fork needs; `Access` exposes narrow static accessors only.

| Upstream class (file) | Private/protected members the fork needs | Used by |
|---|---|---|
| `IFGFeature_Dx12` (`framegen/IFGFeature_Dx12.h`); covers derived `DLSSG_Dx12` and `XeFG_Dx12` through base access | `_uiCommandList`, `_scCommandList`, `_resourceMutex`, UI/SC allocators and fences, frame-resource arrays, `_jitterX/_jitterY`, `_cameraNear/_cameraFar`, `_lastDispatchedFrame`, `_waitingNewFrameData`, `Deactivate()`, `ReleaseObjects()` | `fg::*`, `CancelPendingUpscalerWork`, `SubmitSC`, `PrepareSC`, guide matcher |
| `XeFG_Dx12` (`framegen/xefg/XeFG_Dx12.h`) | `_reset`, XeFG context/session handles, `WAR_xefgRequestFGToggle`, frame-index state | `XefgSelectFactor`, `XefgFrameTime`, `XefgPrepareHudless`, rebuild-on-upscaler-change |
| `Dx11wDx12SC` (`with_dx12/dx11_with_dx12_sc.h`) | `_fgSwapChain`, `_currentFakeIndex`, `_openedDx11BackBuffers`, copy queue/allocators/fences, D3D11 immediate context, `_hdrOutput` (moves to `_ext`), `_RequestSharedBackBuffer` shape | `dx11bridge::*`, `HudlessEngine` |
| `IFeature_Dx11wDx12` (`upscalers/IFeature_Dx11wDx12.h`) | `dx12Feature`, `_dx11on12Device`, `Dx12CommandQueue`, `Dx12CommandList`, `Dx12Fence`, `ResourceBarrier`, `_frameCount` | `PrepareUpscalerColor`, `BridgeEvalScope`, `BeforeReleaseFeature` |
| `IFeature` (`upscalers/IFeature.h`) | `_renderWidth/_renderHeight`, `_displayWidth/_displayHeight`, `Handle()`/`Id`, create-flag accessors | `CorrectRenderSubrect`, `ResolveRenderGeometry`, `ApplyForcedQuality` |
| `MenuCommon` (`menu/menu_common.h`) | nested `RenderMenuContext`, `MenuBox`, `ScopedCard`, `RenderCard`, `Keybind`, TU-local `ShowTooltip`/`SectionTitle` equivalents (via `ext/core/MenuWidgets`; upstream private helpers are not exposed) | `MergeBoxes`, `ext::menu::Ctx` construction, `PollShortcuts`, `KeybindRows` |
| `DLSSG_Common` (`framegen/dlssg/DLSSG_Common.h`) | (only if option storage is private) the current options struct and `_maxInterpolationCount` read-only | `AdjustDlssgOptions`, `AdjustMfgMax` |
| `ResTrack_Dx12` (`resource_tracking/ResTrack_dx12.h`) | `o_Reset`, `_persistentBindings` read-only (for `EnsureResetHook`) | `restrack` install condition and late entry |

No friend line: `Config` (public `CSimpleIni`/`CustomOptional`), `State` (public struct), `WrappedIDXGISwapChain4` (`_composition` is a plain inline field), `OS_Dx12`/`OS_Vk` (inline edits), `IFeature_VkwDx12` (`[UP]`), `DLSSFeature_Dx12` (uses `_ext`).

# (c) Per-object ext state (`_ext`)

One member each, marked `// ext:`, `std::unique_ptr<ext::.../...Ext>` with a forward declaration (1 include-free line plus 1 member line).

| Upstream class (file) | `_ext` type | Holds |
|---|---|---|
| `IFGFeature_Dx12` (`framegen/IFGFeature_Dx12.h`) | `ext::fg::Dx12Ext` | guide snapshot ring and metadata, `_guideAge`, `_retiredGuides`, `_sceneHistory`, SC fence value/failed flag, DLSS-G observed multiplier/sample mode/last options, `_historyFrames`, `_forceConstantsReset`, XeFG dynamic state (`_dynamic`, `_dynamicWasEnabled`, `_dynamicLastTarget`). Covers `DLSSG_Dx12`, `XeFG_Dx12`, `FSRFG_Dx12` (derived), so no member is added in those classes or in `DLSSG_Common`. |
| `Dx11wDx12SC` (`with_dx12/dx11_with_dx12_sc.h`) | `ext::dx11bridge::BridgeExt` | HudlessEngine ring and flags (`_hudlessPending`, `_hudlessSlot`, `_uiFreeThisFrame`, UI image ring, `_uiExtract`), `_hdrOutput` |
| `IFeature_Dx11wDx12` (`upscalers/IFeature_Dx11wDx12.h`) | `ext::dx11bridge::BridgeState` | `ForcedQualityColorScaler` (`OS_Dx12`), `ReportedForcedQualityColorScale`; freed on the GPU-idle path (C6) |
| `DLSSFeature_Dx12` (`upscalers/dlss/DLSSFeature_Dx12.h`) | `ext::hdr::DlssExt` | `_sceneHdrInput`, `_sceneHdrAvailable`, saved create flags |

Single-field exceptions kept inline (hot path, per resource or per present, marked `// ext:`): `Dx12Resource::hdrEncoded` (`IFGFeature_Dx12.h`), `Dx11wDx12SC::_copySubmissionFailed` (in the `[UP]` fence commit), `WrappedIDXGISwapChain4::_composition`.

# (d) Projected footprint

Basis: added plus removed lines, after the port, per upstream file. "Seam" = seam calls and includes (one `#include "ext/Seams.h"` per file); "Fr/ext" = friend/`_ext`/forward-decl lines; "Inline" = `// ext:`-marked inline changes; "[UP]" = lines in the section (a) commits. Estimates; each seam-replaced block counts its removed lines (U2/U4/U5 ledger residuals understate this).

| Upstream file | Seam | Fr/ext | Inline | [UP] | Total |
|---|---|---|---|---|---|
| `OptiScaler.vcxproj` (one `Import`) | - | - | 2 | - | 2 |
| `OptiScaler.sln` (forwarder project and mappings) | - | - | 8 | - | 8 |
| `.gitignore` | - | - | 6 | - | 6 |
| `Config.cpp` | 4 | - | - | 6 | 10 |
| `Util.cpp` | 2 | - | - | - | 2 |
| `dllmain.cpp` | 2 | - | 4 | - | 6 |
| `NVNGX_Parameter.cpp` | 7 | - | - | - | 7 |
| `misc/FrameLimit.cpp` | 8 | - | - | 20 | 28 |
| `misc/GameConfigs.cpp` | - | - | 1 | - | 1 |
| `misc/Quirks.h` | - | - | 2 | - | 2 |
| `nvapi/NvApiHooks.cpp` | 2 | - | - | - | 2 |
| `proxies/NVNGX_Proxy.h` | - | - | - | 3 | 3 |
| `proxies/Streamline_Proxy.h` | 3 | - | - | - | 3 |
| `proxies/XeFG_Proxy.h` | 2 | - | - | - | 2 |
| `spoofing/Vulkan_Spoofing.cpp` | - | - | - | 53 | 53 |
| `framegen/IFGFeature_Dx12.cpp` | 9 | - | - | 9 | 18 |
| `framegen/IFGFeature_Dx12.h` | - | 3 | 1 | - | 4 |
| `framegen/dlssg/DLSSG_Common.cpp` | 9 | - | - | - | 9 |
| `framegen/dlssg/DLSSG_Dx12.cpp` | 29 | - | - | 9 | 38 |
| `framegen/ffx/FSRFG_Dx12.cpp` | 15 | - | - | - | 15 |
| `framegen/xefg/XeFG_Dx12.cpp` | 103 | - | - | 11 | 114 |
| `low_latency/input/input_common.h` | - | - | - | 30 | 30 |
| `low_latency/input/input_common.cpp` | - | - | - | 5 | 5 |
| `low_latency/input/ConcurrentInputSelection.h` (new) | - | - | - | 71 | 71 |
| `inputs/FG/Upscaler_Inputs_Dx11wDx12.cpp` | 4 | - | - | - | 4 |
| `inputs/NVNGX_DLSS_Dx11.cpp` | 16 | - | - | 3 | 19 |
| `inputs/NVNGX_DLSS_Dx12.cpp` | 6 | - | 2 | - | 8 |
| `inputs/NVNGX_DLSS_Vk.cpp` | 5 | - | - | - | 5 |
| `upscalers/IFeature.cpp` | 5 | - | - | - | 5 |
| `upscalers/IFeature.h` | - | 1 | - | - | 1 |
| `upscalers/IFeature_Dx11.cpp` | - | - | - | 15 | 15 |
| `upscalers/IFeature_Dx11wDx12.cpp` | 13 | - | 1 | 78 | 92 |
| `upscalers/IFeature_Dx11wDx12.h` | - | 3 | - | - | 3 |
| `upscalers/IFeature_Dx12.cpp` | 4 | - | 6 | - | 10 |
| `upscalers/IFeature_Dx12.h` | - | - | 2 | - | 2 |
| `upscalers/IFeature_Vk.cpp` | - | - | - | 11 | 11 |
| `upscalers/IFeature_VkwDx12.cpp` | 4 | - | - | 67 | 71 |
| `upscalers/dlss/DLSSFeature.cpp` | 4 | - | - | - | 4 |
| `upscalers/dlss/DLSSFeature.h` | - | - | - | 5 | 5 |
| `upscalers/dlss/DLSSFeature_Dx11.cpp` | - | - | - | 6 | 6 |
| `upscalers/dlss/DLSSFeature_Dx12.cpp` | 6 | - | - | 6 | 12 |
| `upscalers/dlss/DLSSFeature_Dx12.h` | - | 2 | - | - | 2 |
| `upscalers/dlss/DLSSFeature_Vk.cpp` | - | - | - | 6 | 6 |
| `upscalers/dlssd/DLSSDFeature.cpp` | 2 | - | - | - | 2 |
| `upscalers/dlssd/DLSSDFeature.h` | - | - | - | 5 | 5 |
| `upscalers/dlssd/DLSSDFeature_Dx11.cpp`, `_Dx12.cpp`, `_Vk.cpp` | - | - | - | 18 | 18 |
| `hooks/D3D11_Hooks.cpp` | 6 | - | - | - | 6 |
| `hooks/D3D12_Hooks.cpp` | 9 | - | 8 | - | 17 |
| `hooks/DxgiFactory_Hooks.cpp` | 20 | - | - | - | 20 |
| `hooks/DxgiFactory_WrappedCalls.cpp` | 14 | - | - | - | 14 |
| `hooks/Dxgi_Hooks.cpp` | 7 | - | - | - | 7 |
| `hooks/FG_Hooks.cpp` | 5 | - | 5 | - | 10 |
| `hooks/LibraryLoad_Hooks.cpp` | 2 | - | - | - | 2 |
| `hooks/Reflex_Hooks.cpp` | - | - | - | 25 | 25 |
| `hooks/Streamline_Hooks.cpp` | 13 | - | - | 17 | 30 |
| `hooks/Vulkan_Hooks.cpp` | 4 | - | - | - | 4 |
| `resource_tracking/ResTrack_dx12.cpp` | 8 | - | 12 | - | 20 |
| `resource_tracking/ResTrack_dx12.h` | - | - | 2 | - | 2 |
| `wrapped/wrapped_swapchain.cpp` | 6 | - | 6 | - | 12 |
| `wrapped/wrapped_swapchain.h` | - | - | 3 | - | 3 |
| `with_dx12/dx11_with_dx12_sc.cpp` | 33 | - | 2 | 40 | 75 |
| `with_dx12/dx11_with_dx12_sc.h` | - | 3 | - | 1 | 4 |
| `menu/menu_common.cpp` | 16 | - | - | - | 16 |
| `menu/menu_common.h` | - | 1 | - | - | 1 |
| `menu/menu_overlay_dx.cpp` | 7 | - | - | 178 | 185 |
| `menu/menu_overlay_dx.h` | - | - | - | 9 | 9 |
| `menu/menu_overlay_vk.cpp` | 2 | - | - | - | 2 |
| `shaders/hudless_compare/HC_Dx12.cpp`, `shaders/render_ui/RUI_Dx12.cpp` | - | - | - | 4 | 4 |
| `shaders/output_scaling/OS_Dx12.cpp`, `.h`, `OS_Vk.cpp`, `.h` (A8 scaler override and resource-size dispatch, gated on the override per SEAMS 5) | - | - | 122 | - | 122 |
| **Total** | **~412** | **13** | **~195** | **~715** | **~1,335** |

**Files touched:** 75 upstream files (including the new `ConcurrentInputSelection.h`, `.sln`, `.vcxproj` and `.gitignore`). Of these, 18 contain only `[UP]` lines (`Vulkan_Spoofing.cpp`, `NVNGX_Proxy.h`, `input_common.h/.cpp`, `ConcurrentInputSelection.h`, `IFeature_Dx11.cpp`, `IFeature_Vk.cpp`, `HC_Dx12.cpp`, `RUI_Dx12.cpp`, `menu_overlay_dx.h`, `Reflex_Hooks.cpp`, `DLSSFeature_Dx11.cpp`, `DLSSFeature_Vk.cpp`, `DLSSFeature.h`, `DLSSDFeature.h`, `DLSSDFeature_Dx11/_Dx12/_Vk.cpp`).

**Against the target (≤ 40 files, ≤ 1,000 lines): not met as written.** About 1,335 lines in 75 files with every `[UP]` commit counted (the footprint tool counts them). The persistent conflict surface that survives a rebase once the `[UP]` commits are merged upstream or dropped as empty is about 620 lines in about 57 files (the seam, friend, `_ext` and `// ext:` lines). Levers to reach the target, largest first:

| Lever | Lines | Files |
|---|---|---|
| Overlay fence ring accepted upstream (#20) | -191 | -1 |
| `OS_*` scaler override and sizing (A8): upstream or replace by ext-owned resampler | -122 | -4 |
| Recorded Init (#14, #15) accepted upstream | -132 | -1 (Vk only) |
| `ConcurrentInputSelection` (#5) accepted | -106 | -3 |
| Vulkan spoofing (#3) accepted | -53 | -1 |
| Bridge fence hardening (#18, #19) accepted | -57 | 0 |
| Per-API NGX init flags (#16, #17) accepted | -46 | -9 |
| `XefgBuildCamera` kept as `// ext:` fix instead of a seam (or dropped) | up to -30 | 0 |

Even with a normal acceptance rate the first pass lands above 1,000 lines; the file count stays above 40 unless the A8 and DLSS init-flag items go upstream. This needs a designer decision (section (e), item 11).

# (e) Unresolved conflicts between ledgers that SEAMS.md does not settle

1. **Recorded Init dependency on C6/C10.** SEAMS 5 makes it `[UP]`; U3 proposed an out-of-line `ext::dx11bridge::InitFeature` using `Hdr10::ResetCommands/ExecuteCommands`. A self-contained `[UP]` commit can only use raw `Reset`/`ExecuteCommandLists`; the C6 and C10 series then add inline `ext::gpu::*` and `ext::hdr::BridgeInitScope` lines inside the block (+4-6 lines). Decide whether Init needs GpuLifetime registration at all (U3 notes the Vulkan variant uses raw calls). The catalog assumes raw in `[UP]`, one RAII line from C10, no `ext::gpu` in Init.
2. **Bridge fence/allocator hardening scope.** U3 (`ProcessDx11Textures`, 13 lines) and U5 (`dx11_with_dx12_sc.cpp`, ~44 lines) call it generic and upstream-worthy; SEAMS 5 does not list it. Catalog: `[UP]` rows 18-19. Alternative: inline `// ext: C6`.
3. **Menu tonemap under OptiHDR.** U5 proposes `AdjustUiTargetMode` (return SDR while OptiHDR composites the menu); SEAMS 5 makes nightly's `outputColorSpace`/`hdrOutputActive` the single truth and the bridge fills them. If the bridge fills them with the HDR10 space, nightly's legacy tone-map path would also engage unless `AdjustUiTargetMode` overrides it. Needs an in-game check that the SDR-target composite (SEAMS 5, `imgui_impl_dx12` row) and `AdjustUiTargetMode` together produce the oracle's menu.
4. **XeFG camera build.** U2 offers a seam (`XefgBuildCamera`, chosen: ~37 lines of footprint) or an inline `[UP]` fix with only non-mutating near/far (~8 lines, loses `BuildView` validation). SEAMS 5 lists the XeFG barrier but not the camera fix.
5. **Detours transaction lock.** U4 proposes one process-wide `ext::core::DetourLock` shared with ResTrack/DXGI hooks; SEAMS 5 puts the Streamline mutex under `[UP]` (self-contained, file-local) and lists `DetourLock` under `ext/core`. The file-local `[UP]` mutex and the ext lock do not exclude each other. Decide whether the `[UP]` commit uses a header-declared shared mutex or the race between Streamline hooks and ext hooks is accepted.
6. **Forwarder rename and `Kernel_Hooks`.** Rename is pending the S2 check (SEAMS 5). Contingent +7 lines in `Kernel_Hooks.cpp` and a `.sln`/package-path rename. Also affects U1 `Streamline_Proxy.h` `FindFilePath(exePath, "nvngx_dlssnr.dll")` naming.
7. **Back-buffer release quirk.** U4 proposed a `GameQuirk::ReleaseBackbufferOnce`; SEAMS 3.8 prefers `ext::game` interfaces, but 5 keeps a quirk for C4. Catalog uses the quirk (extends one GameConfigs line, 3 more lines in `Quirks.h`/`dllmain.cpp`). Alternative: a seam in `FG_Hooks.cpp` through a game interface (no FFXIV-visible upstream lines, +1 interface).
8. **`FrameLimit` interval.** U1: `ext::present::FrameInterval` seam vs. sending the whole file upstream. The FG-doubling semantics ("cap counts generated frames") is fork policy, not a bug fix; catalog keeps the seam and sends only the timer fixes.
9. **`menuOverlayIsVulkan` / Vulkan DLSS-G interlock.** U4 keeps `ApplyMenuDlssgInterlock` (minus `delayMenuRenderBy`); U5 asks whether nightly already covers the menu delay. Without the delay the interlock may be a no-op. S6 decides; if dropped: -`menu::OnVulkanOverlay`, -`streamline::ApplyMenuDlssgInterlock`, -2 files' lines.
10. **Generic menu behaviour not covered by "cosmetic" in SEAMS 3.6.** A11 preset live-apply (U5 rows 16-19), OptiFG state text (row 26), `SaveIni` bool result and "Settings saved" text (rows 37-38 and `Config.cpp` hunks 18, 19). Catalog: not carried. Confirm.
11. **Footprint target.** About 1,335 lines in 75 files with the current `[UP]` set against ≤ 1,000 and ≤ 40. The levers are in section (d); either the target is restated as "excluding `[UP]` commits" (≈ 620 lines / 57 files) or the A8 `OS_*` edits, the per-API init flags and the recorded-Init need a different shape.
12. **`OS_Dx12` sizing policy.** SEAMS 5 says both `OS_Dx12` and `OS_Vk` gate on the NR override, leaving Output Scaling and Magnifier unchanged; the oracle's `OS_Dx12` changed all callers (U5 OD1). Gating adds lines to the 122-line estimate. Verify the resource-size claim for Output Scaling and Magnifier first.
13. **`IFeature_Dx12::Evaluate` `timingQueue` parameter.** U3 keeps a 2-line signature extension; alternative is a thread-local `ext::nr::CurrentQueue` set by the bridges. Catalog keeps the signature (smaller, visible).
14. **XeFG interpolation-count behaviour.** The fork rewrite stops writing `FGXeFGInterpolationCount` back to max; SEAMS settles the 1..7 range but not this behaviour change. `XefgSelectFactor` documents it; Phase 4 should confirm with the oracle log.
15. **`ext::fg::OnUpscaleStart` per-frame cast.** Removing both `IFGFeature` virtuals means ext casts `IFGFeature*` to `IFGFeature_Dx12` each frame unless cached in `Dx12Ext` at activation. Catalog assumes the cache; verify there is no path where the FG object changes without `OnActivated`.
16. **C3 gate.** SEAMS 5 settles the design (both halves, default off). U1 shows the key has no reader in the oracle and U3 H5b shows an ungated minimum; S7 must compare the gated default-off behaviour against the oracle for FFXIV before `ResolveRenderGeometry` merges (flagged, not a conflict).
