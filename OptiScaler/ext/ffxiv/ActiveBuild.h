#pragma once
// ActiveBuild(): the build every game-address consumer reads, and ActiveGate(): which of those consumers may run.
//
// Header-only and constant-initialised (one atomic pointer), so any translation unit and any test can include it; nothing here reads the
// game. The startup (ext/ffxiv/resolve/ResolveStartup.cpp, called once from the first device hook) resolves the executable, decides, and
// publishes. Until then:
//   * ActiveBuild() is the compile-time table (kCurrentBuild), exactly what every consumer read before the resolver (and what the unit tests of the
//     consumers see: they never run the startup);
//   * ActiveGate() is the "not started" report: every feature OFF (fail open), so a consumer that is reached before the startup refuses and
//     answers "not ready" instead of latching a wrong answer. A consumer must therefore ask the gate on every call (or cache only a TRUE
//     answer), never cache a refusal computed before ActiveStarted().
//
// After the startup:
//   * resolved mode (the default): ActiveBuild() is ResolvedBuild::build. On the known build every field equals the table (the resolver's
//     cross-check makes the table win on a disagreement and fills an unresolved seam from it; a failClosed shape or an EntryPatched seam stays
//     ZERO, and the gate refuses the feature that reads it). On an unknown build it holds the resolved values; every Rva/Guard field of a seam that
//     did not resolve is ZERO, offsets keep the table defaults (logged as not self-healing).
//   * legacy mode ([FFXIV] FfxivLegacyGameBuild): ActiveBuild() stays the compile-time table and the gate says "Group 2 iff the exact known
//     build", i.e. exactly the legacy path.

// relative includes: some test recipes put only their own folders on the include path
#include "GameBuild.h"
#include "resolve/GateTypes.h"

#include <atomic>
#include <utility>

namespace ext::ffxiv
{
namespace detail
{
struct ActiveState
{
    GameBuild build {};
    resolve::GateReport gate {};
};
inline std::atomic<const ActiveState*> g_active { nullptr };

inline resolve::GateReport MakeNotStartedReport()
{
    resolve::GateReport r;
    for (resolve::Decision& d : r.features)
    {
        d.enabled = false;
        d.reason = "the startup resolve has not run yet: fail open";
    }
    return r;
}
} // namespace detail

// True once the startup has published (also a failure: the report then says why every Group 2 feature is off).
inline bool ActiveStarted() { return detail::g_active.load(std::memory_order_acquire) != nullptr; }

inline const GameBuild& ActiveBuild()
{
    const detail::ActiveState* s = detail::g_active.load(std::memory_order_acquire);
    return s != nullptr ? s->build : kCurrentBuild;
}

inline const resolve::GateReport& ActiveGate()
{
    const detail::ActiveState* s = detail::g_active.load(std::memory_order_acquire);
    if (s != nullptr)
        return s->gate;
    static const resolve::GateReport notStarted = detail::MakeNotStartedReport();
    return notStarted;
}

inline bool FeatureEnabled(resolve::Feature f) { return ActiveGate().Enabled(f); }

// THE ONE WAY a game-address consumer gets the build it reads: the build only while the gate has enabled the feature THAT consumer asks
// for, nullptr otherwise. The pure form has no globals (a test passes a report and a build); the second form is the process's published state. Not
// cached: a refusal before the startup has published must never stick.
inline const GameBuild* BuildFor(const resolve::GateReport& gate, const GameBuild& build, resolve::Feature feature)
{
    return gate.Enabled(feature) ? &build : nullptr;
}
// The same for one published state (nullptr: nothing published yet, no build for any feature). The gate and the build are taken from the SAME state.
inline const GameBuild* BuildForState(const detail::ActiveState* state, resolve::Feature feature)
{
    return state != nullptr ? BuildFor(state->gate, state->build, feature) : nullptr;
}
// ONE acquire load of the published state: ActiveGate() and ActiveBuild() are two loads, and the order in which the two arguments of a call are
// evaluated is unspecified, so BuildFor(ActiveGate(), ActiveBuild(), f) could pair the compile-time table with a gate that was published a moment later.
inline const GameBuild* BuildFor(resolve::Feature feature) { return BuildForState(detail::g_active.load(std::memory_order_acquire), feature); }

// Publishes the build and the report for the life of the process (heap, never freed: game threads may read them while the process shuts
// down). The first call wins; later calls change nothing and return false.
inline bool PublishActive(const GameBuild& build, resolve::GateReport gate)
{
    auto* state = new detail::ActiveState { build, std::move(gate) };
    const detail::ActiveState* expected = nullptr;
    if (detail::g_active.compare_exchange_strong(expected, state, std::memory_order_acq_rel))
        return true;
    delete state;
    return false;
}
} // namespace ext::ffxiv
