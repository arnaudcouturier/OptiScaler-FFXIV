# Feature reference

What this repository bundles on top of OptiScaler, where each piece comes from and lives, how it attaches to upstream files and how to configure it. The short status table is in [STATUS.md](STATUS.md) (the README has a one-screen summary); this file is the long version and uses the same status words. Long histories, review rounds and measurements are in [docs/notes/](notes/); the entries link to them. Credit and licences: [ATTRIBUTION.md](ATTRIBUTION.md).

**Status words.** *tested* means the author played the game with it on an RTX 4090. *untested* means it compiles and follows the same code paths as the tested features, but nobody has verified it on real hardware (or in game) yet. Where a feature runs inside a tested path but was never exercised alone, the entry says so. Nothing here is a promise that a feature works on your system.

**Origin words.** *SirenBrink's fork* is the community repository `SirenBrink/OptiScaler_DLSSNR_Multipass_MFG_FFXIV` (GPL-3.0). *The import base* is its commit `36c53a15`. *v6.0* is its commit `e17a7362` (2026-10-20). *Ported* means the code came from SirenBrink's fork at the import base and was moved under `ext/` behind seams. *Follows the approach* means the design is the same or close and the code was written for this fork.

**Fields of an entry.** **Origin**: where the code or the design comes from. **Config**: `[Section] Key` as written in `OptiScaler.ini`; defaults and explanations are in the matching fragment under `OptiScaler/ext/ini/`, which `tools/package.ps1` merges into the shipped `OptiScaler.ini`. **Menu**: the card id in the in-game menu, with its tab. **Code and seams**: directories and key files under `OptiScaler/ext/` (every file the fork owns is under it) and the areas of the one-line seam calls in upstream files. File lists and seam function names per feature: [notes/feature-code-and-seams.md](notes/feature-code-and-seams.md); the full list is [SEAMS-catalog.md](SEAMS-catalog.md), the rules are in [SEAMS.md](SEAMS.md).

## Summary

| Feature | Origin | Status |
|---|---|---|
| [DLSS Neural Rendering (NR) with pre-SR multipass](#dlss-neural-rendering-nr-with-pre-sr-multipass) | ported from SirenBrink's fork | tested |
| [NR model version picker](#nr-model-version-picker) (`OptiScaler\NR.<version>`, switch live) | follows SirenBrink's approach, fork code | new; unit-tested; tested (RTX 4090) on 2026-10-23: live switch and reload from an `NR.<version>` folder work; `NR.310.8.0.0` cannot create its feature on this GPU (NR skips itself, switching back recovers); a switch between two different working models was not seen |
| [NR pause on focus loss](#nr-pause-on-focus-loss) | follows SirenBrink's approach, fork code | experimental, off by default; new; unit-tested; the preview pause has not run in game |
| [NR native path](#nr-native-path-and-vit-capture) | ported from SirenBrink's fork | tested (part of NR) |
| [NR ViT capture](#nr-native-path-and-vit-capture) | fork | diagnostic only |
| [NR ViT reuse](#nr-native-path-and-vit-capture) (`VitEvery`) | ported from SirenBrink's fork | untested |
| [AMD HIP pre-SR](#other-nr-paths), [Vulkan NR path](#other-nr-paths) | ported from SirenBrink's fork | untested |
| [OptiHDR (HDR10 output)](#optihdr-hdr10-output) | ported from SirenBrink's fork | tested |
| [RenoDX scene HDR](#renodx-scene-hdr) | ported from SirenBrink's fork (RenoDX shaders) | tested |
| [HDR PNG capture](#hdr-png-capture) | ported from SirenBrink's fork | untested |
| [DLSS-G multi frame generation unlock, RTX 40](#dlss-g-multi-frame-generation-unlock-rtx-40) | ported from SirenBrink's fork (y4my4my4m's unlock) | tested at 2x to 4x |
| [DLSS-G MFG loader for RTX 30](#dlss-g-mfg-loader-for-rtx-30) | ported from SirenBrink's fork | untested |
| [DLSS-G render preset (A, B, C)](#dlss-g-render-preset-a-b-c-switchable-while-the-game-runs), switchable live | follows SirenBrink's approach, fork code | new; code checks pass on the real 310.9.16 runtime; tested on the 310.9.16 runtime (RTX 4090): boot on Auto and on Preset C; live Auto/B/C switches without a refusal (B on 2026-10-22, C on the released build); Preset A seen only on an earlier build |
| [Dynamic MFG readout and RTX 50 behaviour](#dynamic-mfg-on-rtx-50) | upstream OptiScaler keys; readout origin not determined | untested |
| [Automatic multiplier (dynamic MFG) on RTX 40](#dynamic-mfg-on-rtx-40-automatic-multiplier) | fork (selector rules follow SirenBrink's XeFG selector) | opt-in, off by default; tested (RTX 4090), 2x to 4x, stall rule seen working; the self-test has not passed in game |
| [Frame pacing, Reflex and Streamline hardening](#pacing-markers-and-hardening) | ported from SirenBrink's fork | runs inside the tested DLSS-G path |
| [XeFG / XeMFG](#xefg-and-xemfg) | ported from SirenBrink's fork (Citrus333's base) | untested |
| [HUD-less frame generation](#hud-less-frame-generation) | adapted from byunjoe's code in SirenBrink's fork; native capture and engine layers are fork code | opt-in; SDR with DLSS-G 2x-4x tested, HDR untested |
| [ReShade built-in add-on](#reshade-built-in-add-on) | fork | tested as part of HUD-less, ReShade 6.8.0 only |
| [Automatic ReShade scene depth](#automatic-reshade-scene-depth) (`[FFXIV] AutomaticReShadeDepth`) | fork | opt-in, off by default; new; unit-tested on fakes and a WARP device, the live ReShade side has not run in game |
| [Window mode](#window-mode) (log probe, border fix, deferred mode switch) | fork | opt-in, off by default; tested (RTX 4090) on 2026-10-19: border fix and two held-back mode switches with DLSS-G on; the open points listed in the entry were not exercised |
| [FFXIV native quality, render geometry, colour scaling](#ffxiv-module) | ported from SirenBrink's fork | tested |
| [Native quality hook install retry](#native-quality-and-forced-quality) (up to 3 attempts) | follows SirenBrink's approach, fork code | new; tested against the real Detours library; the install succeeded on its first attempt in every game log since 2026-10-22; the retry path itself (a failed first attempt) has never been seen in game |
| [Quality change without the 500 ms pause](#quality-change-without-the-stall) (`[NgxRelease] SkipReleaseSleep`) | follows SirenBrink's approach, fork code | on by default; new; ran in game (RTX 4090) since 2026-10-20: the sleep was skipped and 0.06 to 30 ms were waited instead of 500 ms |
| [FFXIV lighting scan](#lighting-scan) | ported from SirenBrink's fork | runs inside the tested NR path |
| [FFXIV Companion nameplates](#companion-nameplates) | ported from SirenBrink's fork | tested |
| [Composition swapchain hook](#composition-swapchain-hook) | ported from SirenBrink's fork (janblade's hook) | untested on its own |
| [Game jitter probe](#game-jitter-probe) (`JitterProbe`) | fork | diagnostic, off by default; tested (ratio 0.600) |
| [Jitter correction](#jitter-correction) (`JitterCorrection`, `JitterScaleOverride`) | fork | experimental, off by default; the correction ran in game and scaled the told jitter (2026-10-20 and 2026-10-23); its earlier version showed no visible gain at the Quality mode (2026-10-19); the look of the current version is not recorded |
| [Jitter source](#jitter-source) (`JitterSource`) | follows SirenBrink's approach, fork code | experimental, opt-in, off by default; full lattice seen in game; "less jumpy" impression confounded by `ui_free` |
| [Character preview DLSS and spatial AA (CharaView)](#character-preview-charaview) | adapted from SirenBrink's v6.0 (same feature) | opt-in, off by default; tested (RTX 4090) on the 2026-10-18/19 builds with one preview window: spatial AA, DLSS 2x, NR, NR before DLSS; not re-checked on this release; several preview windows and the D3D12 debug layer not checked |
| [Preview Neural Rendering](#preview-neural-rendering), including NR before DLSS | fork (different mechanism from v6.0) | opt-in, off by default; tested (RTX 4090) on the 2026-10-18/19 builds with one preview window: spatial AA, DLSS 2x, NR, NR before DLSS; not re-checked on this release; several preview windows and the D3D12 debug layer not checked |
| [Frame pacing trace and hidden-present `DO_NOT_WAIT`](#frame-pacing-trace) | follows SirenBrink's approach, fork code | new; unit-tested, untested in game |
| [Automatic mipmap bias, bind-time mode](#bind-time-mode-ffxiv) | follows SirenBrink's approach, fork code | on by default for FFXIV; ran in game (RTX 4090) on 2026-10-20, 21 and 23; no fps loss measured (comparison across sessions, not a same-session A/B); look not compared |
| [Automatic mipmap bias, create-time mode](#create-time-mode) | fork | opt-in, off by default; ran in game on 2026-10-19; the bind-time mode replaced it as the default |
| [DLSS presets L and M](#dlss-presets-l-and-m) | upstream OptiScaler; forwarding origin not determined | check recipe only |
| [Menu style (Ember)](#menu-style-ember) (`[Menu] Style`) | fork | opt-in, off by default (classic); tested (RTX 4090) |
| [Linear-light NR mode, HDR PNG with menu (F2, F3, F4)](#linear-light-nr-mode-and-hdr-png-with-menu-f2-f3-f4) | adapted from SirenBrink's v6.0 (same features) | untested |
| [Menu layout](#menu-layout), card registry and manifest | fork (tabs, cards and Custom Tab are upstream) | offline tests only |

## DLSS Neural Rendering (NR)

### DLSS Neural Rendering (NR) with pre-SR multipass

**Origin.** Ported from SirenBrink's fork at the import base (originally Dagherbou's NR work, with y4my4my4m's multipass, janblade's PassFeedback and later work by wilsjo2), moved under `ext/nr` behind seams. Of the 47 files in `ext/nr` that have a counterpart at the import base, 22 are identical apart from line endings and 25 add about 8% more lines; about 20% of the lines in `ext/nr` are in files with no counterpart. The integration layer (NrSeams, NrHooks, the `[DlssNr]` config binder, build-time shaders) and `VitCapture` are fork code.

**What it does.** Runs NVIDIA's DLSS Neural Rendering model on the frame, before ordinary super resolution (pre-SR) or after it, in one to three passes with per-pass profiles. A colour pipeline maps the game's image into what the model expects and back (white point, exposure, reversible tone mapping). It includes the exposure scan (reads the game's exposure from resource creation and unordered-access views) and named user presets. It needs NVIDIA's `nvngx_dlssnr.dll`; see [INSTALL.md](INSTALL.md).

**Status.** tested (RTX 4090), pre-SR multipass included. Exposure scan, white point, presets, compare views and the debug view are used by the tested sessions but not tested separately. `DeferredDLSS`, `ResidualFG`, spatial compression and `WorkingScale` other than 1.0 are experimental or opt-in and untested.

**Config.** `[DlssNr]` (82 keys): `Enabled`, `DisableOnFocusLoss`, `ToggleKey`, `RunBeforeSR`, `Passes`, `PassFeedback`, `Pass2*`, `Pass3*`, `Preset`, `Style`, `Intensity`, `WorkingScale`, `ReversibleMode`, `WhitePointSource`, `ScanAnchors`, `LightingHistory`, `SkinProtection`, `PreSRTrailGuard`, `SplitFrameWork`, `Compare*`, `DebugView`, `Runtime`. `[Hotfix] RenderSizeFromWidthHeight` and `[QualityOverrides] ForcePerfQuality` belong to the [FFXIV module](#ffxiv-module).

**Menu.** `dlss_nr` (tab Upscaling), plus an overlay for the compare tags and the exposure-scan lamp, a GPU-time row, and the NR on/off shortcut in the Keybinds list.

**Code and seams.** `ext/nr/` (`pass/DlssNr_Dx12.cpp` is the D3D12 pass; `presr/`, `exposure/`, `presets/`; shaders in `shaders/`, compiled at build time). The forwarder `ext/nr/forwarder/dlssnr_forwarder.cpp` is the second project in `OptiScaler.sln`; its odd output name `nvngx.dll_dlssnr.dll` is deliberate, because NVIDIA's model only answers a caller whose module path contains `nvngx.dll`. Seams: nr, restrack, dx11bridge, menu, gpu.

### NR model version picker

**Origin.** Follows the approach of SirenBrink's v6.0 NR model picker (same search roots in the same order, a runtime revision and "runtime generation", `Current()` and `Startup()`). The catalog, scan, card, live switch and rebuild code are written for this fork. `Runtime=auto` means the newest model here; in v6.0 it means the legacy file only.

**What it does.** Keeps several NR model files side by side and switches the running model without a restart, in the scene and in the character previews. Put each model file in its own folder `OptiScaler\NR.<version>` (digits separated by dots, for example `OptiScaler\NR.310.9\nvngx_dlssnr.dll`) next to the game, beside or instead of the old single `nvngx_dlssnr.dll`. The `NR model version` combo in the Neural Rendering card lists the old file ("Legacy location") and every valid folder; `Automatic` (the default) is the newest version, and on a tie the old file wins. A choice that is not installed never falls back to another model: the card says Neural Rendering is unavailable and the pass shows the plain upscaler image. A switch happens 250 ms after the last click and loads the new file by its full path; model files are never unloaded. A restart is needed only for the NGX core's first search path, native Vulkan NR, the hybrid FP4 precision (`Precision=4`) and the AMD bridge.

**Status.** new; tested (RTX 4090) on 2026-10-23: live switch and reload from an `NR.<version>` folder work; `NR.310.8.0.0` cannot create its feature on this GPU (NR skips itself, switching back recovers); a switch between two different working models was not seen. The catalog, scan, selection, switch, text and glue are unit-tested, including that two same-named DLLs in two folders load as two modules when loaded by full path. Known limits: [notes/nr-picker-and-focus-pause.md](notes/nr-picker-and-focus-pause.md).

**Config.** `[DlssNr] Runtime`: empty or `auto` = newest, `legacy`, or a folder name such as `NR.310.8`. Not part of the saved NR presets. **Menu.** `dlss_nr` card, combo `NR model version` and button `Rescan`.

**Code and seams.** `ext/nr/runtime/`, `ext/nr/pass/DlssNr_Dx12.cpp`, `ext/ffxiv/chara/CharaNr.cpp`. Seam: nr `AppendNgxSearchPaths` (body changed, no new seam). Log: `DLSS-NR runtime catalog: N candidate(s)`, `DLSS-NR runtime revision N: <id> (<path>), file version ...`.

### NR pause on focus loss

**Origin.** Follows the approach of SirenBrink's v6.0 `PauseOnFocusLoss`: both pause when the foreground window does not belong to the game's process. v6.0 pauses at once; this fork waits 1 s and also covers the character preview.

**What it does.** Pauses NR while the game window is not focused (after 1 s without focus, at once when minimised). It covers the D3D12 and D3D11-bridge scene NR and the character preview NR; native Vulkan is not paused. The preview shows its plain image while paused and resumes with a clean history. Log: `CharaView NR: paused, window not focused` and `CharaView NR: resumed`.

**Status.** experimental, off by default; new; unit-tested. Nothing of the preview pause has run in game (the in-game proof needs a visible but unfocused window, not a minimised one).

**Config.** `[DlssNr] DisableOnFocusLoss` (off by default). `PauseOnFocusLoss` (the v6.0 name) is accepted as another name: it is moved into `DisableOnFocusLoss` when the ini is read and saved back as `auto`; when both are set, `DisableOnFocusLoss` wins.

**Menu.** In the `dlss_nr` card. **Code and seams.** `ext/nr/NrFocus*.h`, `ext/nr/NrConfig.cpp`, `ext/ffxiv/chara/CharaNr.cpp`. No new upstream seam.

### NR native path and ViT capture

**Origin.** Ported from SirenBrink's fork: the native path (`WrapNvapi`, module identity by size and SHA-256), `VitEvery` (following janblade's work) and the hybrid precision. The verified-module profile table uses the technique of wilsjo2's commit with a small addition here. `VitCapture` is fork-original (a log-only diagnostic).

**What it does.** The fork drives the NR model's own DLL directly. It records which CUDA entry points the game queried (`WrapNvapi`), identifies the model's CUDA module by size and SHA-256, and can skip repeated vision-transformer launches (`VitEvery`) for a module that has been verified. Unknown modules are never touched: reuse stays off and the log says why.

**Status.** The native path is tested as part of NR. `VitCapture` is a diagnostic that only observes. `VitEvery=2` reuse is untested for speed: it only activates for a module listed as verified in `ext/nr/native/DlssNrModuleProfiles.h`, and is a no-op otherwise. `VitAllowUnverified=true` skips the fingerprint check and can hang the GPU; leave it off.

**Config.** `[DlssNr] VitCapture`, `VitFirstPassOnly`, `VitEvery`, `VitAllowUnverified`, `Precision`. **Menu.** Status text in the `dlss_nr` card.

**Code and seams.** `ext/nr/native/`, `ext/nr/hybrid/`. The capture summariser is `tools/nr-vit-capture-summary.ps1`; profiling a new model file: [nr-vit-reuse-profile-runbook.md](nr-vit-reuse-profile-runbook.md). Seam: nr.

### Other NR paths

**AMD HIP pre-SR.** Origin: ported from SirenBrink's fork, which adapted Ainquisition's AMD HIP work; MatheusGViana is named in the AMD file headers. Runs the pre-SR pass on AMD devices through a small bridge. Status: untested (community). Config: none of its own; it follows `[DlssNr] RunBeforeSR`. Code: `ext/nr/amd/`; seams nr (`PreSrScope` chooses the AMD edit) and restrack.

**Vulkan NR path.** Origin: ported from SirenBrink's fork. NR for native Vulkan games, kept for reuse with other games; FFXIV never reaches it. Status: untested. Config: follows `[DlssNr]`. Code: `ext/nr/vk/`, `ext/nr/NrResampler_Vk.cpp`; seams nr.

## HDR

All HDR code is under `OptiScaler/ext/hdr/`. The parts share one set of seams.

### OptiHDR (HDR10 output)

**Origin.** Ported from SirenBrink's fork (first committed there 2026-09-26). 14 of its files are byte-identical to the import base and 5 more differ only by namespace.

**What it does.** Presents the D3D11 game through OptiScaler's DX12 presenter as HDR10 (R10G10B10A2, PQ). In the default mode the game's SDR image is expanded to HDR. Windows HDR must be on for the game display before launch.

**Status.** tested (RTX 4090).

**Config.** `[HDR] FfxivHDR` (restart after changing), `PeakNits`, `PaperWhiteNits`, `HighlightExpansion`, `Contrast`, `Saturation`, `Vibrance`. **Menu.** `ffxiv_hdr` (tab FFXIV, second card; titled "FFXIV HDR10 output").

**Code and seams.** `ext/hdr/` (`Hdr10.cpp`, `PresentHdr.cpp`, `BridgeHdr.cpp`, `FgHdr.cpp`, `menu/`, `shaders/Hdr10.hlsl`). Seams: present, dx11bridge, fg, menu, gpu, hdr.

### RenoDX scene HDR

**Origin.** Ported from SirenBrink's fork. The shaders come from clshortfuse's RenoDX (the FFXIV mod is by Ersh and Pumbo) as adapted there; the five shaders, `reinhard`, `shared.h`, `NOTICE.md` and the licence are identical to SirenBrink's files.

**What it does.** `[HDR] Mode = 1` replays the game's tone-mapping stages in FP16 next to the SDR chain, so HDR output keeps the highlights the SDR path clips. The SDR chain, ReShade and the HUD are untouched. Two experimental switches feed the linear scene into NR (`NRSceneInput`) or DLSS (`DLSSSceneInput`). The scene stages are recognised by shader fingerprint, so a game patch that changes those shaders disables it; see [FFXIV-PATCH-RUNBOOK.md](FFXIV-PATCH-RUNBOOK.md).

**Status.** tested (RTX 4090) for `Mode = 1`; `ReShadeHighlights`, `NRSceneInput`, `DLSSSceneInput` are experimental and untested.

**Config.** `[HDR] Mode`, `ReShadeHighlights`, `NRSceneInput`, `DLSSSceneInput`. **Menu.** The `ffxiv_hdr` card.

**Code and seams.** `ext/hdr/scene/`, `ext/hdr/DlssHdr.cpp`, `ext/hdr/shaders/renodx/` (licence and notice shipped as `Licenses/RenoDX_*`). Seams: hdr, dx11bridge, fg.

### HDR PNG capture

**Origin.** Ported from SirenBrink's fork (first committed there 2026-10-01; ICC-only variant 2026-10-09). The `ScreenshotMenu` option is covered under [F2, F3, F4](#linear-light-nr-mode-and-hdr-png-with-menu-f2-f3-f4).

**What it does.** Saves one PNG on a key press, as SDR for sharing or as HDR, with BT.2100 PQ metadata (an ICC-only variant exists for viewers that mishandle the cICP chunk).

**Status.** untested (the author does not use it day to day); the PNG writer has unit tests.

**Config.** `[HDR] ScreenshotKey`, `ScreenshotFormat`, `ScreenshotIccOnly`, `ScreenshotMenu`. **Menu.** Key row in the Keybinds list; options in the `ffxiv_hdr` card.

**Code and seams.** `ext/hdr/screenshot/`. Seam: dx11bridge `PrepareScreenshot`; the menu variant is taken inside `hdr::OverlayScope`.

## Frame generation

### DLSS-G multi frame generation unlock, RTX 40

**Origin.** Ported from SirenBrink's fork. The unlock is y4my4my4m's (`MfgUnlock.cpp` still says "Adapted from y4my4my4m"); wilsjo2 hardened it and SirenBrink reworked it. `AdaTemporal` adapts mavismmg / ImDreamt's MIT code (header kept) and the unlocker technique is dashdogy's.

**What it does.** Raises the frame-generation ceiling that the driver reports for GeForce RTX 40 cards, in memory only (the `nvngx_dlssg.dll` on disk is never modified), and applies a targeted temporal correction. Only supported runtime builds are patched; an unknown build is left alone.

**Status.** tested (RTX 4090) at 2x to 4x. 5x and 6x are experimental; the author saw one GPU hang at 6x.

**Config.** `[DLSSG] AdaMfgUnlock` (save and restart after changing). Do not combine with an external unlocker. **Menu.** `fg_unlocks` (tab Frame Gen, titled "Frame generation unlocks", after the runtime cards; the RTX 20/30 runtime sits in its closed fold "RTX 20/30 frame-generation runtime").

**Code and seams.** `ext/fg/dlssg/` (`MfgUnlock.cpp`, `AdaTemporal.h`), `ext/fg/streamline/`. Seams: streamline, fg.

### DLSS-G MFG loader for RTX 30

**Origin.** Ported from SirenBrink's fork, which adapted ShyVortex's loader.

**What it does.** Loads a separate RTX 20/30 frame-generation runtime (a DLSS-G build) from `OptiScaler/dlssg_sm86` so Ampere cards can generate more than one frame. You must supply the runtime DLL yourself.

**Status.** untested.

**Config.** `[DLSSG] AmpereMfgUnlock`, `AmpereMfgMaxFrames`, `AmpereMfgKernelImage`, `AmpereMfgRouter`, `AmpereMfgHardwareBilinear`, `AmpereMfgOptimized`, `AmpereMfgPreset`, `AmpereMfgSpoofArchToGame`, `AmpereMfgLogLevel` (the last seven are written to the runtime's own `dlssg_sm86.ini`). **Menu.** `fg_unlocks`; the less common keys are in its closed "Advanced (RTX 20/30 runtime)" fold.

**Code and seams.** `ext/fg/dlssg/AmpereMfgLoader.cpp`. Seam: streamline `OnLoadStreamline`.

### DLSS-G render preset (A, B, C), switchable while the game runs

**Origin.** Follows the approach of SirenBrink's v6.0 `PresetCache`, committed there first (2026-10-19; live switching 2026-10-20): the setting id, the three-hook live design, the byte anchors and the create signature are shared. The guards, the instruction decoder (`PresetX64.h`), the single-thread install, the switch state machine and the readout are written for this fork. The rule that UI recomposition is requested only with preset Auto or B is also in v6.0, committed there first.

**What it does.** Chooses the render preset of NVIDIA's frame generation runtime (`nvngx_dlssg.dll`): Auto, Preset A, B or C. The request is answered inside the process (setting id 0x10E41DF1), so no NVIDIA profile is written. The DLSS override id (0x10E41E03) is never answered: answering it "on" made NGX load NVIDIA's own copy of the runtime (log `Built with APP_NAME = nvapp_override`), with no multi-frame unlock and about a third fewer frames per second (as the author measured on one RTX 4090 setup). A change in the menu while the game runs switches frame generation off, releases the DLSS-G resources on a later frame, lets the runtime build its next endpoint under the new request and resumes. Only 310.8 and 310.9 runtimes are accepted; a failed check keeps the old request and the menu says "restart needed".

Preset A and C turn the runtime's UI recomposition off, B turns it on, Auto leaves it to the application (C also turns FG Lite on). The fork requests recomposition only with `[HudLess] Mode = recomposition` and preset Auto or B (`ext/fg/dlssg/UiRecompositionRule.h`); with A or C the log says once `Preset C turns NVIDIA's UI recomposition off on this runtime, so the HUD is interpolated; use Auto or B for recomposition, or ui_free with C`. The menu shows what the runtime reported and `Chosen: X, in use: Y` when they differ; that readout stays empty with `[Log] LogLevel` at warn or higher or with `LogAsync`. The selector is disabled for the RTX 20/30 runtime, a replacement runtime and any output other than DLSS-G.

**Status.** new. The code checks pass on the real 310.9.16, 310.9.1.0 and 310.8.0 runtimes (manual recipe `tests/ext/fg/fg_preset_guards_dll.cpp`). Tested on the 310.9.16 runtime (RTX 4090): boot on Auto and on Preset C; live Auto/B/C switches without a refusal (B on 2026-10-22, C on the released build); Preset A seen only on an earlier build. A start with `RenderPreset = 3` loads the local runtime (`max generated frames 5`, log `Applying settings for render preset N.`) and applies Preset C (checked 2026-10-22, about 74 fps on that setup); the live change Auto to Preset C from the menu was also seen working (2026-10-21, about 74 fps as measured on one RTX 4090 setup). Known limits and the full explanation: [notes/dlssg-render-preset.md](notes/dlssg-render-preset.md).

**Config.** `[DLSSG] RenderPreset` (0 Auto, 1 A, 2 B, 3 C; the ini value applies at the next start either way). **Menu.** Section "DLSS-G render preset" of the `fg_unlocks` card.

**Code and seams.** `ext/fg/dlssg/Preset*`, `ext/fg/MenuFgPreset.cpp`. Seam: fg `PresetSwitchTick`; the driver-settings hook calls `ext::fg::preset::OnDrsGetSetting` (upstream: `nvapi/NvApiHooks.cpp` +2 lines, `framegen/dlssg/DLSSG_Dx12.cpp` +2 lines). On a new runtime, run the recipe `tests/ext/fg/fg_preset_guards_dll.cpp`.

### Dynamic MFG on RTX 50

**Origin.** Upstream OptiScaler provides `ForceDMFG` and `OverrideForceDMFG` (FakeMichau). The origin of the readout code was not determined.

**What it does.** Reads back the multiplier that DLSS-G actually applied and shows it, and sends the frame limit you set to Reflex in dynamic mode. A `FramerateLimit` read from the ini that is not a positive number (`nan`, `inf`, `0` or negative) means no limit and is saved back as `auto`; a limit set at run time with the "Set as FPS Limit" button of the VRR calculator is not checked.

**Status.** untested. **Config.** Upstream keys `[DLSSG] ForceDMFG`, `OverrideForceDMFG`. **Menu.** None of its own.

**Code and seams.** `ext/fg/MenuFgStatus.cpp`, `ext/fg/dlssg/DlssgSeams.cpp`. Seams: menu `ObservedMultiplier`; fg.

### Dynamic MFG on RTX 40 (automatic multiplier)

**Origin.** The selector rules (500 ms average, thresholds 0.93 and 1.10, dwell of 1 s up and 2 s down, hold of 3 s, frames over 250 ms ignored, 2x to 4x) are the rules of SirenBrink's XeFG selector; `DynamicMfg.h` says "same rules". The controller, kill-switch, probe and regret steps, stall rule, self-test and keys are fork-original: SirenBrink's fork has no automatic multiplier on RTX 40.

**What it does.** Chooses 2x, 3x or 4x from the real frame time and changes the count while DLSS-G keeps running, instead of the off/on cycle of the Frame Gen slider. It takes the lowest multiplier that still presents the target frame rate, waits 3 s between changes, tries one step lower every 12 s or longer while the output sits at the target, and reverts a step up that gains less than 4 percent. It watches one second after every change: a long frame slows the mode, repeated ones stop it, and a change that is not applied or that upsets Streamline goes back to the last good count and stops the mode. While the real frame rate is below about 33 fps (a stall) no change starts, and a change that cannot be judged within 5 seconds ends as unknown (log `AutoMFG: judging the change at Nx skipped, the game was stalled`).

It never goes above 4x, is off by default, and is inert on RTX 50 (use Force Dynamic MFG there), with Frame Gen on Default, with Force Dynamic MFG and with the other outputs. A guard keeps NVIDIA's own dynamic mode from choosing 5x or 6x on an unlocked RTX 40; with Force Dynamic MFG on an unlocked RTX 40 and a frame limit that OptiScaler enforces itself, keep the slider on 4X. All rules and the review-round history: [notes/automfg-history.md](notes/automfg-history.md).

**Status.** opt-in, off by default; tested (RTX 4090), 2x to 4x, stall rule seen working; the self-test has not passed in game. The stall rule came from the first two runs in game, in which a loading screen or a DLSS preset change stalled the game at about 18 real frames per second while a change was being checked and the mode stopped for the whole session.

**Check.** Set `[DLSSG] AutoMFG=true`, `AutoMFGSelfTest=true` and the slider on 3X or 4X, start the game, stand in the game world for about 70 seconds (the test first waits for 20 seconds without a stall), then read `OptiScaler.log` for `AutoMFG self-test: PASS` or `FAIL` (needs the INFO log level and `AdaMfgUnlock`). Stall check: with `AutoMFG=true`, change a DLSS preset in the menu or load a zone; there should be no `automatic mode stopped` line, and a change made right before the stall ends with `settled` or with the `skipped` line.

**Config.** `[DLSSG] AutoMFG`, `AutoMFGMax` (2 to 4), `AutoMFGTarget` (0 = the lower of the frame rate limit and the display refresh), `AutoMFGSelfTest`. **Menu.** `fg_auto_mfg` (tab Frame Gen, after the `fg_unlocks` card; titled "Automatic multi frame generation"; its slider is "Target FPS (automatic multiplier)").

**Code and seams.** `ext/fg/DynamicMfg*.h` (`DynamicMfg.h` is the selector), `DynamicMfgSeams.cpp`, `MenuAutoMfg.cpp`. Seams: fg (no new seam).

### Pacing, markers and hardening

**Origin.** Ported from SirenBrink's fork: marker pacing, `FrameInterval`, `GuideMatcher`, ordered swapchain submission and GPU lifetime tracking (after stegalloway's work). The `[UP]` Reflex and Streamline fixes reproduce lines found in SirenBrink's tree (19 of 25 measurable commits).

**What it does.** The frame-rate cap counts generated frames when NVIDIA frame generation is active (`FrameInterval`). For FFXIV the fork sends its own simulation and render latency markers so Reflex pacing sees the game's real frames (`ext/ffxiv/markers/`; it needs a matched FFXIV build). It also carries ordered swapchain-command submission, guide matching for the history reset, and several Reflex and Streamline fixes as small `[UP]` commits (listed in [SEAMS-catalog.md](SEAMS-catalog.md), section (a)).

**Status.** Runs inside the tested DLSS-G path, not tested separately.

**Config.** `[LowLatency] Input`, `Output` (menu selection saved). **Menu.** None of its own.

**Code and seams.** `ext/fg/` (`FrameInterval.cpp`, `GuideMatcher.cpp`, `MarkerSeams.cpp`), `ext/ffxiv/markers/`, `ext/gpu/`. Seams: present, fg, gpu.

## XeFG and XeMFG

**Origin.** Ported from SirenBrink's fork. The unlock and pacing build on Citrus333's work; dynamic MFG, camera and keys are SirenBrink's. The seams, state and context-rebuild glue are fork code.

**What it does.** Unlocks Intel's multi frame generation in `libxess_fg.dll` for any adapter by patching the loaded image (never the file), selects the interpolation factor from real frame timing (dynamic MFG), spreads generated frames in time (extra pacing), builds a validated camera from the game's data, and rebuilds the context when the upscaler is released so the pacing history starts clean. Unknown provider builds are left untouched.

**Status.** untested (community).

**Config.** `[XeFG] DynamicMFG`, `DynamicTargetFPS`, `UnlockMFG`, `MaxInterpolatedFrames` (1 to 7), `ExtraPacing`, `ResetOnUpscalerChange`. Upstream's own `[XeFG] InterpolationCount` is validated against the same limit. `[XeFG] ForceBorderless` is upstream's. **Menu.** Extra rows inside upstream's XeFG card.

**Code and seams.** `ext/fg/xefg/`. Seams: menu `XeFgExtras`; fg; dx11bridge `BeforeReleaseFeature`; config `SaveXeFG`.

## HUD-less frame generation

**Origin.** The mask and paste GPU module, its two modes, shaders and defaults come from byunjoe's HUD-less code in SirenBrink's fork (nine commits, 2026-10-02 and 03); the engine glue follows the same approach in code written for this fork. Fork-original: the native capture at the first FFXIV UI draw (byunjoe's capture is an external ReShade add-on driven by REST markers; the UI and scene shader fingerprints come from SirenBrink's HDR code), the per-frame state machine with its coverage guard, paste hold and reader-fenced slots, and the summary log. FSR-FG and XeFG receive the tags through upstream's consumption code.

**What it does.** With DLSS-G at 2x to 4x, frame generation works on the UI-free image and the UI is pasted back at the real frame rate, so the HUD stays sharp in fast camera motion. Two modes: `ui_free` (UI-free base image plus pasted UI) and `recomposition` (DLSS-G recomposes the UI from a UI-free and a UI image). FSR-FG and XeFG only receive the tags in `recomposition` mode. Any doubt (title screens, fades, a refused capture) falls back to normal frame generation for that frame. Use `recomposition` with DLSS-G preset Auto or B; use preset A or C only with HUD-less off or `ui_free`, because A and C turn NVIDIA's UI recomposition off (see the [preset entry](#dlss-g-render-preset-a-b-c-switchable-while-the-game-runs)).

**Status.** Opt-in, off by default. Tested in SDR with DLSS-G 2x to 4x on an RTX 4090; no visible gain was reported. **Recommendation: use `recomposition`; treat `ui_free` as experimental.** In one session on 2026-10-22 (SDR, RTX 4090, DLSS-G 2x) `ui_free` stayed in `Fallback(MaskCoverage)` for most presents because the detected UI mask covered 50 to 100 % of the picture, so DLSS-G interpolated the whole frame, UI included. The author saw a faint, fast micro-stutter in motion, only with frame generation on, and it was gone with `Mode = recomposition`. The cause of the mask failure is not investigated; earlier sessions saw `ui_free` Active on 96 to 100 % of presents. HDR output with `ui_free` is untested. One GPU hang happened at the experimental 6x unlock after the mode was toggled quickly. Numbers: [notes/hudless-session-measurements.md](notes/hudless-session-measurements.md). Design and limits: [HUDLESS.md](HUDLESS.md).

**Config.** `[HudLess] Mode` (`off`, `recomposition`, `ui_free`), `MaskThreshold`, `MaskDilation`, `PasteCleanup`, `SteadyTiming`, `CoverageGuard`, `ShowDetectedUi`, `ReShadePresets`, `Diagnostics`; `[DLSSG] UIRecomposition`. **Menu.** `fg_hudless` (tab FFXIV, titled "HUD-less (UI-free frame generation)"); `hudless_diag` (tab FFXIV, diagnostics, details in a closed fold).

**Code and seams.** `ext/hudless/` (engine, ring of shared textures, mask and paste shaders, provider adapters), `ext/ffxiv/ui/` (finds the first UI draw by shader fingerprint). Seams: hudless, present `BeforeOverlay`, fg `AdjustDlssgOptions`, menu `ObservedMultiplier`.

### ReShade built-in add-on

**Origin.** Fork-original. SirenBrink's fork uses an external optional add-on DLL; this one is built in.

**What it does.** OptiScaler registers itself with your ReShade as an add-on (API 20). At the UI boundary it asks ReShade to apply its enabled presets in place, so the UI-free image has the presets and the HUD stays unfiltered, and ReShade's own end-of-frame pass is skipped for that frame. If ReShade is absent, is another version, or anything fails, HUD-less falls back to normal frame generation.

**Status.** Tested as part of the HUD-less sessions, with ReShade 6.8.0 only.

**Config.** `[HudLess] ReShadePresets` (`auto`, `on`, `off`), `ReShadeProbe` (diagnostics). **Menu.** `reshade_addon_probe` (tab Compatibility, diagnostics).

**Code and seams.** `ext/reshade/` (the public ReShade API headers are vendored unmodified; licence notice shipped as `Licenses/ReShade_ATTRIBUTION.txt`). Seam: reshade `OnReshadeLoaded`.

### Automatic ReShade scene depth

**Origin.** Fork-original.

**What it does.** ReShade effects that read depth (MXAO and the like) find the game's depth buffer on their own. When the game renders below the output size (DLSS Quality and lower), its scene buffers are still output-sized and only a window of them holds this frame, so the depth ReShade finds (Generic Depth) does not line up with the picture and effects that read it misfit. That is inferred from the buffer sizes in earlier logs until the capture line in `OptiScaler.log` shows the real depth size. With this option on, each DLSS evaluate copies the Depth texture the upscaler was given (cropped by the subrect origin, nearest-mapped from the render size) into an owned R32_FLOAT texture the size of ReShade's back buffer. While ReShade renders its effects (`begin_effects` to `finish_effects`) the `DEPTH` input is pointed at that copy through `update_texture_bindings`, and finish puts ReShade's own binding back. The copy is made on the game's native immediate context, in a private context state, so the game's pipeline state is untouched. Under frame generation ReShade renders an effect pass for every output frame while the upscaler evaluates once per real frame: every pass between two evaluates binds the copy of the last evaluate (a generated frame gets the real frame's depth, which is intended), and an evaluate that stops (a menu, a loading screen) is caught by the age of the capture (250 ms), not by a frame count. The depth-owned frame counter that stamps a capture ticks once per capture; the first game test (2026-10-25) showed the earlier rule, one tick per finish, refusing about 60 % of the passes as stale under frame generation.

**G-buffers (same switch).** An add-on such as REST (ReshadeEffectShaderToggler) hands the game's G-buffers to ReShade's semantics `NORMALS`, `DECALS`, `DECALS_WATER`, `MOTIONVECTORS` and `MATERIAL` (`update_texture_bindings`, rebound in its own `begin_effects`), and effects such as the Alive NEXT preset's `ffxiv_common.fxh` read them with plain UV 0..1 (`texture texNormals : NORMALS;` and so on, declared in a namespace, so the API names are the short `texNormals`, `texDecalNormals`, `texDecalWaterNormals`, `texNativeMotionVectors`, `texMaterial`). Those buffers are output-sized with the picture only in the render window, exactly like the depth, so with the depth aligned and the normals not, MXAO drew ghost objects into walls (seen in game). With the one option on, after the add-on's begin the binder reads its binding of each buffer, and if the source is output-sized (the size of ReShade's back buffer) while the fresh depth capture says render < output, it makes an output-sized copy of the render window (the same exact-integer nearest mapping as the depth, origin 0,0) in the SAME format as the add-on's view, binds it to the same semantic until `finish_effects`, and puts the add-on's binding back (only if the entry is still ours; `overwrittenByOther` counts the rest). A depth subrect base other than 0,0 (the remap assumes the window starts at the corner; the depth honours the base), a source that is already render-sized, render equal to output (DLAA: the add-on's bindings are not even read; one log line per change), an unreadable or non-2D view, a format the pass does not do (typeless, sRGB views, integer, depth) or any failure keeps the add-on's binding (one log line per change says why). Variables of the same name that show different bindings, the EMPTY ones included (an effect's own texture next to readers that show nothing yet), are ambiguous and are left alone. The counter `kept` is counted but never logged as a rate line. The copy is made at `begin_effects` on the thread that runs `begin_effects` (two possible paths: the HudLess UI-boundary `render_effects` call and ReShade's own pass in Present; the log prints, once per thread that runs begin_effects, the thread next to the evaluate thread and the native context pointer begin_effects drew on), on the context ReShade gives (`command_list::get_native()`), serialized with the depth pass by one process-wide context-state mutex (taken with a 30 ms timeout, fail open: a holder that does not come back costs the pass 30 ms, never a freeze. A timeout happens BEFORE any state swap: the depth capture is skipped and ages out, the G-buffer remap fails and keeps the add-on's binding, a pending swap-back recovery is retried on the next call; each logs once a minute 'context state lock busy for N ms: pass skipped'. It serializes only OUR depth and G-buffer passes; the character-preview passes swap private states without it), in a private context state; one pass object per buffer, cached by (the add-on's view, the depth capture counter), so under frame generation the copy is made once per real frame and the generated passes rebind it. The slots reuse the depth binder (`DepthBinder`: a slot per semantic with its own pass state, counters and trust latch; the variable that reads exactly the copy back vouches for the pass and its pre-bind reading is the original), run only after the depth reached Bound (so a manual depth transform, an untrusted depth input or no capture means no G-buffer work), and cost no backend call when the effects declare none of the five variables. **Motion vector VALUES** are scaled per axis as well: multiplier = the motion vector scale the game reports to the upscaler (`MV_Scale_X/Y`, e.g. 3840 x 2160) divided by the render size (x2 at Performance, x1.5 at Quality), falling back to output/render when the game reports none. DERIVED, not yet confirmed visually: under DLSS's convention the stored vector times the scale is a displacement in render pixels, so the screen-UV motion of the stretched window is the stored value times scale/render; ini-only switch `[FFXIV] AutomaticReShadeMotionScale` (default true, no menu control, goes once confirmed). Unverified undo: if no variable of that name reads the copy back (they are not the semantic's readers), the entry is erased and the slot stops until the effects reload (`erasedUnverified`, `noDepthInput`); readers that go by another name would lose the add-on's binding until it rebinds. A target format the GPU cannot make a render target for (a permanent creation failure such as E_INVALIDARG) is remembered per (view format, output size) and not retried every frame; out-of-memory and device errors are retried. Memory: up to five output-sized copies (33 MB for RGBA8, 66 MB for RGBA16F at 3840x2160), released when the option goes off.

It leaves ReShade alone, and ReShade keeps the depth it detected, whenever anything is not clearly right: another device, no `DEPTH` reader that reads the copy back, no or a stale capture, a size mismatch, the character preview (its output is not the back-buffer size), or any of `RESHADE_DEPTH_INPUT_X_SCALE`, `_Y_SCALE`, `_X_OFFSET`, `_Y_OFFSET`, `_X_PIXEL_OFFSET`, `_Y_PIXEL_OFFSET` set in ReShade's preprocessor definitions to a non-neutral value (a manual depth transform; defined with the neutral value, 1 for the scales and 0 for the offsets, it does not stand down). The game's depth is reverse-Z: if ambient occlusion looks inverted with the option on, set `RESHADE_DEPTH_INPUT_IS_REVERSED=1`. It is for FFXIV only: another executable with the key on does nothing and the status says so. It needs ReShade 6.8 or newer (add-on API 20) loaded in the game; a ReShade installed as `dxgi.dll` that loads after OptiScaler is looked for once a second for the first minute while the option is on. ReShade reports a texture variable under its short declared name (no namespace and no mangling: measured against ReShade 6.8.0.2155), so the depth input is the variable named `DepthBufferTex` or `DepthInputTex`; the matching stays exact. A texture of another effect that happens to read like it (`Foo::DepthBufferTex` comes back as `DepthBufferTex`) is caught by the readback after the bind: a candidate that shows something that is not a depth view makes the runtime untrusted, and one that does not read the copy back is not the reader.

**Status.** Opt-in, off by default. New. The decision logic, the copy pass (real shader, WARP device), the raw ReShade layer (against a fake ReShade DLL), the event wiring, the whole evaluate path (fake parameter block and fake host) and the whole loop (the real engine, the real binder and the real event dispatcher over a fake ReShade world: a full cycle ends bound, the frame stamps agree) have offline tests; the source pins keep the one upstream line and the shutdown calls in place. The first game log (2026-10-25, ReShade 6.8.0.2155) found three things the offline fakes had assumed wrongly, now fixed and covered: ReShade's string getters answer a copy with the number of characters copied WITHOUT the terminating NUL (the vendored header says "including" and was only right for the size query), so every pass failed at its first string read; ReShade hands the game PROXY objects (the device, the context and the swap chain from `D3D11CreateDeviceAndSwapChain`; `GetDevice()` of any resource answers the proxy) while the runtime's `device::get_native()` is the real device, so the two never compare equal by pointer. A runtime now counts as the game's when `get_native()` or the `GetDevice()` of its back buffer equals the capture's device, or when it is the only D3D11 runtime of the process (FFXIV has one device, one proxy/native pair); with two or more runtimes the strict comparison stands and a refusal logs all the pointers once; and ReShade delivers its events on another thread than the NGX evaluate thread, so the events are subscribed at load instead of from the evaluate thread. An offline harness then ran the real raw layer, backend and binder against the real ReShade 6.8.0.2155 (a copy of the game's DLL, a hidden window on the RTX 4090, a fake capture): the string contract holds for every getter (a too-small buffer is silently cut and still answers true, so fixed buffers must never be used), `update_texture_bindings` is reflected by `get_texture_binding` for every `DEPTH` variable, the full cycle reaches bound with the binding restored at finish, and every ReShade event was delivered on one thread in the harness (the game may deliver begin_effects from two paths, see the G-buffer paragraph). The order against Generic Depth and REST is settled by construction (begin_effects and finish_effects are registered from init_effect_runtime, after theirs; the real-ReShade harness with a fake REST shows REST first, ours second, and the game log of depth-20261025d shows the G-buffers realigned with no overwrittenByOther). Still open: whether an add-on registers its begin_effects later than that, which the counter overwrittenByOther (per semantic) would show. `INGAME-TEST-DEPTH.md` in the build folder lists what to compare and what to send back.

**Config.** `[FFXIV] AutomaticReShadeDepth` (false; ini default `auto`; fragment `ext/ini/FFXIV.ini`) governs the depth and the G-buffer realignment together; `[FFXIV] AutomaticReShadeMotionScale` (true; ini only, no menu control; developer switch of the derived motion vector value scaling). Loading or reloading the ini only stores the value; the machinery is built by the first evaluate that finds the option on. **Cost.** With the option off, an evaluate costs one relaxed load. With it on and no ReShade registered, each evaluate also reads the clock and checks the registration, and the late look for ReShade is bounded: once a second, 60 attempts, then it stops (the status line says so). With it on in another game, a few relaxed loads and a cached flag, nothing is built. **Events.** On the load path (ReShade was there when OptiScaler loaded) five ReShade events are subscribed once, right after the add-on registration and whatever the option says (before any dispatch can run, so `register_event` cannot race ReShade's dispatch loop): `init_effect_runtime`, `destroy_effect_runtime`, `reshade_reloaded_effects`, `reshade_set_technique_state` and `reshade_set_current_preset_path`. `begin_effects` and `finish_effects` are subscribed from INSIDE the `init_effect_runtime` callback (finish first, begin only if finish and the rest succeeded; once per process, failures remembered per event, the thread is logged). Why: REST and ReShade's Generic Depth register their `begin_effects` at device creation and rebind their textures there, and ReShade calls the callbacks of an event in registration order, so a begin registered at the first effect runtime runs AFTER theirs and reads what they bound (measured against the real ReShade 6.8.0.2155 with a fake REST: REST first, ours second; if an add-on registers its begin from an init callback that is registered after ours, ours runs first and the counter `overwrittenByOther` shows it). The init callback looks at neither the dispatcher nor the option (the facade is built at the first evaluate with the option on, long after the runtime was created); the other callbacks return at once while nothing was built, a deliberate deviation from "no callbacks with the option off". Nothing but the late path ever subscribes from the evaluate thread: a ReShade that loads after OptiScaler (its runtime and add-ons already exist) gets all six (`begin_effects`, `finish_effects`, `destroy`, `reloaded`, `technique state`, `preset path`) at once after the late registration, on the evaluate thread (the log line says the thread; that path can still race). **Counters (log, per semantic).** `passesBound`, `kept`, `failed`, `overwrittenByOther`, `erasedUnverified`, `noDepthInput` for the depth and, with the semantic and variable name in the line, for each G-buffer (`NORMALS (texNormals): overwrittenByOther = N`); plus a line per change of the decision (`NORMALS (texNormals): the add-on's binding is 3840x2160 DXGI format 28, render 1920x1080 in an output of 3840x2160: realigned ...`) and whether the add-on rebinds a new view between frames. **Menu.** Card "ReShade depth compatibility" (`reshade_depth`, FFXIV tab, after "Window mode"), always shown on the FFXIV build match: checkbox "Automatic scene depth" (it applies without a restart; switching it on takes effect within a few frames) and a status line.

**Status line.** `Automatic scene depth: off`; `ReShade not detected` (also `(stopped looking)` after the late attempts ran out); `ReShade found but refused add-on API 20 (needs ReShade 6.8 or newer)`; `ReShade found but without add-on support ...`; `ReShade found but OptiScaler's own module is unknown`; `Automatic scene depth: not available for this game` (not the FFXIV executable); `Waiting for ReShade depth effects` (registered, no effect asked for depth yet); `Automatic scene depth: 1920x1080 -> 3840x2160` (a pass is bound; at DLAA render and output are one size and it reads `Automatic scene depth: 3840x2160`); `Scene depth capture refused (<reason>); generic depth retained`; `ReShade events could not be subscribed; generic depth retained`; `Automatic scene depth: native context not available`. While G-buffers are realigned the bound line carries a suffix: `Automatic scene depth: 1920x1080 -> 3840x2160; G-buffers realigned: normals, motion` (labels: normals, decals, water decals, motion, material).

**Code and seams.** `ext/reshade/ReShadeDepth*.{h,cpp}` (facade, evaluate engine `ReShadeDepthEval`, binder `ReShadeDepthCore`, copy pass `ReShadeDepthCopy` with `shaders/depth_crop.hlsl`, G-buffer remap pass `ReShadeGBufferCopy` with `shaders/gbuffer_remap.hlsl` and `ReShadeGBuffer.shaders.props`, live backend, event callbacks, late registration, menu card). Seams: reshade `OnEvaluated` (one line after the successful `Evaluate` in `NVSDK_NGX_D3D11_EvaluateFeature`); the shutdown call `ext::reshade::depth::OnNgxShutdown()` sits in the existing seam bodies `dx11bridge::OnNgxShutdownBegin` and `dx11bridge::BeforeReleaseFeature`.

## Window mode

**Origin.** Fork-original. The `[XeFG] ForceBorderless` key it depends on is upstream's (cdozdil).

**What it does.** FFXIV runs borderless when `[XeFG] ForceBorderless = true` (a setting the fork does not turn on by itself). In that mode upstream's hook swallows the game's request to change the window mode and restyles the window itself. Two problems, both seen in logs: after leaving the pseudo fullscreen the window keeps a thin edge (client area 3836x2156 inside a 3840x2160 window), and one window-mode switch made while DLSS-G was running stalled Streamline's frame pacer ("Potential dead-lock in Present ...") and ended with the GPU device removed. Zero-code advice: choose "Borderless Window" in the game's display settings, restart the game after changing the mode, and do not switch to "Fullscreen" while the game runs.

- Probe (`probe` or `on`): one `Window:` line after every successful resize of the bridge swapchain and for every `SetFullscreenState` call that ReShade sees, with styles, rectangles, the frame generation state, thread ids and the verdict. It only reads and logs.
- Border fix (`on`): after `SCExclusiveFullscreen` goes from true to false and frame generation is confirmed off (at least 2 complete off presents and 50 ms), a window with neither caption nor sizing frame but with `WS_EX_CLIENTEDGE` or `WS_EX_WINDOWEDGE` gets those bits cleared. A window with a caption or a sizing frame is never touched.
- Deferred mode switch (`on`): when `ForceBorderless` is on, the output is DLSS-G, frame generation is active and the call would really change `SCExclusiveFullscreen`, a ReShade add-on event holds the game's `SetFullscreenState` back, asks frame generation off, and replays the call once DLSS-G is confirmed off (at least 2 complete presents and 50 ms). Soft wait limit 6 presents or 2 s; the hard limit (24 presents and at least 500 ms, or 6 s) replays regardless, with a warning. Anything unexpected passes the call through as today.

**Status.** Opt-in, off by default; tested (RTX 4090) on 2026-10-19: border fix and two held-back mode switches with DLSS-G on; the open points listed here were not exercised. The rules and decision flows have unit tests on fakes, and the window calls are tested on a real hidden window; the game itself decides whether it tolerates a held-back window-mode call. Not verified in game: what ReShade hands back to the game for a prevented call (not documented in the vendored headers); from roughly 190 to 200 game presents per second the 50 ms floor outlasts the pause in which frame generation stays off, so the hold ends at the hard limit and the border fix expires (`border fix expired`); whether the replay avoids the Streamline stall (look for the absence of `Potential dead-lock in Present` after a `replaying SetFullscreenState` line); a size-changing `ResizeBuffers` cannot be held (warning `the follow-up ResizeBuffers is not protected`), so do the risky switch only after a border-clean leave.

**Config.** `[Window] Mode`: `off` (default: nothing runs and nothing is logged), `probe` (log only), `on` (probe, border fix and deferred mode switch). It applies after a game restart; any other value counts as `off`. **Menu.** Drop-down in the card "Window mode (FFXIV)" (`window_mode`, FFXIV tab, first card): the choice is stored for the next start and the card shows the mode active in this session.

**Log lines.** All start with `Window:`: `module active, mode=...`, `border fix applied exstyle A -> B client WxH -> WxH`, `SetFullscreenState(TRUE|FALSE) held back: ...`, `replaying SetFullscreenState(...)` with the reason (`frame generation confirmed off`, `wait limit reached`, `hard wait limit reached without a confirmation`), `held SetFullscreenState dropped, not replayed: ...` (the mode did NOT change). Probe fields, the other log lines, the present-counter logic and all open points: [notes/window-mode-design.md](notes/window-mode-design.md).

**Code and seams.** `ext/window/` (`WindowRules.h` pure rules, `WindowController.h` decisions behind an `Env` interface so tests use fakes, `WindowModule.cpp` live entry points). No seams in upstream files: the module is called from the existing fork seam bodies `dx11bridge::OnResizeDone` and `dx11bridge::PresentScope` and from a ReShade add-on event (`ext/reshade/ReShadeEvents.cpp`).

## FFXIV module

Everything that is specific to FFXIV is under `OptiScaler/ext/ffxiv/`. It activates only when the running `ffxiv_dx11.exe` matches the supported build recorded in `ext/ffxiv/GameBuild.h` (see [FFXIV-PATCH-RUNBOOK.md](FFXIV-PATCH-RUNBOOK.md)); on any other executable the pieces below do nothing. The module implements the interfaces in `ext/game/`. The build gate compares the executable's PE `TimeDateStamp` and `SizeOfImage`; SirenBrink's fork compares fixed addresses with `memcmp`. The addresses and guard bytes are SirenBrink's.

### Native quality and forced quality

**Origin.** Ported from SirenBrink's fork (the hook code matches in comments, log lines and GUIDs). The install retry follows the approach of SirenBrink's hook-install retry, committed there first (2026-10-19); its code is written for this fork.

**What it does.** Reads the game's own DLSS quality setting and its live changes, answers the game's size query with the preset you choose (`ForcePerfQuality`), requires true DLAA where the game asks for it, and lets the render size follow. If the first Detours transaction fails for a reason that can clear by itself (a game thread exiting while it is enlisted, another component's transaction open), the install is tried again 250 ms and then 2 seconds later, for at most three attempts; a wrong game build or a byte mismatch ends after one. Log: `install attempt N/3 failed at <stage>` and, if a later attempt works, `installed on attempt N`.

**Status.** tested. The retry is new and tested against the real Detours library; the install succeeded on its first attempt in every game log since 2026-10-22; the retry path itself (a failed first attempt) has never been seen in game.

**Config.** `[QualityOverrides] ForcePerfQuality`; no setting for the retry. The `ffxiv_dx11.exe` entry in `GameConfigs.cpp` gains the quirks `ScaleDisplayColorForForcedQuality` and `ReleaseBackbufferOnce`. **Menu.** `forced_quality` (tab Upscaling).

**Code and seams.** `ext/ffxiv/quality/`, `ext/upscaler/ForcedQuality.h`, `UpscalerSeams.cpp`. Seams: upscaler `EffectivePerfQuality`, `OnOptimalSettings`, `ApplyForcedQuality`.

### Render geometry

**Origin.** Ported from SirenBrink's fork.

**What it does.** FFXIV can allocate its scene buffer at display size while rendering a smaller region. The module corrects the render size and the sub-rectangle handed to the upscaler.

**Status.** tested. **Config.** `[Hotfix] RenderSizeFromWidthHeight` (default off; opt-in). **Menu.** Card "Advanced (FFXIV)" (`ffxiv_advanced`, FFXIV tab, closed fold).

**Code and seams.** `ext/ffxiv/geometry/`, `ext/upscaler/RenderGeometry.h`. Seams: upscaler `ResolveRenderGeometry`, `CorrectRenderSubrect`.

### Forced-quality colour scaling

**Origin.** Ported from SirenBrink's fork.

**What it does.** When the game's colour is display-sized but the forced preset renders smaller, the display-sized colour is Lanczos-scaled to render size before DLSS.

**Status.** tested as part of forced quality. **Config.** None. **Menu.** None. **Code and seams.** `ext/dx11bridge/ColorScaler_Dx12.cpp`, `BridgeState.cpp`. Seam: dx11bridge `PrepareUpscalerColor`.

### GPU submission ownership

**Origin.** Ported from SirenBrink's fork (GPU lifetime tracking, after stegalloway's work).

**What it does.** Serialises command-list execution and teardown for the objects NR, HDR and the AMD bridge keep alive across submissions.

**Status.** Runs inside the tested paths. **Config.** None. **Menu.** None. **Code and seams.** `ext/gpu/`. Seams: gpu, restrack, dx11bridge.

### Quality change without the stall

**Origin.** Follows the approach of SirenBrink's sleep skip, committed there first (2026-10-19). The code is written for this fork.

**What it does.** Changing the DLSS quality in FFXIV releases the DX11 upscaler and builds a new one; upstream sleeps a fixed 500 ms between the two. On a matched FFXIV build the release closes the handle to new evaluates, waits (at most 200 ms) for an evaluate already running, and drains the GPU queues the feature could still be running on (each fence wait capped at 5 s). Only if all of that is proven (log result `drained`) does it skip the sleep. Anything unproven gives `not-applicable` and the 500 ms pause stays; a failed drain (`failed`) refuses the release as before. With XeFG output and `[XeFG] ResetOnUpscalerChange` on (the default) the pause stays by design. The DX11 upscaler table has a narrow lock now, because the game changes quality on its update thread while the render thread evaluates (inferred from the code, not yet seen).

**Status.** on by default; new, unit-tested (drain classification, lock and registry, release sequence with fakes, thread trace, source pins); ran in game (RTX 4090) since 2026-10-20: the sleep was skipped and 0.06 to 30 ms were waited instead of 500 ms. Known limits and the full text: [notes/ngx-release-skip-sleep.md](notes/ngx-release-skip-sleep.md).

**Config.** `[NgxRelease] SkipReleaseSleep` (ini only, default on; off = the old flow exactly: no closing, no waiting, the 500 ms pause; the table lock stays). **Menu.** None.

**Code and seams.** `ext/dx11bridge/` (`NgxLifecycle.cpp`, `NgxFeatureTable.h`, `ReleaseSequence.h`), `ext/gpu/DrainResult.h`, `ext/upscaler/NgxThreadTrace*.h`. Seams: dx11bridge `BeforeReleaseFeature` (now returns a three-way result), `ShouldSkipReleaseSleep`, `SetCurrentFeature`, `ClearCurrentFeatureIf`; upstream `inputs/NVNGX_DLSS_Dx11.cpp` (+13 -13). Log: `FFXIV upscaler release: handle <n> result=<drained|not-applicable|failed> (...), waited <x> ms, sleep <skipped|kept (500 ms)|n/a (shutdown)>`, and `FFXIV NGX thread: ...`.

### Lighting scan

**Origin.** Ported from SirenBrink's fork.

**What it does.** Hooks the game's tone-mapping and exposure shaders (by fingerprint, no game addresses) to read scene lighting and tell NR when the history is invalid (lighting changes, scene transitions).

**Status.** Runs inside the tested NR path; not tested on its own.

**Config.** `[DlssNr] LightingHistory`. (`ScanAnchors`, `ScanExposure`, `ScanMeter`, `ScanTrim` and `ScanInverted` belong to the generic DX12 exposure scan, which is switched off on FFXIV: `Wanted()` in `DlssNr_ExposureScan.cpp` returns false for a native-lighting game.) **Menu.** None of its own.

**Code and seams.** `ext/ffxiv/lighting/`. Seams: game `OnD3D11DeviceHooked`, `OnD3D11Unhook`; dx11bridge `OnUpscaleBoundary`.

### Companion nameplates

**Origin.** Ported from SirenBrink's fork (the Companion files match it closely by line).

**What it does.** Replays the game's nameplate (HUD label) draws in a separate layer so they can be interpolated and kept sharp with frame generation, and draws source-frame markers after the bridge copy. It reads the game's UI queue through the addresses in `GameBuild.h`.

**Status.** tested. **Config.** `[Companion] HudReplacement`, `HudInterpolation`. **Menu.** `ffxiv_companion` (tab FFXIV, card "FFXIV Companion (nameplates)", shown for FFXIV).

**Code and seams.** `ext/ffxiv/companion/`. Seams: game, dx11bridge `AfterSharedCopy`, core `HooksBypassed` (lets the layer's own device and swapchain calls pass the fork's hooks).

### Composition swapchain hook

**Origin.** Ported from SirenBrink's fork (the hook was started by janblade and wilsjo2).

**What it does.** Hooks `CreateSwapChainForComposition` so composition swapchains created by overlays do not break the frame-generation presenter, with a pass-through for the Companion layer's own swapchain.

**Status.** untested on its own. **Config.** None. **Menu.** None.

**Code and seams.** `ext/present/` (`CompositionSwapchain.cpp`, `CompositionPassThrough.h`). Seams: present `HookFactoryComposition`, `ResolveCompositionHandle`; core `HooksBypassed`. Inline marker: `// ext:` comments in `wrapped/wrapped_swapchain.cpp/.h` (the lines that use `_composition`) and in `hooks/DxgiFactory_Hooks.cpp/.h`.

## Game jitter (FFXIV)

The game renders its scene with a jitter translation it scales itself and tells the upscaler the unscaled pixel jitter. The two agree only when the game's scale `s` is 2. Three features deal with that: a read-only probe, a correction of what DLSS is told, and a fix of what is drawn. They share the card "Game jitter (FFXIV)" (`ffxiv_jitter`, FFXIV tab, after the FFXIV HDR10 card), the code in `ext/ffxiv/jitter/` and the existing upscale-boundary seam (`ext/Seams.cpp` calls `ext::ffxiv::jitter::OnUpscaleBoundary`); none adds an upstream seam. Measurements, the mechanism text and the log budget: [notes/jitter-measurements.md](notes/jitter-measurements.md).

### Game jitter probe

**Origin.** Fork-original; SirenBrink's fork has no probe. SirenBrink's `Config.md` documents the 0.6 native raster strength first (commit `5bfa6e92`, 2026-10-19); this fork's first in-game sample of 0.600 came about 1 h 22 min later, and its formula and the design of the probe date from 2026-10-18.

**What it does.** `[FFXIV] JitterProbe` reads the game's camera jitter object (read only, guarded, matched game build only) and writes one log line about every 10 seconds, and early when the render size or preset changes: `Game jitter: counter N, reported jx/jy ..., render WxH (source), applied ax/ay ..., ratio rx/ry ..., signs ok, implied s = 2.000/2.000 (consistent), ...`. `s = 2` on both axes means the game applies what it reports; another value means the upscaler is told a different jitter than the one in the picture; `signs MISMATCH` means an axis has the opposite sign. A failed check logs one line per episode (`Game jitter: not read, guard failed: ...`) and does nothing else.

**Status.** Diagnostic, off by default. Measured in game (the released build, DLSS preset M): the picture moves by only 60% of the jitter the game tells DLSS about, `ratio rx/ry 0.600/0.600` in every sample, at DLAA and at Performance alike. Transformer presets (L, M) depend on accurate jitter more than the CNN preset K, which fits the reports that L and M look jittery. Quality (1.5x) and Balanced are not measured.

**Config.** `[FFXIV] JitterProbe` (false). **Menu.** Checkbox in the card "Game jitter (FFXIV)"; it applies at once and is stored in the ini when the settings are saved. **Code and seams.** `ext/ffxiv/jitter/` (`FfxivJitterProbe.cpp|.h`, `FfxivJitterCore.h`, `FfxivJitter.h`, `FfxivJitterMenu.cpp`).

### Jitter correction

**Origin.** Fork-original mechanism: it learns the applied/reported ratio and scales what DLSS is told. SirenBrink's fork solves the same goal with the source fix described below.

**What it does.** `[FFXIV] JitterCorrection` learns the ratio applied/reported from snapshots of the game's jitter object and multiplies the `Jitter.Offset.X/Y` pair of every DLSS evaluate by it. Five accepted snapshots in a row that agree within 2% adopt their mean (`Jitter correction: calibrated ratio 0.600/0.600 from 5 snapshots for render 1920x1080, output 3840x2160, quality 0; scaling the reported jitter`); until then the game's values pass through. A ratio within 2% of 1 applies nothing; above 1.02 or below 0.25 it is refused. A different configuration (render size, output size, quality) drops the calibration. The scaled pair is written once and nothing puts it back, so super resolution, frame generation, the DLSS-NR path, the `Jitter Count` value in the menu and the FSR and XeSS backends all see it. Only the input named `Upscaler` reads the scaled pair from the parameter block. Test with frame generation (and NR) off first, then on.

`[FFXIV] JitterScaleOverride` (0 = off; `auto` means 0) is an experiment key and may be removed once the correction is validated. A value from 0.25 to 1.0, used while `JitterCorrection` is true, takes precedence over the calibration. Values to try with the same scene: `1.0` (the baseline), `0.8`, `0.6` (what the probe measured) and `0.5`.

**Status.** Experimental, off by default. The correction ran in game and scaled the told jitter (2026-10-20 and 2026-10-23); its earlier version showed no visible gain at the Quality mode (2026-10-19); the look of the current version is not recorded. Test with frame generation off first. An earlier version replaced the pair only after proving for every evaluate that the object belonged to it; in the author's session that proof held for 11% of 9418 evaluates and the correction flapped, which is why this design replaced it. Quality (1.5x) and Balanced are not measured. Other limits: the notes file.

**Config.** `[FFXIV] JitterCorrection` (false), `JitterScaleOverride` (0). **Menu.** Checkbox for `JitterCorrection` and the slider "Manual jitter scale (0 = learn it)" in the card "Game jitter (FFXIV)" (steps of 0.05, below 0.25 snaps to 0, greyed out while the correction switch is off).

**Check.** A status line comes about every 10 seconds for the first 60 lines, then once a minute: `Jitter correction: on; totals since the start of the session: N evaluates seen, N scaled, ...; state: calibrated ratio 0.600 for render 1920x1080, ...`. In a good session a `calibrated ratio` line comes within a second or two, `scaled` stays close to the number of evaluates, and `disagreed` and `ratio changes` stay near zero. **Code and seams.** `ext/ffxiv/jitter/` (`FfxivJitterCalibration.h`, `FfxivJitterCore.h`).

### Jitter source

**Origin.** Follows the approach of SirenBrink's v6.0 `JitterSource`, committed there first (2026-10-19). Hook scope, return-address filters, the DLSS gate, the strength range, the normalise and projection checks and the ring criteria are the same, and constants and names (`Generator`, `Getter`, `NormalizeHalton`, a ring of 32 with 1e-7 and 1000 ms) are shared. The logic was ported by re-typing it. The phase-count rule is v6.0's `TemporalSampling::PhaseCount`, and the guard table shares block starts and one fingerprint. The controller, world-camera tie, verification, watchdogs, fallback, stand-down, opt-in key and menu are fork code. How NGX is told differs: v6.0's getter returns a thread-local packet and a scope restores it; this fork writes once and never restores.

**What it does.** In DLSS mode the game draws the scene with only a fraction of the sub-pixel jitter its generator produces (native strength, measured 0.6, so +-0.3 px instead of +-0.5 px) and calls the generator with an unbounded phase count. `JitterCorrection` cannot recover samples that were never drawn. With `JitterSource=true` a hook on the generator divides its output by the native strength, so the game's multiplication leaves the full +-0.5 px lattice, and replaces the phase count by NVIDIA's `ceil(8 * output pixel area / render pixel area)` (8 at DLAA, 18 Quality, 24 Balanced, 32 Performance, 72 Ultra Performance). It only touches the WORLD camera's jitter object. At the DX11 evaluate boundary `Jitter.Offset.X/Y` are replaced by the offset that was drawn, so super resolution, frame generation and the Jitter Count readout get the drawn value; `JitterCorrection` stands down for every evaluate the fix tells.

Fail-open means today's behaviour: a mismatch of the executable, a failed hook install, the key off, or a generator that is not being normalised switches the hook and the told-offset write off together. Every frame is verified (tolerance 0.005 px); 90 consecutive frames outside, or 90 consecutive evaluates that match no camera sample, make the fix fall back for the session (switching the key off and on again tries once more). The character preview keeps its own 32-phase path.

**Status.** Experimental, opt-in, default off; tested in game for the lattice only. The arithmetic and the executable anchors were checked against the October 20 executable (SHA-256 46A52AB1...). In game, the log showed the full +-0.49 px lattice with 32 distinct phases at Performance and 0 frames outside tolerance. The author reported motion "less jumpy" with it on, but `[HudLess] Mode = ui_free` was active at the time, so that impression is confounded by `ui_free`. The key did not remove the faint micro-stutter seen with frame generation in that session: that came from the `ui_free` fallback (see [HUD-less frame generation](#hud-less-frame-generation)) and was gone with `recomposition` whether the key was on or off (with the DLSS-G preset on Auto or B). It supports one game executable build.

**Config.** `[FFXIV] JitterSource` (false; ini default `auto`). **Menu.** Checkbox in the card "Game jitter (FFXIV)".

**Check.** Log lines: `Jitter source: hooks installed (...)`, `Jitter source: active (strength 0.600, phases 32 for render 1920x1080 output 3840x2160)`, `Jitter source: verified N frames, drawn offset equals the generator's value (max deviation ... px, outside 0.005 px: 0), ...`, `Jitter source: inactive (<reason>)`, `Jitter source: fell back to the old behaviour, ...`. **Code and seams.** `ext/ffxiv/jitter/FfxivJitterSource*`.

## Character preview (CharaView)

**Origin.** SirenBrink's v6.0 contains the same feature (commit `fe208a59`, 2026-10-16): the isolated DLSS Super Resolution pass with 2x output, the replayed G-buffer draws, the depth and camera guides and the spatial AA with 2x upscaling. This fork's version matches it closely. The config, the target table and the guards are written here. The FXAA and guide shaders (same constants, order and names), seven geometry shader CRCs, the 18-byte code pattern with 20 entries and the 100 ms cache match v6.0. The 32-phase preview jitter was in this fork's code first (2026-10-18); v6.0 used 16 phases at `fe208a59` and moved to 32 in `5bfa6e92` (2026-10-19). Credit: [ATTRIBUTION.md](ATTRIBUTION.md); details of the behaviour: [RECONSTRUCTION-REFERENCE.md](RECONSTRUCTION-REFERENCE.md).

**What it does.** The game draws the 3D character preview (character, equipment and glamour windows) in a small separate render. CharaView gives that render its own isolated DLSS Super Resolution pass with 2x output (jittered rasterisation, replayed G-buffer draws for signed motion vectors, depth and camera guides, native alpha composed back), and offers an optional FXAA-style spatial anti-aliasing pass with 2x upscaling for the equipment preview. The jitter cycle is 32 phases (the log line "CharaView: DLSS path in that burst" ends with "jitter cycle 32 phases"). It fails open: any doubt leaves the native image. It reads the game's `.text` once, read-only, for an 18-byte code pattern that must match exactly once at the documented addresses; otherwise the feature switches itself off for the session with one log line.

**Status.** Opt-in, off by default; tested (RTX 4090) on the 2026-10-18/19 builds with one preview window: spatial AA, DLSS 2x, NR, NR before DLSS; not re-checked on this release; several preview windows and the D3D12 debug layer not checked. Not implemented: frame generation for previews (not needed: the preview is part of the UI, which the game's frame generation does not interpolate separately), portrait windows (it is not known which windows reach this renderer), and deferred contexts (those draws stay native).

**Config.** `[FFXIV] PreviewDLSS` (false); `PreviewAA` (false; with `PreviewDLSS` on, the DLSS pass takes the equipment preview and the AA is bypassed, but while that pass cannot run the AA is used instead); `PreviewUpscale` (true, only meaningful with the AA); the NR keys under [Preview Neural Rendering](#preview-neural-rendering). Debug: `PreviewGuideCapture` (false; capped dumps to `OptiScaler\PreviewDLSSDiagnostics\`), `PreviewDebugTint` (false), `PreviewDebugFlood` (false), `PreviewProbe` (false; logs copies of the preview, saves PNGs to `OptiScaler\PreviewDebug\`, reads live commands from `OptiScaler\chara-probe.cmd`). The probe commands, output files and caps: [notes/charaview-preview-nr.md](notes/charaview-preview-nr.md).

**Menu.** Card "Character preview" (`ffxiv_character_preview`, FFXIV tab): Preview DLSS, Preview spatial AA with its 2x upscaling option and Preview NR with "Run NR before DLSS" on top, the one-line states and the reason when the feature is off. Every counter and debug aid sits in a closed "Advanced (debug)" fold.

**Code and seams.** `ext/ffxiv/chara/` (with `shaders/`), hooks inside `ext/ffxiv/lighting/FfxivLightingCapture.h`, the per-frame callback through `dx11bridge::AfterSharedCopy` in `ext/Seams.cpp`, ini fragments `ext/ini/FFXIV.ini` and `ext/ini/HDR.ini`. No seams in upstream files.

### Preview Neural Rendering

**Origin.** Different mechanism from v6.0, which runs NR at the render size, adds the delta after DLSS and has a `PreviewNRReuse` option. This fork runs NR on the 2x output, alone on the native image, or (experimental) before DLSS. The code is the fork's.

**What it does.** `[FFXIV] PreviewNR` runs the DLSS NR model on each open character preview window. The model only runs on Direct3D 12, so the preview image is handed to the DirectX 11 on 12 bridge's device through shared textures and fences and drawn from a shared result texture. Every failure keeps the native image and switches preview NR off for the session. It needs an NVIDIA card, the bridge, `nvngx_dlssnr.dll` and `nvngx.dll_dlssnr.dll`; `[DlssNr] Enabled` is not needed. Alone (with `PreviewDLSS` off) it runs on the native preview image; with `PreviewDLSS` on it runs on the Super Resolution result (the 2x output) between the Super Resolution evaluate and the alpha pass. The cost per window is one NR pass at 2.2 megapixels.

`PreviewNRBeforeSR` (false, experimental) runs the preview NR on the render-size picture before the DLSS evaluate, with `PreviewDLSS` and `PreviewNR` both on. The model works at a quarter of the pixels, so each window is expected to use less GPU time and video memory; with one window it cost about 2.8 to 3.6 ms of GPU time per frame and about 213 MB (2026-10-19), against about 4.1 ms and 739 MB for NR after DLSS on an earlier build (2026-10-18; different builds, so the comparison is rough). NR sees a jittered picture that has not been anti-aliased, so shimmer or trails on hair and cloth are possible. If the DLSS evaluate refuses the NR image, that render is evaluated again on the plain picture and NR before DLSS is switched off for the session with one log line.

**Status.** Opt-in, off by default; tested (RTX 4090) on the 2026-10-18/19 builds with one preview window: spatial AA, DLSS 2x, NR, NR before DLSS; not re-checked on this release; several preview windows and the D3D12 debug layer not checked. Cost of one open window with NR before DLSS (2026-10-19): the model took about 72.8 ms to create, the GPU time was about 2.8 to 3.6 ms per frame and the video memory grew by about 213 MB; an earlier build with NR after DLSS (2026-10-18) showed about 4.1 ms and +739 MB. Not checked in game: the D3D12 state tracking of the production inputs under the debug layer, whether the model accepts guides smaller than the colour, the image quality on the alpha-masked silhouette and on moving characters, and the cost with several windows open. Full text: [notes/charaview-preview-nr.md](notes/charaview-preview-nr.md).

**Config.** `[FFXIV] PreviewNR` (false), `PreviewNRBeforeSR` (false), `PreviewNRLingerSeconds` (5; keeps a window's model after the window closed), `PreviewNRLate` (false; shows the previous frame's result and avoids the GPU wait on the DirectX 11 side at the cost of one frame of lag; bring-up only); `[HDR] PreviewNRTrace` (false; trace log lines only). **Menu.** In the "Character preview" card; the cost per open window is shown there and in the log (`CharaView NR:` lines). **Code and seams.** `ext/ffxiv/chara/CharaNr.cpp`, `CharaNr.h`, `CharaNrRules.h`. No seams in upstream files.

## Frame pacing trace

**Origin.** Follows the approach of SirenBrink's frame pacing trace and hidden-present `DO_NOT_WAIT` switch, both committed there first (2026-10-16). The code is written for this fork; the trace accumulator and the log line format match.

**What it does.** With `[FramePacing] Trace=true` (or file logging at Debug or Trace with the key left on `auto`), FFXIV logs every 10 seconds and per thread one line per timed place of the present path (native present, hidden DX11 present, FG dispatch, Reflex sleep, output present, scene NR recording, preview NR): `Frame pacing CPU [<name>] tid <n>: calls N, mean x ms, max x ms, >=8ms a, >=16ms b, >=33ms c`. With DLSS-G active the phases nest, so compare each on its own. The lines are CPU time only; queued GPU work is not counted. One line at the start says whether the trace runs: `Frame pacing trace: on (key true)`, `on (Debug log)`, `off (key unset, log level 2)`, `off (key false)` or `off (not FFXIV)`. `[FramePacing] HiddenPresentDoNotWait=true` adds `DXGI_PRESENT_DO_NOT_WAIT` to the bridge's present of the hidden swapchain; a busy chain skips that present instead of blocking (counted as `skipped k` on the hidden-present line). If DXGI answers a flagged present with an invalid-argument or invalid-call result, the flag is switched off for the rest of the session (one WARN line, `hidden DX11 present: DXGI rejected DO_NOT_WAIT`).

**Status.** new. The core, the policy, the seam body (against a fake swapchain) and the DXGI flag handling (against a real WARP swapchain) are unit-tested; nothing of it has run in game.

**Config.** `[FramePacing] Trace` (default off), `HiddenPresentDoNotWait` (default off). Both are ini only. **Menu.** None.

**Code and seams.** `ext/present/` (`FramePacingCore.h`, `FramePacingTrace.cpp`, `HiddenPresentPolicy.h`). Seam: present (`PacingScope`, `HiddenPresent`, `RecordSample` and the begin/end pairs); upstream lines: 3 (`wrapped/wrapped_swapchain.cpp`, `hooks/FG_Hooks.cpp`, `with_dx12/dx11_with_dx12_sc.cpp`).

## Automatic mipmap bias

Two modes. In FFXIV the bias is applied when the game binds a sampler (the bind-time mode). In other games, or if the sampler hooks cannot be attached, the older create-time mode is used; it stands down while the bind-time hooks are attached. Design discussion and the full text of both modes: [notes/mipmap-bias-design.md](notes/mipmap-bias-design.md).

**Origin.** The bind-time mode follows the approach of SirenBrink's v6.0 bind-time mip bias, committed there first (2026-10-19): six shader stages, an immutable sampler copy, and `GetSamplers` returning the game's sampler. The table cap of 256, a comment text and the `AutomaticFFXIV` alias may echo v6.0. v6.0 replaces the game's bias, while this fork adds to it by default and detects add or replace; the bind path here has no lock and a manual override works live. The create-time mode, `MipmapBiasAuto`, the offset and the add or replace detector are fork-original (committed 2026-10-18); the formula is upstream's mip bias calculator.

**Config.** `[Mipmap] MipmapBiasAuto` (default on for FFXIV, off for other games; `AutomaticFFXIV` is accepted as another name, the main key wins), `MipmapBiasAutoOffset` (-3 to 3, default 0), `MipmapBiasObserve` (debug, off: logs every distinct original sampler bias seen at each ratio); fragment `ext/ini/Mipmap.ini`. A value set by hand (`MipmapBiasOverride`, also the "Mipmap Bias" card) always wins: the bind-time mode applies nothing while it is set (log `Bind-time mip bias: not applied because MipmapBiasOverride is set by hand`); set it to `auto` and the feature resumes at the next evaluate. A live ini that carries a number in `MipmapBiasOverride` gets none of this until that key is `auto`. **Menu.** Card "Automatic Mipmap Bias" (`mipmap_auto`, Textures tab, after Anisotropic Filtering).

### Bind-time mode (FFXIV)

**What it does.** Keeps the texture sharpening matched to the upscaling ratio with no number to work out by hand and no restart after a quality change. The target is `log2(render width / output width)` of each DX11 upscaler evaluate (DLAA 0, Quality 1.5x -0.585, Performance 2x -1.0) plus `MipmapBiasAutoOffset`. FFXIV caches its sampler states, so the fork hooks `SetSamplers` and `GetSamplers` of the six shader stages on the game's immediate context and binds an immutable copy with the current target for samplers the game made with a negative bias; `GetSamplers` still returns the game's own sampler, and at most 256 samplers take part. The target is ADDED to the game's own bias until the fork sees that the game follows the mode; it then switches to REPLACE, provisional first. The decision is logged (`Bind-time mip bias: ... REPLACE mode ...` or `... staying in ADD mode ...`).

**Status.** Ran in game (RTX 4090) on 2026-10-20, 21 and 23; no fps loss measured (comparison across sessions, not a same-session A/B); look not compared. The pure rules, the sampler table, the bind path, the six stages through real Detours hooks on a software D3D11 device, the settings and the install order are tested offline (`tests/ext/upscaler/mip_bind_*`). The older create-time mode ran in sessions with the manual value. Known limits: the notes file.

**Check.** Remove or set to `auto` any `MipmapBiasOverride` in the ini, start the game with default settings and read `OptiScaler.log`: `Bind-time mip bias: sampler hooks installed ...`, `CreateSamplerState hook installed` at the first upscaler create, then one `Bind-time mip bias: render width W, output width O, ratio bias B, offset X -> target T (add mode, first measurement)` line and one line per later change of the quality preset (targets 0.0000 at DLAA, -0.5850 at Quality 1.5x, -1.0000 at Performance 2x), and after the first change one `REPLACE` or `ADD` decision line. The textures must keep their sharpness after each change without a restart and the frame time must not change. With `MipmapBiasAuto=false` there are no `Bind-time` lines.

**Code and seams.** `ext/upscaler/MipBind*.h`, `MipBindHooks.cpp`. No new seam: `OnD3D11DeviceHooked`, `OnD3D11Unhook` and `OnUpscaleBoundary` also reach the bind-time module, and the first upscaler create reaches it through `mipbias::OnFeatureCreate`.

### Create-time mode

**What it does.** For other games, or when the sampler hooks cannot be attached. While `MipmapBiasAuto` is on, the value is `log2(render width / display width)` of the newest upscaler the game created plus the optional offset. It is applied the way upstream applies `MipmapBiasOverride` to the samplers the game creates: added to the game's own bias, only for samplers that already have a negative bias unless "MB Override All Textures" is ticked, and replacing those biases by the single value with the upstream "MB Fixed Override" option. 3840x2160 gives 0, a forced Quality preset (2560x1440) -0.585, Balanced (2258x1270) -0.766. A create that is not a game scene (size missing or outside 20 to 16384, display narrower than 640 pixels, portrait display) is ignored. It only reaches samplers created after a change.

The automatic bias does not engage while "MB Scale Override" is on (one warning, `Automatic mipmap bias: not applied while MB Scale Override is on ...`, a red line in the card, your manual value stays in force). Ticking or unticking Scale while the option is on takes effect at once while the Automatic Mipmap Bias card is on screen, and otherwise at the next quality change (the next time the upscaler feature is created).

**Status.** Opt-in, off by default; ran in game on 2026-10-19; the bind-time mode replaced it as the default. The sampler path is the manual value's, which runs in every tested session; the automatic part (which create to follow) was also replayed offline on the logged sessions.

**Check.** Set `MipmapBiasAuto = true`, start the game and read `OptiScaler.log`: every accepted create logs a line (on FFXIV every line comes twice in a row and the second says `unchanged`): `Automatic mipmap bias: create render 3840x2160, display 3840x2160, ratio 1.000 -> bias 0.0000 ...` at the login screen and `... create render 2560x1440, display 3840x2160, ratio 1.500 -> bias -0.5850 (was 0.0000 ...)` after the quality switch. An ignored create logs `ignored create ...` with the reason. With `MipmapBiasScaleOverride = true` the first create logs the warning above instead.

**Code and seams.** `ext/upscaler/MipBias.h`, `MipBias.cpp`, `ext/upscaler/menu/MipBiasMenu.cpp` (tests `tests/ext/upscaler/mip_bias_*`). No seam added: the existing upscaler seam `ApplyForcedQuality` also hands every feature create to the automatic mip bias.

## DLSS presets L and M

**Origin.** Upstream OptiScaler's `[DLSS] RenderPresetForAll`. The origin of the fork's forwarding was not determined.

**What it does.** The fork forwards `[DLSS] RenderPresetForAll = 12` (L) and `13` (M) to NGX unchanged, like the working `11` (K). **Status.** No in-game result is recorded here. **Check.** Set the key, start the game and look for `Using App hint Preset L` (or `Preset M`) in `OptiScaler.log`; a line that still says K means the DLL does not accept the letter. **Config.** `[DLSS] RenderPresetForAll`. **Menu.** None of its own.

## Menu style (Ember)

**Origin.** Fork-original. `EmberPalette.h` says its values are "the same values as Lucent's app tokens"; that source is not identified.

**What it does.** A second look for the in-game menu, selected with `[Menu] Style = ember` (the default, `classic`, is upstream's). The classic theme mixes the accent colour into almost everything, so a bright accent such as orange turns buttons, title bar and headers brown. Ember keeps every surface and control neutral (three steps of one charcoal, hairline borders, rounder corners, wider padding) and uses the accent only to mark state: the check mark, the slider grab, the overline of the selected tab, the focus ring, the text selection, the resize grips and the plots. The accent is `[Menu] AccentColorR/G/B` and the window's opacity is `[Menu] BGColorA`; `BGColorR/G/B` is not used. Under HDR output the colours are tone-mapped like the classic ones. With `LightTheme = true` the menu stays classic.

**Status.** Opt-in, off by default. Played in game on one RTX 4090. The palette and its rules are unit-tested (every slot set, the neutrals identical for every accent, contrast of text 12:1, dim text 4.5:1 and the orange accent 3:1 on the card), and so are the ImGui adapter and the single upstream line. The colour values and shape list: [notes/ember-style-details.md](notes/ember-style-details.md).

**Config.** `[Menu] Style` (`classic` or `ember`; anything else is `classic`). Ini only: no menu control. It applies the next time the menu opens, and when a theme colour is changed in the menu's own theme section. **Menu.** None of its own.

**Code and seams.** `ext/menu/` (`EmberPalette.h` is the palette as plain data, `MenuStyleSeams.cpp` copies it into the ImGuiStyle). Seam: menu `ApplyStyle`. Tests: `tests/ext/menu/ember_palette`, `menu_style_parse`, `menu_style_config`, `menu_style_imgui_map`, `menu_style_call_site`.

## Linear-light NR mode and HDR PNG with menu (F2, F3, F4)

**Origin.** SirenBrink's v6.0 contains the same three features (commit `fe208a59`, 2026-10-16), and this fork's versions match it closely. The NR linear-light shader branch is identical; the compiled shader blob in v6.0 has the SHA-256 (`b9cbd405...aab8`) that this fork records for its own compile. The F3 menu strings and the F4 tooltip are verbatim. CharaView, described above, is the fourth feature of the same commit. Credit: [ATTRIBUTION.md](ATTRIBUTION.md); details of the behaviour: [RECONSTRUCTION-REFERENCE.md](RECONSTRUCTION-REFERENCE.md).

**Status.** untested in game (all three). Seams: none added by F2 or F3; F4 uses the existing `dx11bridge::PrepareScreenshot` (it returns early while a menu capture is wanted) and `hdr::OverlayScope`.

| ID | What | Code | Config | Menu |
|---|---|---|---|---|
| F2 | NR compute-shader edit for the "Linear light" proxy | `ext/nr/shaders/dlssnr.hlsl` | none | none |
| F3 | NR HDR mapping "Linear light + composed" | `ext/nr/` (config, menu, presets), `ext/ini/DlssNr.ini` | `[DlssNr] ReversibleMode = 5` | `dlss_nr` |
| F4 | HDR PNG "Include OptiScaler menu" | `ext/hdr/screenshot/` (`MenuCapture.h`), `ext/ini/HDR.ini` | `[HDR] ScreenshotMenu` | `ffxiv_hdr` |

## Menu layout

**Origin.** The tab bar, the cards and the Custom Tab are upstream OptiScaler's (FakeMichau). The card registry, the layout table and the manifest are fork-original. The FFXIV tab is one `tabs[]` row here; SirenBrink's fork checks the game executable inline.

**What it does.** The in-game menu keeps upstream's tabs and cards; the fork's cards join them through the registry in `ext/core/MenuRegistry.h`.

- **FFXIV tab.** Everything that needs the FFXIV build match sits in one tab, "FFXIV", third in the tab bar right after Frame Gen, hidden in every other game. Its cards, in order: Window mode, ReShade depth compatibility, FFXIV HDR10 output, Game jitter, Character preview, HUD-less (UI-free frame generation), FFXIV Companion (nameplates), HUD-less diagnostics, Advanced (FFXIV) (a closed fold with `[Hotfix] RenderSizeFromWidthHeight` and `[XeFG] ResetOnUpscalerChange`, both FFXIV only). The cards keep their ids, so the pins of the Custom Tab still work.
- **Draw once.** A group of upstream cards that share one drawing function (the Frame Gen runtime cards, Mipmap Bias with Anisotropic Filtering, the advanced Compatibility cards) is drawn once per tab, at the place of its first card, so a fork card sits before or after such a group, never inside it.
- **Layout table and manifest.** `ext/core/MenuLayout.h` says where each fork card goes (the card it follows, per card id); a card without an entry goes to the end of its tab, and the Custom Tab draws its pinned cards in the same order. `ext/core/MenuManifest.h` lists the FFXIV-only cards, every released card id (retired ids get an alias) and the config keys that stay ini-only on purpose; `tests/ext/menu/menu_manifest.cpp` checks it against the sources offline. Diagnostics (HUD-less diagnostics, the ReShade add-on probe, the Companion diagnostics) are closed by default.

**Status.** Offline tests only; the layout has not been looked at in the game yet. Placement details and the test list: [notes/menu-layout-details.md](notes/menu-layout-details.md).

**Config.** None. **Menu.** The whole menu. **Code and seams.** `ext/core/` (`SourceTracker.h`, `MenuLayout.h`, `MenuManifest.h`, `MenuRegistry.*`). Seams: menu `SourceTracker::Claim` (the tab loop) and `FfxivTabVisible` (one `tabs[]` row).

**Controls for the less common keys.** Every config key the fork binds has a control in the card it belongs to, or is ini-only on purpose (`[HudLess] Diagnostics` and `ReShadeProbe`); offline tests check that. The keys added by the menu cleanup:

| Key | Card (tab) and control | Applies |
|-----|------------------------|---------|
| `[FFXIV] AutomaticReShadeDepth` | ReShade depth compatibility (FFXIV tab, after Window mode): "Automatic scene depth" checkbox and a status line | without a restart, within a few frames |
| `[HudLess] ReShadePresets` | HUD-less: "ReShade presets at the UI boundary" (Auto, On, Off), only used in UI-free mode | Off at once; on again after a start with Off, restart |
| `[DLSSG] UIRecomposition` | HUD-less, closed Advanced fold: "UI recomposition (restart)" (Auto, On, Off) | restart |
| `[XeFG] UnlockMFG`, `MaxInterpolatedFrames` | upstream's XeFG card: "Unlock multi frame generation (restart)", "Max XeFG multiplier (restart)" (2X to 8X) | restart |
| `[DLSSG] AmpereMfgKernelImage`, `Router`, `HardwareBilinear`, `Optimized`, `Preset`, `SpoofArchToGame`, `LogLevel` | Frame generation unlocks, closed "Advanced (RTX 20/30 runtime)" fold drawn only while the runtime is on | restart |
| `[DlssNr] SpatialOffsetX/Y`, `SpatialShiftX/Y` | DLSS Neural Rendering, closed "Advanced" fold at the end, shown while peripheral spatial compression is on; saved in the NR presets | when the slider is released |
| `[DlssNr] ScanExposure`, `AutoCapture`, `VitCapture` | DLSS Neural Rendering, the same Advanced fold | at once, at once, restart |
| `[FFXIV] PreviewNRLingerSeconds`, `PreviewNRLate`, `PreviewGuideCapture`, `[HDR] PreviewNRTrace` | Character preview, closed "Advanced (debug)" fold | at once |

The two FFXIV-only NR options, "Scanned exposure" and "Spread alternating NR across two frames", stay in the NR card, are drawn only on the FFXIV build match and carry "(FFXIV)" in their labels.

## Footprint

**Origin.** Fork-original: the seams architecture (all fork code under `OptiScaler/ext/`, a small listed set of upstream seams) and the footprint script. SirenBrink's tree has no `ext/` folder and no seam document.

Upstream files carry only the seam calls and the small marked edits. `tools/footprint.ps1` counts them (added plus removed lines against the upstream base `fd6c2870`, excluding fork-only paths). It needs only git: the `upstream` remote must be fetched so `upstream/master` exists, and a history that reaches the base commit; no build, no submodules. Measured on the integration branch that adds the four features following SirenBrink's v6.0 and the DLSS-G 310.9.16 preset fix (the earlier published `main` at `c3277532` measured 758 persistent lines):

| Measure | Value |
|---|---|
| Upstream files touched | 71 |
| Lines added / removed | +1278 / -308 (1586) |
| Lines carried by `[UP]` commits (separate, self-contained fixes) | 792 |
| Persistent seam surface (total minus `[UP]`) | 794 |
| Target in [SEAMS.md](SEAMS.md) section 1.3 | 600 (over by 194) |

The overage is known: the seam surface grew with each feature port and with the later HUD-less and composition work, and the earlier figures in [SEAMS-catalog.md](SEAMS-catalog.md) section (d) are historical snapshots. Re-measure with `pwsh -File tools/footprint.ps1` after every sync.
