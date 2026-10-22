#pragma once
// Window-state source of [DlssNr] DisableOnFocusLoss (decision logic: NrFocusGate.h, NrFocusTracker.h).

namespace ext::nr
{
// One call per rendered frame from the NR pass (under its mutex). Samples the game window at most every 50 ms, feeds
// the debounce gate and returns whether the NR pass may run. `switchOn` is DisableOnFocusLoss combined with "NR is
// on"; false returns true and clears the gate. Fails open: with no window found the pass is allowed.
bool FocusAllowsNr(bool switchOn);

// The same question for the character preview NR (one call per preview render, under the preview module's mutex). It has
// its own gate, so the scene pass and the preview never clear each other's pause, and shares the window sample. Writes
// one log line when the pause starts and one when it ends. `switchOn` is "preview NR is wanted" combined with
// DisableOnFocusLoss.
bool FocusAllowsPreviewNr(bool switchOn);
} // namespace ext::nr
