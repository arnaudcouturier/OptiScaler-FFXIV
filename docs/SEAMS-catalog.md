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
| U1 `ext::pacing::FrameInterval` | `ext::present::FrameInterval` **S5 (placed, revised):** `FrameLimit::sleep` calls `FrameInterval(cap, fgActive)` and compares the returned nanoseconds directly (`ext/fg/FrameInterval.cpp`); NaN/infinite/negative caps mean no cap. |
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

`MenuRegistry` cards registered through `MergeBoxes`: `dlss_nr` (tab Upscaling, after `upscaler`, drawn inside `ScopedCard`; `DlssNr::RenderMenu` loses its own CollapsingHeader and starts with the card title; **S2c placed**, `ext/nr/menu/`), `forced_quality` (Upscaling, `after = "upscale_ratio"`'s predecessor), `ffxiv_hdr` (Frame Gen, visible when FFXIV), `fg_unlocks` (Frame Gen), `fg_hudless` (Frame Gen, visible when DLSS-G + OptiFG), `ffxiv_companion` (Compatibility, visible when FFXIV). Draw callbacks receive `ext::menu::Ctx`, built from `RenderMenuContext` through `ext::Access`.

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::menu::MergeBoxes(const MenuBox* base, size_t& count) -> const MenuBox*` | decision | `menu_common.cpp` `MenuCommon::GetMenuBoxes` (static array) | A5, A6, A12, C1, C5, C7, C8, C9, C10, C11, D1, D4 | init, once | Replaces the 2 NR/Companion box literals. Fixes the RenderCard/ScopedCard bypass. Registered card order uses `after`. |
| `ext::menu::XeFgExtras(IFGFeature* fgOutput)` | hook | `menu_common.cpp` `RenderFrameGenerationRuntimeSettings`, XeFG card, right after the Frame Gen slider | D3 | per-frame (menu open) | Dynamic MFG, target FPS, current XeMFG label, Extra Pacing. In-card because of `SameLine` layout. **S5 (placed):** `ext/fg/xefg/XefgSeams.cpp`; takes the FG object (as `ObservedMultiplier` does), not `Ctx`. |
| `ext::menu::AdjustFgOutputOptions(OutputOptions&)` | hook | `menu_common.cpp` `RenderFrameGenerationSelection` | C8 | per-frame (menu open) | FSR FG output disabled while OptiHDR active. |
| `ext::menu::ObservedMultiplier(IFGFeature*)` | hook | `menu_common.cpp` FG status/debug area | D2, C11 | per-frame (menu open) | "Current DMFG: Nx (observed)" and the HUD-less tagged/rejected counters. Replaces the `IFGFeature::GetObservedFrameMultiplier` virtual (ext reads its own state). **S5 (placed, split):** the DMFG readout is `ObservedMultiplier` in the DLSSG card (`ext/fg/MenuFgStatus.cpp`); the HUD-less counters are a separate C11 seam (S10). |
| `ext::menu::PollShortcuts(CheckFn&)` | hook | `menu_common.cpp` `UpdateManualInput`, inside the `!capturingKey && canAcceptInputs` block | A5, C9 | per-frame (input thread) | Polls every registered `ext::menu::Shortcut` with the menu's own `CheckShortcut` lambda (passed in as a template argument; the `pressed` flag lives in the registration). **S2c (placed, signature refined):** template in `ext/core/MenuRegistry.h`, registry in `MenuRegistry.cpp`; NR registers its toggle from `ext/nr/menu/NrMenu.cpp`. The HDR screenshot key (C9) registers the same way with slot 15, but note the oracle calls `Screenshot::Request` at poll time, not in the shortcut pass. |
| `ext::menu::HandleShortcuts(RenderMenuContext&)` | hook | `menu_common.cpp` `HandleMenuShortcuts`, after the FPS-overlay toggle | A5 | per-frame | Runs the handler of every shortcut pressed since the last frame (NR on/off flip and toast). Not merged with PollShortcuts (the poll sits in `UpdateManualInput`, a separate function). **S2c (placed):** template deduced from the real context (`MakeCtx`), so no fork code names `RenderMenuContext`. The NR handler also latches `ext::SetActive(F::Nr, true)` when it turns NR on, so the exposure scan starts the same frame. |
| `ext::menu::KeybindRows<Keybind>()` | hook | `menu_common.cpp` `RenderKeybindSettings`, after the last upstream row | A5, C9 | per-frame (menu open) | One `Keybind` row per registered `Shortcut`; slot indices 14 (NR) and 15 (screenshot) are persisted: keep stable. **S2c (placed, signature refined):** explicit template argument = the TU-local `Keybind` class (no promotion to a header needed); the rows are a function-local static so the capture state survives between frames, like the upstream `static auto` rows. |
| `ext::menu::DrawOverlays(RenderMenuContext&)` | hook | `menu_common.cpp` `RenderMenu`, right after `RenderPerformanceOverlay` | A6, A4 | per-frame | NR compare tags plus exposure-scan lamp. One call replaces U5 rows 15 and 40. **S2c (placed):** runs every registered `ext::menu::Overlay`. The oracle drew the compare labels at the top of `RenderPerformanceOverlay`; they go to the foreground draw list, which ImGui renders after every window, so moving them after the performance overlay changes nothing visible. The lamp window is created at the same point as before (after the performance overlay). |
| `ext::menu::WantsFrame() -> bool` | decision | `menu_common.cpp` `BeginMenuFrameIfNeeded` (last OR term) | A4, A6 | per-frame | Menu frame is built when the scan lamp or compare tags are on. Overlay Present must still draw when the menu is closed (it does: `RenderMenu` returns `ctx.newFrame`). **S2c (placed):** asks every registered `Overlay::wantsFrame`; no overlay registered = false. |
| `ext::menu::ShaderTimeRows() -> ExtraTimes` (+ `DrawShaderTimeRows(const ExtraTimes&)`) | hook | `menu_common.cpp` `RenderShaderTimes` (before the loop; the `hasExtra` condition; after the extra-shaders loop) | A1 | per-frame (menu open) | The "Neural Rendering N ms" row of the extra-shaders block and `hasExtra` feeding. **S2c (placed, signature refined):** returns a fixed-size list of the registered `TimeRow`s that have a value (no heap); 3 call lines plus the 1-line condition. The row only shows when upstream's own table is shown (`detailedGpuTimes` not empty), as in the oracle. |
| `ext::menu::UseSdrUiTarget() -> bool` | inline decision | `menu_common.cpp` `getUiTargetMode`, first statement | C8 | per-frame | Early return SDR while OptiHDR composites the menu; preserves the normal upstream selection otherwise. |
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
| `ext::dx11bridge::ConfigureOutput(Dx11wDx12SC&)` | hook | `dx11_with_dx12_sc.cpp` ctor, after `_RefreshCachedSwapchainDesc` | C8 | init | Negotiates PQ/BT.2020 when FFXIV HDR is enabled and the FG chain is R10G10B10A2; restores the original format on failure. |
| `ext::dx11bridge::OnDestroy(Dx11wDx12SC&)` | hook | `dx11_with_dx12_sc.cpp` dtor | C8 | teardown | Deactivates OptiHDR. Never blocks or releases GPU objects from DLL detach under loader lock. |
| `ext::dx11bridge::OutputFormat(Dx11wDx12SC&, DXGI_FORMAT) -> DXGI_FORMAT` | decision | `dx11_with_dx12_sc.cpp` `ResizeBuffers` and `ResizeBuffers1` | C8 | rare | Requests R10G10B10A2 while OptiHDR owns the bridge output. |
| `ext::dx11bridge::OnResizeDone(Dx11wDx12SC&) -> HRESULT` | decision | same functions, after successful resize | C8 | rare | Renegotiates HDR output; returns `DXGI_ERROR_UNSUPPORTED` on failure. |
| `ext::dx11bridge::PresentScope` (RAII) | RAII | `dx11_with_dx12_sc.cpp` `Present`, after bypass early-return | C10 | per-frame | Calls `ISceneHdr::EndFrame` at every exit. |
| `ext::dx11bridge::PreInteropCopy(Dx11wDx12SC&, uint32_t slot)` | hook | `dx11_with_dx12_sc.cpp` `Present`, before `_WaitDx11ThenDx12` | C11 | per-frame | HudlessEngine P1: `HudlessSource::Take`, validate, normalise, `CopyResource` on the D3D11 immediate context before the interop signal. |
| `ext::dx11bridge::UiFreeFrame(Dx11wDx12SC&) -> bool` | decision | `dx11_with_dx12_sc.cpp` `Present`, after `_WaitDx11ThenDx12` | C11 | per-frame | P2: hud-less pending and paste ready. |
| `ext::dx11bridge::PostInteropCopy(Dx11wDx12SC&) -> bool needRecopy` | decision | `dx11_with_dx12_sc.cpp` `Present`, after `_WaitForInteropCopyOnPresentQueue` | C11 | per-frame | P4+P5: tag hud-less on the FG UI list, produce the paste. Upstream keeps `if (needRecopy) { copy; wait }` (3 inline lines). |
| `ext::dx11bridge::ConvertCopySource(Dx11wDx12SC&, UINT, ID3D12Resource*&, D3D12_RESOURCE_STATES&) -> bool` | decision | `dx11_with_dx12_sc.cpp` `_CopyDx11SharedToDx12FGBackBuffer`, after source selection | C8, C10 | per-frame | Converts the SDR source to PQ; mode-1 scene HDR also opens the scene and re-tags the HUD-less FG input. False closes the copy list and returns failure. |
| `ext::dx11bridge::PrepareScreenshot(Dx11wDx12SC&, UINT, ID3D12Resource*)` | hook | same function, after `CopyResource` | C9 | per-frame | Records a requested SDR or HDR readback. Submission occurs in the registered GPU owner's after-execute callback; a Reset callback drops captures whose recording was discarded. |
| `ext::dx11bridge::BeforeSharedCopy(Dx11wDx12SC&)` | hook | `dx11_with_dx12_sc.cpp` `_CopyDx11BackBufferToShared`, before | C10 | per-frame | Seals the scene HDR preview mask at the SDR backbuffer frame boundary. |
| `ext::dx11bridge::AfterSharedCopy(ID3D11DeviceContext*, ID3D11Texture2D* dst)` | hook | same, after | C7 | per-frame | Companion source markers; the FFXIV test is behind `game::Current()`. |
| `ext::dx11bridge::SetColorSpace(Dx11wDx12SC&, DXGI_COLOR_SPACE_TYPE, HRESULT& out) -> bool handled` | decision | `dx11_with_dx12_sc.cpp` `SetColorSpace1` | C8 | rare | Swallows the game's call when OptiHDR owns the output. Otherwise fills nightly's `outputColorSpace`/`hdrOutputActive` (SEAMS 5; the bridge never filled them in the oracle). |
| `ext::dx11bridge::OnReleaseInterop(Dx11wDx12SC&)` | hook | `dx11_with_dx12_sc.cpp` `_ReleaseInteropBackBuffers` | C11 | rare (resize), teardown | P8: drain FG queue (2 s), release the hud-less ring; leaks on failure by design. |
| `ext::dx11bridge::PrepareUpscalerColor(IFeature_Dx11wDx12&, cmd, params, color, uint2& guideSrc) -> ID3D12Resource*` | decision | `IFeature_Dx11wDx12.cpp` `Evaluate`, before binding Color | C4 | per-frame | Lanczos3 scale of the display-sized colour to render size when `GameQuirk::ScaleDisplayColorForForcedQuality` and `IRenderGeometry::ColorIsDisplayAllocation`. nullptr = failure, upstream `break`s inline. |
| `ext::dx11bridge::OnUpscaleBoundary(ID3D11DeviceContext*, ID3D11Resource* color, const NVSDK_NGX_Parameter*)` | hook | `IFeature_Dx11wDx12.cpp` `Evaluate`, top | B6 | per-frame | `ISceneLighting::OnUpscaleBoundary`; one relaxed load when disarmed. |
| `ext::dx11bridge::OnNgxInit()` | hook | `NVNGX_DLSS_Dx11.cpp` `NVSDK_NGX_D3D11_Init_Ext` | A9, C6 | init | NR resume after bridge init (`DlssNr::ResumeAfterBridgeInit`, a no-op unless suspended). **S2b:** placed before `return Success`, body in `ext/nr/NrSeams.cpp`; no game test needed (the oracle's FFXIV exe test was only a guard on a no-op). |
| `ext::dx11bridge::OnNgxShutdownBegin()` | hook | `NVNGX_DLSS_Dx11.cpp` `NVSDK_NGX_D3D11_Shutdown` | A9, C6 | teardown | NR suspend (`DlssNr::SuspendForBridgeShutdown`); before the shutdown-state teardown. **S2b:** placed as the first statement after `shutdown = true;`; body suspends while `WithDx12::IsInited()` (the oracle also required the FFXIV exe: other games with the D3D11-on-D3D12 bridge now get the same orderly NR stop). |
| `ext::dx11bridge::ShutdownNgx(ID3D11Device*, NVSDK_NGX_Result& out) -> bool handled` | decision | `NVNGX_DLSS_Dx11.cpp` `NVSDK_NGX_D3D11_Shutdown` | C6 | teardown | `INgxLifecycle::IsolatesShutdown`: device-specific `Shutdown1` only, keeps Streamline's DX12 NGX alive. The `D3D11Device = nullptr` reorder and success-gated `SetDx11Inited(false)` stay inline. |
| `ext::dx11bridge::RedirectShutdown1(ID3D11Device* in, ID3D11Device*& recorded) -> bool` | decision | `NVNGX_DLSS_Dx11.cpp` `NVSDK_NGX_D3D11_Shutdown1`, first statement | C6 | teardown | A null-device `Shutdown1` has the same global effect as the legacy one. Signature refined: the file-local `D3D11Device` is passed by reference (the seam records the device). True -> caller returns `NVSDK_NGX_D3D11_Shutdown()`. |
| `ext::dx11bridge::BeforeReleaseFeature(IFeature_Dx11&, bool shuttingDown) -> bool ok` | decision | `NVNGX_DLSS_Dx11.cpp` `NVSDK_NGX_D3D11_ReleaseFeature` | C6, D3, D2 | per-create (feature release) | Cancel pending FG upscaler work under `fg->Mutex` (`ext::Access::CancelPendingUpscalerWork`, ext-internal; needs the `friend struct ext::Access;` line in `IFGFeature_Dx12.h`), drain FG and upscale queues (`ext::gpu::DrainQueue`), rebuild the XeFG context when `FGXeFGResetOnUpscalerChange`. false -> `FAIL_PlatformError`. **S4 status:** everything but the XeFG rebuild. **S5 (D3, placed):** the XeFG rebuild block in `ext/dx11bridge/NgxLifecycle.cpp` (`ExtConfig` key `[XeFG] ResetOnUpscalerChange`). |

## nr

Native DLSS Neural Rendering (A*, B*, E4 AMD PreSR). All internals live in `ext/nr`; these are the upstream attachment points.

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::nr::EvalBefore(NrEvalCtx&)` | hook | `NVNGX_DLSS_Dx12.cpp` `NVSDK_NGX_D3D12_EvaluateFeature` (native passthrough and OptiScaler path: 2 sites); `IFeature_Dx11wDx12.cpp` `Evaluate`; `IFeature_VkwDx12.cpp` `Evaluate` | A1, A9, B1, B8, B9, C4 | per-frame | `NrEvalCtx` (ext/nr/NrSeams.h) carries site (Native, Dx11Bridge, VkBridge), cmd, params, queue, frame, the NGX feature (SR/RR; bridges derive it from the upscaler) and the C4 guide extent. The SR/RR-only filter lives inside (native: both Before and After, so a DLSS-D/other handle never runs the model). Not gated on the NR switch: the NR internals still see the evaluate while NR is off (toggle reset, skip log), as in the oracle. `EvalBefore` latches `F::Nr` once NR was seen enabled (hot-path guard of the A4 scan seams). **S2b:** placed in `NVNGX_DLSS_Dx12.cpp` (2 sites), `IFeature_Dx11wDx12.cpp` and `IFeature_VkwDx12.cpp` (`NrEvalCtx::Bridge(...)` + the pair around `dx12Feature->Evaluate(cmd, params, Dx12CommandQueue)`; the one-shot "bridge reached the hand-off" log lives in `EvalAfter`, per bridge; the C4 guide extent stays 0 until S7 passes it). |
| `ext::nr::EvalAfter(NrEvalCtx&, bool ok)` | hook | same four sites | A1, A9, B8, B9 | per-frame | Replaces `DlssNr::EvaluateAfterUpscale` and the one-shot hand-off log. `ProbeD3D11` (E2) is not called. |
| `ext::nr::VkEvalBefore(cmd, params, inst, pd, dev, Upscaler backend) -> VkNrToken` | decision | `NVNGX_DLSS_Vk.cpp` `NVSDK_NGX_VULKAN_EvaluateFeature`, before `deviceContext->Evaluate` | B9 | per-frame | Token carries `handled`, `rayReconstruction`, `bridged`, original and NR colour; destructor restores `Parameter_Color` (at scope exit, not right after the evaluate: nothing between reads Color, the after-pass reads Output). The bridged-backend list is an `IsOn12(Upscaler)` helper in ext (the oracle list omits `DLSS_on12`; no Vulkan provider hands it out, so no behaviour change). **S2b:** placed. |
| `ext::nr::VkEvalAfter(VkNrToken&, bool upscaleOk, cmd, params, inst, pd, dev)` | hook | same, after the backend-fallback early return | B9 | per-frame | Signature refined: `upscaleOk` is passed in (the fallback block only returns for a failed upscale when the configured backend is not FSR22). Skipped when `token.bridged` or the model already ran before the upscale. |
| `ext::nr::PreSrScope` (RAII) `(ID3D12Device*, Upscaler, int featureFlags, cmd, params, ID3D12Resource* outputTarget, ID3D12CommandQueue* timingQueue)` | RAII | `IFeature_Dx12.cpp` `Evaluate`, replaces the `Set(Output, currentTarget)` line | B1, B3, E4 | per-frame | Points Output at the first shader pass' input and, for run-before-SR on an AMD device, Color at the AMD pre-SR edit (`AmdBridge::CanUse/EnsureHooks/Prepare`, ApplyModel gating); restores both on every exit (replaces the `NgxResourceBinding` plumbing; Motion/Depth bindings are not carried). Arguments are values because `Device` is protected. `timingQueue` is a 1-line inline signature extension (`// ext:`, `IFeature_Dx12.h` + `.cpp`). |
| `ext::nr::NormalizeDlssPresets(NVSDK_NGX_Parameter*)` | hook | `DLSSFeature.cpp` `ProcessInitParams` | B1 | per-create | Maps the six `Hint_Render_Preset_*` equal to `NV_PRESET_LATEST`. SR only. **S2b:** placed after the preset-override block, before the PerfQuality fix-up. |
| `ext::nr::WrapNvapi(unsigned int id, void* fn) -> void*` | decision | `NvApiHooks.cpp` `hkNvAPI_QueryInterface`, final return | A10, B2 | init (hundreds of calls) | Not gated on the NR switch (the oracle always recorded the CUDA entry points the game queried, so a later enable still finds them): `DlssNrNative::WrapNvapi` takes a recursive mutex and a switch per call, ids outside the six pass through. The E6 early-return and KCD2 hunks are not carried. **S2b:** placed. |
| `ext::nr::NeedsDlssgRuntime(const std::filesystem::path& fileName) -> bool` | decision | `Util.cpp` `Util::FindFilePath` (`isDlssgOutput` term) | B7 | per-create (file lookup) | NR + DeferredDLSS + ResidualFG needs the official FG runtime (the file name must be `nvngx_dlssg.dll`). Signature refined: `FindFilePath` takes a `std::filesystem::path`. **S2b:** placed. |
| `ext::nr::AppendNgxSearchPaths(const std::filesystem::path& exePath, std::vector<std::wstring>&)` | hook | `Streamline_Proxy.h` `LoadStreamline`, where the SL NGX path list is built | A1, D5 | init, once | Merges U1 hunks 3 and 4: the seam does the `FindFilePath(exePath, "nvngx_dlssnr.dll")` and pushes its folder (after `DLSSFeaturePath`, as the oracle). Signature refined: `exePath` is passed in. **S2b:** placed. |
| `ext::nr::VkDeviceExtScope(PFN_vkGetInstanceProcAddr, VkPhysicalDevice, VkDeviceCreateInfo& local)` (RAII) | RAII | `Vulkan_Hooks.cpp` `hkvkCreateDevice`, after the spoofing call, plus `LogResult(VkResult)` after the real call | B9 | per-create | Replaces the planned `AppendVkDeviceExtensions -> ExtScope` pair (same shape, one constructor). Owns the name vector so `ppEnabledExtensionNames` outlives the call; no-op when NR is off (switch read once at construction). The swapchain colour-space log (dead) is dropped. **S2b:** placed. |

**S2a status (code is in `ext/nr`, nothing in upstream calls it yet; the seams above are S2b's).** Ext-internal hooks of the moved code, none of them an upstream seam:

| Hook | Where | Notes |
|---|---|---|
| `DlssNr::EnsureGpuHooks(ID3D12Device*)` | `ext/nr/pass/DlssNr_Dx12.cpp` | Registers NR's one `ext::gpu::Owner` (begin = `BeginGpuSubmission`, quarantine = `QuarantineGpuSubmission`, reset = `NotifyGpuReset`) on first call, then `ext::restrack::EnsureHooks(device)`. Called inside NR before it records; replaces `ResTrack_Dx12::HookLateNrQueue`. |
| `DlssNr::AmdBridge::EnsureHooks(ID3D12Device*)` | `ext/nr/amd/AmdBridge.cpp` | Registers the AMD pre-SR owner (isolate = `PendingListIndex`, before = `Submitting`, after = `Submitted`), then `ext::restrack::EnsureHooks`. S2b's `PreSrScope` calls it where `IFeature_Dx12::Evaluate` called `CanUse(Device) && HookLateNrQueue(Device)`. |
| `ext::nr::SetSceneHdrHooks(const SceneHdrHooks&)` | `ext/nr/NrHooks.h` | C10 (S6) fills it. With nothing registered NR runs on the SDR colour (= HDR.NRSceneInput / DLSSSceneInput off). |
| `ext::game::Current().nrQuirks` (`INrQuirks::BridgedNativeDx11`) and `.lighting` (`ISceneLighting::ConsumeHistoryReset(uint64_t& cursor)`, `ISceneLighting::Status()` for the NR menu, S2c) | `ext/game/GameInterfaces.h` | Replace the `gameExe == "ffxiv_dx11.exe"` tests inside NR; `ext/ffxiv` (S7) provides both. |

Not seams: `Shader_Dx12.cpp` `TranslateTypelessFormats` skip is solved by the NR call site passing `translateTypeless=false` (0 upstream lines; done in `DlssNr_Dx12::DispatchPass`, S2a). `OS_Dx12`/`OS_Vk` are not touched: NR owns `NrResampler_Dx12`/`NrResampler_Vk` (SEAMS 7 A8, S2a). The `dlssnr` forwarder DLL is the second project in `OptiScaler.sln`.

## fg

Provider-neutral frame-generation seams (DLSS-G, XeFG, FSR-FG) on `IFGFeature_Dx12`, `DLSSG_Common`, `Upscaler_Inputs_Dx11wDx12`, `FG_Hooks`. Per-object state: `IFGFeature_Dx12::_ext` (`ext::fg::Dx12Ext`, holding guide ring, `_sceneHistory`, SC fence state, DLSS-G observed multiplier and history, XeFG dynamic state). `DLSSG_Common` gets no member; ext reaches the derived object by `dynamic_cast` cached in `Dx12Ext`.

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::fg::OnActivated(IFGFeature*)` | hook | `DLSSG_Common.cpp` `Activate`; `XeFG_Dx12.cpp` `Activate` | D3, E3 | init | XeFG `RequestRecovery`; DLSS-G present stats reset. Fullscreen-state logging is dropped. **S5 (placed):** both hosts, `ext/fg/Activation.cpp`. |
| `ext::fg::OnDeactivating(IFGFeature*)` | hook | `DLSSG_Common.cpp` `Deactivate`; `XeFG_Dx12.cpp` `Deactivate` | B5, C5 | teardown | Invalidate history and guide snapshots, observed multiplier. **S5 (placed):** `ext/fg/GuideSeams.cpp`, matcher in `ext/fg/GuideMatcher.cpp` (state `GuideState` behind `Dx12Ext::guides`, `Dx12Ext::dlssgFrames/forceConstantsReset`). Inert unless `game::Current().fgMarkers`. |
| `ext::fg::AdjustMfgMax(int driverMax) -> int` | decision | `DLSSG_Common.cpp` `QueryState` | D1 | init | `max(driverMax, MfgUnlock::UnlockedMax())`. Must agree with `streamline::RaiseMfgCeiling`. **S5 (placed):** `ext/fg/streamline/StreamlineSeams.cpp`. |
| `ext::fg::AdjustDlssgOptions(IFGFeature*, sl::DLSSGOptions&)` | hook | `DLSSG_Common.cpp` `SetOptions` | D1, D2, C11 | per-frame | UI recomposition and hud-less/ui buffer formats, dynamic-MFG frame limit in the Reflex options, menu interlock (calls `streamline::ApplyMenuDlssgInterlock`). Logs on change only. The add-on module probe is gone. **S5 (placed):** `ext/fg/dlssg/DlssgSeams.cpp`. The Vulkan interlock runs for every output (DLSSG_Vk shares `SetOptions`) as an ext-internal copy of the [UP] rule in `Streamline_Hooks.cpp` (that one only sees the game's own calls), so `ext::streamline::ApplyMenuDlssgInterlock` is not a seam. The recomposition half reads `ext/fg/HudlessLink.h` (stub: no source until C11/S10) and `[DLSSG] UIRecomposition`; its log line says `hud-less source active` instead of the dropped add-on probe. |
| `ext::fg::AdjustReflexOptions(const sl::DLSSGOptions&, sl::ReflexOptions&)` | hook | `DLSSG_Common.cpp` `SetOptions`, after the Reflex options are built | D2 | per-frame | In dynamic mode the user's FramerateLimit is sent as `frameLimitUs`. **Added by S5** (the catalog's single `AdjustDlssgOptions` could not cover it: the Reflex options are built after `slDLSSGSetOptions`). |
| `ext::fg::AfterDlssgSetOptions(IFGFeature*, const sl::ViewportHandle&, const sl::DLSSGOptions&, sl::Result)` | hook | `DLSSG_Common.cpp` `SetOptions`, after | D2 | per-frame | Observed multiplier sampling; runs on the existing SL thread. **S5 (placed):** state in `Dx12Ext` (observed multiplier, sample mode, last options); E3 (placed): the 5 s "DLSSG presentation" telemetry log (state: file-local `PresentStats` in `ext/fg/dlssg/DlssgSeams.cpp`, reset by `fg::OnActivated`). |
| `ext::fg::ForceConstantsReset(DLSSG_Common*) -> bool` | decision | `DLSSG_Common.cpp` `SetConstants` (`reset` term) | B5, C5 | per-frame | State held in `Dx12Ext`, no `_forceConstantsReset` field upstream. **S5 (placed):** `ext/fg/GuideSeams.cpp`, matcher in `ext/fg/GuideMatcher.cpp` (state `GuideState` behind `Dx12Ext::guides`, `Dx12Ext::dlssgFrames/forceConstantsReset`). Inert unless `game::Current().fgMarkers`. |
| `ext::fg::GuideAge(IFGFeature_Dx12*, int fIndex, uint64_t willDispatchFrame) -> GuideInfo` | decision | `DLSSG_Dx12.cpp` and `XeFG_Dx12.cpp` `Dispatch` | B5 | per-frame | `age` and `sceneFrame`. FFXIV test is `game::Current().fgMarkers != nullptr`. **S5 (placed):** `ext/fg/GuideSeams.cpp`, matcher in `ext/fg/GuideMatcher.cpp` (state `GuideState` behind `Dx12Ext::guides`, `Dx12Ext::dlssgFrames/forceConstantsReset`). Inert unless `game::Current().fgMarkers`. |
| `ext::fg::DispatchResetHistory(IFGFeature_Dx12*, const GuideInfo&, uint64_t frame, Provider) -> bool` | decision | `DLSSG_Dx12.cpp` and `XeFG_Dx12.cpp` `Dispatch`, before constants | B5, C5 | per-frame | Replaces `DlssgPreConstants` and `XefgResetHistory`. **S5 (placed):** `ext/fg/GuideSeams.cpp`, matcher in `ext/fg/GuideMatcher.cpp` (state `GuideState` behind `Dx12Ext::guides`, `Dx12Ext::dlssgFrames/forceConstantsReset`). Inert unless `game::Current().fgMarkers`. |
| `ext::fg::PostDispatch(IFGFeature_Dx12*, const GuideInfo&, uint64_t frame, bool discontinuity, Provider)` | hook | same, after constants/dispatch | B5, C5 | per-frame | Commit or invalidate history; replaces `DlssgPostConstants` and `XefgPostDispatch`. **S5 (placed):** `ext/fg/GuideSeams.cpp`, matcher in `ext/fg/GuideMatcher.cpp` (state `GuideState` behind `Dx12Ext::guides`, `Dx12Ext::dlssgFrames/forceConstantsReset`). Inert unless `game::Current().fgMarkers`. |
| `ext::fg::SetResourcePre(IFGFeature_Dx12*, int fIndex, Dx12Resource*&, Dx12Resource&, Provider) -> bool` | decision | `DLSSG_Dx12.cpp` and `XeFG_Dx12.cpp` `SetResource`, entry | B5, C8, C10 | per-resource | Matches Depth/Velocity guides and dispatches HDR handling for HudlessColor/UIColor through the `Provider` (DLSS-G or XeFG). False skips the input. **S5:** `ext/fg/GuideSeams.cpp`, matcher in `GuideMatcher.cpp`; HDR body in `ext/hdr/FgHdr.cpp`. |
| `ext::fg::SetResourcePost(const Dx12Resource& in, Dx12Resource& frameResource)` | hook | same, after the copy | B5, C8 | per-resource | `hdrEncoded` flag copy; null the copy for encoded and Depth/Velocity. **S5 (placed):** `ext/fg/GuideSeams.cpp`, matcher in `ext/fg/GuideMatcher.cpp` (state `GuideState` behind `Dx12Ext::guides`, `Dx12Ext::dlssgFrames/forceConstantsReset`). Inert unless `game::Current().fgMarkers`. |
| `ext::fg::NormalizeFormat(DXGI_FORMAT) -> DXGI_FORMAT` | decision | `DLSSG_Dx12.cpp` and `XeFG_Dx12.cpp` `SetResource` (3 sites) | C8 | per-resource | R10G10B10A2 under OptiHDR. |
| `ext::fg::DlssgTagHudless(DLSSG_Dx12*, Dx12Resource&, sl::ResourceTag&)` | hook | `DLSSG_Dx12.cpp` `SetResource` tag | C8 | per-frame (HDR only) | Converts hud-less to PQ and tags `eOnlyValidNow`; deactivates FG on failure. |
| `ext::fg::XefgPrepareHudless(IFGFeature_Dx12*, Dx12Resource&, int, _xefg_swapchain_d3d12_resource_data_t&) -> bool` | decision | `XeFG_Dx12.cpp` `SetResource` | C8 | per-frame (HDR only) | Immediate SDK copy path; failure deactivates FG. |
| `ext::fg::SceneRawHudless(const Dx12Resource&) -> bool` | decision | `DLSSG_Dx12.cpp` and `XeFG_Dx12.cpp` `SetResource` | C10 | per-frame (scene HDR) | Holds the raw SDR HUD-less input for later scene-HDR conversion after the interop fence. |
| `ext::fg::AdjustConstants(FG_Constants&)` | hook | `Upscaler_Inputs_Dx11wDx12.cpp` `UpscaleStart` | C8 | per-frame | Adds the HDR flag to the FG constants while OptiHDR output is active. |
| `ext::fg::SubmitSC(IFGFeature_Dx12*, UINT idx) -> bool` | decision | `DLSSG_Dx12.cpp`, `XeFG_Dx12.cpp`, `FSRFG_Dx12.cpp` `Present` (3 sites) | C6, D5 | per-frame | Ordered SC list submit with monotonic `_scFenceValue`; false = `return false`. Net -10 lines per site upstream. Fence state in `Dx12Ext`. **S4:** the execute/signal half exists as `ext::gpu::ExecuteOrdered` (its `ext::gpu::OrderedFence {value, failed}` replaces `_scFenceValue`/`_scSubmissionFailed` inside `Dx12Ext`); the wrapper itself, `Dx12Ext` and the SC wait are S5. **S5 (placed):** `ext/fg/Dx12Sc.cpp`, state in `Dx12Ext::sc` (an `ext::gpu::OrderedFence`). |
| `ext::fg::PrepareSC(IFGFeature_Dx12*, UINT idx) -> bool` | decision | `IFGFeature_Dx12.cpp` `GetSCCommandList` | C6, D5 | per-frame | Replaces the 7 inline early-outs/waits/null-checks; tied to `SubmitSC` (stale lists are discarded, not executed). **S5 (placed):** the call sits after the stale-list discard loop and also does the allocator Reset (the 16-line reset block is gone from upstream); `ext/fg/Dx12Sc.cpp`. Only the index bounds check stays inline (`// ext: C6`). |
| `ext::fg::ReleaseGuides(IFGFeature_Dx12*)` | hook | `DLSSG_Dx12.cpp` and `XeFG_Dx12.cpp` `ReleaseObjects` | B5 | teardown | Collect and retire guide snapshots (fence refs); intentional leak of unfinished copies at shutdown. **S5 (placed):** `ext/fg/GuideSeams.cpp`, matcher in `ext/fg/GuideMatcher.cpp` (state `GuideState` behind `Dx12Ext::guides`, `Dx12Ext::dlssgFrames/forceConstantsReset`). Inert unless `game::Current().fgMarkers`. |
| `ext::fg::XefgSelectFactor(IFGFeature_Dx12*, int current, int maxInterpolation, XefgFactorInfo& info) -> int` | decision | `XeFG_Dx12.cpp` `Dispatch`, interpolation-count block | D3 | per-frame | Clamp, dynamic MFG, target capped by `FramerateLimit`. Behaviour change vs nightly: no longer writes `FGXeFGInterpolationCount` back to max. **S5 (placed):** `info` (dynamic, target) is for the caller's "XeFG factor" log; the change/apply block stays inline (`// ext: D3`) because it shares `_framesToInterpolate`/`WAR_xefgRequestFGToggle` with upstream. Dynamic state in `Dx12Ext::xefg`. |
| `ext::fg::XefgFrameTime(double inputFt) -> double` | decision | `XeFG_Dx12.cpp` `Dispatch` | D3, B5 | per-frame | A game module with `fgMarkers` (FFXIV) prefers the paced time; others only when input is missing; falls back to the Opti time. **S5 (placed):** the `gameExe == "ffxiv_dx11.exe"` test is `game::Current().fgMarkers != nullptr` (live once S7 registers the FFXIV module); no `XeFG_Dx12*` parameter needed. |
| `ext::fg::XefgNoteFrameTime(float& ft, bool& resetHistory)` | hook | `XeFG_Dx12.cpp` `Dispatch` | D3 | per-frame | Discards samples outside [0, 250] ms (ft = 0, reset unless `FGSkipReset`) and feeds `XeFGPacing`. **S5 (placed):** takes the sample by reference. |
| `ext::fg::XefgBuildCamera(float* view, const float* pos, right, up, forward, uint64_t frameCount, int guideAge) -> bool havePose` | decision | `XeFG_Dx12.cpp` `Dispatch` camera build | D3, B5 | per-frame | Validated view matrix with stationary fallback (`XeFGCamera::BuildView`) plus the periodic fallback log. **S5 (placed):** the non-mutating near/far conversion stays inline as locals (`// ext: D3`; the matched B5 snapshot metadata must not be written). |
| `ext::fg::OnXefgProviderLoaded(HMODULE)` | hook | `XeFG_Proxy.h` after the provider module is bound | D3 | init, once | `XeFGUnlock::Apply`; runs before the first call into the provider. Failure leaves the provider untouched. |
| `ext::fg::OnUpscaleStart(IFGFeature*)` | hook | `Upscaler_Inputs_Dx11wDx12.cpp` `UpscaleStart` | B5, D2, C5 | per-frame | `IFgMarkers::PresentationGuideDelay` into the guide ring, `IFgMarkers::OnUpscale`. Replaces `IFGFeature::SetPresentationGuideDelay` virtual. Cast to `IFGFeature_Dx12` is done once and cached. **S5 (placed):** both parts placed (`ext/fg/GuideSeams.cpp`): the guide delay goes to the matcher, then `IFgMarkers::OnUpscale`. `IFgMarkers` signatures are final in `ext/game/GameInterfaces.h`. |
| `ext::fg::MarkersBeforePresent(uint32_t frameId)` | hook | `FG_Hooks.cpp` `FGPresent`, before `GetNewFrameToken` | D2, C5 | per-frame | `IFgMarkers::BeforePresent`; pointer test only when no game module. Raw-send function on the fork's Reflex path replaces the `bridgeMarkerCall` thread-local (SEAMS 5): no check in `hkNvAPI_D3D_SetLatencyMarker`. **S5 (placed):** `ext/fg/MarkerSeams.cpp`. No `bridgeMarkerCall` is carried: the markers go to Streamline PCL under `ScopedOptiScalerReflex`, which upstream's Reflex input (`FromOptiScaler`) already recognises (S7 verifies with the log). |
| `ext::fg::MarkersNextFrame(uint64_t frame)` | hook | `FG_Hooks.cpp` `FGPresent`, after `ReflexSleep` | D2, C5 | per-frame | `IFgMarkers::NextFrame`. |

Drop list for this group (SEAMS 5): E6 KCD2 waitable-object hunks, `External` early-returns, 64-bit `sleep` widening (18 `low_latency` files untouched), `IFGFeature.cpp` log tweak, `OnFgActivated` diagnostics.

## present

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::present::PlanDx12Presenter(HWND, bool skipFgSwapchainCreation) -> PresenterPlan{hdr, createFg}` | decision | `DxgiFactory_Hooks.cpp` `CreateSwapChain`, `CreateSwapChainForHwnd`; `DxgiFactory_WrappedCalls.cpp` same two (4 sites) | C8 | per-create | `Hdr10::Request(hwnd)` must stay side-effect free. Upstream ORs `plan.hdr` into its condition. S6 restores upstream's `activeFgInput != NvngxFG` term unless `git log -S` justifies its removal (SEAMS 5). |
| `ext::present::PatchPresenterDesc(DXGI_SWAP_CHAIN_DESC1&/DXGI_SWAP_CHAIN_DESC&, const PresenterPlan&)` | hook | same 4 sites | C8 | per-create | Sets R10G10B10A2. Overloads for both desc types. |
| `ext::present::LogPlainPresenter(const PresenterPlan&, unsigned int fgResult)` | hook | `DxgiFactory_Hooks.cpp` / `DxgiFactory_WrappedCalls.cpp`, plain presenter fallback | C8 | per-create | Logs when no FG provider was selected or when FG swapchain creation fails. |
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
| `ext::restrack::NoteUav(ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*)` | inline-guarded | `ResTrack_dx12.cpp` `hkCreateUnorderedAccessView` | A4 | per-resource (very hot, thousands per frame) | `F::Nr` bit test (ext/nr/NrSeams.h) before any call; the bit is set when NR is enabled in the ini (`NrConfig::PostLoad`) or first seen enabled by `EvalBefore`, and stays set for the session; the scan re-checks the live switch. Never touches the resource unless NR is on. **S2b:** placed before `o_CreateUnorderedAccessView`. |
| `ext::restrack::NoteResourceCreated(HRESULT hr, const D3D12_RESOURCE_DESC*, REFIID, void** resource)` | inline-guarded | `D3D12_Hooks.cpp` tail of `hkCreateCommittedResource`, `hkCreatePlacedResource` (2 sites) | A4 | per-resource (any thread, bursts of thousands) | Keeps upstream's `const HRESULT hr = o_...; note; return hr;` shape. Signature refined: takes `hr` and the out pointer so the host line is one call. The `F::Nr` guard and the success/non-null tests are inline; the slow part checks `riid` (Resource/Resource1/Resource2) before casting (the oracle cast blindly). Lock-free on the miss path. |
| `ext::restrack::WantsResourceCreateHooks() -> bool` | decision | `D3D12_Hooks.cpp` `HookToDevice` install block | A4 | init, per device | True when NR is enabled (the oracle's `wantScan`). An `else if` after the UE-spoof block installs only the two resource-creation detours, so the spoof-only hooks stay gated by the spoof flag alone (0 upstream lines moved; same detours as the oracle for every flag combination). |
| `ext::restrack::OnDeviceHooked(ID3D12Device*)` | hook | `ResTrack_dx12.cpp` `HookDevice`, last statement | A1, C8, E4, C6 | init, per device | `EnsureHooks` when an owner is active; no-op otherwise. Mutexed. |
| `ext::restrack::ReleaseQueueHook()` | hook | `ResTrack_dx12.cpp` `ReleaseDeviceHooks`, `ReleaseHooks` | A1, C8 | teardown | Own transaction before upstream's; the only detach of the fork queue detour. |
| `ext::restrack::OnFeatureRelease()` | hook | `NVNGX_DLSS_Dx12.cpp` `NVSDK_NGX_D3D12_ReleaseFeature`, before freeing resources | A4 | per-create (feature release) | Releases the pinned scan resources (Cyberpunk device-removal fix); must run before any teardown. Unconditional, as in the oracle (empty list while NR never ran). |

## streamline

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::streamline::OnLoadStreamline()` | hook | `Streamline_Proxy.h` `LoadStreamline`, first statement | D4 | per-create (each call, guarded inside) | Ampere MFG loader setup; must precede the SL module load and the `_dll != nullptr` early-out. **S5 (placed):** `ext/fg/streamline/StreamlineSeams.cpp` -> `AmpereMfgLoader::TrySetup()` (`ext/fg/dlssg/`). The 9 `[DLSSG] AmpereMfg*` keys are `FgConfig::ampereMfg*`; menu in card `fg_unlocks`. |
| `ext::streamline::MfgUnlockTick()` | hook | `Streamline_Hooks.cpp` `hkslDLSSGSetOptions`, `hkslDLSSGGetState` | D1, D4 | per-frame | `TryApply()`; atomic load when idle. **S5 (placed):** `MfgUnlock.cpp` and `AdaTemporal.h` are in `ext/fg/dlssg/`; `[DLSSG] AdaMfgUnlock` is `FgConfig::adaMfgUnlock` (`AdaBlackwellKernels` dropped). |
| `ext::streamline::MfgCeilingStale(const std::optional<int>& cached) -> bool` | decision | same two functions, populate-once condition (duplicated upstream block, 2 sites) | D1, D4 | per-frame | True while an unlock raised the ceiling after the first populate. **S5:** signature refined: it is the whole populate-once condition `(!cached || UnlockedMax() > *cached) && !Pending()` (the provisional-answer term is inside), so the upstream line is `if (ext::streamline::MfgCeilingStale(state.dlssgMfgMax))`. |
| `ext::streamline::RaiseMfgCeiling(uint32_t& numFramesToGenerateMax) -> uint32_t` | hook | `hkslDLSSGGetState` after each `o_slDLSSGGetState` read; `hkslDLSSGSetOptions` (3 sites) | D1, D4 | per-frame | Must stay inside the struct v2 branch (v1 ends before the field). **S5:** returns the raised value; the two populate sites use it inside the existing `eOk && ... > 0 && ... < 6` condition (no re-indent), the two GetState sites ignore it. |
| `ext::streamline::OnDlssgModuleLoad(const std::wstring& normalizedPath, LPCWSTR fullPath, HMODULE& module) -> bool handled` | decision | `LibraryLoad_Hooks.cpp` `LoadLibraryCheckW`, `nvngx_dlssg.dll` branch | D1, D4 | per-create (DLL load, any thread) | nullptr = not handled. Loads through `NtdllProxy::LoadLibraryExW_Ldr` and applies `MfgUnlock::TryApply`; early-out is a filename compare plus `Watching()`. **S5:** returns "handled" (as the oracle, which returned the load result even when null); the caller returns `module`. |
| `ext::streamline::WarnMfgOverrideIgnored(uint32_t major, minor, patch)` | hook | `Streamline_Hooks.cpp` `hkslDLSSGSetOptions`, the `else` of the Streamline >= 2.7.1 branch | D5 | per-frame | Once-only warning that an interpolation-count override cannot apply (Streamline < 2.7.1); moves the 14-line inline block out. **Added by S5** (U4 Streamline #4). |
| `ext::streamline::ApplyMenuDlssgInterlock(sl::DLSSGOptions&, bool active)` | hook | `Streamline_Hooks.cpp` `hkslDLSSGSetOptions` (the `StreamlineHooks::applyMenuDlssgInterlock` function and its `.h` declaration move to ext) | B9, D5 | per-frame (menu open, Vulkan) | Forces DLSS-G off with `eRetainResourcesWhenOff` while the menu is visible. `delayMenuRenderBy` is dropped. Depends on `menu::OnVulkanOverlay` unless dropped (see menu). |

## gpu

C6 submission ownership (`GpuLifetime`, `GpuSubmission`, `ext/gpu/`). The wrappers replace every `Hdr10::ResetCommands/ExecuteCommands`; they are inline in `ext/gpu/Gpu.h` (included by `Seams.h`) and cost one relaxed load while no owner is registered.

**Owner registry.** A feature that keeps GPU objects alive across submissions (NR, the HDR conversion pool, the AMD PreSR bridge) calls `ext::gpu::RegisterOwner(const Owner&)` once at init and then `ext::restrack::EnsureHooks(device)`. `Owner` is a table of optional function pointers: `begin` (capture a `GpuSubmission` before the real execute), `quarantine` (failure path of `begin`), `reset` (a list was Reset), `isolate` (index of a list that must run alone, AMD PreSR pending list), `before`/`after` (around each actual execute). Slots are never recycled, `SetOwnerEnabled(handle, bool)` toggles. `SubmissionOwnersActive()` (inline, feature bit `F::Gpu`) and `OwnersWantReset()` are the fast-path tests. The detour and `ExecuteCommands` run every owner in registration order: all `before`, all `begin`, execute, all `after`, all `CompleteNoThrow` (the reference build's AMD Submitting / NR / HDR / AMD Submitted order). A throwing `begin` calls that owner's `quarantine` and the submission proceeds. When the detour covers the queue, `ExecuteCommands` only executes (the detour does the owners' work), so tokens are not doubled.

Ext-internal (not seams): `ext::gpu::Scope` (RAII over a bridge-owned recording: `Reset` through the owners, `Submit(queue)` = Close + `ExecuteCommands`, an abandoned scope closes the list; the SEAMS 7 item 1 line for the recorded-Init block), `ext::gpu::DrainQueue(queue)` (5 s fence drain, device-removal tolerant; was `WaitForFfxivReleaseQueue`), `ext::gpu::ExecuteOrdered(queue, list, fence, OrderedFence&, UINT64& allocatorValue)` (the C6 half of `ext::fg::SubmitSC`: execute, `++value`, signal, `failed` on a failed signal).

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::gpu::ResetCommands(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*) -> HRESULT` | decision | `IFGFeature_Dx12.cpp` `GetUICommandList`; `IFeature_Dx11wDx12.cpp` `ProcessDx11Textures`; `dx11_with_dx12_sc.cpp` `_CopyDx11SharedToDx12FGBackBuffer`; `DLSSG_Dx12.cpp`/`XeFG_Dx12.cpp` where present | C6, C8 | per-frame | `Reset`, then every owner's `reset` callback (the oracle's `Hdr10::Reset`: `ResetRecording` + pool collect). Inline, owner callbacks run on the calling thread. Name loses `Hdr10`. **Placed (S4):** `IFeature_Dx11wDx12.cpp` `ProcessDx11Textures` and `Init`. **Open (S5/S6):** `IFGFeature_Dx12.cpp` `GetUICommandList` (FG, U2 row 6), `dx11_with_dx12_sc.cpp` copy (C8, `_hdrOutput` only). **Placed (S5):** `IFGFeature_Dx12.cpp` `GetUICommandList`. |
| `ext::gpu::ExecuteCommands(ID3D12CommandQueue*, UINT n, ID3D12CommandList* const*)` | decision | `IFGFeature_Dx12.cpp` `SubmitUICommandList`; `DLSSG_Dx12.cpp` `Present`; `XeFG_Dx12.cpp` `Present`, `Deactivate`; `IFeature_Dx11wDx12.cpp` `Evaluate`; `dx11_with_dx12_sc.cpp` copy execute | C6, C8 | per-frame | Runs the owners around the execute (see above); the HDR pool's `begin` marks conversion packets submitted so they recycle. **Placed (S4):** `IFeature_Dx11wDx12.cpp` `Evaluate` and `Init`. **Open (S5/S6):** `IFGFeature_Dx12.cpp` `SubmitUICommandList`, `DLSSG_Dx12.cpp` `Present`, `XeFG_Dx12.cpp` `Present` + `Deactivate` (U2 rows 5/8/16, owner C8), `dx11_with_dx12_sc.cpp` copy execute (C8), `UiPaste` (C11, not in this repo yet). **Placed (S5):** `IFGFeature_Dx12.cpp` `SubmitUICommandList`, `DLSSG_Dx12.cpp` `Present`, `XeFG_Dx12.cpp` `Present` + `Deactivate` (FSRFG keeps its raw calls, as in the oracle). |

## hdr

OptiHDR (C8), HDR PNG + ICC (C9), RenoDX scene HDR (C10, NOTICE shipped), F3, F4.

| Seam | Form | Host file(s) and function | Owner IDs | Frequency | Notes |
|---|---|---|---|---|---|
| `ext::hdr::Active() -> bool` | inline decision | used inside HDR seams and FG path | C8, C10 | per-frame | Requires OptiHDR's feature bit and nightly's `hdrOutputActive`; the selected output colour space is recorded in `State`. |
| `ext::hdr::DlssExt { bool sceneHdr; int available; unsigned originalFlags; }` | per-feature state | `DLSSFeature_Dx12::_ext` | C10 | per-feature | Replaces the three scene-HDR fields formerly on the DLSS feature. |
| `ext::hdr::AdjustDlssCreateFlags(DlssExt&, NVSDK_NGX_Parameter*)` | hook | `DLSSFeature_Dx12.cpp` `InitDLSS`, before `CreateFeature` | C10 | per-create | Adds `IsHDR | AutoExposure` only while bridge-initialising with scene HDR; saves the original flags in `DlssExt`. |
| `ext::hdr::RestoreDlssCreateFlags(DlssExt&, NVSDK_NGX_Parameter*)` | hook | same function, after `CreateFeature` | C10 | per-create | Restores flags after feature creation. |
| `ext::hdr::DlssEvalScope(DlssExt&, device, commands, params, width, height, handleId)` (RAII; `ok()`, `Finish(bool)`) | RAII | `DLSSFeature_Dx12.cpp` `EvaluateInternal`, around native evaluate | C10 | per-frame | Opens same-frame scene data, prepares/substitutes input/output/exposure, restores bindings and publishes the reconstructed image in `Finish`. On preparation failure, `ok()` is false and the session disables the option/recreates the backend. |
| `ext::hdr::BridgeInitScope` (RAII) | RAII | `IFeature_Dx11wDx12.cpp` `Init` around `dx12Feature->Init` | C10 | per-create | Sets `DlssSceneInput::bridgeInitialising` so DLSS creation flags are adjusted. |
| `ext::hdr::BridgeEvalScope(ID3D12Resource*, ID3D12Resource*, D3D12_RESOURCE_STATES)` (RAII) | RAII | `IFeature_Dx11wDx12.cpp` `Evaluate` around `dx12Feature->Evaluate` | C10 | per-frame | Publishes bridge colour/output/state for scene HDR input and clears them on scope exit. |
| `ext::hdr::OverlayRtvFormat(DXGI_FORMAT) -> DXGI_FORMAT` | decision | `menu_overlay_dx.cpp` overlay init, RTV format | C8 | init | Selects R8G8B8A8 for the menu layer while OptiHDR is active. |
| `ext::hdr::OverlayPrepare(ID3D12Device*, ID3D12Resource*, ImDrawData*, bool (*)(UINT64), UINT64) -> bool` | decision | `menu_overlay_dx.cpp` per-frame prepare | C8 | per-frame | Computes bounds, ensures the layer and waits for the retired resource on resize; false skips the SDR draw into HDR. |
| `ext::hdr::OverlayScope(ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_CPU_DESCRIPTOR_HANDLE)` (RAII) | RAII | `menu_overlay_dx.cpp` around `ImGui_ImplDX12_RenderDrawData` | C8 | per-frame | Composites the SDR ImGui layer into the PQ output; the overlay fence ring remains in its `[UP]` host code. |
| `ext::hdr::OverlayRelease()` | hook | `menu_overlay_dx.cpp` `CleanupRenderTargetDx12` | C8 | teardown | Releases the HDR menu layer. |
| `ext::hdr::SetResourceHdr(IFGFeature_Dx12*, int, Dx12Resource*&, Dx12Resource&, fg::Provider) -> bool` | ext-internal hook | `fg::SetResourcePre` | C8, C10 | per-resource | Handles raw scene HUD-less capture, UI tag drop and PQ HUD-less conversion before the guide matcher. |

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

`ext::game::Current()` returns a struct of interface pointers selected once at init (null for every game except a matched FFXIV build). Seam bodies test a pointer, never a `gameExe` string. `ext/ffxiv/GameBuild.h` is the only file with FFXIV addresses; the offsets hard-coded inside `ProcessFfxivQualityRequest` move there too. The current descriptor is keyed by `TimeDateStamp=0x6ac3dc96` and `SizeOfImage=0x380a000`; its native-quality and Companion RVAs, structure offsets, and byte guards are centralized in that descriptor.

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
| `ISceneLighting` | `OnUpscaleBoundary(ID3D11DeviceContext*, ID3D11Resource* color, bool hdr)`, `ConsumeHistoryReset(uint64_t& cursor)`, `Status()` (`LightingStatus`, the NR menu's lighting-scan text) | `dx11bridge::OnUpscaleBoundary`, `fg::DispatchResetHistory`; NR (A3, PreSR) calls `ConsumeHistoryReset` directly | B6, A3 |
| `INrQuirks` | `BridgedNativeDx11` | NR internals: history reset on disable, split frame work, motion-onset sampling, exposure scan off (was `gameExe == "ffxiv_dx11.exe"`) | A1-A8, B5 |
| `ISceneHdr` | `Open(ID3D12Device*, UINT, UINT, bool) -> SceneInput`, `PublishReconstructed(SceneInput&&)`, `EndFrame()` | `hdr::DlssEvalScope`, `dx11bridge::ConvertCopySource`, `dx11bridge::PresentScope` | C10 |
| `IFgMarkers` | `PresentationGuideDelay`, `OnUpscale`, `BeforePresent`, `NextFrame` | `fg::OnUpscaleStart`, `GuideAge`, `MarkersBeforePresent`, `MarkersNextFrame` | B5, D2, C5 |
| `INgxLifecycle` | `IsolatesShutdown` | `dx11bridge::ShutdownNgx`, `RedirectShutdown1`, `BeforeReleaseFeature` | C6 |

**B6 implementation (`ext/ffxiv/lighting/`):** `Provider()` supplies `ISceneLighting`; `Status()` maps the latest scan reading and capture-install state to the NR menu, and `ConsumeHistoryReset(cursor)` acknowledges each fresh lighting event independently for NR and PreSR. `OnUpscaleBoundary(ctx, color, hdr)` closes the sample only after the DX11 bridge has confirmed that the verified tone-mapped color reaches DLSS. `OnDeviceHooked` / `OnUnhook` install and detach the native shader capture hooks. The capture exposes `SetCompanionCallbacks`; C7 supplies optional function pointers for shader/layout tagging, draw observation, visibility interception, and indexed-draw wrapping, with null callbacks as the inert default.

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
| nr | 10 | 2 RAII |
| fg | 27 | 0 |
| present | 6 | 0 |
| restrack | 8 | 3 inline-guarded |
| streamline | 7 | 0 |
| gpu | 2 | 0 |
| hdr | 11 (+2 contingent) | 1 inline-guarded, 3 RAII |
| hudless | 0 | 0 |
| game | 2 (+6 interface contracts) | 0 |
| core | 2 | 1 inline-guarded |
| ffxiv-in-upstream | 0 (3 inline items) | 0 |
| **Total** | **117** | |

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

Count: 26 rows (row 26 added by S1a; #16 skipped in S1b because nightly's DLSS-SR flag is per instance) (items 1-17 and 21-25 are enumerated or implied by SEAMS 5 line 160; 18-19 are additions). The old 718-line estimate is a historical inventory only; section (d)'s current `[UP]` measurement comes from `tools\\footprint.ps1`.

# (b) `ext::Access` needs

One `friend struct ext::Access;` per class. Members listed are what the fork needs; `Access` exposes narrow static accessors only.

| Upstream class (file) | Private/protected members the fork needs | Used by |
|---|---|---|
| `IFGFeature_Dx12` (`framegen/IFGFeature_Dx12.h`); covers derived `DLSSG_Dx12` and `XeFG_Dx12` through base access | `_uiCommandList`, `_scCommandList`, `_resourceMutex`, UI/SC allocators and fences, frame-resource arrays, `_jitterX/_jitterY`, `_cameraNear/_cameraFar`, `_lastDispatchedFrame`, `_waitingNewFrameData`, `Deactivate()`, `ReleaseObjects()` | `fg::*`, `CancelPendingUpscalerWork`, `SubmitSC`, `PrepareSC`, guide matcher. **As built (S5):** the one friend line names `ext::Access`; the fork code that needs these members uses the nested `ext::Access::FgDx12` (declared in `Seams.h`, defined in `ext/fg/Dx12Access.h`, which upstream never includes). A nested class of a friend has the friend's access, so no further friend line and no accessor churn in `Seams.cpp`. No separate friend line is needed for `XeFG_Dx12`: XeFG state that the seams need is passed as arguments (`XefgSelectFactor`, `XefgBuildCamera`) or kept in `Dx12Ext::xefg`. |
| `XeFG_Dx12` (`framegen/xefg/XeFG_Dx12.h`) | `_reset`, XeFG context/session handles, `WAR_xefgRequestFGToggle`, frame-index state | `XefgPrepareHudless` (S6), rebuild-on-upscaler-change. **As built (S5, D3):** not needed for `XefgSelectFactor`, `XefgFrameTime`, `XefgNoteFrameTime`, `XefgBuildCamera` (they take values or arrays as arguments; the rebuild uses the public `DestroyFGContext()`), so no friend line in `XeFG_Dx12.h` so far. |
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
| `IFGFeature_Dx12` (`framegen/IFGFeature_Dx12.h`) | `ext::fg::Dx12Ext` | guide snapshot ring and metadata, `_guideAge`, `_retiredGuides`, `_sceneHistory`, SC fence value/failed flag, DLSS-G observed multiplier/sample mode/last options, `_historyFrames`, `_forceConstantsReset`, XeFG dynamic state (`_dynamic`, `_dynamicWasEnabled`, `_dynamicLastTarget`; as built: `Dx12Ext::xefg`, an `XefgState` created on the first XeFG Dispatch). **As built (S5):** the fields are reached through `ext::Access::FgDx12::Ext(fg)` (see section (b)). Covers `DLSSG_Dx12`, `XeFG_Dx12`, `FSRFG_Dx12` (derived), so no member is added in those classes or in `DLSSG_Common`. |
| `Dx11wDx12SC` (`with_dx12/dx11_with_dx12_sc.h`) | `ext::dx11bridge::BridgeExt` | `hdrOutput` and the C9 screenshot's borrowed SDR source/state (`screenshotSdrSource`, `screenshotSdrState`); the HudlessEngine ring is planned for S10 and is not present yet |
| `IFeature_Dx11wDx12` (`upscalers/IFeature_Dx11wDx12.h`) | `ext::dx11bridge::BridgeState` | `ForcedQualityColorScaler` (`OS_Dx12`), `ReportedForcedQualityColorScale`; freed on the GPU-idle path (C6) |
| `DLSSFeature_Dx12` (`upscalers/dlss/DLSSFeature_Dx12.h`) | `ext::hdr::DlssExt` | `sceneHdr`, cached `available` state, and saved `originalFlags` for feature creation |

Single-field exceptions kept inline (hot path, per resource or per present, marked `// ext:`): `Dx12Resource::hdrEncoded` (`IFGFeature_Dx12.h`), `Dx11wDx12SC::_copySubmissionFailed` (in the `[UP]` fence commit), `WrappedIDXGISwapChain4::_composition`.

# (d) Measured footprint

Basis: added plus removed lines against upstream base `fd6c2870`, reported by `tools\footprint.ps1`. For the committed S6 stack at `373dab43`: **58 upstream files; +1115/-266 = 1381 total lines; `[UP]` 787 lines; persistent seam surface 594 lines.** The current policy in [SEAMS.md](SEAMS.md) section 1.3 is persistent surface <= 600 lines. `[UP]` is reported separately; touched-file count is informational. The S6 stack has six lines of persistent-surface headroom after C9. S7 and S10 are not included in this measurement; measure and explain their actual changes at those wave boundaries. The former <=40-file/<=1000-total projection and the 122-line `OS_*` resampler estimate are obsolete.
# (e) Unresolved conflicts between ledgers that SEAMS.md does not settle

1. **Recorded Init dependency on C6/C10.** SEAMS 5 makes it `[UP]`; U3 proposed an out-of-line `ext::dx11bridge::InitFeature` using `Hdr10::ResetCommands/ExecuteCommands`. A self-contained `[UP]` commit can only use raw `Reset`/`ExecuteCommandLists`; the C6 and C10 series then add inline `ext::gpu::*` and `ext::hdr::BridgeInitScope` lines inside the block (+4-6 lines). Decide whether Init needs GpuLifetime registration at all (U3 notes the Vulkan variant uses raw calls). The catalog assumes raw in `[UP]`, one RAII line from C10, no `ext::gpu` in Init.
2. **Bridge fence/allocator hardening scope.** U3 (`ProcessDx11Textures`, 13 lines) and U5 (`dx11_with_dx12_sc.cpp`, ~44 lines) call it generic and upstream-worthy; SEAMS 5 does not list it. Catalog: `[UP]` rows 18-19. Alternative: inline `// ext: C6`.
3. **Menu tone-map under OptiHDR (resolved in C8).** `UseSdrUiTarget()` returns SDR from `getUiTargetMode()` while the menu layer composites into the HDR output. Phase 4 should verify visual parity with the oracle.
4. **XeFG camera build.** U2 offers a seam (`XefgBuildCamera`, chosen: ~37 lines of footprint) or an inline `[UP]` fix with only non-mutating near/far (~8 lines, loses `BuildView` validation). SEAMS 5 lists the XeFG barrier but not the camera fix.
5. **Detours transaction lock.** U4 proposes one process-wide `ext::core::DetourLock` shared with ResTrack/DXGI hooks; SEAMS 5 puts the Streamline mutex under `[UP]` (self-contained, file-local) and lists `DetourLock` under `ext/core`. The file-local `[UP]` mutex and the ext lock do not exclude each other. Decide whether the `[UP]` commit uses a header-declared shared mutex or the race between Streamline hooks and ext hooks is accepted.
6. **Forwarder rename and `Kernel_Hooks`.** Rename is pending the S2 check (SEAMS 5). Contingent +7 lines in `Kernel_Hooks.cpp` and a `.sln`/package-path rename. Also affects U1 `Streamline_Proxy.h` `FindFilePath(exePath, "nvngx_dlssnr.dll")` naming.
7. **Back-buffer release quirk.** U4 proposed a `GameQuirk::ReleaseBackbufferOnce`; SEAMS 3.8 prefers `ext::game` interfaces, but 5 keeps a quirk for C4. Catalog uses the quirk (extends one GameConfigs line, 3 more lines in `Quirks.h`/`dllmain.cpp`). Alternative: a seam in `FG_Hooks.cpp` through a game interface (no FFXIV-visible upstream lines, +1 interface).
8. **`FrameLimit` interval.** U1: `ext::present::FrameInterval` seam vs. sending the whole file upstream. The FG-doubling semantics ("cap counts generated frames") is fork policy, not a bug fix; catalog keeps the seam and sends only the timer fixes.
9. **`menuOverlayIsVulkan` / Vulkan DLSS-G interlock.** U4 keeps `ApplyMenuDlssgInterlock` (minus `delayMenuRenderBy`); U5 asks whether nightly already covers the menu delay. Without the delay the interlock may be a no-op. S6 decides; if dropped: -`menu::OnVulkanOverlay`, -`streamline::ApplyMenuDlssgInterlock`, -2 files' lines.
10. **Generic menu behaviour not covered by "cosmetic" in SEAMS 3.6.** A11 preset live-apply (U5 rows 16-19), OptiFG state text (row 26), `SaveIni` bool result and "Settings saved" text (rows 37-38 and `Config.cpp` hunks 18, 19). Catalog: not carried. Confirm.
11. **Footprint target (resolved).** Section (d) records actual S6 counts and the current policy: persistent surface <=600 lines, `[UP]` reported separately, file count informational. Future waves measure their actual contribution at completion.
12. **`OS_Dx12` sizing policy (resolved in SEAMS 7 A8).** NR uses its own `NrResampler_Dx12`/`NrResampler_Vk` and leaves the upstream `OS_*` callers unchanged; the previous inline-size estimate does not apply.
13. **`IFeature_Dx12::Evaluate` `timingQueue` parameter.** U3 keeps a 2-line signature extension; alternative is a thread-local `ext::nr::CurrentQueue` set by the bridges. Catalog keeps the signature (smaller, visible).
14. **XeFG interpolation-count behaviour.** The fork rewrite stops writing `FGXeFGInterpolationCount` back to max; SEAMS settles the 1..7 range but not this behaviour change. `XefgSelectFactor` documents it; Phase 4 should confirm with the oracle log.
15. **`ext::fg::OnUpscaleStart` per-frame cast.** Removing both `IFGFeature` virtuals means ext casts `IFGFeature*` to `IFGFeature_Dx12` each frame unless cached in `Dx12Ext` at activation. Catalog assumes the cache; verify there is no path where the FG object changes without `OnActivated`.
16. **C3 gate.** SEAMS 5 settles the design (both halves, default off). U1 shows the key has no reader in the oracle and U3 H5b shows an ungated minimum; S7 must compare the gated default-off behaviour against the oracle for FFXIV before `ResolveRenderGeometry` merges (flagged, not a conflict).
