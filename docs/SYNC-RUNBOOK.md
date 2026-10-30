# Syncing with upstream OptiScaler

How to move the fork's stack onto a newer upstream commit. Everything the fork owns lives under `OptiScaler/ext/` (plus `docs/`, `tools/`, `tests/ext/`), and upstream files carry only the seams listed in [SEAMS-catalog.md](SEAMS-catalog.md). A sync is therefore: bring upstream in, resolve the few places where both sides edited the same lines, check that every seam still reaches the code it should, rebuild, test, play, tag.

The upstream commit the stack is currently built on is recorded in [UPSTREAM-BASE](UPSTREAM-BASE) (`d9b62110` at the time of writing). The weekly `ffxiv-upstream-watch` workflow draft compares it with upstream `master`; see [RELEASE.md](RELEASE.md) for why it is inactive.

Commands are PowerShell 7 unless they start with `git`. `<repo>` is your clone.

## 0. Preconditions

- A clean working tree on `main`, equal to what is published (`git status`, `git fetch origin`, `git rev-parse main origin/main`).
- The submodules are initialised (`git submodule update --init --recursive`); the build needs them. The dry-run described below did not need them because it never builds.
- `rerere` is on: `git config rerere.enabled` prints `true`. If it does not, `git config rerere.enabled true` (this stores resolved conflicts in `.git/rr-cache` and replays them when the same conflict appears again).
- Visual Studio 2022 Build Tools (the scripts locate MSBuild through `OPTISCALER_MSBUILD`, a default path, or `vswhere`), and the game installed for the smoke test.
- Note the current tip and tag so a rollback is one command: `git tag --points-at main`, `git rev-parse main`.

## 1. Fetch and size the job

```powershell
git fetch upstream
$base = (Get-Content docs/UPSTREAM-BASE | Where-Object { $_ -notmatch '^\s*(#|$)' } | Select-Object -First 1).Trim()
git log --oneline "$base..upstream/master"                 # what upstream did
git diff --stat "$base" upstream/master | Select-Object -Last 1
```

Then list the files **both sides** changed. Those are the only files that can conflict textually:

```powershell
$fork = '^(OptiScaler/ext/|docs/|tools/|tests/ext/|\.github/README\.md|\.github/assets/)'
git diff --name-only $base upstream/master | Sort-Object > $env:TEMP\up.txt
git diff --name-only $base main | Where-Object { $_ -notmatch $fork } | Sort-Object > $env:TEMP\fork.txt
Compare-Object (Get-Content $env:TEMP\up.txt) (Get-Content $env:TEMP\fork.txt) -IncludeEqual -ExcludeDifferent | ForEach-Object InputObject
```

Also look at `git diff --name-only $base upstream/master -- .github/workflows`: a workflow file that upstream adds starts out active as soon as GitHub Actions are enabled on this repository (see section 10). Compare the overlap result with the hot-spot list in section 6. A file that upstream changed and that is not on the list is not a seam host and needs only a build.

## 2. Choose the strategy

The stack was 198 commits on top of the base at the time of the example in section 8. There are two ways to put it on a new upstream commit.

| | Merge | Rebase with `rerere` (recommended) |
|---|---|---|
| Command | `git merge --no-ff upstream/master` | `git rebase --onto upstream/master <base> <branch>` |
| Conflicts to resolve | all at once, against the final result | per commit, where the stack touched the same lines; `rerere` replays an identical resolution automatically |
| Equivalent upstream fixes | stay in the tree; you have to find and drop the `[UP]` commits by hand | an `[UP]` commit that upstream shipped identically becomes empty and drops itself; a near-miss conflicts and you skip it on purpose |
| Review | a normal diff of the merge result | `git range-diff` shows what changed in each fork commit |
| `footprint.ps1` | works (merge base = upstream) | works |
| Cost | history gets one merge commit per sync, and the stack stops being a clean series on top of upstream | rewrites published history: `main` moves, so the old tip survives only through its tags, and a push needs `--force-with-lease` |
| Intermediate commits | still build as before | may no longer build individually; only the tip is verified |

**Recommendation: rebase with `rerere`, onto a new branch, publish once.** The reasons: the design of the stack (see [SEAMS.md](SEAMS.md) section 3.5) assumes `[UP]` commits retire themselves when upstream ships the same fix; `range-diff` is the right tool to prove that each seam commit still does what it did; and the second and later syncs get cheaper because `rerere` has learned the recurring hunks (the one-line include collisions in particular). Do the rebase on a new branch (`sync-<sha8>`), never on `main`, so nothing public moves until the result is built, tested and played.

**Use the merge instead** when the rebase stops more than about 15 times, or when upstream rewrote a seam host so heavily that replaying the individual commits makes no sense (a merge turns that into one conflict to think about). If you merge, retire the `[UP]` commits that upstream now covers by hand with a normal commit that removes the fork's copy, and note it in section (a) of the seam catalog.

Both options were exercised mechanically on `46e1dd3d`; the numbers are in section 8.

## 3. Rebase

```powershell
git switch -c "sync-$((git rev-parse --short=8 upstream/master))" main
git rebase --onto upstream/master $base HEAD
```

Each time it stops:

1. `git status` lists the conflicted files; `git diff --name-only --diff-filter=U`. With `rerere.autoUpdate=true` a replayed resolution is staged for you (`Staged '<file>' using previous resolution`), so that command can print nothing even though files were conflicted: read the `Resolved` / `Staged` lines of the merge or rebase output instead, and check every replayed file.
2. Resolve with the rules in section 5, `git add <file>`, `git rebase --continue`. `rerere` will say `Resolved ... using previous resolution` for a hunk it has seen.
3. If the stopped commit is an `[UP]` commit and upstream has its own version of the same fix, `git rebase --skip` and write down which one (it goes into the commit message of the sync and into section (a) of the seam catalog). Expect the next commit that sits on top of the skipped one to conflict as well.
4. If you lose track: `git rebase --abort` returns to the pre-rebase state. Aborted attempts leave unresolved entries behind in `.git/rr-cache` (a directory with a `preimage` and no `postimage`). They are harmless and `git rerere gc` prunes them after a while; if you want them gone now, delete exactly those directories and leave the ones that have a `postimage`.

The merge alternative: `git switch -c sync-<sha8> main` then `git merge --no-ff upstream/master`, resolve, `git commit`. It was used for the first real sync (below): 6 conflicted files, one hunk each, no compile errors. It never rewrites published history, so no force-push is needed and the earlier tags stay ancestors.

**Do not trust a replayed resolution blindly.** `rerere` replays whatever was recorded first, including a resolution that was later found wrong. For `OptiScaler/menu/menu_overlay_dx.cpp` the cached resolution dropped upstream's per-back-buffer menu fence; the correct result keeps upstream's fence next to the fork's submission ring (the menu blur writes a descriptor slot per back-buffer index, which the ring does not cover). After a merge or rebase has replayed it, compare that file against the last good result (`git show <known-good-commit>:OptiScaler/menu/menu_overlay_dx.cpp`) and fix it before committing. In general: review every replayed file against the catalog entry of its seam, and run `git rerere forget <file>` for a resolution you find wrong, so it is not replayed again.

## 4. Review

```powershell
$new = 'upstream/master..HEAD'       # run on the sync branch
git range-diff "$base..main" $new   # rebase only: every fork commit should be '=' or have an explainable '!'
pwsh -File tools\footprint.ps1 -Rev HEAD   # same or smaller than before
```

After a **merge**, `git range-diff` shows nothing useful (the commit ids are unchanged, so everything is `=`). Compare the per-file footprint listing before and after instead: the seam lines in upstream files must be identical to those on `main`.

Then prove the seams are still in place:

```powershell
# seam lines in upstream-owned files, per file, before and after: the two listings must be identical
$pattern = 'ext::|// ext:'
git grep -c -E $pattern main -- 'OptiScaler/*' ':!OptiScaler/ext/**' ':!OptiScaler/external/**' | ForEach-Object { $_ -replace '^main:','' } | Sort-Object > $env:TEMP\seams-before.txt
git grep -c -E $pattern -- 'OptiScaler/*' ':!OptiScaler/ext/**' ':!OptiScaler/external/**' | Sort-Object > $env:TEMP\seams-after.txt
Compare-Object (Get-Content $env:TEMP\seams-before.txt) (Get-Content $env:TEMP\seams-after.txt)
```

(In the worked example below, both listings held 266 matching lines in the same 51 files.) A seam call that moved to a different function is fine; one that disappeared is a bug.

The textual merge cannot see the next three things, so check them by hand:

- **Headers upstream changed that fork code includes or names.** Fork code sits in `OptiScaler/ext/` and reaches upstream's private state only through `ext::Access` ([SEAMS-catalog.md](SEAMS-catalog.md) section (b)). For every header upstream changed, list the fork code that mentions it:
  ```powershell
  git diff --name-only $base upstream/master -- '*.h' | ForEach-Object { $n = Split-Path $_ -Leaf; "$n : " + ((git grep -l $n -- OptiScaler/ext tests/ext) -join ', ') }
  ```
  Then read the diff of the classes named in section (b) of the catalog (`IFGFeature_Dx12`, `Dx11wDx12SC`, `IFeature_Dx11wDx12`, `IFeature`, `MenuCommon`, `DLSSG_Common`, `ResTrack_Dx12`) for removed or renamed members.
- **Identifiers upstream removed that the fork still uses.** Extract the removed identifiers of the upstream diff, subtract the ones that reappear in added lines, and intersect with the identifiers used under `OptiScaler/ext` and `tests/ext`. Expect noise from generic names; look at each hit.
- **Menu anchors.** Fork cards are placed with `after = "<upstream card id>"`: `fg_selection`, `upscaler`, `sharpness` and `upscale_ratio` must still exist in `menu_common.cpp` (`git grep -c '"fg_selection"' -- OptiScaler/menu/menu_common.cpp`, and so on).
- **Config members.** Fork code reads upstream members of `Config` (for example `OverlayMenu`, `RestoreComputeSignature`). If upstream changes their type, check that every use still compiles (the build tells you).

## 5. How to resolve the recurring conflicts

| Conflict | Resolution |
|---|---|
| An `#include` line next to the fork's `#include "ext/Seams.h"` (or `<mutex>`) | Keep both lines. |
| A seam call next to a new upstream statement that reads the same value (for example the Vulkan evaluate path, where upstream reads `Parameter_Output` and the fork's `ext::nr::VkEvalBefore` runs before the evaluate) | Keep both; upstream's read first, the seam call after it, unless the seam's contract in the catalog says otherwise. |
| An upstream line the fork extended with one `// ext:` loop or call (menu rows) | Take upstream's new line and re-add the fork's extension underneath it. |
| Upstream rewrote a region that also hosts a fork `[UP]` fix | Prefer upstream's code. Drop the `[UP]` commit (or reduce it to what upstream still lacks) and re-place any seam that sat on top of it. Update section (a) of the catalog. |
| A conflict inside a seam host that is not one of the above | Stop. Re-read the seam's row in the catalog, then redo the seam on the new code. Do not guess. |

## 6. Conflict hot spots

Derived from the catalog and from the diff of the stack against the base. 71 upstream files carry fork lines; 51 of them carry seam calls or `// ext:` markers. In descending order of seam lines: `framegen/xefg/XeFG_Dx12.cpp` (28), `with_dx12/dx11_with_dx12_sc.cpp` (22), `upscalers/IFeature_Dx11wDx12.cpp` (22), `menu/menu_common.cpp` (16), `framegen/dlssg/DLSSG_Dx12.cpp` (14), `hooks/DxgiFactory_Hooks.cpp` (11), `resource_tracking/ResTrack_dx12.cpp` (10), `hooks/Streamline_Hooks.cpp` (10), `hooks/DxgiFactory_WrappedCalls.cpp` (10), then smaller counts in `wrapped/wrapped_swapchain.cpp`, `upscalers/IFeature_VkwDx12.cpp`, `inputs/NVNGX_DLSS_Dx11.cpp`, `hooks/FG_Hooks.cpp`, `framegen/dlssg/DLSSG_Common.cpp`, `inputs/NVNGX_DLSS_Dx12.cpp`, `NVNGX_Parameter.cpp`, `Config.cpp`, the DLSS feature files, `menu/menu_overlay_dx.cpp`, `hooks/D3D11_Hooks.cpp`, `framegen/IFGFeature_Dx12.cpp` and others (all under `OptiScaler/`).

| Area | Upstream files (under `OptiScaler/`) | Seams (catalog group) |
|---|---|---|
| Menu | `menu/menu_common.cpp`, `menu/menu_common.h`, `menu/menu_overlay_dx.cpp`, `menu/menu_overlay_vk.cpp` | menu, hdr (`Overlay*`) |
| Frame generation core | `framegen/IFGFeature_Dx12.cpp/.h`, `framegen/dlssg/DLSSG_Common.cpp`, `DLSSG_Dx12.cpp`, `framegen/xefg/XeFG_Dx12.cpp`, `framegen/ffx/FSRFG_Dx12.cpp`, `inputs/FG/Upscaler_Inputs_Dx11wDx12.cpp` | fg, gpu |
| Bridge (D3D11 on D3D12) | `with_dx12/dx11_with_dx12_sc.cpp/.h`, `upscalers/IFeature_Dx11wDx12.cpp/.h`, `inputs/NVNGX_DLSS_Dx11.cpp` | dx11bridge, hudless, hdr, gpu |
| NR attachment | `inputs/NVNGX_DLSS_Dx12.cpp`, `inputs/NVNGX_DLSS_Vk.cpp`, `upscalers/IFeature_Dx12.cpp`, `upscalers/IFeature_VkwDx12.cpp`, `upscalers/dlss/DLSSFeature*.cpp`, `nvapi/NvApiHooks.cpp`, `Util.cpp`, `hooks/Vulkan_Hooks.cpp`, `proxies/Streamline_Proxy.h` | nr |
| Quality and geometry | `NVNGX_Parameter.cpp`, `upscalers/IFeature.cpp`, `upscalers/IFeature.h` | upscaler |
| Hooks | `hooks/D3D11_Hooks.cpp`, `hooks/D3D12_Hooks.cpp`, `hooks/Dxgi_Hooks.cpp`, `hooks/DxgiFactory_Hooks.cpp/.h`, `hooks/DxgiFactory_WrappedCalls.cpp`, `hooks/FG_Hooks.cpp`, `hooks/Streamline_Hooks.cpp`, `hooks/LibraryLoad_Hooks.cpp`, `hooks/Kernel_Hooks.cpp`, `resource_tracking/ResTrack_dx12.cpp/.h` | core, game, present, restrack, streamline |
| Presenter | `wrapped/wrapped_swapchain.cpp/.h`, `misc/FrameLimit.cpp` | present |
| Process start | `dllmain.cpp`, `Config.cpp`, `misc/GameConfigs.cpp`, `misc/Quirks.h` | config, core, reshade, ffxiv-in-upstream |
| Project files | `OptiScaler.vcxproj` (one `ext.props` import), `OptiScaler.sln` (the forwarder project) | none |

The `[UP]` fixes (section (a) of the catalog) sit in `low_latency/input/*`, `hooks/Reflex_Hooks.cpp`, `hooks/Streamline_Hooks.cpp`, `framegen/*`, `upscalers/IFeature_Dx11.cpp`, `IFeature_Vk.cpp`, `dlssd/*`, `spoofing/Vulkan_Spoofing.cpp`, `proxies/NVNGX_Proxy.h`, `menu/menu_overlay_dx.cpp/.h`, `menu/menu_overlay_vk.cpp`, `shaders/hudless_compare/HC_Dx12.cpp`, `shaders/render_ui/RUI_Dx12.cpp`, `State.h`; check each against upstream's change in the same file first.

## 7. Rebuild, test, package

```powershell
git submodule update --init --recursive
pwsh -File tools\build-locked.ps1 -Rebuild     # the lock keeps two builds on one machine from colliding
pwsh -File tools\test.ps1                      # exit code = number of failures
pwsh -File tools\test.ps1 -List                # recipe list; compare the counts
pwsh -File tools\footprint.ps1                 # persistent surface, files touched, [UP] lines
pwsh -File tools\package.ps1 -Target <package folder>
```

Expected: the build prints `[build] OK`. `tools\test.ps1` builds and runs every recipe under `tests\ext` and ends with `passed N, failed 0, manual M`. The warp tests use the Direct3D software adapter and need no GPU; local recipes (they need this machine: a real GPU, the NVIDIA runtime DLLs, a product build) are listed and run only with `-Local`, and one whose requirements are missing is reported `NOT-RUN` and is not a failure; manual recipes are listed and never run. The last line is then `passed N, failed 0, manual M, local L, not-run K`. At the time of the first real sync (`main` `0d8264f2`) a run gave 108 passed, 0 failed, 7 manual (89 at `c3277532`, then the focus-gate test and the character-preview tests); N should equal the number on `main` unless the sync added or removed recipes, so run `tools\test.ps1` on `main` first and write that number down. These are list counts, not results of a run: treat `0 failed` as the criterion and the count as a sanity check, and write the real numbers into the sync commit message.

When this build is the one to release, do section 10 step 1 first, then create its tag (step 2) on the commit you build, and build on the tagged commit: the build embeds `git describe --long --dirty`, and `tools\package-release.ps1` refuses a DLL whose embedded string is not `<tag>-0-g<sha>` for the release tag with a matching tag commit (exit 6, [RELEASE.md](RELEASE.md) section 1). `tools\package.ps1` for a development build needs no tag.

If the shipped `OptiScaler.ini` changed upstream, check that `tools\package.ps1` still merges every fragment (`[package] [Section] merged` lines, one per fragment in `OptiScaler\ext\ini\`).

## 8. Worked example: `fd6c2870` to `46e1dd3d`

A real dry-run on a throwaway worktree of `main` (`c3277532`), nothing built, nothing pushed, everything removed afterwards.

**Upstream side.** 18 commits, 118 files changed (+13898/-1634), mostly Vulkan frame-generation work (reprojection, hudfix, resource flip, precompiled shaders); 27 of those files are also changed by the fork.

**Merge (`git merge --no-commit --no-ff upstream/master`).** The merge itself takes 0.2 s. Six files had a textual conflict, one hunk each:

| File | Hunk | Seam host? | Resolution |
|---|---|---|---|
| `hooks/Streamline_Hooks.cpp` | `#include <mutex>` against upstream's new include | yes (`[UP]` serialisation) | keep both |
| `upscalers/IFeature_Dx11wDx12.cpp` | `#include "ext/Seams.h"` against `misc/DeferredRelease.h` | yes (22 seam lines) | keep both |
| `upscalers/IFeature_Dx12.cpp` | the same include collision | yes | keep both |
| `inputs/NVNGX_DLSS_Vk.cpp` | the `ext::nr::VkEvalBefore` call against upstream's new read of the output parameter | yes (`nr`) | keep both, upstream's read first |
| `menu/menu_common.cpp` | upstream's `NameWithVersion()` line against the fork's version line plus its extra-rows loop | yes (`menu`, 16 lines) | upstream's line, fork loop re-added |
| `menu/menu_overlay_dx.cpp` | upstream added its own per-allocator completion fence for the menu command lists, in the same lines as the fork's `[UP]` overlay fence ring | yes (`[UP]` plus the HDR menu layer) | **not resolved, needs a decision**: the two solve the same problem. Either drop or reduce the `[UP]` ring and re-place the HDR menu-layer seam on top of upstream's fence, or keep both if the ring's extra coverage (it also rings the ImGui upload buffers, which upstream's fence, judging by its diff, does not) is still wanted. Test the menu in SDR and HDR either way |

The five mechanical resolutions took one small script. After them the number of seam lines in upstream files was 266 in 51 files, identical to `main`, so no seam was lost textually.

**Semantic check.** No identifier that upstream removed is used by fork code (the 14 intersecting names were generic: `FAILED`, `GetDevice`, `signal` and similar). Five `Config` members that fork code reads changed to `CustomOptional<bool, SoftDefault>` (`OverlayMenu`, `OverrideVsync`, `RestoreComputeSignature`, `RestoreGraphicSignature`, `FGDisableHudless`); fork code uses them only through `value_or_default()`, which that type keeps, but this is by reading, not compiled. The menu anchors `fg_selection`, `upscaler`, `sharpness`, `upscale_ratio` all still exist.

**Rebase (`git rebase --onto upstream/master fd6c2870`, `rerere` on).** It stopped at three commits and was then aborted, as planned:

| Stop | Commit | Conflict |
|---|---|---|
| 12 of 198 | `[UP] streamline: serialize Detours transactions` | the include collision; resolved keep-both |
| 30 of 198 | `[UP] menu: fence-based ring for the DX12 overlay's allocators and ImGui upload buffers` | the overlay fence overlap; skipped (not resolved), so the commits above it in the stack that edit the overlay are expected to conflict next |
| 32 of 198 | `[C6] gpu: owner registry, Reset/Execute wrappers ...` | the include collision again in `IFeature_Dx11wDx12.cpp` |

Commits 13 to 29 and the steps between applied cleanly. Each step took under a second. Over the whole stack the number of stops cannot be extrapolated from three samples; what can be said is that the recurring kinds are the one-line include collisions (which `rerere` replays after the first resolution) and the small set of files in the table, where the fork has many commits: per-file commit counts since the base (fork / upstream) were `IFeature_Dx11wDx12.cpp` 11/1, `menu_common.cpp` 8/8, `Streamline_Hooks.cpp` 5/1, `menu_overlay_dx.cpp` 2/1, `IFeature_Dx12.cpp` 2/1, `NVNGX_DLSS_Vk.cpp` 1/2. The only file where both sides made many commits is `menu_common.cpp`, so that is where a full rebase will ask for the most attention.

**Effort estimate (inference, not measured).** The mechanical part is minutes. The decision about the overlay fence, the rebuild, the test run and the in-game check of the menu in SDR and HDR dominate: plan on a few hours, most of it waiting for builds and playing.

**Cleanup done after the dry-run.** Both throwaway worktrees and their branches were removed, `main`, the tags and the remotes were unchanged, and the preimage-only entries the dry-run wrote into the shared `.git/rr-cache` were deleted (they had no resolution attached; the pre-existing resolved entries were left alone).

## 9. In-game smoke test

Use the freshly packaged build in a copy of the game folder (back up first; see [RELEASE.md](RELEASE.md)). Set `[Log] LogToFile = true` first (the default is no log file) and check `OptiScaler.log` after each step for warnings.

1. **Launch.** The game reaches the title screen and a character in the world. In the log: the version banner names the fork tag and the upstream sha, and `Matched supported FFXIV build for game-module seams` appears together with the `[ffxiv-gate] build ... known=yes` line (if it does not, see [FFXIV-PATCH-RUNBOOK.md](FFXIV-PATCH-RUNBOOK.md); what the `[ffxiv-resolve]` and `[ffxiv-gate]` lines should show is in [FFXIV-PATTERN-SEARCH-TEST-PLAN.md](FFXIV-PATTERN-SEARCH-TEST-PLAN.md)).
2. **DLSS-NR on.** Enable it in the `dlss_nr` card; the picture changes, no black or corrupted frame, no error lines. Toggle off and on once.
3. **DLSS-G on.** Frame generation at 2x, then your usual factor: the FPS counter shows the multiplied rate; no flicker; the menu opens and closes.
4. **HDR output.** With Windows HDR on, `[HDR] FfxivHDR = true`: the image is HDR, the menu colours are not washed out. With `Mode = 1`, highlights are not clipped.
5. **HUD-less off, then on.** `[HudLess] Mode = ui_free`: the HUD stays sharp while turning the camera quickly; the log shows the engine active and no refused-capture line in normal scenes. Set it back to `off` afterwards if you do not want it.
6. **Nameplates.** Nameplates draw above characters, move with them, and stay readable with frame generation on.
7. **Exit** cleanly; the game process ends without a crash dump.

## 10. Publish

1. Update [UPSTREAM-BASE](UPSTREAM-BASE) to the new upstream sha, and the catalog for any seam that was added, removed or moved (a seam change and its catalog change belong in the same commit). The sha also appears, as history or as a fallback, in `OptiScaler/ext/ext.props` (banner fallback used only when no tag can be read) and in the fallbacks of `tools/footprint.ps1`; those were deliberately left alone in the first real sync: update them only if you rely on the fallback.
2. Tag with the convention `ffxiv-<yyyy.mm.dd>-<upstream sha8>[-rcN]`, for example `ffxiv-2026.10.23-d9b62110`: the date is the tag day, the sha8 is the upstream commit the stack sits on, `-rcN` marks a release candidate that has not been through the full play-test. An annotated tag, created locally AFTER step 1's commit, on the commit to be tested, before the build you package and smoke-test (section 7, then section 9): `git tag -a <tag> -m "<summary>"`. If the smoke test fails, delete the tag (`git tag -d <tag>`) and create it again on the fixed commit. It is pushed only in step 4, after the smoke test passed.
3. The packaging and the release steps are in [RELEASE.md](RELEASE.md). If GitHub Actions are enabled on the repository, enabling them also enabled upstream's six workflows, which are kept disabled one by one with `gh workflow disable` (exact procedure in RELEASE.md section 5). A sync can add a new upstream workflow file, and a new file starts active: before pushing, run `gh workflow list --all` and disable every workflow that is not one of the fork's own `ffxiv-*` drafts.
4. Publishing a rebased `main` moves the branch. Keep the old tip reachable first (the last release tag already does; if the tip is not tagged, `git tag archive/main-before-<sha8> main`), then `git push --force-with-lease origin <sync branch>:main` and push the new tag (the one created in step 2; it is not pushed earlier). The repository owner does this step.

## 11. Rollback

- During the sync: `git rebase --abort` (or `git merge --abort`), `git switch main`, `git branch -D sync-<sha8>`. Nothing public changed.
- After building but before publishing: delete the sync branch and the local tag (`git tag -d <tag>`); `main` was never touched and nothing public carries the tag.
- After publishing: `git push --force-with-lease origin <old tip>:main`, delete the new tag (`git push origin :refs/tags/<tag>`, `git tag -d <tag>`), and restore [UPSTREAM-BASE] from the old commit. Users who installed the new package restore their backup (see [RELEASE.md](RELEASE.md)).
