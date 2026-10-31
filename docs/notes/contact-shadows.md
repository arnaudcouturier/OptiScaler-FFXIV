# Contact shadows: what was measured, how to measure it again, and what is not known

Companion to the [contact shadows](../FEATURES.md#contact-shadows-ffxiv) entry. The facts this feature stands on were taken from the
benchmark with the fork's own draw diagnostic; the notes below say which, how to repeat the measurement after a game patch, and what the
benchmark showed when the pass ran.

**Status: experimental.** The key is off by default and the effect is not verified: it has not been tried in game, and on the one benchmark
frame that was dumped the term mostly did not land on contact edges (3.0 % of its texels were on one). The synthetic proofs in the tests are
synthetic; they say the march follows the geometry, not that the picture looks right. Read every number below with that in mind.

## The two facts the pass needs, and how they were read

The sun light draw and its mask were measured on 2026-10-30 (`builds/bench-runs/20261030-cap-diag-*`): the engine draws a screen-space sun
visibility mask (`0xD8CDA48A`, ten draws a frame, `B8G8R8A8_UNORM` at the display allocation's size, accumulated with `MIN`) and the sun
light draw `0x63035CD0` samples it as `g_SamplerShadow` at slot 6, together with `g_SamplerDepth` (it binds no view-position buffer; the
pass reconstructs the position from the depth). The mask is
rebuilt inside the frame (writes at ordinals 10..20, light at 22, more writes at 33..41, light at 44), which is why the pass runs per light
draw and not once per frame. The engine's own AO is a half-resolution `B8G8R8A8_UNORM` buffer, the precedent for a half-resolution term.

The third fact - the space of the sun direction - is what the pass needs before it can be built, and it is read from `cb.csv`, which since
this package carries the view matrix of the same draw beside the direction:

- a direction in **world space** stays the same while the camera turns, and `view * direction` changes;
- a direction in **view space** turns with the camera.

`g_LightParam` is 512 bytes in the shipped build (an earlier layout, measured before this build, was derived from 400), so only
`+0x00` (position) and `+0x10` (direction) are corroborated, and the direction's space and sign were both unknown. The vector the capture read is `(0, 0.8, 0.6)`, exactly unit length.

### The protocol, to repeat after a patch

1. Build a package from the branch and run the benchmark once with the diagnostic armed and a start delay that lands on gameplay:
   `FFXIV.DrawDiagnostics=capture`, `FFXIV.DrawDiagnosticsFrames=3`, `FFXIV.DrawDiagnosticsDelaySeconds=150`, `-Nvidia dlss`, no frame
   generation (`tools-local\Bench\README-diag.md` has the command).
2. Read `cb.csv`. The rows taken while the diagnostic waited have `frame` 0 and are the series that matters: one sample a second over the
   whole wait, so the camera has moved between them.
3. Check the view matrix is a view matrix before trusting anything else: rows 0..2 are three mutually perpendicular unit vectors and the
   fourth register at +0x30 is the first row of the inverse view (the view matrix is 3x4 - there is no `(0, 0, 0, 1)` row; a check written
   for four rows fails on the real data, which is how the wrong matrix offsets of the first benchmark build were found). The pairs to check
   are `m_ViewProjectionMatrix * m_InverseViewProjectionMatrix = I` and `m_ProjectionMatrix * m_InverseProjectionMatrix = I`, both within
   1e-3: the pass itself now checks exactly that on the first light draw of a session and refuses with one line if it fails.
4. If the direction is not a unit vector on every sample, or it jumps by more than five degrees inside a second, the member has moved in a
   patch: the pass refuses such a draw (`Contact shadows: the draw was left alone (the sun direction is not a unit vector)`) and this
   measurement has to be redone before the key is worth switching on.

The sign cannot be read off one vector, and no benchmark comparison has shown it either way (the screenshot pair below has no localisation
to judge). What the shipped build's `(0, 0.8, 0.6)` says is only that it points **up**, which reads as "toward the sun" - a direction of
travel would point down - so `[FFXIV] ContactShadowsSunSign` defaults to `toward` (the ray is traced along the vector). The one-frame dump
of 2026-10-30 18:32 did not settle it: with the sign as shipped the term landed on object silhouettes, and the frame's geometry made the
sun-side and anti-sun-side statistics of the engine's own shadows inconclusive (91 against 69). The check agrees with the engine's shadow
running opposite the sun's screen direction (166 degrees from it), but with a 4.5 px peak on one frame it is weak evidence: read the sign as
consistent with the engine, not as settled.

## The benchmark check

Same package, same benchmark, two runs one key apart (`ContactShadows` off, then on), the same six screenshots at the same scripted moments,
and the diff measured with the approach of `tools-local\Bench\Diff-MaskZones.py`: the per-pixel absolute difference of the 8-bit luma, in
regions chosen from the picture rather than from the pass, and the share of pixels that moved.

## Why the index did not recognise the draw (found)

The draw diagnostic's own capture of the sun light draw answers it (`builds/bench-runs/20261030-contact-space`, the row of
`ps_crc 0x63035CD0`): the resources are reflected as `0:g_SamplerDepth.T`, `6:g_SamplerShadow.T`, `1:g_CameraParameter`,
`3:g_LightParam`. The textures carry a `.T` and each of them has a `.S` twin, because the game's shaders are effect-style: one effect
variable declares a texture and its sampler together, and the compiler writes the pair into the reflection as `<name>.T` (the texture) and
`<name>.S` (the sampler state). A constant buffer keeps its plain name. `LayoutOfShader` compared the plain names only, so it found
`g_LightParam` and `g_CameraParameter`, never the mask or the depth, and answered "not a sun light draw" for every draw of the frame - the
answer that costs no log line, by design. Every other candidate was ruled out before: the key arms the module, the seam runs, the bytecode
is readable, and the reflection of that shader carries exactly the four names at the slots the pass expects.

The rule now compares the variable's name (`diag::ResourceBaseName` strips one trailing `.T`/`.S`; the binding's type already tells a
texture from a sampler), and `tests/ext/ffxiv/relight/contact_reflection` carries the engine's name table as a hand-built RDEF, so a future
patch that changes the naming fails a test instead of going silent. The same helper is what the draw diagnostic's shader fixtures use: with
the plain comparison they would have kept none of the game's shaders at all, because every texture name carries the suffix.

**The one-frame dump (2026-10-30 18:32, run `20261030-contact-dump-r4c`).** `[FFXIV] ContactShadowsDump = 240` writes one frame at the first
sun light draw after 240 s into `OptiScaler\contact-dump\<time>\`: the engine's mask before the pass, the half-resolution term, the mask
copy the draw sampled, the depth (the shipped build's buffer is R24G8_TYPELESS), the camera bytes and the sun vector, plus `facts.txt`.
`tools-local\Bench\ContactDumpCheck.py` reads that folder and answers from the one frame: **60,303 pixels darkened of 3,686,400 (1.64 %)**,
**0 pixels brighter**, **0 changed outside the render window**, **0 changed where the term is 1.0** - the mask copy is exactly the engine's
mask times the term, and the draw sampled it. What it does *not* show: only **1,134 of 37,925** darkened texels sit on a depth edge (3 %), and
the darkened area follows object silhouettes (limbs, foliage, rock rims) rather than the ground where an object meets it. That is the
classic self-shadowing of a screen-space march whose thickness test compares **view depth only**: an object's own surface a few pixels away
differs in depth by less than the 3 cm thickness, so it counts as an occluder. The dump also proves the camera layout end to end: the view
matrix's three rows at +0x000, the inverse view at +0x030, the view-projection at +0x060, its inverse at +0x0A0 and the inverse projection
at +0x0E0, with the projection at +0x120 (the reflected layout of the engine's own light shader, read with `D3DReflect`), and the march must
project its view-space ray points with the **projection**, not the view-projection: the first fixed build projected with the view-projection
and every tap left the screen (term 255 everywhere, frame `20261030-183241` of the 240 s run before the fix). The next step is a hit test
that measures `|S - Q|` in 3D and a thickness that follows the step size, then the same dump again.

The screenshot pair from the earlier runs follows, and it still cannot show anything: two runs are never the same frame.

**Result on the benchmark, and what the screenshots can and cannot show.** Three builds, three answers.

The first build was inert: the index never recognised the sun light draw, because the engine's effect compiler writes the mask and the depth
as `g_SamplerShadow.T` and `g_SamplerDepth.T` while the two constant buffers keep their plain names (above). Three key-on runs and one
key-off run on that build produce the same picture, and the log carries no state line at all - a draw the index does not know is silent by
design.

With the suffix fix, the run of 2026-10-30 15:13 writes the module's lines on the same benchmark, the same preset and the same upscaler:
`Contact shadows: sun light draw shader indexed (mask slot 1, depth slot 0, g_LightParam slot 5, g_CameraParameter slot 1)`, then
`Contact shadows: sun light draw found, mask 2560x1440 format 87, sun (0.000, 0.800, 0.600) read as view space`, then
`Contact shadows: multiplying the sun mask (2560x1440) by a 0.50 m march over the 2560x1440 render window, strength 1.00, 12 step(s), sun
in view space toward the sun`. The draw is recognised, the mask is the display allocation's size and an 8-bit colour format, and the pass
draws. Two defects of that run are fixed: the shader fixtures were dropped by a bound that a session's ambient-occlusion shaders had filled
(they are key shapes too), and sixteen refusals of the cascade atlas writer in a row - it declares the same four names and binds the atlas
instead of the screen mask - switched the module off a minute into the session. The last run (15:41, after both fixes) keeps the pass
running: one refusal line, no switch-off line, and 16 shader fixtures with the mask shape, 8 of them with all four names. Two of those blobs
are the fixtures `contact_reflection` reads back.

**The screenshot comparison does not settle the localisation, and the numbers say why.** The benchmark's camera path is time based, so two
runs are never the same frame: the pictures differ where the scene moved, far more than the term the pass writes. Measured against the
key-off run (same preset, same upscaler, same window, every key at its default), mean |luma| difference per screenshot and the share of pixels
above 4 of 255 levels:

| pair | mean abs diff (levels) | pixels above 4 | pixels above 16 |
|---|---|---|---|
| key-off vs the four key-on runs of the inert build (no effect at all) | 1.9 to 22.5 | 9 % to 69 % | 2 % to 35 % |
| key-off vs key-on, fixed build (the pass running all session) | 1.8 to 16.2 | 8 % to 55 % | 2 % to 26 % |

The second row sits inside the first: run-to-run variation is larger than anything the pass adds, so this pair cannot show "confined to
shadowed contact areas". A frame-exact pair (a fixed camera, or a key the harness can flip between two shots of the same frame) is what the
comparison needs; the benchmark's screenshots are six moments of a moving camera and the harness takes them from two separate runs. What the
runs do establish is in the log above and in the two real blobs: the engine's bytecode is recognised, the draw is classified with the
expected sizes and formats, and the pass runs for the session without switching itself off.

**What it does not show.** One scene, one camera path, one time of day: the benchmark's sun is bright and high, which is the case the pass is
for, but night, interiors, rain and the second light draw of a scene whose two masks differ in size are not in it. The pass is also not
measured for GPU cost in a real session; the estimate is about 0.04 ms per light draw at 1440p in half resolution (two draws: the march and
the combine), and the log's run line carries the real sizes.

## Limits this build has

- **One space and one sign.** Both are keys (`ContactShadowsSunSpace`, `ContactShadowsSunSign`), both default to the measured reading, and
  neither has a menu control: they exist for a build where the measurement above has to be redone.
- **The scene HDR replay.** With `[HDR] Mode = 1` the fork replays a draw to re-run its tone chain; the pass stands down for those calls
  rather than running twice, so contact shadows do not apply in that mode. The DX11 path with SDR output is the measured one.
- **The character preview.** The preview's draws are a different render size, which the size rules refuse; the pass never touches them.
- **8-bit.** The engine's mask is quantised to 1/255, so a low strength lands on whole levels. At 0.2 the term is 51 levels of 255: visible
  on a large smooth surface, easy to miss on a busy one.
- **No temporal filter.** The term is a fixed function of the frame's buffers, by design (DLSS Super Resolution is not a denoiser). If a
  future session shows shimmer in motion, the next step named by the design is a depth-aware blur of the term at half resolution, not a
  history buffer.

## The second march, and what the second dump shows

The first march's hit test was a depth difference with an absolute band (0.03 to 0.06 m of view-space z). It accepts a surface that is a
metre to the side as long as its depth is a centimetre away, which is what made the term trace object outlines instead of contacts. The
march now does what a screen-space contact shadow normally does:

- **A start off the surface**: the ray begins on the receiver's normal by 1.5 of the world size of a pixel at that depth, and a hit closer
  than 2.5 pixels along the ray is ignored, so the receiver cannot shadow itself.
- **A thickness that follows the view depth**, through `pixel = 2 * depth / (P11 * windowHeight)`: 12 pixels' worth of world size, clamped
  to 5 mm .. 0.25 m, and at least 0.6 of the step that reached the tap.
- **A 3D distance test**: a hit needs the ray point to be within that thickness of the sampled surface point, not merely close in depth.
- **A receiver-facing test**: the tap's own normal (from its right and down neighbours) must point at the receiver, so an object's surface
  curving away from the receiver does not report itself as its own occluder.
- **A distance fade** over the last 30 % of the march, and a **terminator fade** (`dot(N, L)` from 0.02 to 0.25), because a surface that
  faces the sun only just is already dark in the engine's shading.
- **A normal from the pixel's own depth neighbourhood**: a neighbour takes part only if its depth belongs to the same surface, so a
  silhouette between two surfaces does not hand its normal to the pixel (the 2x2 quad derivatives did exactly that).

**What the synthetic scenes show** (`contact_pass_warp`, a software adapter, a scene built by intersecting rays with a floor, a box, a
sphere and a wall): on the wall's base 512 of 512 darkened texels are floor contact edges and none are on the wall; the box on the floor 19
and none on the box; the sphere 31 and none on the sphere's own surface; a flat floor and a 24-bit-quantised one stay at 255; full strength
reaches 0 at the wall's base; the CPU reference of both shaders matches the GPU to 0 levels of difference (373 pixels within 1 level before
the change, at 127 levels of disagreement).

**What the benchmark dump shows** (`20261030-contact-dump-r5b`, the sun at (0.000, 0.800, 0.600) view space, strength 1.0): 1.77 % of the
render window darkened (16,295 half-resolution texels); of those 3.0 % sit on a depth edge with the darkened texel on the far side (a
contact edge), 1.1 % on the near side (a silhouette), 95.9 % have no depth step in their 3x3 neighbourhood; nothing got brighter, nothing
outside the window changed and nothing where the term is 1.0 changed. A trace of the march at fourteen of those texels shows hits 4 to 25 mm
deep at 5 to 10 m distance, i.e. a lit surface's own screen neighbourhood is still within the thickness: at 10 m one pixel is 2.8 mm of
world, 12 pixels are 33 mm, and a surface that recedes or curves by a few millimetres per pixel along the sun's screen direction passes both
the depth bias and the 3D distance test. **The effect is therefore still not verified.**

**What is missing.** A test that separates "the surface's own slope" from an occluder: the depth bias must follow the surface's own
screen-space gradient along the sun direction (a slope-scaled bias, or the tap's normal compared with the receiver's), not only the view
depth. The synthetic scenes do not reproduce that case because their surfaces are flat or their occluders are far larger than a thickness;
a scene with a curved receiver (a sphere seen edge-on under a low sun, at 1440p) is what a unit test needs next, and then the same dump.

**The sun sign, decided from the engine's own shadows.** The dump's engine mask (before the pass) is the ground truth. Correlating the
depth-edge map with the engine's shadow over shifts of up to 40 half-resolution texels puts the peak displacement from an occluder's edge to
its cast shadow at (-4, +2) px, 153.4 degrees on the screen, against the sun's screen direction of -40.5 degrees: the shadow lies
**166 degrees** from the sun's screen direction, i.e. opposite it, which is what a vector pointing **toward the sun** produces. The check is
therefore consistent with the measured reading (to within 14 degrees, and the 4.5 px displacement is small, so the frame's occluders are
close to their shadows - a wider search or a scene with long cast shadows would tighten it).

**One new detour, and what it costs with the key off.** The pass needs the pixel shader the game has bound, and asking the context for it
on every draw was 0.65 ms a frame. So the fork hooks one more game function, `ID3D11DeviceContext::PSSetShader` (vtable index 9), in the
existing hook transaction beside the draw hooks. It exists in every session, key on or off, and it costs, per bind: one out-of-line call
into the module (1.0 ns measured, about 0.7 binds per draw, so 0.007 ms a frame) and one relaxed atomic load; with the key off, nothing
else. That is the price of the feature being switchable in a running session without a restart, and it is why the docs no longer say "no new
detour". The draw hook itself gains one relaxed load and one comparison per draw, and shader creation gains the byte scan described above.

**The cost, measured.** The draw hook's section now reads `0.00003 ms per draw` (0.32 ms at 9,250 draws) and the seam's
`0.00004 ms per bind`, both of which are dominated by the two `QueryPerformanceCounter` reads of the meter's own bracket: the draw hook no
longer reads the clock except on the 64 draws of a sample, and the section then is one atomic load (and, for the shader the table knows, one
stamp check). What the same line establishes is the rate: 177,979 pixel-shader binds for 262,208 draws, i.e. 0.68 binds per draw - about
6,300 binds a frame at 9,250 draws. Before this change every draw made a `PSGetShader` (0.65 ms a frame, 32x the design's 0.02 ms budget).
The draw side's real work is now one relaxed load, one counter and the meter's two tests, measured outside the game at 0.9 ns per draw,
so a frame's lookup is 0.008 ms (well inside the 0.02 ms budget); with the key off the seam adds one out-of-line call and one relaxed load
per bind (1.0 ns, i.e. 0.007 ms a frame at 6,300 binds), and shader creation adds one linear byte scan of each pixel shader's bytecode
(0.43 ns per byte: 1.8 us for a 4 KB shader, about 58 ms over a session's 33,000 shaders at that size). `QueryPerformanceCounter` itself
costs 16.2 ns on the same machine, which is why the draw hook no longer reads it.

**The pass's GPU cost is still not measured in a session**: the run line's `its two draws ... ms on the GPU` never appeared. The pass ended
the timestamp-disjoint query of the previous run without a begin on every run after the first, so the pair never completed; the pairing is
fixed (a run ends only the pair it began) and the software-adapter test now pins that a begin happens on run after run and that the number
resolves there. A session measurement (a real GPU) is still missing.
