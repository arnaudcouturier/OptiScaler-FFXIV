#pragma once
// Test stand-in for the feature bits: a mask the test sets directly.

#include <cstdint>

namespace ext
{
enum class F : uint64_t
{
    None = 0,
    Nr = 1ull << 0,
    Hdr = 1ull << 1,
};

inline uint64_t g_activeFeatures = 0;

inline bool Active(F f) { return (g_activeFeatures & static_cast<uint64_t>(f)) != 0; }
} // namespace ext
