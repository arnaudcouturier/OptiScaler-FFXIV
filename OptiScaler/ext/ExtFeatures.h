#pragma once
// Feature bits for the inline fast-path guards in Seams.h (docs/SEAMS.md section 3.1).
// A hot-path seam is `if (!ext::Active(ext::F::X)) return <upstream default>;` followed by an out-of-line call,
// so a feature that is off costs one relaxed load and one branch.

#include <atomic>
#include <cstdint>
#include <type_traits>

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
    DrawDiag = 1ull << 12,  // developer draw diagnostic ([FFXIV] DrawDiagnostics, log only)
    Plate = 1ull << 13,      // native nameplate interpolation (ffxiv/plates + hudless/UiPlates.cpp)
    ContactShadows = 1ull << 14, // contact shadows before the sun light pass ([FFXIV] ContactShadows)
    EnginePassOverrides = 1ull << 15, // engine pass overrides: a constant-buffer patch for one draw ([FFXIV] EnginePassOverrides)
    LightShafts = 1ull << 16, // light shafts before the tone map ([FFXIV] LightShafts)
    ShadowMapping = 1ull << 17, // shadow mapping: the sun mask's lift and the composite's night ambient lift ([FFXIV] ShadowMapping)
    GodraysVolumetric = 1ull << 18, // the volumetric god rays march ([FFXIV] GodraysVolumetric); bit 17 is the shadows slice's claim
    CharaAmbient = 1ull << 19,      // the lighting tab's chara ambient: one scale entry on the chara draws' own g_AmbientParam ([FFXIV] CharaAmbient)
    Gi = 1ull << 20, // the screen-space light bounce, the tone-map chain's stage after the march ([FFXIV] Gi)
    DepthOfField = 1ull << 21, // depth of field, Route B: this fork's own chained stage after the light bounce ([FFXIV] DepthOfField)
    LensFlare = 1ull << 22, // the flare family's own card: the ring's, the sun's and the ghosts' buffer patches, the veil's skip ([FFXIV] FlareCard)
};

namespace detail
{
inline std::atomic<uint64_t> g_activeFeatures { 0 };
}

inline bool Active(F f) { return (detail::g_activeFeatures.load(std::memory_order_relaxed) & static_cast<uint64_t>(f)) != 0; }

// True when ANY of the given features is active: one relaxed load and one test, the same cost as Active() whatever the count. The seams that
// serve several consumers (the draw seam's hook serves five today) test them together, so all of them off costs one load. This is the ONLY
// definition: a slice that adds a consumer appends its flag to the call and never adds an overload.
template <class... Fs>
inline bool ActiveAny(F first, Fs... rest)
{
    static_assert((std::is_same_v<Fs, F> && ...), "ActiveAny takes features only");
    return (detail::g_activeFeatures.load(std::memory_order_relaxed) &
            (static_cast<uint64_t>(first) | ... | static_cast<uint64_t>(rest))) != 0;
}

inline void SetActive(F f, bool on)
{
    if (on)
        detail::g_activeFeatures.fetch_or(static_cast<uint64_t>(f), std::memory_order_relaxed);
    else
        detail::g_activeFeatures.fetch_and(~static_cast<uint64_t>(f), std::memory_order_relaxed);
}
} // namespace ext
