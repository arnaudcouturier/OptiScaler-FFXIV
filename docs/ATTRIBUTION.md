# Attribution

OptiScaler-FFXIV is licensed GPL-3.0, like OptiScaler. This file says whose work it contains. Licences of vendored libraries stay next to their code (`external/`, `OptiScaler/ext/hdr/RenoDX_ATTRIBUTION.txt`, `OptiScaler/ext/hdr/shaders/renodx/NOTICE.md`, `OptiScaler/ext/reshade/ReShade_ATTRIBUTION.txt`).

## Where the code comes from

A large part of the feature code under `OptiScaler/ext/` was carried over from SirenBrink's community fork, [`SirenBrink/OptiScaler_DLSSNR_Multipass_MFG_FFXIV`](https://github.com/SirenBrink/OptiScaler_DLSSNR_Multipass_MFG_FFXIV) (GPL-3.0), as it stood at commit `36c53a15` (2026-10-09, called "the import base" in the docs). That fork in turn builds on work by Dagherbou, y4my4my4m and wilsjo2. The code was moved under `OptiScaler/ext/` behind a seam layer, so most upstream files stay untouched (see [SEAMS.md](SEAMS.md)). Licence files that came with moved code stay next to it, and many moved files say in their headers that they were moved.

A few features follow code SirenBrink published later, in v6.0 (commit `e17a7362`, 2026-10-20). The table says which.

## By area

| Area | Source | Who | What this fork did |
|---|---|---|---|
| OptiScaler | upstream [optiscaler/OptiScaler](https://github.com/optiscaler/OptiScaler) | cdozdil, FakeMichau and the OptiScaler contributors | tracks the nightly; carries small listed seams |
| DLSS Neural Rendering: core pass, colour pipeline, exposure scan, presets, compare views | import base | Dagherbou (base), wilsjo2, SirenBrink | moved to `ext/nr/` with a config binder and seams |
| NR multipass, pre-SR, split frame work, native path, hybrid precision, Vulkan path | import base | y4my4my4m (multipass), wilsjo2 (pre-SR, native path, hybrid, Vulkan parity), SirenBrink | moved; added `VitCapture` and the verified-module table |
| NR pass feedback, bottleneck reuse (`VitEvery`), NR direct-linear input | import base | SirenBrink, adapting janblade's work | moved |
| AMD HIP pre-SR | import base | SirenBrink, adapting Ainquisition's `amd-v084-minimal` and MatheusGViana's AMD project | moved |
| Spatial compression, ResidualFG | import base | SirenBrink and wilsjo2; `PeripheralWarp` by BeliyG3 and ShyVortex (MIT licence kept next to the code) | moved |
| NR Foveated spatial profile (`[DlssNr] SpatialProfile = 1`) | design follows maximedgoulet/FoveatedDLSS5 (MIT, copyright Carlos Lopez Jr. 2025, from RenoDX; its foveated grid is that repository's own work) | maximedgoulet | the maths (centre zone, smoothstep falloff, edge plateau, packed distance and its inverse) is written independently here from the described design, no source was copied; the Peripheral profile stays credited to `PeripheralWarp` by BeliyG3 as above |
| NR model version picker | follows SirenBrink's v6.0 (`5bfa6e92`, `50ca6733`) | SirenBrink | same `NR.<version>` folders, `[DlssNr] Runtime` key, selection with a revision, three search roots and switch order; this fork's catalog, scan, card and rebuild code is separate |
| HDR10 output (OptiHDR), FG HDR, HDR menu layer, HDR PNG capture | import base | SirenBrink | moved behind seams |
| RenoDX scene HDR and its NR/DLSS scene input | import base | RenoDX by clshortfuse (Carlos Lopez Jr., MIT); the FFXIV shaders are by Ersh and Pumbo; adapted by SirenBrink | moved; notices in `OptiScaler/ext/hdr/shaders/renodx/` |
| NR colour composition | RenoDX | clshortfuse (MIT) | see `OptiScaler/ext/hdr/RenoDX_ATTRIBUTION.txt` |
| NR "Linear light" proxy and "Linear light + composed" mapping, HDR PNG option "Include OptiScaler menu", character preview (CharaView) | SirenBrink's v6.0 (`fe208a59`, 2026-10-16) | SirenBrink; the NR direct-linear idea cites janblade | see below |
| HUD-less frame generation: mask, paste, UI image, defaults, `[DLSSG] UIRecomposition` | import base | byunjoe wrote it (nine commits, 2026-10-02/03); SirenBrink merged it and added the slot ring | this fork added native capture at the first FFXIV UI draw, an in-process ReShade add-on, a state machine with coverage guard and fenced slots |
| FFXIV integration: native quality hooks, `ForcePerfQuality`, render size, colour scaling, nameplates (companion), composition swapchain hook, lighting scan, frame markers | import base | SirenBrink; the composition swapchain hook was started by janblade and wilsjo2 | moved with the same offsets and guard bytes; added an executable identity check (`TimeDateStamp` and `SizeOfImage`) in front of them |
| DLSS-G multi frame generation unlock (RTX 40) | import base | y4my4my4m wrote the original; wilsjo2 hardened it; SirenBrink reworked it; `AdaTemporal` adapts mavismmg's MFGAdaUnlock-RenoDx (MIT) and dashdogy's technique | moved |
| RTX 30 MFG loader | import base | SirenBrink, adapting ShyVortex | moved |
| Frame interval, guide matching, ordered swapchain submission, GPU lifetime and the `[UP]` Reflex and Streamline fixes | import base | SirenBrink; GPU lifetime tracking adapts stegalloway's work | moved |
| XeFG / XeMFG: unlock, pacing, dynamic multiplier, camera, HDR | import base | Citrus333 (unlock, pacing base), SirenBrink (dynamic multiplier, camera, pacing rework, HDR) | moved; glue and context rebuild are this fork's |
| DLSS-G render preset (`[DLSSG] RenderPreset`), live switching | follows SirenBrink's v6.0 (`5bfa6e92`, `50ca6733`) | SirenBrink | same setting id, hook points and byte anchors; the guards, instruction decoder and switch logic are this fork's |
| Bind-time mipmap bias, quality change without the 500 ms pause, hook-install retry, frame pacing trace, hidden-present `DO_NOT_WAIT`, NR pause on focus loss | follow SirenBrink's v6.0 | SirenBrink | same approach; this fork's code is separate except the frame pacing trace line format and counters |
| Source-level jitter fix (`[FFXIV] JitterSource`) | follows SirenBrink's `FfxivJitter` hooks (`5bfa6e92`) | SirenBrink | same hook points, strength range, phase-count rule, ring criteria and most guard blocks; the controller, verification, fall-back and install are this fork's |
| Automatic DLSS-G multiplier (`[DLSSG] AutoMFG`) | selector rules from SirenBrink's XeFG selector (`XeFGDynamic.h`) | SirenBrink | controller, kill-switch, stall rule and self-test are this fork's |
| Window mode, Ember menu style, FFXIV tab and card registry, create-time automatic mipmap bias, game jitter probe and learned correction, packaging and release tooling, patch runbook | this fork | arnaudcouturier | no counterpart in SirenBrink's tree |

## Linear light, HDR PNG menu option and character preview

SirenBrink's v6.0 source contains these features. This fork has them with the same ini keys and menu texts. The NR linear-light shader branch is identical to his, and the compiled shader has the same SHA-256 (`b9cbd405...aab8`). The "Linear light + composed" menu texts and the "Include OptiScaler menu" tooltip match word for word, and the CharaView anti-aliasing and guide shaders use the same constants and registers. The CharaView C++ is a separate implementation, and the fork's preview Neural Rendering works differently from his (NR on the 2x output, on the native image alone, or before DLSS).

The fork first derived these features from an unpublished community build, a `winmm.dll` whose version string is `36c53a15-community-preview-depth-jitter-20261013`. The notes on that analysis are in [RECONSTRUCTION-REFERENCE.md](RECONSTRUCTION-REFERENCE.md). Git commit times cannot settle which side wrote a feature first, so treat SirenBrink's repository as the primary source for these three features.

## People and projects

Dagherbou, y4my4my4m, wilsjo2, byunjoe, SirenBrink, janblade, Citrus333, Ainquisition, MatheusGViana, ShyVortex, BeliyG3, maximedgoulet (FoveatedDLSS5), stegalloway, mavismmg, dashdogy, clshortfuse (RenoDX), Ersh and Pumbo (RenoDX FFXIV mod), the OptiScaler team. If your work is here and credited wrongly or not at all, open an issue and it will be fixed.

Thank you.
