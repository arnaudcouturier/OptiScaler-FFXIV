# FFXIV pattern search: in-game test plan

The checks the maintainer runs in the game, because the offline tests cannot. What they cannot show: the resolver reading the running process (`LiveImage`), `EntryPatched` against a real injector, the shape gating on live memory, the startup order, the real cost of the first scan. Everything else was proven offline on the current executable and on the recorded bytes of the previous build ([FFXIV-PATTERN-SEARCH.md](FFXIV-PATTERN-SEARCH.md), "Honest limits").

Four runs, in this order. Each run is one game start: reach the title screen, log in to a character, stand in the world for at least a minute, open the character and equipment windows once (the preview), then close the game. The catalog summary needs 600 Presents and 30 seconds, so do not quit early.

## Before the first run

- Install the build under test the usual way (the package from `tools\package.ps1`; the installer is you).
- In `OptiScaler.ini` set `[Log] LogToFile=true`. Delete or rename the old `OptiScaler.log` so each run starts a fresh file, and keep one log per run.
- In `[FFXIV]` leave `FfxivLegacyGameBuild`, `FfxivUnknownBuildHooks` and `FfxivForceUnknownBuild` at `auto` (false) except where a run says otherwise. The three keys are read once at start; change them with the game closed.
- Turn on the features you actually use as you normally would (native quality, jitter source, character preview, HUD-less, HDR). A feature that is off in the ini has no log lines of its own, which makes a gate line `ON` impossible to check against behaviour.
- Pull the lines out of each log with:
  ```powershell
  Select-String -Path OptiScaler.log -Pattern '\[ffxiv-(gate|resolve|catalog)\]' | ForEach-Object Line > ffxiv-lines-runN.txt
  ```

## Run 1: the known build, default ini

Expected: every feature as in the previous release, and the new log blocks clean.

| Check | Pass |
|---|---|
| `Matched supported FFXIV build for game-module seams` appears once | yes |
| `[ffxiv-gate] build stamp=0x6ad10bfd size=0x381f000 known=yes forced_unknown=no legacy=no unknown_build_hooks=off exe=ffxiv_dx11.exe resolve_ms=...` appears once; note `resolve_ms` | yes |
| The `[ffxiv-resolve]` block: 53 seam lines, every one `Found` with `agree` (or `table=n/a` for the shape and helper seams), no `WARN`, no `DISAGREE`, no `fell back` note | yes |
| `[ffxiv-resolve] total seams=53 found=53 single=0 notfound=0 ambiguous=0 conflict=0 patched=0 ... known=yes` | yes |
| `[ffxiv-gate] group1 ON (...)` | yes |
| One `[ffxiv-gate] feature <name> ON` line each for `qualityRead`, `qualityHooks`, `jitterProbe`, `jitterSource`, `charaViewAccessor` | all five ON |
| Four `[ffxiv-gate] cross-check ... agree` lines | all agree |
| No `[ffxiv-gate] not self-healing` line (it only appears on a build that is not the table's, or after a resolver exception on the known build) | absent |
| The last `[ffxiv-catalog]` summary line reads `UI 6/7` and ends with `UI boundary ok` | yes |
| Features behave as before: the lines of the previous release for native quality (`FFXIV native quality v2: hook transaction result=0 (0=installed); native scene updates enabled`, once; `FFXIV native DLAA gate: ...` after a quality change in the game), lighting (`FFXIV lighting: native exposure hooks ready`), jitter source if on (`Jitter source: active (...)`, `verified N frames`), the character preview, HUD-less, scene HDR | yes |
| No new freeze or hitch when the device is created: the one-time scan is expected to cost about 170 to 250 ms about 8 seconds after the DLL loads, in the loading phase. Note anything you notice | none |

If `UI 6/7` shows another number, that is a finding, not necessarily a failure. Send the whole `[ffxiv-catalog]` set of lines. If it reads `6/7`, the `[ffxiv-catalog] first seen crc=0x... name=UI:...` lines name the six that were created and the line `[ffxiv-catalog] UI #n (name) crc=0x... missing` names the seventh: send those, they pin the exact required set.

## Run 2: forced unknown build

Set `FfxivForceUnknownBuild=true` and `FfxivUnknownBuildHooks=true`. The resolver now runs without the table on today's executable, which is what a patched game will get.

| Check | Pass |
|---|---|
| `[ffxiv-gate] build ... known=no forced_unknown=yes legacy=no unknown_build_hooks=on` | yes |
| `[ffxiv-resolve]` block: every seam `Found`, `table=n/a` on every line, `known=no` in the total | yes |
| All five Group 2 features `ON` | yes |
| Four cross-checks `agree` | yes |
| A `WARN` line `[ffxiv-gate] not self-healing: 13 struct offsets keep their table values on this build (UNKNOWN build ...)` listing the offsets with their values | present |
| Every feature behaves exactly as in run 1, because every value the search found equals the table's | yes |
| `resolve_ms` | note it |

Optional second half: leave `FfxivForceUnknownBuild=true` and set `FfxivUnknownBuildHooks=false`. Expect all five Group 2 features `OFF` with the reason `unknown build and FfxivUnknownBuildHooks is off`, Group 1 still `ON`, and none of the address features working (native quality, jitter, preview lookup), while scene HDR, lighting, HUD-less, geometry and markers still do.

## Run 3: the usual injectors, on the known build

Default keys. Start the game the way you normally do, with the programs that touch the game's memory running: Dalamud through XIVLauncher, ACT with its overlay, ReShade. This is the first time the resolver reads a process that has third-party code in it.

| Check | Pass |
|---|---|
| The same as run 1: all 53 seams `Found`, five features `ON`, cross-checks agree, `UI 6/7`, features behave | ideally yes |
| Any seam that is not `Found`: copy the line. The ones to watch are the function entries a plugin might hook (`quality.querySize`, ...). `EntryPatched` means a jump sits over the entry: the feature is off by design, on the known build too | note which |
| A seam that is `FoundSingleAnchor` or filled from the table (`fell back` in the note) | note which; the feature stays on |
| `[ffxiv-gate] feature ... OFF` lines on the known build (they are WARNs) | none expected; if present, the line names the first failing seam |
| `resolve_ms` compared with run 1 | similar |

Do the run once with all three and, if anything differs from run 1, once per program to find which one causes it.

## Run 4: the legacy key, A/B

Set `FfxivLegacyGameBuild=true` on the known build. This is exactly the old path.

| Check | Pass |
|---|---|
| `[ffxiv-gate] build ... legacy=yes` | yes |
| No `[ffxiv-resolve]` lines at all | yes |
| Group 1 and Group 2 features `ON` by the legacy rule | yes |
| Every feature behaves as in run 1 | yes |
| Frame time and the loading phase compared with run 1 (the legacy run has no scan) | note any difference |

If you have a patched executable to hand, one more start with `FfxivLegacyGameBuild=true` should show every FFXIV feature off, as before the pattern search existed.

## What to send back

From each run's `OptiScaler.log`:

- every `[ffxiv-gate]`, `[ffxiv-resolve]` and `[ffxiv-catalog]` line, in order (the extraction command above);
- the `Matched supported FFXIV build` line and the feature lines listed in run 1 for the features you use;
- for run 3, which injectors were running;
- the executable's `TimeDateStamp` (the `[ffxiv-gate] build` line has it), the ini keys of `[FFXIV]` as you had them, and a sentence on whether anything looked different from the previous release (frame time, a missing feature, a hitch at start).

## What this decides

- Run 1 and run 3 clean: the known-build path is safe to release. The resolver added only the scan and the log.
- Run 2 clean: the unknown-build path works on live memory for the current executable. It does not prove that it survives a patch.
- Run 4 clean: the rollback key does what it says.
- `FfxivUnknownBuildHooks` stays off by default until a real patch has gone through: every Group 2 seam found uniquely, the per-seam lines matching what the executable holds, and each feature checked in game. After that it flips to on.
