# Ambient occlusion: this fork's own occlusion in place of the engine's

Companion to the [ambient occlusion](../FEATURES.md#ambient-occlusion-ffxiv-experimental) entry. This note is about the machinery: what the
engine's occlusion texture is in a captured frame, how the pass computes its own, and how the draws of a frame from the reader that completes the
pass's inputs onward get this one instead (the readers before it keep the engine's, which the status section states in full).

**Status: experimental.** The switch is off by default. The first session that turned it on found the engine's own occlusion disappearing
instead of deepening: the pass had stamped its reader's sampler register as the normal's slot, read G-buffer 3 there, whose texels decode to no
unit normal, and returned 1.0 for every texel; a linear combine then wrote that 1.0 over the engine's red channel. Both were fixed (the
texture's slot, and a product that makes a term of 1.0 the identity), and in a second session the user confirmed the pass works: about 610 to 640
reader draws of a frame were bound to this module's texture.

That session's log shows the frame fallback this note's first step describes, and the numbers are worth stating exactly because they are easy
to misread: 24 `the first reader of the frame could not serve it` lines between 17:48:36.699 and 17:49:57.729 (24 is the module's log budget,
so that is a floor, not a count), in three reader shapes - 6 declaring the camera table and neither texture, 10 the depth and the camera, 8 the
G-buffer normal and the camera. Frames were served inside that window (a pass at 17:49:04.506, 636 reader draws bound its texture) and
refusals were interleaved with served frames all along; the 64th frame in a row that no reader completed came at 17:49:58.660, with the
OptiScaler menu opened at 17:48:52.774 and the window unfocused from 17:48:55.991 - and that build had no pause, so the module stood itself down
for the rest of that session there (`off for this session`). The two shapes that read as readers of one frame (a light draw's and the
composite's) are 42 to 44 ms apart, which is about two frames at this session's 45 frames a second (22 ms a frame), not two readers of one
frame - so what the merge's premise rests on is the captures, not that spacing: in a captured frame the sun light draws run at ordinals
1370-1379, the deferred composite at 1442 and the materials from 1444, and those light draws declare the depth at texture slot 0, the G-buffer
normal at slot 7 and the camera table at constant-buffer slot 1, while the composite declares the normal at 9 and no depth. That is a premise
about one draw order in two captured frames, not a measurement of the frames that refused.

A frame now collects the three inputs across its readers, and the pass runs at the reader that completes them - which makes a frame torn, and
this note has to say so. The texture is swapped only after the pass has run, so every reader of that frame before the completing one keeps the
engine's occlusion and every reader after it gets this module's. In the captures read here the first occlusion reader of a frame is always a
light draw, and all but one of them declares the depth at texture slot 0, the G-buffer normal at slot 7 (0x9BB42D42 in one capture, 0xC6F83D9F
in another) and the camera table at constant-buffer slot 1 - so there the pass runs at the frame's first reader and no reader of that frame keeps
the engine's mask. The exception is the measured tear: in one captured frame the first three occlusion readers are light draws of a family that
declares the depth and the camera but no `g_SamplerGBuffer4` (crc 0xC2DD805A, ordinals 3871, 3874 and 3877), and the fourth - a light draw that
declares the normal at 7 (0x9BB42D42) at ordinal 3880 - completes the frame, so those three keep the engine's mask and everything from 3880 on
gets this fork's. A frame whose first occlusion reader is the composite (the normal at 9, the camera at 1, no depth) or a material draw (the
camera table at 3, neither texture) has no later depth supplier in that order, so it is never completed and keeps the engine's occlusion for the
whole frame. Fail-open is unchanged - a draw this module does not serve has the engine's texture - but serving starts at the completing reader,
and this note used to read as if it always were the whole frame.

A row of frames that no reader completes no longer costs the session the effect: after 64 in a row the module pauses, looks at a frame again
about 2.7 seconds later (120 frames at that session's 45 frames a second), and re-arms at the first frame it can serve (the section on the
session says why the streak is usually a menu's). A look is a whole frame, not its first reader: the frame it inspects is read from its first
draw to its last bounded one, a reader of it that only has part of the pass's inputs neither ends the look nor defers the next one, and a look
starts its frame from scratch - without that the look would spend itself on the frame that paused the module, whose one attempt was already
spent, and the pause would be a stand-down that never came back. Each frame is counted once towards the streak, whether it waited first or was
refused at frame level. That path has not been judged in game yet. The tests say the pass reproduces a CPU reference of the same search to
within a thousandth of a level on a software adapter, that a flat floor stays lit, and that a corner darkens; they say nothing about how the
picture looks in a real scene.

## What the game does

Measured from one captured frame (`draws.csv` format 3, a rainy exterior at 3840x2160 with the picture rendered at 1920x1080):

- `g_SamplerOcclusion` is a `B8G8R8A8_UNORM` texture at the **render size** (1920x1080 in that session; the G-buffer and the depth are at
  the display allocation's 3840x2160 and the render window is a 1920x1080 viewport at a +0.5 origin inside them).
- Readers of it in one frame: about 50 sun light draws (two `R11G11B10_FLOAT` targets), the two deferred composite draws (three vertices,
  one `R16G16B16A16_FLOAT` scene target) and about 250 forward/material draws into that same scene target - 299 bindings at that texture's
  pointer in the frame, at slots 1 (composite), 1 (light, which also bind the depth at 0 and the five G-buffers at 3..7) and 8..14 (forward
  draws, whose own reflection says where).
- The engine binds a **4x4** texture of the same format under the same name on a few dozen draws that have no occlusion of their own. That is
  why the module only serves the render-size binding and passes the placeholder on without counting a refusal.
- The texture the module replaces **alternates between two objects from one frame to the next**. Nothing about it is remembered across a
  frame.
- The producer draw in that frame is a **four-vertex `Draw` at a 960x540 viewport** inside the 1920x1080 target, reading G-buffer 0 and
  binding no constant buffer at all; the next draw writes an `R16G16_FLOAT` 1920x1080 target from the same 960x540 window. So the engine's
  own occlusion is half the render window's resolution (a quarter of the G-buffer's), upsampled by whoever reads it.

### The quadrant, and why a texel of ours maps by the allocation and not by the window

The engine fills **only the top-left 960x540 of its 1920x1080 occlusion texture**, and the readers sample that texture with the same
allocation-relative screen coordinate they use for the G-buffer and the lighting: the captured readers multiply the pixel position by
`g_CommonParameter.m_RenderTarget` and use that one coordinate for the lighting texture (a 3840x2160 allocation) and the occlusion texture
alike. A reader at the bottom-right of the 1920x1080 picture therefore fetches the occlusion texel at about (959.7, 539.7) - the last texel of
the 960x540 quadrant the producer wrote, and one inside it, not one near the middle of the texture.

Two consequences, and both are the whole of this module's geometry:

- a texel of ours stands for the depth texels at the same fraction of the **depth allocation** (the ratio of the two sizes: 2 in that session),
  never for the depth texels under the render window's own coordinates;
- the part of the texture to compute is the part that lies inside the render window at that ratio: 960x540 texels, exactly the engine's own
  quadrant. Everything else is not the engine's answer and keeps the engine's texel, which the pass copies into its target before it writes
  the quadrant.

A mapping by the window's size instead (`scale = window / occlusion = 1` in that session) would put our full-resolution occlusion into a
texture the readers read as a 2x magnified quarter: dark corners in the wrong places, and a picture that does not line up. The block's **first**
texel is the one a texel of ours stands for, not the centre of the block: at 2:1 the mapping is `2t`, and scaling the pixel centre (`t + 0.5`)
would read `2t + 1` and move the picture by one texel. The host test pins every part of the rule: a depth allocation twice the occlusion
texture's leaves every texel outside the quadrant byte for byte the engine's, the quadrant itself comes out identical to the same scene rendered
with the picture as its own allocation, and it matches a CPU reference of the same search at that 2:1 scale texel for texel (a scene with a row
of walls, so the shift the wrong texel produces is visible wherever it lands).
- The G-buffer target that holds the normal is the **fifth** (`g_SamplerGBuffer4`, `B8G8R8A8_UNORM`, 3840x2160). Its rgb decodes to a
  world-space normal (`2c - 1`) whose length is 1.000 for 99.7 % of the frame's pixels, with the floor at (0, 1, 0) - so +Y is up and the
  values are world space, not view space.

## What the module does

1. **Once a frame, at the first reader that can supply the pass's inputs.** The draw path recognises a reader by the name in its pixel
   shader's reflection (one row of the shared name table, one bit in the packed anchors word). A frame's readers do **not** all declare the
   same three inputs, and the game does not put the same reader first every frame (measured below), so each reader of the frame contributes
   the depth, the normal and the camera table it does declare, the frame keeps the first usable binding of each, and the pass runs at the
   first reader for which all three are in hand - which may be a later reader than the frame's first, and then the frame is torn (the section
   on the status above says which draws keep the engine's mask). A reader that supplies nothing usable leaves the frame waiting: the draw runs
   with the engine's texture, nothing is spent, and one rate-limited line says what the frame has and what it is waiting for. A frame no
   reader completes keeps the engine's texture and counts once towards the pause; the waiting is bounded to 64 readers of a frame, so a build
   that cannot supply the inputs costs a fixed number of state queries rather than every draw of the frame. The three slots come from the
   shader's **texture** bindings, never from its samplers': the effect compiler writes a texture and its sampler under one name and lists
   every sampler before every texture, so a scan that took the first name it saw stamped the sampler's register and the pass read another
   texture as the normal (measured: G-buffer 3, no valid normal in any texel, the whole term 1.0).

   **The three inputs are tied to one view.** The camera table's device object identifies the view: in a captured frame the sun light draws
   (camera at constant-buffer slot 1), the deferred composite (slot 1) and the material draws (slot 3) all bind the *same* `g_CameraParameter`
   object, whatever slot their own reflection gives it. So a reader contributes a texture only together with the camera table it binds, the
   frame adopts that table with the first texture it takes, and a later reader whose camera is another object is another view - a reflection, a
   mirror, a preview camera - whose depth has the same allocation's size and would otherwise pass for this frame's. Such a frame is refused
   (`another view's camera table`), not served with a search run from two views' buffers. What the guard does not catch: a view that reuses one
   camera object and writes other matrices into it, and a later *served* reader of another view (the swap is per draw and compares only the
   occlusion texture's identity).

   **What the three reader shapes are** (one session's log and its captures, 2026-11-01): a material draw declares the camera table at
   constant-buffer slot 3 and neither the depth nor `g_SamplerGBuffer4` (it reads its own `g_SamplerNormal` instead); the deferred composite
   declares the G-buffer normal at texture slot 9, the camera at 1 and no depth; a light draw declares the depth at texture slot 0, the
   G-buffer normal at slot 7 and the camera at 1, and at least two captured light families declare only the depth and the camera
   (0xC2DD805A in this session's tear frame, 0x3306FD4B a day earlier). With the frame's inputs collected across its readers, a frame whose
   first reader is a light draw with the normal is served at that reader; a frame that starts on the light family without the normal is served
   at the next light draw that has it, its first light draws keeping the engine's texture (the measured tear above); a frame whose first reader
   is the composite or a material draw has no later supplier of the depth in the captured order and is never completed (the status section
   above).
2. **The pass** (two draws in a private device-context state, so no game binding is disturbed): a horizon search over the quadrant, the
   part of the occlusion target that lies inside the render window, into an owned `R16_FLOAT` target of the occlusion texture's size; then a
   4x4 denoise over the same quadrant, the strength, the blend and the 8-bit encoding into an owned `B8G8R8A8_UNORM` target of the engine's
   size, which starts as a copy of the engine's own texture so every texel outside the quadrant is the engine's. The camera table is copied
   GPU side with a partial `CopySubresourceRegion`; nothing is read back to the CPU.
3. **The swap.** Every later draw of the same frame whose pixel shader declares the name and that binds the same texture gets this one bound
   at its own slot for that draw, and the game's binding back afterwards - also when the draw throws.

The picture is a fixed function of the frame's buffers: no frame number, no random offset and no history. A still scene gives the same
occlusion every frame, which matters because the upscaler is not a denoiser.

## The search

Two horizon slices, `AmbientOcclusionQuality` steps each (4, 8 or 16), in the tangent plane of the pixel's normal. The two slice directions
are rotated per pixel by a 4x4 pattern (a banding breaker, not a random number). A step is longer than the one before it, so the near field
gets most of the taps and the last one still reaches the whole radius.

A tap counts as an occluder only when

- the sampled surface is in front of the ray point (a surface behind it is the background), and not farther in front than the radius
  (the thickness heuristic: a wall a metre nearer the camera is another object, not the occluder this ray is looking for), and
- it rises above the pixel's tangent plane by more than a small cosine bias.

The second rule is what keeps a flat floor from shadowing itself: its neighbours sit *on* its tangent plane at any depth quantisation. The
value written is the mean of the two slices' horizons, clipped to the plane, so 1 is a pixel that sees the open sky in both of its directions.

Every texel is reconstructed from **its own depth and its own centre**. That is not a detail: a position reconstructed from a fractional
projection coordinate and a neighbour texel's depth lands up to three centimetres off the surface, and a flat floor then reads itself as an
occluder. The host test's CPU reference found exactly that, as a uniform half-darkening of the near floor, before anyone saw it in game.

## One channel, and only one

The engine's occlusion value is its texture's **red channel**, and 1 is unoccluded: every reader whose bytecode was captured samples exactly
that one channel and uses it as `1 + (x - 1) * k`, so the pass writes red and returns the engine's own green, blue and alpha untouched. Both
values are visibility factors in that same convention, so they compose by multiplication: `AmbientOcclusionBlend` takes
`engine * (1 + (ours - 1) * blend)`, so 1.0 applies the engine's occlusion and this module's multiplied, 0.0 keeps the engine's texel, and a
term of 1.0 - a search that found nothing, or a strength of 0 - is the identity at every blend. That last property is the fail-open one: an
empty or wrong term cannot delete the engine's occlusion, which is exactly what a linear mix did at blend 1.

Writing one channel and copying the rest is the smaller failure: a channel this module has not measured then cannot be corrupted by it, and the
worst case is a picture that does not change. The convention 1 = unoccluded is the other half of the same argument, because it is the direction
that fails open; if the engine's own encoding were the opposite, the effect would show as an inverted darkening and the fix is one line, not a
redesign. The light and composite shaders were not captured, so a session still confirms that they read the same channel.

## What the module remembers about a shader

Nothing but the shader's own answer. When the game creates a pixel shader whose bytecode names the depth, the G-buffer normal or the camera
table, the module reflects its slots once and writes them on the shader object (`SetPrivateData`); the draw path reads them back with one call
for each reader of a frame it inspects. There is no table of shaders anywhere, and that is a fix rather than a preference: the game creates tens
of thousands of pixel shaders a session, every material shader declares the camera table, and a bounded table of them loses the sun light
shader's entry long before a frame's readers ask for it. The device test pins the shape of it - a stamp written on one shader, five thousand more shaders created
after it, and the first one's stamp read back unchanged - and the note's cost section says what the read costs.

## The keys

| key | default | what it does |
|---|---|---|
| `AmbientOcclusion` | false | the master switch; off is today's picture and the pass releases its textures |
| `AmbientOcclusionRadius` | 1.0 m | how far around a pixel the search looks; costs nothing extra |
| `AmbientOcclusionStrength` | 1.0 | how much of the computed occlusion is applied; 0 runs nothing |
| `AmbientOcclusionBlend` | 1.0 | how much of this module's term is applied on top of the engine's own, multiplied; 0 runs nothing |
| `AmbientOcclusionQuality` | medium | 4, 8 or 16 taps a direction - the one setting that changes the GPU cost |

A master switch left on with the strength or the blend at 0 arms nothing: the draw path is then one relaxed load per draw, exactly as with the
key off.

## What it refuses, and what it costs

Reasons a reader is left alone (each is one log line the first time it appears, the draw runs unchanged with the engine's texture):
nothing bound at the occlusion slot, a texture that is not the engine's render-size one, no usable depth (nothing bound, a multisampled one),
no usable G-buffer normal, no camera table or one smaller than the matrices, a depth and a normal that are not the same allocation, a reader
of the frame that binds another view's camera table, a render window of a few texels or one that does not fit its depth, a character-preview
replay, an owned texture or a shader that could not be created, and an exception. After sixty-four frames in a row left alone
(one count a frame, at its first wait or its frame-level refusal) the module pauses with one line and inspects every reader of one frame again
120 frames later - about 2.7 seconds at the measured session's 45 frames a second - or, for a present counter that stands still, after 65,536
reader draws; the look starts its frame from scratch, so it inspects instead of reaching an attempt that was already spent. A menu, a loading
screen or a run of frames whose first reader is the composite then costs the effect for a few seconds and not for the session; the first frame
it can serve re-arms it at once, and any change on the card does too. Its pause is its own: the engine pass overrides card's knobs stand
themselves down separately, and either one being off leaves this pass armed (a paused module keeps the draw path armed too, because that early
return is how its next look is reached).

Per reader draw the cost is one relaxed load, one `PSGetShaderResources` for its slot, one `GetResource`, the slot swap and its restore. Per
frame: one `PSGetShader`, one copy of the camera table, one copy of the engine's occlusion texture, one parameter write, two draws over the
quadrant only and a timestamp pair that is read a run or two later and never waited for. The search dominates: over the 960x540 quadrant at
the medium quality it is about 16 depth loads a pixel, a quarter of what the same quality would cost over the whole texture. The log's GPU
line reports the measured pair once it has landed.

Four lines are there for a session to read rather than for a player. The first is a wait's: `the frame waits for another reader (the reader
declares or binds no usable depth)` followed by what the frame has collected - the occlusion texture's size and format, the depth's and the
normal's sizes and slots, the camera table's size and slot, and the render window - so a frame that is one input short is visible before the
picture is judged. The second is a frame's refusal: `the frame could not be served (…)`, with the same numbers, for a frame no reader completed
or whose inputs cannot agree (a size, the view). The third says how many reader draws one frame bound this module's texture (roughly three
hundred occlusion bindings a frame in the capture; a number near two hundred means some readers' shaders declare the name in a way the
reflection did not read, and those keep the engine's texture); the card shows the same number for the last frame. The fourth is the run line,
one per toggle, which is what tells a wrong slot from a scene the search finds nothing in: `Ambient occlusion: 1920x1080 at 8 steps a
direction, radius 1.00 m, strength 1.00, blend 1.00, depth 3840x2160 at 0, normal 3840x2160 at 7, camera 1024 at 1, its GPU time not measured
yet` - the occlusion texture's size and tuning, then the depth, the normal and the camera table the pass read, each at the slot of the reader
of the frame that supplied it. A fifth line is the pause's (`… so the module pauses and looks at a frame's readers again 120 frames later
(about 2.7 seconds at 45 frames a second)`).

## What is not known

- Whether the visible difference is the one a player wants. The acceptance test is two screenshots at the same camera in a corner-rich scene
  (a stone wall meeting a floor, a pillar's foot), the master off against on, and a third with the blend at 0.5 if the full strength is too
  dark.
- What the engine's own producer actually computes. It writes the quadrant with no constant buffer at all, which this fork has not tried to
  explain; what it does not do is write the whole texture, and that decides where this module's own texels go.
- Whether a full-screen pass at the render size costs what the log says on a real GPU. It has not been run in game yet.
