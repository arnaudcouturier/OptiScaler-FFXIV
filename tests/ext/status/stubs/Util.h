#pragma once
// Test stand-in for the path helpers: only the exe folder the collector derives
// the game folder from is modelled. The test drives it directly.

#include <filesystem>

namespace Util
{
inline std::filesystem::path g_exePath;

inline std::filesystem::path ExePath() { return g_exePath; }
} // namespace Util
