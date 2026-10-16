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
input, applies a model edit, that other modes still decode the proxy as sRGB (negative control), and that the
matched-residual rebuild (the third shader change) gives the same result as the classic transfer (a mutation that drops
the mode-5 term from the rebuild makes it fail). The test also pins the documented cost: a pixel above the white point is clipped in the proxy, so its hue can shift
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

Shader side, settled: the NR scene-input shader blob and both `DlssNr_Spatial` blobs in Nickle's build are
byte-identical to the baseline's (`analysis/dll-string-diff.md` section 5; only `DlssNr_Shader` differs), so the only
shader that knows about mode 5 is the one reproduced in F2. C++ side: the scene-input code in this repo never reads the
mapping mode, and nothing observed in Nickle's build suggests it does (the scene path hands NR a linear surface and the
mapping setting then applies to it like to any other linear input). The C++ side was not searched exhaustively.

### Unknown / not established

* Whether Nickle's build changed any C++ besides the menu, the preset range and the cbuffer value. The `ReversibleMode`
  field offset (0x1198 in the config object) is read from 33 places in Nickle's text section against 31 in the
  baseline (`winmm-ffxiv-20261013.dll`), but that offset is shared with unrelated structures and the two builds differ in
  many other ways, so the difference was not attributed.
* Nickle's default for `ReversibleMode` was not observed (the constructor write for the default byte at config offset
  0x11a0 was not located). The menu resets an out-of-range value to 0, which says nothing about the default; this repo
  keeps `0` (soft knee), the baseline's default.

### What to verify in-game

1. NR on, an HDR (linear FP16) FFXIV session: choose **Linear light + composed**. The picture should stay stable and
   close to the other "composed" modes; compare against `Off (soft knee)`.
2. Bright, saturated highlights (spell effects, sky): they may change hue (the model sees a clipped input). That is the
   documented cost of the mode, not a bug; tell me if it looks worse than the other modes.
3. Shadows: look for banding (the proxy is FP16 on a linear HDR surface, so none is expected).
4. SDR / tone-mapped input: the setting must do nothing (passthrough).
5. Save a preset with the mode selected, reload it; set `ReversibleMode=5` in the ini and confirm the menu shows it.
6. Paths no automated test runs with mode 5 (they run the same shaders as Nickle's build, so the behaviour is faithful,
   but they were not exercised here): working scale below 1 (the box downsample now averages the proxy in linear instead
   of sRGB), working scale above 1 (supersample), spatial compression on, and matched residual at a reduced scale. The
   matched-residual rebuild at full scale is covered by the WARP test.

---

## F4 - HDR PNG option "Include OptiScaler menu" (`[HDR] ScreenshotMenu`)

**Result: reconstructed, high confidence on the behaviour; medium on the identity of two global flags (inferred from
how they are used).** With HDR PNG selected and the menu open, the PNG is taken from the presented image *after* the
menu was drawn onto it (so the menu, in its HDR colours, is in the file). With the menu hidden, or with SDR PNG, the
existing clean capture is used unchanged.

### Evidence (all in `builds\winmm.dll`)

| Observation | Where |
|---|---|
| Label `Include OptiScaler menu`, reset id `FfxivHDRScreenshotMenu`, tooltip `HDR PNG: capture the presented image after drawing the visible OptiScaler menu, including its HDR colours. Useful for settings comparisons.` / `With the menu hidden, screenshots use the normal clean capture. Requires the overlay menu. SDR PNG captures remain unchanged. The separate Companion window is not captured.` | strings 0x1818df388, 0x1818df3a0, 0x1818df3c0; single code reference in the HDR menu card function `0x180269f10` |
| The checkbox sits right after the "Screenshot format" combo, before "HDR PNG: ICC only (sharing)", inside the same `BeginDisabled(format != HDR PNG)` | decompiled HDR menu card |
| Ini key `ScreenshotMenu` in section `[HDR]` (the section operand is the string "HDR" at 0x18186DEB4, bytes read directly, the same operand `ScreenshotIccOnly` uses), a `CustomOptional<bool>` with value / has-value / default bytes at config offsets 0x31ae / 0x31af / 0x31b0 (between `ScreenshotIccOnly` 0x31a8 and `ScreenshotFormat` 0x31b4) | string 0x1818d1428, load reference 0x1801fea67, save reference 0x180204335 |
| Default `false`: the object is built by the `CustomOptional<bool>` constructor at 0x18020d940, which stores `DL` in the default byte, called with `edx = 0` | constructor sequence at 0x1801f2df0 |
| One predicate decides everything (`0x18034ce80`): "HDR output is active" (global byte 0x1819b93fb) **and** screenshot format == 1 (HDR PNG) **and** `ScreenshotMenu` **and** `[Menu] OverlayMenu` (config offset 0x1f28, the key string 0x1818d07a0 is what the config load stores there) **and** "menu is visible" (global byte 0x1819cc620, cleared together with the other menu state when the overlay is not initialised) | tiny function, callers 0x18015a660 (bridge present) and 0x18028dbe0 (the DX12 overlay render) |
| **Bridge side:** when the predicate holds, the bridge does *not* prepare its own screenshot, so the pending request is left for the overlay | the bridge function guards its screenshot preparation with `!predicate` |
| **Overlay side** (function with the log tag `RenderImGui_DX12`, 0x18028dbe0): after `RenderDrawData` inside the HDR menu-layer scope, if the HDR overlay path is on and the predicate holds, it calls the screenshot `Prepare` (0x18034cf30, the same function as the bridge's) with the menu window handle, the device, the overlay's command list, the current back buffer, no SDR source, and resource state `4` (= RENDER_TARGET); then it transitions the back buffer to present, closes and executes the list, and submits the job on that queue (0x18034db30) | |
| `Prepare` is shared: same request window (1000 ms), same screenshot-key range check (1..255), same foreground-window test, same busy flag, same `OptiHDR-<date>-<ms>-<tick>-HDR.png` file name and "HDR screenshot queued" status. No new strings: the screenshot-related strings of the build contain no menu-specific status or log text | string search over `creenshot` |

### What the repo does (all inside `ext/`, zero upstream lines)

* `ext/hdr/HdrConfig.{h,cpp}`: `screenshotMenu` bound to `[HDR] ScreenshotMenu`, default false. `ext/ini/HDR.ini` documents it.
* `ext/hdr/menu/HdrMenu.cpp`: the checkbox, reset button and Nickle's tooltip, in the same disabled block as the ICC option.
* `ext/hdr/screenshot/MenuCapture.h`: the pure five-condition decision; `HdrScreenshot.{h,cpp}`: `MenuCaptureWanted()` gathers the five inputs, `PrepareMenu()` runs the existing `Prepare` for the finished PQ output in RENDER_TARGET state.
* `ext/hdr/menu/Overlay.cpp`: `OverlayScope`'s destructor calls `PrepareMenu` right after the menu layer was composited into the PQ output (the output is still a render target there; upstream transitions it to present afterwards, exactly the state Nickle used). The existing GPU-owner after-execute callback submits the job when the overlay's command list executes (Nickle submitted explicitly right after its own execute; same effect).
* `ext/hdr/BridgeHdr.cpp`: the bridge leaves the request alone while `MenuCaptureWanted()`.
* SEAMS catalog: the `PrepareScreenshot` and `OverlayScope` rows note the new behaviour; no new seam.

### Tests

`tests/ext/hdr/hdr_screenshot_menu` (cpu): the five-condition decision, every single dropped condition, format values other than 1, and the exhaustive 32-combination check. `tests/ext/hdr/hdr_config` (existing test) is extended with the new key (default, table entry, ini load and save); no assertion was loosened.

### Unknown / not established

* The exact meaning of the two global bytes is inferred from use. 0x1819b93fb: in the overlay function the "HDR overlay on" flag (0x1819b91ad) is set from `0x1819b93fb != 0 && swapchain format == 0x18 (R10G10B10A2)`, which mirrors this repo's `OverlayRtvFormat` (`Active() && format == R10G10B10A2`), so it is the OptiHDR-active flag. 0x1819cc620: reset next to the menu's other visibility state in the overlay function, so it is the menu-visible flag.
* What happens when a menu capture was requested but the overlay does not draw that frame (empty draw data, layer preparation failure): in the binary the request is simply not consumed and expires after one second with no message. The repo behaves the same.
* Nickle's `Prepare` and `Submit` were compared by reading against this repo's versions (not diffed byte for byte): no difference was seen.
* **The one real design difference from Nickle, and the top risk.** Nickle submits the job explicitly right after its own `ExecuteCommandLists` on the overlay queue. This repo relies on the GPU-owner after-execute callback seeing the overlay's command list through the queue `ExecuteCommandLists` detour (which hooks the shared queue vtable, so it should). If the detour misses that list, no PNG is written and the capture stays prepared, blocking further screenshots until the next key press after about 5 seconds drops it.

### What to verify in-game

1. **Exactly one PNG appears with the menu open** (the top risk above): Settings, FFXIV HDR card: Screenshot format = **HDR PNG**, tick **Include OptiScaler menu**, bind a screenshot key, save. Open the OptiScaler menu and press the key: one PNG appears in `game\OptiScaler\Screenshots`, the menu is in it with its HDR colours (not dimmed or tone-mapped, no black rectangle). If no file appears and the status line stays "HDR screenshot queued" or "Screenshot requested", the overlay's command list did not reach the after-execute callback (check the queue detour, `ext/gpu/QueueHook.cpp`, and that the GPU owner is registered); a later key press after about 5 seconds recovers.
2. Close the menu and press the key again: a clean capture, as before.
3. Screenshot format = SDR PNG: the option is greyed out and the SDR PNG has no menu.
4. Frame generation on (DLSS-G / XeFG): still exactly one file per key press, no hitch, no device-removed in `OptiScaler.log`.
5. Does the screenshot key fire while the menu is open? (The key is polled in the menu's shortcut pass; if it does not fire with the menu up, the menu capture cannot be triggered by the key. That limitation would be shared with Nickle's build.)
6. `OverlayMenu=false`: the menu cannot be open, so the capture is the clean one.
