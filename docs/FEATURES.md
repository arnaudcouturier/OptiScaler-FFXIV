# Feature reference

What this fork adds to OptiScaler, where each piece lives, how it attaches to upstream files, and how to configure it. The short status table is in [.github/README.md](../.github/README.md); this file is the long version and uses the same status words.

**Status words.** *tested* means the maintainer played the game with it on an RTX 4090. *untested* means it compiles and follows the same code paths as the tested features, but nobody has verified it on real hardware (or in game) yet. Where a feature runs inside a tested path but was never exercised alone, the entry says so. Nothing here is a promise that a feature works on your system.

**How to read the entries.**

- **Code:** directories and key files under `OptiScaler/ext/`. Every file the fork owns is under `OptiScaler/ext/`.
- **Seams:** the one-line calls in upstream files that reach the fork code, by name. The full list with host files is [SEAMS-catalog.md](SEAMS-catalog.md); the rules are in [SEAMS.md](SEAMS.md).
- **Config:** `[Section] Key` as written in `OptiScaler.ini`. The defaults and the explanation of each key are in the matching fragment under `OptiScaler/ext/ini/`, which `tools/package.ps1` merges into the shipped `OptiScaler.ini`.
- **Menu:** the card in the in-game OptiScaler menu.

## Summary

| Feature | Status | Section |
|---|---|---|
| DLSS Neural Rendering (NR) with pre-SR multipass | tested | [NR](#dlss-neural-rendering-nr) |
| NR native path (module identity, CUDA launch hooks) | tested (part of NR) | [NR native path](#nr-native-path-and-vit-capture) |
| NR ViT capture | diagnostic only | [NR native path](#nr-native-path-and-vit-capture) |
| NR ViT reuse (`VitEvery`) | untested | [NR native path](#nr-native-path-and-vit-capture) |
| AMD HIP pre-SR | untested | [Other NR paths](#other-nr-paths) |
| Vulkan NR path | untested | [Other NR paths](#other-nr-paths) |
| OptiHDR (HDR10 output) | tested | [HDR](#hdr) |
| RenoDX scene HDR | tested | [HDR](#hdr) |
| HDR PNG capture | untested | [HDR](#hdr) |
| DLSS-G multi frame generation unlock, RTX 40 | tested | [Frame generation](#frame-generation) |
| DLSS-G MFG loader for RTX 30 | untested | [Frame generation](#frame-generation) |
| Dynamic MFG readout and RTX 50 behaviour | untested | [Frame generation](#frame-generation) |
| Frame pacing, Reflex and Streamline hardening | runs inside the tested DLSS-G path | [Frame generation](#frame-generation) |
| XeFG / XeMFG | untested | [XeFG](#xefg-and-xemfg) |
| HUD-less frame generation | opt-in; SDR with DLSS-G 2x-4x tested, HDR untested | [HUD-less](#hud-less-frame-generation) |
| ReShade built-in add-on | tested as part of HUD-less, ReShade 6.8.0 only | [HUD-less](#hud-less-frame-generation) |
| Window mode (log probe, border fix, deferred mode switch) | opt-in, off by default; untested in game | [Window mode](#window-mode) |
| FFXIV native quality, render geometry, colour scaling | tested | [FFXIV module](#ffxiv-module) |
| FFXIV lighting scan | runs inside the tested NR path | [FFXIV module](#ffxiv-module) |
| FFXIV companion / nameplates | tested | [FFXIV module](#ffxiv-module) |
| FFXIV composition swapchain hook | untested on its own | [FFXIV module](#ffxiv-module) |
| Character preview DLSS and spatial AA (CharaView) | opt-in, off by default; untested in game | [CharaView](#character-preview-charaview) |
| Recovered items F2, F3, F4 | untested | [Recovered items](#items-recovered-from-nickles-build) |

## DLSS Neural Rendering (NR)

**What it does.** Runs NVIDIA's DLSS Neural Rendering model on the frame, either before ordinary super resolution (pre-SR) or after it, in one to three passes with per-pass profiles. A colour pipeline maps the game's image into what the model expects and back (white point, exposure, reversible tone mapping). It also contains the exposure scan, which reads the game's exposure from resource creation and unordered-access views, and named user presets.

**Status.** tested (RTX 4090), pre-SR multipass included. Exposure scan, white point, presets, compare views and the debug view are used by the tested sessions but not tested separately. `DeferredDLSS`, `ResidualFG`, spatial compression and `WorkingScale` other than 1.0 are experimental or opt-in and untested.

**Code.**

- `OptiScaler/ext/nr/pass/DlssNr_Dx12.cpp`: the Direct3D 12 pass (compute shaders in `ext/nr/shaders/`, compiled at build time).
- `OptiScaler/ext/nr/presr/`: pre-SR placement, pass profiles, cadence, trail guard, split frame work.
- `OptiScaler/ext/nr/exposure/DlssNr_ExposureScan.cpp`: exposure scan.
- `OptiScaler/ext/nr/NrResampler_Dx12.cpp`: the NR-owned resampler used for `WorkingScale`.
- `OptiScaler/ext/nr/presets/NrUserPresets.h`: named user presets.
- `OptiScaler/ext/nr/forwarder/dlssnr_forwarder.cpp`: the `nvngx.dll_dlssnr.dll` forwarder (second project in `OptiScaler.sln`). The odd file name is deliberate: NVIDIA's model only answers a caller whose module path contains `nvngx.dll`.

**Seams.** nr: `EvalBefore`, `EvalAfter`, `PreSrScope`, `NormalizeDlssPresets`, `WrapNvapi`, `NeedsDlssgRuntime`, `AppendNgxSearchPaths`. restrack: `OnCommandListReset`, `WantsCommandListReset`, `NoteUav`, `NoteResourceCreated`, `WantsResourceCreateHooks`, `OnDeviceHooked`, `ReleaseQueueHook`, `OnFeatureRelease`. dx11bridge: `OnNgxInit`, `OnNgxShutdownBegin`. menu: `MergeBoxes`, `PollShortcuts`, `HandleShortcuts`, `KeybindRows`, `DrawOverlays`, `WantsFrame`, `ShaderTimeRows`. gpu: `ResetCommands`, `ExecuteCommands`.

**Config.** `[DlssNr]` (80 keys): `Enabled`, `DisableOnFocusLoss` (experimental, off by default: pauses NR while the game window is not focused), `ToggleKey`, `RunBeforeSR`, `Passes`, `PassFeedback`, `Pass2*`, `Pass3*`, `Preset`, `Style`, `Intensity`, `WorkingScale`, `ReversibleMode`, `WhitePointSource`, `ScanAnchors`, `LightingHistory`, `SkinProtection`, `PreSRTrailGuard`, `SplitFrameWork`, `Compare*`, `DebugView`. `[Hotfix] RenderSizeFromWidthHeight` and `[QualityOverrides] ForcePerfQuality` belong to the FFXIV module.

**Menu.** `dlss_nr` (tab Upscaling), plus an overlay for the compare tags and the exposure-scan lamp, a GPU-time row, and the NR on/off shortcut in the Keybinds list.

**Needs from you.** NVIDIA's `nvngx_dlssnr.dll`; see [Install](../.github/README.md#install).

## NR native path and ViT capture

**What it does.** The fork drives the NR model's own DLL directly. It records which CUDA entry points the game queried (`WrapNvapi`), identifies the model's CUDA module by size and SHA-256, and can skip repeated vision-transformer launches (`VitEvery`) for a module that has been verified. Unknown modules are never touched: reuse stays off and the log says why.

**Status.** The native path is tested as part of NR. `VitCapture` is a diagnostic that only observes. `VitEvery=2` reuse is untested for speed: it only activates for a module listed as verified in `ext/nr/native/DlssNrModuleProfiles.h`, and is a no-op otherwise. `VitAllowUnverified=true` skips the fingerprint check and can hang the GPU; leave it off.

**Code.** `OptiScaler/ext/nr/native/` (`DlssNrNative.cpp`, `DlssNrModuleProfiles.h`, `DlssNrVitCapture.h`, `DlssNrVitReuse.h`), `OptiScaler/ext/nr/hybrid/`. The capture summariser is `tools/nr-vit-capture-summary.ps1`; how to profile a new model file is in [nr-vit-reuse-profile-runbook.md](nr-vit-reuse-profile-runbook.md).

**Seams.** nr: `WrapNvapi`, `AppendNgxSearchPaths`.

**Config.** `[DlssNr] VitCapture`, `VitFirstPassOnly`, `VitEvery`, `VitAllowUnverified`, `Precision`.

**Menu.** Status text in the `dlss_nr` card.

## Other NR paths

**AMD HIP pre-SR.** Runs the pre-SR pass on AMD devices through a small bridge. Status: untested (community). Code: `OptiScaler/ext/nr/amd/` (`AmdBridge.cpp`, `AmdPreSr.cpp`, shaders). Seams: nr `PreSrScope` (the AMD edit is chosen inside it), restrack `OnDeviceHooked`. Config: none of its own; it follows `[DlssNr] RunBeforeSR`.

**Vulkan NR path.** NR for native Vulkan games, kept for reuse with other games; FFXIV never reaches it. Status: untested. Code: `OptiScaler/ext/nr/vk/`, `OptiScaler/ext/nr/NrResampler_Vk.cpp`. Seams: nr `VkEvalBefore`, `VkEvalAfter`, `VkDeviceExtScope`. Config: follows `[DlssNr]`.

## HDR

All HDR code is under `OptiScaler/ext/hdr/`. The four parts share one set of seams.

**OptiHDR (HDR10 output).** Presents the D3D11 game through OptiScaler's DX12 presenter as HDR10 (R10G10B10A2, PQ). In the default mode the game's SDR image is expanded to HDR. Windows HDR must be on for the game display before launch.

- Status: tested (RTX 4090).
- Code: `ext/hdr/Hdr10.cpp`, `PresentHdr.cpp`, `BridgeHdr.cpp`, `FgHdr.cpp`, `ext/hdr/menu/` (menu layer composited into the HDR output), `ext/hdr/shaders/Hdr10.hlsl`.
- Seams: present `PlanDx12Presenter`, `PatchPresenterDesc`, `LogPlainPresenter`. dx11bridge `ConfigureOutput`, `OnDestroy`, `OutputFormat`, `OnResizeDone`, `ConvertCopySource`, `SetColorSpace`. fg `SetResourcePre`, `SetResourcePost`, `NormalizeFormat`, `DlssgTagHudless`, `XefgPrepareHudless`, `AdjustConstants`. menu `AdjustFgOutputOptions`, `UseSdrUiTarget`, `AdjustTheme`. gpu `ResetCommands`, `ExecuteCommands`. hdr `OverlayRtvFormat`, `OverlayPrepare`, `OverlayScope`, `OverlayRelease`.
- Config: `[HDR] FfxivHDR` (restart after changing), `PeakNits`, `PaperWhiteNits`, `HighlightExpansion`, `Contrast`, `Saturation`, `Vibrance`.
- Menu: `ffxiv_hdr` (tab Frame Gen, shown for FFXIV).

**RenoDX scene HDR (`[HDR] Mode = 1`).** Replays the game's tone-mapping stages in FP16 next to the SDR chain, so HDR output keeps the highlights the SDR path clips. The SDR chain, ReShade and the HUD are untouched. Two experimental switches feed the linear scene into NR (`NRSceneInput`) or DLSS (`DLSSSceneInput`).

- Status: tested (RTX 4090) for `Mode = 1`; `ReShadeHighlights`, `NRSceneInput`, `DLSSSceneInput` are experimental and untested.
- Code: `ext/hdr/scene/` (`FfxivSceneHdr.h`, `SceneHooks.cpp`, `DlssSceneInput.h`, `NrSceneInput.h`), `ext/hdr/DlssHdr.cpp`, `ext/hdr/shaders/renodx/` (RenoDX shaders; licence and notice shipped as `Licenses/RenoDX_*`).
- Seams: hdr `DlssExt`, `AdjustDlssCreateFlags`, `RestoreDlssCreateFlags`, `DlssEvalScope`, `BridgeInitScope`, `BridgeEvalScope`. dx11bridge `PresentScope`, `BeforeSharedCopy`. fg `SceneRawHudless`.
- Config: `[HDR] Mode`, `ReShadeHighlights`, `NRSceneInput`, `DLSSSceneInput`.
- The scene stages are recognised by shader fingerprint, so a game patch that changes those shaders disables it; see [FFXIV-PATCH-RUNBOOK.md](FFXIV-PATCH-RUNBOOK.md).

**HDR PNG capture.** Saves one PNG on a key press, as SDR for sharing or as HDR, with BT.2100 PQ metadata (an ICC-only variant exists for viewers that mishandle the cICP chunk).

- Status: untested (the maintainer does not use it day to day); the PNG writer has unit tests.
- Code: `ext/hdr/screenshot/`.
- Seams: dx11bridge `PrepareScreenshot`; the menu variant (`ScreenshotMenu`) is taken inside `hdr::OverlayScope` with no extra upstream line.
- Config: `[HDR] ScreenshotKey`, `ScreenshotFormat`, `ScreenshotIccOnly`, `ScreenshotMenu`.
- Menu: key row in the Keybinds list; options in the `ffxiv_hdr` card.

## Frame generation

**DLSS-G multi frame generation unlock, RTX 40.** Raises the frame-generation ceiling that the driver reports for GeForce RTX 40 cards, in memory only (the `nvngx_dlssg.dll` on disk is never modified), and applies a targeted temporal correction. Only supported runtime builds are patched; an unknown build is left alone.

- Status: tested (RTX 4090) at 2x to 4x. 5x and 6x are experimental; the maintainer saw one GPU hang at 6x.
- Code: `ext/fg/dlssg/MfgUnlock.cpp`, `AdaTemporal.h`, `DlssgSeams.cpp`; `ext/fg/streamline/StreamlineSeams.cpp`.
- Seams: streamline `MfgUnlockTick`, `MfgCeilingStale`, `RaiseMfgCeiling`, `OnDlssgModuleLoad`, `WarnMfgOverrideIgnored`. fg `AdjustMfgMax`, `AdjustDlssgOptions`, `AdjustReflexOptions`, `AfterDlssgSetOptions`, `OnActivated`.
- Config: `[DLSSG] AdaMfgUnlock` (save and restart after changing). Do not combine with an external unlocker.
- Menu: `fg_unlocks` (tab Frame Gen).

**DLSS-G MFG loader for RTX 30.** Loads a companion DLSS-G runtime from `OptiScaler/dlssg_sm86` so Ampere cards can generate more than one frame. Status: untested. Code: `ext/fg/dlssg/AmpereMfgLoader.cpp`. Seams: streamline `OnLoadStreamline`. Config: `[DLSSG] AmpereMfgUnlock`, `AmpereMfgMaxFrames`, `AmpereMfgKernelImage`, `AmpereMfgRouter`, `AmpereMfgHardwareBilinear`, `AmpereMfgOptimized`, `AmpereMfgPreset`, `AmpereMfgSpoofArchToGame`, `AmpereMfgLogLevel`. Menu: `fg_unlocks`. You must supply the companion DLL yourself.

**Dynamic MFG on RTX 50.** The fork reads back the multiplier that DLSS-G actually applied and shows it, and sends the frame limit you set to Reflex in dynamic mode. Status: untested. Code: `ext/fg/MenuFgStatus.cpp`, `ext/fg/dlssg/DlssgSeams.cpp`. Seams: menu `ObservedMultiplier`; fg `AfterDlssgSetOptions`, `AdjustReflexOptions`. Config: upstream keys `[DLSSG] ForceDMFG`, `OverrideForceDMFG`.

**Pacing, markers and hardening.** The frame-rate cap counts generated frames when NVIDIA frame generation is active (`FrameInterval`); for FFXIV the fork sends its own simulation and render latency markers so Reflex pacing sees the game's real frames (`ext/ffxiv/markers/`); ordered swapchain-command submission, guide matching for the history reset, and several Reflex and Streamline fixes that are carried as small `[UP]` commits (the list is in [SEAMS-catalog.md](SEAMS-catalog.md), section (a)). Status: runs inside the tested DLSS-G path, not tested separately. Code: `ext/fg/` (`FrameInterval.cpp`, `GuideMatcher.cpp`, `GuideSeams.cpp`, `MarkerSeams.cpp`, `Dx12Sc.cpp`), `ext/gpu/`. Seams: present `FrameInterval`; fg `MarkersBeforePresent`, `MarkersNextFrame`, `OnUpscaleStart`, `GuideAge`, `DispatchResetHistory`, `PostDispatch`, `ForceConstantsReset`, `SubmitSC`, `PrepareSC`, `ReleaseGuides`, `OnDeactivating`; gpu `ResetCommands`, `ExecuteCommands`. Config: `[LowLatency] Input`, `Output` (menu selection saved). Marker pacing needs a matched FFXIV build.

## XeFG and XeMFG

**What it does.** Unlocks Intel's multi frame generation in `libxess_fg.dll` for any adapter by patching the loaded image (never the file), selects the interpolation factor from real frame timing (dynamic MFG), spreads generated frames in time (extra pacing), builds a validated camera from the game's data, and rebuilds the context when the upscaler is released so the pacing history starts clean. Unknown provider builds are left untouched.

**Status.** untested (community).

**Code.** `ext/fg/xefg/` (`XeFGUnlock.h`, `XeFGDynamic.h`, `XeFGPacing.h`, `XeFGCamera.h`, `XeFGHdr.h`, `XefgSeams.cpp`).

**Seams.** menu `XeFgExtras`; fg `XefgSelectFactor`, `XefgFrameTime`, `XefgNoteFrameTime`, `XefgBuildCamera`, `OnXefgProviderLoaded`, `XefgPrepareHudless`; dx11bridge `BeforeReleaseFeature`; config `SaveXeFG`.

**Config.** `[XeFG] DynamicMFG`, `DynamicTargetFPS`, `UnlockMFG`, `MaxInterpolatedFrames` (1 to 7), `ExtraPacing`, `ResetOnUpscalerChange`. Upstream's own `[XeFG] InterpolationCount` is validated against the same limit.

**Menu.** Extra rows inside upstream's XeFG card.

## HUD-less frame generation

**What it does.** With DLSS-G at 2x to 4x, frame generation works on the UI-free image and the UI is pasted back at the real frame rate, so the HUD stays sharp in fast camera motion. Two modes: `ui_free` (UI-free base image plus pasted UI) and `recomposition` (DLSS-G recomposes the UI from a UI-free and a UI image). FSR-FG and XeFG only receive the tags in `recomposition` mode. Any doubt (title screens, fades, a refused capture) falls back to normal frame generation for that frame.

**Status.** Opt-in, off by default. Tested in SDR with DLSS-G 2x to 4x on an RTX 4090; no visible gain was reported. HDR output with `ui_free` is untested. One GPU hang happened at the experimental 6x unlock after the mode was toggled quickly. Design, measurements and limits: [HUDLESS.md](HUDLESS.md).

**Code.** `ext/hudless/` (engine, ring of shared textures, mask and paste shaders, provider adapters), `ext/ffxiv/ui/` (finds the first UI draw in FFXIV by shader fingerprint and snapshots the scene there).

**Seams.** hudless `PreInteropCopy`, `UiFreeFrame`, `PostInteropCopy`, `OnReleaseInterop`; present `BeforeOverlay`; fg `AdjustDlssgOptions`; menu `ObservedMultiplier` (counters).

**Config.** `[HudLess] Mode` (`off`, `recomposition`, `ui_free`), `MaskThreshold`, `MaskDilation`, `PasteCleanup`, `SteadyTiming`, `CoverageGuard`, `ShowDetectedUi`, `ReShadePresets`, `Diagnostics`; `[DLSSG] UIRecomposition`.

**Menu.** `fg_hudless` (Frame Gen); `hudless_diag` (Frame Gen, diagnostics).

### ReShade built-in add-on

**What it does.** OptiScaler registers itself with your ReShade as an add-on (API 20). At the UI boundary it asks ReShade to apply its enabled presets in place, so the UI-free image has the presets and the HUD stays unfiltered, and ReShade's own end-of-frame pass is skipped for that frame. If ReShade is absent, is another version, or anything fails, HUD-less falls back to normal frame generation.

**Status.** Tested as part of the HUD-less sessions, with ReShade 6.8.0 only.

**Code.** `ext/reshade/` (the public ReShade API headers are vendored unmodified in `ext/reshade/third_party/reshade`; licence notice shipped as `Licenses/ReShade_ATTRIBUTION.txt`).

**Seams.** reshade `OnReshadeLoaded`.

**Config.** `[HudLess] ReShadePresets` (`auto`, `on`, `off`), `ReShadeProbe` (diagnostics).

**Menu.** `reshade_addon_probe` (tab Compatibility, diagnostics).

## Window mode

**What it is for.** FFXIV runs borderless when `[XeFG] ForceBorderless = true` (a setting the fork does not turn on by itself). In that mode upstream's hook swallows the game's request to change the window mode and restyles the window itself. Two problems come from that, both seen in logs: after leaving the pseudo fullscreen the window keeps a thin edge (the client area becomes 3836x2156 inside a 3840x2160 window), and one window-mode switch made while DLSS-G was running stalled Streamline's frame pacer, which logged "Potential dead-lock in Present - please make sure DLSS-G is disabled when making any modifications to the window" and ended with the GPU device removed.

**Zero-code advice.** In the game's display settings choose "Borderless Window", and restart the game after changing the mode. Do not switch to "Fullscreen" while the game runs. Without any switch the window is clean and nothing here is needed.

**Status.** Opt-in, off by default, untested in game. The rules and the decision flows have unit tests on fakes (frame generation, window, clock and swapchain are replaced by test doubles) and the window calls are tested on a real hidden window; the game itself decides whether it tolerates a held-back window-mode call (see "Open points" below).

**Config.** `[Window] Mode`: `off` (default: nothing runs and nothing is logged), `probe` (log only), `on` (probe, border fix and deferred mode switch). Applies after a game restart; any other value counts as `off`. With `off` the module costs one atomic load and one branch at each of its four entry points (bridge resize, Present entry, Present exit, ReShade event), and the ReShade event is not even subscribed.

**Probe.** With `probe` or `on`, one line starting with `Window:` is written (a) after every successful resize of the bridge swapchain and (b) for every window-mode call (`IDXGISwapChain::SetFullscreenState`) that ReShade sees, before the call. Fields: the window's style and extended style, window and client rectangles, frame generation state (`active`, `paused`, `off`, `none`) and whether DLSS-G is confirmed off, `SCExclusiveFullscreen` (and its value at the previous resize), `ForceBorderless`, the window's owner thread id, the current thread id and the thread that presents, and for the call: its argument, the swapchain ReShade passed and the bridge swapchain side by side, the verdict and the value the handler would return. The probe only reads (window styles and rectangles, frame generation state, thread ids) and logs; it changes no window and never prevents a call. The ReShade event needs ReShade (the log line `Window: ReShade set_fullscreen_state event registered` at start-up says whether it was subscribed).

**Border fix (`on`).** When `SCExclusiveFullscreen` goes from true to false, the module arms a fix. The flag is sampled at every bridge resize and at the end of every game present (the hook flips it without any resize necessarily following; the logged startup enter has none). From the end of a game present, once DLSS-G is confirmed off, it reads the window's styles. If the style has neither a caption nor a sizing frame but the extended style carries `WS_EX_CLIENTEDGE` or `WS_EX_WINDOWEDGE`, it clears those bits and applies them with `SetWindowLongPtr(GWL_EXSTYLE)` plus `SetWindowPos(NOMOVE | NOSIZE | NOZORDER | NOACTIVATE | FRAMECHANGED)`; the client area is again as large as the window. A window with a caption or a sizing frame (a real windowed mode) is never touched. "DLSS-G confirmed off" means one complete game present that started and ended with frame generation inactive, with DLSS-G as the output (Streamline applies "off" inside the next present). The fix never runs while frame generation is active (it can come back about ten frames after a Deactivate); it waits for the next off period, gives up after 120 checks or when 600 presents or 30 s have passed since it was armed, and runs only on the window's owner thread (otherwise it logs `owner thread differs, border fix skipped` once and does nothing). It is never called inside `ResizeBuffers` or a paced present.

**Deferred mode switch (`on`).** A ReShade add-on event runs before the game's `IDXGISwapChain::SetFullscreenState`. When all of these hold, the call is held back and the handler returns `true` (ReShade's "prevent"): `[XeFG] ForceBorderless` is on, the output is DLSS-G, frame generation is active, the call would really change `SCExclusiveFullscreen` (a repeat of the state in effect is not held), the call is for the bridge swapchain, its target output is null or the window monitor (the replay passes a null target), and the window's owner thread, the calling thread and the presenting thread are one thread. The module then asks frame generation off through the feature API (what upstream's hook does first: `UpdateTarget` and `Deactivate`), remembers the swapchain (no extra reference is taken; the hold is dropped, unreplayed, when the bridge swapchain changes or is destroyed, and the replay only runs on the live bridge), and from the end of a game present, once DLSS-G is confirmed off (at least 2 complete presents with frame generation off and at least 50 ms since the request), replays `SetFullscreenState(state, null)` directly on the swapchain ReShade passed (not through ReShade, so the event is not entered again). Upstream's hook and Streamline then see an ordinary call with frame generation already off. A second call while one is held keeps only the latest; if the latest asks for the state already in effect the held call is dropped. The wait is bounded: after 6 presents or 2 s the call is replayed if frame generation is off at that moment; frame generation can come back after about ten frames, so every present that finds it active asks it off again and the replay never runs with it active; only the hard limit (24 presents or 6 s) replays regardless, with a warning (on a presenting thread that is not the window owner thread the hold is dropped with a warning instead of being replayed). Anything unexpected passes the call through as today. The monitor argument of the event is logged but never used. What the game receives for a prevented call is decided by ReShade and is not documented in the local headers; the log records only what this handler returned.

**Log lines.** All start with `Window:` (the logger puts the function name in front). `module active, mode=...` once; `ReShade set_fullscreen_state event registered` at start-up; `resize-done ...` and `set-fullscreen-state ...` probe records; border fix: `border fix armed`, `border fix waiting`, `border fix applied exstyle A -> B client WxH -> WxH`, `border fix gave up`, `border fix FAILED`, `owner thread differs, border fix skipped` (warning, once); mode switch: `SetFullscreenState(TRUE|FALSE) held back ...; handler returns true (prevent)`, `replaces the held call`, `held call dropped`, `frame generation came back while a call is held`, `replaying SetFullscreenState(...)` with the reason, `replayed SetFullscreenState(...) returned 0x...`. A probe record ends its call part with `verdict=` (`defer` or `pass(reason)`), `wouldReturn=` (what `on` would return) and `returned=` (what was returned).

**Complete presents.** "DLSS-G confirmed off" counts only game presents that really reached the frame generation present. The bridge advances its back buffer index only after the frame generation swapchain's `Present` returned success, with frame generation on or off, and never on one of its early error returns; the module reads that index at the start and at the end of each game present. A changed value (not "greater": the index wraps) is a complete present, an unchanged one resets the off count and is not counted. With fewer than two buffers the index cannot change, and with no bridge known (or a different bridge by the end of the present) nothing can be told: such a present is counted as it always was. Log lines: `the bridge present counter is live` (once: the positive proof), `the bridge present counter is not available (fewer than two buffers)` (once), `a game present ended before the frame generation present ... not counted` (the first five). Failed presents delay the confirmed replay by at most the soft wait limit (6 presents or 2 s) described above, and a held call is never kept longer because of them. The soft limit itself replays whenever frame generation is not active at that moment (true right after the request), so a run of failed presents still ends in a replay at the soft limit although no counted present has gone by; the log then says `wait limit reached` instead of `frame generation confirmed off`.

**Open points, continued.** (6) The live side's two out-calls (the frame generation request in the order `fgChanged`, `UpdateTarget`, `Deactivate`, and the replay with a null target) are tested on fakes and by a source check that the live code goes through them; the module lock being given up around each call out has no offline test and is read in code only. (7) If the bridge's swapchain has a single buffer the present counter is unavailable (see "Complete presents") and a present that failed before the frame generation present cannot be told apart; the gate then behaves as before this check.

**Open points.** (1) What ReShade hands back to the game for a prevented call is not documented in the vendored headers; the module logs what its handler returned (`true`) and nothing else, and the game may or may not accept a call that is carried out a few frames later. (2) Which thread owns the game window is not known from the logs; the probe prints it next to the calling and presenting threads, and every action is skipped (the call passes through as today) unless they are one thread. (3) Whether FFXIV rewrites its own frame-less style after the fork's leave branch has run is inferred from window sizes in a log, not observed; the probe lines show the style before and after, and the border fix keeps checking for a while and then gives up without touching a window that still has a caption. (5) The hold cannot cover a game `ResizeBuffers` that changes the size: no ReShade event exists before a resize, and a size-changing resize runs Streamline's flush at once, before any present has applied "off". So the call is held only when the FG swapchain already has the window's size (the probe line prints `swapchain=WxH` next to `client=`); otherwise it passes through with a warning `the follow-up ResizeBuffers is not protected`. The border fix must therefore have made the client 3840x2160 first: do the risky switch (leave or enter with frame generation on) only after a border-clean leave. (4) Whether the replayed call, made with frame generation off, avoids the Streamline stall can only be seen in the game: look for the absence of `Potential dead-lock in Present` after a `replaying SetFullscreenState` line.

**Code.** `ext/window/`: `WindowRules.h` (pure rules: border rule, DLSS-G off gate, thread rule, defer verdict), `WindowDeferral.h` (the Idle/Pending/Replaying machine), `WindowProbe.h` (the line format), `WindowController.h` (decisions, driven through an `Env` interface so tests use fakes), `WindowWin32.cpp` (window reads and the extended-style call), `WindowModule.cpp` (live `Env`, entry points), `WindowConfig.h`, `WindowMode.h`, `WindowSeams.h`.

**Seams.** None in upstream files. The module is called from the existing fork seam bodies `dx11bridge::OnResizeDone` and `dx11bridge::PresentScope` (`ext/hdr/BridgeHdr.cpp`) and from a ReShade add-on event (`ext/reshade/ReShadeEvents.cpp`).

## FFXIV module

Everything that is specific to FFXIV is under `OptiScaler/ext/ffxiv/`. It activates only when the running `ffxiv_dx11.exe` matches the supported build recorded in `ext/ffxiv/GameBuild.h` (see [FFXIV-PATCH-RUNBOOK.md](FFXIV-PATCH-RUNBOOK.md)); on any other executable the pieces below do nothing. The module implements the interfaces in `ext/game/`.

**Native quality and forced quality.** Reads the game's own DLSS quality setting and its live changes, answers the game's size query with the preset you choose (`ForcePerfQuality`), requires true DLAA where the game asks for it, and lets the render size follow. Status: tested. Code: `ext/ffxiv/quality/` (`FfxivNativeQualityHooks.h`, `QualityProvider.cpp`), `ext/upscaler/ForcedQuality.h`, `ext/upscaler/UpscalerSeams.cpp`. Seams: upscaler `EffectivePerfQuality`, `OnOptimalSettings`, `ApplyForcedQuality`. Config: `[QualityOverrides] ForcePerfQuality`. Menu: `forced_quality` (tab Upscaling). Fork-added entries in upstream tables: the `ffxiv_dx11.exe` entry in `GameConfigs.cpp` gains the quirks `ScaleDisplayColorForForcedQuality` and `ReleaseBackbufferOnce`.

**Render geometry.** FFXIV can allocate its scene buffer at display size while rendering a smaller region. The module corrects the render size and the sub-rectangle handed to the upscaler. Status: tested. Code: `ext/ffxiv/geometry/`, `ext/upscaler/RenderGeometry.h`. Seams: upscaler `ResolveRenderGeometry`, `CorrectRenderSubrect`. Config: `[Hotfix] RenderSizeFromWidthHeight` (default off; opt-in).

**Forced-quality colour scaling.** When the game's colour is display-sized but the forced preset renders smaller, the display-sized colour is Lanczos-scaled to render size before DLSS. Status: tested as part of forced quality. Code: `ext/dx11bridge/ColorScaler_Dx12.cpp`, `ext/dx11bridge/BridgeState.cpp`. Seams: dx11bridge `PrepareUpscalerColor`.

**GPU submission ownership.** Serialises command-list execution and teardown for the objects NR, HDR and the AMD bridge keep alive across submissions. Status: runs inside the tested paths. Code: `ext/gpu/`. Seams: gpu `ResetCommands`, `ExecuteCommands`; restrack `OnCommandListReset`, `WantsCommandListReset`; dx11bridge `ShutdownNgx`, `RedirectShutdown1`, `BeforeReleaseFeature`.

**Lighting scan.** Hooks the game's tone-mapping and exposure shaders (by fingerprint, no game addresses) to read scene lighting and tell NR when the history is invalid (lighting changes, scene transitions). Status: runs inside the tested NR path; not tested on its own. Code: `ext/ffxiv/lighting/`. Seams: game `OnD3D11DeviceHooked`, `OnD3D11Unhook`; dx11bridge `OnUpscaleBoundary`. Config: `[DlssNr] LightingHistory`, `ScanAnchors`, `ScanExposure`, `ScanMeter`, `ScanTrim`, `ScanInverted`.

**Companion / nameplates.** Replays the game's nameplate (HUD label) draws in a separate layer so they can be interpolated and kept sharp with frame generation, and draws source-frame markers after the bridge copy. It reads the game's UI queue through the addresses in `GameBuild.h`. Status: tested. Code: `ext/ffxiv/companion/` (native hooks in `CompanionNative.cpp`, layer in `CompanionLayer.cpp`, replay in `replay/`). Seams: game `OnD3D11DeviceHooked`, `OnD3D11Unhook`; dx11bridge `AfterSharedCopy`; core `HooksBypassed` (lets the layer's own device and swapchain calls pass the fork's hooks). Config: `[Companion] HudReplacement`, `HudInterpolation`. Menu: `ffxiv_companion` (tab Compatibility, shown for FFXIV).

**Composition swapchain hook (E5 composition hook).** Hooks `CreateSwapChainForComposition` so composition swapchains created by overlays do not break the frame-generation presenter, with a pass-through for the Companion layer's own swapchain. Status: untested on its own. Code: `ext/present/CompositionSwapchain.cpp`, `CompositionPassThrough.h`, `CompositionWindow.h`. Seams: present `HookFactoryComposition`, `ResolveCompositionHandle`; core `HooksBypassed`. Inline marker `// ext: E5` in `wrapped_swapchain.cpp/.h`. Config: none.

**Frame markers.** See [Frame generation](#frame-generation): `ext/ffxiv/markers/`, seams fg `MarkersBeforePresent`, `MarkersNextFrame`, `OnUpscaleStart`.

**HUD-less boundary detector.** See [HUD-less](#hud-less-frame-generation): `ext/ffxiv/ui/`.

## Character preview (CharaView)

**What it does.** The game draws the 3D character preview (character, equipment and glamour windows) in a small separate render. CharaView gives that render its own isolated DLSS Super Resolution pass with 2x output (jittered rasterisation, replayed G-buffer draws for signed motion vectors, depth and camera guides, native alpha composed back), and offers an optional FXAA-style spatial anti-aliasing pass with 2x upscaling for the equipment preview. It was reconstructed from an unpublished build by Nickle and written fresh; credit and method: [ATTRIBUTION.md](ATTRIBUTION.md) and [RECONSTRUCTION-NICKLE.md](RECONSTRUCTION-NICKLE.md).

**Status.** Off by default and untested in game. Not implemented, as in the build it was reconstructed from: frame generation for previews (not needed: the preview is part of the UI, which the game's frame generation does not interpolate separately), portrait windows (it is not known which windows reach this renderer), and deferred contexts (those draws stay native).

**Preview Neural Rendering (new work).** `[FFXIV] PreviewNR` (false) runs the DLSS NR model on each open character preview window. This is new work and is not reconstructed from Nickle's build, which never had it (its menu said NR was not active). The NR model only runs on Direct3D 12, so the preview image is handed to the DirectX 11 on 12 bridge's device through shared textures and fences, processed there and drawn from a shared result texture; every failure keeps the native image and switches preview NR off for the session. It needs an NVIDIA card, the bridge running, `nvngx_dlssnr.dll` and `nvngx.dll_dlssnr.dll`; `[DlssNr] Enabled` is not needed, the `[DlssNr]` tuning is shared. It works alone or with `PreviewDLSS`. Alone it runs on the native preview image (bring-up: native size, synthetic guides: a cleared depth and zero motion). With both on it runs on the Super Resolution result (production: the 2x output, copied into shared images, with copies of the preview's own depth and motion at the render size, the Super Resolution feature's depth convention and motion scale), between the Super Resolution evaluate and the alpha pass; the original alpha is recomposed onto the NR image exactly as it is onto the plain Super Resolution output, and any render where NR cannot run (the model is still being built, no command list free, a failure) shows the plain Super Resolution image. With both options on, only the production model is built (at twice the size): a render that Super Resolution did not evaluate (the first renders after a window opens, a rejected finisher) shows the native image, exactly as with `PreviewDLSS` alone, and the first renders after the model is built are plain Super Resolution; preview NR on the native image (bring-up) runs only for a preview Super Resolution does not serve (`PreviewDLSS` off, Super Resolution latched, a failed or too large preview); the cost per window is one NR pass at 2.2 megapixels. `PreviewNRLate` applies to the bring-up only (the alpha recomposition needs the colour of the same render). It takes the equipment preview before the spatial AA; for a draw it has no result for (the first renders after a window opens, renders it skipped, a finisher the checks reject) the spatial AA still takes the draw when `PreviewAA` is on, so the AA is not lost, unless `PreviewDLSS` is on and its pass runs (then the draw stays as it is; while the `PreviewDLSS` pass cannot run, the AA takes it). `PreviewNRLingerSeconds` (5) keeps a window's model after the window closed; `PreviewNRLate` (false) shows the previous frame's result and avoids the GPU wait on the DirectX 11 side at the cost of one frame of lag. Cost per open window (GPU time, video memory) is shown in the menu and the log (`CharaView NR:` lines); it has not been measured in game yet. NR before Super Resolution (a pre-pass on the render-size image) is not implemented; the placement is after Super Resolution when both options are on. In game still to be checked: the D3D12 state tracking of the production inputs under the debug layer, whether the model accepts guides smaller than the colour as the main pass's do, the image quality on the alpha-masked silhouette and on moving characters, and the cost with several windows open.

**Where.** `OptiScaler/ext/ffxiv/chara/` (with `shaders/`), hooks inside `ext/ffxiv/lighting/FfxivLightingCapture.h`, the per-frame callback through `dx11bridge::AfterSharedCopy` in `ext/Seams.cpp`, the game build entry in `ext/ffxiv/GameBuild.h`, ini fragments `ext/ini/FFXIV.ini` and `ext/ini/HDR.ini`.

**Seams.** None in upstream files.

**Config.** `[FFXIV] PreviewDLSS` (false), `PreviewAA` (false; with `PreviewDLSS` on, the DLSS pass takes the equipment preview and the AA is bypassed, but while that pass cannot run, for example because the game's D3D11 DLSS is not initialised or it failed, the AA is used instead; with `PreviewNR` on, the AA is used for every draw NR has no result for while `PreviewDLSS` is off or its pass cannot run), `PreviewUpscale` (true, only meaningful with the AA), `PreviewGuideCapture` (false; writes capped diagnostic dumps to `OptiScaler\PreviewDLSSDiagnostics\`), `PreviewNR` (false), `PreviewNRLingerSeconds` (5), `PreviewNRLate` (false) (see Preview Neural Rendering), `PreviewDebugTint` (false; debug aid, mixes magenta into the AA or DLSS result and cyan into an image preview NR produced, bring-up or production, so it can be seen which image is on screen), `PreviewDebugFlood` (false; debug aid, draws the preview as solid magenta wherever the game samples it, with or without AA or DLSS), `PreviewProbe` (false; debug probe: logs copies of the preview and draws that sample it or a copy, can save preview images as PNG to `OptiScaler\PreviewDebug\` and reads live commands from `OptiScaler\chara-probe.cmd`), `[HDR] PreviewNRTrace` (false; trace log lines only).

**Menu.** Card "Character preview" in the Upscaling tab, with the counters and status lines.

**Probe output.** While PreviewProbe is on, the button "Save preview images" (or the command `dump`) writes `native.png`, `aa.png`, `dlss.png`, `nr.png` (whichever exist; `nr.png` is the NR result converted to 8 bits), each with `alpha-<name>.png` (the alpha channel as a grayscale picture), and `state.txt` (one `key: value` per line) into a new `OptiScaler\PreviewDebug\<yyyyMMdd-HHmmss>\` folder; at most 40 files and 300 MiB per session. About twice a second the probe reads `OptiScaler\chara-probe.cmd` (one command per line: `tint|flood|aa|dlss|upscale|copies on|off`, `dump`, `state`, `reset`, `help`; at most 4 KiB), runs it, deletes it and appends the results to `OptiScaler\chara-probe.out` (capped at 256 KiB). Commands change the settings in memory only; the ini file is not touched.

**Safety.** It fails open: any doubt leaves the native image. It reads the game's `.text` once, read-only, for an 18-byte code pattern that must match exactly once at the documented addresses; otherwise the feature switches itself off for the session with one log line. The DLSS pass uses the driver's real D3D11 NGX and latches off for the session if feature creation fails. Switching `PreviewDLSS` off at run time releases the preview GPU objects (images and NGX features) as soon as their last GPU work has finished.

## Items recovered from Nickle's build

Three behaviours were reconstructed by static analysis of an unpublished build by Nickle and re-implemented; none of Nickle's source is used. Credit and method: [ATTRIBUTION.md](ATTRIBUTION.md); confidence of each reconstruction: [RECONSTRUCTION-NICKLE.md](RECONSTRUCTION-NICKLE.md). Status of all three: untested in game.

| ID | What | Code | Config | Menu |
|---|---|---|---|---|
| F2 | NR compute-shader edit for the "Linear light" proxy | `ext/nr/shaders/dlssnr.hlsl` | none | none |
| F3 | NR HDR mapping "Linear light + composed" | `ext/nr/` (config, menu, presets), `ext/ini/DlssNr.ini` | `[DlssNr] ReversibleMode = 5` | `dlss_nr` |
| F4 | HDR PNG "Include OptiScaler menu" | `ext/hdr/screenshot/` (`MenuCapture.h`), `ext/ini/HDR.ini` | `[HDR] ScreenshotMenu` | `ffxiv_hdr` |

Seams: none added by F2 or F3; F4 uses the existing `dx11bridge::PrepareScreenshot` (it returns early while a menu capture is wanted) and `hdr::OverlayScope`. The fourth item from that build, a separate DLSS for the character preview window, is described in [Character preview](#character-preview-charaview).

## Footprint

Upstream files carry only the seam calls and the small marked edits. `tools/footprint.ps1` counts them (added plus removed lines against the upstream base `fd6c2870`, excluding fork-only paths). It needs only git: the `upstream` remote must be fetched so `upstream/master` exists, and a history that reaches the base commit; no build, no submodules.

Measured on `main` at `c3277532` (the release-candidate commit):

| Measure | Value |
|---|---|
| Upstream files touched | 71 |
| Lines added / removed | +1258 / -292 (1550) |
| Lines carried by `[UP]` commits (separate, self-contained fixes) | 792 |
| Persistent seam surface (total minus `[UP]`) | 758 |
| Target in [SEAMS.md](SEAMS.md) section 1.3 | 600 (over by 158) |

The overage is known: the seam surface grew with each feature port and with the later HUD-less and composition work, and the earlier figures in [SEAMS-catalog.md](SEAMS-catalog.md) section (d) are historical snapshots. Re-measure with `pwsh -File tools/footprint.ps1` after every sync.
