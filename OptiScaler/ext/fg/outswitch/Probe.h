#pragma once
// The read-only observation of the live chain, and why the destructive measurement left the game's process.
//
// Whether a frame-generation library's destroy call already ends the swapchain object cannot be decided from source, and
// measuring it needs two things the game's process cannot offer: chains of its own, and a release of a reference that may
// already be freed. That measurement therefore runs in a separate host executable (tools/fg-probe, built by the repo,
// with its own device, queue, window and chains). What stays here is read-only, and only on the live chain:
//   P1  the reference count at standstill (AddRef/Release around one read), at the FIRST present, where the boot code has
//       already dropped its creation references;
//   P4  the present vtable slot and the library it lives in (name, size, PE timestamp);
//   P9  how the class answers one DXGI_PRESENT_TEST.
// Nothing here creates, destroys or releases a chain, and nothing here touches process-global state: the old in-process
// probe released the shared XeLL context through the XeFG destroy, which broke a live XeFG chain (release_xefg_xell is
// process-global). Its rows are log-only as well: the switch reads compiled-in rows (LibraryModel.h, Seeded()), so one
// noisy measurement can never authorise a release rule.
//
// P1 runs once per process from the bridge's first present with either the probe key or the hot swap key on: a switch
// needs the standstill count to compare the live chain against the row it is about to use, and the read is one
// AddRef/Release. P4 (the identity line, including the companion DLL of a XeFG pair) runs with both keys too, because a
// row has to be checkable against the DLLs a player runs. P9 (the test present) is the developer measurement and stays
// with [FrameGen] FGOutputSwitchProbe. Both keys are ini only and default off; log lines carry the [fg-probe] prefix.

#include <cstdint>

namespace ext::fg::outswitch::probe
{
// Called once per bridge present from the bridge's own present seam. Reads the key, latches the answer and returns: with
// the key off (the default) the whole feature costs one relaxed load per present and one config read per process.
void OnFirstPresent();

// P1's answer for the live chain, in the same units the preflight compares: the count the first present measured, 0 when
// nothing was measured. A switch that finds a different count now refuses transiently (Fail::RefMismatch) instead of
// discovering an unaccounted holder in the teardown, after the old chain is already gone.
uint32_t StandstillRefs();
} // namespace ext::fg::outswitch::probe
