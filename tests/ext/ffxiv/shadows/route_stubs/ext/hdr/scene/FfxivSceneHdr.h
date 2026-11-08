#pragma once
// Test double of the scene HDR state the shadow mapping module samples: one switch the test sets.
namespace ext::hdr
{
struct FfxivSceneHdr
{
    static inline bool g_requested = false;
    static bool Requested() { return g_requested; }
};
} // namespace ext::hdr
