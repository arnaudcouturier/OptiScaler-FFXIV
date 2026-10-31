# Release-model probe host

A small standalone program that measures, per frame-generation library build, what the library's swapchain destroy
call does to the swapchain object. The product needs that answer to know whether it may release the chain after the
destroy (`M2`) or must not touch it (`M1`); the two release orders in the existing code assume opposite answers.

It runs in its own process on purpose. The measurement creates frame-generation chains of its own and releases a
reference that may already be freed, and the XeFG destroy releases process-global state a live chain depends on. None of
that may happen in a game's process, so the in-game probe (`OptiScaler/ext/fg/outswitch/Probe.cpp`) only reads the live
chain, and the destructive half lives here.

## Build

```
pwsh -NoProfile -File tools\fg-probe\build.ps1
```

Produces `x64\fg-probe.exe` (the folder is ignored by git). The build needs the vendored FidelityFX SDK headers and
`external\xess`; it is independent of `OptiScaler.sln` and of the product build. The test recipe `fg_probe_host_build`
runs this build in the gate, so a header change that breaks the host fails there and not at the next measurement.

## Run

```
x64\fg-probe.exe --folder <folder holding the frame-generation libraries> [--out <file>] [--runs 3] [--class fsrfg|xefg|all]
```

- `--folder` is searched for `amd_fidelityfx_framegeneration_dx12.dll` (FSR FG) and `libxess_fg.dll` (XeFG, which loads
  `libxell.dll` and `libxess.dll` from the same folder). A relative path is resolved first: the loader needs a fully
  qualified one. Nothing is written or installed there.
- `--runs` is clamped to 3, the number of consistent runs a row needs, and a larger value is reported (a saved log has
  to say how many runs the row really rests on).
- `--out` also writes everything to a file, for `Update-ModelTable.ps1`. stdout is unbuffered and every line is flushed,
  so a crash or a kill keeps the runs that already happened; a library call that does not return within 90 s prints what
  it has and stops the process.

The program creates its own D3D12 device, command queue and a hidden 64x64 window. It never touches the game's window.
The library modules are unloaded after the device is released, so a library that registered something on the device is
not pulled out from under it.

## The configuration a row is measured in

A row only authorises a release rule in the configuration the product runs:

- **`config=xell` (XeFG)**: the XeFG context is created, a XeLL context is created from `libxell.dll` in low-latency
  mode, and it is attached with `xefgSwapChainSetLatencyReduction` **before** the context is initialized — the product's
  own order, and a required one: an initialize call without a XeLL context answers
  `XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED` (-15). The XeLL context lives across the three runs and is
  released after the last one, as the product releases it after its own context is destroyed. Without `libxell.dll` the
  class is not measured at all.
- **`config=direct` (FSR FG)**: the library's `ffxCreateContext` is called directly. The product reaches the same DLL
  through the FidelityFX loader and patches `Release`; neither changes what the library's own destroy does, and both are
  named here so the row is not read as "the whole product path".

## What it measures

Per class, three times, in the teardown order that class is used with (`order=` in the row):

1. create a real frame-generation chain on the hidden window and time the creation (the switch hitch);
2. put a private-data witness on the chain (diagnostic only);
3. read the chain's reference count at standstill (AddRef, read, Release);
4. **destroy-first classes** (FSR FG): destroy the chain through the library,
   and poll whether the window is free again (a plain flip chain can be created on it) until the same answer holds for
   three seconds without a change, up to nine seconds. Three seconds because a library that finishes its teardown on its
   own thread (a join with a timeout, a fence wait) keeps the window bound for a while, and a shorter hold cannot tell
   that from `M2`;
4. if the window stayed bound, release the chain once — but only after the witness agreed the object was still there at
   the end of the quiet window. A witness that says the object is gone while the window reads bound is the one case where
   the two signals disagree: the release is skipped and the run is unusable, which is safer than a release that may touch
   freed memory. The witness is read before the release on purpose: under `M2` the release is what frees the object, so a
   sample taken after it always reads "gone";
5. if the pin release happened (the `M2` direction), destroy the context a second time and record the answer: XeFG
   refuses the first destroy with `POINTER_STILL_IN_USE` while the bridge still holds the chain, so a success afterwards
   is the proof that its frame-generation context was still there and had not been destroyed (it is leaked, not freed,
   unless that second call succeeds);
6. **pin-first classes** (XeFG): release the creation reference first, check that the witness is still there (the
   provider's context holds the chain, which is what makes the release safe), destroy the chain through the library
   exactly once, then check that the window is free and the witness gone. `orderB=` stays `unknown` here: the other order
   is not measured in this mode, because a context the destroy refused while the chain was still held has no owner left
   and a second destroy on the freed chain is an access violation;
   if the pin release happened in the destroy-first mode, measure the other order on a fresh chain: release the creation
   reference first, then destroy. That order may be the leak-free one, and it is recorded separately
   (`orderB=clean|leaked`) — it is not part of the model;
7. read local video memory in use before the chain and after the teardown (`vramDeltaMb=`): a context that a refused
   destroy leaked shows up there and nowhere else;
8. run the corroborating extra-reference pass: keep one reference, destroy, release the kept reference, then the
   bridge's. A clean return is the second signal; under a page heap an `M1` library stops there. The line ends with a
   verdict token (`corroborates-M2` / `not-M2`) so it is classified, not just printed.

Three runs must agree, every poll must hold one answer for three seconds, and the witness must agree, before a row is
printed. A pin-first row additionally needs the witness alive after the release, gone after the single destroy, the
library's own destroy success, and no second destroy anywhere; `M2` needs the window bound after the destroy, free after exactly one pin release, a witness the library
accepted that stayed alive at the end of the quiet window in every run, and that witness to have died with the outer
chain (a wrapper that forwards the interface to an inner chain reports nothing about the object the release touches). A
refused witness makes the measurement unknown; the window alone never decides `M2`. The standstill count has to be the
same in every run as well. A noisy result prints `verdict=unknown`, and the switch refuses that build. That is deliberate: classifying an asynchronously-torn-down library as `M2` would make the product
release freed memory on every switch.

### Page heap

For the corroborating pass, enable a page heap on this executable only (not on the game, not on the product):

```
gflags /p /enable fg-probe.exe /full
```

and disable it afterwards (`gflags /p /disable fg-probe.exe`). The image name must be `fg-probe.exe`: gflags matches the
executable's file name, and `FgProbe.exe` would silently enable nothing. gflags needs an elevated shell and writes the
Image File Execution Options key for that image. The host prints `pageheap=on|off` on its first line and in every row
line, so a saved run says whether it was active; without it, step 5 is a plain release whose return adds little, and the
classification stands on steps 3 and 4 either way.

## Output

```
fg-probe host begin runs=3 folder=C:\... pageheap=off
fg-probe class=fsrfg run=1 createMs=812 windowAfterDestroy=bound stable=1 heldMs=3010 windowAfterPin=free stable=1 witnessAtPin=alive witnessAfterPin=died-with-object pinRelease=1 skipped=0 refs=2 destroyAfterPin=not-measured orderB=clean vramDeltaMb=0 heldBudget=1
fg-probe class=xefg run=1 destroyAfterPin code=0x00000000 (destroyed)
...
fg-probe model class=fsrfg file=amd_fidelityfx_framegeneration_dx12.dll size=12345678 stamp=65F1A2B3 config=direct verdict=M2 witness=honoured refs=2 ctx=unknown orderB=clean vramDeltaMb=0 pageheap=off runs=3 evidence=the window stayed bound until the pin release in every run
fg-probe model class=xefg file=libxess_fg.dll size=22957432 stamp=69CB0F4D config=xell verdict=M2 witness=honoured refs=2 companionFile=libxell.dll companionSize=415368 companionStamp=6A561284 ctx=leaked orderB=leaked vramDeltaMb=-3 pageheap=off runs=3 evidence=...
fg-probe host done rows=1
```

Only the `fg-probe model ...` lines matter to the table. Save the whole output, then:

```
pwsh -NoProfile -File tools\fg-probe\Update-ModelTable.ps1 -Source <saved output>
```

That rewrites `tools/fg-probe/model-rows.txt` (the reviewed record of what was measured) and
`OptiScaler/ext/fg/outswitch/ModelRows.h` (the rows the product compiles in). `-Check` verifies the two agree and is
what the test `fg_switch_model` enforces on every run, by parsing the data file and comparing it with the compiled rows.
A `verdict=unknown` line is reported and skipped; a row measured in the wrong configuration for its class is an error.

## Rules for a row

- Three runs that agree, from one library build (file name, size and PE `TimeDateStamp`), measured on the build's own
  files, in the configuration the product runs (`config=xell` for XeFG, `config=direct` for FSR FG). An XeFG row also
  carries the build of its companion DLL (`companionFile=`, `companionSize=`, `companionStamp=`): another `libxell.dll`
  next to the same `libxess_fg.dll` is another pair and is refused.
- The standstill reference count (`refs=`) is part of the row: the switch compares a live chain against it when the
  in-game probe did not run. `ctx=`, `orderB=` and `vramDeltaMb=` are what the same runs measured about the context, the
  other order and video memory; they are evidence, never a decision.
- `M1` means the destroy ends the object: the product must not release the chain after it.
- `M2` means the context held a reference: the product releases the chain exactly once.
- `order=destroyfirst` (FSR FG) releases after the destroy; `order=pinfirst` (XeFG) releases before the single destroy,
  because that library's context refuses to be destroyed while the caller still holds the chain. A row authorises its own
  order only.
- A row that the runs do not support is not written. An unlisted build is refused at run time (`NoModel`), which is the
  safe answer.

## Presenting modes

The rows above never present a chain. Two modes do, with the D3D12 debug layer and DRED always on, on a 256x256 window at
the top-left corner of the desktop that is shown without ever taking the focus (no activation, a tool window, nothing is
sent to it). A hidden window can answer `DXGI_STATUS_OCCLUDED`, and a library may then skip the present path these modes
exist to exercise. The libraries are handed a factory that records the queue each one creates its real chain on, which
is the queue the product's hooked factory sees.

```
x64\fg-probe.exe --folder <dir> --cycle [--class fsrfg|xefg|all] [--runs 1..3] [--frames 180]
x64\fg-probe.exe --folder <dir> --replay [--frames 180]
```

- `--cycle`: per class and run, create a chain on the host's own queue, present `--frames` frames that each clear the
  back buffer on that queue, wait for the GPU, tear the chain down in the class's own order (XeFG releases the caller's
  reference first and destroys once; FSR FG destroys first and releases after), then do it again on the same window. The
  line per leg says how the presents answered, whether the device survived, whether the library's chain is on a queue of
  its own (`libQueue=own|host|unknown`) and how many references that queue had left after the teardown once the host's
  own probe reference is set aside (`refsAfterDestroy=0`: the queue died with the library). A device removal prints the
  DRED breadcrumbs and page fault. The verdict line per class is `result=clean` or `FAILED`; the exit code is 0 only when
  every leg was clean.
- `--replay`: the switch's own queue rules (`OptiScaler/ext/fg/outswitch/QueueRules.h`) against the real libraries, in
  the order the benchmark's two boots take: boot with FSR FG, then FSR FG -> XeFG -> plain -> FSR FG -> plain -> XeFG ->
  FSR FG, presenting through every chain. It keeps the product's three queues by name: the interop queue, which a
  library's create moves to the library's own queue; the global queue, which follows it; and the bridge's copy queue,
  taken from the interop queue after the boot create. Before every teardown the host takes one probe reference on each
  queue the switch goes on using afterwards. A probe that turns out to be the last reference means the switch would have
  used a queue its owner had just freed: the line says which one, and the replay stops there without touching it.
  `verdict=pass` (exit 0) or `verdict=FAILED` (exit 1).

Neither mode is part of the gate: they create frame-generation chains on a real GPU. Run them by hand, under an external
timeout, from a copy of the library files.
