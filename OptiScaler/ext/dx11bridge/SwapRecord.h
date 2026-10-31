#pragma once
// The per-chain record the live output switch replays after it has put a new chain on the window.
//
// Two things a chain carries cannot be read back from a dead chain, so they are recorded while the game uses the old one:
// the colour space the game asked for (SetColorSpace1, recorded in the bridge's existing seam) and how many resizes ran
// (the bridge's resize epilogue). The rules are here, no Windows include, so the unit tests can state them:
//
//   * the game's colour space is replayed unless it is the desktop default (DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, 0):
//     writing the default back is a no-op the new chain already has, and it is what a fresh chain is created with.
//   * a resize generation that changed between the capture and the switch means a resize ran while the chains were being
//     swapped: the new chain's size is not the one the capture described, so it is re-issued (a zero resize carries the
//     window's current size).
//
// Pure: no Windows or upstream include (tests/ext/fg/fg_switch_bridge.cpp).

#include <cstdint>

namespace ext::dx11bridge
{
// DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: what a flip chain is created with, and what the game's SDR default asks for.
inline constexpr uint32_t kColorSpaceDefault = 0;

// True when the recorded colour space must be written to the new chain. `recorded` is the bridge's own record; a game
// that never called SetColorSpace1 has none and the new chain keeps what it was created with.
constexpr bool ColorSpaceNeedsReplay(bool recorded, uint32_t colorSpace)
{
    return recorded && colorSpace != kColorSpaceDefault;
}

// True when a resize ran while the switch held the window: the new chain must be resized to the window again.
constexpr bool ResizeCrossed(uint32_t before, uint32_t after) { return before != after; }
} // namespace ext::dx11bridge
