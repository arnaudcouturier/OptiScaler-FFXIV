# Engine pass overrides: the constant-buffer patch primitive, and how to add a knob

Companion to the [engine pass overrides](../FEATURES.md#engine-pass-overrides-ffxiv-experimental) entry. This note is about the machinery: how
a draw is recognised, why the GPU path needs three buffers, what the module refuses and how to add the next knob without writing a second
mechanism.

**Status: experimental.** The switch is off by default, and neither knob's visible effect has been judged in game yet. The tests say the
primitive scales exactly the bytes it is told to, replaces exactly the components it is told to and puts every binding back; they say nothing
about how the picture looks.

## What it does

The engine hands its own parameters to its own passes in constant buffers whose names are in the shader's bytecode (`g_AmbientParamArray`,
`cBrightPassParam`, ...). For one draw of its own choosing, the module copies such a buffer, applies a small table to the float4s it addresses,
binds the patched copy for that draw and puts the game's buffer back afterwards. Nothing of the game is written, no address of the executable
is used, and no shader of the game is replaced.

The first knob scales the per-probe fields the deferred composite multiplies its ambient (sky) light with, in the engine's ambient probe
table. The layout comes from the shader's own reflection: a 64-byte header followed by 64 probes of 192 bytes. Two fields of each probe carry
the ambient's level, and the knob writes both in one patch:

- `skyVisibilityScale`, probe offset `0xB0`, at byte `0x40 + 0xB0 = 240`, one every 192 bytes, 64 of them. The multiplier is
  `{scale, 1, 1, 1}`, so the `ssaoMask` in the next component is untouched. This is the engine's own field, but the composite reads it only
  inside its probe loop (`index < paramNum - 1`) and only for a probe whose `(1 - bakeLightRate) * skyVisibility` is below 0.5: a table with
  one probe never enters the loop, and an outdoor probe with a high sky visibility skips the field entirely.
- `shRecipScale`, the `.w` of the float4 at probe offset `0x30` (`color.xyz` in `.x/.y/.z`), at byte `0x40 + 0x30 = 112`. The multiplier is
  `{1, 1, 1, scale}`: the probe's colour keeps its bits, and the field the composite applies to every probe it ends up using is scaled. That
  includes the default probe a one-probe table falls back to, and the shader reads it on the ambient and again on the indirect specular. It
  ends at `112 + 63 * 192 + 16 = 12,224`, inside the declared table.

Both entries fit the declared 12,352 bytes (`240 + 63 * 192 + 16` ends exactly on it), the same compute dispatch scales both, and the knob
writes both or neither: 0xB0 alone leaves the knob silent on a one-probe table and on any pixel a high-sky-visibility probe serves, and 0x3C alone
would drop the engine's own per-probe meaning. The cost of the second field is stated where the knob is documented: it dims the probe's share of
the indirect specular reflection as well as its share of the diffuse ambient.

The second knob forces the engine's wetness. `g_WetnessParameter` is a 32-byte constant buffer (8 floats, no member names in the reflection) the
game binds at the pixel-shader slot each shader declares - 4 on almost every material draw, 5 and 1 on a few - for about 1,500 draws a frame,
and the slot always comes from the reflection, never from a constant. Its first float is the wetness amount (1.0 in rain, 0.0 dry, ramping
through 0.85 and 0.46 while the weather changes) and its second is the engine's own dryness blend, `1 - f0/2`. The patch replaces exactly those
two components for the draws that bind the buffer; the angles and tuning constants after them (`.zw`, and the whole second float4) keep the bits
the game wrote. The engine's rain-exposure mask is untouched: it is a separate full-screen pass, and it still decides where the ground is
sheltered, so the gloss appears on an open stone plaza and not under a canopy. Grass has no gloss response, which is why the visible effect has
to be judged on stone.

## Why three buffers

D3D11 forbids `D3D11_BIND_CONSTANT_BUFFER` on a buffer that carries any other bind flag, and a compute shader cannot write a constant buffer.
So the buffer the dispatch writes can never be the one the draw reads:

1. `src` (owned, `BIND_CONSTANT_BUFFER`, the game's size) is filled with `CopyResource(game → src)`;
2. `work` (owned, `BIND_UNORDERED_ACCESS`, `MISC_BUFFER_ALLOW_RAW_VIEWS`, a raw `R32_TYPELESS` view, byte width a multiple of 4) is the
   dispatch's output: the shader binds `src` as its own constant buffer at `b0` (up to 16,384 bytes, under the 64 KB limit), reads every
   float4 as raw bits and writes it, converting only a component whose multiplier is not exactly 1, so a denormal, an integer-as-float or a NaN
   payload beside the knob never passes through a float conversion;
3. `dst` (owned, `BIND_CONSTANT_BUFFER`) is filled with `CopyResource(work → dst)` and is what the draw binds.

The buffers are created once per byte size (the ambient table is bound at 16,384 and 12,352 bytes, the wetness buffer at 32; both ambient sizes
are accepted) and released when the device goes away. Two copies and one dispatch are far under a tenth of a millisecond, measured with a
timestamp pair that brackets both copies and the dispatch, and is read a run or two later and never waited for. One thread per float4: the
dispatch launches exactly the groups the byte count needs.

Two tables address the float4s of the copy. A **scale entry** carries one multiplier per component, and a component whose multiplier is exactly 1
keeps its bits. A **set entry** carries a value, a component mask and the same address triple: a masked component is REPLACED with the value's
bits, an unmasked one keeps the bits the game wrote. The value is a bit pattern computed on the CPU, not a float, so a replacement passes through
no conversion either; that is how the wetness knob writes `.x = w` and `.y = 1 - w/2` and leaves a denormal or a NaN payload beside them alone.

The dispatch saves and restores the compute stage it touches: the shader with its class instances, its two constant-buffer slots and the one
unordered-access slot. The restore is a scope guard, so a call that throws cannot leave the game in the module's state.

## The CPU-written mode

Some knobs are a handful of floats the caller already knows (the engine's wetness is 32 bytes). For those, the other mode keeps one owned
`D3D11_USAGE_DYNAMIC` buffer per (name, size), written with `Map(WRITE_DISCARD)` **at most once per frame** from the caller's values and
swapped in per draw. The caller passes whether its target equals the value it last read, and a frame whose target equals the engine's value
swaps nothing at all, so a dry frame costs nothing. The read path belongs to the caller; the primitive only owns the write, the swap and the
restore.

## The wetness patch's per-frame cache

The game binds ONE `g_WetnessParameter` buffer for every material draw of a frame, and the same object in every frame, so the wetness knob does
not dispatch per draw. It runs the set patch once per (frame, buffer pointer) and every later draw of that frame that binds the same pointer gets
the cached copy bound: per draw only the slot swap and its restore remain, and the CPU cost added to a material draw is the slot read, the
`GetDesc` of the 32-byte buffer, the swap, its restore, and one relaxed load of the frame token.

- **The frame token** is the output-frame counter of `ext/present` (`FrameCount()`, one relaxed load per candidate draw): the plain wrapped
  swapchain ticks it at every present from its `PacingScope` (`Phase::OutputPresent`) and the bridge ticks it at every present from
  `PresentScope`, so the token moves in every session - with frame generation on or off, HDR or SDR - and both paths present in some
  sessions, which only adds an advance. The module keeps no frame counter of its own.
- **What the cache keeps** is one entry (buffer pointer, frame, patched buffer). It is reset when the frame token changes, when a knob moves (the
  config publish), when the master switch changes state, when the module stands itself down, when the patch primitive frees its buffers (a
  device change) and when the device objects are released. The knob and the master publish an epoch instead of writing the entry, so only the
  draw thread ever writes the cache.
- **A second buffer in one frame** is a miss: the patch runs again, up to four patches per frame. The fifth is passed through and counted as
  `the frame bound more wetness buffers than the per-frame cache patches`, so one frame can never dispatch without a bound.
- **A frame token that does not advance** would freeze the first frame's value for the session, so the entry also expires after 4,096 binds and
  the patch budget starts over. The cost of a dead token is one dispatch every few thousand material draws, not a stale value.

A draw whose buffer under that name is not the engine's own 32 bytes, or a character-preview replay of one, is passed on without a refusal
counted: the name sits on about 1,500 shaders, and the wetness knob has no render-target, size or viewport rule at all. A material draw writes the
five-target G-buffer and a shadow-atlas draw writes no colour, and a wetness amount cannot change a draw that writes no colour, so both carry the
patch; the query that would tell them apart would cost every material draw more than the swap it saves.

## How a draw is recognised

1. **Shader creation** (whatever the key says): a byte scan for the names of the table in `EnginePassRules.h` drops every shader that cannot
   be one of these draws; for the few that name one, the DXBC reflection is parsed once, the slot of each name and the shader's declared
   output count are read, and a shader whose layout carries an anchor bit for a knob is stored in the index and stamped with the index's id.
   A shader that names a buffer but carries no anchor bit is not stored ("The index" below).
2. **Pixel-shader bind**: the seam records one 64-bit value for the bound shader - the anchors word below - so a draw costs one load and a
   bit test instead of a `PSGetShader` (0.65 ms a frame in the contact shadow measurement) and takes no lock. Every arm and disarm
   transition clears that value: a slider moved from 1.0 to 0.7, or a re-arm after the module stood down, waits for the next bind rather than
   patching a draw with a value from another state.
3. **The draw**: the composite is patched only when it declares both of its anchors and the runtime validation passes - the ambient buffer bound
   at the slot the shader declares, its size one of the two the engine binds, exactly one render target at least a scene in size, a render window
   that fits inside it, and no character-preview replay. A draw that declares `g_WetnessParameter` is patched when the buffer at its reflected
   slot is exactly 32 bytes and the draw is not a replay. Anything else draws the game's original call.

### The anchors word

One 64-bit word per indexed shader carries everything the draw path needs to know about it: which knobs' draws the shader is, and at which
slot each of its anchors sits. Each knob owns a flag bit and an 8-bit slot field of its own. The map is fixed in `EnginePassRules.h` and
asserted at compile time - no two fields share a bit, every slot field is 8 bits - because a collision is not a refusal: it would classify
one knob's draw as another knob's and patch it at the wrong slot.

| bits | owner | meaning |
|---|---|---|
| 0..7 | ambient | the composite's `g_AmbientParamArray` slot |
| 8..15 | wetness | the `g_WetnessParameter` slot |
| 16..29 | - | free in the shipped word, never handed to a knob |
| 30 | wetness | the shader declares `g_WetnessParameter` |
| 31 | ambient | the shader declares both anchors of the composite |
| 32 | ambient occlusion | the shader reads the engine's occlusion texture |
| 33..40 | ambient occlusion | that anchor's slot |
| 41 | light shafts | the light pass |
| 42 | light shafts | the composite |
| 43 | light shafts | the tone map |
| 44..51 | light shafts | the slot those passes read |
| 52 | puddles | the shader reads one of the engine's G-buffer targets (the exact slots travel in the pass's own stamp) |
| 53..60 | - | free: the G-buffer reader's slots travel in the pass's own stamp, not in this word |
| 61 | screen-space reflections | the shader reads the engine's planar reflection map |
| 62..63 | - | free |

The two shipped knobs keep the values they have always had - bit 31 with the ambient slot in bits 0..7, bit 30 with the wetness slot in bits
8..15 - so every log line, ini key and check that names them still means the same thing. The ambient occlusion pass sets its reader bit and
slot, the light shafts module sets its three bits and its slot field, the puddles pass sets its reader bit, and the screen-space reflections pass
sets its own bit, when each recognises a shader's layout (the occlusion texture's reader, the light pass, the composite, the tone map, a G-buffer
reader, a sampler of the planar reflection map). Each of those passes stamps the slots it needs on the shader object itself, so the word carries
only their flag bits.

### The index

The bind path asks the module's own table, not the context: `PSGetShader` on every draw measured 0.65 ms a frame in the contact-shadow capture,
while the seam's packed value costs one load. The table is what answers the bind.

- **What is stored.** Only a shader whose bytecode names one of `kNames` *and* whose layout carries an anchor bit for a knob: the composite
  (both anchors), a shader that declares `g_WetnessParameter` at a slot D3D11 can bind, a shader that reads the engine's occlusion texture
  (`g_SamplerOcclusion`, the ambient occlusion pass's reader) at a slot D3D11 exposes, the light shafts' three passes - a light draw that
  declares `g_LightParam`, the deferred composite and the tone map - a shader that declares one of the four G-buffer targets (`g_SamplerGBuffer1..4`, the puddles pass's readers),
  and a sampler of the engine's planar reflection map (`g_SamplerReflectionMap`, the screen-space reflections pass's reader). A material pass
  that merely names a buffer without a shape the knobs address takes no slot and no id, so the engine's common parameter names cannot fill the
  table and leave a patchable shader without its record.
  `g_WetnessParameter` alone is declared by about 1,585 pixel shaders of a live session; the composite and the other named passes are a handful.
- **How it is stored.** Open addressing with linear probing over 131,072 slots: a pointer, the anchors and the layout's id per slot, 24
  bytes, so the table is 3 MiB, allocated on first use and never freed (it is the module's session state). The slot count is twice the
  40,960-bytecode cap on the index, because linear probing at the cap in half these slots leaves about 1.5% of the inserts without a free slot
  at a probe of 8, and 0.2% even at 16. At the cap the table is 0.31 full. A bind walks at most 16 slots: one hash, a short probe, a pointer
  compare and, on a hit, the stamp the object itself carries. Insertions happen at shader creation only, serialised by the index's one lock; the
  bind takes no lock.
- **Why the cap is 40,960.** The index keeps the first 40,960 bytecodes it is asked to store and refuses every later one. A session creates
  about 33,000 pixel shaders, and a late one is exactly the kind that matters: a water material, or the composite of a zone entered after
  twenty minutes. A bytecode created past the cap is counted and its refusal is written once, with its own line ("the shader index is at its
  cap (...), so a bytecode created from here on is not indexed and its draws are left alone"); a capture shows the count on the `index:` line of
  `engine.txt` (bytecodes stored, bytecodes refused) and the card adds ", the shader index refused N bytecode(s) at its cap". Its draws are
  passed on like any other unindexed shader's, so the engine picture is unchanged.
- **A slot's three fields are published in one order**, because a bind reads them with no lock while shader creation replaces a record: the
  anchors first, then the id, then - for a slot that was empty - the pointer. A find loads the pointer, then the id, then the anchors. A bind
  that races a re-index can therefore see at worst the previous id with the new anchors, which the stamp check refuses; it can never see a new
  id with the previous anchors, which is the pair that would classify a draw by another layout's slots.
- **A recycled address.** The runtime can hand a released shader object's address to another shader. The record's id must equal the id
  `SetPrivateData` stored on the object now, so an old record answers "not mine" instead of classifying a draw by another layout's slots. A
  shader that is indexed again replaces the record in place, so one address never takes two slots.
- **A full table** is the one failure the table cannot avoid: if all 16 slots of a shader's probe are taken, the shader is not stored, its
  draws are passed on exactly like a shader the index never saw (the game's own call, no refusal counted), and the module writes one line about
  it for the session. With candidates only, a session uses about 1,600 of the 131,072 slots, and at the index's own cap the bounded probe refuses
  at most one insert on a dense address pattern - the same fail-open, not a wrong classification.

A draw whose shader declares none of those names - most of a frame's draws - is passed on at the bit test: no state query, no patch, and **no
refusal counted**. That is what keeps the module from standing itself down in the first frame. The same goes for an indexed draw that belongs to
the other knob: with the ambient knob at 1.0 the composite draws are not refusals of the wetness knob, and with the wetness knob off the material
draws are not refusals of the ambient one. Each knob keeps its own sixty-four-refusal streak, and a draw a knob never asked for is neither its
run nor its refusal: a build whose composite changed shape stands down even while 1,500 material draws a frame keep working.

Names, not hashes: a shader CRC changes with a patch or a recompile, while the parameter names are the engine's own ABI. The one hash in this
module is the shader index's key, and it only saves parsing the same bytecode twice.

## The drop list, and what it costs when nothing runs

A draw whose shader does not declare both anchors is not a refusal: it is another draw of the frame, and it is passed on at one integer load
before anything is measured or counted. For the composite draws that are looked at, every refusal is one log line the first time its reason
appears, and it leaves the draw exactly as it was. A knob at 1.0 is not a refusal either: it switches the feature bit off, so the seam is one
relaxed load per draw and one per pixel-shader bind, and nothing else. After sixty-four of one knob's own draws in a row are refused the module
stands itself down for the session with one line, so a build whose composite changed shape costs one line instead of a check on every draw.

| reason | what it means |
|---|---|
| `nothing is bound at the ambient slot` | the draw declares the table and the composite's light, but no buffer is bound there |
| `neither 16,384 nor 12,352 bytes` | the buffer under that name is another one |
| `not exactly one render target` / `not a scene` | a different draw of the same shader family |
| `the render window does not fit` | not a full-target pass (a dynamic-resolution sub-rectangle still fits) |
| `character-preview replay` | the preview draws the same shader families as the world |
| `the frame bound more wetness buffers than the per-frame cache patches` | a frame bound a fifth distinct wetness buffer; the draw ran unchanged |
| `the owned buffers or the compute scale could not run` | allocation or dispatch failure |
| `a call threw` | a D3D call in the patch path threw; the game's draw ran unchanged |

A wetness draw that binds another buffer under that name (or writes no colour) is not a refusal and does not appear in this list.

## What a capture records

The module exports its counters instead of leaving them to a log: `CurrentDiagnostics()` (`EnginePass.h`) returns the key, the status the
card shows, one count per decline reason and the facts of the last draw it patched (the ambient buffer's byte size, the composite's render
target and that draw's viewport). Every refusal increments its own reason's count at the same place `declined` is incremented, so the
per-reason table cannot drift from the total. The draw diagnostic writes the result to `engine.txt` in each capture folder, one line per
field, and `module: off` is the whole file while the key is off. A session therefore says whether the module patched a composite draw and,
when it did not, which reason it declined for, without a log file and without the menu.

The counts are the session's, so the diagnostic also writes the part that belongs to one capture: it reads the counters when a capture arms
(before its first draw) and writes the difference as the `window` lines beside the session's. A second capture of one session then answers
for its own frames, which is the question a knob that showed no change asks ("did the module patch a draw here") - the session counters
alone cannot, because they never go back to zero.

## Adding the next knob

1. Add the name (and whether it is a buffer or a texture anchor) to `kNames` in `EnginePassRules.h`, its flag bit and its 8-bit slot field to
   the anchors map there (the compile-time check then proves it collides with nothing), and a byte-offset/stride/count triple for it where the
   reflection says the value sits.
2. Add the value to `Params`, clamp it, and add the rule that says the default is today's picture (an identity test, or "off means no
   entries"). `AnyKnobOn` is the feature-bit rule: the master switch alone changes nothing.
3. Write `AmbientEntries`'s sibling: the entry table the GPU path applies, with `EntryFits` (scale) or `SetEntryFits` (set) checked against the
   buffer size. One draw may carry both tables.
4. Add the key, its ini fragment line and its settings-schema entry, and a control on the module's card that publishes on change.
5. Add a branch to `ApplyDraw` if the draw shape differs from the composite's, and a pure candidate rule if the knob must skip draws.
6. Test it three ways: a pure test of the entry table and the validation, a WARP test that reads the patched buffer back (one entry, known
   float4s, everything else bit-identical), and a text pin for the key and the control. A knob whose patch runs once per frame also needs the
   cache's transitions tested: same frame, new frame, a second pointer, the cached bind and the expire.

## What is not known

- Whether either knob's visible difference is the one the reference screenshots show. The ambient acceptance test is two screenshots at the same
  camera at night, slider 1.0 against 0.7, in a game session; the benchmark has no night scene and its run-to-run variation swamps an effect
  this small.
- Whether the wetness knob's forced value reaches the stone as gloss. The buffer was measured, but the knob has not been tried in game: the check
  is a dry stone plaza by day, the slider at 1.00 against -1.00, two screenshots at the same camera. Weather forced with a client-side plugin
  works as a control, because the patch replaces the weather's ramp while it is on.
- Whether the frame token holds still for a whole game frame in every session. The present count is the only boundary this path can see without
  a new hook; the 4,096-bind expire is the fail-safe for a session where it does not run, and the per-frame patch bound is the fail-safe for a
  frame that binds a second buffer.
- Whether `skyVisibilityScale` and `shRecipScale` are ~1.0 in the shipped build. The layout says they are multipliers the engine applies; a capture
  of the buffer's bytes shows what the game wrote (the dump is taken before the module swaps its patched copy in, so it can never show the patch
  itself), and the pass reads them only on the GPU, never on the CPU. A table whose header says one probe (`paramNum 1`) is a spot where the first
  field cannot show at all and only the second one can.
- The other two names in the table (`cCommonTexParam`, `cBrightPassParam`) are indexed and have no knob yet: their byte offsets are the next
  measurement, not a guess to ship.
