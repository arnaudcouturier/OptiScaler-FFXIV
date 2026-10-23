# Attribution

Third-party work this fork builds on. Licences for vendored libraries stay next to their code
(`external/`, `OptiScaler/ext/hdr/RenoDX_ATTRIBUTION.txt`, `OptiScaler/ext/hdr/shaders/renodx/NOTICE.md`).
This file credits people whose work is **in the fork's own code**, and says plainly where that work came from.

## A collaborator - `depth-jitter-20261013` build (behavioural reconstruction from a binary)

A collaborator of the fork's author shipped an unpublished build of this fork
(`OptiScaler v10.0.0-dev (36c53a15-community-preview-depth-jitter-20261013)`, a `winmm.dll`). No source for it
is available. The features below were **reconstructed from that binary by static reverse engineering**
(a disassembly comparison for the shader, decompiler output read for the C++), then re-implemented from the
observed behaviour. They are not the collaborator's source code, and nothing was copied from decompiler output. Where
the reconstruction reproduces the collaborator's visible names (ini keys, ini values, menu labels, tooltip meaning) it does
so on purpose, so that the collaborator's users can move between the two builds.

| ID | What | Where in this repo |
|---|---|---|
| F2 | The edit to the NR compute shader for the "Linear light" proxy (reproduced to the byte: the compiled shader is identical to the one in the collaborator's build) | `OptiScaler/ext/nr/shaders/dlssnr.hlsl` |
| F3 | NR HDR mapping mode "Linear light + composed" (`ReversibleMode = 5`) | `OptiScaler/ext/nr/` (config, menu, presets), `OptiScaler/ext/ini/DlssNr.ini` |
| F4 | HDR PNG option "Include OptiScaler menu" (`ScreenshotMenu`) | `OptiScaler/ext/hdr/screenshot/`, `OptiScaler/ext/ini/HDR.ini` |
| F5 | CharaView: isolated treatment of the 3D character preview (configuration, shader recognition, the preview renderer's target table with its guards, the spatial anti-aliasing path with optional 2x upscaling, the hooks and menu card; the isolated DLSS pass follows) | `OptiScaler/ext/ffxiv/chara/`, `OptiScaler/ext/ini/FFXIV.ini`, `OptiScaler/ext/ini/HDR.ini` |

F5 deserves a plain note: it is the largest of these features and the only one that reads a structure of the game executable. It was reconstructed from the binary's behaviour and the shader source it embeds (read and described in words, not copied), then written fresh; the executable is read through a pattern scan and checks of this fork's own, stricter than the original's. The configuration keys, section name and visible menu texts are reproduced on purpose so ini files and menus match between the two builds.

What was observed and how sure the reconstruction is: `docs/RECONSTRUCTION-REFERENCE.md`.

Five later features take their names and ideas from the collaborator's v6.0 build, whose source is published (GPL-3.0). Each is described in one sentence; where the sentence says the code is the fork's own, no source of his was copied. The NR model version picker follows his naming and idea: the `OptiScaler\NR.<version>` folders, the `[DlssNr] Runtime` key and one immutable selection with a revision that the scene pass compares; the code (catalog, scan, selection, switch, card, preview rebuild) is this fork's own and nothing was copied from his source. The DLSS-G render preset (`[DLSSG] RenderPreset`, 0 Auto / 1 A / 2 B / 3 C) and its live switching follow the approach of his `PresetCache`: answering the driver-settings query and, for the live change, rebuilding each new endpoint's private configuration through NVIDIA's own code by hooking the cached-settings reader, the endpoint create function and the configuration copy function; the structural guards (masked frame sizes, same-global and context checks), the provider choice, the readout and the switch state machine are written fresh for this fork. The bind-time mipmap bias (hooks on the six `SetSamplers`/`GetSamplers` entry points that bind an immutable sampler copy carrying log2(render/output), `GetSamplers` returning the game's sampler, the `[Mipmap] AutomaticFFXIV` name) follows the approach of his `Dx11AutomaticMipBias`; the table, the lock-free bind path, the add-or-replace detection and the settings precedence are the fork's own code. The idea of skipping the release sleep once the queues are drained, and of serialising the DX11 feature lifecycle, follows his v6.0 source; the code here is the fork's own, written fresh with a narrower lock, a closed/in-flight handle protocol and a three-way result, and nothing was copied. Two more pieces follow his source in the idea only: the install retry of the native-quality hooks follows his `IFeature.cpp` (up to three attempts, dead threads skipped before suspending), and the frame-pacing CPU trace (the line format, the phase names) and the idea of presenting the hidden DX11 swapchain with `DXGI_PRESENT_DO_NOT_WAIT` follow his `FramePacingTrace` work; the code of both is this fork's own and nothing was copied. The source-level jitter fix (`[FFXIV] JitterSource`: dividing the game's jitter generator output by the native strength so the full +-0.5 px lattice is drawn, NVIDIA's `8 * output/render area` phase count in place of the game's unbounded one, and telling the upscaler the offset projected from the same frame's NDC translation) follows the approach of his `FfxivJitter` hooks (v6.0 source, GPL-3.0); the controller, the pairing ring, the verification against the generator's own value, the fall-back, the instruction-guard table and the install are this fork's own code, with the pure arithmetic (phase count, native strength, normalisation, projection) re-derived from the executable's instructions.

Thank you.

## clshortfuse / RenoDX

See `OptiScaler/ext/hdr/RenoDX_ATTRIBUTION.txt` and `OptiScaler/ext/hdr/shaders/renodx/NOTICE.md`.
