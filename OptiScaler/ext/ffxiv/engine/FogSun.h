#pragma once
// The fog sun accessor (ext/ffxiv/engine): the one thing the fog-post capture and the night curve share.
//
// The engine's fog post binds cFogParam, whose third row holds the fog's own sun: a unit vector in world space whose y is the visible sun's elevation
// sine (0.183 on the daylight frame of the rc6 session, -0.109 / -0.058 / -0.012 on the three evening frames). The module that copies that buffer
// (the god rays' fog capture) publishes y here after a read landed; the module that needs it (shadow mapping's night curve) reads it back and asks for
// the capture with the wanted flag. Neither names the other: two inline atomics and one hook, so a change of the flag re-evaluates the capture's arming
// whichever module's config load ran first (the capture registers its hook at start-up; registering with the flag already up calls it once).
//
// The flag is false until a consumer sets it, so with shadow mapping and the character ambient off (the shipped defaults) the fog-post capture stays
// off. Two consumers today (shadow mapping, the character ambient): each owns one bit of the flag.

#include <atomic>
#include <cstdint>
#include <cstring>

namespace ext::ffxiv::fogsun
{
struct Reading
{
    bool have = false; // at least one read has been published
    float y = 0.0f;    // the fog sun's world y of the newest published read
    uint32_t count = 0; // how many reads were published: the consumer's freshness clock watches it change
};

using WantedChangedFn = void (*)();

// The consumers that may ask for the fog sun (2026-11-07 integration: the character ambient keys on the same curve as shadow mapping). Each sets
// its own bit; the capture is wanted while ANY bit is set, so one consumer letting go never drops the other's request.
inline constexpr uint32_t kShadowMapping = 1u;
inline constexpr uint32_t kCharaAmbient = 2u;

namespace detail
{
inline std::atomic<uint32_t> g_yBits { 0 };
inline std::atomic<uint32_t> g_count { 0 };
inline std::atomic<uint32_t> g_wantedBy { 0 };
inline std::atomic<WantedChangedFn> g_hook { nullptr };
} // namespace detail

// A landed read's sun y. A NaN is not a sun: neither stored nor counted.
inline void Publish(float y) noexcept
{
    if (!(y == y))
        return;
    uint32_t bits = 0;
    std::memcpy(&bits, &y, sizeof bits);
    detail::g_yBits.store(bits, std::memory_order_relaxed);
    detail::g_count.fetch_add(1, std::memory_order_release);
}

inline Reading Read() noexcept
{
    Reading reading;
    reading.count = detail::g_count.load(std::memory_order_acquire);
    reading.have = reading.count > 0;
    const uint32_t bits = detail::g_yBits.load(std::memory_order_relaxed);
    std::memcpy(&reading.y, &bits, sizeof reading.y);
    return reading;
}

inline bool Wanted() noexcept { return detail::g_wantedBy.load(std::memory_order_relaxed) != 0; }

// One consumer's flag (its kShadowMapping / kCharaAmbient bit). The hook is called when the OVERALL answer (any consumer wants it) CHANGES, never
// when a consumer repeats itself or joins / leaves while another still asks.
inline void SetWanted(uint32_t consumer, bool wanted) noexcept
{
    const uint32_t previous = wanted ? detail::g_wantedBy.fetch_or(consumer, std::memory_order_relaxed)
                                     : detail::g_wantedBy.fetch_and(~consumer, std::memory_order_relaxed);
    const uint32_t now = wanted ? (previous | consumer) : (previous & ~consumer);
    if ((previous != 0) == (now != 0))
        return;
    if (const WantedChangedFn hook = detail::g_hook.load(std::memory_order_acquire))
        hook();
}

// The capture's hook; registering with the flag already up calls it once, so the order of the two modules' start-up does not matter.
inline void SetWantedChangedHook(WantedChangedFn hook) noexcept
{
    detail::g_hook.store(hook, std::memory_order_release);
    if (hook != nullptr && Wanted())
        hook();
}

// Host tests only: the accessor back to its initial state.
inline void ResetForTest() noexcept
{
    detail::g_yBits.store(0, std::memory_order_relaxed);
    detail::g_count.store(0, std::memory_order_relaxed);
    detail::g_wantedBy.store(0, std::memory_order_relaxed);
    detail::g_hook.store(nullptr, std::memory_order_relaxed);
}
} // namespace ext::ffxiv::fogsun
