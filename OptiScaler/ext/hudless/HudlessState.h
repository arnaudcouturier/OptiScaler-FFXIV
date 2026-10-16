#pragma once
// S10: the one flag the three wave-A work packages share (docs/HUDLESS.md section 4.3).
//
// Written by the engine (package B) from the [HudLess] Mode key; read by the hook sites and the game-side boundary
// detector (package A) so they cost one atomic load when the feature is off. Defined in HudlessState.cpp so every
// package builds on its own.

namespace ext::hudless
{
// True while [HudLess] Mode != off (and a game source exists). Cheap, any thread.
bool Wanted();
void SetWanted(bool wanted);
} // namespace ext::hudless
