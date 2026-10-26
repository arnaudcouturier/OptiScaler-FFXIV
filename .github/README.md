<div align="center">

<img src="assets/banner.svg" alt="OptiScaler-FFXIV: DLSS upscaling, frame generation and HDR for Final Fantasy XIV on DirectX 11" width="860">

[![Release](https://img.shields.io/github/v/release/arnaudcouturier/OptiScaler-FFXIV?display_name=release&style=flat-square)](https://github.com/arnaudcouturier/OptiScaler-FFXIV/releases)
[![Downloads](https://img.shields.io/github/downloads/arnaudcouturier/OptiScaler-FFXIV/total?style=flat-square)](https://github.com/arnaudcouturier/OptiScaler-FFXIV/releases)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue?style=flat-square)](../LICENSE)
[![Game](https://img.shields.io/badge/FFXIV-DirectX%2011-5b6b7c?style=flat-square)](#requirements)
[![GPU](https://img.shields.io/badge/GPU-NVIDIA%20RTX-5b6b7c?style=flat-square)](#requirements)
[![Built on OptiScaler](https://img.shields.io/badge/built%20on-OptiScaler-5b6b7c?style=flat-square)](https://github.com/optiscaler/OptiScaler)

</div>

## What it is

OptiScaler-FFXIV is a fork of [OptiScaler](https://github.com/optiscaler/OptiScaler) for Final Fantasy XIV on DirectX 11. It provides DLSS upscaling, DLSS frame generation and HDR10 output for the game, plus features written for FFXIV itself, and puts the settings in an in-game menu that opens with the Insert key. A large part of the feature code was carried over from [SirenBrink's community fork](https://github.com/SirenBrink/OptiScaler_DLSSNR_Multipass_MFG_FFXIV) (see [Credits](#credits)).

## Highlights

| Feature | What it does | Status |
|---|---|---|
| DLSS upscaling | The game renders below your screen resolution and DLSS upscales it, or DLAA runs at full resolution | tested |
| DLSS Neural Rendering | Adds detail to the upscaled image with NVIDIA's Neural Rendering model | tested |
| Frame generation | DLSS multi frame generation at 2x to 4x on RTX 40 cards | tested; 5x and 6x are experimental |
| HDR10 output | HDR10 for the game, with RenoDX scene HDR as an option | tested |
| ReShade depth alignment | Keeps ReShade's depth lined up with the picture when the game renders below the output size, so ambient occlusion such as MXAO sits on the geometry | optional, tested with one preset |
| HUD-less frame generation | Handles the interface separately from the game picture during frame generation | optional, tested in SDR; one of its two modes is experimental |
| Character preview | DLSS, spatial anti-aliasing and Neural Rendering in the character preview | optional, tested on earlier builds with one preview window |

Tested means played in game by the author on an RTX 4090. Neural Rendering, frame generation, HDR10 output and the optional features are off until you switch them on in the menu or in `OptiScaler.ini`; upscaling follows the quality set in the game.

## Requirements

- Windows and the DirectX 11 version of Final Fantasy XIV (`ffxiv_dx11.exe`).
- An NVIDIA RTX graphics card. Only an RTX 4090 has been tested.
- NVIDIA's own DLSS files. They cannot be redistributed here, so you bring them yourself; the [install guide](../docs/INSTALL.md) names them.
- ReShade 6.8.0, only for the ReShade features.

## Get started

1. Download the zip of the release you want from the [Releases](https://github.com/arnaudcouturier/OptiScaler-FFXIV/releases) page.
2. Close the game. In the folder that contains `ffxiv_dx11.exe`, back up the files the install guide lists.
3. Extract the zip into that folder and overwrite, then add NVIDIA's files.
4. Start the game, press Insert and switch on the features you want.

The [install guide](../docs/INSTALL.md) has the details, the settings of the tested setup and the uninstall steps.

## Status

The current release is v0.2.0 (tag `ffxiv-2026.10.25-d9b62110`) on OptiScaler nightly `d9b62110`. The first release was v0.1.0 (tag `ffxiv-2026.10.23-d9b62110`). Both were played in game by the author on one RTX 4090; other graphics cards are untested. The FFXIV-specific features follow one game build and switch themselves off after a game patch until the fork is updated. What was and was not tested is listed in [docs/STATUS.md](../docs/STATUS.md).

## Documentation

| Document | Contents |
|---|---|
| [Install guide](../docs/INSTALL.md) | Backup, files, NVIDIA files, settings, known limits, uninstall |
| [What works](../docs/STATUS.md) | Every feature with what was tested and what was not |
| [Features and settings](../docs/FEATURES.md) | Each feature, where it comes from, its `OptiScaler.ini` keys and menu card |
| [HUD-less frame generation](../docs/HUDLESS.md) | How it works and what was measured |
| [After a game patch](../docs/FFXIV-PATCH-RUNBOOK.md) | Updating the fork for a new game build |
| [Releasing](../docs/RELEASE.md) | Building, packaging and publishing an archive, and how releases are numbered |
| [Code layout](../docs/SEAMS.md) and [upstream sync](../docs/SYNC-RUNBOOK.md) | All fork code lives under `OptiScaler/ext/`; upstream files carry only small, listed hooks |
| [Credits](../docs/ATTRIBUTION.md) | Who wrote what |
| [OptiScaler README](../README.md) | Upstream's own description of OptiScaler |

## Credits

Most of the feature code in this repository was written by other people. The full table, by area and with commits, is in [docs/ATTRIBUTION.md](../docs/ATTRIBUTION.md).

- The [OptiScaler](https://github.com/optiscaler/OptiScaler) team, for OptiScaler itself.
- [SirenBrink](https://github.com/SirenBrink/OptiScaler_DLSSNR_Multipass_MFG_FFXIV), whose community fork this repository carries over in large part: the FFXIV integration, OptiHDR and scene HDR, HDR PNG capture, XeFG/XeMFG, and frame markers and pacing (as they stood at the import base `36c53a15`), plus the NR linear-light mode, the HDR PNG menu option and the character preview (v6.0, `e17a7362`). The DLSS-G render preset, the NR model picker, the bind-time mip bias and the source-level jitter fix follow the design of his v6.0 source, with code written for this repository.
- Dagherbou (DLSS Neural Rendering base), y4my4my4m (multipass, MFG unlock), wilsjo2 (pre-SR, native path, MFG unlock hardening), byunjoe (HUD-less frame generation).
- janblade, Citrus333, Ainquisition, MatheusGViana, ShyVortex, BeliyG3, stegalloway, mavismmg and dashdogy, whose work SirenBrink's fork adapts; see the attribution table.
- [RenoDX](https://github.com/clshortfuse/renodx) (clshortfuse), the HDR shaders behind scene HDR; its notice ships with the code that uses it.

## License

GPL-3.0, the same as upstream; see [LICENSE](../LICENSE). This is an unofficial fork and is not affiliated with the OptiScaler team or Square Enix.
