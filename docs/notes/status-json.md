# status.json (generic status file)

While `[Status] Enabled` is true, the DLL writes `status.json` next to itself (the game folder, beside the log
file). The file is a small, versioned snapshot for bug reports and tools: versions, feature states and frame-time
percentiles. It never holds a path, a drive letter, a user name or a character name.

- Format marker: `"format": "optiscaler-ffxiv-status"`, `"version": 1`. Readers ignore unknown fields, and the
  documented core below keeps its meaning within version 1 (new fields may be added). `truncated` is `true`
  only when details were dropped to fit the cap, and absent otherwise.
- Size: UTF-8, at most 16 KB, always valid JSON: over the cap the free-text details are dropped and
  `"truncated": true` is set (a minimal fixed-fields document if even that does not fit — never chopped).
- Timing: first write about 2 s after the first present, then when the state changes at most once a second, plus
  a heartbeat every 10 s (`seq` counts every write). A state change is a change of the state fields only (features,
  runtimes, frame generation, errors, versions): percentiles and timestamps alone never cause a write.
- Atomicity: a per-pid temp file renamed over the old one; a crash leaves the previous file intact.
- Ownership: only the presenting game client writes; with two clients the first one to present owns the file
  (`status.lock`, refreshed on every write, stale after 30 s). The lock names our own pid while we hold it, so the
  writer never blocks itself on its own lock. A lock pid the writer cannot open for lack of rights counts as
  a live owner and is never evicted. A take/replace failure is counted and logged once, then backed off
  for 30 s. Crashes leave stale `status.tmp-<pid>.json` files behind; the worker deletes the dead ones at startup.
  On a normal game exit, the writer deletes its own `status.json` + `status.lock` when it still owns them (only a
  game exit deletes the file once the worker ran): no file means a clean exit, a file with a stale lock means a
  dead process.
- Cost: a below-normal-priority worker thread started on the first present (nothing in DllMain); the present path
  pays one ignored-phase test, otherwise one key read when the key is off, a timestamp and a ring push when it is
  on. The GPU adapter is enumerated under the hook bypass scope (never a wrapped factory), retried on later
  write-due ticks when the first enumeration fails; the
  runtime versions and `ffxivgame.ver` are cached and refreshed only when a write is due. The game never waits for a
  write: failures are counted into the file's own `errors` block and skipped. The module is pinned before the
  worker starts (if pinning fails the worker never starts and the key stays effectively off), so the DLL cannot be
  unloaded until process exit (intended) and the FreeLibrary detach branch is unreachable once the worker runs; the
  detach still only signals the worker, never waits on it (it may need the loader lock the detach holds), so a late
  worker never runs unmapped code. On a normal game exit our own file is deleted too; with any other detach reason
  the process is going away and nothing blocks.

## Fields (v1)

- `seq`, `writtenUtc`: write counter and time (UTC, ISO-8601).
- `session`: `id` (random 64-bit hex, new every process), `pid`, `startUtc`, `uptimeSec`.
- `fork`: `describe` (the build tag) and `upstream` (the upstream base commit).
- `game`: `exe` (bare file name), `matched` (a game module is published for this build), `peStamp` and
  `peImageSize` (TimeDateStamp and SizeOfImage of the running exe's own headers, read without the game-build
  table), `buildVer` (first line of `ffxivgame.ver`, "" when unreadable).
- `gpu`: adapter display `name` and `vramMB` (first non-software DXGI adapter; cached for the session).
- `runtimes`: `dlss`, `dlssg`, `nr`, `streamline`, each `{found, loaded, version, folder}`. `found` = a copy exists
  where the loader looks (DLSS: the NGX search order, then the OptiScaler folder, then the game folder; others: the
  loaded copy or the known install folders). `loaded` = mapped into the process. `version` = snippet or file
  version, "" when unknown (a non-version readout — the unlock reports `metadata pending` while discovery
  settles — never enters the field). `folder` = a label from a closed set only (`game folder`, `OptiScaler folder`,
  `streamline folder`, `NGX driver folder`, `other folder`), "" when unknown: the exe's folder, the DLL's folder
  (including a direct child such as `dlssg_sm86`), a folder named `streamline`, anywhere else on the NGX search
  order (driver-store copies), or anywhere else. The DLSS-G version prefers the unlock's readout. The loaded bit is
  read every tick; the version text and the found folder are cached and refreshed only when a write may be due.
- `features`: one entry per feature, `{state, reason, detail}`. `state` is one of `engaged` (on and delivering),
  `pending` (on but not delivering yet, or armed and falling back), `off` (switched off), `failed` (on and
  known-broken), `unknown` (on, but the state could not be read). `reason` is a closed per-feature vocabulary
  (below); `detail` is short free text (counts, a menu status line), never a path.
  - `gameModule`: `engaged`/`matched` when any game interface is published (detail names them: `geometry`,
    `quality`, `lighting`, `hdr`, `fgMarkers`, `ngx`, `nrQuirks`, `hudless`), else `off`/`no-match`.
  - `nativeQuality`: `off`/`no-game-module` without the interface, `pending`/`not-live` before the first reading,
    `engaged`/`live` after (detail `dlaa-required` or `preset`).
  - `companion`: `engaged`/`installed` when the native module is installed, `pending`/`configured-not-installed`
    when the ini asks for it but it is not (detail `replacement`, `interpolation` or both), else `off`/`disabled`.
  - `nr`: `off`/`disabled` when the key is off, `engaged`/`evaluating` once an evaluate ran, else
    `pending`/`armed-no-evaluate`.
  - `frameGeneration`: from the upstream input/output state: both `NoFG` gives `off`/`disabled`, an output gives
    `engaged`/`active`, input without output gives `pending`/`no-output`. The detail names both (`Upscaler-in`,
    `DLSSG-out`, ...).
  - `hudless`: `off`/`disabled` when the mode is off; otherwise the engine state: `engaged`/`delivering`,
    `pending`/`searching`, `pending`/`fallback` (the dominant reason rides in the detail, e.g.
    `Fallback(MaskCoverage)`), `unknown`/`no-phase`.
  - `hdr`: `off`/`disabled` when both keys are off; `engaged`/`active` once created (detail `sdr-expansion` or
    `scene`), else `pending`/`not-created`.
- `frameGeneration`: `{input, output}` codes (`NoFG`, `Upscaler`, `DLSSG`, `NvngxFG`, `FSRFG`, `FSRFG30`, `XeFG`,
  `ForceXeLL`, `Reprojection`).
- `nr`: `{enabled, active}` from the key and the feature bit.
- `performance`: `base` (bridge-present cadence) and `output` (visible-present cadence; `null` until an output
  present has been seen). Each holds `recent` (the last 180 intervals, exact percentiles) and `session` (whole
  session: exact count/mean/max, reservoir-sampled p50/p95/p99 over 1024 slots) with `frames`, `p50Ms`, `p95Ms`,
  `p99Ms`, `meanMs`, `maxMs`. Gaps over 60 s (a hitch or a pause) are not frame times and are skipped.
- `errors`: the writer's own tally: `count` of failed takes/replaces (counted once per episode, with a 30 s
  backoff), `code` (`""` while the count is 0, `"lock"` or `"write"`), `detail` (failing step + numeric error:
  `open`/`write` for the lock, `create`/`write`/`flush`/`move` for the replace; never message text).
  Feature-error aggregation is future work: collecting it needs hooks in the feature files, which are out of scope
  here.

## Privacy

A unit test scans the produced file for `C:\`, `\Users\` and `/Users/` and fails on any hit. The writer emits no
absolute or relative path anywhere: folders appear as labels, the exe as a bare name, errors as numbers. Folder
labels are a closed set (`game folder`, `OptiScaler folder`, `streamline folder`, `NGX driver folder`,
`other folder`) and cannot leak a user name because they never contain a path component from the machine.
Every string is cleaned to valid UTF-8 (invalid bytes become U+FFFD) and capped at render time, so no field can
carry unbounded or non-UTF-8 content.
