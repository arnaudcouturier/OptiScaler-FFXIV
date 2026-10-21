#pragma once
// What counts as a usable frame rate cap. A cap that is NaN, infinite or not above zero means "no cap" (0).
//
// Pure (no Config, no Win32) so the rule is unit-tested without the game (tests/ext/fg/frame_limit_sanity.cpp).
// Users: FrameInterval.cpp (the limiter's interval) and FgConfig::PostLoad, which drops such a value read from
// [Framerate] FramerateLimit so that the unsigned conversion in the NVIDIA Reflex limiter path
// (Reflex_Hooks.cpp, setFPSLimit) never sees it. A positive cap so small that 1e6 / cap leaves the 32-bit range is not
// handled here; it is not a value anyone sets.

#include <cmath>

namespace ext::present
{
inline bool IsUsableFrameLimit(float fps) { return std::isfinite(fps) && fps > 0.0f; }

// The cap to use: `fps` when it is usable, 0 (no cap) otherwise.
inline float SaneFrameLimit(float fps) { return IsUsableFrameLimit(fps) ? fps : 0.0f; }
} // namespace ext::present
