#pragma once
// Test double of the output-frame counter (ext/present/PresentSeams.h): the test advances it, by any step it likes.
#include <cstdint>

namespace ext::present
{
inline uint64_t g_testFrame = 0;
inline uint64_t FrameCount() noexcept { return g_testFrame; }
} // namespace ext::present
