#pragma once
// Stand-in for the scene HDR module in the puddles' chained-order test: the chain asks it one question - is the engine's scene HDR path on - and
// the harness answers from its own switch, so the draw path runs without the HDR module or a config.
namespace ext::hdr::FfxivSceneHdr
{
inline bool& TestRequested() noexcept
{
    static bool requested = false;
    return requested;
}

inline bool Requested() noexcept { return TestRequested(); }
} // namespace ext::hdr::FfxivSceneHdr
