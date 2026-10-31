# VFX mask probe: what it is for, how to run it, what a result means

Material moved out of [FEATURES.md](../FEATURES.md#vfx-mask-probe-ffxiv-developer): the reasoning behind the probe, the
measurement protocol and the limits. The feature itself is one ini key and one log line; everything below is how to read them.

**Result (closed).** On the official benchmark client with DLSS 310.9.16 at the title's default preset, a constant half-screen mask changed nothing measurable: the masked half differed from the unmasked baseline by 7.55 against 8.51 mean absolute levels (two pairs), with no change in aliasing and at most 2 % spread across three runs. The gate is closed: the upscaler does not read the mask in that setup, so nothing here builds a mask. The null has no positive control, so it holds for that setup only (see below). Not tried in a played session.

## The question

The upscalers this fork drives can be handed a mask that marks pixels a temporal upscaler should not trust: NVIDIA calls it the
bias current colour mask, AMD's reactive and transparency-and-composition masks are the same idea, Intel calls it the responsive
pixel mask. All three arrive at the same place in the fork: the DirectX 11 bridge copies the NGX bias-mask parameter and hands it to
whichever backend runs (DLSS takes it as the bias mask, FSR 3.x as reactive and optionally transparency, XeSS as responsive).

Two things are unknown, and the fork has to know both before it builds anything that produces a mask:

1. **Does the backend read the mask at all?** NVIDIA's programming guide still documents the buffer but adds that it "has not been
   introduced for use with latest models and should not be used", and names only the older preset F as supporting it. The fork's
   default DLSS preset for a long time has been a transformer preset, and the guide says nothing about the mask for those. The
   runtime still parses the parameter, which is not the same as applying it.
2. **Is there anything to mask?** No public report of DLSS ghosting on this game's spell effects, particles or ground telegraphs was
   found. The mechanism is plausible (particles carry no motion vectors of their own) but unmeasured.

The probe answers question 1 cheaply and in one session. Question 2 needs a separate comparison (see the benchmark section), and a
mask can only be judged against a measured problem.

## What the probe does

`[FFXIV] VfxMaskProbe` (ini only, `off` by default, values `off`, `left`, `right`) replaces the mask input of every upscaler
evaluate with a constant R8_UNORM texture the size of the game's colour input:

- one half of the mask is 255 (masked), the other half is 0 (unmasked);
- `left` masks the left half, `right` the right half, so a single screenshot pair shows the masked and the unmasked half of the
  same scene;
- the texture is created once and refilled only when the render size or the half changes;
- nothing else is touched: no game memory, no shader, no extra pass, no DirectX 12 resource. The bridge copies it like any other
  input it is handed.

The mask is exactly the colour input's allocation size, because that is the size the runtime expects a mask in and the render window
sits at the top-left of that allocation. **The halves are halves of the allocation, not of the visible picture**, and the difference
matters when the game renders below the output size: at DLAA the split is the middle of the picture, at Quality (2560 of 3840) the
masked half covers the left 75% of it, and at Performance (1920 of 3840) the boundary falls on the right edge, so `left` masks the
whole picture and `right` masks none of it. Two consequences for the protocol below: the two-halves-of-one-frame comparison is made at
DLAA, and at a smaller render size the usable comparison is the same region of the picture across an off run and an on run.

Fail open: with the key off the cost is one relaxed load per evaluate, and the game's own value for the key (nothing) reaches the
upscaler exactly as without this feature. A frame the probe cannot build a mask for - no colour input, a size it cannot split, a
device that refuses the texture, a description that is not what was created - writes nothing at all. The probe never clears the
key either.

The log says, once per change of the situation (not once per frame, and never more than 32 lines a session):

```
VFX mask probe: left half masked in a 3840x2160 R8_UNORM mask, given to the DLSS bias-mask input; the parameter block reports the mask back
VFX mask probe: right half masked in a 1920x1080 R8_UNORM mask, given to the FSR bias-mask input; the parameter block does not report the mask back (the key was written, the block did not keep it)
VFX mask probe: nothing written this evaluate (the colour input's size cannot be split into two halves), colour input 3840x0
VFX mask probe: off, the game's own value for the mask input is used again
```

The write call of an NGX parameter block returns nothing, so "the backend accepted it" can only be read back: the key is read again
and the pointer compared. `reports the mask back` means the block kept what it was given. It does not prove the runtime used it -
only the picture can show that.

## The measurement protocol

The rule has to be fixed before the pictures are looked at, because two frames of this game's still scene never match bit for bit
(the jitter phase keeps moving) and the two halves hold different content anyway.

1. **Setup.** SDR output, one resolution, frame generation off, DLSS with the preset that is to be tested (the default is a
   transformer preset). For the masked-half-versus-unmasked-half comparison, DLAA, where the split is the middle of the picture; at a
   smaller render size compare the same region across an off run and an on run instead. A static scene (the game's own group-pose
   mode freezes the world) or one that moves by a script.
2. **Pairs.** At least ten pairs of screenshots, "probe off" against "probe on with a given half", the scene untouched. Wait about
   three seconds after switching so the upscaler's history has converged before the picture is taken.
3. **Measure per half.** For each pair, compute the difference (mean absolute difference of the two images, or NVIDIA FLIP)
   separately inside the masked half and inside the unmasked half.
4. **Decide from the unmasked half first.** The spread of the unmasked-half differences over the pairs is the noise floor; write
   its threshold down before looking at the masked half.
5. **Verdict.** *Read* = the masked-half difference is clearly above that threshold in most pairs, and the masked half looks more
   aliased or shimmery (NVIDIA's own warning about a working mask). *Ignored* = the two halves differ about equally.
6. **Then repeat while the camera moves** and compare edge stability rather than pixels: a working mask is expected to trade trails
   for shimmer within the masked half.

A null result (ignored, on the default preset) only counts with a positive control:

- **An older runtime.** The programming guide's own statement covers the current SDK; older DLSS versions documented the mask as
  normal. Put the fork's own `nvngx_dlss.dll` aside for one session so the game's older library is loaded, with any preset override
  off, and run the same pairs. A masked-half effect there and none on the current runtime is a clean answer: the probes and the
  injection are fine and the current models ignore the buffer.
- **Preset F.** Some runtime builds accept the preset letter and ignore it; read the runtime's own log line about the app hint to
  see which preset actually ran.

If neither control shows a masked-half effect, assume the probe is broken rather than the runtime (check the key, the mask size, the
half, and whether the log line says the mask reached the parameter block) and repeat.

## The benchmark

The Dawntrail benchmark is a separate executable with a scripted camera and combat scenes with spell effects, so it can stand in
for a raid when the game itself cannot be driven that way. Whether the fork's mask reaches the upscaler there is what the log line
says; a benchmark whose game build the fork does not recognise still runs the upscaler and the name- and shader-based features.

The scripted run is identical from run to run in its camera path, which makes "a pair" cheap: two runs of the same segment with the
probe off and with it on. Its screenshots land in the benchmark's own `screenshots\` folder. Compare three things in the frames with
the heaviest effects:

- the masked half against the unmasked half of the same frame,
- the masked half of the "probe on" run against the same half of the "probe off" run,
- the unmasked half across the same two runs (the noise floor of the run pair).

Particle-heavy moments are easy to find by eye; note the timestamp so the same moment is compared on both sides.

## Limits

- The probe says nothing about whether this game's effects ghost under any upscaler; it only says whether a mask is read. Both
  answers are needed before mask generation is worth building.
- A gradient, a graded mask or a generated mask is not tested: the probe is binary and constant, which is what NVIDIA asks for and
  the cheapest thing that can be seen.
- The upscaler is handed a mask the guide says not to use. Where a mask works, the guide warns of increased aliasing inside its
  borders; the probe is meant for a session, not for playing.
- FSR and XeSS read the same slot, so the same probe works on those backends; it has not been run on AMD or Intel hardware.
- On a still scene the picture is dominated by the jitter phase, which is why the unmasked half is used as the noise floor instead
  of an absolute threshold.
