# OptiScaler-FFXIV

Unofficial fork of OptiScaler (tracking nightly), not affiliated with the OptiScaler team.

An FFXIV-focused build of [OptiScaler](https://github.com/optiscaler/OptiScaler). It adds DLSS Neural Rendering with pre-SR multipass, DLSS-G multi frame generation unlocks for RTX 30/40, XeFG/XeMFG, OptiHDR, RenoDX scene HDR and a native FFXIV integration. All fork code lives under `OptiScaler/ext/`, so upstream files carry only small, listed seams (see [docs/SEAMS.md](../docs/SEAMS.md)). The upstream README is [README.md](../README.md).

## Features

| Feature | Status |
|---|---|
| DLSS Neural Rendering with pre-SR multipass | tested (RTX 4090) |
| DLSS-G MFG unlock, RTX 40 | tested (RTX 4090) |
| OptiHDR (HDR10 output) | tested (RTX 4090) |
| RenoDX scene HDR | tested (RTX 4090) |
| FFXIV native integration (render geometry, native quality, nameplates, companion) | tested (RTX 4090) |
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
