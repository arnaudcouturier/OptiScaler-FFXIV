# After an FFXIV patch

What to do when a game patch changes `ffxiv_dx11.exe`. Some of the fork's FFXIV features read the game's memory or hook its functions at fixed addresses. A patch moves those addresses; the fork notices and switches those features off instead of guessing. This runbook says how the check works, which addresses exist, how to find them again in the new executable, and how to prove the new values are right.

Nothing in this document needs the game's source. Everything is recovered from the executable with a disassembler or decompiler (Ghidra, or the REA tools with a Ghidra back end).

## 1. How the build identity guard works

All game-build-dependent values are in one file, `OptiScaler/ext/ffxiv/GameBuild.h`; `GameBuild.cpp` has the matching code. There are three layers.

1. **Image identity.** `GameBuild` holds the PE `TimeDateStamp` and `SizeOfImage` of the supported executable. `MatchBuild()` compares them with the running process; `MatchCurrentProcess()` reads the running executable's own headers and calls it. `ext::game::Current()` (`OptiScaler/ext/game/Game.cpp`) does the same walk once at start-up. A mismatch means every game-module pointer stays null and every FFXIV feature stays off (section 6). This layer is silent: the only sign is that the log line `Matched supported FFXIV build for game-module seams` is missing.
2. **Byte guards.** Before any hook is installed, the code reads the bytes at each hook target and compares them with the expected prologue (`settingsInstructions`, `querySizeInstructions`, `rendererUpdateInstructions`, `resizeCallbacksInstructions`, `batchInstructions`, `enqueueInstructions`, `bindStateInstructions` in `GameBuild.h`). Any difference logs a warning and installs nothing (fail closed). The guards are what catch a patch that keeps the image size by chance, or a function that moved by a few bytes.
3. **Plausibility checks at run time.** Pointer reads are bounds-checked and structures are range-checked (queue sizes, counts); a failed read makes that frame fall back to the native path. The one pointer that has no byte guard, the nameplate module global, is logged once at start-up so you can see whether the chain resolves (section 4).

**Fail open.** In every case the game keeps running with the fork's FFXIV features disabled, as if the fork were plain OptiScaler: no crash, no hook, no memory read outside the guarded paths. The disk hash of the executable is not checked at run time; the identity is the PE header plus the byte guards.

## 2. Single build, not a table

`GameBuild.h` defines one build, `kCurrentBuild`, and `MatchBuild()` compares only against it. A patch is therefore "replace the values in `kCurrentBuild`": the old build stops being supported. There are 59 direct references to `kCurrentBuild` outside `GameBuild.h` (the Companion code reads `kCurrentBuild.companion.*` in about forty places), and two members, `nativeBatchHeaderSize` and `drawCommandSize`, are used as compile-time `std::array` sizes. Supporting two builds at once would need, in this order:

1. `MatchBuild()` returning the matching entry of a table instead of the single constant.
2. The 59 references going through a cached pointer to the matched entry (`ext::game::Current()` is the natural owner) instead of the constant.
3. The two array sizes moved out of the per-build struct into shared constants (with a `static_assert` that every entry agrees), or the arrays made dynamic.
4. A unit test per entry for the guard arrays.

None of that is done. If you only need to follow the game, edit the values in place.

## 3. Every game address the code uses

All values are for the build recorded in `GameBuild.h` (`TimeDateStamp 0x6ac3dc96`, `SizeOfImage 0x380a000`). RVAs are relative to the image base. "Guard" means a byte array that the code compares before use.

### 3.1 Native quality (`ext/ffxiv/quality/FfxivNativeQualityHooks.h`)

| Name in `GameBuild.h` | Value | What it is | Guard | Used by |
|---|---|---|---|---|
| `settingsInstructionsRva` | `0x374347` | Function body that loads the graphics settings and gates on `+0x54`, `+0x44`, `+0x45`, `+0x55` | 37 bytes | `SettingsGuardOk()`; if it fails, the settings read is disabled |
| `querySizeRva` | `0x3520c0` | Wrapper around the game's DLSS size query (detoured) | 28 bytes | `DoInstall()` |
| `rendererUpdateRva` | `0x2da290` | Renderer update (detoured) | 18 bytes | `DoInstall()` |
| `resizeCallbacksRva` | `0x236a40` | Resize-callback dispatcher (called, not detoured) | 20 bytes (generic prologue) | `DoInstall()` |
| `settingsPointerRva` | `0x28f9470` | Global: pointer to the graphics settings | none | `DlaaRequiredNow()` |
| `managerPointerRva` | `0x28f9498` | Global: manager pointer | none | context lookup |
| `devicePointerRva` | `0x28f2d00` | Global: device pointer | none | resize-callback dispatch |
| `settingsModeOffset` `0x54`, `settingsQualityOffset` `0x44`, `settingsDlaaOffset` `0x45`, `settingsRendererOffset` `0x47`, `settingsAdditionalOffset` `0x49`, `settingsSelectorOffset` `0x55` | | Fields of the settings structure; DLAA is required when `+0x54 == 2` and `+0x45 == 0` | via the settings guard | `DlaaRequiredNow()` |
| `managerContextOffset` `0x4220`, `contextFlagsOffset` `0x181`, `contextCacheOffset` `0x150`, `deviceCallbacksOffset` `0x30`, `rendererSceneSizeOffset` `0x428` | | Manager to context, context flags and cache, device callback array (critical section at `+8`, count at `+0x38`), renderer scene size | none | live quality change and renderer update |

### 3.2 Companion / nameplates (`ext/ffxiv/companion/`)

| Name | Value | What it is | Guard | Used by |
|---|---|---|---|---|
| `batch16Rva`, `batch32Rva` | `0x6e5570`, `0x6e6110` | The two UI batch functions (16-bit and 32-bit index buffers), detoured | the same 16 bytes at both | `CompanionNative.cpp` |
| `enqueueRva` | `0x240bc0` | Command append (detoured) | 16 bytes | `CompanionNative.cpp` |
| `bindStateRva` | `0x22d7f0` | Draw-state bind (detoured) | 16 bytes | `CompanionNative.cpp` |
| `nameplateModulePointerRva` | `0x2aa35c0` | Global: UI module pointer; the queue is `[[[g] + 8] + 8]` | **none** (medium confidence) | `CompanionNative.cpp`, `CompanionPackets.h`, `replay/FfxivNameplatePackets.h` |
| `layoutSlotTableRva` | `0x217bc00` | Read-only table: vertex slot per layout, 8-byte entries, low dwords `0,0,0,0,1,2,2,1,1` | none (pattern checked by hand) | `CompanionPackets.h`, `CompanionVisibilityQueue.h` |

Structure offsets, all read by the Companion code; none has its own guard:

- Renderer: `rendererBatchListOffset` `0x580`, `rendererBatchCountOffset` `0x588`, `rendererIndexBufferOffset` `0x1e0`, `rendererVertexAllocationOffset` `0x570`, `rendererVertexStrideOffset` `0x500`, `rendererVertexOffsetOffset` `0x504`, `rendererVertexSlotStride` `0x10`.
- UI module and state: `moduleStatePointerOffset` `0x08`, `stateQueuePointerOffset` `0x08`, `resourceWrapperOffset` `0x68`, `nativeContextOffset` `0x17b8`, `drawStateCommandBackPointerOffset` `0x20`.
- Index buffer: `indexBufferCapacityOffset` `0x38`, `indexBufferFlagsOffset` `0x40`, `indexBufferCpuStartOffset` `0x60`, `indexBufferCpuEndOffset` `0x68`.
- Queue: `queueHeadOffset` `0x50`, `queueRemainingOffset` `0x68`, `queueBlocksOffset` `0x6c`, `queueStridesOffset` `0x34`, `queueNextBlockOffset` `0x3f0`, entry size `nativeBatchEntrySize` 16.
- Batch header (size `nativeBatchHeaderSize` 64): kind `0x00`, extension `0x04`, wrapper `0x10`, count `0x20`, vertices `0x28`, format `0x30` (low four bits are the layout); entry pointer `0x08`.
- Draw command (`drawCommandType` 6, size `drawCommandSize` `0xb0`): type at `0x00`, `baseVertexOffset` `0x08`, `startIndexOffset` `0x14`, `indexCountOffset` `0x18`.
- Declared but not read by any code today: `rendererQueuesOffset`, `rendererQueueCount`, `rendererQueueStride`, `drawStateFlagOffset`. They are kept as documentation of the structure.

### 3.3 Things that are not addresses but break on a patch

Shader fingerprints. Several features recognise the game's shaders by size and CRC-32 of the compiled bytecode (and, for the lighting scan, by a 16-byte checksum). A patch that recompiles those shaders silently disables the feature, and the image-identity guard cannot see it. The tables are in:

| File | What it identifies | Feature that goes inert |
|---|---|---|
| `OptiScaler/ext/ffxiv/lighting/FfxivLightingCapture.h` (`shaders`, 9 entries: tone mapping, tone adjust, luminance measure and adapt, bright-pass, tone-map LUT) | exposure and tone-mapping stages | lighting scan, and NR history rejection from it |
| `OptiScaler/ext/hdr/scene/FfxivSceneHdr.h` (`Stage()`) and the RenoDX shader copies under `OptiScaler/ext/hdr/shaders/renodx/` (their file names carry the CRCs) | the five scene and post stages | RenoDX scene HDR |
| `OptiScaler/ext/hdr/FfxivHdrPreview.h` (`Tag()`), `OptiScaler/ext/ffxiv/ui/UiShaderCatalog.h` (`kUiCrcs`, `SceneStageOfCrc`) | the seven UI-only pixel shaders and the scene stages | HDR UI transmittance, and the HUD-less boundary detector |
| `OptiScaler/ext/ffxiv/companion/CompanionGpu.h` (`Identify`), `replay/FfxivNameplateRenderer.h` | the nameplate pixel shaders per layout | Companion nameplate replay |

How to refresh: the game's shaders are created through `ID3D11Device::CreatePixelShader`; the fork already computes the CRC at creation. The HUD-less diagnostics (`[HudLess] Diagnostics = true`) write one row per distinct pixel shader, with its CRC, size and stage, to `shaders.csv` in a run folder under `<game folder>\OptiScaler\hudless-diag\`; compare it against the tables. Retag by replacing the numbers; the stage meaning of each entry is in the comments next to it.

## 4. Re-deriving the addresses

Tools: a current Ghidra release (headless works; the previous remap used 12.1), or REA with its Ghidra back end. Import the **new** `ffxiv_dx11.exe` as a PE x64 with the default analysers and let auto-analysis finish; for this 50 MB image that takes a long time, and some tool deadlines will expire before it does, so prefer a headless import into a private project folder. Keep the **old** executable (or its notes) next to it, because the method is "find the same function again", not "search from scratch".

### 4.1 Read the PE identity

```powershell
# prints TimeDateStamp, SizeOfImage and the section table of any PE file (read-only)
$b = [System.IO.File]::ReadAllBytes('<new ffxiv_dx11.exe>')
$pe = [BitConverter]::ToInt32($b, 0x3c)
'TimeDateStamp = 0x{0:x8}' -f [BitConverter]::ToUInt32($b, $pe + 8)
'SizeOfImage   = 0x{0:x}'  -f [BitConverter]::ToUInt32($b, $pe + 24 + 56)
$n = [BitConverter]::ToUInt16($b, $pe + 6); $sec = $pe + 24 + [BitConverter]::ToUInt16($b, $pe + 20)
0..($n - 1) | ForEach-Object { $o = $sec + 40 * $_
  '{0,-8} VA 0x{1:x8} size 0x{2:x8} raw 0x{3:x8}' -f [Text.Encoding]::ASCII.GetString($b, $o, 8).TrimEnd([char]0), [BitConverter]::ToUInt32($b, $o + 12), [BitConverter]::ToUInt32($b, $o + 8), [BitConverter]::ToUInt32($b, $o + 20) }
```

### 4.2 Find each guarded function again

Most guards survive a patch unchanged except for their RIP-relative displacements, so scan the new `.text` for the old guard with those displacement bytes wildcarded. Only the function bodies shift; the guard prologues are short and usually identical. Save the snippet below as `scan.ps1`.

```powershell
# usage: .\scan.ps1 -Exe <exe> -Pattern '4c 8b 05 ?? ?? ?? ?? 45 33 e4 41 80 78 54 02'
param([string]$Exe, [string]$Pattern, [string]$Section = '.text')
$b = [System.IO.File]::ReadAllBytes($Exe)
$pe = [BitConverter]::ToInt32($b, 0x3c); $n = [BitConverter]::ToUInt16($b, $pe + 6); $sec = $pe + 24 + [BitConverter]::ToUInt16($b, $pe + 20)
foreach ($i in 0..($n - 1)) { $o = $sec + 40 * $i
  if ([Text.Encoding]::ASCII.GetString($b, $o, 8).TrimEnd([char]0) -eq $Section) { $va = [BitConverter]::ToUInt32($b, $o + 12); $size = [BitConverter]::ToUInt32($b, $o + 8); $raw = [BitConverter]::ToUInt32($b, $o + 20) } }
$pat = $Pattern -split '\s+' | ForEach-Object { if ($_ -eq '??') { -1 } else { [Convert]::ToInt32($_, 16) } }
$end = [Math]::Min($raw + $size, $b.Length) - $pat.Count
for ($p = $raw; $p -le $end; $p++) {
  if ($b[$p] -ne $pat[0]) { continue }
  $ok = $true; for ($k = 1; $k -lt $pat.Count; $k++) { if ($pat[$k] -ge 0 -and $b[$p + $k] -ne $pat[$k]) { $ok = $false; break } }
  if ($ok) { 'RVA 0x{0:x}' -f ($va + $p - $raw) } }
```

It takes about a minute on this image. Expect exactly one hit for each guard except `resizeCallbacks` (its prologue is generic) and the batch pair (exactly two hits, one for each function). Zero hits or many hits mean the function changed: go to the decompiler.

### 4.3 Per-symbol method

| Symbol | Recognise it by | Cross-check |
|---|---|---|
| `settingsInstructionsRva` | Unique 37-byte guard. The first instruction is `mov r8, [rip+disp]` loading the settings global; then tests of bytes `+0x54`, `+0x44`, `+0x45` and a load of `+0x55` compared against 6 | `instruction address + 7 + disp` is `settingsPointerRva` |
| `settingsPointerRva` | Target of that `mov` | Read by the size query (`+0x54 == 2`) |
| `querySizeRva` | Unique 28-byte guard; decompiles to: manager global, `+0x4220` to the context, `settings+0x54 == 2`, copy of three size values out of the context | `address + 21 + 7 + disp` of the `mov rax,[rip+disp]` at `+21` is `managerPointerRva` |
| `managerPointerRva` | Target of that `mov` | Context `+0x4220` is non-null in a running game |
| `rendererUpdateRva` | 18-byte guard; several hits for the fixed bytes, the right one calls `resizeCallbacksRva` and reads settings `+0x54/+0x44/+0x45/+0x47/+0x49`; it has a single caller | The displacement in the guard points at the stack cookie, not at a game global: do not use it to find anything |
| `resizeCallbacksRva` | The prologue is generic (more than a hundred hits). Identify by behaviour: enter a critical section at `param+8`, walk the callback array at `+0x30` (count at `+0x38`), call each, leave the section; called from the renderer update | Called by exactly the function above |
| `devicePointerRva` | A global loaded by the command-append function (`enqueueRva`) before an atomic add on `[device+0xd4]`; referenced from over a thousand places | Same relative shift as the settings globals in the same section |
| `batch16Rva`, `batch32Rva` | The shared 16-byte guard has exactly two hits in `.text`. The one that locks an index buffer as 16-bit values is `batch16`; both read the renderer list at `+0x580/+0x588`, the table at `layoutSlotTableRva`, and are called from the same dispatcher, which chooses by the index-buffer format | By address order if the decompiler cannot tell: in the supported build the 16-bit one comes first |
| `enqueueRva` | Unique 16-byte guard; decompiles to: context index at `+0xc`, key at `+8`, a per-group counter at `ctx + index*0x18 + 0x28`, refill through the device global, write of a 16-byte (key, command) entry | Also called when an indexed queue command (type 6, size `0xb0`) is appended |
| `bindStateRva` | Unique 16-byte guard; tests the byte at `state+0x49` and uses the native context at `+0x17b4/+0x17b3/+0x17b8` | |
| `nameplateModulePointerRva` | **No guard.** A bss pointer global with hundreds of references such that `[[[g] + 8] + 8]` is the queue the batch functions fill (the queue object has the head at `+0x50`, remaining at `+0x68`, blocks at `+0x6c`, next-block pointer at `+0x3f0`). Do **not** assume the old delta: in the previous patch this one moved by a different amount than the neighbouring globals, and the naive address had no references at all | Run the game and read the log line `Companion native submissions: queue chain module=... renderer=... queue=...`; all three must be plausible, non-zero heap or image addresses once you are in the world |
| `layoutSlotTableRva` | The only `.rdata` match of the nine-entry pattern (low dwords `0,0,0,0,1,2,2,1,1` in 8-byte entries), referenced by the batch functions as `table[layout*8]` to pick the vertex slot | |

Struct offsets (the `+0x...` rows) rarely change in a patch, but they are not guarded. Re-verify by decompiling the same functions: renderer list and count, index buffer capacity, flags and CPU pointer, queue head, remaining, blocks, next-block, batch header fields, and the settings fields above. If a function now loads a field from a different offset, edit that member.

### 4.4 Edit and verify

1. Edit `GameBuild.h` only: `timeDateStamp`, `sizeOfImage`, every changed RVA, and, if the new guard bytes differ, the corresponding `std::array` (copy the exact bytes at the new RVA; keep the array length).
2. Build (`tools\build-locked.ps1`) and run the unit tests (`tools\test.ps1`). The tests do not read the game; they only prove the code still compiles and its pure logic is intact.
3. Verify the candidate without the game by re-reading the bytes: for each RVA, check that the bytes at that address in the new executable equal the guard array (`scan.ps1` with the exact bytes, no wildcards, must return the RVA you wrote). Check the three RIP-relative globals with the arithmetic in the table.
4. Verify in game (section 5).

## 5. Verifying a candidate in game

Use a copy of the game folder and the fork build with the edited `GameBuild.h`. Start the game and read `OptiScaler.log`. A correct build shows, in this order:

- `Matched supported FFXIV build for game-module seams`
- no `FFXIV probe: unsupported executable instructions; settings read disabled`
- no `FFXIV native quality: instruction mismatch; hooks disabled`, `... update instruction mismatch; hooks disabled`, `... unable to enlist game threads; hooks disabled`
- `FFXIV lighting: native exposure hooks ready`
- `Companion native submissions: queue chain module=0x... renderer=0x... queue=0x...` with plausible values, then `Companion native submissions: guarded batch hooks ready; substitution off until requested`
- no `Companion native submissions: unsupported executable; no batch hooks installed`, `... batch byte guards rejected; native drawing unchanged`, `... queue hook byte guards rejected`

Then exercise each feature once: change the in-game DLSS quality setting and check the log lines `FFXIV native DLAA gate: ...` follow it; turn nameplates on with the Companion replacement enabled and check they draw; switch on RenoDX scene HDR and HUD-less. The expected struct values are in the decompilation notes above; a wrong offset usually shows as a rejected read in the Companion statistics or a settings read of `0`.

## 6. What happens on a build that does not match

`ext::game::Current()` stays all-null, silently. Each feature then does the following (from the places that test the game-module pointers under `OptiScaler/ext/`):

| Feature | On an unknown build |
|---|---|
| Native quality hooks, DLAA gate | not installed; log warning (`unsupported executable instructions` or `instruction mismatch`) |
| `ForcePerfQuality` | **still applies** (the seam is not game-gated), but without the DLAA gate and without the geometry correction; the render size may then be wrong for this game |
| Render geometry and sub-rectangle correction | off (`geometry` is null) |
| Forced-quality colour scaling | off |
| Lighting scan and NR history rejection from it | off (`lighting` is null); NR itself still runs |
| Frame-marker pacing, FG guide matching | off (`fgMarkers` is null); XeFG and DLSS-G use upstream's frame timing |
| OptiHDR output (`[HDR] FfxivHDR`) and its menu card | off: the request returns false when `hdr` is null; also no scene HDR |
| HUD-less engine | no source (`hudless` is null); the mode has no effect |
| Companion / nameplates | log warning `unsupported executable; no batch hooks installed` |
| Isolated NGX shutdown and the bridge's FFXIV-specific NR behaviour | upstream's default behaviour |
| DLSS-G, XeFG, NR core, OptiScaler itself | work as in plain OptiScaler |

So after a patch that is not yet followed, the game still runs with the fork's generic features; the FFXIV-specific ones are off and the log says which.

## 7. Checklist

1. Patch released; install it, keep a copy of the previous executable.
2. Run the game with the existing fork build; read the log (section 5). If every line is as expected the patch did not change the guarded code and `GameBuild.h` needs only the new `TimeDateStamp`/`SizeOfImage`.
3. If a guard warning appears, find the function again (section 4), edit `GameBuild.h`, build, test, play (section 5).
4. Check the shader fingerprint tables if the lighting scan, scene HDR, HUD-less boundary or nameplate replay stay off although the addresses were accepted.
5. Commit as `[ffxiv] GameBuild.h: rebase onto <game version>` with the old and new `TimeDateStamp`, the evidence for each moved RVA, and what was verified in game. Tag a release candidate (see [SYNC-RUNBOOK.md](SYNC-RUNBOOK.md), section 10).
