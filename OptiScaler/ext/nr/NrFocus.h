#pragma once
// Window-state source of [DlssNr] DisableOnFocusLoss (decision logic: NrFocusGate.h).

namespace ext::nr
{
// One call per rendered frame from the NR pass (under its mutex). Samples the game window at most every 50 ms, feeds
// the debounce gate and returns whether the NR pass may run. `switchOn` is DisableOnFocusLoss combined with "NR is
// on"; false returns true and clears the gate. Fails open: with no window found the pass is allowed.
bool FocusAllowsNr(bool switchOn);
} // namespace ext::nr
