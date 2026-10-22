#pragma once
// The two native hooks of the source-level jitter fix ([FFXIV] JitterSource), FfxivJitterSourceHooks.cpp:
//   - the jitter generator, called by the camera update (exe+0x25f79e): divides its output by the native strength and replaces the unbounded
//     phase count, so the game draws the full +-0.5 pixel lattice on NVIDIA's repeating pattern;
//   - the camera getter, called by the world-scene NGX packet producer (exe+0x374122): observes the jitter object the packet is built from
//     (observe only: the pointer it returns is the game's own).
// Installed on the first evaluate after the key is on (never when it is off), through the same guarded Detours transaction and retry rules as
// the native-quality hooks: both hooks or none. Every byte the hooks rely on is checked first (GameBuild.h JitterSourceBuild).
// With the fix switched off (or fallen back) both hooks stay in place and pass straight through to the game.

#include "ext/ffxiv/jitter/FfxivJitterSourceController.h"

namespace ext::ffxiv::jitter::source::hooks
{
// What the detours write and the boundary reads.
SharedState& State();

// The environment's view for the controller: the executable matched, the instruction guards passed, the hooks are installed. Asking installs
// the hooks when they are not yet installed (the retry rules of the native-quality install apply: three attempts, 250 ms and 2 s apart,
// a deterministic refusal ends after one).
HooksView View();
} // namespace ext::ffxiv::jitter::source::hooks
