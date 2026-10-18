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

// The outcome of the finisher check (view.finisherVerdict / view.finisherCheck): counted per reason, and the values the check
// read are logged once for the first rejection of each reason.
void OnFinisher(const DrawView& view);

// A draw with the finisher shader that renders outside the preview renderer's outputs (not a preview draw).
void OnOffTableFinisher(const DrawView& view);

// A depth-only draw (no render target) into a depth texture of the table, whether or not it satisfies the prepass rule: one
// line per distinct combination of depth entry, vertex-shader cb0 size, viewport match, sample count and draw API (at most 12).
void ObserveDepthOnly(const DrawView& view);

// A draw that renders into a table entry or samples one: the CRC-32 and size of its pixel shader are logged once per shader
// (at most 64 lines), so a shader list that does not match this game build can be corrected from the log.
void ObserveShader(const DrawView& view);

// Called at least every 100 ms of draw activity: closes an activity burst that went quiet.
void Maintain(uint64_t nowMs);
} // namespace ext::ffxiv::chara::identify
