#pragma once
// S10 package A: the one config key of the game-side boundary detector, [HudLess] Diagnostics (docs/HUDLESS.md sections 10, 11).
// Package B's HudlessConfig owns every other [HudLess] key; this binder owns only Diagnostics, in the same ini section.
//
// Armed() = Diagnostics || ext::hudless::Wanted(). It sits on the draw hook, so it is ONE relaxed atomic load of a cached
// state. The cache is settled at the first Present (and refreshed at every Present afterwards, so a Mode change from the
// menu takes effect at the next Present). Before the first Present the state is "unsettled" and each call re-evaluates the
// config, because the very first shader creations happen before any Present and must already be tagged.

#include <Config.h>

#include <atomic>
#include <cstdint>

namespace ext
{
namespace config
{
class Binder;
}

namespace ffxiv::ui
{
struct UiConfig
{
    CustomOptional<bool> diagnostics { false }; // [HudLess] Diagnostics: turns on the D1 diagnostic build outputs

    void Bind(config::Binder& b); // defined in ext/core/ExtConfig.cpp next to FfxivConfig::Bind (tests link that file alone)
};

// 0 = unsettled (evaluate on demand), 1 = settled disarmed, 2 = settled armed
inline std::atomic<uint8_t> g_armState { 0 };

bool EvaluateArmed();      // Diagnostics || Wanted(), reads the live config (UiConfig.cpp)
bool ArmedSlow();          // unsettled path of Armed()
void RefreshArmed();       // settles the cache from the live config; called at every Present

inline bool Armed()
{
    const uint8_t s = g_armState.load(std::memory_order_relaxed);
    return s == 2 || (s == 0 && ArmedSlow());
}
bool DiagnosticsOn(); // the config key itself (D1 outputs), independent of Wanted()
} // namespace ffxiv::ui
} // namespace ext
