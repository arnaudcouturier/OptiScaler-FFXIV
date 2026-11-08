# Draw diagnostic, format 3

The reference for the files the developer draw diagnostic writes (`[FFXIV] DrawDiagnostics`, ini only, off by default; the
feature text is in `docs/FEATURES.md`). It covers format 3 and the files format 3 added (the pointer column, `views.txt`,
`cbdump.csv`, the one-frame `dump\`, `shaders.txt` and `engine.txt`); format 2 is the same file without them, and old run folders
stay readable.

Everything is under `<folder of the DLL>\OptiScaler\draw-diag\<run>\`. A `<run>` folder is `yyyyMMdd-HHmmss`; from the second
capture of one session it is `yyyyMMdd-HHmmss-<n>`, because two captures can land in the same second. The eight newest run folders
are kept.

## draws.csv (format 3)

The first line is `# FFXIV draw diagnostics, format 3`, then the column line, then one row per draw, one `#` line per frame and a
trailer. A `#` line is a comment everywhere.

| Column | What |
|---|---|
| `frame`, `ordinal` | the captured frame and the draw's place in it (1 .. draws of the window) |
| `api` | `Draw`, `DrawIndexed`, `DrawInstanced`, `DrawIndexedInstanced` or an indirect call |
| `context` | `game` = the context the FFXIV hooks latched onto, `other` = anything else |
| `count`, `instances`, `start`, `base` | the draw's own arguments |
| `stage` | `1` tone map, `2`..`5` the post-tone-map stages, `6` UI, `0` none of those (from the shader catalog by bytecode CRC) |
| `ps_crc` | the pixel shader's bytecode CRC32 as `0xXXXXXXXX`, `-` when the shader was created before the capture was armed |
| `srv` | per bound texture slot: `slot:reflected name:format:size:pointer`, several joined with `|` |
| `cb` | per bound constant buffer: `slot:reflected name:bytes:pointer` |
| `rtv` | every bound render target: `slot:format:size:pointer` |
| `dsv` | the depth-stencil view: `format:size:pointer`, `-` when none is bound |
| `viewport` | `count:width x height + x + y` for up to four viewports |
| `blend` | enable / src / dst / op / srcAlpha / dstAlpha / opAlpha / write mask (D3D11 enum values) |
| `depth` | enable / depth write / comparison / stencil |
| `markers` | `tone`, `upscale` (the upscale boundary came just before this draw), `ui1` (the frame's first UI draw) |

The **pointer** is the `ID3D11Resource` behind the view, or the `ID3D11Buffer` itself, in lowercase hex with no `0x` and no
padding (`0` for none). The same texture shows the same text in a target column and in the slot of a later draw that reads it, so
a target and its readers can be joined without a symbol table. The size of a texture entry is `width x height` of subresource 0;
a slot the shader declares no name for has `-` as its name.

A texture's reflected name carries the effect compiler's suffix: the texture half of `g_SamplerShadow` is `g_SamplerShadow.T`, its
sampler half `g_SamplerShadow.S`. Constant buffers keep their plain name.

### Frame lines

`# frame <n> draws <d> tone <ordinal> ui1 <ordinal> upscale_after <ordinal>` closes each frame, with `omitted <k>` appended when
the per-frame row cap dropped rows. Because the tone map closes the window, it is the frame's **last** row: a window runs from just
after one tone map to just after the next, so it holds one frame's world (geometry, the shadow mask writers, the sun light passes,
sky, fog, post) and none of the next frame's.

### Trailer

`# rows <n> frames <m> omitted <k>` is the last line. A reader compares it with the rows it parsed: a copy that was cut off by a
failed write or a full disk ends without the line or with a different count. The counts are what the file holds: a frame the byte cap
refused is not counted, because its frame line is not in the file either. `cb.csv` and `cbdump.csv` end with `# rows <n>`.

A file that hits its byte cap (32 MiB for `draws.csv`) ends with `# TRUNCATED: byte cap of this file reached, later rows were
dropped` before the trailer.

## cb.csv

Unchanged by format 3 in its first columns: one row per read of `g_LightParam` (position, direction, diffuse colour at offsets 0x00,
0x10, 0x20), the `g_CameraParameter` view matrix of the same draw (`view_00`..`view_33`, the buffer's first matrix at `+0x000`) and, since the
view-projection settled the sun's space, that buffer's second matrix (`vp_00`..`vp_33`, at `+0x060`) - at most once a second while the
diagnostic is armed. `frame` is `0` for a read taken while it waits for its capture. A read whose camera buffer is missing or too short leaves
the two matrices' 32 columns as `-`.

The row is taken at one draw only: the light pass that writes the frame's two `R11G11B10_FLOAT` light buffers without blending, which is
the draw whose `g_LightParam` holds the directional light's own parameters. The wetness volumes and every local light declare the same
constant buffer and are passed over, so a row's direction, colour, view and view-projection always belong to the sun's own light; the second
spent on a second belongs to the first such draw that comes after it. The view-projection is what the light shafts module projects that direction
through (`clip = viewProjection x (direction, 0)`), so a capture separates "the direction was view space after all" from "the matrix at
`+0x060` is not the view-projection".

## cbdump.csv (new in format 3)

One row per (sample, named buffer) while a capture runs:

| Column | What |
|---|---|
| `frame`, `time_s` | the captured frame, and seconds since the config load |
| `ps_crc` | the pixel shader that bound the buffer (`-` when its reflection was not recorded) |
| `name`, `slot`, `bytes` | the reflected buffer name, its slot and its own byte size |
| `ptr` | the buffer's pointer, the same text `draws.csv` would carry |
| `hex` | the first `min(bytes, 16384)` bytes, lowercase hex, no separators |

The list is fixed: `g_WetnessParameter`, `g_AmbientParamArray`, `g_DirectionalShadowParameter`, `g_ReflectionParameter`,
`g_BGAmbientParameter`, `g_CameraParameter` (the view, projection and previous-frame matrices, 944 B: what an offline run of a
frame's pass rebuilds its inputs from), `cBrightPassParam`, `cMergeWeight`, `cGlareParam`, `cToneMapParam`, `cCommonTexParam`,
`cFilterForDark`, `cDarkFilterParam`, `cFogParam`, `cHaloParam`, `cSunParam` and `cRadialBlurParam` (the engine's flare family: the
ring's UV, radius and colour, the sun sprite's falloffs and colour, and each radial-blur pass's zoom centre and step, none of which any
earlier capture could sample). Each name is read at most once a second and only while the capture collects
(never while the diagnostic merely waits). The file is capped at 4 MiB per capture: it is cleared when the next capture of the same
session starts.

Reading `g_WetnessParameter`'s 32 bytes against `ptr` is what answers whether the engine binds one shared wetness buffer per
frame, and whether one of its eight floats moves with the weather.

## views.txt (new in format 3)

Written once per capture, at the first draw that binds each of `g_SamplerReflectionArray`, `g_SamplerReflectionMap`,
`g_SamplerHierarchicalZ` and `g_SamplerShadow`:

`# FFXIV draw diagnostics, views, format 3`, then
`name,ordinal,view_dimension,view_mips,view_format,array_size,cubes,width,height,texture_array,texture_mips,texture_format,pointer`.

`view_dimension` is the `D3D11_SRV_DIMENSION` name (`TEXTURE2D`, `TEXTURE2DARRAY`, `TEXTURECUBE`, `TEXTURECUBEARRAY`, ...),
`array_size` is the view's array size in slices (`NumCubes * 6` for a cube array), `cubes` the cube count (`0` otherwise), and the
second group describes the texture behind the view. A 256x256 array and a 1x1 dummy look different here at once.

## dump\ (new in format 3, `DrawDiagnosticsDump = frame`)

`OptiScaler\draw-diag\<run>\dump\` holds the targets of one captured frame, as raw bytes:

| File | What |
|---|---|
| `wetness.bin` | the target of the first draw whose pixel shader declares `g_WetnessParameter`, `g_SamplerGBuffer3` and `g_SamplerViewPosition` and writes one target |
| `gbuffer0.bin` .. `gbuffer4.bin` | the five targets of the frame's first five-target draw, in slot order |
| `light_diffuse.bin`, `light_specular.bin` | the textures the first draws that read `g_SamplerLightDiffuse` / `g_SamplerLightSpecular` bind |
| `ssr.bin` | the target of the first draw whose pixel shader binds `g_SamplerHierarchicalZ` |
| `scene.bin` | t0 of the tone map draw |
| `vposition.bin` | the texture the first draw that samples the view-position buffer binds: the view-space position of every pixel, the reflections pass's ray origin. The water shaders name it `g_SamplerVPosition` and the composite and wetness passes `g_SamplerViewPosition`; either name identifies it |
| `tonemap.bin` | the tone map's first target |
| `copyps.bin` | the target of the first draw after the tone map that reads one FP16 texture and writes a different FP16 target of the same size as `scene.bin` |
| `facts.txt` | one line per file and one per skip |

`facts.txt` starts with the format line and a header, then `file,format,size,bytes,row_pitch,draw_ordinal,pointer` per written file.
`row_pitch` is the pitch the readback had (the `.bin` itself is packed: `bytes` rows of `width * bytes_per_pixel`, no padding), and
`draw_ordinal` is the draw that identified the resource. A texture the dump could not take has a `# skipped <file>: <reason>` line
instead (multisampled, a format it cannot write, over the dump's bound, `displaced by <name>` when a texture of a higher priority took
its bytes, `not sampled` for a G-buffer target no draw sampled, a failed copy or map). The lines follow the priority order, skips
included, so `facts.txt` reads the same way as the file list.

The readbacks are staged after the draw that identified a resource, never at it, because the hook runs before the draw; the five G-buffer
targets are taken one step later instead, at the first later draw that samples one of them, because their pass is not over when the
five-target draw returns. "Later" is the capture's own draw counter, which a frame close does not restart, so a target identified at a
window's last draw is still copied in the next window. Maps happen at the frame end, subresource 0 only, and at most 24 files and the
configured byte budget are taken (256 MiB by default; `[FFXIV] DrawDiagnosticsDumpBudgetMiB` sets it, 64 to 4096). At a 4K frame's
size the whole set is about 514 MiB with the view position (451 MiB without it), and about 546 MiB when the SSR target is written too, so a
session that wants all of it sets 1024 MiB. The priority order is
wetness target, G-buffer, light buffers, SSR target, scene, view position, then the rest, and it is the order a
texture of higher priority takes a place back in: a newcomer that does not fit releases the held textures of a lower priority, lowest
first, so the wetness target is never displaced by the G-buffer and the light buffers merely because they were identified first. A set
of equal or higher priorities releases nothing (the first of two equal ones keeps its place) and the newcomer is skipped as over the
bound. The same bound is enforced when a texture is identified, so the staging copies stay inside it, and a staging copy that fails gives
its bytes back to it. The dump is written once the frame that identified its tone map is over, with the game's context: either a later
frame ended, or that frame itself ended at a bridge Present, whose last draw has already run - so a two-frame capture in a session with a
bridge is written at the Present that closes the second frame, and the scene and tone-map textures identified at that frame's last draw are
copied then. A tone-map split writes the dump from the closing tone map's own draw hook, before that draw, so a texture identified at it
keeps its `not copied` line instead. It needs a capture of at least two frames. A key-started session writes up to four dumps, one per capture (`DrawDiagnosticsKey`), each into its own run folder; each dump is bounded by the byte budget on its own, so mind the disk.

## shaders\ and shaders.txt (kept pixel shaders)

Every capture writes the pixel shaders it kept into `shaders\` (one `<crc>.dxbc` per shader) and lists them in `shaders.txt`:

```
# crc,size,origin,name=bindPoint,...
ADE72072,8808,creation,g_WetnessParameter=4,g_SamplerGBuffer3=5,g_SamplerViewPosition=6
```

Two kinds of shader are kept. `capture` rows are the shaders created while the capture ran whose reflection names one of the resources
the diagnostic identifies draws by (the sun mask, the depth, the light and camera buffers, the view position, the occlusion buffer and
the ambient table); they are bounded at 64 shaders plus 16 for the sun light draw's own shape, and 4 MiB. `creation` rows are the ones
kept the moment the game created them, whatever the capture phase was, while the draw diagnostic key is on: the deferred composite
(`g_AmbientParamArray`), the wetness pass (`g_WetnessParameter` with `g_SamplerGBuffer3` and `g_SamplerViewPosition`), the hierarchical-Z
pass (`g_SamplerHierarchicalZ`), the glare chain (`cGlareParam`, `cBrightPassParam`, `cMergeWeight`, `cGaussianBlurParam`,
`cSoftFocusParam`) and the flare family (`cHaloParam` for the ring, `cSunParam` for the sun sprite and the ghost discs it feeds,
`cRadialBlurParam` for each radial-blur pass's step). Those passes are created while the game loads, before any capture can be armed, so a
capture-only list would never hold them. Their reflected names and slots are kept with the blob, on the same bound (32 shaders and 2 MiB,
first come, and one log line says when it is full), and the CRC is stamped on the shader object then too: the draws of those passes are
therefore named in `draws.csv`, and the buffers they bind are sampled into `cbdump.csv`, exactly like a shader created while a capture ran.
That matters because the session's own table of reflected shaders is bounded too and a long session fills it; the creation list sits outside
that bound, and a shader in it is named from its own creation row even when the table had no room. A blob in both lists is written once, as
`creation`. A format-3 file written before the origin column carries `crc,size,name=bindPoint,...` and is read with every
row as `capture`.

The bytecode is the shader the game handed to `CreatePixelShader`, so a reflection rule (or `fxc.exe /dumpbin`) can be replayed on it
offline.

### Joining a row to its names by CRC

`draws.csv` can only name a draw's resources if the diagnostic recorded that shader's reflection, which it does when the game creates the
shader, and a row whose shader it never saw carries `-` in every slot. The passes the game creates while it loads - the glare chain and the
deferred composite among them - are named from their creation row now, so their rows carry their names and their buffers are sampled. The
join below is still what reads a capture whose rows predate that: `shaders.txt` holds the same shader's blob and names from its `creation`
row, the `ps_crc` column is the same value in both files (`0xXXXXXXXX` in `draws.csv`, `XXXXXXXX` in `shaders.txt`), and a reader that needs
a name for such a row looks the CRC up in `shaders.txt` and takes that row's names, dropping the `=bindPoint` suffix each one carries there.
A row that names a resource of its own is never replaced by the fixture: the row is the measurement, the fixture is the fallback for a row
that has none.
`session_answers.py` does this join for the glare chain (question B2's "which draw binds `cGlareParam`") and says how many of the rows it
named came from `shaders.txt` rather than from the row itself, so a "no draw binds it" answer stays distinguishable from "the rows could not be
named at all".

## engine.txt

Every capture writes `engine.txt` next to the other files: one line per field, describing the engine pass overrides module
(`[FFXIV] EnginePassOverrides`) as it stood when the capture was written.

```
module: on
sun: 1342x480, in the window
armed: yes
identity: no
runs: 12
declined: 3
decline: the ambient buffer is neither 16,384 nor 12,352 bytes = 3
index: 33112 bytecodes, 0 refused
window runs: 2
window declined: 2
window decline: the draw is a character-preview replay = 2
last bound bytes: 16384
last target: 3840x2160
last window: 0.5,0.5,3840x2160
ssr module: on
ssr armed: yes
ssr runs: 6
ssr readers: 18
ssr declined: 0
ssr passed on: the draw is not a reflection reader = 40211
ssr passed on: the bound shader carries no reflection stamp: its bytecode was never read = 18
ssr window runs: 1
ssr window readers: 18
ssr window declined: 0
ssr window passed on: the draw is not a reflection reader = 4662
ssr window passed on: the bound shader carries no reflection stamp: its bytecode was never read = 18
```

`sun:` is the frame's sun, projected by the diagnostic itself from the light pass it last read: the unit direction `g_LightParam` carries,
through the view-projection the same draw's `g_CameraParameter` carries, into that draw's window - with whether it fell inside the window
(`sun: 1342x480, in the window`, `sun: 1479.7x-259.7, 0.24 windows outside the window`, `sun: behind the camera`, or `sun: -` when no light
pass has been read or its direction is not a unit vector). The engine's own flare (the halo ring, the sun sprite and the ghost discs) is drawn
around the sun's screen UV, so a reader knows at once whether an F9 could have it in frame. The line is written whatever this block's key says,
because it is a fact about the game's frame and is computed without any other module's card: the light shafts card being off (the state the
engine's flare is captured in) does not remove it. The read is the diagnostic's once-a-second sample of the light pass; arming a capture clears
that clock, so the capture's first light pass reads the frame in front of it, and the frames of the capture that fall inside the same second carry
that read.

`module: off` is the whole engine block when the key is off (the `ssr` block below still follows it, since the reflections have their own master). Otherwise `armed` says whether a draw can be patched now, `identity` whether the
knob is at 1.0, `runs` counts the composite draws the module patched, `declined` the ones it left alone, and one `decline:` line names
each refusal reason that happened with its count. `index:` is the shader index: how many bytecodes it stores and how many new ones its
cap refused (a refused bytecode is not indexed and its draws are left alone, with one line in the log). The three `last` lines are the
latest patched draw's facts: the ambient buffer's byte width, the composite's render target and that draw's viewport.

The `window` lines are the same counts for this capture's own frames: the difference from a baseline the diagnostic takes when the
capture arms, before it collects its first draw. A session keeps up to eight captures and the module's state lives for the whole
session, so the plain counts above answer for everything up to this capture and the `window` lines answer for the frames the capture
itself holds - which is the question "did the module run where the knob showed no change". A second capture that declined every draw
of its own window reads `window runs: 0` even though `runs` is not zero. A file without `window` lines predates them, and a reader then
cannot answer for one capture's window.

The `ssr` block after them is the screen-space reflections module (`[FFXIV] ScreenSpaceReflections`), which answers for draws the engine
pass overrides never patch, so it has counters of its own. `ssr module: on` is the card; `ssr armed:` whether a water draw can run now
(the card on, a blend off zero, no switch-off); `ssr runs:` the passes it ran; `ssr readers:` the water draws that bound its texture;
`ssr declined:` the water draws it refused and left alone; `ssr passed on:` one line per reason it passed a draw on for (a draw it does
not answer for - not a refusal, so it never counts towards the switch-off; the reason "the draw is not a reflection reader" is the catch-all for every other draw of the seam and is large by nature: the readers set it apart, and the gate that stopped a water draw is the largest of the OTHER reasons). `ssr window` lines are the same counts for this capture's own
frames, measured from the same baseline as the engine block's. `ssr module: off` is the whole block when the card is off. Which gate
stopped the water draws is therefore readable from one capture at the water, without the log. `ssr last water draw:` is the stamp and the view
of the last water draw the module passed on after reading its reflection slot (`stamp 0x..., map slot N, read slot N, view ..., array ...,
format ..., WxH, texture ...`), or `-` when this capture's window passed no water draw on: the module keeps one read for the session, so the
line is gated by that reason's own `ssr window passed on:` count and a second capture of one session never prints the first one's slot.

## What a format 2 folder still looks like

Identical except that no entry has its trailing pointer, `cb` entries have `slot:name:bytes`, `views.txt`, `cbdump.csv`,
`dump\`, `shaders.txt` and `engine.txt` do not exist, and there is no trailer. A reader of these files reads both formats and warns
when a trailer is missing or disagrees with the rows it parsed.
