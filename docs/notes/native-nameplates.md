# Native nameplates: what it does, how the image is produced, and what it cannot promise

The game draws its nameplates once per game frame. Frame generation puts two to five outputs on screen per game frame, so
the world glides while the plates step once per real frame. This feature takes the game's own nameplate draws out of the
frame, draws them into a plate-only image on the same GPU, and draws that image back over every output. This build draws
the image back unchanged (`endpoint`): it has no plate tracker, so `PlateInterp = on` behaves exactly like `endpoint` and
nothing is moved. The picture with the key on is the same as with it off; what changes is how the plates reach the screen.

Status: **new, off by default, unit-tested on a software adapter, never tried in a game.** What is proven where is at the
end of this file.

## Config

`[HudLess] PlateInterp` (`off` | `endpoint` | `on`, default `off`): the switch, live from the menu card "HUD-less (UI-free
frame generation)" of the FFXIV tab under "Native nameplates". `endpoint` takes the plate draws out of the game's frame and
draws them back unchanged. `on` is accepted for the next stage: with no plate tracker in this build it draws exactly what
`endpoint` draws and moves nothing.

`[HudLess] PlatePhaseOffset` (`auto` | `0` | `1`, default `auto` = 1): for the plate tracker stage, the constant `c` of the
per-output share `s = (k + c) / multiplier`, i.e. which output of a batch shows the older plate position. `auto` assumes
the generated frames come first and the real frame last. No effect in this build.

`[HudLess] PlateMaxStep` (0..256, default 24): for the plate tracker stage, the largest distance a plate may be moved
between two outputs, in output pixels. No effect in this build.

Any other value of any of the three reads as the default. With `PlateInterp = off` nothing is reserved, redirected,
published or logged: the key costs one atomic load at each seam, one atomic test in the `DrawIndexed` hook (a few hundred
to a few thousand per frame), the nameplate pixel-shader stamps taken once at shader creation, and the engine object
itself, a few KB allocated once at load.

## What it needs

DLSS-G as the output, SDR output, and a multiplier of two or more (the game's frame is one of several outputs, so there is
something to interpolate). The game must be running through the D3D11-to-D3D12 bridge, because the plate image is a shared
texture the D3D12 output pass samples. When any of that stops holding, the gate closes at the next present and the plates
go back to the game's own draws.

## How a frame is produced (the protocol)

1. **The game draws.** At the frame's first draw that looks like a nameplate draw, the game side first reads the draw
   state and tests every condition below. Only then does it ask the engine for a slot of the frame's size and format.
   The engine answers one of three ways: **declined** (no slot, the wrong size, the wrong format, the gate is closed or
   nothing could consume the image: the frame stays fully native), **shadow** (the image is built and the frame is also
   drawn natively: the warm-up proof) or **hidden** (only the image is drawn).
2. **The plate triangles** go into the reserved slot on the game's own immediate context, with the game's depth view,
   blend, shaders and vertex data. The slot is cleared once, before the frame's first plate triangle. There is no copy and
   no keyed mutex: the same device, the same context, the same command stream.
3. **The frame finishes** at the present seam. The engine freezes the frame, signals a fence on the frame-generation queue
   and publishes **exactly one record per present**: Native (nothing captured, which also takes an older hidden image off
   the screen), Shadow or Hidden.
4. **The output pass** draws the newest hidden record's image over every output, and a shadow record with zero instances
   (which proves the pass and the queue ordering without changing the picture). It pins the slot it reads in the same lock
   section that copies the record, so the game can never start writing that slot under it. Every output waits for the
   produce fence on the GPU, so the CPU never blocks on the plates.
5. **Slots.** Three of them, at most one created per present, none created inside a draw. A slot is written only when it
   is free, not drawable, not reserved, not pinned and every reader fence of the previous frames has completed. Slots
   exist only while a capture can be consumed: they are created when the key is on and the output is DLSS-G, SDR, with a
   multiplier of 2 or more, and they are handed back (behind their fences) once the key is off, nothing is drawable any
   more and every reader of every slot has completed. The engine keeps its feature bit until the hand-back has finished,
   so it completes on a later present instead of leaving a texture behind. At 4K one BGRA8 slot is 33 MB, so three are
   about 100 MB, with no per-frame copy.

Two locks only: one pass mutex (the presenting threads) and one leaf mutex (the slot table and the drawable record), taken
in that order. Nothing that can block, log or call back into the plate code runs while the leaf is held.

### How a draw is classified

A draw is a nameplate draw only when **all** of these hold, read from the live D3D11 state at that draw:

- it runs on the game's own immediate context;
- exactly one colour target is bound, of an 8-bit SDR format, not multisampled;
- no unordered access view is bound;
- a depth-stencil view is bound, of the colour target's size, with the depth test on, `GREATER_EQUAL`, no depth write,
  no stencil;
- the blend state is the standard alpha blend: colour `SRC_ALPHA` / `INV_SRC_ALPHA`, alpha `ONE` / `INV_SRC_ALPHA`, both
  `ADD`, all channels written, no alpha-to-coverage, no independent blend;
- the pixel shader is one of the four nameplate variants (stamped at shader creation from its bytecode CRC), and the input
  layout carries one of the game's three nameplate vertex-shader tags.

The rule fails closed: a draw that does not match every condition is drawn by the game and is never taken into an image,
so a wrong condition costs the feature, never a plate. What it cannot tell apart are draws that share the whole state with
a nameplate draw; those are moved into the image whole. The `[HudLess] PlateDiagnostics` draw census is what says whether
that happens in practice: it logs the depth, blend, target and shader state of every UI draw.

## What it promises, and what it cannot

The promise, in the wording this feature stands behind:

- the pass never draws a plate image for a frame the game drew natively;
- the pass never draws two records on one output;
- the game's own plates and the pass can both appear, only in outputs generated from a native frame, and at most one batch
  per hide/unhide transition (frame generation builds a batch from two real frames, so the older one can be in the
  generated outputs while the pass draws the newer one: cosmetic, one batch);
- plates can be missing for at most the outputs of one game frame, and only after one of the listed events below, plus the
  late and early outputs of a hide/unhide transition while the pacer lags.

The earlier wording ("never drawn twice, never vanish while hidden") was too strong at a hide/unhide transition, for the
reason above; this is the corrected bar.

Bounded residuals, each visible in the engine's counters:

| Event | Effect | Bound |
|---|---|---|
| A hidden frame's produce signal or promote fails | the previous hidden image keeps being drawn one more frame, else one frame without plates | one game frame |
| The output pass skips an output (allocator busy, list reset, a slot's reader could not be added) | that output misses the plates, and the next frame is not hidden | one game frame |
| The output state flips between the gate and the publish (HDR on, DLSS-G off, multiplier drop, preset pause) | the frame is drawn unshifted or not at all | one game frame |
| Release, device change or resize while a hidden frame is open | that frame loses its plates | that frame |
| The pacer presents an output of batch N after batch N+1 was published | the output shows the newest record: a loss (hidden to native) or a double (native to hidden) limited to the late or early outputs of that one transition | one transition, measured by the counters |
| A frame's draw is refused by the state rule inside an already hidden frame | that draw is drawn natively and the frame's image is drawn too: the draw appears twice | one draw |
| The plate draws move to another thread | the engine latches off once, logs once, hides nothing afterwards | permanent, fail open |

## Threads

The draws and the present seam are the game's render thread, which is assumed: if the seam ever runs on another thread
than the draws, the engine closes its gate for good, logs one warning and keeps the plates native. The output pass runs on
the presenting threads, one at a time. The menu only writes a settings snapshot and, when the key goes on, the feature bit
after it; it never clears the bit (the engine clears it once nothing drawable is left, and re-reads the snapshot, so a key
turned back on in that instant ends with the bit on).

## Log lines

`Plates:` (engine, written at most once per 600 presents): `first plate slot created (WxH, format N)`, `first plate frame
promoted (WxH)`, the counter line, and warnings for a failed signal, a failed slot track, two drawing threads and one for a
seam on another thread. `Plates:` (game side, once per 600 finished frames): the draw counters (drawn, matched, asks,
declined, shadowed, hidden, fallback).

## Code

- `OptiScaler/ext/ffxiv/plates/PlateClassify.h`: the draw rule, pure and header-only (the whole state matrix is a CPU
  test).
- `OptiScaler/ext/ffxiv/plates/PlateShaderKind.h`: the four nameplate pixel-shader CRCs and the stamp.
- `OptiScaler/ext/ffxiv/plates/PlateNative.{h,cpp}`: the game side: state read, the frame's latched answer, the one
  redirect routine, the frame-finish hook, the counters.
- `OptiScaler/ext/hudless/UiPlates.{h,cpp}`: the engine: slots, records, produce fence, the drain, the output pass.
- `OptiScaler/ext/hudless/PlateSlots.h`, `PlateRules.h`, `PlateSync.h`, `PlateMode.h`, `PlateEngine.h`, `PlateSource.h`:
  the slot rules, the pass rule, the one leaf mutex, the settings snapshot, the game side's half of the interface and the
  frame finish hook.
- `OptiScaler/ext/hudless/shaders/UiPlateDraw.hlsl`: the output pass's quad.

Seams: `ext/hudless/HudlessSeams.h` and `ext/hudless/PlateSeams.h` (`PostInteropCopy`, `OnReleaseInterop`,
`BeforeOverlay`, `NoteExpectedMultiplier`, each behind its own feature bit), and two lines in
`ext/ffxiv/lighting/FfxivLightingCapture.h` (the `DrawIndexed` intercept and the pixel-shader stamp).

## What is verified, and what is not

Verified on a software adapter (D3D11 and D3D12 WARP) and by CPU tests: the draw rule and its fail-closed property; that a
render-target-bindable shared texture can be drawn into on the game's device and sampled on the D3D12 side; the slot
states, pins and reader fences; the whole engine protocol (reserve before hide, one record per present, warm-up, skips,
cutscene, release, key toggles and the drain, multiplier 1 to 3, pause, resize, a refused reservation, a failed produce
signal, the thread latch, late and early outputs); the game side on a real D3D11 context (a broken condition keeps the
draw native and asks nothing, one answer per frame, hidden draws only in the image, shadow draws in both places, a refused
redirect stays native); and the two sides together end to end.

Not verified, and only a game session can: whether the game's plate draws really have that state (the diagnostics census
answers it); whether the driver accepts the render-target shared texture on the user's GPU; that one render thread runs
the draws and the present; the two freshness windows (250 ms for the record, 100 ms for the pass) against the real output
cadence at three or four outputs per frame; how far the pacer lags; whether the game draws plates under every HUD window
(z-order); and the draw-time cost of the extra redirect.

Two questions are open by design in this build:

- **The UI boundary detector does not see a hidden plate draw.** The draw intercept returns before the hook that feeds the
  boundary detector (the same place the character layer is fed), because a hidden draw is one the frame really does not
  get. Whether the UI-free mode needs those draws in its boundary (the plates are UI that now reaches the frame through
  the pass, after the mask was built) cannot be answered without the game. The first session with UI-free capture and
  `PlateInterp = endpoint` compares the boundary lines of the diagnostics with the key off and on; if the boundary moves
  with the key, the intercept has to feed the detector before it returns.
- **The per-output shifting code is compiled and tested but unreachable in this build.** The pass's mover path runs only
  when the frame finish reports movers, and the game side reports none (no tracker), so `PlateInterp = on` is `endpoint`.
  The engine test drives the mover path with a finish hook of its own, so the arithmetic and the upload are covered; the
  tracker itself is the next stage.
