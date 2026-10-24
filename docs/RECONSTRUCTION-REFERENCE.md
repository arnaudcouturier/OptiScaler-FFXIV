# Binary-derived analysis of an unpublished 2026-10-13 community build

SirenBrink's v6.0 source (commit `fe208a59`, 2026-10-16, contained in `e17a7362`) contains these features, and the fork's versions match it closely: the compiled F2 shader has the same SHA-256 as the one committed there, and the F3 menu strings and the F4 tooltip are identical. This file records what was observed in an earlier, unpublished community build (`36c53a15-community-preview-depth-jitter-20261013`, called "the community build" below) and how sure each point was. It is kept for its byte-level notes: hashes, addresses, string tables and open questions.

F2 (an NR shader edit), F3 (an NR menu setting), F4 (an HDR screenshot option) and F5 (CharaView) are the labels used below. Credit and provenance: [ATTRIBUTION.md](ATTRIBUTION.md).

**Binaries used (read-only; local copies, not part of this repository).**

| Role | File | Size | Notes |
|---|---|---|---|
| The community build | `builds\winmm.dll` | 27,207,680 B | SHA-256 `8837e1d7dbffcac9...`, PE time 2026-10-13 11:51:26 UTC |
| Baseline (closest) | `builds\winmm-ffxiv-20261013.dll` | 27,097,088 B | The fork before the community build's changes. Chosen over `winmm-ffxiv-overlayfix.dll`: the community build shares the older overlay-wait log text (`RenderImGui_DX12 DX12 overlay completion wait ...`, 2 hits in both); `overlayfix` has the newer `WaitForOverlayFence` text, which the community build lacks. A raw ASCII-string diff (strings >= 8 chars) gives 49 baseline-only / 126 community-build-only strings for `20261013`, 47 / 128 for `overlayfix` - a close call, the overlay-wait string decides |

Confidence words used below: **observed** (read directly from the binary), **inferred** (follows from observed
facts), **unknown** (not established).

---

## F2 - the edited NR compute shader (`dlssnr`, +15 instructions)

**Result: reproduced exactly.** The HLSL change in `OptiScaler/ext/nr/shaders/dlssnr.hlsl` compiles, with the
project's own fxc and recipe (`-T cs_5_0 -E CSMain -O3`, the one in `ext/ext.props`), to a 40,748-byte DXBC
container that is **byte-identical** to the one embedded in the community build (SHA-256
`b9cbd405231c648f4d647fd36009298b51b63b37d28101a74f7de2247af2aab8`). Identical bytes mean the same shader
semantics and the same instruction stream; they do not prove the same source text (comments, names and
equivalent spellings of an expression are invisible in a compiled shader).

### Evidence

| What | Where |
|---|---|
| The community build's blob | `winmm.dll` file offset `0x17eae00` (RVA `0x17ec000`), 40,748 B, cs_5_0, container hash `c830df5d58e7aa8bdf185a70d2fe7ddf` |
| Baseline blob | `winmm-ffxiv-20261013.dll` `0x17d7d50`, and `winmm-ffxiv-overlayfix.dll` `0x17d7f60`: both 40,424 B, container hash `11b7158f6461fe76a2e1d05c08dd7a23`, i.e. the same blob in both candidates |
| Baseline source check | the unmodified `dlssnr.hlsl` of this repo, compiled with the same fxc and flags, is byte-identical to the baseline blob (so the baseline HLSL is the right starting point) |
| Instruction slots | baseline 1244, the community build 1259 (`+15`), as the earlier string analysis found; the modified HLSL also gives 1259 |
| Method | DXBC containers carved out of each DLL by magic and size, disassembled with `fxc -dumpbin` (Windows SDK 10.0.26100), diffed; the candidate HLSL was then compiled and its disassembly diffed against the community build's (0 differing lines) |

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
input surface is shown to the neural model.

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
* `ext/nr/menu/DlssNr_Menu.cpp`: six entries, the clamp follows the table size, the community build's tooltip text.
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
the clean repo's code path; whether the community build changed one was not exhaustively excluded (see unknowns).

### Interplay with the NR scene input (`[HDR] NRSceneInput`)

Shader side, settled: the NR scene-input shader blob and both `DlssNr_Spatial` blobs in the community build are
byte-identical to the baseline's (a string diff of the two builds, section 5 of it; only `DlssNr_Shader` differs), so the only
shader that knows about mode 5 is the one reproduced in F2. C++ side: the scene-input code in this repo never reads the
mapping mode, and nothing observed in the community build suggests it does (the scene path hands NR a linear surface and the
mapping setting then applies to it like to any other linear input). The C++ side was not searched exhaustively.

### Unknown / not established

* Whether the community build changed any C++ besides the menu, the preset range and the cbuffer value. The `ReversibleMode`
  field offset (0x1198 in the config object) is read from 33 places in the community build's text section against 31 in the
  baseline (`winmm-ffxiv-20261013.dll`), but that offset is shared with unrelated structures and the two builds differ in
  many other ways, so the difference was not attributed.
* The community build's default for `ReversibleMode` was not observed (the constructor write for the default byte at config offset
  0x11a0 was not located). The menu resets an out-of-range value to 0, which says nothing about the default; this repo
  keeps `0` (soft knee), the baseline's default.

### What to verify in-game

1. NR on, an HDR (linear FP16) FFXIV session: choose **Linear light + composed**. The picture should stay stable and
   close to the other "composed" modes; compare against `Off (soft knee)`.
2. Bright, saturated highlights (spell effects, sky): they may change hue (the model sees a clipped input). That is the
   documented cost of the mode, not a bug; report it if it looks worse than the other modes.
3. Shadows: look for banding (the proxy is FP16 on a linear HDR surface, so none is expected).
4. SDR / tone-mapped input: the setting must do nothing (passthrough).
5. Save a preset with the mode selected, reload it; set `ReversibleMode=5` in the ini and confirm the menu shows it.
6. Paths no automated test runs with mode 5 (they run the same shaders as the community build, so the behaviour is faithful,
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
* `ext/hdr/menu/HdrMenu.cpp`: the checkbox, reset button and the community build's tooltip, in the same disabled block as the ICC option.
* `ext/hdr/screenshot/MenuCapture.h`: the pure five-condition decision; `HdrScreenshot.{h,cpp}`: `MenuCaptureWanted()` gathers the five inputs, `PrepareMenu()` runs the existing `Prepare` for the finished PQ output in RENDER_TARGET state.
* `ext/hdr/menu/Overlay.cpp`: `OverlayScope`'s destructor calls `PrepareMenu` right after the menu layer was composited into the PQ output (the output is still a render target there; upstream transitions it to present afterwards, exactly the state the community build used). The existing GPU-owner after-execute callback submits the job when the overlay's command list executes (the community build submitted explicitly right after its own execute; same effect).
* `ext/hdr/BridgeHdr.cpp`: the bridge leaves the request alone while `MenuCaptureWanted()`.
* SEAMS catalog: the `PrepareScreenshot` and `OverlayScope` rows note the new behaviour; no new seam.

### Tests

`tests/ext/hdr/hdr_screenshot_menu` (cpu): the five-condition decision, every single dropped condition, format values other than 1, and the exhaustive 32-combination check. `tests/ext/hdr/hdr_config` (existing test) is extended with the new key (default, table entry, ini load and save); no assertion was loosened.

### Unknown / not established

* The exact meaning of the two global bytes is inferred from use. 0x1819b93fb: in the overlay function the "HDR overlay on" flag (0x1819b91ad) is set from `0x1819b93fb != 0 && swapchain format == 0x18 (R10G10B10A2)`, which mirrors this repo's `OverlayRtvFormat` (`Active() && format == R10G10B10A2`), so it is the OptiHDR-active flag. 0x1819cc620: reset next to the menu's other visibility state in the overlay function, so it is the menu-visible flag.
* What happens when a menu capture was requested but the overlay does not draw that frame (empty draw data, layer preparation failure): in the binary the request is simply not consumed and expires after one second with no message. The repo behaves the same.
* The community build's `Prepare` and `Submit` were compared by reading against this repo's versions (not diffed byte for byte): no difference was seen.
* **The one real design difference from the community build, and the top risk.** The community build submits the job explicitly right after its own `ExecuteCommandLists` on the overlay queue. This repo relies on the GPU-owner after-execute callback seeing the overlay's command list through the queue `ExecuteCommandLists` detour (which hooks the shared queue vtable, so it should). If the detour misses that list, no PNG is written and the capture stays prepared, blocking further screenshots until the next key press after about 5 seconds drops it.

### What to verify in-game

1. **Exactly one PNG appears with the menu open** (the top risk above): Settings, FFXIV HDR card: Screenshot format = **HDR PNG**, tick **Include OptiScaler menu**, bind a screenshot key, save. Open the OptiScaler menu and press the key: one PNG appears in `game\OptiScaler\Screenshots`, the menu is in it with its HDR colours (not dimmed or tone-mapped, no black rectangle). If no file appears and the status line stays "HDR screenshot queued" or "Screenshot requested", the overlay's command list did not reach the after-execute callback (check the queue detour, `ext/gpu/QueueHook.cpp`, and that the GPU owner is registered); a later key press after about 5 seconds recovers.
2. Close the menu and press the key again: a clean capture, as before.
3. Screenshot format = SDR PNG: the option is greyed out and the SDR PNG has no menu.
4. Frame generation on (DLSS-G / XeFG): still exactly one file per key press, no hitch, no device-removed in `OptiScaler.log`.
5. Does the screenshot key fire while the menu is open? (The key is polled in the menu's shortcut pass; if it does not fire with the menu up, the menu capture cannot be triggered by the key. That limitation would be shared with the community build.)
6. `OverlayMenu=false`: the menu cannot be open, so the capture is the clean one.

---

## F5 - CharaView: isolated treatment of the 3D character preview

CharaView is the feature of the community build that gives the game's 3D character preview (the off-screen scene shown in the
character, equipment and glamour windows) its own handling: an isolated DLSS Super Resolution pass with 2x output, and an
optional spatial anti-aliasing pass on the equipment image. It is off by default and fails open in the community build (any doubt
leaves the native draw untouched), and it is reproduced that way here.

**Status of this repo.** Implemented: everything the feature stands on (configuration, shader recognition, the
preview renderer's target table with its guards, the draw classifier, the slot bookkeeping, the jitter sequence, the hook
layer, the menu card and an identification log), the spatial anti-aliasing path with the optional 2x preview upscaling, and the
isolated DLSS pass (section "F5 DLSS core" below; its interface is `ext/ffxiv/chara/CharaDlssApi.h`).
The evidence for the DLSS path is kept below because the classifier, the slot rules and the jitter follow from it.

Confidence: **high** on the data flow, configuration, menu, shader recognition and the AA filter; **medium** on what each
engine entity is (the names "finisher" and "equipment display" are inferred from structure, the engine names nothing at this
level); **low** on which windows reach the preview renderer (needs one session in the game).

Labels: **[O]** observed in the binary, **[I]** inferred from observed facts, **[U]** unknown. Addresses are Ghidra virtual
addresses of the community build's `winmm.dll` (image base `0x180000000`) unless marked `exe RVA`.

### Evidence

| Observation | Where |
|---|---|
| Nothing of CharaView exists in the baseline build: the strings `FfxivPreviewDLSS`, `CharaView`, `PreviewAA`, `Camera[59]`, `Character preview (experimental)` have 0 hits there and 13 / 27 / 1 / 1 / 1 in the community build **[O]** | string search of both DLLs |
| Five new configuration keys, all `CustomOptional<bool>`: `[FFXIV] PreviewDLSS` (false), `PreviewAA` (false), `PreviewUpscale` (**true**), `PreviewGuideCapture` (false), `[HDR] PreviewNRTrace` (false). `[FFXIV]` is a new section **[O]** | key strings `0x1818d13c8..0x1818d1418`, load `0x1801fe521..0x1801fe6cb`, defaults `0x1801f2d1a..0x1801f2d53` (`mov dl,1` at `0x1801f2d45` for `PreviewUpscale`) |
| Menu: a collapsible header `Character preview (experimental)` inside the NR card, checkbox `Preview DLSS (2x output, experimental)`, counter line `DLSS previews: %u active; %llu evaluations; %llu fallbacks`, two checkboxes `Preview spatial AA` and `2x preview upscaling` (disabled while DLSS is on), a wrapped note, and for AA the status lines `Processed preview draws: %llu`, `Native preview retained: deferred rendering needs a separate integration path.`, `Waiting for the native equipment preview texture.`, `Waiting for the equipment preview display shader.`. No tooltips, no reset buttons **[O]** | strings `0x1818fae00..0x1818faf78`, `0x1818fabf0..0x1818faba0`; code `0x1803510c6..0x180351346` (inside the NR menu function `0x180350f10`) |
| The preview renderer is reached through **one** structure of the game executable: a 20-entry table of render targets. The table is found by a read-only scan of the running executable's `.text` for the 18-byte accessor `48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 C8 02 00 00 C3`; exactly one match is required, there is no RVA and no PE stamp check in the community build's code **[O]**. In the current game executable the pattern exists exactly once (exe RVA `0x26ABD0`) and the global it loads is exe RVA `0x290E490` **[O]** (a read-only byte scan of the executable, nothing was run) | accessor reader `0x18022e460` (cache), scan `0x18022ddc0`, read `0x18022e160` |
| Table layout: entry `i` is a pointer at manager `+0x2C8 + 8i` for `i` in 0..12, `+0x378 + 8(i-13)` for 13..17, `+0x360` (18) and `+0x368` (19); 0x400 bytes of the manager are read, then 0x70 bytes of each entry object: native texture pointer at `+0x68`, width (u32) at `+0x40`, height (u32) at `+0x44`. An entry with a width or height of 0 or above 0x4000 is zeroed. Meaning: 0-7 output images, 8-12 G-buffer, 13-17 transparent G-buffer, 18-19 depth candidates **[O]**; all targets are 576 x 960 in the current executable's initialiser (exe RVA `0x2D51E0`) **[O]**, entries 16 and 17 stay null **[I]** | exe constructor `0x2D4DE0`, factory `0x224870`; `companion.resourceWrapperOffset = 0x68` is the same engine wrapper offset **[I]** |
| A texture matches an entry only if pointer **and** both sizes match; the table is re-read at most every 100 ms; an all-null table matches nothing **[O]** | `0x18022e160` |
| Shaders are recognised by the CRC-32 of the DXBC at `CreatePixelShader`: seven geometry (G-buffer) pixel shaders, one "finisher" (`0xFDB2F4FC`), one "equipment display" (`0x7B40E31A`) **[O]**; the UI set that shows preview images is the one the HDR preview mask already uses **[O]** | `0x1802345f0` |
| Spatial AA runs only when DLSS is off, only on `Draw`, only for the equipment display shader whose `t0` is table entry 0; the output is a private RGBA8 texture 1x or 2x the source, the draw runs with `t0` swapped and `t0` is restored afterwards; deferred contexts are counted and left native **[O]** | `0x1802404d0`, `0x1802308a0`, init `0x1802304b0` |
| The AA filter is a luma-based, alpha-preserving edge filter in the style of FXAA 3.11 (one source texture, one bilinear clamp sampler, no constant buffer); texel size from the **source** dimensions also when the output is 2x **[O]** | HLSL string at file offset `0x18d7b50` |
| DLSS path: per preview texture an NGX feature (render size N x M, output 2N x 2M, MaxPerf, flags `0x42` plus `0x8` when the depth is reversed), 16-phase Halton (2,3) jitter applied as a viewport shift to the depth-prepass and G-buffer draws, a replay of the G-buffer draws into a private RGBA16F target for signed motion, a copy of the pass camera constant buffer (59 float4, only element 58's `.zw` is read), a guides pass, the real NGX evaluate, an alpha re-composition from the native alpha, and an SRV swap for the UI draws; slots expire after 2000 ms, recent entries after 250 ms **[O]** | draw hook `0x1803b5400`, finish `0x1803b46c0`, replay `0x1803b7200`, camera `0x1803b2880` |

### What the repo does (`OptiScaler/ext/ffxiv/chara/`, zero upstream-file lines)

* `CharaConfig.h`, `ext/ini/FFXIV.ini`, one append to `ext/ini/HDR.ini`: the five keys with the community build's names, sections and defaults.
* `CharaTags.h`: the CRC lists; the roles of a shader are one bit mask in one private-data value (the community build uses one GUID per
  role; the same recognition, fewer calls per draw).
* `CharaTable.{h,cpp}`: the pattern scan (a pure function over a byte span, tested with 0, 1 and 2 hits, hits that straddle
  read windows and RIP-displacement decoding), the PE section lookup, the manager parse with the zeroing rules, the lookup
  and the 100 ms cache. The accessor is trusted only if **all** of these hold: the process matches the build described in
  `ext/ffxiv/GameBuild.h` (`MatchCurrentProcess()`); the scan finds the pattern exactly once; the hit and the global it loads
  equal the documented RVAs. Otherwise the feature switches itself off for the session with one log line (never written to
  the config). A null manager pointer or an all-null table is not a failure: it is re-read until the renderer exists.
* `CharaDecision.h`, `CharaSlots.h`, `CharaJitter.h`: the draw classifier (prepass, G-buffer, finisher, consumer), the slot and
  recent-list rules with an injected clock, the Halton jitter. All pure, all tested.
* `CharaDispatch.{h,cpp}`: the dispatcher the lighting capture calls around every draw (`PreviewDLSS ? DLSS : PreviewAA ?
  spatial AA : native`), its own re-entrancy flags, a replay marker for the HDR replays of the same draw, forwarders for copies,
  `ClearRenderTargetView`, `Begin`/`End` and `CreateVertexShader`, and the frame callback.
* `CharaEnhance.{h,cpp}`, `CharaEnhancePass.{h,cpp}`, `shaders/chara_fxaa.hlsl`: the spatial AA, matching the
  observed behaviour. The pass runs inside a private device-context state so the game's pipeline state is untouched; the
  filtered image is bound as `t0` for the one draw and the game's `t0` is bound again afterwards.
* `CharaIdentify.{h,cpp}`: log lines that work with `PreviewAA` alone: the table at the first draw and at the first preview
  activity, activity start and end with counts per draw kind, one line per distinct draw signature (kind, API, target entry
  and size, depth entry, sampled preview entry, shader roles). A signature is **not** a window name: the game gives no window
  identity at this level.
* `CharaMenu.cpp`: the card, with the community build's visible strings verbatim.
* `CharaDlssApi.h` + `CharaDlssStub.cpp`: the fixed interface of the DLSS path and its inert placeholder.
* Hooks and the frame callback attach in fork-owned files only: `ext/ffxiv/lighting/FfxivLightingCapture.h` (same Detours
  transaction as the existing hooks) and the `ext::dx11bridge::AfterSharedCopy` dispatcher in `ext/Seams.cpp`.

### Differences from the community build (deliberate)

* The diagnostics that write files are not reproduced as they were: the community build writes six texture dumps (about 35 MB per
  phase) into the game folder after **every** successful DLSS evaluation, behind no configuration key. When the DLSS path
  arrives this behaviour will sit behind a diagnostics key that defaults to off.
* HDR replays of a draw (the HDR preview mask, the scene replays) go through the draw lambda the preview layer wraps; here
  they are marked and skipped, so a replay is neither counted nor filtered twice. The HDR UI-mask replay of a UI draw
  therefore sees the native image, not the filtered one; the mask is built from alpha, which the filter never changes.
* A UI draw only counts as a consumer when it samples a preview image (the community build lets every UI-set draw reach the consumer
  branch and decides there).
* The check of the executable is stricter than the community build's (build match, single pattern hit and the documented RVAs together).

### Tests (`tests/ext/ffxiv/chara/`)

`chara_table` (scan, PE sections, manager parse, zeroing, cache), `chara_decision` (classifier rules and an exhaustive
combination check), `chara_slots`, `chara_jitter` (exact Halton values), `chara_tags` (lists, no overlap, role bits, private
data on a fake shader), `chara_config` (defaults, ini round trip, ini fragments carry the keys), `chara_accessor` (fail-open:
in a process that is not the matched build the feature goes inert with one log line and reads nothing), `chara_fxaa` (class
`warp`: the real shader through the real pass: alpha rule, flat early-out and the contrast threshold, edge smoothing, 2x
output size, sRGB views, the game's pipeline state restored, the swapped `t0` seen by the draw and restored) and
`chara_enhance` (class `warp`: the real integration with a fake table, including every case that must stay native).

### Unknown / not established

* Which windows use the preview renderer (character, equipment, glamour, portrait editor, adventurer plate) and which of the
  recognised shaders belong to which window **[U]**. The community build's own notes list portraits as still to do.
* The meaning and sign of `Camera[58].zw`, the exact encoding of the game's motion output and whether the engine's depth is
  conventional or reversed **[U]** (the DLSS path decides the depth convention at run time).
* What the global "disabled" byte at OptiScaler state `+0x930` is **[U]**; it is not used here.
* Whether the equipment display shader fetches texels with `Load` instead of `Sample` **[U]**: with the 2x output a shader that
  uses integer texel coordinates would show only a quarter of the image. `PreviewUpscale=false` is the switch if the equipment
  preview looks zoomed.

### What to verify in the game

1. `PreviewAA=true`, open a character window with an equipment preview: `OptiScaler.log` should contain `equipment display shader tagged`
   (written when the shader is created, so first), then the accessor line (`CharaView native lookup: verified accessor
   available true ...`) at the first draw after the option is on, `shaders tagged so far: ...`, the table lines (576 x 960 entries once a preview exists), `preview render activity started`, draw signature lines,
   `native equipment output matched` and `spatial AA: 576x960 -> 1152x1920, processed draws 1`.
   If the table lines show 576 x 960 entries but no activity line ever appears while a preview is open, the comparison of the
   table's texture pointer with the D3D11 texture does not match in this build (the entry's `+0x68` value is assumed to be the
   native texture pointer); send the log.
2. The equipment image should look smoother on diagonal edges and not zoomed or cropped; transparent edges stay as they were.
3. Close and reopen the window: `preview render activity ended` / `started` lines; no device-removed or flicker.
4. `PreviewUpscale=false`: same image at 1x.
5. A game patch: the log must show exactly one line `FFXIV character preview: switched off for this session (...)` with the
   reason (preceded by a more detailed warning when the scan itself failed), the menu card says the same, and the preview stays
   native.

### F5 DLSS core: isolated DLSS for the preview (`PreviewDLSS`)

Source files: `ext/ffxiv/chara/CharaDlss*.{h,cpp}` (gate, slots, finish step, NGX composition), `CharaMotion*`, `CharaCamera*`,
`CharaGuides.*` with `shaders/chara_guides.hlsl`, `CharaDiag*`. The behaviour matches the description in words and formulas below.
Credit: [ATTRIBUTION.md](ATTRIBUTION.md).

What it does, per preview render:

1. A UI draw that samples a preview output within 250 ms of its finisher claims a slot (up to 16). Only then is the preview
   jittered, replayed and upscaled, and the very first evaluation of a slot is not jittered.
2. Depth prepass and G-buffer draws run with the viewport shifted by a Halton (2, 3) jitter over 32 phases. The opaque
   G-buffer draws are replayed once into a private RGBA16F target with render target 3 only and depth writes off, which keeps the
   signed motion that the game's 8-bit target cannot hold. The replay is skipped while an occlusion or statistics query is
   open, with unordered access views or stream-out targets bound, on multisampled or non-opaque targets, and for pixel
   shaders that are neither tagged nor of the exact G-buffer shape.
3. The pixel-shader constant buffer 2 of the first G-buffer draw (59 vectors) is copied; the helper pass subtracts its last
   vector's zw from the motion, maps non-finite values to zero and clamps to +-2. Depth is read through a 24-bit view.
4. After the finisher the native image becomes the colour input, an NGX super resolution feature (render size W x H, output
   2W x 2H, MaxPerf, flags 0x42 or 0x4A with a reversed depth buffer, motion at render resolution, automatic exposure) is
   evaluated through the driver's D3D11 NGX entry points, and the native alpha is read back at `uv + jitter / size` into an
   RGBA8 image. UI draws that sample the preview get that image for the one draw.
5. All helper passes and the upscaler run inside a private device context state; the game's state is swapped back.
6. Every failure keeps the native image: a slot failure is counted and sticks, a feature creation failure ends the path for
   the session, and a latched path releases its slots (after the GPU finished with them) before it goes quiet.

Differences from the community build: the diagnostics are off by default and capped (12 files, 100 MiB, `PreviewGuideCapture`);
the finish step also waits for the camera copy; the copy-chain trigger is not implemented (the dispatcher treats a UI draw that
samples a table entry as the consumer); the NGX coverage counters of the exported shim are not reproduced (no seam without
touching the upstream shim).

Not established: whether the game's NGX device is the immediate context's device under the DX11-on-DX12 bridge, whether a
D3D11 feature can be created while the bridge's own feature exists, the meaning and sign of the camera offset (the log prints
it with `PreviewGuideCapture`), and the window coverage (portraits).

## Preview Neural Rendering: not part of the 2026-10-13 binary

`[FFXIV] PreviewNR`, `PreviewNRLingerSeconds` and `PreviewNRLate` and the module behind them (`ext/ffxiv/chara/CharaNr*.{h,cpp}`, the small
`ext/nr` seam) are not taken from the community build. That build only carried a trace that records which draws read the
preview images (the "preview NR trace"); it contains no NR call for previews, and its menu said "Preview NR and FG are not active".
SirenBrink's v6.0 source (`fe208a59`) has a preview NR of its own that works differently: NR runs at render size, its result is added after DLSS, and a `PreviewNRReuse` key exists. The fork's modes use a different mechanism: the bring-up (NR on the native preview image) and the production mode (NR on the Super Resolution output, between the
Super Resolution evaluate and the alpha pass, with copies of the preview's depth and motion). The cyan debug tint (`PreviewDebugTint` for an image
NR produced) is the fork's own as well; the magenta tint of the Super Resolution image is the community build's. What the production mode still needs from a game session
is listed in FEATURES.md (state tracking of the shared inputs under the debug layer, the model's handling of render-size guides under an
output-size colour image, image quality on the alpha-masked silhouette, cost with several windows).
The design, the reasons for running the model on the Direct3D 12 side of the bridge, and the independence of the NR and
Super Resolution paths are described in the feature section of FEATURES.md. The debug probe (`PreviewProbe`, `PreviewDebugFlood`,
`chara-probe.cmd`) is also the fork's own work and has no counterpart in that build.
`[FFXIV] PreviewNRBeforeSR` (a third placement, NR on the render-size picture before the Super Resolution evaluate, off by default) is the fork's own work in the same way: the community build has no key or code for it.

**Jitter (CharaView, deliberate difference).** The preview's jitter cycle is 32 phases (`kJitterPhases`, derived from the fixed 2:1 preview scale by `RecommendedPhases`), where the community build's string-level reading suggests 16; the main scene is unchanged (the game's own generator never repeats in its DLSS mode). The read-only game-jitter probe and the experimental jitter correction are fork additions (`[FFXIV] JitterProbe`, `JitterCorrection`, both off by default; see FEATURES.md).
