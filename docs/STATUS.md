# What works

What has been played in game and what has not, for every feature of this fork. The short version is in the [README](../.github/README.md); each feature, its origin and its settings are described in [FEATURES.md](FEATURES.md); installation is in [INSTALL.md](INSTALL.md).

## Release status

> **Status (October 2026): v0.2.0.** `ffxiv-2026.10.25-d9b62110` (the tag of v0.2.0, on OptiScaler nightly `d9b62110`) adds the automatic ReShade scene depth (depth and the game's normals, decals and motion vectors lined up with the picture when the game renders below the output size); it was played in game on 2026-10-25 on one RTX 4090 (SDR, one ReShade preset, DLAA, Quality and Performance). The first release was v0.1.0, tag `ffxiv-2026.10.23-d9b62110` (first official release, played in game on 2026-10-23 on one RTX 4090: SDR, DLSS-G 2x to 4x, recomposition HUD-less, preset Auto and a boot on Preset C, the NR model picker). Download the zips from the Releases page. See the table below for what was played and what was not.

## Features

| Feature | Status |
|---|---|
| DLSS Neural Rendering with pre-SR multipass | tested (RTX 4090) |
| DLSS-G MFG unlock, RTX 40 | tested (RTX 4090) at 2x to 4x; 5x and 6x are experimental (one GPU hang seen at 6x) |
| OptiHDR (HDR10 output) | tested (RTX 4090) |
| RenoDX scene HDR | tested (RTX 4090) |
| FFXIV native integration (render geometry, native quality, nameplates, companion) | tested (RTX 4090) |
| HUD-less frame generation (`[HudLess] Mode`: `recomposition` lets DLSS-G recompose the UI, `ui_free` generates on the UI-free image and pastes the HUD back; ReShade presets via a built-in add-on) | opt-in, off by default; `recomposition` tested in SDR with DLSS-G 2x to 4x (RTX 4090); `ui_free` is experimental (in one scene its UI mask fell back for most frames); DLSS-G Preset A and C turn NVIDIA's recomposition off, so with them the fork does not request it and says so in the log and the menu; HDR untested; one GPU hang at the experimental 6x MFG |
| Automatic ReShade scene depth (`[FFXIV] AutomaticReShadeDepth`, one checkbox in the FFXIV tab: under upscaling ReShade's depth, and the normals, decals, motion vectors and material that the Reshade Effect Shader Toggler add-on binds, are lined up with the picture automatically, so ambient occlusion such as MXAO sits on the geometry; needs ReShade 6.8.0) | opt-in, off by default; tested (RTX 4090) on 2026-10-25 with one preset (Alive NEXT Stable): the realignment ran at DLAA, Performance and Quality with no failed pass; no AO trails seen while panning; other presets, other GPUs and HDR not tried; a preset that sets `RESHADE_DEPTH_INPUT_*` scales or offsets by hand switches the feature off for itself |
| NR "Linear light" shader edit, "Linear light + composed" HDR mapping, HDR PNG with menu (the same features are in SirenBrink's v6.0 source) | untested in game (credit: [ATTRIBUTION.md](ATTRIBUTION.md)) |
| Character preview DLSS, spatial AA and Neural Rendering (CharaView), including the experimental "Run NR before DLSS" option | opt-in, off by default; tested (RTX 4090) on the 2026-10-18/19 builds with one preview window: spatial AA, DLSS 2x, NR, NR before DLSS; not re-checked on this release; several preview windows and the D3D12 debug layer not checked |
| Automatic mipmap bias (`[Mipmap] MipmapBiasAuto`: texture sharpening follows the upscaling ratio and a quality change) | on by default for FFXIV (bind-time mode; a hand-set `MipmapBiasOverride` in an existing ini disables it); ran in game (RTX 4090) on 2026-10-20, 21 and 23; no fps loss measured (comparison across sessions, not a same-session A/B); look not compared |
| Window mode (`[Window] Mode`: border fix after leaving the borderless "fullscreen" and a held-back mode switch while frame generation runs; needs `[XeFG] ForceBorderless` and ReShade) | opt-in, off by default; tested (RTX 4090) on 2026-10-19: border fix and two held-back mode switches with DLSS-G on; the open points listed in [FEATURES.md](FEATURES.md#window-mode) were not exercised |
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
