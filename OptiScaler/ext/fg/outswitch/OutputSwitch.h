#pragma once
// The composition of the live output switch into the bridge: the real Ops binding, the switch point the bridge's present
// calls, the status the menu reads, and the epoch a reader uses to notice that the output under it changed.
//
// The switch point runs inside one bridge present, on the present thread, from the present scope's first statement. With
// both feature keys off it is one relaxed load, one config read and one compare against the recorded bridge: nothing else
// runs, and no hook body, no bridge seam and no accessor is reached.

#include "ext/fg/outswitch/StatusBox.h"
#include "ext/fg/outswitch/SwitchTypes.h"

#include <cstdint>

namespace ext::fg::outswitch
{
// Once per bridge present, first statement of the present scope.
void OnPresent();

// Incremented whenever the output that carries frame generation changed: a commit, a rollback, a limp rebuild. A reader
// that cached the provider's own state (the automatic MFG selector) compares it and starts over instead of acting on a
// chain that is gone.
uint32_t SwitchEpoch();

// The status the menu shows and the log's last transition. Atomics only: safe to read from the menu thread.
Status MenuStatus();

// The menu's "try again": forget one target's failure for this session. The budget is not refunded.
void TryAgainTarget(Target target);

// The module's own reason for the menu's red line: true once the switch gave up on this session.
bool RestartNeeded();
} // namespace ext::fg::outswitch
