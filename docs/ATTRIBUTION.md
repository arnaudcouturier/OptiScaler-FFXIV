# Attribution

Third-party work this fork builds on. Licences for vendored libraries stay next to their code
(`external/`, `OptiScaler/ext/hdr/RenoDX_ATTRIBUTION.txt`, `OptiScaler/ext/hdr/shaders/renodx/NOTICE.md`).
This file credits people whose work is **in the fork's own code**, and says plainly where that work came from.

## Nickle - `depth-jitter-20261013` build (behavioural reconstruction from a binary)

Nickle, a collaborator of the fork's author, shipped an unpublished build of this fork
(`OptiScaler v10.0.0-dev (36c53a15-community-preview-depth-jitter-20261013)`, a `winmm.dll`). No source for it
is available. The features below were **reconstructed from that binary by static reverse engineering**
(a disassembly comparison for the shader, decompiler output read for the C++), then re-implemented from the
observed behaviour. They are not Nickle's source code, and nothing was copied from decompiler output. Where
the reconstruction reproduces Nickle's visible names (ini keys, ini values, menu labels, tooltip meaning) it does
so on purpose, so that Nickle's users can move between the two builds.

| ID | What | Where in this repo |
|---|---|---|
| F2 | The edit to the NR compute shader for the "Linear light" proxy (reproduced to the byte: the compiled shader is identical to the one in Nickle's build) | `OptiScaler/ext/nr/shaders/dlssnr.hlsl` |
| F3 | NR HDR mapping mode "Linear light + composed" (`ReversibleMode = 5`) | `OptiScaler/ext/nr/` (config, menu, presets), `OptiScaler/ext/ini/DlssNr.ini` |
| F4 | HDR PNG option "Include OptiScaler menu" (`ScreenshotMenu`) | `OptiScaler/ext/hdr/screenshot/`, `OptiScaler/ext/ini/HDR.ini` |

What was observed and how sure the reconstruction is: `docs/RECONSTRUCTION-NICKLE.md`.

Thank you, Nickle.

## clshortfuse / RenoDX

See `OptiScaler/ext/hdr/RenoDX_ATTRIBUTION.txt` and `OptiScaler/ext/hdr/shaders/renodx/NOTICE.md`.
