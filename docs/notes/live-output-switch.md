# Live frame-generation output switch: measured release rules and known limits

The switch tears down the output that carries frame generation and puts another one on the same window while the game
runs: FSR FG, XeFG or plain presentation. It is **off by default and experimental, and it has not been tried in a real
game session**; its evidence comes from the official benchmark.

**All six directions between FSR FG, XeFG and plain presentation switch live in this build** (benchmark-verified, see
"What the benchmark showed" below). A run still has to boot with FSR FG or XeFG selected: a boot with frame generation
off has no input path to switch to.

The master key `[FrameGen] FGOutputHotSwap` (menu: Frame Gen, the live output switch card) arms it. With the key on, a
present that finds the configured output different from the one on the window runs one switch: it quiesces and drains the
old provider, tears the old chain down in the order its library build was measured with, unhooks its swapchain class,
creates the new chain from the old chain's own descriptor, adopts it, restores the colour space and HDR, and judges the
new chain by its first presents. A chain that fails is put back to plain presentation and the target is remembered until
a restart or the card's "Try again". A session allows eight switches. With the key off the present path is unchanged:
one clock read and one store (the present's start, which the two present seams measure from), one configuration read
and one comparison. A refused switch (an XeFG direction this build does not take, a remembered target) is looked at
again at most once a second, not on every present.

Two diagnostic keys exist for the benchmark and are ini-only: `FGOutputSwitchScript` (for example
`45:xefg,90:none,135:fsrfg`) sets the configured output at those seconds after the first present, and
`FGOutputSwitchFault` (`drain`, `teardown`, `create`, `adopt`, `replay`, `probation`) injects one failure once to prove
that the rollback really runs. Neither does anything with the master key off.

## Reading a switch in the log

Every step is one `[fg-switch]` line at INFO, with token fields. A complete switch looks like this:

```
[fg-switch] begin gen=1 from=fsrfg to=xefg why=config
[fg-switch] preflight ok loaders=fsrfg,xefg thread=14404 resizeGen=0
[fg-switch] barrier ok waitedMs=2
[fg-switch] drain game=Drained fg=Drained ms=117
[fg-switch] teardown old chain=000001C4A2B3D4E0 class=fsrfg lib=amd_fidelityfx_framegeneration_dx12.dll/12345678/65F1A2B3 model=M2 pinRelease=yes witness=dead ms=800
[fg-switch] hooks unhooked=4 raw=6
[fg-switch] create ok chain=000001C4A2B3D4F8 hr=0x00000000 retries=0 ms=1543
[fg-switch] hooks class vtbl[8]=00007FFB2A3C1120
[fg-switch] adopt ok colorspace=0 hdr=off
[fg-switch] commit gen=1 hitchMs=2400 budgetLeft=7
[fg-switch] probation ok presents=20 ms=8000 output=fg
```

A failed step prints `FAILED step=<step> hr=0x... rollback=plain remember=<target>`; a chain that cannot be put back to
plain prints `LIMP` or `DEAD`. `lib=` is the measured library identity (file, size, PE timestamp) and `model=` the row the
teardown used.

## Why a build has to be measured

What a frame-generation library's swapchain destroy frees is not visible in its source and differs between the two paths
that already exist in this project, so the switch never guesses: a library build is used only when a measurement of it
has been written down, and an unknown build is refused.

## How a build is measured

`tools/fg-probe` builds a small host executable that loads the libraries from a folder given on the command line, creates
its own D3D12 device, queue, hidden window and chains, and prints one line per class. It is never run inside the game:
the game's process only reads its live chain (refcount, the module its present slot lives in, one test present).

Each measurement is three runs, and every run has to agree:

- The window is polled by trying to create a plain flip chain on it. The answer has to hold for three seconds without
  changing before anything is released, because a library that finishes its teardown on its own thread keeps the window
  bound for a while.
- A private-data interface is placed on the chain before the destroy and read at the end of the quiet window, before the
  pin release. It can only stop a release, never force one.
- A window that is free after the destroy means the destroy ended the object (call it M1: the extra release must not
  happen). A window that stays bound until exactly one release of the creation reference means the library's context held
  that reference (M2: that release frees it). Anything else is unknown, and unknown is refused.
- M2 is only written when the witness was accepted and stayed alive in every run. Without it the window alone would
  decide, and a wrong M2 is a release of freed memory.
- The row carries the order it was measured in. `destroyfirst` destroys, then releases the creation reference (FSR FG).
  `pinfirst` releases the creation reference first and then destroys exactly once, which is what XeFG needs: its
  `xefgSwapChainDestroy` answers `POINTER_STILL_IN_USE` while the caller still holds the chain, and the wrapper drops the
  handle, so the context is left with nobody to destroy it. A pin-first row needs the witness alive after the release,
  gone after the single destroy, the library's own destroy success, and no second destroy anywhere.

The same run also records, without deciding anything: the chain's reference count at standstill (the switch compares a
live chain against it), a second destroy after the pin release (for XeFG, which refuses the first one while the bridge
still holds the chain), the reverse order (release the pin, then destroy), and the change in local video memory across
the run.

## What the current rows say

Both rows are the vendor builds shipped with the benchmark package (2026-10-21). The companion DLL of the XeFG row is
part of the row: another `libxell.dll` next to the same `libxess_fg.dll` is a different pair and is refused.

| Class | Library | Model | Meaning |
|---|---|---|---|
| FSR FG | `amd_fidelityfx_framegeneration_dx12.dll` | M2 | the destroy drops its context's reference; one release of the creation reference frees the chain |
| XeFG | `libxess_fg.dll` + `libxell.dll` | M2, `order=pinfirst` | the same, with a XeLL context attached as the product always does, and the creation reference released before the single destroy |

## Why XeFG is torn down in the other order

`xefgSwapChainDestroy` answers `POINTER_STILL_IN_USE` while the caller still holds the chain, and the wrapper then drops
the handle: the chain is freed by the creation reference, but the frame generation context that owned it is left
allocated with nobody holding it any more. A second destroy on that handle is not a way out either: asked as a
deliberate experiment, it stops the host with an access violation (`0xC0000005`) right after the refused destroy,
before printing anything else.

The order that leaves nothing behind is the other one, and it is what the switch now uses for an XeFG row: release the
creation reference first, watch the witness stay alive (the context still holds the chain), then destroy once, and watch
the witness die with it. Nine runs in that order, in three separate invocations of the host, all reported the window free
right after the single destroy, the witness alive after the release and gone after it, the library's destroy answer
`0`, and a local video-memory delta of zero. The row's `ctx=` column says `destroyed` for that measurement; it said
`refused` when the destroy ran while the caller still held the chain.

The teardown stops instead of retrying if the witness survives that destroy (a leaked chain and context), and it never
calls the destroy a second time.

## Accepted limits of the hook rebind around a switch

- The detach holds the process-wide Detours transaction for a few milliseconds. An upstream attach that races into that
  window fails its own `DetourAttach` with `ERROR_INVALID_OPERATION` and silently misses a hook. The preflight's
  read-only look holds the same transaction for microseconds, once per attempt, for the same reason.
- The stall watch (500 ms) cannot tell a thread suspended inside the heap lock from a commit that is slow for another
  reason (a hard page fault, an antivirus scan), and the threads it resumes at the boundary would then run a prologue
  that is only half written. The constant is not lowered: it is the time a legitimate window needs on this process.

## What is not measured

- Page heap: the host prints whether a page heap is active for its image, and the procedure enables it with `gflags`
  (which needs an elevated shell and writes the Image File Execution Options key). Without it, the corroborating run
  that keeps a reference across the destroy still runs, but a use of freed memory there would not stop the process.
- The FSR FG leg calls the library directly, without the loader and without the release patch, and without the
  frame-generation context destroy the product runs first. Both are documented in `tools/fg-probe/README.md`; the
  remaining question is whether that context destroy drops its hold on the chain synchronously.
- Video memory is read per run and printed; it is not part of any decision.
- A real game session. The benchmark's executable is older than the game, has no HUD, no nameplates, no crowds, runs
  windowed only and gets no frame-generation input unless the run is set up for it, so it proves the chain swap, the
  hook rebind, the teardown and that presents continue; it does not prove the game's own thread model, exclusive
  fullscreen, game-native HDR or how frame generation looks in play. Each direction still needs a play session.
- The hitch. The switch is synchronous inside one present, and the commit line carries the measured `hitchMs`; it is a
  pause of the game, not a dropped frame count, and no promise is made that it is one frame long.
- A direction that fails a later run can be taken out on its own: the product keeps a table of the four XeFG directions
  (into XeFG from FSR FG or plain, out of XeFG to either), and a direction set off there is refused before anything is
  touched.

## Which queue a new chain is created on

Each frame-generation library creates its real swapchain on a D3D12 queue of its own, through the factory the product
hooks, and frees that queue in its destroy (the probe host's `--cycle` mode shows it for both libraries: the queue has
no reference left once the destroy and the caller's release are done). The hooked factory points the D3D11 interop's
queue, the one the upscaler's evaluate thread and the D3D11 to D3D12 synchronisation use, and the process's global queue
at it. The bridge takes its own copy queue from the interop after the boot library's create, so at a frame-generation
boot the bridge copies on the boot library's queue.

An earlier build of the switch created the next chain on the global queue and left the interop where the library's
create had put it. Both were then the old library's queue, which its own teardown freed: the first present after an
XeFG teardown found the device removed, and the upscaler's next submission was made on freed memory. FSR FG to anything
probably seemed to work only because the bridge's GPU timers hold a reference on the bridge's queue (not measured). The
switch now:

1. takes one reference of its own on the bridge's copy queue at its first capture, before anything is torn down, and
   takes it again whenever the bridge's copy queue changes (a bridge the game recreates, even at the old one's address,
   has the new boot library's queue);
2. creates every chain on that queue: the plain chain, and the game queue a provider is created with;
3. gives the interop back the queue it had once a library's create returned.

The same build created the provider without the factory guard boot uses, so the library's own inner create was taken
for a new game swapchain and ran the provider's create a second time from inside the first. For XeFG the log shows it:
a second initialisation of the context it was initialising failed (`D3D12InitFromSwapChainDesc error` in the log of
every live XeFG create, never at boot). For FSR FG the code path nests a second frame-interpolation swapchain under the
first; the driver faults a few seconds after a live FSR FG create are consistent with that, but no run separated it
from the queue defect (both were fixed together). The provider is now created exactly as boot creates it.

One line per create names every queue involved:

```
[fg-switch] queues pinned=1D9804E7D70 creation=1D9804E7D70 interopBefore=1D9804E7D70 libraryLeft=1DB19A89600 interopNow=1D9804E7D70 providerQueue=1D9804E7D70
```

`creation`, `interopNow` and `providerQueue` equal `pinned`; `libraryLeft` is the new library's own queue, which nothing
of the switch's uses. The probe host's `--replay` mode runs these rules against the real libraries in the six
directions of boots A and B below (a boot with FSR FG).

## What the benchmark showed

Two boots with FSR FG and a switch script: boot A switches to XeFG, plain, FSR FG, XeFG and FSR FG; boot B to plain,
XeFG and FSR FG. Two boots with XeFG: boot C switches to plain, FSR FG, XeFG, FSR FG and plain; boot D to FSR FG, XeFG,
plain, XeFG and FSR FG (the first teardown in both is the chain XeFG created at boot). Every switch committed and passed
its 20-present probation, the game presented to the end and exited normally, and neither the log nor the system
recorded a device removal or a driver fault. The hitch of a switch was about 45-265 ms on that machine (a switch to
XeFG is the longest: its library's create takes about 150 ms). The
rollback to plain presentation was proven with a forced failure at the create, at the adopt and in the probation of a
switch to XeFG (the last one tears the committed XeFG chain down again); after each, plain presents continued, the
failed target stayed refused for the session, and a later switch from plain to FSR FG committed normally.

A rollback after a failed probation (and a limp) creates the plain chain from the descriptor the last switch read on
the window. An earlier build took the process's global descriptor instead, which the frame-generation library's inner
chain rewrites on every present; after a rollback from XeFG the plain chain then carried XeFG's frame-latency waitable
flag, and every later switch was refused.

The windows of these runs were covered and never in front (the runs must not take input), so displayed-frame intervals
were not recorded; the game's own per-second counter and the fork's present trace are the interval evidence.
