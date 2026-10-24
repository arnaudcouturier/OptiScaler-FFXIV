# OptiScaler-FFXIV

Unofficial fork of OptiScaler (tracking nightly), not affiliated with the OptiScaler team.

> **Status (October 2026): first official release.** `ffxiv-2026.10.23-d9b62110` (on OptiScaler nightly `d9b62110`) is the current packaged build; it was played in game on 2026-10-23 on one RTX 4090 (SDR, DLSS-G 2x to 4x, recomposition HUD-less, preset Auto and a boot on Preset C, the NR model picker): download the zip from the Releases page. See the table below for what was played and what was not.

An FFXIV-focused build of [OptiScaler](https://github.com/optiscaler/OptiScaler). It bundles DLSS Neural Rendering with pre-SR multipass, DLSS-G multi frame generation unlocks for RTX 30/40, XeFG/XeMFG, OptiHDR, RenoDX scene HDR, a native FFXIV integration (character preview with DLSS, spatial AA and Neural Rendering, window-mode handling, an automatic frame generation multiplier, automatic mipmap bias, game jitter tools) and an FFXIV tab in the OptiScaler menu that is hidden in other games. Much of this was carried over from [SirenBrink's community fork](https://github.com/SirenBrink/OptiScaler_DLSSNR_Multipass_MFG_FFXIV) and the work it builds on; see [Credits](#credits) and [docs/ATTRIBUTION.md](../docs/ATTRIBUTION.md) for who wrote what. All code of this repository lives under `OptiScaler/ext/`, so upstream files carry only small, listed seams (see [docs/SEAMS.md](../docs/SEAMS.md)). The upstream README is [README.md](../README.md).

## Features

| Feature | Status |
|---|---|
| DLSS Neural Rendering with pre-SR multipass | tested (RTX 4090) |
| DLSS-G MFG unlock, RTX 40 | tested (RTX 4090) at 2x to 4x; 5x and 6x are experimental (one GPU hang seen at 6x) |
| OptiHDR (HDR10 output) | tested (RTX 4090) |
| RenoDX scene HDR | tested (RTX 4090) |
| FFXIV native integration (render geometry, native quality, nameplates, companion) | tested (RTX 4090) |
| HUD-less frame generation (`[HudLess] Mode`: `recomposition` lets DLSS-G recompose the UI, `ui_free` generates on the UI-free image and pastes the HUD back; ReShade presets via a built-in add-on) | opt-in, off by default; `recomposition` tested in SDR with DLSS-G 2x to 4x (RTX 4090); `ui_free` is experimental (in one scene its UI mask fell back for most frames); DLSS-G Preset A and C turn NVIDIA's recomposition off, so with them the fork does not request it and says so in the log and the menu; HDR untested; one GPU hang at the experimental 6x MFG |
| NR "Linear light" shader edit, "Linear light + composed" HDR mapping, HDR PNG with menu (the same features are in SirenBrink's v6.0 source) | untested in game (credit: [docs/ATTRIBUTION.md](../docs/ATTRIBUTION.md)) |
| Character preview DLSS, spatial AA and Neural Rendering (CharaView), including the experimental "Run NR before DLSS" option | opt-in, off by default; tested (RTX 4090) on the 2026-10-18/19 builds with one preview window: spatial AA, DLSS 2x, NR, NR before DLSS; not re-checked on this release; several preview windows and the D3D12 debug layer not checked |
| Automatic mipmap bias (`[Mipmap] MipmapBiasAuto`: texture sharpening follows the upscaling ratio and a quality change) | on by default for FFXIV (bind-time mode; a hand-set `MipmapBiasOverride` in an existing ini disables it); ran in game (RTX 4090) on 2026-10-20, 21 and 23; no fps loss measured (comparison across sessions, not a same-session A/B); look not compared |
| Window mode (`[Window] Mode`: border fix after leaving the borderless "fullscreen" and a held-back mode switch while frame generation runs; needs `[XeFG] ForceBorderless` and ReShade) | opt-in, off by default; tested (RTX 4090) on 2026-10-19: border fix and two held-back mode switches with DLSS-G on; the open points listed in [docs/FEATURES.md](../docs/FEATURES.md#window-mode) were not exercised |
| Ember menu style (`[Menu] Style = ember`: charcoal surfaces, hairline borders, rounder corners and more room; the accent colour only marks state instead of tinting every button and the title bar) | opt-in, off by default (`classic` is upstream's look); tested (RTX 4090) |
| Automatic DLSS-G multiplier, 2x to 4x (`[DLSSG] AutoMFG`; a game stall is never held against it) | opt-in, off by default; tested (RTX 4090), 2x to 4x, stall rule seen working; the self-test has not passed in game |
| Game jitter probe and correction (`[FFXIV] JitterProbe`, `JitterCorrection`) | probe tested (ratio 0.600); the correction ran in game and scaled the told jitter (2026-10-20 and 2026-10-23); its earlier version showed no visible gain at the Quality mode (2026-10-19); the look of the current version is not recorded; experimental, off by default |
| Source-level jitter fix (`[FFXIV] JitterSource`: the game draws the full +-0.5 px jitter in NVIDIA's repeating cycle, DLSS is told the drawn offset; falls back to the correction on any mismatch) | experimental, opt-in, off by default; tested (RTX 4090): full lattice, 32 phases at Performance, motion judged less jumpy; supports one game executable build |
| DLSS-G render preset (`[DLSSG] RenderPreset`: Auto, A, B, C; switch while the game runs or set at boot) | opt-in (Auto by default); tested on the 310.9.16 runtime (RTX 4090): boot on Auto and on Preset C; live Auto/B/C switches without a refusal (B on 2026-10-22, C on the released build); Preset A seen only on an earlier build; Preset A and C turn NVIDIA's UI recomposition off (Preset C also turns FG Lite on) |
| NR model version picker (`OptiScaler\NR.<version>` folders, `[DlssNr] Runtime`, switch in the Neural Rendering card) | new; unit-tested; tested (RTX 4090) on 2026-10-23: live switch and reload from an `NR.<version>` folder work; `NR.310.8.0.0` cannot create its feature on this GPU (NR skips itself, switching back recovers); a switch between two different working models was not seen |
| Quality change without the 500 ms pause (`[NgxRelease] SkipReleaseSleep`) | on by default; ran in game (RTX 4090) since 2026-10-20: the sleep was skipped and 0.06 to 30 ms were waited instead of 500 ms |
| Native-quality hook install retry, frame pacing trace (`[FramePacing] Trace`, off by default), preview pause when the window loses focus | the retry: the install succeeded on its first attempt in every game log since 2026-10-22; the retry path itself (a failed first attempt) has never been seen in game. The trace and the preview pause are unit-tested and untested in game |
| NR ViT capture (`[DlssNr] VitCapture`) | diagnostic only, off by default |
| AMD HIP pre-SR | untested / community |
| XeFG / XeMFG | untested / community |
| DMFG on RTX 50 | untested / community |
| Ampere (RTX 30) MFG loader | untested / community |
| Vulkan NR path | untested / community |

"tested" means played in game by the author on the stated GPU. Everything else compiles and follows the same code paths, but nobody has verified it on real hardware.

## Install

Download `OptiScaler-FFXIV-<version>.zip` from the [Releases](../../releases) page (tags are named `ffxiv-<yyyy.mm.dd>-<upstream sha8>[-rcN]`), or build one yourself ([docs/RELEASE.md](../docs/RELEASE.md)). The archive contains its own `INSTALL.txt` with the same steps.

1. **Back up first.** Close the game and the launcher. In the folder that contains `ffxiv_dx11.exe`, copy `winmm.dll`, `nvngx.dll_dlssnr.dll`, `OptiScaler.ini` and the `OptiScaler` folder to a new folder, for those that exist.
2. **Copy the files.** Extract the archive into that folder and overwrite. The archive holds `winmm.dll` (the fork's DLL, installed under the name the game loads), the NR forwarder `nvngx.dll_dlssnr.dll` (keep the exact name), `OptiScaler.ini`, the `OptiScaler` folder with third-party libraries, a `Licenses` folder, `NOTICE.txt` and `SHA256SUMS.txt`. Optional: compare hashes with `SHA256SUMS.txt`.
3. **Know the defaults.** Every value in the shipped `OptiScaler.ini` is `auto`, which means the built-in default: Neural Rendering, frame generation, HDR output, HUD-less, the DLSS-G unlock, window mode, the automatic multiplier and the jitter correction and the source-level jitter fix are all off, and the game's own quality choice is left alone. Two behaviours are on by default in FFXIV: the automatic mipmap bias (`[Mipmap] MipmapBiasAuto`) and the quality change without the 500 ms pause (`[NgxRelease] SkipReleaseSleep`); set them to `false` to turn them off. The in-game menu opens with Insert; in FFXIV it has an FFXIV tab (window mode, HDR, game jitter, character preview, HUD-less, companion, advanced) that other games do not show. The menu keeps upstream's look unless you set `[Menu] Style=ember` (with your own `AccentColorR/G/B`). Set `[Log] LogToFile=true` to get `OptiScaler.log` for bug reports.
4. **Bring your own NVIDIA files.** They are not redistributable from this repository and are not in the archive: `nvngx_dlss.dll` (the game normally has one), `nvngx_dlssnr.dll` for Neural Rendering (beside OptiScaler as before, or one folder per version, `OptiScaler\NR.<version>\nvngx_dlssnr.dll`, to switch between versions in the menu), `nvngx_dlssg.dll` plus the Streamline plugins in `OptiScaler\streamline\` for DLSS-G frame generation, and for the experimental RTX 30 path a community companion runtime in `OptiScaler\dlssg_sm86\`. Get them from NVIDIA's DLSS and Streamline distributions; `INSTALL.txt` lists the exact names. HUD-less with ReShade presets needs your own ReShade 6.8.0.
5. **Turn features on** in the menu or in `OptiScaler.ini` (see [docs/FEATURES.md](../docs/FEATURES.md) for every key). The set used in the author's tested setup, for DLSS on this DirectX 11 game: `[Upscalers] Dx11Upscaler=dlss_12`; Neural Rendering `[DlssNr] Enabled=true`; frame generation `[FrameGen] Enabled=true`, `FGInput=upscaler`, `FGOutput=dlssg`; HDR10 output `[HDR] FfxivHDR=true` with Windows HDR on (restart; `[HDR] Mode=1` adds RenoDX scene HDR); HUD-less `[HudLess] Mode=ui_free` (opt-in, needs DLSS-G output).
6. **Known limits.** Only the features marked tested above were played, on one RTX 4090; everything else is untested. Window mode needs `[XeFG] ForceBorderless=true` (the borderless "fullscreen" of FFXIV with DLSS-G) and ReShade. HUD-less is opt-in and untested in HDR. The game jitter correction ran in game and scaled the told jitter (2026-10-20 and 2026-10-23); its earlier version showed no visible gain at the Quality mode (2026-10-19) in the author's test, the look of the current version is not recorded, and it stays experimental. Do not combine `[HudLess] Mode=recomposition` with DLSS-G Preset A or C: NVIDIA turns its UI recomposition off with them, so the fork does not request it (use Auto or B with `recomposition`, or `ui_free`, which is experimental). DLSS presets L and M cost more fps than K (they are heavier models); K stays the default. 5x and 6x multi frame generation on RTX 40 are experimental and one GPU hang happened at 6x: stay at 4x or lower. FFXIV-specific features follow one game build; after a game patch they switch themselves off until the fork is updated ([docs/FFXIV-PATCH-RUNBOOK.md](../docs/FFXIV-PATCH-RUNBOOK.md)).
7. **Uninstall.** Close the game, delete the files and folders listed in step 2 (and `Licenses`, `NOTICE.txt`, `SHA256SUMS.txt`, `INSTALL.txt`), and copy your backup back.

## Credits

Most of the feature code in this repository was written by other people. The full table, by area and with commits, is in [docs/ATTRIBUTION.md](../docs/ATTRIBUTION.md).

- The [OptiScaler](https://github.com/optiscaler/OptiScaler) team, for OptiScaler itself.
- [SirenBrink](https://github.com/SirenBrink/OptiScaler_DLSSNR_Multipass_MFG_FFXIV), whose community fork this repository carries over in large part: the FFXIV integration, OptiHDR and scene HDR, HDR PNG capture, XeFG/XeMFG, and frame markers and pacing (as they stood at the import base `36c53a15`), plus the NR linear-light mode, the HDR PNG menu option and the character preview (v6.0, `e17a7362`). The DLSS-G render preset, the NR model picker, the bind-time mip bias and the source-level jitter fix follow the design of his v6.0 source, with code written for this repository.
- Dagherbou (DLSS Neural Rendering base), y4my4my4m (multipass, MFG unlock), wilsjo2 (pre-SR, native path, MFG unlock hardening), byunjoe (HUD-less frame generation).
- janblade, Citrus333, Ainquisition, MatheusGViana, ShyVortex, BeliyG3, stegalloway, mavismmg and dashdogy, whose work SirenBrink's fork adapts; see the attribution table.
- [RenoDX](https://github.com/clshortfuse/renodx) (clshortfuse), the HDR shaders behind scene HDR; its notice ships with the code that uses it.

## License

GPL-3.0, the same as upstream. See [LICENSE](../LICENSE).
