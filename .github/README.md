# OptiScaler-FFXIV

Unofficial fork of OptiScaler (tracking nightly), not affiliated with the OptiScaler team.

> **Status (October 2026): release candidate.** `ffxiv-2026.10.23-d9b62110-rc1` (on OptiScaler nightly `d9b62110`) is the current packaged build (a pre-release until the final build has been played in game): download the zip from the Releases page. On top of `ffxiv-2026.10.20-d9b62110` it adds the DLSS-G render preset picker (A, B, C) with one rule that keeps NVIDIA's UI recomposition consistent with the preset, the NR model version picker, the bind-time mip bias that follows quality changes, a quality change without the 500 ms pause, a retry for the native-quality hook install, a frame pacing trace, an opt-in source-level jitter fix and a pause of the character preview when the window loses focus (see the table for what was played). The earlier tags `ffxiv-2026.10.17-fd6c2870-rc1` and `ffxiv-ref-equiv-fd6c2870`, the pre-release `ffxiv-2026.10.18-d9b62110` and the releases `ffxiv-2026.10.19-d9b62110` and `ffxiv-2026.10.20-d9b62110` are superseded.

An FFXIV-focused build of [OptiScaler](https://github.com/optiscaler/OptiScaler). It adds DLSS Neural Rendering with pre-SR multipass, DLSS-G multi frame generation unlocks for RTX 30/40, XeFG/XeMFG, OptiHDR, RenoDX scene HDR, a native FFXIV integration (character preview with DLSS, spatial AA and Neural Rendering, window-mode handling, an automatic frame generation multiplier, automatic mipmap bias, game jitter tools) and an FFXIV tab in the OptiScaler menu that is hidden in other games. All fork code lives under `OptiScaler/ext/`, so upstream files carry only small, listed seams (see [docs/SEAMS.md](../docs/SEAMS.md)). The upstream README is [README.md](../README.md).

## Features

| Feature | Status |
|---|---|
| DLSS Neural Rendering with pre-SR multipass | tested (RTX 4090) |
| DLSS-G MFG unlock, RTX 40 | tested (RTX 4090) at 2x to 4x; 5x and 6x are experimental (one GPU hang seen at 6x) |
| OptiHDR (HDR10 output) | tested (RTX 4090) |
| RenoDX scene HDR | tested (RTX 4090) |
| FFXIV native integration (render geometry, native quality, nameplates, companion) | tested (RTX 4090) |
| HUD-less frame generation (`[HudLess] Mode`: `recomposition` lets DLSS-G recompose the UI, `ui_free` generates on the UI-free image and pastes the HUD back; ReShade presets via a built-in add-on) | opt-in, off by default; `recomposition` tested in SDR with DLSS-G 2x to 4x (RTX 4090); `ui_free` is experimental (in one scene its UI mask fell back for most frames); DLSS-G Preset A and C turn NVIDIA's recomposition off, so with them the fork does not request it and says so in the log and the menu; HDR untested; one GPU hang at the experimental 6x MFG |
| Recovered from Nickle's build: NR shader edit, "Linear light + composed" HDR mapping, HDR PNG with menu | untested in game (credit: [docs/ATTRIBUTION.md](../docs/ATTRIBUTION.md)) |
| Character preview DLSS, spatial AA and Neural Rendering (CharaView), including the experimental "Run NR before DLSS" option | opt-in, off by default; tested (RTX 4090) |
| Automatic mipmap bias (`[Mipmap] MipmapBiasAuto`: texture sharpening follows the upscaling ratio and a quality change) | on by default for FFXIV (bind-time mode; a hand-set `MipmapBiasOverride` in an existing ini disables it); ran in game (RTX 4090) at no measurable fps cost, the visual effect was not compared |
| Window mode (`[Window] Mode`: border fix after leaving the borderless "fullscreen" and a held-back mode switch while frame generation runs; needs `[XeFG] ForceBorderless` and ReShade) | opt-in, off by default; tested (RTX 4090) |
| Ember menu style (`[Menu] Style = ember`: charcoal surfaces, hairline borders, rounder corners and more room; the accent colour only marks state instead of tinting every button and the title bar) | opt-in, off by default (`classic` is upstream's look); tested (RTX 4090) |
| Automatic DLSS-G multiplier, 2x to 4x (`[DLSSG] AutoMFG`; a game stall is never held against it) | opt-in, off by default; tested (RTX 4090) |
| Game jitter probe and correction (`[FFXIV] JitterProbe`, `JitterCorrection`) | probe tested; the correction learned the game's 0.6 ratio and scaled what DLSS is told (experimental, off by default) |
| Source-level jitter fix (`[FFXIV] JitterSource`: the game draws the full +-0.5 px jitter in NVIDIA's repeating cycle, DLSS is told the drawn offset; falls back to the correction on any mismatch) | experimental, opt-in, off by default; tested (RTX 4090): full lattice, 32 phases at Performance, motion judged less jumpy; supports one game executable build |
| DLSS-G render preset (`[DLSSG] RenderPreset`: Auto, A, B, C; switch while the game runs or set at boot) | opt-in (Auto by default); tested on the 310.9.16 runtime (RTX 4090): boot on Preset C keeps the patched runtime, live Auto/B/C switches ran without a refusal; Preset A and C turn NVIDIA's UI recomposition off (Preset C also turns FG Lite on) |
| NR model version picker (`OptiScaler\NR.<version>` folders, `[DlssNr] Runtime`, switch in the Neural Rendering card) | new; unit-tested; untested in game |
| Quality change without the 500 ms pause (`[NgxRelease] SkipReleaseSleep`) | on by default; ran in game (RTX 4090) |
| Native-quality hook install retry, frame pacing trace (`[FramePacing] Trace`, off by default), preview pause when the window loses focus | the retry installed on its first attempt in game; the trace and the preview pause are unit-tested and untested in game |
| NR ViT capture (`[DlssNr] VitCapture`) | diagnostic only, off by default |
| AMD HIP pre-SR | untested / community |
| XeFG / XeMFG | untested / community |
| DMFG on RTX 50 | untested / community |
| Ampere (RTX 30) MFG loader | untested / community |
| Vulkan NR path | untested / community |

"tested" means played in game by the maintainer on the stated GPU. Everything else compiles and follows the same code paths, but nobody has verified it on real hardware.

## Install

Download `OptiScaler-FFXIV-<version>.zip` from the [Releases](../../releases) page (tags are named `ffxiv-<yyyy.mm.dd>-<upstream sha8>[-rcN]`), or build one yourself ([docs/RELEASE.md](../docs/RELEASE.md)). The archive contains its own `INSTALL.txt` with the same steps.

1. **Back up first.** Close the game and the launcher. In the folder that contains `ffxiv_dx11.exe`, copy `winmm.dll`, `nvngx.dll_dlssnr.dll`, `OptiScaler.ini` and the `OptiScaler` folder to a new folder, for those that exist.
2. **Copy the files.** Extract the archive into that folder and overwrite. The archive holds `winmm.dll` (the fork's DLL, installed under the name the game loads), the NR forwarder `nvngx.dll_dlssnr.dll` (keep the exact name), `OptiScaler.ini`, the `OptiScaler` folder with third-party libraries, a `Licenses` folder, `NOTICE.txt` and `SHA256SUMS.txt`. Optional: compare hashes with `SHA256SUMS.txt`.
3. **Know the defaults.** Every value in the shipped `OptiScaler.ini` is `auto`, which means the built-in default: Neural Rendering, frame generation, HDR output, HUD-less, the DLSS-G unlock, window mode, the automatic multiplier and the jitter correction and the source-level jitter fix are all off, and the game's own quality choice is left alone. Two behaviours are on by default in FFXIV: the automatic mipmap bias (`[Mipmap] MipmapBiasAuto`) and the quality change without the 500 ms pause (`[NgxRelease] SkipReleaseSleep`); set them to `false` to turn them off. The in-game menu opens with Insert; in FFXIV it has an FFXIV tab (window mode, HDR, game jitter, character preview, HUD-less, companion, advanced) that other games do not show. The menu keeps upstream's look unless you set `[Menu] Style=ember` (with your own `AccentColorR/G/B`). Set `[Log] LogToFile=true` to get `OptiScaler.log` for bug reports.
4. **Bring your own NVIDIA files.** They are not redistributable from this repository and are not in the archive: `nvngx_dlss.dll` (the game normally has one), `nvngx_dlssnr.dll` for Neural Rendering (beside OptiScaler as before, or one folder per version, `OptiScaler\NR.<version>\nvngx_dlssnr.dll`, to switch between versions in the menu), `nvngx_dlssg.dll` plus the Streamline plugins in `OptiScaler\streamline\` for DLSS-G frame generation, and for the experimental RTX 30 path a community companion runtime in `OptiScaler\dlssg_sm86\`. Get them from NVIDIA's DLSS and Streamline distributions; `INSTALL.txt` lists the exact names. HUD-less with ReShade presets needs your own ReShade 6.8.0.
5. **Turn features on** in the menu or in `OptiScaler.ini` (see [docs/FEATURES.md](../docs/FEATURES.md) for every key). The set used in the maintainer's tested setup, for DLSS on this DirectX 11 game: `[Upscalers] Dx11Upscaler=dlss_12`; Neural Rendering `[DlssNr] Enabled=true`; frame generation `[FrameGen] Enabled=true`, `FGInput=upscaler`, `FGOutput=dlssg`; HDR10 output `[HDR] FfxivHDR=true` with Windows HDR on (restart; `[HDR] Mode=1` adds RenoDX scene HDR); HUD-less `[HudLess] Mode=ui_free` (opt-in, needs DLSS-G output).
6. **Known limits.** Only the features marked tested above were played, on one RTX 4090; everything else is untested. Window mode needs `[XeFG] ForceBorderless=true` (the borderless "fullscreen" of FFXIV with DLSS-G) and ReShade. HUD-less is opt-in and untested in HDR. The game jitter correction made no visible difference in the maintainer's test at the Quality mode and stays experimental. Do not combine `[HudLess] Mode=recomposition` with DLSS-G Preset A or C: NVIDIA turns its UI recomposition off with them, so the fork does not request it (use Auto or B with `recomposition`, or `ui_free`, which is experimental). DLSS presets L and M cost more fps than K (they are heavier models); K stays the default. 5x and 6x multi frame generation on RTX 40 are experimental and one GPU hang happened at 6x: stay at 4x or lower. FFXIV-specific features follow one game build; after a game patch they switch themselves off until the fork is updated ([docs/FFXIV-PATCH-RUNBOOK.md](../docs/FFXIV-PATCH-RUNBOOK.md)).
7. **Uninstall.** Close the game, delete the files and folders listed in step 2 (and `Licenses`, `NOTICE.txt`, `SHA256SUMS.txt`, `INSTALL.txt`), and copy your backup back.

## Credits

- The [OptiScaler](https://github.com/optiscaler/OptiScaler) team, for OptiScaler itself.
- Dagherbou, DLSS Neural Rendering base.
- wilsjo2, reimplementation of multipass, pre-SR and the MFG unlock.
- SirenBrink, FFXIV integration.
- byunjoe and Nickle, contributions to the fork.
- [RenoDX](https://github.com/clshortfuse/renodx) (clshortfuse), the HDR shaders behind scene HDR; its notice ships with the code that uses it.

## License

GPL-3.0, the same as upstream. See [LICENSE](../LICENSE).
