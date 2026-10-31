# Simulation tick probe: how to run it and what the line says

`[FramePacing] TickProbe` (off by default) measures where the game's own simulation tick sits relative to the frame that reaches the display. `W` and `queue` are the numbers that say whether the render side waits for the simulation or the other way round. A sleep at the start of the tick was an experiment and is not shipped.

Which number that is depends on the shape of the loop, and the line says which shape it measured. FFXIV runs its main loop on one thread: the bridge present is a piece of the tick body, so a finished simulation never waits in a queue. There `W` is the time the game thread spends inside
the native present. On a game with a separate render thread (or on this one if an overlay moves the present), the present runs outside the tick and `queue` is the old queue wait: a finished tick waiting for the render thread's previous present to end.

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
Tick probe: ticks=1432 presents=1440 period p50=7.1 p95=7.9 body p50=2.4 | age p5=9.6 p50=13.8 p95=19.2 | W p50=6.2 p95=8.0 |
queue p50=n/a p95=n/a | slack p50=0.6 | sleep->tick p50=0.6 | sleep p50=0.0 p95=0.1 (DLSS-G) | threads tick=0x6554 bridge=0x6554 same |
paired 99.9% | counter ok
```

The numbers above are illustrative. With the key off, or on a build whose anchors did not resolve, there is no install line and no window line,
and one line says why (`Tick probe: unavailable: ...`, `Tick probe: refused: ...`).

`order=` names who was first in the slot: `slot-first` means the slot already held the game's tick when the observer was written, so no other tool
owned the slot before the fork (a tool that had replaced it is refused, never chained). `entry=` shows the first 16 bytes of the function in the
running process. If they start with `E9` or `FF 25`, another tool inline-hooked the entry before the fork looked at it: harmless for the lease,
but its work then runs inside the tick body this probe measures, which is worth noting in a session report.

`counter=on` means this build resolved the game's frame counter and the numbers below are paired by it. `counter=n/a` means the frame-counter
anchor did not resolve (an inline hook over the tick entry is the usual reason): the lease still installs, and `age`, `W` and `queue` read `n/a`
rather than guessing a pairing.

## Reading one window

All times are milliseconds of CPU time, from `QueryPerformanceCounter`. One line per 10 seconds, per thread that produced events.

| Field | Meaning |
|---|---|
| `ticks` | tick calls in the window |
| `presents` | bridge presents in the window |
| `period p50/p95` | distance from one tick start to the next |
| `body p50` | how long one tick call took |
| `age p5/p50/p95` | tick start to the bridge present of the same frame: to the present's start when the present runs inside the tick (`same`), to its end when the present runs outside it (`split`) |
| `W p50/p95` | inside the tick: the game thread's time inside the native present of that tick (an upper bound, see below) |
| `queue p50/p95` | outside the tick: a tick finished, and the render thread's previous present had not ended yet |
| `slack p50` | tick start minus the last finished present; negative means the render thread was still busy when the simulation began the next tick |
| `sleep->tick p50` | tick start minus the end of the last Reflex sleep (the frame-generation present path); negative means the game thread was already running when that sleep ended |
| `sleep p50/p95` | how long that Reflex sleep lasted, measured from its own begin and end; it sits inside the present path, so inside the tick it is part of `W` |
| `threads tick/bridge` | the thread id that ran each kind, with a count in brackets when a kind ran on more than one thread |
| the word after `threads` | `same` when the window's presents ran inside the tick on its thread, `split` when they ran outside it, `mixed` when both happened, `none` when there is no present or no tick to describe |
| `paired` | share of presents that found the tick their frame number named |
| `counter` | `ok`, `missing` (the offset did not resolve) or `inconsistent` (the counter did not advance by exactly one per tick) |

`W` is an upper bound: it contains the bridge's copy and submit as well as its waits, and the probe cannot tell them apart.

Outside the tick, `queue` is the time a finished simulation spends waiting for the render thread: time nothing else can use. A
negative `queue` says the opposite (the render thread waited for the simulation).
That is why `queue`, not the spread of `age`, is the reading for that shape: a pipeline that always runs two frames deep has a nearly constant
`age` and still a large positive `queue`.

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
2. `TickProbe=true`, overlay closed: the install line with `order=slot-first`, `counter=on`, a `threads` table ending in `same`, `paired` near 100
   percent and `counter ok`; `W` and `age` are numbers and `queue` reads `n/a`.
3. Same with an overlay's framework hook on: `entry=` starts with a jump, the probe still works, and the session report says so (its work runs
   inside the tick body).
4. A loading screen, a cutscene and an alt-tab: `paired` dips and recovers, `counter` stays `ok`.
5. A session where the game build's frame-counter anchor did not resolve: `counter=n/a` on the install line, `age`, `W` and `queue` read `n/a`,
   and the period, body, slack and sleep still report.
6. Exit and restart: no hang (the module stays pinned only if the slot is still ours).

## What it cannot tell

- It is CPU timestamps. It does not measure input-to-photon latency, and `age` includes whatever the GPU needs anyway.
- `W` inside the tick is the whole native present, copy and submit included. It does not say how much of that time is a wait for the GPU or the
driver; a session that needs that split has to time the present's stages or read the GPU stage timers beside it.
- A frame the game never ticked has no pairing; `paired` is the share that did.
- A loading screen, a cutscene or an alt-tab makes `paired` dip and the metrics jump. That is expected and recovers by itself; a session report
  should say which part of the window was a scene change.
- On an AMD machine the Reflex sleep does not exist, so `sleep->tick` and `sleep` stay `n/a`.

## Cost

With the key off, one relaxed load of the mirrored key per event and nothing else: the configuration load mirrors the key, so no tap reads the ini,
takes the clock or locks anything. With it on, one `QueryPerformanceCounter` read, one counter read, one mutex and a few dozen instructions per
event, about five events per frame, and one log line per 10 seconds. The tick body the game itself measures is unaffected: the observer adds its
bookkeeping around the call, and the probe's own line reports the tick body it measured.
