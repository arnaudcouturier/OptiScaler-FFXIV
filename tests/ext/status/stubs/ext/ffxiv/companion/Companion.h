#pragma once
// Test stand-in for the companion detector: a flag the test sets directly.

namespace ext::ffxiv::companion
{
inline bool g_installed = false;

inline bool IsInstalled() noexcept { return g_installed; }
} // namespace ext::ffxiv::companion
