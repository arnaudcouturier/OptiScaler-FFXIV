#pragma once
// The lease of the game's simulation tick: the vtable slot of the Framework object, not its code. ext/latency/TickLeaseCore.h holds every rule;
// this header is the small surface the rest of the fork calls.
//
// Why the slot and not a detour: a code hook over the entry is refused by this fork's own resolver (EntryPatched) and does not survive another
// inline hook over the same entry (Dalamud's framework update hook). The slot is a pointer in a read-only image table, so replacing and calling it
// composes with an inline hook in either order: the observer calls the old pointer, which may be another tool's detour, which calls the game.
//
// What is written: one pointer-sized compare-exchange, after the checks of TickLeaseCore.h. Everything the observer reads (the original tick, the
// canonical tick, the Framecounter address and the slot) is published BEFORE that exchange, so a tick that takes the slot the instant it changes
// always has a callable original and never returns without running the game's tick. When the key is off nothing is installed at all, so a default
// ini behaves exactly like a build without this code.
//
// Lifetime: installing pins this module (GET_MODULE_HANDLE_EX_FLAG_PIN) before the write, and a pin that fails refuses the install. OnUnload is the
// best-effort restore of the unhook path: it puts the old pointer back only while the slot still holds our observer. It is unreachable in today's
// build (the fork never unhooks its D3D11 hooks), so the restore path is covered by the unit tests and not by a session; if it is ever wired up, an
// inline hook another tool placed on the observer would be orphaned (the slot still equals the observer, so the restore proceeds) and the slot's
// old pointer (the game's tick) is what comes back.
//
// The install line names the order it found: `order=slot-first` means the slot already held the game's tick when we wrote (no earlier slot owner;
// another slot-swapping tool is refused, never chained), and `entry=` shows the first bytes of the function, so a foreign inline hook over the entry
// - whose plugin update then runs inside our tick timing - is visible next to it.

#include "ext/latency/TickLeaseCore.h"

#include <cstdint>

namespace ext::latency
{
// The first D3D11 device hook, right after the startup resolve: the first install attempt. Refreshes the mirrored [FramePacing] TickProbe key first.
void OnDeviceHooked();

// One DX11 bridge present: advances the retry plan (one attempt every kRetryPresents presents, for kRetryWindowUs at most) and nothing at all with the
// key off.
void OnBridgePresent();

// The game's Framecounter as the install published it. False when the offset did not resolve or the lease has not installed; the probe then reports
// age and W as n/a instead of guessing. Called on the game and present threads, one relaxed load plus the u32 read.
bool ReadFrameCounter(std::uint32_t& value);

// The fork's own unhook (ext::game::OnD3D11Unhook): restore the slot if it still holds our observer. Unreachable in today's build; see above.
void OnUnload();

// True while our observer sits in the slot: the probe arms only on top of this.
bool Leased();

// The state as the last log line described it: "not installed", "installed", "restored", or a refusal reason. For tests and the log only.
const char* StateText();
} // namespace ext::latency
