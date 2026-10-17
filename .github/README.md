# OptiScaler-FFXIV

Unofficial fork of OptiScaler (tracking nightly), not affiliated with the OptiScaler team.

> **Status (October 2026): release candidate.** The feature stack is ported onto OptiScaler nightly `fd6c2870`. Tag `ffxiv-2026.10.17-fd6c2870-rc1` is the first tagged build that launches and plays; there are no packaged binaries yet. The older tag `ffxiv-ref-equiv-fd6c2870` does not launch (two startup bugs fixed after it) and is superseded by the rc tag.

An FFXIV-focused build of [OptiScaler](https://github.com/optiscaler/OptiScaler). It adds DLSS Neural Rendering with pre-SR multipass, DLSS-G multi frame generation unlocks for RTX 30/40, XeFG/XeMFG, OptiHDR, RenoDX scene HDR and a native FFXIV integration. All fork code lives under `OptiScaler/ext/`, so upstream files carry only small, listed seams (see [docs/SEAMS.md](../docs/SEAMS.md)). The upstream README is [README.md](../README.md).

## Features

| Feature | Status |
|---|---|
| DLSS Neural Rendering with pre-SR multipass | tested (RTX 4090) |
| DLSS-G MFG unlock, RTX 40 | tested (RTX 4090) at 2x to 4x; 5x and 6x are experimental (one GPU hang seen at 6x) |
| OptiHDR (HDR10 output) | tested (RTX 4090) |
| RenoDX scene HDR | tested (RTX 4090) |
| FFXIV native integration (render geometry, native quality, nameplates, companion) | tested (RTX 4090) |
| HUD-less frame generation (UI-free image, HUD pasted back; ReShade presets via a built-in add-on) | opt-in, off by default; tested in SDR with DLSS-G 2x-4x (RTX 4090), no visible gain reported; HDR untested; one GPU hang at the experimental 6x MFG |
| Recovered from Nickle's build: NR shader edit, "Linear light + composed" HDR mapping, HDR PNG with menu | untested in game (credit: [docs/ATTRIBUTION.md](../docs/ATTRIBUTION.md)) |
| Character preview DLSS and spatial AA (CharaView, reconstructed from Nickle's build) | opt-in, off by default; untested in game |
| NR ViT capture (`[DlssNr] VitCapture`) | diagnostic only, off by default |
| AMD HIP pre-SR | untested / community |
| XeFG / XeMFG | untested / community |
| DMFG on RTX 50 | untested / community |
| Ampere (RTX 30) MFG loader | untested / community |
| Vulkan NR path | untested / community |

"tested" means played in game by the maintainer on the stated GPU. Everything else compiles and follows the same code paths, but nobody has verified it on real hardware.

## Install

There is no published release yet; the release-candidate tag has no packaged binaries. When a release archive exists it is attached to the GitHub release of its tag (`ffxiv-<yyyy.mm.dd>-<upstream sha8>[-rcN]`); until then you can build one yourself ([docs/RELEASE.md](../docs/RELEASE.md)). The archive contains its own `INSTALL.txt` with the same steps.

1. **Back up first.** Close the game and the launcher. In the folder that contains `ffxiv_dx11.exe`, copy `winmm.dll`, `nvngx.dll_dlssnr.dll`, `OptiScaler.ini` and the `OptiScaler` folder to a new folder, for those that exist.
2. **Copy the files.** Extract the archive into that folder and overwrite. The archive holds `winmm.dll` (the fork's DLL, installed under the name the game loads), the NR forwarder `nvngx.dll_dlssnr.dll` (keep the exact name), `OptiScaler.ini`, the `OptiScaler` folder with third-party libraries, a `Licenses` folder, `NOTICE.txt` and `SHA256SUMS.txt`. Optional: compare hashes with `SHA256SUMS.txt`.
3. **Know the defaults.** Every value in the shipped `OptiScaler.ini` is `auto`, which means the built-in default: Neural Rendering, frame generation, HDR output, HUD-less and the DLSS-G unlock are all off, and the game's own quality choice is left alone. The in-game menu opens with Insert. Set `[Log] LogToFile=true` to get `OptiScaler.log` for bug reports.
4. **Bring your own NVIDIA files.** They are not redistributable from this repository and are not in the archive: `nvngx_dlss.dll` (the game normally has one), `nvngx_dlssnr.dll` for Neural Rendering, `nvngx_dlssg.dll` plus the Streamline plugins in `OptiScaler\streamline\` for DLSS-G frame generation, and for the experimental RTX 30 path a community companion runtime in `OptiScaler\dlssg_sm86\`. Get them from NVIDIA's DLSS and Streamline distributions; `INSTALL.txt` lists the exact names. HUD-less with ReShade presets needs your own ReShade 6.8.0.
5. **Turn features on** in the menu or in `OptiScaler.ini` (see [docs/FEATURES.md](../docs/FEATURES.md) for every key). The set used in the maintainer's tested setup, for DLSS on this DirectX 11 game: `[Upscalers] Dx11Upscaler=dlss_12`; Neural Rendering `[DlssNr] Enabled=true`; frame generation `[FrameGen] Enabled=true`, `FGInput=upscaler`, `FGOutput=dlssg`; HDR10 output `[HDR] FfxivHDR=true` with Windows HDR on (restart; `[HDR] Mode=1` adds RenoDX scene HDR); HUD-less `[HudLess] Mode=ui_free` (opt-in, needs DLSS-G output).
6. **Known limits.** Only the features marked tested above were played, on one RTX 4090. HUD-less is opt-in and untested in HDR. 5x and 6x multi frame generation on RTX 40 are experimental and one GPU hang happened at 6x: stay at 4x or lower. FFXIV-specific features follow one game build; after a game patch they switch themselves off until the fork is updated ([docs/FFXIV-PATCH-RUNBOOK.md](../docs/FFXIV-PATCH-RUNBOOK.md)).
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
