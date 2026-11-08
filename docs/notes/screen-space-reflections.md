# Screen-space reflections: this fork's own reflection in place of the engine's planar map

Companion to the [screen-space reflections](../FEATURES.md#screen-space-reflections-ffxiv-experimental) entry. This note is about the
machinery: what the engine's planar reflection map is in a captured frame, what the pass marches, and how every water draw of a frame gets this
one instead.

**Status: experimental.** The switch is off by default and the visible effect has not been judged in a game session yet. The tests say the pass
reproduces a CPU reference of the same march to within a thousandth of a level on a software adapter, that the floor below a bright square shows
the square's colour with a confidence above one half, that a texel whose ray leaves the picture keeps the engine's own bytes byte for byte, and
that a blend of 0 is the engine's map; they say nothing about how the water looks in a real scene.

## What the game does

Measured from one captured frame (`draws.csv` format 3, a rainy exterior at 3840x2160 with the picture rendered at 1920x1080):

- `g_SamplerReflectionMap` is a `B8G8R8A8_UNORM` texture at the **picture's size** (1920x1080), and three draws of the frame write it at a
  960x540 viewport: the engine's own water reflection chain, at half the picture's resolution, into the top-left quadrant of that texture.
- Its readers are the **water material**: two shader families, 11 draws of one and 27 of the other in a frame, all `DrawIndexed` into the FP16
  scene target at the render window. Both declare `g_SamplerWaveMap`, `g_SamplerFresnel`, `g_SamplerDistortion`, `g_SamplerRefractionMap`,
  `g_SamplerCaustics`, `g_SamplerVPosition`, the two light buffers, the reflection cube array and the camera table. Neither declares
  `g_SamplerDepth` and neither declares a G-buffer target: the geometry a water shader has is the **view-position buffer**
  (`g_SamplerVPosition`, `R16G16B16A16_FLOAT`, at the display allocation's size).
- The deferred composite and the material draws use the five-probe reflection **cube array** (`g_SamplerReflectionArray`, 256x256, five slices),
  not this map.
- The engine's own screen-space ray march (a hierarchical-Z chain, named `g_SamplerHierarchicalZ` in other builds) never ran in any captured
  frame: no draw bound that name.

### The quadrant, and why a texel of ours maps by the allocation

The engine fills **only the top-left 960x540 of its 1920x1080 map**, and the readers sample that texture with the same allocation-relative screen
coordinate they use for the lighting and the G-buffer. So a texel of ours stands for the scene texels at the same fraction of the **scene
allocation** (the ratio of the two sizes: 2 in that session), never for the scene texels under the render window's own coordinates, and the part
of the map to compute is the part that lies inside the render window at that ratio: 960x540 texels, exactly the engine's own quadrant.

What is disassembled and what is inferred: the allocation-relative coordinate was disassembled for the light shaders that sample the same buffers,
and for the water shaders it is inferred from the producer's and the reader's sizes (a 960x540 producer viewport inside a 1920x1080 map, read by
draws of a 1920x1080 picture). A session's own capture at a different render window settles it, which is why one is on the session's list.

That rule is not restated here: it is the ambient occlusion pass's own (`ext/ffxiv/engine/AoRules.h`, `Mapping` and `RegionOf`), and this pass
feeds it its three numbers - the map's size, the scene allocation's size and the render window - so the two passes cannot disagree about where a
texel of theirs goes.

## What the module does

1. **Once a frame, before its first water draw.** The draw path recognises a sampler of the map by the name in its pixel shader's reflection (one
   row of the shared name table, one bit in the packed anchors word). The map's slot, the view-position slot, the camera slot and whether the
   shader is a water surface are stamped on the shader object itself, exactly as the ambient occlusion pass stamps its own slots, so nothing is
   looked up per draw and no table can lose it. Every one of those slots is read through one typed lookup (`diag::BindPointOf` in
   `ext/ffxiv/diag/ShaderBindings.h`): the effect compiler writes a texture and its sampler under one variable name, as `<name>.T` and `<name>.S`,
   and writes the sampler first at a register of its own, so a scan that compared names alone records the **sampler's** slot - a texture at the
   draw that is not the one the pass meant. The ambient occlusion, the ground wetness, the shader index and the contact shadow index read their
   slots through the same lookup (a test parses one compiled shader whose samplers sit at s1/s3/s5/s7 and whose textures sit at t4/t6/t9, and holds
   every module to the texture's register), and a text pin holds that no caller compares a binding's type by hand. The first water draw of a frame
   that binds the engine's map, the view-position buffer, the camera table and a single-sample scene target it can copy runs the pass; a draw that
   cannot serve its frame is refused with one log line that carries what it bound, and that frame's one attempt is spent so the refusal does not
   repeat a few dozen times.
2. **The copy of the scene.** The water draw's own render target *is* the scene the frame had rendered so far, so the pass copies the render
   window's texels out of it into an owned FP16 texture of the allocation's size before it binds anything. Nothing of the game is written: the
   target is a copy source only. The game still has that target bound when the copy is taken, so the pass unbinds its render targets and
   depth-stencil for the one copy and puts them back at once; a fresh copy of the scene starts cleared, so a copy that does not land reads as sky
   and answers with the engine's own texels instead of marching whatever the allocation held. A frame has one attempt, so one scene target is
   copied once and a frame that drew into two targets is served from the first one's copy (a session would see it as a reflection of the wrong
   picture, not as a wrong format).
3. **The march.** For every texel of the quadrant: the view-space position of the scene texel it stands for, the reflected direction about the
   **water plane's normal** (the world's up turned into view space by the view matrix's own second column - the map this pass replaces is a mirror
   about the water plane, so the normal is the same at every texel and is not derived from neighbouring pixels), and a geometric step ramp out to
   100 m. A step that leaves the picture ends the march with no hit. A step whose sampled surface lies behind the ray point is a crossing; the
   interval it sits in is halved a few times to the surface's own texel, and the thickness heuristic rejects a surface farther behind the crossing
   than the key allows. The confidence is the crossing's distance to the edge of the picture over the fade key, and the reflected colour is the
   scene colour at the crossing's texel. The confidence stays inside the pass: it scales how much of the hit's colour replaces the engine's, and
   the map's alpha comes back exactly as the engine wrote it, so the only channel a session can see change is a colour one.
4. **The swap.** Every **water** draw of that frame whose pixel shader samples the map and binds the same texture gets this one bound at its own
   slot for that draw, and the game's binding back afterwards - also when the draw throws. A draw that samples the map without the water anchors
   (the unnamed non-water reader the capture shows, an effect's own copy) keeps the engine's texel: this pass answers for the water surface only. A
   texel whose march missed, and every texel outside the quadrant, keeps the engine's own bytes: the owned target starts as a copy of the engine's
   map.

The picture is a fixed function of the frame's buffers: no frame number, no random offset and no history. A still scene gives the same reflection
every frame, which matters because the upscaler is not a denoiser.

## Why the geometry is a view-position buffer and not a depth

The captured water shaders declare no depth texture and no G-buffer target, and the draw seam reads a shader's slots from its own reflection, so a
pass that waited for `g_SamplerDepth` at a water draw would never run. What the water shader does declare is the engine's view-position buffer:
the camera is the origin in that space, the position is exact at every pixel, and a marching ray's own depth along the ray follows from the
position it samples - the thickness rule compares view-space distances, so no surface normal is needed at the hit either.

The space **is view space**, and a captured shader of the engine says so: the glare source (`0xAE4A07BE`, kept in a capture's shader fixtures)
rebuilds the previous frame's view-space position from `prevProjToView` and the previous depth, then subtracts `sViewPosition` - this buffer - from
it and scales the squared difference by that sample's own view depth (`mul r1.w, r3.z, cb0[4].y; mul r1.w, r1.w, r1.w; div r1.z, r1.z, r1.w`).
A temporal-rejection distance only means something if both are in the same space, and the reconstructed one is the view's. The deferred composite
agrees: it normalizes `-g_SamplerViewPosition` into a view vector and transforms the G-buffer normal by the view matrix's rows before dotting the
two. So the pass's `V = normalize(P)` and its view-space up are in the same space by the engine's own use, and no matrix is applied to the sample.

The offline test (`tests/ext/ffxiv/engine/ssr_frame.cpp`, local: `OPTISCALER_SSR_FRAME_DIR`) answers the cost question from a capture: it runs this
pass on the WARP adapter over the dump's own `scene.bin` and `vposition.bin` (or a depth target rebuilt through the camera table's inverse
projection), with the shader compiled for the march's exit codes. The capture of 2026-11-01 17:35:25 holds neither the view position nor a depth
target (eleven files, all colour or G-buffer), so its own march is still unmeasured. What the test can settle, it settles on a reference frame built
from that capture's camera table: a closed box - a floor 1.5 m below the camera and a wall 12 m ahead - so every texel of the render region is
geometry and none is sky. There every one of the 518,400 region texels marched, 348,531 of them (67%) found a reflection, the mean hit was 1.32 m
along the ray, 23% of the texels ended on the thickness rule and 8% at the distance limit. That says the march does not leave the picture at once
on geometry that surrounds the camera. It says nothing about a real frame, whose open sky is exactly the case the reference cannot produce: the
shader's `dot(Q, Q) > 1e-6` early exit and its steps-exhausted exit never run in the reference at all.

The session's own 0.097 ms for 48 steps is therefore not explained either way. The reading that it means "the rays exit at once" does not follow
from the reference: the step ramp starts at 8.08e-7 m and grows by 1.45x per tap, so a hit at the reported mean 1.32 m is reached after about 37 of
the 48 steps, which puts the reference frame's march at about 20 million taps (518,400 texels times that mean step count - a number derived from
the two, not the exit histogram, whose own 169,869 entries are the 4,860 rays that left the picture, 41,933 at the distance limit, 121,151 on the
thickness rule and 1,925 below the confidence threshold). That is a near-full march, not a handful of taps, and about 210 G taps/s for the reported
time, a rate a 63 MiB buffer that fits a large GPU's cache can serve. So the number is consistent with a full march as much as with
an early exit, and the measurement that settles it (a tap count on the real frame) is what the next capture with the view position provides.

## The keys

| key | default | what it does |
|---|---|---|
| `ScreenSpaceReflections` | false | the master switch; off is today's picture and the pass releases its textures |
| `ScreenSpaceReflectionsSteps` | 48 | taps the march takes, 16..128: the one setting that changes the GPU cost |
| `ScreenSpaceReflectionsThickness` | 0.3 m | how deep behind a crossing a surface still counts as the one the ray hit |
| `ScreenSpaceReflectionsBlend` | 0.65 | how much of ours replaces the engine's where the march found something; 1.0 is ours only, 0 runs nothing |
| `ScreenSpaceReflectionsFade` | 0.1 | the fraction of the picture at its edges over which a hit is trusted less |

A master switch left on with the blend at 0 arms nothing: the draw path is then one relaxed load per draw, exactly as with the key off. The key
arms the seam on its own: the draw path is entered while any of the module's features wants a draw, so the engine pass overrides' master switch
neither has to be on for the reflections nor turns them off when it is.

## What it refuses, and what it costs

Reasons a water draw is left alone (each is one log line the first time it appears, the draw runs unchanged with the engine's map): nothing under
the reflection name or a texture that is not the engine's map, no single-sample scene target this pass can copy, no usable view-position buffer
(or one at another allocation), no camera table or one smaller than the matrices, a render window of a few texels or one that does not fit its
scene, an owned texture or a shader that could not be created, and an exception. A character-preview replay is passed on instead of refused: it
is not a draw of the frame, so it is not counted as a refusal, it does not spend the frame's one attempt and the frame's own water draws are still
served while a preview is open - the passed-on counter and its one line record it like the other shapes this pass does not answer for. After sixty-four refused water draws in a row the module switches itself off for the session with one line;
the switch turned off and on again re-arms it. Its switch-off is its own: the engine pass overrides and the ambient occlusion pass keep their
arming.

A draw the module does not serve is not a refusal, and the two are read differently: a refusal is a shape this pass meant to serve and could
not, counts towards the sixty-four and is written once, while a **passed-on** draw is one this pass does not answer for. Every passed-on stop
has its own reason and its own counter (not a reflection reader, the card off or unarmed, this module's own work, another module's replay, a
shader whose stamp never landed, a sampler of the map without a water anchor, nothing bound at the slot, another texture there, the frame's one
attempt spent, another binding, no texture to bind, an empty slot), each is written **once a session** (never counted as a refusal, so it can
never stand the module down), and a passed-on line names the shader's bytecode CRC where the draw's own shader is at hand. The card shows the
reasons that happened under its state line, and the card writes one line when its own arming changes ("the card is on, blend 1.00, so a water
draw can run"), so a session that sees no run can tell "the card was off" from "the card was on and the water shader carried no stamp" without a
counter's help.

The cost a session reads for every draw is nothing new: a passed-on draw pays one relaxed load and, for a reflection reader, the stamp read it
already made. Per water draw the cost is one relaxed load, one `PSGetShaderResources` for its slot, one `GetResource`, the slot swap and its restore. Per frame:
one `PSGetShader`, one copy of the camera table, one partial copy of the render window out of the scene target, one copy of the engine's map, one
parameter write, one draw over the quadrant only and a timestamp pair that is read a run or two later and never waited for. The march dominates:
over the 960x540 quadrant at 48 steps it is 48 position loads, a scene-colour load per texel and the same number of projections - about a quarter
of what the same march would cost over the whole map. The owned copy of the scene is an FP16 texture at the display allocation's size (63 MB at
4K), created once per size.

The log's GPU line reports the measured pair once it has landed. Two lines are there for a session to read rather than for a player: the refusal's,
which names the reason and then what that draw bound, and the served line, which says how many water draws one frame bound this module's texture (a
capture shows about thirty-eight reflection-map bindings in a water frame, so a number far below that means some readers' shaders declare the name
in a way the reflection did not read and those keep the engine's map).

A capture answers the same questions without the log. `engine.txt` carries a screen-space reflections block after the engine pass overrides'
counters: `ssr module:` (the card), `ssr armed:`, `ssr runs:`, `ssr readers:` (the water draws that bound this module's texture), `ssr declined:`,
one `ssr decline: <reason> = n` line per reason that happened and one `ssr passed on: <reason> = n` line per passed-on reason, then `ssr window
runs:`, `ssr window readers:`, `ssr window declined:` and the same reason lines for this capture's own frames (the counters minus the baseline
taken when the capture armed). `tools/diag/session_answers.py` reads that block for its engine question, so one capture at the water says whether
the card was armed, whether the module ran and, when it did not, which gate stopped the water draws. The block's `ssr last water draw:` line carries
the stamp of the last water draw the gate passed on and the view it read there (`stamp 0x..., map slot N, read slot N, view ..., array ..., format
..., WxH, texture ...`), so a capture says which slot the module read instead of leaving it to be inferred; the `OtherMap` line in the log names the
same facts.

## What is not known

- Whether the visible difference is the one a player wants. The acceptance test is a water body in view with the camera low over it, two
  screenshots at the same camera with the master off and on, and a third with the blend at 0.5 if the reflection is too strong.
- The session's own march cost: a dump that carries the view position (the frame dump takes it since this build) answers it; the capture of
  2026-11-01 17:35:25 does not, and its 0.097 ms is therefore still only a number.
- Whether the water plane the pass mirrors about is the one the engine's own map is a mirror about. A fixed normal is right for still water and
  wrong for a wave crest that leans, which is where the engine's own chain has more to work with; a session's screenshots on a calm surface say
  whether the plane alone is enough.
- Whether the engine's own chain looks better in the scenes this pass cannot march (off-screen reflections): where the march misses, the engine's
  texel is kept, so the picture is the engine's own there rather than a hole.
- The colour encoding. The scene is a linear HDR buffer, and this pass writes the hit's RGB straight into the map's eight-bit UNORM, with no
  exposure and no tone map: a reflection brighter than 1 clips to white, and the darks are quantised differently from the engine's own chain. A
  session should look at a bright reflection (a lamppost, the sun on the water) next to the engine's map at blend 0 and say whether it clips.
- Whether the copy of the planar map is a typeless resource in another build. A typeless parent is accepted as its typed sibling here, so a
  mismatch would cost nothing; no capture has shown one, and the CSV prints the view's format, not the resource's.
