# After an FFXIV patch

What to do when a game patch changes `ffxiv_dx11.exe`. The fork finds the game's code addresses by pattern search and checks them against its table of the last verified build ([FFXIV-PATTERN-SEARCH.md](FFXIV-PATTERN-SEARCH.md) explains the mechanism). On patch day you run a command-line tool against the new executable, read what it found, let it write the new table entry for review, build, test and release. No game launch and no disassembler is needed unless a seam fails.

Nothing here needs the game's source. Anything that has to be recovered by hand comes from the executable with a disassembler or decompiler (Ghidra or a comparable tool).

## 1. What happens to players before you do anything

| Part of the fork | On the new, unlisted build |
|---|---|
| Features that identify the game by program name or shader fingerprint: scene HDR, lighting, HUD-less, geometry, markers, mipmap bias, the FFXIV quirks | run, as on any `ffxiv_dx11.exe`. (For these features the one exact build in the table is identified by its PE timestamp and size, whatever the file is called; the program name only decides on a build the table does not know.) |
| Features that read or hook addresses of the executable: native quality, jitter probe, jitter source, the character preview lookup | off by default. With `FfxivUnknownBuildHooks=true` each runs if every address it needs was found |
| DLSS-G, XeFG, NR core, OptiScaler itself | as in plain OptiScaler |

The log block that says which (`[ffxiv-gate]`, `[ffxiv-resolve]`) is described in [FFXIV-PATTERN-SEARCH.md](FFXIV-PATTERN-SEARCH.md). A release that pins the new build in `GameBuild.h` brings the default-key players back to the full, verified fast path. That release is the end point of this runbook.

## 2. The patch-day workflow

1. **Get the new executable and keep a copy of the old one.** Work on copies. The CLI only reads the file.
2. **Build the CLI.** It lives under `tools\ffxiv-resolve\` and is never part of the product. The test runner builds it as part of `ffxiv_resolve_cli`:
   ```powershell
   pwsh -NoProfile -File tools\test.ps1 -Filter ffxiv_resolve_cli
   ```
   For a copy you keep, use the same compile line from a `vcvars64` prompt of the VC tools installation (the one `tools\test.ps1` finds; plain `vswhere -latest` can return a Visual Studio without the C++ tools, so use `-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64`, or set `OPTISCALER_VCVARS`):
   ```
   cl /nologo /std:c++20 /EHsc /W4 /DNOMINMAX /Fo<out>\ /Fe<out>\ffxiv-resolve.exe /I<repo>\OptiScaler /I<repo>\tools\ffxiv-resolve <repo>\tools\ffxiv-resolve\main.cpp
   ```
3. **Run it on the new executable.**
   ```
   ffxiv-resolve "<new ffxiv_dx11.exe>" --strict --margin
   ```
   It prints the image identity, one line per seam in the format of the game's log, the generated entry, the count of unbound fields and, with `--margin`, how far each pattern is from the next-nearest site in the executable. The resolve itself takes a fraction of a second, `--margin` a few seconds more.
4. **Read the status table** (section 3). Every seam should read `Found`. `--strict` exits 2 for anything else.
5. **Generate the new table entry** (section 5):
   ```
   ffxiv-resolve "<new ffxiv_dx11.exe>" --emit-gamebuild OptiScaler\ext\ffxiv\GameBuild.h --out GameBuild.new.h
   ```
6. **Review the "NOT SELF-HEALING / NOT RE-VERIFIED" list** the tool prints (section 5). Those fields still hold the old build's values.
7. **Install the reviewed text as `GameBuild.h`** (a copy over the file, by you; the tool never writes the source), update the tests that pin the table, build, run the full test suite (section 6).
8. **Play-test** with [FFXIV-PATTERN-SEARCH-TEST-PLAN.md](FFXIV-PATTERN-SEARCH-TEST-PLAN.md), then tag a release candidate (section 6).

If a seam fails and you are not ready to write a new anchor, you can still release: the features that need that seam stay off by themselves, the rest of the fork runs. Say which features are off in the release notes.

## 3. Reading the CLI output

One line per seam, the same text the game writes to `OptiScaler.log`:

```
[ffxiv-resolve] quality.querySize Found rva=0x352350 hits=1+1+1 table=0x352350 agree ms=0.127
```

`hits=1+1+1` is what each anchor left after its predicates (a seam has one to three anchors). `table=` shows the table value when the executable is the table's build and `n/a` otherwise. A line that is not a clean `Found` starts with `WARN`. `[ffxiv-resolve] total ...` ends the block (seams, counts per status, scan time, stamp, size, `known=yes|no`).

`known=no` is what a new build shows: the table is never offered to a different build, so there is no cross-check and no `DISAGREE`.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | The report printed and every requested check passed. With `--emit-gamebuild`: the entry was emitted and every bound field was re-verified |
| 1 | Usage error, the file is not a readable x64 PE, the header cannot be read or does not fit the field table, `--out` is the source header or the input executable, or the output cannot be written |
| 2 | `--strict` and a seam is not plain `Found`, or disagrees with the table, or a registry error, or (with `--margin`) an anchor is below its required margin. Also `--emit-gamebuild` refused because a bound seam is unusable |
| 3 | `--compare-table` on the table's own build: a seam disagrees with the table, or the entry resolved without the table differs from it |
| 4 | `--emit-gamebuild --allow-partial` emitted a header with fields that are not re-verified |

When several apply, the worst wins: 1, then 2, then 3, then 4.

### What each status means and what to do

| Status | What it means | In the game | What to do |
|---|---|---|---|
| `Found` | All anchors that could be evaluated agree on one unique value | the seam is usable | nothing |
| `FoundSingleAnchor` | One anchor found it and the others found nothing, or it rests on a weak seam | usable, a WARN in the log | Usable but unconfirmed. Find out which anchor stopped matching (the seam line shows `hits=1+0`) and check the value against the executable: the decompiler, or the second anchor's logic. Either repair the stale anchor or accept the value on purpose. The codegen marks the line `// single-anchor: review before pinning` |
| `NotFound` | No anchor produced a candidate, or a seam it depends on has no value | every feature that needs the seam stays off (the log names it). On the known build a seam that is not a shape is filled from the table | The code was rewritten, or the pattern is too specific for the new compiler output. Find the function by hand (appendix B), write a new anchor in `OptiScaler\ext\ffxiv\resolve\Anchors*.h` with its pinned test, or ship with the feature off |
| `Ambiguous` | More candidates than the seam expects | as `NotFound` | The pattern is no longer unique, or the compiler emitted another copy. Tighten the pattern or add a predicate (a call relation, a position relative to another seam). `deviceSize` is the known sensitive case (below) |
| `Conflict` | Two anchors found different values, or a unique anchor is not among another anchor's candidates | seam inert (no value) | The dangerous class: one anchor now matches the wrong place. Do not pin anything that depends on it. Decompile both candidates, fix the wrong anchor |
| `EntryPatched` | The function's first bytes are a jump (`E9`, `EB`, `FF 25`) | never hooked, also on the known build (it is not filled from the table) | In the file on disk this means the game itself now starts the function with a thunk. Look at it in the decompiler and anchor the real body, or use an anchor inside the function. In memory the same status is what a foreign hook (Dalamud, another injector) produces; that is logged at run time and is not fixable offline |
| `DISAGREE` | The table's build only: a seam resolved to a value different from the table | the table wins, WARN | After an anchor change this means the change broke the seam (or the table is wrong). `--compare-table` exits 3. Fix before releasing |

Other lines you can meet: `registry error: ...` (the anchor definitions themselves are inconsistent: `--strict` exits 2), `ffxiv-resolve: cannot use <file>: ...` (not an x64 PE, exit 1), a note `the image could not be scanned: ...` on every seam (a damaged copy of the file; not a verdict about the patch).

**`jitterSource.deviceSize` and the copy count.** The strict 20-byte pattern for the device output size matches two byte-identical copies of a template instance (`0xd3f73` and `0xd4013`), and the seam expects exactly two, taking the first. A compiler that emits a third copy makes the seam `Ambiguous` and the jitter source goes off; a second copy that disappears does the same. If it happens, check in the decompiler that all the copies are the same template instance reading the pair at `+0x8c` and `+0x90`, then change `expectedHits` in `AnchorsJitter.h` together with its pinned test, or give the anchor a disambiguating predicate.

**Shader catalogs cannot be checked here.** The shader CRC tables are content-addressed and the game's shaders are not in the executable. After a patch the log line `[ffxiv-catalog]` is the only evidence (appendix C).

## 4. After the new build resolves: what the table still needs from you

The search covers 53 seams and 51 fields of `GameBuild.h`, all the Rva and Guard fields. It does not cover the fields in `GameBuild.h` that the executable's code does not let it derive: 13 struct offsets (the CharaView entry offsets, the jitter object layout and others), 17 constants (sizes, counts, limits, and two sanity constants the jitter source checks live) and the CharaView anchor pattern. They keep the values the table already had. Section 5 explains how the tool lists them.

## 5. `--emit-gamebuild`: the new table entry

```
ffxiv-resolve <exe> --emit-gamebuild <GameBuild.h> [--out <file>] [--allow-partial]
```

What it does:

- It resolves the executable without the table (the pure pattern-search answer: no cross-check overwrite, no fallback to the table) and reads the given `GameBuild.h`.
- It writes a **copy** of the header text with every bound field rewritten: each Rva, each struct offset a seam decodes, each guard byte array (same length), `timeDateStamp` and `sizeOfImage`. Comments, line endings and layout stay as they are. A value that is already right keeps the spelling it had.
- The copy goes to `--out`, or to stdout. The source is never written, and `--out` may not name the source file or the input executable. Everything the tool says (the seam table, the changes, the review list) goes to stderr, so `> GameBuild.new.h` captures only the header.
- **It refuses** (exit 2, nothing written) if the seam of any bound field is not usable on this executable, and lists the unresolved seams and the fields they fill. `--allow-partial` emits anyway: the unresolved fields keep their old text and get a trailing `// NOT re-verified for this build` (exit 4). A value found by one anchor, or one that rests on a weak seam, is written and marked for review.
- A header that does not fit the field table (a bound member missing, an array whose length or literal count differs from the table, an initialiser that is not one integer literal) is an error, exit 1.

The report has three parts you must read.

1. **The seam status table** (every seam, its status, its value, how many candidates each anchor left) and **the values that changed** against the input header, one line each with old and new.
2. **`== NOT SELF-HEALING / NOT RE-VERIFIED ==`.** The fields the resolver cannot derive, with the values that stand in the output: the 13 fixed struct offsets, the 17 constants, the CharaView pattern and mask, and every bound field whose seam failed (partial only). None of these was checked against the new executable. A patch that moved one of them is invisible to the tool. Check them by decompiling the code that reads them (appendix B names the functions), or by running the game and reading the log (the offsets are range-checked at run time, and the consumers refuse a read that fails). The jitter source offsets, `contextCacheOffset` and `targetsOffset` are written from the decoded instruction, but the strict shape that carries the offset also pins it: if a patch moved one, the shape seam fails and the codegen refuses, so a silently wrong value cannot come out of that path.
3. **`== SHADER CATALOGS: NOT VERIFIABLE OFFLINE ==`.** The CRC catalogs (UI, scene, lighting, chara, nameplate) cannot be checked against the executable: only the `[ffxiv-catalog]` line of the first in-game launch shows them. A healthy known build reads `UI 6/7` and `UI boundary ok`.

The header's comments are copied unchanged. Anything that quotes the old build's addresses, the executable's SHA-256 or the game version (the `JitterBuild` and `JitterSourceBuild` notes, the CharaView accessor note) is stale; the tool lists the comment lines that still quote the old value of a changed field. Edit them by hand.

## 6. Install, build, test, release

1. Review `GameBuild.new.h` against the old file (`git diff --no-index OptiScaler\ext\ffxiv\GameBuild.h GameBuild.new.h`). Every changed line should be explainable: addresses move with the patch, guard arrays change only where a RIP-relative displacement sits in them.
2. Copy it over `OptiScaler\ext\ffxiv\GameBuild.h`, fix the stale comments, and add the new executable's SHA-256, size and game version to the comment block.
3. Update the tests that pin the table. `tests\ext\ffxiv\build\game_build.cpp` writes every RVA, offset and guard of the current build as a literal, by design, so a wrong edit fails it. Other tests name members or values of the header (chara, jitter and the resolver tests that compare the entry text with the table). Run the full suite once; two of them were found only by the full run after the 2026.10.16 patch. Three more things carry build-C numbers on purpose and need the new ones. First, the real-executable tests (`resolve_golden_exe`, `gate_real_exe` and the `ffxiv_resolve_cli` runner) read `..\analysis\patch-20261016\ffxiv_dx11.exe`: point them at the new executable (their `Args` and the runner's `$gameExe`), or keep the old one as data and change what they expect. Second, `run_ffxiv_resolve_cli.ps1` pins the stamp `0x6ad10bfd`, the accessor's file offset `0x269fd0`, and the values `0x26abd0`, `0x290e490` and `0x290e498` of its negative control; these must be re-derived for the new build. Third, `resolve_emit_gamebuild.cpp` pins `0x290e498`, the `0x26abd0 -> 0x26ab40` line and a stale-comment case that relies on a header comment quoting `0x2ebd90`. Tests that plant their own committed bytes (the build-B and build-C fixtures) stay valid unchanged.
4. Build (`tools\build-locked.ps1`) and test (`tools\test.ps1`; the full suite takes 6 to 40 minutes depending on other builds running). Check `tools\footprint.ps1` against the value before the change: the table edit is under `OptiScaler\ext\` and must not move it.
5. Rebuild the CLI (the table is compiled into it) and re-run it against the new executable with `--strict --compare-table --margin`. It must exit 0 and say `the generated entry equals the table`. That proves the table you pinned is what the search finds.
6. Play-test with [FFXIV-PATTERN-SEARCH-TEST-PLAN.md](FFXIV-PATTERN-SEARCH-TEST-PLAN.md) (runs 1 and 3 on the new build, default ini).
7. Commit as `[ffxiv] GameBuild.h: rebase onto <game version>` with the old and new `TimeDateStamp`, what changed, what was reviewed by hand and what was verified in game. Then create the release candidate's tag locally and package from a build made after it, because the tag name is embedded in the DLL (see [SYNC-RUNBOOK.md](SYNC-RUNBOOK.md), section 10, and [RELEASE.md](RELEASE.md), section 1).

`GameBuild.h` holds one build (`kCurrentBuild`): the previous build is no longer in the table after the pin. It still runs through the unknown-build path (with `FfxivUnknownBuildHooks=true`) if its seams resolve.

## 7. Build identities and history

| | Game version | `TimeDateStamp` | `SizeOfImage` |
|---|---|---|---|
| Current (build C) | 2026.10.16 | `0x6ad10bfd` (2026-10-15 17:23 UTC) | `0x381f000` |
| Previous (build B) | not recorded | `0x6ac3dc96` | `0x380a000` |

Build C: executable sha256 `46A52AB1DBE80B9A8D6C4D4531BF66693904200C5135FACACD053EF28A5DE14A`, 51,976,960 bytes. Build B no longer exists on disk. What survives of it is a set of byte strings that were read from it in earlier work; the offline tests use them as the second build.

B to C: every address moved. The globals by `+0x15000`, the nameplate module global by `+0x15090` (a different amount than its neighbours, and its naive address had no references), code by `-0xa0` to `+0x320`. Every guard was found again, and only the three guards that carry a RIP-relative displacement changed their bytes (settings, query size, renderer update). **RVA reuse trap:** an RVA of the old build can be a different function in the new one (`0x25f610` was the camera update in B and is the camera getter in C). Never carry an old RVA into a new anchor.

Image identity is checked by the PE `TimeDateStamp` and `SizeOfImage` (`MatchBuild()`, `MatchCurrentProcess()`, and `ext/game/Game.cpp` at start-up); the disk hash is not checked at run time.

## Appendix A. What each field of `GameBuild.h` is

RVAs are relative to the image base. "Guard" is a byte array the hook code compares with the live bytes before it installs anything. The values are for build C.

### A.1 Native quality (`ext/ffxiv/quality/FfxivNativeQualityHooks.h`, seams `quality.*`)

| Field | Value | What it is | Guard | Used by |
|---|---|---|---|---|
| `settingsInstructionsRva` | `0x3745d7` | Function body that loads the graphics settings and gates on `+0x54`, `+0x44`, `+0x45`, `+0x55` | 37 bytes | `SettingsGuardOk()`; if it fails, the settings read is disabled |
| `querySizeRva` | `0x352350` | Wrapper around the game's DLSS size query (detoured) | 28 bytes | `DoInstall()` |
| `rendererUpdateRva` | `0x2da5b0` | Renderer update (detoured) | 18 bytes | `DoInstall()` |
| `resizeCallbacksRva` | `0x2369a0` | Resize-callback dispatcher (called, not detoured) | 20 bytes | `DoInstall()` |
| `settingsPointerRva` | `0x290e470` | Global: pointer to the graphics settings | none | `DlaaRequiredNow()` |
| `managerPointerRva` | `0x290e498` | Global: manager pointer | none | context lookup |
| `devicePointerRva` | `0x2907d00` | Global: device pointer | none | resize-callback dispatch |
| `settingsModeOffset` `0x54`, `settingsQualityOffset` `0x44`, `settingsDlaaOffset` `0x45`, `settingsRendererOffset` `0x47`, `settingsAdditionalOffset` `0x49`, `settingsSelectorOffset` `0x55` | | Fields of the settings structure; DLAA is required when `+0x54 == 2` and `+0x45 == 0` | via the settings guard | `DlaaRequiredNow()` |
| `managerContextOffset` `0x4220`, `contextFlagsOffset` `0x181`, `contextCacheOffset` `0x150`, `deviceCallbacksOffset` `0x30`, `rendererSceneSizeOffset` `0x428` | | Manager to context, context flags and cache, device callback array (critical section at `+8`, count at `+0x38`), renderer scene size | none | live quality change and renderer update |

### A.3 Game jitter chain (`JitterBuild`, seams `camera.*`)

`cameraGlobalRva` `0x290dba0`: global holding the scene camera holder G; the camera is `[G + 0x3028]`, its 20-byte jitter object `{jx, jy, ndcX, ndcY, counter}` is at `[camera + 0x278]`. No guard: every hop is range- and readability-checked at run time. Read by `FfxivJitterProbe`.

### A.4 Jitter source (`JitterSourceBuild`, seams `jitterSource.*`)

Guard blocks: the jitter generator (`0x2ebd90`), the camera getter (`0x25f610`, returns `[camera+0x278]`), the NGX packet producer call (`0x37411d`, return address `0x374122`), the camera head (`0x25f690`), the phases block (`0x25f721`, 120 bytes, runs up to the strength block), the strength block (`0x25f799`, the generator call, return address `0x25f79e`), and the device size reader (`0xd3f73`). Offsets: settings `+0x4c` scale, `+0x54` mode (2 = DLSS), `+0x78` amount; renderer `+0x428` scene size, `+0x700`/`+0x704` extents; device `+0x8c`/`+0x90` output size. Globals: settings `0x290e470`, renderer `0x290e490`, device `0x2907d00`. Two constants the core assumes, checked live by their seams: `strengthFloorBits` (0.001f) and `ndcSignMaskBits` (`0x80000000`).

## Appendix B. Finding an address by hand

Use this when a seam is `NotFound`, `Ambiguous` or `Conflict` and you need to write a new anchor. Tools: a current Ghidra release (headless works) or another decompiler. Import the new `ffxiv_dx11.exe` as a PE x64 with the default analysers and let auto-analysis finish; for this 50 MB image that takes a long time, so prefer a headless import into a private project folder. Keep the old executable's notes next to it: the method is "find the same function again", not "search from scratch". Never carry an old RVA over.

| Symbol | Recognise it by | Cross-check |
|---|---|---|
| `settingsInstructionsRva` | The first instruction is `mov r8, [rip+disp]` loading the settings global; then tests of bytes `+0x54`, `+0x44`, `+0x45` and a load of `+0x55` compared against 6 | `instruction address + 7 + disp` is `settingsPointerRva` |
| `settingsPointerRva` | Target of that `mov` | Read by the size query (`+0x54 == 2`) |
| `querySizeRva` | Decompiles to: manager global, `+0x4220` to the context, `settings+0x54 == 2`, copy of three size values out of the context | `address + 21 + 7 + disp` of the `mov rax,[rip+disp]` at `+21` is `managerPointerRva` |
| `managerPointerRva` | Target of that `mov` | Context `+0x4220` is non-null in a running game |
| `rendererUpdateRva` | Several hits for the fixed prologue bytes, the right one calls `resizeCallbacksRva` and reads settings `+0x54/+0x44/+0x45/+0x47/+0x49`; it has a single caller | The displacement in the prologue points at the stack cookie, not at a game global |
| `resizeCallbacksRva` | The prologue is generic (more than a hundred hits). Identify by behaviour: enter a critical section at `param+8`, walk the callback array at `+0x30` (count at `+0x38`), call each, leave the section; called from the renderer update | Called by exactly the function above |
| `devicePointerRva` | A global loaded by the command-append function (`enqueueRva`) before an atomic add on `[device+0xd4]`; referenced from over a thousand places | Same relative shift as the settings globals in the same section |
| `cameraGlobalRva`, `cameraOffset`, `jitterPointerOffset` | Search the string `Jitter.Offset.X`; its only user is the game's DLSS evaluate wrapper (RVA `0x3787d0` in build C), which copies two floats of a descriptor. The function that records that descriptor from the scene camera (`0x374050`) loads `[[global] + 0x3028]` and then `[camera + 0x278]` | The one-line getter `0x25f610` returns `[camera + 0x278]`. Check that the object is `{float, float, float, float, int}` with `abs(jx), abs(jy) <= 0.5` |
| jitter source blocks | The camera update (`0x25f690`) contains the only call of the Halton generator (`0x2ebd90`: `index % count + 1`, outputs `x - 0.5`, `y - 0.5`); its output pointer is the jitter object base (`[camera+0x278]`) and the second output is base+4. The native strength is `(amount - 0.001f) * ratio^2 + 0.001f` with `ratio = extent0 / extent2 * scale`; the game doubles it, multiplies by the jitter and divides by the render size to get the NDC translation. The NGX packet producer is the only caller of the camera getter that is followed by the DLSS evaluate descriptor | Dump the bytes of the new executable at each block and compare with the guard arrays |

Struct offsets (the `+0x...` entries) rarely change in a patch. Those without a seam are not checked by any tool: renderer list and count, index buffer capacity, flags and CPU pointer, and the settings fields. Re-verify them by decompiling the same functions. If a function now loads a field from a different offset, edit that member and, if a strict shape carries the old offset, the shape too.

## Appendix C. Shader fingerprints

Several features recognise the game's shaders by size and CRC-32 of the compiled bytecode (and, for the lighting scan, by a 16-byte checksum). The image-identity check and the pattern search cannot see these: a patch that recompiles a shader silently disables the dependent feature, and only the `[ffxiv-catalog]` log line shows it. The tables:

| File | What it identifies | Feature that goes inert |
|---|---|---|
| `OptiScaler/ext/ffxiv/lighting/FfxivLightingCapture.h` (`shaders`, 9 entries: tone mapping, tone adjust, luminance measure and adapt, bright-pass, tone-map LUT) | exposure and tone-mapping stages | lighting scan, and NR history rejection from it |
| `OptiScaler/ext/hdr/scene/FfxivSceneHdr.h` (`Stage()`) and the RenoDX shader copies under `OptiScaler/ext/hdr/shaders/renodx/` (their file names carry the CRCs) | the five scene and post stages | RenoDX scene HDR |
| `OptiScaler/ext/hdr/FfxivHdrPreview.h` (`Tag()`), `OptiScaler/ext/ffxiv/ui/UiShaderCatalog.h` (`kUiCrcs`, `SceneStageOfCrc`) | the seven UI-only pixel shaders and the scene stages | HDR UI transmittance, and the HUD-less boundary detector |

`OptiScaler/ext/ffxiv/catalog/CatalogTable.h` lists the same CRCs per catalog for the health log. How to refresh after a patch: the game's shaders are created through `ID3D11Device::CreatePixelShader`; the fork already computes the CRC at creation. The HUD-less diagnostics (`[HudLess] Diagnostics = true`) write one row per distinct pixel shader, with its CRC, size and stage, to `shaders.csv` in a run folder under `<game folder>\OptiScaler\hudless-diag\`; compare it against the tables. The `[ffxiv-catalog] near-miss` block and its `candidate` lines name uncatalogued shaders with the same structure as a missing one. Retag by replacing the numbers; the stage meaning of each entry is in the comments next to it.
