# Light shafts before the tone map

Companion to the `[FFXIV] LightShafts` key. This note is about the machinery and what a session has to check: which draws the pass hangs on,
how the sun and the scene are recognised without a shader CRC or an address, what it refuses, and what a captured frame measured.

**Status: experimental.** The switch is off by default and the visible effect has not been judged in game yet. The tests say the mask, the blur,
the colour and the composite do what this note describes on a synthetic frame; they say nothing about how the picture looks with the real sun.

## What it does

The engine renders the frame's lighting into an FP16 scene texture, and its tone map reads that texture once (`t0`) to write the post-tone
colour. Before that one draw the pass:

1. builds a half-resolution **sky mask** from the frame's depth: `1` where the depth is the far plane (the sky the light comes from), `0` on
   every surface. The engine's depth is reversed-Z, so the far plane is `0` and a near surface is close to `1`;
2. **blurs that mask radially toward the sun's screen position** - `Samples` taps, each `Decay` times fainter than the one before, the whole
   reach scaled by `Length` (the key's `Density`, shown as "Length" on the card);
3. **takes the sun's colour** into one texel: the sky's own colour in a small square around the sun, counting only texels whose depth is the far
   plane. With the sun low behind trees the sun's own pixel is a lit leaf, and a leaf's colour is not the sun's;
4. **composites**: a copy of the lit scene plus the shafts' light, over the render window only. The copy is what the tone map's `t0` holds for
   that one draw, and the game's own view is put back afterwards - also when the draw throws.

### The added light is a share of the white the pass reads

The render target's units are arbitrary: the same intensity key meant a sixth of a bright afternoon's white and four times a dusk frame's, which
is how a default came to burn the picture white. So the light the shafts add is measured against the frame's **white point** - the scene value
whose tone-mapped output is 1.0, where the picture turns display-white:

- the light at a pixel is `mask x intensity x gain x tint x fade`, in fractions of that white, with `gain` a fixed twelfth of the white per unit of
  intensity;
- the **addition** is bounded by how much room the pixel has left to the white: `head = white - scene` and `add = head x raw / (raw + head)`, so
  the sum is strictly below the white point whatever the key, the mask or the fade are, and a pixel already at that white gets nothing. The
  approach is smooth, so nothing reads as a clip. The default key adds about 10.7 % of the white to a surface with no light of its own, the top
  key about 19 %, and both are the same share in a day frame and a dusk one while the live white is the frame's own.

Where the white comes from, and how far it is trusted. A captured frame settles the measurement - its tone map's input and output pair (the same
pixels, **inside the 1920x1080 render window**, which is the only part the tone map writes): the target crosses 1.0 at a scene value of **0.838**
(red), 0.853 (green), 0.858 (blue) and 0.851 (luma), with a local gain of 1.18 to 1.22 over 0.2..0.8 and no clamp on the output. The gain *is* the
tone map's own exposure multiplier, which its parameter buffer (`cCommonTexParam`, the draw the shafts are composited into) carries as its second
float, and the white point is its reciprocal: 1/1.21 = 0.826, within 1.5 % of the pair's own crossing. The pass reads that float - copied at the
tone map draw and mapped without waiting from a ring of four copies - and keeps the last value it read.

The measured 0.838 is the only white point this build has verified, so it is the **ceiling**: a live exposure that claims a white above it is a
buffer this build has not measured. Below it the live exposure is what makes the same key the same share of the white in a day frame and a dusk
one; the floor is the exposure range's dark end, **0.15**.

**What the bound does not do is measure this frame's white.** The band can tell a plausible exposure from an implausible one; it cannot tell a
wrong-high live value from a legitimately bright frame. The bound is the white the pass reads, and when that value is too high for a much darker
frame the shafts over-light it - work the numbers: a live 0.838 on a frame whose real white is 0.2 takes the top key's 0.1 surface to **0.258**
(29 % past that frame's white) and its 0.02 surface to 0.182 (81 % of it). Had the live value been the frame's own 0.2, the same key would have
added 0.032 and 0.014. The 0.2-white dusk frame is a **computed example**, not a dump: the session's pair dumps measure 0.838 (17:35:25), 0.736
and above 1 (the earlier session); no dump has a 0.2 white.

The **fallback** is the band's dark end, not the captured frame's 0.838: an exposure the pass has not read (a session's first frames, or a build
whose tone map's parameter buffer is not where this build's is) might belong to any frame in the range, and the measured 0.838 on the worked dusk
frame adds 0.162 to its 0.02 surface - 81 % of its 0.2 white - where the dark end adds 0.028, 14 % of it. The dark end is not universally
conservative either: 0.15 is above the real white of a frame darker than 0.15 (an exposure above 6.67), so on such a night frame even the
fallback adds a little past the frame's white (0.021 to a 0.1 surface in a 0.12-white one, which reads 0.121). A white point is this frame's
only while a copy has landed and it is no more than ninety frames old; after that the bound is the dark end again and one line says so. The same
line is written before the first live white arrives, and the line that prints the live white names the exposure it is the reciprocal of. A session
whose log has neither line has neither read: the effect is then dim, which is the fail-open direction.

The tint is a **colour**: the light's own diffuse is divided by its brightest component, so `{1.96, 1.79, 1.58}` becomes `{1.0, 0.92, 0.81}`. The
light's own brightness is not the effect's - one scene reports 1.96 and another 0.06 - so the intensity key decides how much light there is and
the light decides only what colour it is.

The sky itself keeps its own value: the shafts land on the surfaces the light crosses, never on the sky they come from, which would only smear
the sky's own brightness. Nothing of the game is written: the engine's scene and its depth are read, and the copy is this pass's own texture.

What the default looks like, with the captured frame's own tone-map pair. The display values are the tone-mapped **luma** of the pair (binned
medians inside the render window) and the addition carries the captured light's tint, `{1.0, 0.9156, 0.8065}` (luma 0.9256); the red channel of
the same shaft steps +29/255 at the dark end, and the tinted luma steps +27:

| scene (luma) | before | default (1.0) | top key (2.0) |
|---|---|---|---|
| a dark tree, 0.05 | 0.066 | 0.172 (+27/255) | 0.261 (+50/255) |
| a mid surface, 0.4 | 0.513 | 0.603 (+23/255) | 0.670 (+40/255) |
| a sunlit surface, 0.8 | 0.954 | 0.978 (+6/255) | 0.976 (+6/255) |
| at the white, 0.838 | 0.987 | 0.987 (nothing added) | 0.987 (nothing added) |

The knee is what makes the bound safe and also what makes the effect faint on a surface that is already near white: there is only its room to the
white to add to, so the dark side gets tens of levels where the bright side gets **six**, which is at the edge of what reads as a change in
motion - a shaft on a bright wall may not read as one. The sky itself is **exactly unchanged** (the composite skips far-plane texels), and a
pixel at the white gets nothing. The clear part of the effect is the dark and mid geometry.

## The identity: the light has to be the frame's sun

The sun's own draw is found by its shape and its light parameters, and neither can tell the sun from another directional light: a frame draws one
directional light in every capture so far, but a session drew shafts from a light that was a **48.9 degree yaw** of the sun at the same elevation
(32.0 against 32.2 degrees) - a light the elevation rule cannot refuse, and the position it produced was 1,274 pixels from the sun's own.

The frame carries the identity itself. The light draw binds the engine's `g_DirectionalShadowParameter` (128 bytes) beside the light, and its first
member is the directional shadow's 4x4 light-space matrix, whose **third row (the bytes at +0x20)** is the light's own world axis negated and
scaled by the cascade. Negated and normalised it is the frame's sun: the row the capture dumped, `(-0.005546, -0.036772, 0.058028)`, normalises to
`(0.080473, 0.533533, -0.841942)`, which is the direction the same capture's light draw reported (0.02 degrees apart), and the yawed light is
48.9 degrees from it. So the pass copies every 128-byte constant buffer the light draw binds, with the light and the camera, and requires the
light's direction to agree with the shadow's axis within **3 degrees**:

- a candidate's bytes are only read as the shadow's if they are a light-space matrix: the fourth row is `(0,0,0,1)` and the first three rows are
  mutually orthogonal (`ShadowMatrixShape`). A captured sun draw binds `g_FakeSpecularParam` at the same 128 bytes, which is why the shape is
  checked; **what that buffer's real bytes are is unmeasured** - no capture dumped them. The shape is a guard, and it has not been proved that the
  game's own neighbour fails it. If the neighbour is matrix-shaped, the shape does not keep it out: at a sun draw the shadow's row agrees, so the
  light is accepted; but a frame whose light draw binds only the shaped neighbour (no shadow buffer) would be judged by the neighbour and refused
  rather than drawn from the direction alone. The developer draw diagnostic now dumps every 128-byte pixel-shader buffer of the sun draw itself
  (with its reflected name), so the next capture answers this; until then the shape is a guard whose real-buffer behaviour is unmeasured;
- a light whose direction disagrees is refused with **its own line and its own count** (the card's "the light not the frame's sun"), on the same
  path that refuses a lamp and a mirror light. The refused read is dropped and the **same draw is looked at again on the next frame's read** - it
  is not held off. Only a lamp's buffer is remembered (for the few seconds the engine's buffer pool keeps that pointer, so the frames after it look
  at the next light draw instead);
- a build whose matrix stores the light's axis with the other sign is accepted only after every candidate has failed this build's sign, so a
  different layout costs a line, not the session. A light that is another direction fails both signs;
- a frame whose light draw binds no shaped 128-byte buffer is not refused: it **draws from the light's direction alone** and one line
  (`kShadowAnchorMissing`) says so. It is a frame with shafts in it, not one without.

The captured row above is the **rain volume's** `g_DirectionalShadowParameter` (the frame's first light draw, at its slot 3), not the sun draw's
own: the dump list samples the first draw of a capture that binds the name, and the rain volumes come first in the frame. That row is still the
frame's directional shadow, so it proves the identity the rule uses; what is still pending is a capture of the **sun draw's own slot-5 buffer**,
which the diagnostic's new 128-byte dump of that draw will write. Until then the pass copies every 128-byte buffer the sun-shaped draw binds and
lets the content decide.

The 48.9-degree frame is what the rule was built for: at 17:53:08 the log's own camera line puts the frame's sun at (249, 41) of the window while
the module drew from (1523, 89). With the rule in place that light is refused, and the run line prints the angle the accepted light agreed at and
the shadow's own row is in the capture's `cbdump.csv` to check it against. The refusal line prints the light's direction and the angle; it does
**not** name which 128-byte candidate was judged or its slot, so the capture's `cbdump.csv` is still what says which buffer the angle came from.

## The four draws, and how they are found

| draw | how it is recognised | what the pass takes from it |
|---|---|---|
| the sun's light draw | its pixel shader declares `g_LightParam`, and the draw's own shape fits: **two** render targets of `R11G11B10_FLOAT` at the scene allocation's size (the engine's light-buffer pair), written **without blending** - the local lights accumulate into the same pair - beside a depth view this pass reads and a constant buffer large enough to hold `g_CameraParameter` | the light's position, direction and diffuse colour (copied at the draw and read at a later frame's tone map); this frame's depth; the camera's view, view-projection and projection; every 128-byte parameter buffer, for the shadow's axis |
| the deferred composite | its shader declares `g_AmbientParamArray` and `g_SamplerLightDiffuse` (the engine pass overrides module's own anchors) | its single render target: the frame's **lit scene**, which is what makes the scene recognisable without a hash |
| the tone map | its shader declares `cCommonTexParam`, and the draw's own shape fits: one FP16 render target at the scene's size, a `t0` that is the very resource the composite wrote | the scene as `t0`, the render window, the draw whose `t0` is swapped for one call, and the tone map's own exposure multiplier (copied for a later frame) |

The evidence is one captured frame (`draws.csv`, format 3): the composite's single target (an `R16G16B16A16_FLOAT` 3840x2160 texture) is the
tone map's `t0` in the same frame. The sun's light draw binds `g_LightParam` at slot 2, `g_CameraParameter` at slot 1 and the directional shadow
at slot 5; the rain volumes before it bind `g_LightParam` too, at slot 4, with the same shadow at slot 3 - which is why the shape rule, not the
name alone, decides. The draw diagnostic's `cb.csv` sample is taken **at that draw**, so a capture prints the directional light's own direction,
colour and view matrix beside the frame's shadow matrix. The tone map's `t0` and `t1` carry no reflected names of their own in that capture (the
shader was created before the diagnostic could read its bytecode), which is why the shape rule, not a name on the tone map's textures, is what
identifies the scene.

### Why the sun's draw needs both rules

Every light draw of the frame declares `g_LightParam`, binds the depth and binds `g_CameraParameter` - including the rain-exposure volumes of
the wetness mask, which come first in the frame and whose light reports "straight up". The local lights write the same light-buffer pair as the
sun. So the pass needs three independent facts:

- the **draw's shape**: the light-buffer pair, not blended into. The wetness volumes write one `R8` target and blend; the local lights blend;
- the **light's own parameters**, read back from the buffer: a directional light has **no world position**, a unit direction, and that direction
  **above the horizon** - a tolerance about one degree *below* the line, because a sun one to three degrees up at dawn or dusk is a sun the player
  can see and exactly when the shafts are longest. A lamp reports its position and can report a unit direction too, so the parameter test alone
  cannot tell a lamp from a zenith sun, and the shape test alone cannot tell a local light's draw from the sun's. The horizon half is the sign: the
  buffer holds the direction the light shines *from*, so the same vector negated - the sun's own mirror, about 0.5 to 0.8 below the line - is a
  different directional light's draw, and it was the value that read "behind the camera" or "three quarters of a window outside" while the disc
  was on screen;
- the **frame's directional shadow**, as above: the light's direction has to be the axis the frame's own shadow is built from.

Only the **first** draw of a frame that passes all of them is taken: the frame makes two light passes (the second into another pair) and the
composite reads the first pass's buffers. A light the read-back refuses - a lamp, a direction that is not a unit vector, a directional light below
the horizon, or one the frame's shadow disagrees with - does not end the search for a light to read: a lamp's buffer is held off for a few seconds
and the frame's **next** sun-shaped light draw is taken, while a direction the shadow disagrees with is simply looked at again on the next frame's
read. Whether a candidate's screen position lands in the window is not known before its copy is read (the bytes arrive a frame or two later), so a
sun that is merely behind the camera or outside the frame is *not* refused: it keeps the light it measured, which is what keeps a camera turned
away from the sun cheap.

## What the live build reports, and what the colour key is for

A captured frame's sun draw reports a real `m_DiffuseColor` - `(1.96, 1.79, 1.58)` - and another light of the same session reports
`(0.048, 0.112, 0.16)`, which is exactly why the tint is a colour and not a magnitude (see above). With `LightShaftsColor = auto` the shafts take
that colour; where a build reports none, they take the **sky's own colour around the sun's screen position**, and a frame with no sky in that
square - night, or the sun behind a wall - adds nothing at all: a frame without shafts in it, not a frame the pass failed on, so it counts no
refusal. A sky below a small luminance floor behaves the same way.

`LightShaftsColor = white` replaces all of that with a neutral tint, which is the way to tell a colour problem from a geometry problem: a white
shaft in the wrong place is the projection, a coloured one is the colour. It has no brightness floor of its own, so a white run at night glows.

The direction is a **world** vector, and the pass projects it with the camera's **view-projection** (`g_CameraParameter`'s
`m_ViewProjectionMatrix` at `+0x060`): `clip = viewProjection x (direction, 0)`. The measurement is camera-free - the buffer's direction
`(0.4204, 0.8181, 0.3925)` is, to four decimals, minus the third row of the same frame's captured `g_DirectionalShadowParameter`
`m_ShadowProjectionMatrix` - and it moves with Eorzea's own sun clock (`(0.445, 0.511, 0.736)` twenty-three seconds later). Projecting that vector
with the projection alone answers a screen position that belongs to a camera with no rotation at all: for the afternoon sun it is *behind the
camera* on every frame, which is exactly what a session logged while the disc was on screen.

The pass copies the camera's whole first `0x160` bytes in one box - view (3x4 at `+0x000`), view-projection (4x4 at `+0x060`) and projection
(4x4 at `+0x120`) - and reads two matrices out of them. The **projection** stays the shape guard on the buffer (`ProjectionOk`: finite, positive x
and y scales, a `(0, 0, -1, 0)` w row within a thousandth), so a buffer that is not this camera's is refused before anything is projected with it;
the **view-projection** is checked for being one too (finite, a last row of about unit length, which is a rotation's row seen through the
projection), and a buffer whose `+0x060` does not hold it is refused as "the camera buffer did not hold the camera" rather than lighting the frame
from a wrong screen position. One log line a session names the projection's own rows, and the next line prints the view-projection's four rows at
four decimals, so a session tells a wrong direction from a wrong matrix without a capture; the developer draw diagnostic's `cb.csv` carries the
same block one row a second beside the light's direction.

## The frame's readings

The light's bytes, the camera and the shadow's candidates are read back **without ever waiting for the GPU**: small staging copies issued at the
sun draw, and a map of the newest one the ring still holds (`D3D11_MAP_FLAG_DO_NOT_WAIT`) at a **later frame's** tone map, from a ring of four
staging sets. The ring's copies are made on the device that issued the copy: a context from another device (a device change in the session) gets a
fresh one instead of having its copy refused into the old device's buffer, which the read would then hand back as a stale sun. The age is what
makes the read belong to the draw it came from: a copy made earlier in the same frame has not run when that frame's tone
map asks for it - at 4K the GPU is a frame or two behind - so a same-frame read is refused by the driver on every frame and the pass would never
see a sun. One fixed age is not enough either: a copy would get exactly one map attempt, and a session where that attempt never landed would read
nothing for its whole length. So each read tries the newest copy first (the frame before this one) and takes an older one - up to three frames
back, while the ring still holds it - when the newest has not landed, never one older than the last copy it read. A light the rules refuse is not
an accepted read, so a refusal clears that floor and the frame after it may take whichever copy the ring still holds. A read that is still not
ready keeps the last good values and counts as a not-ready read, apart from a read that landed; the depth is always this frame's. The tone map's
parameters are read through the same ring and the same age rule, so a GPU two or three frames behind still answers a white point (the exposure
moves over seconds, not frames).

### A missed read costs a little light, not a frame

A frame whose own light bytes have not landed yet draws with the last sun a read accepted, for at most several rendered frames, fading a fifth
at a time: 1, 1, 0.85, 0.65, 0.45, 0.25 and 0.05 of it, then the frame is skipped. Every step loses at most a fifth of the full light, which is
what "a fifth at a time" means here; the largest *relative* step is 0.45 to 0.25, 44 % of what was left, the same relative drop the first version
had when it fell from 1 to 0.5625 in one frame at the render boundary (which frame generation interpolates into a visible slide). What changed is
that the absolute addition no longer drops by half of full light in one frame, and that the fall is spread over six frames instead of landing in
one.

The held sun's **direction is a world vector**, so it is projected with the newest camera a read landed, not the camera it was measured with:
while the view moves the shafts stay on the sun instead of sliding with an old view. The facts and the camera come from the same ring copy, so a
missed light read also missed the camera - the limit of that rule is a cut inside a stretch of missed reads, which is seen when the next read
lands (at most the hold's frames), not before. A cut a landed read *does* see - a turn of twenty degrees or more between two frames' view-projections,
which is a look-behind, a menu change or a teleport - drops the whole hold at once: a sun from before a cut would light the new picture from the
old one's sky.

Both counts run on the module's own count of **rendered frames**, not on the output-present token. One rendered frame is the game's light pass,
the composite that writes the lit scene and the tone map that reads it; frame generation presents that frame's outputs once or twice (the counter
steps about 1.8 times inside one rendered frame), so a token comparison turns a frame's own light draw and tone map into two different frames
whenever a present lands between them - half a session's frames were dropped that way while the captures show exactly one light pass in every
frame. The count therefore moves when the frame **closes**: at its first tone-map-shaped draw, and nothing else moves it inside a frame. The light
draw stores the count it saw and the tone map compares it (`SameRenderedFrame`), as do the "one tone-map draw a frame" and "this frame's scene"
tests. A frame that drew no tone map leaves the count where it is, so the frame after it finds this frame's light draw still stored and composites
with it and with its depth - a stale-frame path the count does not remove, and a frame the module has nothing to say about. A camera map that has
not landed is not a refusal: the frame is skipped, the camera of the last good read stays, and one line says the bytes have not arrived yet.

A read-back that has not landed for ninety frames is not a GPU that is behind any more: the frames are then skipped (one line, no refusal)
instead of compositing a sun that no longer belongs to the frame. A session whose maps have been answered "still drawing" sixty-four times and
that has never read anything gets its own line, so a stall is not reported as a frame without a sun.

## What it refuses

Every one of these leaves the game's draw exactly as it was, with the game's own scene still bound, and writes one log line the first time its
reason appears. Sixty-four of these refusals in a row switch it off for the session with one line.

| reason | what it means |
|---|---|
| the light draw's camera buffer did not hold the camera's projection | the buffer the pass took for `g_CameraParameter` does not hold this build's projection and view-projection (a build mismatch) |
| `the draw is not the tone map` | `t0` is not the scene the composite wrote, or the draw writes the scene itself (passed on, **not counted**: another draw that declares the same parameter buffer, or the engine's own post-tone copy, is another draw of the frame) |
| `the tone map's render target is not one post-tone texture` | more than one target |
| `the tone map's render target is not FP16` | another post pass's 8-bit surface |
| `the scene is not FP16` | another pass's binding under the same shape |
| `the scene, the depth and the render window do not belong to one frame` | a stale binding, or a resolution change mid-frame |
| `the render window is empty or does not fit the scene` | no viewport, or one that runs past the texture |
| `character-preview replay` | the preview draws the same shader families as the world |
| `the pass could not build its textures, shaders or draws` | an allocation, a shader or a binding the device refused |
| `a call threw` | a D3D call in the pass's path threw; the game's draw ran unchanged |

These are **not** refusals: a frame whose light draws do not have the sun's shape (so its depth is not this frame's), a light whose bytes have
not arrived or have not landed yet, a camera whose bytes have not landed yet, a light at a world position, a directional light whose direction
points below the horizon, a light the frame's own shadow disagrees with, a sun **behind the camera**, a sun further outside the render window than
a quarter of it, and a read-back that went stale. Each is a frame with no shafts in it, and the streak is untouched. One line per change of reason
still says which one it was, and the card counts them apart: each frame it leaves alone for one of these reasons is counted once, as that reason,
so the five numbers are the whole of that count (a frame skipped while the camera copy is still pending, or on a stale read-back, is not in it).
A frame whose light draw bound no shadow buffer at all is **not** in this list: it draws from the direction alone, with one line
(`kShadowAnchorMissing`), because the direction is a source and the shadow is the check, not a second one.

A lamp is the one non-refusal whose buffer is remembered: the read-back saw a world position, so the frames after it look at the frame's next
light draw until the engine's buffer pool reuses that pointer a few seconds later. A light the shadow disagrees with is not remembered - the same
draw is looked at again on the next frame's read - so a wrong light costs the frames that read it, not the session.

## The card

The card's one line is built by a rule (`FormatCard`), so its format is checked on the host. It prints the module's rendered frame count and the
frame count that drew (of which the ones drawn with a held sun), then the five reasons a frame had no shafts in it separately, because one number
for three of them is what made a session read "3259 without the sun in view" and left its own question unanswerable. The line below is an
illustration of the shape `FormatCard` prints, not a session's numbers: only a session's own counts line gives its share, and the span model the
replay tool prints is a wall-clock upper bound on frames, not a counts line (its 291 is not a card number):

```
on at 1.00: 3600 rendered frames, 3418 with the shafts (12 of them drawn with a held sun), 9 left alone, 173 with no sun to draw (no sun among
the frame's light draws: 41, the light's bytes not arrived: 96, the sun out of view: 22, the light below the horizon: 3, the light not the frame's
sun: 11); the read-back: 1200 reads, 3 not ready, 4 refused as not the sun; the last sun at 1720,640 of a 1920x1080 window
```

The first of the five covers two things a frame's light draws can come to: none of them had the sun draw's shape, or the light that did was a
lamp - both are "no sun among the frame's light draws", which is why the reason is named that way rather than by one of them. The fifth is the
frame whose light the directional shadow refused. The read-back's own `refused` count is the shadow refusals; a refused read never draws, so a
frame drawn from a held sun is not counted there (the held count is printed beside it).

The last sun and the window are written on **every** frame the sun was projected for, including the frames it was skipped on, so "the last sun at 0,0 of
a 0x0 window" only appears before the module has measured anything at all. The five numbers are the whole of the frames with no sun to draw: a
skipped frame is counted once and under one reason, so a session can add them up and get the count they sit under.

The same line goes into the **log** about once a minute of rendered frames - from the frame's close, so a session that draws nothing writes it too,
which the version that logged it only when a frame drew never did.

A session's decisions are replayed from the log and its captures by `tools/shafts-coverage.py`:

- the module's own share comes from the counts line; the decision spans are the log's own, with the frames the menu covered cut out of them (a span
  is wall-clock x frame rate, so it is an upper bound, not a count);
- every captured pose's own sun is the capture's **directional-shadow row** (`cbdump.csv`, negated and normalised), not the light the module read,
  and a pose is only judged when the capture has a shadow row of its own frame (within half a second);
- the light the capture sampled is compared with that row: more than three degrees off it is not the frame's sun, and a frame the module left
  alone for such a light is the **correct** non-draw, never a miss;
- a pose whose own sun is inside the margin and whose light IS that sun must have drawn, and its run line (when it is that pose's own) must put the
  sun within two pixels of the row's projection. A miss, or a draw at the wrong position, exits 1;
- a pose inside a menu-open span is not a played frame, so a frame left alone there is not a miss - but a draw there from a light that is not the
  frame's sun still fails. A span the log ends inside (the user quits from the overlay) runs to the log's end, and a close whose opening line the
  log does not hold (a log whose first lines were cut) runs from the log's first line, so neither turns the poses after it into false misses;
- the capture's clock is **checked**, not assumed: the log's own `written to <the capture's directory>` line against the capture's last row, or its
  `capturing ... now` line - whose own second is the one the directory is named for - against the capture's first captured frame, or one of the
  capture's view-projections against the log's camera line. A capture whose clock cannot be checked fails rather than placing every pose wrongly;
  two `capturing` lines in that same second are refused, because two captures started together and neither line names its directory.

An earlier version of the tool measured the poses against the light the module read and counted only the frames the module itself said it drew, which
made "291 of 291" true by construction; the current version's session numbers are the paired poses it can actually judge.

## The keys

| key | default | what it does |
|---|---|---|
| `LightShafts` | `false` | the master switch; off is today's picture and costs one test per draw |
| `LightShaftsIntensity` | `1.0` | how much light the shafts add (0..2), as a share of the white the pass reads (the default about 10.7 % of it); 0 turns the pass off |
| `LightShaftsDecay` | `0.96` | how fast a sample toward the sun fades (0.80..1.0) |
| `LightShaftsSamples` | `48` | the blur's tap count (16..96) |
| `LightShaftsDensity` | `0.9` | how far the taps reach toward the sun (0.1..1.5), shown as "Length" |
| `LightShaftsColor` | `auto` | `auto` is the sun's own colour (the sky's around the sun when the engine reports none), `white` is a neutral shaft |

## The engine's scene HDR path

While the engine's scene HDR is on, that path replays the engine's own tone chain in its own branch and the seam hands the draw path the plain
call: this module's draws never arrive, so the pass cannot draw. The key is read where the module decides and on its card, not remembered from
the config load, so switching HDR on or off with the shafts key untouched stops and starts the pass again: the log gets one line each time
HDR comes on, and the card says why instead of looking armed. A real HDR path (reading the replayed chain's scene) is a later package.

## What is not known

- Whether the shafts look right in game. The numbers above are the captured frame's own pair; the first check is a low sun behind trees, at the
  same camera, the key off and on; a `white` colour separates the projection from the colour in the same session. **The next session must face the
  sun**: the user's own screenshot in the last one was a pose with the sun behind the camera (the log says so at that same second, and the capture's
  own matrix puts its sun at clip w = -0.108), so that session could not show the effect whatever the rule did. Judge 1.0 and 2.0, not only 0.5.
- Whether the frame's `g_DirectionalShadowParameter` is bound at every sun draw. It was on the captured build (slot 5, 128 bytes), and with it
  the yawed light is refused; a build that does not bind it draws from the light's direction alone and says so once. The card's "the light not the
  frame's sun" number is zero in such a session. The shape guard against the same-sized neighbour `g_FakeSpecularParam` is **unmeasured**: no
  capture dumped that buffer's bytes, and if they are a light-space matrix then a frame that binds only the neighbour is refused instead of drawn
  from the direction alone. The diagnostic now dumps every 128-byte pixel-shader buffer of the sun draw itself, so the next capture closes this.
- Whether the live exposure is the white point. It agrees with the pair's gain on the captured frame, and the run line prints the white each armed
  period uses. **But the band cannot tell a wrong-high live value from a legitimately bright frame**: on a frame much darker than the measured one
  a wrong-high 0.838 still over-lights it (0.258 for a 0.1 surface in a **computed** 0.2-white frame at the top key, the worked example above, not
  a dump), and the 0.15 fallback is above the real white of a frame darker than 0.15. A session with a bright sky and a dark interior, logging the
  white point's own line, settles whether the live value tracks the frame at all; it cannot settle the band's ceiling, which needs a second
  measured white.
- Whether the hold's frames are enough on a GPU-bound session. It costs a little light rather than a frame, the ring's older copies are the bigger
  half of the missed frames, and the counts line prints the frames drawn from the hold: a session that sees that number large has a read-back
  problem, not a look problem.
- Which 128-byte buffer the identity judged, when a light is refused. The refusal line prints the light's direction and the angle from the row, not
  the row's own values or the buffer's slot; the capture's `cbdump.csv` is still what says which buffer that angle belongs to.
- Whether the shafts' cost is worth its look: the pass is four draws (two at half the render window, one texel, one full-screen composite) and
  one scene copy of the whole allocation at 4K. The GPU time is measured with a timestamp pair around the four draws and written to the log once
  it has landed.
