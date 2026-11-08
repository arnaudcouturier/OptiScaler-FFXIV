# Draw-diagnostic analysis tools

Readers and reports for the files the developer draw diagnostic writes (`[FFXIV] DrawDiagnostics = capture`, described in
[`docs/FEATURES.md`](../../docs/FEATURES.md)). They turn a capture folder into the numbers a lighting, reflection or
weather investigation needs, without a build, a device or an image library.

Everything here is Python 3.14, run with `python -I` (the scripts insert their own folder on `sys.path`, because isolated
mode drops it). Only `numpy` is used beyond the standard library; the PNG writer is part of `dump_to_png.py`.

## The files a capture folder holds

The diagnostic writes one folder per capture, `<date-time>` (and `<date-time>-<n>` for a second capture of the same
session, because a folder name has one-second resolution):

| file | content |
|---|---|
| `draws.csv` | one row per draw of each captured frame, then a `# frame` line per window and a `# rows` trailer |
| `cb.csv` | the sun/camera constant buffer of one draw, read back once a second |
| `cbdump.csv` | the bytes of the named constant buffers (wetness, the ambient probe array, the shadow/reflection/glare/post buffers), sampled once a second while the capture runs |
| `views.txt` | the shader-resource description of a few identifying textures (the reflection array and map, the hierarchical-Z, the shadow atlas) |
| `shaders.txt` | the kept pixel shaders' bytecode (`shaders\<crc>.dxbc`) with what each one declares, and the origin it was kept at |
| `engine.txt` | the engine pass overrides' counters: the module's runs and declines by reason (session and this capture's own window), and the last patched draw's facts |
| `dump\` | when `[FFXIV] DrawDiagnosticsDump = frame`: raw `.bin` textures, one per file, plus `facts.txt` |

### `draws.csv`

```
# FFXIV draw diagnostics, format 3
frame,ordinal,api,context,count,instances,start,base,stage,ps_crc,srv,cb,rtv,dsv,viewport,blend,depth,markers
1,1,DrawIndexed,game,990,1,0,0,0,0x98CB1685,0:g_SamplerColorMap0.T:BC7_UNORM:512x512:1a2b|...,0:g_MaterialParameter:512:2c3d,0:B8G8R8A8_UNORM:2560x1440:4e5f,D24_UNORM_S8_UINT:2560x1440:6a7b,1:2560x1440+0.5+0.5,0/2/1/1/2/1/1/0,1/1/2/0,
# frame 1 draws 9214 tone 4620 ui1 5 upscale_after 4622 omitted 3214
# rows 6000 frames 1 omitted 3214
```

* `srv` entry: `slot:name:format:size[:pointer]`. `name` is the shader's variable name with the compiler's type suffix
  (`g_SamplerDepth.T`); `-` means the shader declares no binding for that slot.
* `cb` entry: `slot:name:bytes[:pointer]`.
* `rtv` entry: `slot:format:size[:pointer]`.
* `dsv`: `format:size[:pointer]`, or `-`.
* `viewport`: `count:widthxheight+left+top` and, when the draw set several, `|`-joined rectangles. The leading number is
  how many viewports the draw set, not a slot; the half-texel offsets are floats (`+0.5+8192.5` for the fifth shadow
  slice).
* `markers`: `tone`, `ui1` and/or `upscale`, joined with `+`.
* `stage`: `0` unknown, `1` tone map, `6` first UI draw.
* `# frame` line: the window's own counts (complete even when the row cap dropped rows) and the marker ordinals.
* `# rows <n> frames <m> omitted <k>`: the writer's self-check. A reader compares it with what it parsed; `# TRUNCATED`
  means the file's byte cap was reached.

**Format 2** files carry no pointer column (`srv` has four fields, `cb` three, `rtv` three, `dsv` two) and no trailer.
Both formats are read; a file cut before its trailer is reported as a notice, not accepted silently.

### `cbdump.csv`

```
frame,time_s,ps_crc,name,slot,bytes,ptr,hex
```

`hex` is the first `min(bytes, 16384)` bytes of the buffer, lowercase; `ptr` is the buffer's pointer. A name the layout
table does not know still decodes as a float list.

### `cb.csv`

`frame,time_s,ps_crc,cb,slot,bytes,pos_x..diffuse_b,view_00..view_33,vp_00..vp_33` — the light constant buffer's position (offset
`0x00`), direction (`0x10`) and diffuse colour (`0x20`), plus the same draw's camera view matrix (`0x000`) and view-projection
(`0x060`). The view-projection is the matrix the light shafts module projects the sun's world direction through, so a capture
tells a wrong direction (a `dir_*` triple that does not match the shadow matrix) from a wrong matrix (a `vp_*` block that is the
view, the identity, or whose w row is not about `(0, 0, -1, 0)`).

### `views.txt`

```
name,ordinal,view_dimension,view_mips,view_format,array_size,cubes,width,height,texture_array,texture_mips,texture_format,pointer
```

One line per identifying texture, written by the first draw that binds it: the view's dimension/mips/format and, from the
texture behind it, its size, array size, mip count and format. This is what tells a cube-array probe from a 1x1 dummy.

### `shaders.txt`

```
# crc,size,origin,name=bindPoint,...
ADE72072,8808,creation,g_WetnessParameter=4,g_SamplerGBuffer3=5,g_SamplerViewPosition=6
```

One line per kept pixel shader, and one `<crc>.dxbc` per line under `shaders\`. `origin` says when the diagnostic copied
the bytecode: `creation` is a shader kept the moment the game created it (`g_AmbientParamArray`, the wetness pass, the
hierarchical-Z pass, any glare parameter), which is how a capture holds the composite the game creates while it loads;
`capture` is a shader kept while the capture ran. A format-3 file written before that column carries two columns
(`crc,size,...`) and is read with every row as `capture`.

### `engine.txt`

```
module: on
armed: yes
identity: no
runs: 12
declined: 3
decline: the ambient buffer is neither 16,384 nor 12,352 bytes = 3
window runs: 2
window declined: 2
window decline: the draw is a character-preview replay = 2
last bound bytes: 16384
last target: 3840x2160
last window: 0.5,0.5,3840x2160
```

One line per field. `module: off` is the whole file when the `[FFXIV] EnginePassOverrides` key is off. `runs` counts the
composite draws the module patched, `declined` the ones it left alone, one `decline:` line names each reason that
happened with its count, and the three `last` lines describe the latest patched draw. The `window` lines are the same
counts for this capture's own frames (the difference from when the capture armed), and the EP answer is decided from them:
the module's counters live for the whole session, so a second capture can show session runs while its own window saw none.
A capture written before the file existed has none, and a file without the window lines cannot answer for one capture's
window; the EP answer says which case it is.

### `dump\facts.txt`

```
<name>.bin,<DXGI format>,<width>x<height>,<bytes>,<row_pitch>,<draw_ordinal>,<pointer>
# skipped <name>.bin: <reason>
```

The bytes are the resource's content, written packed; `row_pitch` is the pitch the readback had. An older path of the
same writer put the size in two fields (`<name>.bin,<format>,<width>,<height>,<bytes>,...`); that shape is read too, with
its columns shifted by one.

A 16-bit depth written with four bytes per texel is **refused** with a note, not decoded: the value is 16 bits but the
readback decides which half of the texel holds it, and a preview built from a guess is worse than no preview. A
`_TYPELESS` format is read as the concrete twin of its own width (the view, not the texture, decides the type); a format
the tool has no decoder for is listed with a message and skipped.

## The tools

| tool | what it does |
|---|---|
| `drawdiag.py` | the library: loads a capture folder into typed objects — draws, frame windows, marker ordinals, the trailer check, `cb.csv`, `cbdump.csv`, `views.txt`, `shaders.txt` (with each fixture's origin), `engine.txt`, `dump\facts.txt`. Run it directly on a folder to print what it understood and every notice it raised. |
| `cblayouts.py` | the named constant-buffer layouts as data (wetness, the 64-probe ambient array with its 192 B records, the directional-shadow, reflection and light buffers, the flat post buffers) and `decode(name, bytes)`, which never throws on a short buffer. `python -I cblayouts.py` prints the table. |
| `cbdump_report.py` | `<capture-folder>`: the decoded values of every sampled buffer per sample, the min/max/delta of every member over the samples, the wetness floats with the buffer-pointer question (samples, grouped by frame, and the frame's draws), and the ambient probe table with the "`skyVisibilityScale` ≈ 1.0 in N of M probes" verdict, counted over the probes the header's `paramNum` names. |
| `dump_to_png.py` | `<capture-folder>`: every `dump\*.bin` to a PNG preview (tone-mapped for float colour, exposure flag, per-channel greyscale with `--channels`), plus a raw `.npy` for the float formats. Honours row pitch; pure-Python PNG writer. `--formats` lists what it decodes. |
| `session_answers.py` | `<session-folder>`: runs the fixed question list (below) over every capture folder it finds and writes `session-answers.md` next to them, each item `[O]` (answered, with the evidence row) or `[U]` (with what would answer it). |
| `selftest.py` | the test suite: generates a small session of two captures in a temporary folder with known values, adds one minimal capture per rule (a window the row cap cut, a window with two tone-map rows, a 4K depth prepass next to the atlas, a wetness pass nothing reads), runs every tool on them, then checks a folder of real captures when one is named. Exit code non-zero on any failure. |

Examples:

```
python -I tools\diag\session_answers.py <copied draw-diag folder>
python -I tools\diag\cbdump_report.py    <capture folder> --out report.txt
python -I tools\diag\dump_to_png.py      <capture folder> --channels --exposure 2
python -I tools\diag\selftest.py                          # generated fixtures only
python -I tools\diag\selftest.py --captures D:\captures   # ... and the real captures in that folder
```

`selftest.py --captures DIR` (or `DRAWDIAG_CAPTURES=DIR` in the environment) reads a folder the caller names; there is no
default, and without the argument the real-capture part is skipped with a printed note.

### The question keys

`session-answers.md` names each item with a short key so the same key can be quoted across sessions; the report's header
repeats this table.

| key | question |
|---|---|
| `P1` | the wetness parameter: its 8 floats, their movement, and whether one buffer serves the frame |
| `P1x` | the wetness value compared between captures (dry against rain) |
| `P2` | the fullscreen wetness pass: what it writes, and who reads that target later in the frame |
| `P4` | the reflection chain: which draws bind the hierarchical-Z, the reflection map and the array |
| `P5` | the reflection array: a real array or a 1x1 dummy, and its dimensions |
| `B1` | the frame split: one tone-map row per window, and a window whose rows are all in the file |
| `B2` | the glare chain: are its draws present, and is `cGlareParam` stable across the samples |
| `B3` | the screen-space reflection ray march: is a draw binding the hierarchical-Z present |
| `B4` | the ambient probe table compared between captures (night against day) |
| `B7` | the shadow atlas: depth-only draws over its slices, and the depth view's size |
| `GB` | the G-buffer: the five-target draw's formats and sizes |
| `LB` | the light buffers: the formats and sizes the light draws read and write |
| `EP` | the engine pass overrides: did the module patch a composite draw in this capture's own window, or which reason did it decline for |

`session_answers.py` writes `session-answers.md` into the folder it was given (so copy the captures out of the game's
`OptiScaler\draw-diag\` first, or pass `--out`), and `dump_to_png.py` writes into `<capture>\dump\png\` unless `--out`
says otherwise.

## The one-session workflow

The session is set up so that one launch answers everything the capture can answer:

1. **Before launching**, set in `OptiScaler.ini` under `[FFXIV]` (these four are ini-only and read at start, so a change
   needs a relaunch):
   * `DrawDiagnostics = capture`
   * `DrawDiagnosticsFrames = 3`
   * `DrawDiagnosticsKey = F9`
   * `DrawDiagnosticsDump = frame`
   * `DrawDiagnosticsDumpBudgetMiB = 1024` (a 3840x2160 frame's full set needs about 500 MiB; the default 256 holds the wetness
     target, the five G-buffer targets and one light buffer)
2. **In game**, visit the four places in this order, pressing **F9** at each (one press, then release, per capture; a
   session keeps up to eight):
   1. a wet or rainy exterior with a light source in view (the ambient probes and the wetness value at their wettest),
   2. a candle-lit interior (glare, ambient indoor probes),
   3. the same exterior by day or in a dry zone (the dry end of the wetness comparison; a zone hop also resets the
      engine's state),
   4. water (the reflection chain and its textures).
   Leave `ReflectionType_DX11` at the value the game already uses, and do not change glare or shadow settings during the
   session: the captures are compared against each other.
3. **After exiting**, copy the `OptiScaler\draw-diag\` folder (its `draws.csv`/`cbdump.csv`/`views.txt`/`shaders.txt`/
   `engine.txt`/`dump\`) out of
   the game directory and run:
   ```
   python -I tools\diag\session_answers.py <the copied draw-diag folder>
   ```
4. Read `session-answers.md`. Every `[O]` carries the evidence row it was answered from; every `[U]` says what would
   answer it. Then, when a value matters, look at it directly: `cbdump_report.py` for the buffer bytes and their movement
   over the session, `dump_to_png.py` for the wetness, G-buffer, light and reflection targets of the dumped frame.

The draw diagnostic's own documentation, the ini fragment and the settings table are the authority for the keys and
their defaults.

## Notes on what the numbers can and cannot say

* A capture keeps a part of each frame (the per-frame row cap drops the rest and the frame line counts it) and a few
  frames. "Not in the capture" is therefore not "not in the build": `session_answers.py` says which of the two a missing
  row is.
* The reflected names come from the shader bytecode. A draw whose shader's reflection the diagnostic did not receive
  writes `-` for every resource, and no name test can see it; the reports count those rows so a negative answer is not
  mistaken for proof.
* `srv` sizes are the bound view's, not the whole resource's: an array or a mip chain shows the base size. `views.txt`
  carries the view's dimension and array size for the four textures where it matters.
* The wetness buffer's eight floats have no member names in the shader reflection; the report calls them `f0..f7` in byte
  order.
* The ambient table's `paramNum` is the number of probes the engine fills: the verdict and every range are counted over
  those records, and the rest of the array is padding. A `paramNum` that is not a count between 1 and the number of
  records is printed as it decodes and every record is counted, so an odd header does not hide the values that are there.
* `paramNum`, `envLocationIndex` and `envSpaceType` are read as integers because their names say count/index/type, but the
  shader reflection the table came from does not record their declared type. When the integer reading is past 2^24 the
  same four bytes are shown as a float too (`paramNum_f32`), which is what a float stored in that field looks like.
