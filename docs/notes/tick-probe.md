# Simulation tick probe: how to run it and what the line says

`[FramePacing] TickProbe` (off by default) measures where the game's own simulation tick sits relative to the frame that reaches the display. It
exists to answer one question before anything is built on it: does the simulation finish early and wait for the render thread, or does the render
thread wait for the simulation? Only the first case is worth acting on, and the number that separates the two is the queue wait `W` below.

Nothing about the feature is guessed from a single session's numbers. The probe is a measurement, and a session that reads badly is a normal
outcome.

## Why the tick, and why the vtable

The game runs one virtual function per frame on its simulation thread: slot 4 of the `Framework` object's vtable. Hooked at its entry, that
function composes badly with any other tool that inline-hooks the same entry (a framework hook from an overlay is a common case), because the
overwriting tool leaves no room for a second patcher and the fork's own resolver refuses a function whose first bytes are a jump.

So the fork leases the vtable slot instead of the code: it replaces the pointer with its own function, keeps the pointer it replaced, and calls it.
An inline hook over the entry is then simply a function that our function calls, in either installation order. The write is one
compare-exchange, and it only happens after the checks in `ext/latency/TickLeaseCore.h`: every pointer the install reads is committed, readable
memory of the expected kind (a pointer to unmapped memory is refused before it is dereferenced), the vtable pointer is read-only image data, the
slot already holds the tick this build expects, the counter offsets that resolved have the layout the tick's code encodes, the module is pinned,
and the page can be made writable. Any check that fails ends in a log line and today's behaviour, never in a partial install.

## Turning it on

```ini
[FramePacing]
TickProbe=true
```

There is no menu control. The key applies live: the configuration load mirrors it, so switching it off turns the observer into a pass-through, and
the lease itself stays where it is. The probe starts recording at the first bridge present after the lease is installed; a tick that was already
running at that moment is not recorded (it would be half a frame), and the next tick pairs normally.

Expected log lines, in order:

```
[ffxiv-gate] feature latencyTick ON all 2 required checks pass
Tick probe: installed instance=0x... vtable=0x... slot=0x... original=0x... order=slot-first counter=on entry=48 8B 5C 24 08 48 89 74 ...
Tick probe: ticks=1432 presents=1440 period p50=7.1 p95=7.9 body p50=2.4 | age p5=9.6 p50=13.8 p95=19.2 | W p50=3.1 p95=5.0 | slack p50=-1.2 |
sleep->tick p50=-0.4 (DLSS-G) | threads tick=0x1a3c bridge=0x2b10 | paired 99.9% | counter ok
```

The numbers above are illustrative. With the key off, or on a build whose anchors did not resolve, there is no install line and no window line,
and one line says why (`Tick probe: unavailable: ...`, `Tick probe: refused: ...`).

`order=` names who was first in the slot: `slot-first` means the slot already held the game's tick when the observer was written, so no other tool
owned the slot before the fork (a tool that had replaced it is refused, never chained). `entry=` shows the first 16 bytes of the function in the
running process. If they start with `E9` or `FF 25`, another tool inline-hooked the entry before the fork looked at it: harmless for the lease,
but its work then runs inside the tick body this probe measures, which is worth noting in a session report.

`counter=on` means this build resolved the game's frame counter and the numbers below are paired by it. `counter=n/a` means the frame-counter
anchor did not resolve (an inline hook over the tick entry is the usual reason): the lease still installs, and `age` and `W` read `n/a` rather than
guessing a pairing.

## Reading one window

All times are milliseconds of CPU time, from `QueryPerformanceCounter`. One line per 10 seconds, per thread that produced events.

| Field | Meaning |
|---|---|
| `ticks` | tick calls in the window |
| `presents` | bridge presents in the window |
| `period p50/p95` | distance from one tick start to the next |
| `body p50` | how long one tick call took |
| `age p5/p50/p95` | tick start to the end of the bridge present that carries the same frame number: the age of the frame at present time |
| `W p50/p95` | the queue wait: a tick finished, and the render thread's previous present had not ended yet |
| `slack p50` | tick start minus the last finished present; negative means the render thread was still busy when the simulation began the next tick |
| `sleep->tick p50` | tick start minus the end of the last Reflex sleep (the frame-generation present path); negative means the game thread was already running when that sleep ended |
| `threads tick/bridge` | the thread id that ran each kind, with a count in brackets when a kind ran on more than one thread |
| `paired` | share of presents that found the tick their frame number named |
| `counter` | `ok`, `missing` (the offset did not resolve) or `inconsistent` (the counter did not advance by exactly one per tick) |

`W` is the number a sleep at tick start could give back: a finished simulation that sits in a queue is time nothing else can use. A negative
`W` says the opposite (the render thread waited for the simulation), and a sleeping tick would then make latency worse, not better. That is why
`W`, not the spread of `age`, is the reading to trust: a pipeline that always runs two frames deep has a nearly constant `age` and still a large
positive `W`.

Ticks and presents are numbered by one source, the game's own `Framecounter`: the observer reads it before and after every tick, the bridge present
reads it when the present begins. A present is paired with the tick whose frame number it carries, so there is no lag to infer and a deeper
pipeline reads correctly. `paired` is the honest limit of everything built on the pairing: a present whose frame number has no recorded tick (the
window after arming, a loading screen that skipped a tick) contributes nothing, and the window says `counter missing` or `counter inconsistent`
instead of printing a number when the counter cannot carry the pairing at all.

A session is a steady 120 second stretch in one scene, once per frame-generation provider, with the overlay closed (a visible overlay forces
desktop composition and wrecks present timing). Run one session without an overlay or framework hook and one with it, and compare `paired`,
`counter` and `threads` first: those are what tells whether the extra tool moved the tick to another thread or put its work inside the measured
tick.

## Play checklist rows for this build

1. Default keys: no `tick.hook` line, no slot write, behaviour unchanged.
2. `TickProbe=true`, overlay closed: the install line with `order=slot-first`, `counter=on`, a `threads` table, `paired` near 100 percent and
   `counter ok`; `W` and `age` are numbers, not `n/a`.
3. Same with an overlay's framework hook on: `entry=` starts with a jump, the probe still works, and the session report says so (its work runs
   inside the tick body).
4. A loading screen, a cutscene and an alt-tab: `paired` dips and recovers, `counter` stays `ok`.
5. A session where the game build's frame-counter anchor did not resolve: `counter=n/a` on the install line, `age` and `W` read `n/a`, and the
   period, body and slack still report.
6. Exit and restart: no hang (the module stays pinned only if the slot is still ours).

## What it cannot tell

- It is CPU timestamps. It does not measure input-to-photon latency, and `age` includes whatever the GPU needs anyway.
- A frame the game never ticked has no pairing; `paired` is the share that did.
- A loading screen, a cutscene or an alt-tab makes `paired` dip and the metrics jump. That is expected and recovers by itself; a session report
  should say which part of the window was a scene change.
- On an AMD machine the Reflex sleep does not exist, so `sleep->tick` stays `n/a`.

## Cost

With the key off, one relaxed load of the mirrored key per event and nothing else: the configuration load mirrors the key, so no tap reads the ini,
takes the clock or locks anything. With it on, one `QueryPerformanceCounter` read, one counter read, one mutex and a few dozen instructions per
event, about five events per frame, and one log line per 10 seconds. The tick body the game itself measures is unaffected: the observer adds its
bookkeeping around the call, and the probe's own line reports the tick body it measured.
