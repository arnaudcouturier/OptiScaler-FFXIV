#pragma once
// Identification log of the character preview feature: what the draw classifier sees, in a form a person can compare
// against what they opened in the game. Nothing here changes a draw. It runs whenever the feature is on (also with
// PreviewAA alone).
//
//  * the table of the preview renderer is logged twice: at the first draw the feature sees, and at the first preview
//    activity (so "no preview open" and "preview open" can be compared);
//  * "preview render activity started / ended" lines bracket a burst of preview draws, with per-kind counts;
//  * one "draw signature" line per distinct combination of draw kind, API, target entry and size, depth entry and shader
//    roles (at most 48 lines). A signature is not a window name: the game gives no window identity at this level.

#include <cstdint>

#include "ext/ffxiv/chara/CharaDraw.h"

namespace ext::ffxiv::chara::identify
{
// One classified draw (view.kind is set). Called before the draw runs.
void Observe(const DrawView& view);

// The outcome of the finisher check after the native finisher draw ran.
void OnFinisher(bool accepted);

// Called at least every 100 ms of draw activity: closes an activity burst that went quiet.
void Maintain(uint64_t nowMs);
} // namespace ext::ffxiv::chara::identify
