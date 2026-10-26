# FFXIV pattern search

How the fork finds the game's code addresses after a game patch, what it does when it cannot, the three `[FFXIV]` keys that control it, the log lines that show what happened, and the limits. For the maintainer's patch-day steps see [FFXIV-PATCH-RUNBOOK.md](FFXIV-PATCH-RUNBOOK.md); for the in-game checks that have not been done yet see [FFXIV-PATTERN-SEARCH-TEST-PLAN.md](FFXIV-PATTERN-SEARCH-TEST-PLAN.md).

## What it is

Some FFXIV features read the game's memory or hook its functions at addresses inside `ffxiv_dx11.exe`: the native quality selection and the DLAA gate, the jitter probe and the jitter source fix, the companion / nameplate native hooks, and the character preview lookup. A game patch moves those addresses. Before the pattern search, the fork knew the addresses of one build (`ext/ffxiv/GameBuild.h`, `kCurrentBuild`) and switched every FFXIV feature off on any other executable.

Now the fork looks for each address in the running executable by instruction patterns, once per game start. If the executable is the build in the table, the table is used and the search only cross-checks it. If it is a new build, the search result is used, for the features that need addresses, only when you ask for it (`FfxivUnknownBuildHooks`) and only for the features whose addresses were all found. A feature whose code was rewritten in the patch goes off by itself with its reason in the log. The others keep running.

The features that identify the game by program name or by shader fingerprint (scene HDR, lighting, HUD-less, geometry, markers, mipmap bias, the FFXIV quirks) never needed an address. They now run on any `ffxiv_dx11.exe`, known build or not.

## How it works

**Resolve once.** The first time the game creates its D3D11 device (about 8 seconds after the fork's DLL loads, on the game's device-creation thread, not in `DllMain`), the fork reads the executable image from memory and runs the search over it. This happens before the fork installs any hook over the game's code. The search is expected to take roughly 170 to 250 ms. That figure comes from offline runs on the real executable (167 ms with `/O2`); it has not been measured inside the game. The sample `total` line under "Log lines" (`ms=300.827`) is from an unoptimised test build, which is why it reads higher than that range. The search runs once, never per frame.

**Seams.** A seam is one thing the fork needs from the executable: a function entry, a global variable, a struct offset, a table. There are 81 seams and 77 bindings, each of which fills one field of `GameBuild.h` (a seam can fill two fields, for example `jitterSource.producerCall`). Each seam has one to three anchors. An anchor is a byte pattern with wildcards (`48 8B 05 ?? ?? ?? ??`) plus the rule that turns a match into the seam's value (the function start from the unwind data, the target of a RIP-relative load, a displacement). Patterns mask the parts that change between builds, the displacements of RIP-relative instructions and the targets of calls, and keep the bytes that carry meaning.

**How the anchors of a seam combine.**

| Anchors | Status | Effect |
|---|---|---|
| Every anchor that found something agrees on one unique value | `Found` | usable |
| Exactly one anchor found it, the others found nothing | `FoundSingleAnchor` | usable, logged as a WARN |
| Two anchors found different values | `Conflict` | seam off |
| The pattern matches more than the expected number of places | `Ambiguous` | seam off |
| No anchor matched | `NotFound` | seam off |
| The function's first bytes are a jump (`E9`, `EB`, `FF 25`) | `EntryPatched` | seam off: another program has hooked it, the fork does not hook on top |

A seam that depends on a weak seam (`EntryPatched` or `FoundSingleAnchor`) is itself capped at `FoundSingleAnchor`.

**Shape seams fail closed.** For every function the fork hooks there are two seams: one finds the address (the location), one checks a long, strict byte sequence at that address (the shape, many more bytes than the old guards, with only the displacements masked). A hook site is usable only when both are usable, that is `Found` or `FoundSingleAnchor` (the location seam of `quality.resizeCallbacks` has two anchors, so it comes out as `FoundSingleAnchor` when one of them stops matching, and that still counts as usable). On a known build a shape that fails is not filled from the table, so a changed function body cannot hide behind the table. The shape's bytes also fill the `...Instructions` guard arrays, and the hook code compares them again with the live bytes before it installs anything.

**Known build: the table, cross-checked.** When the executable's PE timestamp and size equal the table's, the table fills the build the features read. In the fork's game module and gate the exact build is identified by that timestamp and size alone, whatever the file is called (a copy under another name is still the game); there the file name `ffxiv_dx11.exe` only decides on a build the table does not know. Four things outside that rule still need the original file name, as before the pattern search: the installation of the companion native hooks, the per-game entry of the upstream game table (`GameConfigs.cpp`), the frame-pacing trace and the automatic ReShade depth. The search still runs. A value on which the search and the table disagree is logged as `DISAGREE` and the table wins. A seam the search could not resolve is filled from the table with a WARN (not shape seams, not `EntryPatched` ones). The result is the behaviour of the previous release, which tests prove offline for the unmodified executable (every field of the table equal to what the gate publishes, the same 15 gate decisions).

**Unknown build: the search result.** Nothing is taken from the table. Each seam that resolved fills its field; fields no seam fills stay unusable. Every seam a feature reads must be plain `Found` or `FoundSingleAnchor`, and four cross-group checks compare globals that two groups found independently (the CharaView manager pointer with the jitter group's renderer pointer, the camera global, the settings pointer and the device pointer). A mismatch, or a side that was not compared, turns off the features on both sides.

**The gate.** A pure function decides, per feature, on or off and the reason, from the resolver result and the three keys. Group 1 (exe name and shader CRC) has nine features that run on any `ffxiv_dx11.exe`: `sceneHdr`, `lighting`, `uiHudless`, `geometry`, `markers`, `mipBind`, `quirkIsolatesShutdown`, `quirkBridgedNativeDx11`, `catalogHealth`. Group 2 (reads or hooks addresses) has six: `qualityRead`, `qualityHooks`, `jitterProbe`, `jitterSource`, `companionNative`, `charaViewAccessor`. A Group 2 feature needs every seam on its list, so one rewritten function turns off the features that use it and nothing else.

## The three keys

All three live in `[FFXIV]` of `OptiScaler.ini`. The shipped value is `auto`, which is the default shown here. They are read once, when the game creates its first device, so a change needs a game restart. They have no menu control. The explanations are in the ini fragment `OptiScaler/ext/ini/FFXIV.ini`, which `tools/package.ps1` merges into the shipped ini.

| Key | Default | What it does |
|---|---|---|
| `FfxivLegacyGameBuild` | false | Rollback. `true` is exactly the behaviour before the pattern search existed: the build is recognised by its exact timestamp and size, the table is used as it is, nothing is searched, and every feature of the game module (Group 1 too) runs only on that exact build. The other two keys are ignored. As in the old code, the gate identifies the build by timestamp and size only, not by the file name. |
| `FfxivUnknownBuildHooks` | false | Lets Group 2 run on a build that is not in the table, when the search found everything the feature needs. No effect on the known build. |
| `FfxivForceUnknownBuild` | false | Debug. The search runs without the table, as if the game were patched, on today's executable. Group 2 then also needs `FfxivUnknownBuildHooks=true`. |

What runs for each combination (Group 1 means the nine name and CRC features, Group 2 the six address features):

| Legacy | Unknown hooks | Force unknown | Executable | Group 1 | Group 2 |
|---|---|---|---|---|---|
| true | any | any | the table's build | on | on, by the old rules (table and old byte guards) |
| true | any | any | any other build | off | off |
| false | any | false | the table's build | on | on, when the seams of each feature allow it |
| false | false | false | any other build | on | off (the log says why) |
| false | true | false | any other build | on | each feature on only if every seam it needs resolved |
| false | false | true | the table's build | on | off |
| false | true | true | the table's build | on | each feature on only if every seam it needs resolved, from the search alone |

If the resolver itself breaks on the table's build (the image cannot be read, an exception, a scan that aborted), Group 2 falls back to the legacy decision, unless `FfxivForceUnknownBuild` is set. The `[ffxiv-gate]` block carries exactly one WARN for the fallback (`resolver infrastructure failure on the exact known build (...)`); what is printed before it depends on the failure. An image that could not be opened prints nothing else. A scan that aborted still prints the resolver's own WARN line for every seam that did not resolve. A resolver exception prints one `ERROR` line from the resolver (`resolver stopped with an error, nothing resolved`), and the gate block then also carries the `not self-healing` WARN, because that failed result is not marked as a known build. An exception in the startup itself prints an `ERROR` line `[ffxiv-gate] startup failed (...)` just ahead of the gate block. On any other build a broken resolver leaves Group 2 off.

When to use each key:

- `FfxivLegacyGameBuild`: to compare behaviour with and without the pattern search (test plan, run 4), or if the search ever misbehaves on the known build.
- `FfxivUnknownBuildHooks`: after a patch, before a release pins the new build, if you want the address features and accept that this path has not run on a real patch yet. Read the `[ffxiv-gate]` block first and leave it off if a feature you depend on says OFF.
- `FfxivForceUnknownBuild`: to run the whole unknown-build path on the current executable (test plan, run 2).

The default of `FfxivUnknownBuildHooks` stays off until one real game patch has been followed: every Group 2 seam found uniquely, the per-seam log lines matched what the executable holds, and each feature checked in game. After that it flips to on.

## Log lines

Turn on `[Log] LogToFile = true` to get `OptiScaler.log` next to the game. The `[ffxiv-resolve]` and `[ffxiv-gate]` lines are written once, when the game creates its first device; the known build's `Matched supported FFXIV build for game-module seams` line is not written by the device hook. The game selector (`SelectGame`) writes it at the first `ext::game::Current()` call, from wherever that call comes. The device hook calls `Current()` only after the resolver block is written, so the line follows the block unless something asked for the game module earlier; which of the two happens has not been checked. The `[ffxiv-catalog]` lines come later, as described below.

`[ffxiv-resolve]`, one line per seam in registry order, then a total:

```
[ffxiv-resolve] chara.accessor Found rva=0x26abd0 hits=1 table=0x26abd0 agree ms=0.135
[ffxiv-resolve] quality.querySize Found rva=0x352350 hits=1+1+1 table=0x352350 agree ms=0.127
[ffxiv-resolve] total seams=81 found=81 single=0 notfound=0 ambiguous=0 conflict=0 patched=0 scan_ms=288.492 ms=300.827 stamp=0x6ad10bfd size=0x381f000 known=yes
```

`hits=1+1+1` is the number of candidates each anchor left. `table=n/a` means the seam has no table value (a shape seam, a helper seam) or the build is unknown. A clean `Found` is an Info line; every other status and every `DISAGREE` is a WARN, and a seam that was filled from the table says so in its note. The numbers above are from the offline run on the current executable, not from the game, and the times (`scan_ms`, `ms`) are from an unoptimised test build.

`[ffxiv-gate]`, a short block after the resolver lines:

```
[ffxiv-gate] build stamp=0x6ad10bfd size=0x381f000 known=yes forced_unknown=no legacy=no unknown_build_hooks=off exe=ffxiv_dx11.exe resolve_ms=...
[ffxiv-gate] group1 ON (sceneHdr,lighting,uiHudless,...): ffxiv_dx11.exe: identified by name and shader CRC, no executable address is read
[ffxiv-gate] feature qualityHooks ON  all N required checks pass
[ffxiv-gate] feature jitterSource OFF <the first failing seam and why>
[ffxiv-gate] cross-check chara.managerPointer == jitterSource.rendererPointer: agree (0x290e490)
[ffxiv-gate] not self-healing: 29 struct offsets keep their table values on this build (...)
```

A Group 2 feature that is OFF on the known build is a WARN, because it is unexpected and worth sending. The `not self-healing` line appears on a build that is not the table's (and once more after a cached resolver exception on the known build, because that failed result is not marked as a known build) and lists the fixed offsets with their values. Group 2 `ON` means the gate allows the feature; whether the feature itself is switched on is still its own ini key (`JitterSource`, `JitterProbe`, ...).

`[ffxiv-catalog]`, the shader fingerprint tables (UI 7 entries, scene 5, lighting 9, chara 9, companion 4, nameplate 3). The first summary comes after 600 Presents and 30 seconds, later ones whenever a count changes:

```
[ffxiv-catalog] UI 6/7 scene n/5 lighting n/9 chara n/9 companion n/4 nameplate n/3 | pixel shaders created ..., uncatalogued sampled ... | t=...ms present=... | UI boundary ok
```

On a healthy build the UI catalog reads `6/7` and the line ends with `UI boundary ok`. Six, not seven, was seen in all 26 logged runs on both known builds, so the rule is "at least 6 of the 7", evaluated every frame. Which six of the seven a healthy build creates is not known yet. The first real log will say: its `[ffxiv-catalog] first seen crc=0x... name=UI:...` lines name the six, and the missing one is listed as `[ffxiv-catalog] UI #n (name) crc=0x... missing`. The review of the catalog code guessed the missing one is `ui5` (`0xE95DEEC4`, which would make the required set `0x5F`); only a real log decides. Until then the exact required set stays unpinned (`kUiRequiredMask = 0`). With fewer than 6 the line ends with `UI boundary FALLBACK (UI n/7, ...)`, the HUD-less boundary detector reports no boundary that frame, and the HUD-less capture takes its fallback. Scene and chara have no baseline either (scene stage 3 has no match in the installed shader files, and the chara finisher shader appears when the lobby starts, after the settle point), so a count below the table size there is not an alarm on its own. After a count below the baseline, the log adds `missing` lines (with a candidate when a shader of the same structure exists) and a `near-miss` block listing the shaders of the session that belong to no catalog.

## Honest limits

- **No pattern set survives a compiler change or a big refactor.** A new toolchain reshapes bytes everywhere. The search then reports each seam as `NotFound`, `Ambiguous` or `Conflict`, and each feature goes off alone. The anchors prefer what survives a recompile (struct-offset displacements, immediates, call relations, strings) over raw prologue bytes, but that is a preference, not a guarantee. A function that was rewritten in a patch stays inert until someone writes a new anchor for it.
- **Fixed struct offsets are not self-healing.** 29 offsets have no seam at all (settings renderer and additional offsets, the companion queue and batch header offsets, the CharaView entry offsets, the jitter object layout; the list is printed by `ffxiv-resolve --emit-gamebuild`). The offsets that do have a seam (every jitter source offset, `contextCacheOffset`, `targetsOffset`) are decoded and range-checked, but the strict shapes that carry the offset also pin it: if a patch moves one, the shape fails and the hook refuses, it does not follow. If a patch moves an offset that is not pinned, the consumer reads a wrong field. What protects there: the runtime range checks, the rule that moving a healed companion header offset refuses the companion hooks (its unhealed neighbours are read by the same code), and the `not self-healing` log line.
- **Recompiled shader catalogs fail safe.** The CRC tables identify shaders by their bytecode. A patch that recompiles a shader changes its CRC; the feature that depends on it stays inert, and nothing is adopted automatically. The log names near-miss candidates by structural fingerprint so a human can retag. The executable holds only 51 embedded shaders and none of the cataloged ones, so no offline tool can check a catalog: only the in-game log shows it.
- **XeFG and DLSS-G keep their DLL version gates.** The game patch does not change those DLLs, and the pattern search does not touch the gates.
- **Medium-confidence seams.** `companion.nameplateModulePointer`: three distinct code sites load the same global and walk the same pointer chain, but nothing in the executable says that the object at the end is the nameplate command queue the fork snapshots. That last link is checked at run time (every read is range-checked, and the first chain log line shows the values). `jitterSource.deviceSize`: the pattern matches two byte-identical copies of a template instance (`0xd3f73` and `0xd4013`), and the seam expects exactly two. A compiler that emits a third copy makes the seam `Ambiguous` and turns the jitter source off; one copy fewer does the same.
- **Mostly one build of evidence.** The previous build is gone. Byte evidence of it exists for the quality, companion, CharaView and camera seams only; the jitter source seams were written against the current build alone.
- **Never run inside the real game yet.** What has run only offline: `LiveImage` (reading the image from the running process), `EntryPatched` detection against a real injector, the shape gating, and the startup order. Offline proof exists for the rest: all 81 seams `Found` on the current executable with every bound value equal to the table, a relocated copy of the image (0 differences), mutation tests of the anchors and the gate, and a full unit suite. The test plan lists what the first in-game session must show.
- **The default of `FfxivUnknownBuildHooks` is off** until one real patch is proven (see above).
- **The codegen is a draft for review.** `ffxiv-resolve --emit-gamebuild` rewrites the bound fields from the search. It lists what it could not check (`NOT SELF-HEALING / NOT RE-VERIFIED`). A pinned entry is only as good as that review.

## Where it lives

| What | Where |
|---|---|
| Resolver, anchors, gate, startup | `OptiScaler/ext/ffxiv/resolve/` (`Resolver.h`, `Anchors*.h`, `Gate.h`, `ResolveStartup.cpp`) |
| Catalog health | `OptiScaler/ext/ffxiv/catalog/` |
| The known build table | `OptiScaler/ext/ffxiv/GameBuild.h` |
| Patch-day CLI and codegen | `tools/ffxiv-resolve/` (never compiled into the product) |
| Keys | `OptiScaler/ext/ini/FFXIV.ini` |
| Tests | `tests/ext/ffxiv/resolve/` |
