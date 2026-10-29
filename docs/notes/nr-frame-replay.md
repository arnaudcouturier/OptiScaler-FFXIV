# NR frame replay: capture a frame once, check shader changes against it for good

A replay set is a recording of the compute dispatches the Neural Rendering pass makes in one frame: the constants of every dispatch, the contents of every resource it reads and writes, and how those resources were viewed. The recording can be replayed without the game, on the Direct3D 12 software adapter (WARP), with the shaders of any build. Replaying the same inputs with an edited shader and comparing the outputs bit for bit with the outputs of an earlier build shows what the edit changed, down to the first pixel.

It is a developer tool. Nothing is recorded, and nothing is copied on the GPU, unless a capture is running.

## What a set contains

One folder, `dlssnr-capture\replay-<date-time>\` beside OptiScaler:

- `manifest.txt` is the text description (format version 2, one record per line, parseable without the product). A manifest of another version is refused.
- `images\img_NNNN.raw` holds the pixels, tightly packed rows (the readback's row padding is not stored). An image that was copied several times is stored once, by content.

Two consecutive frames are recorded per capture. For each compute dispatch of the pass (meter, encode, downsample, guide match, spatial pack, guides and unpack, multipass feedback blend, resolve, and the residual modes when they run) the manifest records:

- its index, kind and shader (and the hash of the compiled shader that ran);
- the 256-byte constants as bound;
- every resource it touched: width, height, DXGI format as the game declared it, the format the pixels were copied in, mip count and array size, and the image of its contents before the dispatch (resources it reads) and after it (resources it writes);
- every binding: shader view or unordered view, slot, the view format that was bound (the typed view over a typeless or depth resource), mip, slices, and the assumed state. The views of the pass always cover the whole resource; the sub-rectangle a pass works on travels in the constants.
- per frame, how many dispatches the pass made and how many of them (and how many boundary pictures) the recorder had to leave out. Nothing is left out silently: a dispatch that cannot be recorded still advances the dispatch index, so the indices show the gap, and the log line states the counts;
- the pictures at the boundaries of the pass: `game_frame` (the untouched game frame the pass starts from), `model_answer_N` (what the model returned for pass N), `output_scaling_up` and `output_scaling_down` (the Output Scaling legs, when they run) and `final_frame`.

What is not recorded:

- The model itself. It is not replayed; its answer is an input of the dispatches that read it.
- The game's exposure texture. The pass reads it in a state the pass did not set, and copying it would mean transitioning someone else's resource on a guess. A replay sees zeros there, which the shader treats as "no live exposure". (The meter dispatch is therefore replayed with an empty exposure input.)
- Anything beyond mip 0, slice 0 of a resource.
- The RCAS sharpening pass. It runs before Neural Rendering, so with the Magnifier off its output is the frame the pass starts from (`game_frame`); the frame entering RCAS is not reachable without a change to upstream code.
- Compute dispatches made outside the scene pass: the deferred pre-upscale residual stages and the character preview use the same dispatch routine but run in other windows of the frame, and are not recorded.

Captures show the player's screen. Never commit or share them.

## Capturing

1. In `OptiScaler.ini`, section `[DlssNr]`, set `ReplayCapture=true` (and, for one capture on its own, `AutoCapture=true`, which captures once about 180 frames after the pass starts). The key has no menu control. Set it before the game starts.
2. Get the scene on screen. Create an empty file named `dlssnr-capture.trigger` beside `OptiScaler.dll`. It is picked up within a second, deleted, and the next two frames are recorded.
3. A few frames later the set is written, with a short pause. The log says `DLSS-NR wrote a replay set to ...` with the number of dispatches, images and megabytes.
4. Repeat for each scene; every capture is a new `replay-...` folder. The capture folder is cleared when a session first asks for a capture, except for `replay-*` folders.

Size (computed, not measured; there has been no capture in the game yet). One 3840x2160 half-float RGBA image is 66 MB. A frame stores about four distinct such images (the game frame, the encoded proxy, the model's answer, the final frame) plus the guides, so a captured pair of 4K frames is about 0.5 to 0.6 GB on disk. While capturing, the copies of a frame (about eight to ten such images, since a resource is copied again after it was written) stay in memory until the write: up to about 1.2 GB for the two frames. The recorder stops copying at 3 GiB, leaves the remaining dispatches out and says how many. With the model working above native resolution the images are larger.

If the set cannot be written (the folder cannot be made, an image or the manifest cannot be written) the log says so as an error, no "wrote a replay set" line is printed, and the half-written folder is removed so that nothing that looks like a set is left behind.

## Replaying and blessing

The test is `tests\ext\nr\nr_replay.cpp` (class `local`). It needs a product build in the same worktree (it uses the generated shader headers of the build) and the folder of the capture. Move the `replay-...` folders out of the game folder first, to a folder outside the repository: the test writes the blessed images (`reference\`) and the difference pictures (`replay-out\`, emptied at the start of every run) next to the set.

Compare (the normal run):

```
$env:OPTISCALER_NR_REPLAY_DIR = '<folder holding the replay-... folders, or one of them>'
pwsh -NoProfile -File tools\test.ps1 -Local -Filter nr_replay
```

Bless (the first time, and when a shader change is intended). A bless request takes effect only with its confirmation; without it the test prints that it was refused, writes nothing and exits with code 3, so a variable left in the environment cannot re-bless by itself and cannot be taken for a pass:

```
$env:OPTISCALER_NR_REPLAY_BLESS = 'all'      # or a comma list of dispatch kinds, for example resolve,feedback
$env:OPTISCALER_NR_REPLAY_BLESS_OK = '1'
pwsh -NoProfile -File tools\test.ps1 -Local -Filter nr_replay
Remove-Item Env:OPTISCALER_NR_REPLAY_BLESS, Env:OPTISCALER_NR_REPLAY_BLESS_OK
```

The blessed images are not what the game's GPU wrote. A GPU and WARP round differently, so a comparison against GPU output could never be bit exact. They are WARP's own outputs of replaying the captured inputs with the shaders of the build being blessed, written to `<set>\reference\` with a `provenance.txt`. There is one blessed set per capture, not one per shader version: after a shader edit the replay is compared with the blessed images of the earlier build, which is the point. The provenance holds, for every dispatch, the hash of the shader, the commit, the date, whether the working tree had uncommitted changes, and the adapter of the blessing that wrote its images; a partial blessing leaves the other kinds' records as they were.

- No blessed set: the test fails (it is never skipped) and says to bless first.
- A capture that is not complete fails before anything runs, in compare and in bless mode: a dispatch or boundary picture left out by the recorder, a missing dispatch index, a frame with no dispatches, or a frame the manifest does not describe. The messages name the frame and the indices. A folder that holds images but no manifest (a write that failed) fails too.
- Matching run: silent apart from the summary.
- A difference: for each dispatch that differs, the dispatch index and kind, the first differing pixel, the number of differing pixels, a PSNR (peak 1.0) and largest component error, whether the shader of that dispatch differs from the blessed one, and a picture of the difference (`replay-out\diff_f<frame>_d<index>_u<slot>.ppm`, red/green/blue are the error of the first three components, scaled so the largest error is full scale).
- The adapter changed since the blessing (Windows update): the test prints `WARP changed <old> -> <new>: re-bless, not a shader regression` and fails. Bless everything again. An adapter whose version or file cannot be read is never a match: the test fails and nothing is replayed or written, whatever was asked. The replay runs on the software adapter only.
- Blessing prints a `BLESSED <kinds> at <commit>` line (with a note when the working tree had uncommitted changes) and ends with exit code 0 only when every write succeeded. A bless list that names a kind no dispatch of the replayed sets has is refused with the unmatched names, and nothing is written. Blessing `all` writes every dispatch. Blessing a list of kinds rewrites only those and compares the rest; use it when a change to those shaders is intended, and say why in the commit message. The commit recorded is that of the repository the test was started from (not the folder the shell happens to be in).
- Every dispatch is also replayed twice and the two results must be identical; a kind that is not reproducible fails the test (it would need a documented tolerance). The shaders use no atomics and no group-shared memory. The synthetic test replays twice and requires identical bytes for encode, downsample, resolve, meter, multipass feedback, guide match, all eight residual modes and the three spatial stages (pack, guides, unpack), built from source at run time; the build's own optimised shaders are checked the same way on the first real replay.
- A dispatch gets up to ten minutes on the GPU (WARP at 3840x2160 is slow). If it takes longer, or the device is lost, the whole set is abandoned: the test fails naming the dispatch, and the objects the GPU may still be using are never reused.
- The manifest is checked when it is read: every dispatch binds each of t0..t5 and u0..u1 once, no (frame, index) twice, an image's file name stays inside the set folder, and an image's format and size agree with the resource it belongs to.
- The outputs are also compared with what the game's GPU wrote, for information only. Unwritten margins and rounding make a difference there normal.

`tests\ext\nr\nr_replay_synthetic.cpp` (class `warp`, always run) exercises all of this with a set the test writes itself, so the machinery is covered without a capture.

## Limits

- While a capture is written, the NR lock is held for seconds on the render thread. The game hitches at capture time and only then; the key is a developer tool and is off by default.
- To copy the pass's inputs the recorder transitions them between NON_PIXEL_SHADER_RESOURCE and COPY_SOURCE and back. That includes the pass's guide inputs (the game's depth and motion when they are used directly rather than through a copy the pass owns). It relies on the same input-state contract the pass already relies on when it binds them as compute shader views. The game's exposure texture is not recorded, because the pass never binds it in a state it knows.
- The recorder is unit-tested on the software adapter with synthetic dispatches; it has not run in the game yet.
