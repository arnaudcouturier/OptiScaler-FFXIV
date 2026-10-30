#pragma once
// The process-wide shader catalog ledger and the entry points the hooks call (see CatalogLedger.h for what it records and logs).
//
// Included by FfxivLightingCapture.h (the CreatePixelShader hook), PlateLayoutTag.h (the CreateInputLayout tagging) and
// FfxivUiBoundary.cpp (the partial-UI-catalog guard and the Present tick), so it stays light: everything here is inline except OnPresent(),
// which logs and lives in FfxivCatalog.cpp.
//
// Where each call runs:
//   OnPixelShaderCreated  every DXBC pixel shader the creation hook sees, whether or not the HUD-less detector is armed (the same always-run
//                         place as the boundary stamp; NOT the D1 registry, which runs only while armed). Lock free, allocation free, no log.
//   OnNameplateVertexShader / OnLayoutSeen   CreateInputLayout tagging of the nameplate vertex shaders (PlateLayoutTag.h, written by the lighting hooks).
//   OnPresent             bridge Present entry (ui::OnPresentBegin, BEFORE its Armed() test: default-ini sessions have to get the catalog line too).
//   UiBoundaryUsable      UiBoundaryProvider::Take() / LastReason(), on every frame, never latched.

#include "CatalogLedger.h"

#include <chrono>
#include <type_traits>

namespace ext::ffxiv::catalog
{
// The ledger lives for the whole process and is never destroyed (no teardown work under the loader lock): everything in it is trivially
// destructible, which the assert below keeps true.
static_assert(std::is_trivially_destructible_v<Ledger>);

inline Ledger& Global()
{
    static Ledger ledger;
    return ledger;
}

namespace detail
{
inline std::atomic<int64_t>& BaseTicks()
{
    static std::atomic<int64_t> base { 0 };
    return base;
}
} // namespace detail

// Milliseconds since the hooks were installed (OnHooksInstalled), or since the first call if nothing installed them yet. Steady clock.
inline uint32_t ElapsedMs() noexcept
{
    using clock = std::chrono::steady_clock;
    const int64_t now = clock::now().time_since_epoch().count();
    std::atomic<int64_t>& baseSlot = detail::BaseTicks();
    int64_t base = baseSlot.load(std::memory_order_relaxed);
    if (base == 0)
    {
        int64_t expected = 0;
        base = baseSlot.compare_exchange_strong(expected, now) ? now : expected;
    }
    const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::duration(now - base)).count();
    return ms <= 0 ? 0u : ms >= 0xFFFFFFFFll ? 0xFFFFFFFFu : static_cast<uint32_t>(ms);
}

// The creation hooks are about to be attached: time zero for every "t=...ms" the ledger logs.
inline void OnHooksInstalled() noexcept { (void)ElapsedMs(); }

inline void OnPixelShaderCreated(const void* code, size_t size, uint32_t crc) noexcept
{
    try
    {
        Global().NotePixelShader(code, size, crc, ElapsedMs());
    }
    catch (...)
    {
    }
}

inline void OnNameplateVertexShader(const void* code, size_t size, uint32_t crc) noexcept
{
    try
    {
        Global().NoteVertexShader(code, size, crc, ElapsedMs());
    }
    catch (...)
    {
    }
}

inline void OnLayoutSeen() noexcept
{
    try
    {
        Global().NoteLayoutSeen();
    }
    catch (...)
    {
    }
}

// Is the UI shader catalog complete enough for the HUD-less boundary source? Evaluated per call (see UiCatalogHealthy).
inline bool UiBoundaryUsable() noexcept
{
    try
    {
        return Global().UiHealthy();
    }
    catch (...)
    {
        return true; // fail open: the guard only ever REMOVES a feature, an internal failure must not
    }
}

// One Present: counts it and hands every log line that is due to `sink` (pure part of OnPresent, shared with the tests).
template <class Sink> inline void PresentTick(Ledger& ledger, uint32_t nowMs, Sink&& sink)
{
    ledger.NotePresent();
    for (const std::string& line : ledger.Poll(nowMs))
        sink(line);
}

// Present entry: PresentTick on the global ledger, lines to the OptiScaler log. Never throws. (FfxivCatalog.cpp)
void OnPresent() noexcept;
} // namespace ext::ffxiv::catalog
