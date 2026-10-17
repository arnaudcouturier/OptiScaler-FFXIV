# OptiScaler-FFXIV

Unofficial fork of OptiScaler (tracking nightly), not affiliated with the OptiScaler team.

> **Status (October 2026): release candidate.** The feature stack is ported onto OptiScaler nightly `fd6c2870`. Tag `ffxiv-2026.10.17-fd6c2870-rc1` is the first tagged build that launches and plays; there are no packaged binaries yet. The older tag `ffxiv-ref-equiv-fd6c2870` does not launch (two startup bugs fixed after it) and is superseded by the rc tag.

An FFXIV-focused build of [OptiScaler](https://github.com/optiscaler/OptiScaler). It adds DLSS Neural Rendering with pre-SR multipass, DLSS-G multi frame generation unlocks for RTX 30/40, XeFG/XeMFG, OptiHDR, RenoDX scene HDR and a native FFXIV integration. All fork code lives under `OptiScaler/ext/`, so upstream files carry only small, listed seams (see [docs/SEAMS.md](../docs/SEAMS.md)). The upstream README is [README.md](../README.md).

## Features

| Feature | Status |
|---|---|
| DLSS Neural Rendering with pre-SR multipass | tested (RTX 4090) |
| DLSS-G MFG unlock, RTX 40 | tested (RTX 4090) |
| OptiHDR (HDR10 output) | tested (RTX 4090) |
| RenoDX scene HDR | tested (RTX 4090) |
| FFXIV native integration (render geometry, native quality, nameplates, companion) | tested (RTX 4090) |
| HUD-less frame generation (UI-free image, HUD pasted back; ReShade presets via a built-in add-on) | opt-in, off by default; tested in SDR with DLSS-G 2x-4x (RTX 4090), no visible gain reported; HDR untested; one GPU hang at the experimental 6x MFG |
| Recovered from Nickle's build: NR shader edit, "Linear light + composed" HDR mapping, HDR PNG with menu | untested in game (credit: [docs/ATTRIBUTION.md](../docs/ATTRIBUTION.md)) |
| NR ViT capture (`[DlssNr] VitCapture`) | diagnostic only, off by default |
| AMD HIP pre-SR | untested / community |
| XeFG / XeMFG | untested / community |
| DMFG on RTX 50 | untested / community |
| Ampere (RTX 30) MFG loader | untested / community |
| Vulkan NR path | untested / community |

"tested" means played in game by the maintainer on the stated GPU. Everything else compiles and follows the same code paths, but nobody has verified it on real hardware.

## Install

To be written with the first release.

## Credits

- The [OptiScaler](https://github.com/optiscaler/OptiScaler) team, for OptiScaler itself.
- Dagherbou, DLSS Neural Rendering base.
- wilsjo2, reimplementation of multipass, pre-SR and the MFG unlock.
- SirenBrink, FFXIV integration.
- byunjoe and Nickle, contributions to the fork.
- [RenoDX](https://github.com/clshortfuse/renodx) (clshortfuse), the HDR shaders behind scene HDR; its notice ships with the code that uses it.

## License

GPL-3.0, the same as upstream. See [LICENSE](../LICENSE).
