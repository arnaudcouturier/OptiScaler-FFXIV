# Puddles: a pool's colour at the tone map, and the shape the engine's own G-buffer carries

Companion to the [puddles](../FEATURES.md#puddles-ffxiv-experimental) entry. This note is about the machinery: the two stages, which of the
engine's own inputs each one replaces, how the pool is shaped, how the mirror is marched, and what a frame the module cannot serve comes to.

**Status: experimental, and not verified in game.** The switch is off by default. What the code is built on was measured from the engine's own
shaders and from captured frames; the look is a session's judgement, and nothing here has seen one. The tests say the stage reproduces a CPU
reference of the same formula on a software adapter, that a dry frame and every texel outside a pool are pure copies, and that the mirror's hit
is the scene's own colour with water's Fresnel; they say nothing about how it reads in a session.

## Why there are two stages

The engine's own wet look has no shape and no mirror. Its material shaders read `g_WetnessParameter` (its first float the wetness amount, 1.0 in
rain and 0.0 dry) and write the five G-buffer targets; its deferred composite then reads them. On the stone plaza that composite's whole response
to wetness is two things:

- a **maskless multiply on the albedo** of at most about 45 %, saturating at a gloss of 0.35 - so a surface already at 0.83 gains nothing from a
  "wetter" write, and there is no pool-shaped darkening at all;
- a lookup into `g_SamplerReflectionArray`: seven 256-square environment cubes, sampled with a mip the gloss chooses. At the plaza's gloss that
  mip is 3.40 (a 24-texel face) with the engine wet and 5.15-5.91 (4-8 texels) dry; the pass's own write of 1.0 buys 2.16 (57 texels).

The composite is not bound to the scene colour, the depth or the previous frame, so **no G-buffer value can put the aetheryte or a lantern on
the plaza**: the best the gloss channel can do is blur a probe less. That is the "shine" a session reported. What the reference picture shows is a
screen-space mirror of the scene on a darkened surface, which is a pass of this fork's own at the tone-map draw. So:

- **the gloss stage** (once a frame, before the light passes) copies one of the engine's own G-buffer targets and replaces its gloss channel on
  the up-facing ground where a pool is, so the engine's own lighting still draws wet stone around the pool and, inside it, the engine's own probe
  reflection sharpened - which is what a mirrored ray that finds nothing falls back to;
- **the tone stage** (at the frame's tone-map draw) darkens the pool's lit colour and adds a marched reflection of the frame's own scene to it,
  with water's Fresnel. Both stages read one field with one wetness, so the pool the gloss pass shaped is the pool the mirror fills.

## The shape, and the wetness

Two octaves of a value noise on a lattice that wraps, anchored to the world position of the texel: the same world point always gets the same
value, so the pools stay where they are while the camera moves, and two frames of a still scene agree (the upscaler is not a denoiser, and a term
that differs between two frames arrives as noise). The field is thresholded at `1 - coverage * wetness`, with the edge softness as the
smoothstep's half-width, and both stages gate on `saturate(wetness) * factor`, so a dry frame is a pure copy even where the field reaches its top.

The wetness is the amount the engine's own draws read: the engine's weather amount, or the wetness knob's forced value while that knob is armed
(the knob patches the engine's buffer for the material draws, so its value is the one they see). Both stages read the same 32-byte buffer - the
gloss pass's own copy - so a frame cannot be shaped by one amount and shaded by another. Neither stage has a timer, a frame number or any
history.

`PuddlesScale` is metres per noise cell, and a pool is a few cells across. The defaults were measured with the shader's own noise ported to a
host program: over one full 64-cell period of the field (384 m of 6 m cells, sampled every 5 cm) at coverage 0.42, **32.9 % of the
up-facing ground is pool** (mean mask 0.33), and on a 60x60 m patch the blobs are 22 per 3600 m2 with a **median of 4.3 m across** (the
largest tenth 11.5 m) and the 0-to-1 edge is **0.81 m wide** (the field's mean gradient there is 0.074 per metre), with 9 % of the ground in
that edge. The same port at the previous defaults - coverage 0.5, scale 4 m, softness 0.1 - gives a 2.1 m median blob and a 1.7 m edge, which is
the mottled sheet rather than the reference's pools; that is why the three defaults moved. The previous default of 1 m read as speckle and a session
ran into the 5 m cap still seeing none, so the key reaches 16 m.

## What the tone stage does, texel by texel

At the frame's tone-map draw - the one draw that reads the scene the composite wrote and writes one FP16 target - the module runs one full-screen
triangle over the part of the scene that lies inside the render window, and binds its result at `t0` for that one draw, putting the game's own
binding back afterwards (also when the draw throws).

**One owner of that binding.** The light shafts add their air light to the same scene at the same draw, and only one module can rebind `t0`. So the
shafts' tone-map branch is the owner: after its own pass has run, and before it binds anything, it calls this stage on its air-lit output, and what
the tone map then reads is the pool's colour over the air light - the mirror reflects the shafts' light as well. The draw seam's own call to this
stage is the fallback for a frame the shafts left alone (their card off, or a frame they skipped), and it answers nothing for a tone map the chain
already answered. What the chained order costs: one draw and this stage's own texture. The scene copy the fallback needs is only the window's
one-texel ring (see below), so the frame's full copy is the shafts' own - the pool's stage adds none.

**Scene HDR.** The engine's scene HDR path replays its own tone chain in its own branch, so the tone map this stage draws before is not the picture
that path presents. The shafts' branch never sees that draw while HDR is on; the fallback asks the same key itself and leaves the frame alone, with
one log line and nothing counted against the module.

The inputs it needs are measured once per frame at the **deferred composite**, which is the one draw that binds `g_SamplerViewPosition`: its
view-space positions, its G-buffer normal (`g_SamplerGBuffer4`), the camera table and the render target it writes. The tone map declares only
`t0` and a colour LUT, so a frame whose composite was not seen has no pool - the game's own draw, nothing counted.

The rule, in the shader's own order:

- a texel outside the render window, without a surface (a view position that is the cleared sky), whose decoded normal is not a unit vector, whose
  normal does not face up (`n.y` above 0.5: a surface steeper than sixty degrees is a wall), or whose frame has no wetness keeps the scene's own
  texel - byte for byte;
- otherwise its world position is reconstructed from the view position and the camera's inverse view, the field is read there, and the pool mask
  is `saturate(wetness) * smoothstep(level - softness, level + softness, field)`;
- the lit colour becomes `scene * (1 - mask * darkening)`. This is the part that has to work before the mirror does: the reference's wet stone is
  dark even where no reflection lands, and a water film absorbs;
- the mirror's normal is the floor's (the world's up) tilted by a **static** gradient of the same noise field, on a cell of about a metre and
  scaled by the ripple key. It is static on purpose: an animated perturbed direction is exactly what a temporal upscaler turns into noise;
- the ray is marched with the water reflection's own march (`shaders/SceneMarch.hlsl`, one implementation for both passes): a geometric ramp of
  `PuddlesReflectionSteps` taps, four halvings of the crossing's interval, and a thickness test. The colour of the texel the ray crosses is added
  as `hit * mask * strength * Fresnel`, with water's `F0 = 0.02` and Schlick: 2 % straight on, up to 100 % at a grazing angle. That is what makes
  a level camera at eye height a mirror on the ground five to eleven metres ahead and the same camera's near ground almost none of one, which is why
  the look has to be judged from the reference's own pose rather than from a camera tilted down;
- a ray that leaves the picture or finds nothing adds nothing. The darkened scene already carries the engine's own probe reflection of that
  ground, so a miss is never black and never a hole.

The target this pass writes is its own texture of the scene's size, because a resource cannot be a shader resource and a render target in one draw.
It arrives holding the scene's own texels: the draw's viewport is exactly the window's whole texels, so only the one-texel ring around the window -
the texels the tone map's own filter reads at the window's edge - is copied, not the window's sixteen megabytes.

`PuddlesReflection = false` is the darkening alone, which is the A/B a session needs to judge the pool's shape before the mirror; the darkening at
0 with the reflection off is the stage's identity, so nothing is copied and nothing is drawn.

## Frames, upscalers and frame generation

The stage is before the upscaler's evaluate and before the tone map, so a super-resolution pass sees the pool as scene colour, a ray-reconstruction
pass sees the guides of the flat floor under it (which is the right guide for a mirror on a floor), and frame generation interpolates finished
frames: no extra buffer, no new work. The reflection is colour only.

Nothing is remembered between frames: each frame is a pure function of the frame's own buffers, which the tests pin by running one scene twice and
comparing every texel. That is the pass's own property, and it is not a claim about the pipeline. The mirror is **one sample of the scene per
texel**: it has no confidence fade at the edge of the picture, no blend against the engine's probe, and no jitter, so its hit/miss boundary follows
the frame's geometry exactly, and the march's steps are metres long beyond about eleven metres, which is where a reflection of a straight edge
becomes a stipple. Under DLSS's ray reconstruction or super resolution and under frame generation, expect that boundary to shimmer and to smear
along the floor's motion vectors - a mirror's image moves at the reflected objects' velocity, not the floor's. That is the honest expectation until
a session measures it; the cheap fixes (an edge fade and a per-pixel dither of the ray) are not in this version.

## A refused gloss target, and the pool's colour

The gloss stage's keys can name a pair this pass must not write: target 3 or 4 (a 16-bit target, and the world-space normal the up-facing test
reads), or the material pack's red channel (a selector, not a gloss). Those are today's picture for the G-buffer write, and the card says so. The
tone stage has its own identity and is armed by the same switch, so it still runs - which is why the frame's wetness amount is copied for **both**
stages: the wetness draw hands over the engine's 32-byte buffer whether or not the gloss stage has a target to shape, and the pool's colour at the
tone map then reads the weather instead of an all-zero buffer that would leave the pool empty while the card said it was armed.

## Why the gloss stage stays

It writes the wet stone the pool sits in, and inside the pool it sharpens the engine's own probe reflection, which is what a missed ray keeps. Its
target and channel keys keep their measured default (target 1, channel `a`); the material pack's red is a selector the composite and the wetness
pass compare against 1.0 and is refused.

## What it costs

- The gloss stage: one copy of the G-buffer target and one full-screen triangle a frame, then one slot swap per reader draw of that target.
- The tone stage: one small copy of the camera table, the window's one-texel ring out of the scene, and one full-screen triangle with a 24-tap
  march over the render window. **Expected, not measured on a real card:** about 690,000 pool texels (a third of a 1920x1080 window) times 24
  taps is 16.6 M scattered reads a frame, which is 0.4 to 1.3 ms at 1080p, plus the ring's copy, which is a few thousand texels. Nothing here has
  measured that: the WARP adapter's timestamps mean nothing at this size. The pass reports its own GPU time (a timestamp pair read a run or two
  later, never waited for) and the run line prints it, so a session reads the figure instead of trusting this paragraph.
- The owned textures: one of the scene's size (66 MB at 3840x2160 FP16), and - only while a draw-diagnostics capture is armed - a per-texel
  statistics target of the same size.
- Nothing is read back on the draw path. Two reads reach the CPU without waiting: the pass's GPU time from a timestamp pair a run later, and, while
  a capture is armed, the pool and march-hit shares from a no-wait map of the previous frame's copy.

## What a session can read

- One log line a session when the stage first runs: the window, the keys, and - once measured - the pass's GPU time and the shares.
- `engine.txt` in a capture carries both stages: the switch, what is armed, the runs, the reader draws, one line per refusal reason, and the pool's
  share of the frame with the share of it the march found a surface in. `-` means no capture has measured them yet.
- The card's state line names both stages' runs.

## Keys

| key | default | what it does |
|---|---|---|
| `Puddles` | `false` | the master switch of both stages |
| `PuddlesCoverage` | `0.42` | the share of the up-facing ground that becomes puddle at full rain (about a third, measured over the field's 64-cell period); `0` is today's picture and runs nothing |
| `PuddlesScale` | `6.0` | metres one noise cell spans, 0.05 to 16: a pool is a few cells across (4.3 m median, measured) |
| `PuddlesSoftness` | `0.03` | the half-width of a pool's edge, in noise units (0.81 m at the default scale, measured) |
| `PuddlesGlossTarget` | `1` | which of the engine's G-buffer targets carries the gloss channel: 1 or 2 |
| `PuddlesGlossChannel` | `a` | which channel of it is replaced; `r` on target 1 is refused |
| `PuddlesGlossWet` | `1.0` | the channel value of mirror-wet stone, and what a missed ray's probe fallback is sharpened to |
| `PuddlesReflection` | `true` | the marched mirror at the tone map; off is the darkening alone |
| `PuddlesReflectionStrength` | `0.6` | 0 to 1 of the Fresnel reflection the hit is added with |
| `PuddlesDarkening` | `0.45` | 0 to 1 of the lit colour a full pool takes away |
| `PuddlesRipple` | `0.35` | 0 to 1, how much the static ripple tilts the mirror |
| `PuddlesReflectionSteps` | `24` | the march's taps, 8 to 48: the one key that changes the tone stage's cost |

## What a session still has to settle

1. **The darkening first.** Same spot, same camera, same weather, `PuddlesReflection = off`: the ground inside the pools must be clearly darker
   than outside. If that alone does not read as wet, the mirror will not either.
2. **The mirror from the reference's own pose.** Level camera, eye height, the fountain six to ten metres ahead - the pose the reference picture
   was taken from, whose horizon is mid-frame and not near the top. Take a rainy shot with the reflection off and on. The aetheryte's smear must
   appear below the basin rim, and its glow is part of what the march hits (the glare chain has already run by then). Below about three metres the
   mirror is invisible by Fresnel - water reflects 2 % straight on - so the near ground, the bottom of the frame where a player looks at their feet,
   gets the darkening only. That is the design and not a defect: the pool reads as wet there because it is darker, and the mirror appears further out.
3. **The pool size.** `PuddlesScale` 6.0 at a coverage of 0.42 should give pools whose median is about four metres across with a sub-metre edge
   (the measured field: about a third of the ground, 4.3 m median, 0.81 m edge). If they read as speckle, the field's octaves are the problem rather than the
   scale.
4. **The engine's own wet, with the knob off.** One shot in real rain with `WetnessOverride = -1`: the pools must appear from the engine's own
   amount, and the ground around them must be the engine's wet ground.
5. **The dry identity.** Reflection on, dry weather, knob off: no change to a pixel, no copy, and the log line that says the stage was skipped for
   the wetness.
6. **Frame generation and DLSS on, panning across a pool.** The pool's shape must not crawl (its field and its ripple are world-anchored), but
   the mirror's hit/miss boundary is one sample per texel with no fade and no jitter: expect it to shimmer and to smear along the floor's motion
   vectors. Record it, and if it is too much, the next step is the water reflections' edge fade and a per-pixel dither of the ray, not a history
   buffer.
7. **The fountain's basin.** Standing water is a forward draw with the scene as its only target, so the G-buffer normal there is whatever lay
   behind it: the up-facing test can treat basin water as ground and darken it. If a session shows that, excluding the engine's water pixels is the
   next step.

## The splash rings follow the rain

The pools follow the floor's wetness, while the rings follow falling rain. The engine publishes only its wetness amount, which climbs while rain falls and sinks while the ground dries, so the rings read the number's direction: a rise means rain, a fall means it stopped, a flat stretch keeps the last answer and a dry floor is never rain. They fade in and out over 2.5 s. Only the engine's own amount counts, so `WetnessOverride` wets the floor and draws pools but never starts the rings. The log says `Puddles: it is raining` and `it is not raining` at each change. Not yet confirmed in game: whether snow moves the amount, and whether a lighter rain reaches a lower plateau (the rule is written to work there too).
