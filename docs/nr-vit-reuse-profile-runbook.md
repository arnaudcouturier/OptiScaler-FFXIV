# Profiling a new NR model file for ViT bottleneck reuse

Audience: whoever has to make `VitEvery=2` work for a new `nvngx_dlssnr.dll` (the next agent, S9b, or you). This runbook says how to collect the evidence, what the evidence can and cannot show, and how to register the result. Nothing here makes a model "safe" by itself: the table row you add at the end is a statement by a person that the invariants below were checked.

## 0. Why this exists

ViT reuse (`[DlssNr] VitEvery=2`) drops the `cc_vit_1d_*` CUDA launches of every second NR evaluation and leaves the previous result in the run's output buffer. It only activates for a **verified** module: the CUDA blob passed to `NvAPI_D3D12_CreateCuModule` must match a row of `OptiScaler/ext/nr/native/DlssNrModuleProfiles.h` by size and SHA-256. Today there is exactly one row, the 3,202,680-byte module of the original model. A different model file produces a different module, the live log then says

```
NR reuse module: bytes=1417240, sha256=2a05b056..., verified=false, hybrid=false, first kernel=cc_vit_1d_ffn_expand_fp8
NR reuse: unverified NR kernel module; compatibility profile needed; evaluations=1, ... computed 0, reused 0
```

and reuse is a no-op. The only way around it is `VitAllowUnverified=true`, which skips the fingerprint check and nothing else: the code never looks at kernel parameters, so a wrong guess can hang the GPU (a kept kernel spinning on a counter that only a dropped kernel would signal) or silently show stale or garbage frames. A profile replaces that guess with evidence.

State when this runbook was written (2026-10-16): the user's model file is `nvngx_dlssnr.dll` SHA-256 `cdfa6c1ec7848671...` (276,162,560 bytes); its ViT module is 1,417,240 bytes, SHA-256 `2a05b056783f74c602b7a7a53d48629547054cd9b23a4c75a92bc2ce77d6d9c3`. **No row exists for it and this runbook does not claim it is safe.** From the log alone (`tools\nr-vit-capture-summary.ps1 -Log OptiScaler.log`): 42 `cc_vit_1d_*` launches per evaluation, identical in the two logged evaluations (`repack_2d_to_1d`, `ffn_expand_publish`, then groups of `ffn_contract_chained, qkv_chained, attention_chained, projection_chained, ffn_expand_chained` x7, then `ffn_contract_chained, qkv_chained, attention_chained, projection_wait`, `repack_1d_to_2d`); names-only order hash `87d8a2b907aa567a1cd449eccca15e743e88f728c88b9e33dc2082ce25c0d787`. A capture of the same module must reproduce that hash for its `cc_vit_1d_*` family.

## 1. What the reuse filter assumes (the invariants to confirm)

`DlssNrVitReuse.h` (header comment, lines 10-12, and the `Filter` state machine) rests on these. Each needs evidence for the new module:

| # | Invariant | Where it comes from | What the capture + summarizer can show | What it cannot show / what closes the gap |
|---|---|---|---|---|
| A | The module is exactly the one profiled | `Classify(bytes, sha256)` | `MODULE` record: size, SHA-256, first bytes | Nothing else: a row is per blob. The same model file may hand a different blob to a different GPU architecture, so a row verified on one GPU does not cover another |
| B | The run is one contiguous `Start ... End` of `cc_vit_1d_*` kernels in the same order every evaluation | `Filter::Drop`, "unexpected NR launch order; restart required" | Summary checks `run-N`, `contig-N`, `sig-names`; the signature hashes | Order at other settings: repeat the capture with a second resolution/quality (a new capture folder per session) and compare with `-Baseline` |
| C | **Sync counters of the run are referenced by no kernel outside it**, so nothing kept can wait on something dropped | header comment, "checked on a real capture" | Section (c): pointer values used by Inner kernels and also outside (`LEAK`), check `inv1-leak` | Which pointer is a counter, and whether any outside kernel waits on a flag a run kernel sets: needs the kernel code (section 6) |
| D | **The run's output buffer is written only by its last kernel** (so a reused result is a complete result) | header comment | The End kernel's pointer slots and where else those pointers appear (class `boundary` = End and outside only; a pointer also used by Start/Inner is suspicious) | Read versus write: parameters carry no direction. SASS (stores to that parameter's address) or a probe |
| E | The End kernel's output buffer is the same every evaluation and nothing else overwrites it between evaluations | implicit: a reused result is only valid if it is still there | Check `end-stable` (pointer slots identical across evaluations); pointer classes show other users of the same address | A buffer that is aliased by other kernels' scratch under a different pointer value (sub-allocation offsets) is invisible. A visual A/B test catches the symptom, not the cause |
| F | Skipping the run does not desynchronize state the run carries across evaluations (recurrent buffers, epoch counters) | implicit | Pointers that are `run-private` and `Stable=yes` are candidates for carried state | Needs the kernel code, and the in-game A/B |

The summarizer's PASS lines are necessary conditions, not proof. Its limits are printed at the end of every report.

## 2. Collect: a safe capture session

`VitCapture=true` only observes. It records launches as the runtime issues them (before any drop decision), changes no launch, parameter or timing, and works with the override off. **Keep `VitAllowUnverified=false` during the capture session**: with it on, reuse would actually drop kernels of the unprofiled module.

1. Use a build that contains `VitCapture` (fork commit `[B2] nr: VitCapture ...` or later). Copying it to the game is the user's step; agents never do that.
2. In the game's `OptiScaler.ini`, section `[DlssNr]`:
   - `Enabled=true`
   - `VitCapture=true`
   - `VitAllowUnverified=false`
   - `Precision=0` (the hybrid path replaces the FFN kernels and ends the capture's view of them; reuse and hybrid are mutually exclusive anyway)
   - `Passes=1` (a second pass is a second feature and spends the 3-evaluation budget; its `VitEvery` is 1 and it is only watched)
   - `VitEvery` may be `1` or `2`; with `1` the evaluations are watched only (nothing is dropped either way on an unverified module). In both cases the log also gets the usual `NR reuse unverified launch: kernel=..., params=...` lines of the first two evaluations, a second, independent record of the launch order (and the input of `-Log`).
   - Set these **before the game starts**. The runtime creates its CUDA modules when NR starts; a menu toggle later captures launches but no module dump and no kernel names for functions created earlier. There is deliberately no menu checkbox for that reason.
3. Start the game, get to a scene where NR is visibly active (it needs the DX12 NR path), play a few seconds and quit normally.
4. Expected in `OptiScaler.log`:
   ```
   NR ViT capture: recording the first 3 NR evaluations (observation only) into <game>\OptiScaler\nr-vit-capture\<date-time>
   NR ViT capture complete: 3 evaluations, 1 module dumps, folder ...
   ```
   Nothing is written if NR never ran three times; the summary then warns that no `DONE` record exists.
5. Collect the whole folder `<game>\OptiScaler\nr-vit-capture\<YYYYMMDD-HHMMSS>\` (`capture.txt` and `modules\<sha256>.bin`) and the `OptiScaler.log` of the same run. The dumps are NVIDIA's binary: keep them out of git and do not redistribute them.

Bounds (all constants in `DlssNrVitCapture.h`): 3 evaluations, 512 launches per evaluation, 512 parameter bytes per launch, 4 MiB buffered text, at most 6 module dumps of at most 16 MiB, the 5 newest run folders kept (older ones that match the `YYYYMMDD-HHMMSS` pattern are deleted when a session writes its first file; nothing else is ever deleted). The folder can be deleted by hand at any time. If a file cannot be written the capture stops and says so in the log and in a `NOTE` record; the game is unaffected.

### Capture file format (version 1)

One record per line, `TAG key=value key=value ...`; values never contain spaces; numbers are decimal unless they start with `0x`; hex is lowercase; lines starting with `#` are comments. Full definition: header comment of `OptiScaler/ext/nr/native/DlssNrVitCapture.h`.

| Tag | Fields |
|---|---|
| `CAPTURE` | `format=1 evaluations_max launches_max param_bytes_max run` |
| `MODULE` | `handle bytes sha256 magic` (first 4 bytes, `7f454c46` = ELF cubin) `dump` (path under the run folder or `-`) `status` (`dumped`, `duplicate`, `too_large`, `dump_limit`, `no_digest`, `no_folder`, `write_failed`, `blob_not_retained`, `error`) |
| `FUNC` | `fn module kernel role` for each `cc_vit_*` function the runtime created |
| `EVAL` | `n feature reset every override observe_only` |
| `LAUNCH` | `eval ord fn kernel vit role abi grid block shared psize pmode pbytes trunc [kp] params`; `ord` counts every launch of the evaluation; `vit=1` = name starts with `cc_vit_`; `role` = what `RoleOf` says; `abi=basic|ex`; `pmode=blob` (raw bytes in `params`), `ptrs` (EX launch that only carries the `kernelParams` pointer array: its size is unknowable, it is recorded as `kp=0x..` and never read), `none`; `params` = first `pbytes` bytes in hex |
| `END` | `eval launches recorded skipped success` |
| `DONE` / `NOTE` | `evaluations launches` / `text` |

All launches of the captured evaluations are recorded, not only the `cc_vit_*` ones (those are flagged `vit=1`). Invariant C is about kernels outside the run, which cannot be judged without their parameters. Kernels created before the capture was switched on, or by a module the runtime created earlier, show up as `kernel=?`.

## 3. Summarize

```
pwsh -File tools\nr-vit-capture-summary.ps1 -Capture <run folder | capture.txt | nr-vit-capture folder>
pwsh -File tools\nr-vit-capture-summary.ps1 -Log <OptiScaler.log>          # names/sizes/geometry of the first two evaluations only
pwsh -File tools\nr-vit-capture-summary.ps1 -Capture <run> -Baseline <other capture or log>
```

Options: `-Strict` (exit 1 if a check FAILs), `-PassThru` (also returns an object), `-AllKernels` (include non-ViT kernels in table b), `-ShowUnlikely`, `-ClusterMin`, `-PointerMin`/`-PointerMax`, `-MaxRows`. The report:

- **(a)** the `cc_vit_*` order of each evaluation (repeated groups compressed) and three SHA-256 values: names only, names + paramSize, and the `cc_vit_1d_*` family alone (what the log lists; compare it with `-Log` output of the same session as a cross-check of the capture). Grid is left out of the hashes on purpose: it follows the render resolution (the live log shows `repack_2d_to_1d` at 2176 groups and `repack_1d_to_2d` at 2160).
- **(b)** per kernel: launches per evaluation, role, parameter size(s), grid, block, shared memory.
- **(c)** 8-byte values that look like device pointers and appear in two or more launches of the first evaluation. A value at or above 2^32 only counts as `ptr` when several distinct values share its high dword (device pointers come in bulk; a packed width/height pair does not); `lo?` is below 4 GiB, 16-byte aligned; `int?` (hidden unless `-ShowUnlikely`) is probably a packed integer pair. The device address range of this GPU/driver was **not** known when the script was written: look at the printed high-dword histogram and set `-PointerMin` if the clusters look wrong. Classes: `run-private` (only inside Start..End), `boundary` (Start/End and outside), `LEAK` (an Inner kernel and an outside kernel), `outside the run only`, `unknown` (no outside launches captured). Also the pointer slots of the End kernel and whether they are stable across evaluations.
- **(d)** the checks (`PASS`/`WARN`/`FAIL`/`INFO`), the 72-byte FFN layout hypothesis (see below), and the comparison with the original module.
- **(e)** (added with the per-feature judgement) a capture made with `Passes = 2` holds pass 0, pass 1, pass 0, i.e. two NR pass features with their own buffers (`EVAL feature=` is the ordinal of the pass feature in order of first use). Section (c) and the checks of (d) are therefore computed per feature, and a feature needs two or more evaluations to be judged. New checks: `end-stable` (End slots 0/1 and Start slot 1 over the feature's own evaluations only), `start-end-link` (does `Start.p1` equal `End.p0` inside one evaluation; informational), `waiters-outside` (a LEAK pointer shared with an outside kernel whose name ends in `_wait`, `_chained`, `_tilesync` or `_publish`, after dropping a precision suffix such as `_fp8`; the FAIL means section 6), `clear-before` (`cc_cb_clear` launched before Start in every evaluation and its pointer is one of the LEAK pointers) with an informational `clear-range`, `epoch` (a 4-byte value in an Inner kernel's parameters that differs between two evaluations of the same feature), `end-users` (every launch outside the run that carries End slot 1 or Start slot 1, with its position and kernel name) and `end-xfeature`. A LEAK pointer whose only outside sharer is `cc_cb_clear` is labelled `sentinel-clear`. The leak table lists every outside sharer by kernel name. Finally a `RULE <verdict> feature N` block applies the fixed go/no-go rule of the NR reuse study to the feature used first (pass 0): `GO`, `NEEDS-DECODE` (everything passes, but a launch after End that carries End slot 1 must be shown to only read it), `NEEDS-NVDISASM` (no final failure, but an outside waiter or publisher shares a LEAK pointer), `NO-GO`, or `NOT-JUDGED` (fewer than two evaluations). A condition that cannot be evaluated from the files counts as not satisfied.

**Comparison with the original verified module.** There is nothing to diff against: no capture of the 3,202,680-byte module's launch order exists in the repository, the tests or the ledgers (searched 2026-10-16). What the code does fix is structural (one Start, one End, only `cc_vit_1d_*` kernels between, from `DlssNrVitReuse.h`) and, for the hybrid path only, the three FFN kernels `cc_vit_1d_ffn_expand_publish_fp8`, `..._expand_chained_fp8`, `..._contract_chained_fp8` with 72-byte parameters and geometry `expand grid.x = 32*ceil(M/128)`, `contract grid.x = 8*ceil(M/128)`, `z = 4`, block `32x4x1` (`DlssNrNative.cpp`, hybrid launch handler). The summary checks those and prints the slot relations that handler relies on (expand: `p0` x, `p2` y, `p7` done; contract: `p0` y, `p1` x, `p2` output, `p4` counter, `p6` expand's done, `p7` done) as a **hypothesis** for the new module. The live module's geometry already fits the rule (expand 544 = 32*17, contract 136 = 8*17, z 4, block 32x4x1, 72-byte parameters), but that says nothing about reuse safety and the hybrid path stays unverified for it (`hybridVerified=false`).

## 4. Reading the result

- Any `FAIL`: stop. Do not add a row. Understand it first (`contig` = a foreign kernel inside the run, which the filter would also reject; `sig-names` = the order varies; `inv1-leak` = a pointer shared between the run's Inner kernels and an outside kernel).
- A `LEAK` row is not automatically fatal: a clear/zero kernel outside the run that touches the run's counters is harmless when the run is dropped and the clear still runs, whereas an outside kernel that *waits* on a run-produced flag is a hang. The parameters cannot tell those apart. Treat every LEAK as needing section 6.
- `WARN` on `blob`, `outside`, `complete*`: the evidence is incomplete; fix the capture (a `ptrs`-mode EX launch cannot be analysed at all) rather than argue around it.
- All PASS: invariants B, parts of C, D and E are consistent with a profile. Still do section 5 before registering; C and D are not established by parameters alone.
- The `RULE` line of section 3 (e) is the short answer for pass 0: only `GO` allows moving to section 5; `NEEDS-DECODE` and `NEEDS-NVDISASM` mean section 6 first; `NO-GO` means stop and keep reuse off for that module. A `NO-GO` that comes from `clear-before` or from an ordinary (non-waiter) outside sharer is a fact about pointer values, so reading the kernel code does not change it.
- Example (the five start-up captures of module `2a05b056...`, 1,417,240 bytes, `Passes = 2`): pass 0 is `NO-GO` in all five: five LEAK pointers are shared with 27 outside `_chained`/`_wait`/`_tilesync` kernels, none is `sentinel-clear`, and the pointer of `cc_cb_clear` is none of them. Pass 1 has a single evaluation per capture and is `NOT-JUDGED`.

## 5. Decide, register, test, roll back

1. **Look at the code when in doubt.** Section 6 says when it is required. For the first profile of a new model line, treat it as required.
2. **Add the row** (one commit, `[B2] nr: ...`): in `OptiScaler/ext/nr/native/DlssNrModuleProfiles.h` append to `kProfiles`
   ```cpp
   { <bytes>, "<sha256, 64 lowercase hex, from the MODULE record or the 'NR reuse module:' log line>", true, false,
     "<model file name + sha, GPU, date, resolution(s) captured, which invariants were checked and how>" },
   ```
   `reuseVerified=true, hybridVerified=false`: the hybrid rewrite hard-codes one module's ABI and stays off for the new module (even with `Precision=4` it takes the "unsupported original module hash" fallback). One row per blob; a different GPU architecture or model update is another row.
3. **Tests.** `tests\ext\nr\nr_module_profiles.cpp` pins the current state on purpose, in two places: it asserts `2a05b056...` resolves to neither flag, and it asserts `std::size(kProfiles) == 1`. Adding the row means changing both in the same commit (the new module: reuse true, hybrid false; table size +1) and saying why in the message. Then `pwsh -File tools\build-locked.ps1` and `pwsh -File tools\test.ps1` (nothing may regress; the table hygiene check rejects duplicates, uppercase or short digests).
4. **In-game check** (user's step): `VitEvery=2`, override **off**. Expected log: `NR reuse module: ... verified=true, hybrid=false`, then `NR reuse: verified kernel module active; evaluations=..., computed N, reused M` with `reused` growing. Watch for `unexpected NR launch order; restart required` (the filter switched itself off: the order differs from what was profiled, e.g. at another setting), for hangs, and for stale detail in motion (the feature's known cost). Compare frames with the existing `dlssnr-capture.trigger` mechanism if needed.
5. **Roll back**: `VitEvery=1` (or `auto`) stops dropping on the next evaluation (menu: untick "Reuse NR bottleneck", applies live). To withdraw the profile: revert the row commit; the module is then unverified again. If the GPU hangs on the first test, remove `VitEvery` from the ini before the next start.

## 6. When the kernel code has to be read

Needed whenever the parameters cannot answer: any `LEAK`; invariant D (read versus write) and C (which pointers are counters, who waits on what) for a first profile of a model line; any doubt about carried state (F). The capture dumped the module to `modules\<sha256>.bin`; `magic=7f454c46` means a plain ELF cubin. Disassemble it with NVIDIA's `nvdisasm` (cubin) or `cuobjdump -sass` (cubin or fatbin) from the CUDA toolkit; for each `cc_vit_1d_*` kernel look for atomics/reductions (`ATOM*`, `RED*`), memory barriers (`MEMBAR`), strong or volatile loads in a loop (spin-waits), and stores (`ST*`) through the address loaded from a kernel parameter (parameters are read from constant bank 0 at an architecture-dependent offset; match them to the 8-byte slots of the `params` hex). **Neither tool is installed on this machine. Do not download anything: ask the user first, naming the file, the source and the size, as the standing rule requires.** Without the tools the remaining option is an in-game probe, which can only show symptoms.

## 7. Known gaps (read before trusting a result)

- Not verified in a game at the time of writing: that the blob handed to `NvAPI_D3D12_CreateCuModule` names its kernels in clear (the dump relies on that, with a one-blob fallback copy for a module created right before its first function, which the live log shows is how the ViT module is created). The `magic` field tells you what the blob is.
- The device address range is unknown; pointer detection is a heuristic.
- Three evaluations after start-up may all be at one resolution and include a history reset (`reset=1` in `EVAL`). Capture a second session at another setting before registering.
- Parameters never show direction, counters versus data, or kernel semantics.
- A row says nothing about image quality; reuse trades stale detail for GPU time by design.
