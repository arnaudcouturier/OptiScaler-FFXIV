#pragma once
// Effect meter (METER): the runtime's public face. The meter listens at the stage-dump seam (ext/ffxiv/diag/StageDump.h: StageRan and StageReport call it on
// every frame, before the capture's gate), so the stages' own files only gained the frame's white point on their run report and the menu cards one line.
//
// What it does, per effect:
//   every rendered frame   - counts it: reached, ran, or skipped (by the stage's own reason);
//   every ~3 s             - on ONE sampled frame (one effect per frame, staggered), draws the measurement of the stage's input against its output (MeterPass.h) and
//                            reads it a frame or two later without waiting;
//   every ~10 s            - one line per effect: frames reached / ran / skipped by reason, the visible share, the mean change, the region;
//   60 s after the first rendered frame, then every 5 minutes - the summary block, one line per effect:
//                            "Effect visibility: <effect> ran X % of frames, changed Y % of its region by > 8/255 (mean Z/255)".
// The menu cards call CardLine() for their "ran X %, visible Y %" line. Nothing here changes a picture or a stage's decision.

#include "ext/ffxiv/meter/MeterRules.h"

#include <string>

namespace ext::ffxiv::meter
{
// "Meter: ran 80 %, visible 23 % (mean 11.5/255)" for the card of the stage. Thread-safe: the menu thread reads what the render thread published at its last
// window (about ten seconds) and its last sample (about three seconds).
std::string CardLine(StageId id);
} // namespace ext::ffxiv::meter
