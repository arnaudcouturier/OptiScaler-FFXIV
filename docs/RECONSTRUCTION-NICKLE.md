# Reconstruction notes: features recovered from Nickle's `depth-jitter-20261013` build

Nickle's build has no available source. F2, F3 and F4 were recovered from the binary and written fresh here, from
the behaviour that was observed. Credit and provenance: [ATTRIBUTION.md](ATTRIBUTION.md). Nothing in this repo is
pasted decompiler output; the notes below keep only addresses, string anchors and observed constants as evidence.

**Binaries used (read-only).**

| Role | File | Size | Notes |
|---|---|---|---|
| Nickle's build | `builds\winmm.dll` | 27,207,680 B | SHA-256 `8837e1d7dbffcac9...`, PE time 2026-10-13 11:51:26 UTC |
| Baseline (closest) | `builds\winmm-ffxiv-20261013.dll` | 27,097,088 B | The fork before Nickle's changes. Chosen over `winmm-ffxiv-overlayfix.dll`: Nickle's build shares the older overlay-wait log text (`RenderImGui_DX12 DX12 overlay completion wait ...`, 2 hits in both); `overlayfix` has the newer `WaitForOverlayFence` text, which Nickle's build lacks. A raw ASCII-string diff (strings >= 8 chars) gives 49 baseline-only / 126 Nickle-only strings for `20261013`, 47 / 128 for `overlayfix` - a close call, the overlay-wait string decides |

Confidence words used below: **observed** (read directly from the binary), **inferred** (follows from observed
facts), **unknown** (not established).

---

## F2 - the edited NR compute shader (`dlssnr`, +15 instructions)

**Result: reproduced exactly.** The HLSL change in `OptiScaler/ext/nr/shaders/dlssnr.hlsl` compiles, with the
project's own fxc and recipe (`-T cs_5_0 -E CSMain -O3`, the one in `ext/ext.props`), to a 40,748-byte DXBC
container that is **byte-identical** to the one embedded in Nickle's build (SHA-256
`b9cbd405231c648f4d647fd36009298b51b63b37d28101a74f7de2247af2aab8`). Identical bytes mean the same shader
semantics and the same instruction stream; they do not prove the same source text (comments, names and
equivalent spellings of an expression are invisible in a compiled shader).

### Evidence

| What | Where |
|---|---|
| Nickle's blob | `winmm.dll` file offset `0x17eae00` (RVA `0x17ec000`), 40,748 B, cs_5_0, container hash `c830df5d58e7aa8bdf185a70d2fe7ddf` |
| Baseline blob | `winmm-ffxiv-20261013.dll` `0x17d7d50`, and `winmm-ffxiv-overlayfix.dll` `0x17d7f60`: both 40,424 B, container hash `11b7158f6461fe76a2e1d05c08dd7a23`, i.e. the same blob in both candidates |
| Baseline source check | the unmodified `dlssnr.hlsl` of this repo, compiled with the same fxc and flags, is byte-identical to the baseline blob (so the baseline HLSL is the right starting point) |
| Instruction slots | baseline 1244, Nickle 1259 (`+15`), as the earlier string analysis found; the modified HLSL also gives 1259 |
| Method | DXBC containers carved out of each DLL by magic and size, disassembled with `fxc -dumpbin` (Windows SDK 10.0.26100), diffed; the candidate HLSL was then compiled and its disassembly diffed against Nickle's (0 differing lines) |

### What the +15 instructions are

The constant buffer (`Params`, 256 B, 42 variables) and all resources are unchanged: the edit is pure math on
the existing `gReversibleMode` field (`cb0[4].w`), adding a sixth value, `5`. Three places differ from the
baseline; everything else in the diff is the compiler renumbering registers around them (plus a few extra
scalar `dp2`/`sqrt` pairs where the register allocator no longer packs two lengths into one vector op).

1. **Encode (`gMode == 0`), straight after `frame / WhitePoint()`.** New early-out, seven instruction slots
   (`ieq`, `if_nz`, `mov_sat`, `mov`, `store_uav_typed`, `ret`, `endif`): if `gReversibleMode == 5`, write
   `float4(saturate(normalized), 1.0)` to the target and return. No curve, no sRGB encode, alpha forced to 1.
   The untouched-frame copy (`gKeep`) was already written before this point.
2. **Resolve, reading the proxy and the model back.** The decode `gPassthrough != 0 ? raw : SrgbToLinear(raw)`
   becomes `(gPassthrough != 0 || gReversibleMode == 5) ? raw : SrgbToLinear(raw)` for **both** the proxy
   (`t0`) and the model (`t1`). Three slots (`ine`, `ieq`, `or`); the same flag drives both `movc`s.
3. **Resolve, matched-residual proxy rebuild (`gTransfer == 1`).** The rebuilt full-resolution proxy gains one
   `movc` for mode 5: `saturate(original)`, the same value passthrough already used, placed between the
   passthrough test and the soft-knee / hybrid / Neutwo chain.

No other mode's code path changed as far as the disassembly diff shows (modes 0-4 keep the same operations). The `gReversibleMode != 0`
alpha rule of the other reversible proxies is not touched: mode 5 returns before it.

### How the repo expresses it

* `dlssnr.hlsl`: the three edits above (comment on the `gReversibleMode` field extended, new block commented).
* No build wiring changed: the file is already in `ext/nr/nr.shaders.props`; the Vulkan (SPIR-V) variant compiles
  from the same source and gets the same behaviour.
* The change is inert until mode 5 is selected (F3 makes it selectable). A `ReversibleMode` of 0-4 produces
  the same compiled behaviour as before.

### Tests

`tests/ext/nr/nr_linear_light_shader` (class `warp`): runs the real `dlssnr.hlsl` on D3D11 WARP. It checks that
mode 5 encodes `saturate(frame / white)` with opaque alpha and keeps the untouched copy, that mode 0 still writes
the sRGB soft-knee proxy with the frame's alpha, that the resolve returns the frame when the model returns its
input, applies a model edit, and that other modes still decode the proxy as sRGB (negative control). The test
also pins the documented cost: a pixel above the white point is clipped in the proxy, so its hue can shift
while its luminance is kept.

### What to verify in-game

Nothing for F2 alone (it cannot be reached without F3). See F3.

---

## F3 - NR HDR mapping "Linear light + composed"

**Result: reconstructed, high confidence on what the setting is and does, medium on "nothing else changed".** It is
the sixth value (`5`) of the existing NR setting `[DlssNr] ReversibleMode`, shown in the combo "HDR mapping
(experimental)". It is not an HDR-output setting (it lives in `ext/nr`, not `ext/hdr`): it decides how a *linear* NR
input surface is shown to the neural model. The brief expected `ext/hdr`; the code puts the setting in `ext/nr`.

### Evidence (all in `builds\winmm.dll`, Ghidra image base 0x180000000)

| Observation | Where |
|---|---|
| Label `Linear light + composed` | string at 0x1818faaf0 (one hit), referenced only by the 6th pointer of the combo's name table at 0x1819a2af0 (6 entries, in order Off / Neutwo composed / Neutwo replace / Hybrid composed / Hybrid replace / Linear light) |
| Combo built in the NR menu function `0x180350f10`: `HDR mapping (experimental)`, item count 6, stored value reset to 0 when above 5 (`5 < value`) | decompiled block around the label |
| Tooltip gains a last sentence: `Linear shows each normalized colour channel directly to NR. It is experimental, applies to linear input, and clips the model input above white; saturated highlights may change hue. Tone-mapped SDR input remains passthrough.` | same function |
| Saved NR presets accept the new value: the `ReversibleMode` range upper bound is the double `0x4014000000000000` = 5.0 (the neighbouring `Transfer` is 1.0, `MaxRatio` 30.0, matching the clean repo) | preset validation function containing the xref 0x18035c72f |
| Ini key unchanged: `ReversibleMode` (one string, 0x1818d0590, used by config load 0x1801f8c37, save 0x180205fb6 and the preset code); no clamp in the config read, same as before | |
| Shader side | F2 above (the compiled shader gets the mode from the same constant buffer field `gReversibleMode`) |

### What the repo does

* `ext/nr/shaders/dlssnr.hlsl`: F2.
* `ext/nr/menu/DlssNr_Menu.cpp`: six entries, the clamp follows the table size, Nickle's tooltip text.
* `ext/nr/presets/NrUserPresets.h`: `ReversibleMode` range 0..5.
* `ext/ini/DlssNr.ini` and `ext/nr/NrConfig.h` comments list mode 5.
* No config code change (a `uint32` read with no clamp, as before). The two dispatch sites and the Vulkan site pass
  the value through to the shader unchanged, so nothing there needed editing.

### What the proxy surface is (checked, because linear values in an 8-bit surface would band in the shadows)

The encode writes the proxy **in place** into the colour surface NR is handed (`DispatchPass(..., target, ...)` in
`ext/nr/pass/DlssNr_Dx12.cpp`; the untouched frame is kept in a copy). The pass only leaves passthrough when the frame
is flagged as linear HDR (`frame.ColourIsLinearHdr`, a decision made before the mapping mode is looked at); for any
other frame the encode is a plain copy, which is what the tooltip's last sentence says. Linear HDR colour surfaces are
floating-point (the code's own check, `FormatCanHoldLinearHdr`, lists R16G16B16A16_FLOAT, R32G32B32A32_FLOAT,
R11G11B10_FLOAT), so mode 5 does not store linear values in an 8-bit target. No mode-dependent surface format exists in
the clean repo's code path; whether Nickle's build changed one was not exhaustively excluded (see unknowns).

### Interplay with the NR scene input (`[HDR] NRSceneInput`)

The scene-input code in this repo never reads the mapping mode, and nothing observed in Nickle's build suggests it does:
the scene path hands NR a linear surface and the mapping setting then applies to it like to any other linear input.
This was not searched exhaustively.

### Unknown / not established

* Whether Nickle's build changed any C++ besides the menu, the preset range and the cbuffer value. The `ReversibleMode`
  field offset (0x1198 in the config object) is read from 33 places in Nickle's text section against 31 in the
  baseline (`winmm-ffxiv-20261013.dll`), but that offset is shared with unrelated structures and the two builds differ in
  many other ways, so the difference was not attributed.
* Nickle's default is `0` (unchanged), inferred from the clamp resetting to 0 and from the unchanged key.

### What to verify in-game

1. NR on, an HDR (linear FP16) FFXIV session: choose **Linear light + composed**. The picture should stay stable and
   close to the other "composed" modes; compare against `Off (soft knee)`.
2. Bright, saturated highlights (spell effects, sky): they may change hue (the model sees a clipped input). That is the
   documented cost of the mode, not a bug; tell me if it looks worse than the other modes.
3. Shadows: look for banding (the proxy is FP16 on a linear HDR surface, so none is expected).
4. SDR / tone-mapped input: the setting must do nothing (passthrough).
5. Save a preset with the mode selected, reload it; set `ReversibleMode=5` in the ini and confirm the menu shows it.
