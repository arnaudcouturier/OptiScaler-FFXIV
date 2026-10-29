# NR Classic sharpening (RCAS-style)

`[DlssNr] ClassicSharpen` (default off) and `ClassicSharpenStrength` (default 0.4).
When the Classic enlargement applies (the model is reduced on some axis and above full on none, and `Transfer` is anything but Matched residual -- the same rule the Enlargement combo greys by), the NR pass runs one extra compute
dispatch after the resolve: a 3x3 adaptive sharpen over the resolved frame, staged through
`hdrCopy` and copied back over the frame. Matched residual keeps the native frame as its
base and is never sharpened; compare views, debug views, the skin-mask view and the clean-frame switch bypass
the step. With the key off nothing runs and the frame is byte-identical to before.

## Why a separate shader

The sharpen is its own HLSL file (`ext/nr/shaders/dlssnr_classic_sharpen.hlsl`) and its own
compiled blob (`DlssNr_ClassicSharpen_cso`), not a mode of the main shader. The main
`dlssnr.hlsl` and its blob are therefore bit-identical to before, so every existing replay
set still matches. The filter core is upstream OptiScaler's own plain RCAS
(`shaders/rcas/precompile/rcas.hlsl`, after the RCAS-for-ReShade port of AMD FidelityFX
RCAS), reduced to the static-sharpness path: no motion adaptation, no debug tint, no
contrast adaptation. The strength default (0.4) is upstream RCAS's own default sharpness. Strength 0 skips the dispatch and the copy-back entirely, so a no-op costs nothing per frame. The strength clamps to 0..1 at load and again at the dispatch (a live menu write lands past the load clamp); an uninterpretable value fails open to 0.4. A failed pipeline creation is tried once per device lifetime, then left off with one log line, and a target with mips or array slices fails open to the resolve output.

## Plumbing

- Constants: the dispatch reuses `DlssNrConstants`, carrying the strength in the
  `TransferStrength` slot (the shader's `Params` cbuffer mirrors the struct prefix, the way
  the guide-match shader does). `Mode` is the new `DlssNrMode_ClassicSharpen` (15).
- Temp texture: `hdrCopy` is dead after the resolve, has the frame's format, and is always
  created with unordered-access support, so it stages the sharpened frame before the copy
  back. Both textures are returned to the states the resolve left them in.
- Replay: the dispatch goes through `DispatchPass` like every other NR dispatch, is
  recorded under kind `classic_sharpen`, and its blob is in the replay blob table, so a
  captured set with sharpening on replays on the software adapter.
- Menu: checkbox plus strength slider in the NR card next to Enlargement, greyed out unless
  Classic applies; saved in the NR presets next to the other enlargement settings.

Like the upstream filter, the normalized ring clamps at zero: negative scRGB channels clamp rather than sharpen. An Inf pixel normalizes to NaN, which then spreads to its neighbours through the shared min/max ring.

## Vulkan

Unchanged: the Vulkan NR path uses the same main shader (its SPIR-V build is untouched) and
has no consumer of the new sharpen shader, so its frames are identical to before.
