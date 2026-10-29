#pragma once
// Feature bits for the inline fast-path guards in Seams.h (docs/SEAMS.md section 3.1).
// A hot-path seam is `if (!ext::Active(ext::F::X)) return <upstream default>;` followed by an out-of-line call,
// so a feature that is off costs one relaxed load and one branch.

#include <atomic>
#include <cstdint>

namespace ext
{
enum class F : uint64_t
{
    None = 0,
    Nr = 1ull << 0,         // DLSS Neural Rendering (nr/)
    Fg = 1ull << 1,         // frame generation extensions (fg/)
    Hdr = 1ull << 2,        // OptiHDR, HDR PNG, RenoDX scene HDR (hdr/)
    Hudless = 1ull << 3,    // hudless engine (hudless/)
    Gpu = 1ull << 4,        // submission ownership, GPU lifetime (gpu/)
    Restrack = 1ull << 5,   // resource-tracking extensions
    Present = 1ull << 6,    // present path extensions
    Streamline = 1ull << 7, // Streamline extensions
    Dx11Bridge = 1ull << 8, // D3D11 on D3D12 bridge extensions
    Upscaler = 1ull << 9,   // upscaler-layer extensions
    Game = 1ull << 10,      // a game module is active (ext::game::Current() is not all null)
    PlateProbe = 1ull << 11, // developer probes of the output stream and the UI draws ([HudLess] PlateDiagnostics, log only)
};

namespace detail
{
inline std::atomic<uint64_t> g_activeFeatures { 0 };
}

inline bool Active(F f) { return (detail::g_activeFeatures.load(std::memory_order_relaxed) & static_cast<uint64_t>(f)) != 0; }

// True when either bit is set: one relaxed load and one test, the same cost as Active() (seams that serve two consumers).
inline bool ActiveAny(F a, F b)
{
    return (detail::g_activeFeatures.load(std::memory_order_relaxed) & (static_cast<uint64_t>(a) | static_cast<uint64_t>(b))) != 0;
}

inline void SetActive(F f, bool on)
{
    if (on)
        detail::g_activeFeatures.fetch_or(static_cast<uint64_t>(f), std::memory_order_relaxed);
    else
        detail::g_activeFeatures.fetch_and(~static_cast<uint64_t>(f), std::memory_order_relaxed);
}
} // namespace ext
