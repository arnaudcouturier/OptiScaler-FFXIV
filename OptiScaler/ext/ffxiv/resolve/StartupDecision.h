#pragma once
// The startup decision of the game build handling, as PURE code. ResolveStartup.cpp is a thin product shell around RunStartup: it only
// supplies the environment (the config keys, the exe path, the PE stamp and size of the running image, the registry, the live image and the resolver,
// the log, the publish). Every decision lives here and is unit-tested (tests/ext/ffxiv/resolve/startup_flow.cpp), so a wrong key mapping, a published
// build that is not the resolved one, a missing fallback or a table offered to the forced-unknown path is a failing test and not a code review item.
//
// What one startup does (RunStartup):
//   1. read the three keys (ReadSettings) and the exe path, and ask whether the running image has the table's stamp and size (MakeGateInput);
//   2. not the game (neither the table's stamp and size nor the name ffxiv_dx11.exe), or the rollback key: nothing is opened, scanned or built; the
//      gate says what the old rules say;
//   3. otherwise build the registry, offer the table (never under FfxivForceUnknownBuild) and resolve once; a missing image is `nullptr`;
//   4. Decide(): Group 1 by the exe identity (stamp and size on the known build, the name otherwise); Group 2 by the seams; an INFRASTRUCTURE failure
//      on the exact known build falls back to the legacy decision;
//   5. log the block (a logging failure never changes the decision) and publish ONCE: the build the consumers read and the gate report.
//
// What is published as the build (PublishedBuild):
//   * the resolved build (ResolvedBuild::build) after a resolve that worked: on the known build it equals the table row by row, on an unknown build it
//     holds the resolved values with every unresolved Rva / Guard field ZERO;
//   * the compile-time table for the rollback key, for a process that is not the game (as in the previous release) and for the exact known build whose resolver
//     failed (the legacy fallback: the old byte guards decide);
//   * ZeroedBuild() when the resolver failed on any other build: the table's Rvas must never be read as if they described an unknown executable.

#include "FieldTable.h"
#include "Gate.h"
#include "ModulePath.h"
#include "../GameBuild.h"

#include <exception>
#include <functional>
#include <string>
#include <string_view>

namespace ext::ffxiv::resolve
{
// ---- the keys ---------------------------------------------------------------------------------------------------------------------------------

// `cfg` is the FFXIV section of the config (ext::Cfg().ffxiv): any type with the three CustomOptional<bool> members. Each key moves only its own flag.
template <class Cfg> GateSettings ReadSettings(const Cfg& cfg)
{
    GateSettings s;
    s.legacyGameBuild = cfg.legacyGameBuild.value_or_default();
    s.unknownBuildHooks = cfg.unknownBuildHooks.value_or_default();
    s.forceUnknownBuild = cfg.forceUnknownBuild.value_or_default();
    return s;
}

// ---- the gate input ------------------------------------------------------------------------------------------------------------------------------

// `stampSizeMatchesKnown`: the running image's PE stamp and size equal the table's (MatchBuild != nullptr). The exact build is identified by that stamp
// and size, whatever the file is called: a copy of the game under another name (a per-exe driver profile, a custom launcher) is the game, an
// unreadable exe path (GetModuleFileNameW failed: an EMPTY path) does not matter, and the old selector never read the name either. The name only
// decides on a build the table does not know: there, another game's exe never counts and an ffxiv_dx11.exe of another build does.
template <class Char> GateInput MakeGateInputT(std::basic_string_view<Char> exePath, bool stampSizeMatchesKnown)
{
    GateInput in;
    in.exeIsFfxiv = stampSizeMatchesKnown || IsFfxivExeNameT<Char>(exePath);
    in.exactKnownBuild = in.exeIsFfxiv && stampSizeMatchesKnown;
    return in;
}
inline GateInput MakeGateInput(std::wstring_view exePath, bool stampSizeMatchesKnown) { return MakeGateInputT<wchar_t>(exePath, stampSizeMatchesKnown); }
inline GateInput MakeGateInput(std::string_view exePath, bool stampSizeMatchesKnown) { return MakeGateInputT<char>(exePath, stampSizeMatchesKnown); }

// ---- the outcome -------------------------------------------------------------------------------------------------------------------------------

struct StartupOutcome
{
    GateReport gate;
    GameBuild build; // what PublishActive gets
};

inline GameBuild PublishedBuild(const GateInput& in, const GateReport& gate)
{
    // the old rules: the rollback key, and the exact known build whose resolver failed (the legacy decision: the compile-time table and the old
    // byte guards)
    if (gate.legacy || gate.legacyFallback)
        return kCurrentBuild;
    // the startup itself threw on any other build: nothing is known, so no table address may look like one
    if (!in.resolverFailure.empty())
        return ZeroedBuild();
    // a process that is not the game: nothing runs there (as before)
    if (!in.exeIsFfxiv)
        return kCurrentBuild;
    // the resolver failed on any other build
    if (in.rb == nullptr || !ResolverInfrastructureFailure(in.rb).empty())
        return ZeroedBuild();
    return in.rb->build;
}

inline StartupOutcome DecideStartup(const GateInput& in, const GateSettings& settings)
{
    StartupOutcome out;
    out.gate = Decide(in, settings);
    out.build = PublishedBuild(in, out.gate);
    return out;
}

// The startup itself threw (outside the resolver, whose own failures are cached as an all-NotFound result): the same rules as any resolver failure.
inline StartupOutcome FailedStartup(GateInput in, const GateSettings& settings, const std::string& what)
{
    in.rb = nullptr;
    in.resolverFailure = "startup exception: " + what;
    return DecideStartup(in, settings);
}

// ---- the environment of one startup ----------------------------------------------------------------------------------------------------------

struct StartupLog
{
    const GateReport* gate = nullptr;
    const ResolvedBuild* rb = nullptr;     // nullptr: no resolve ran or it produced nothing
    const Registry* registry = nullptr;    // never null
    std::uint32_t stamp = 0, size = 0;
    double resolveMs = 0.0;
    std::string exe;                       // the file name only
    std::string failure;                   // non-empty: the startup threw (what())
};

struct StartupEnv
{
    std::function<GateSettings()> readSettings;
    std::function<std::wstring()> exePath;                // the full path of the running executable, empty when it cannot be read
    std::function<bool()> stampSizeMatchesKnown;          // the running image has the table's PE stamp and size
    std::function<const Registry&()> registry;            // built on first use: only called when a resolve runs
    // Opens the image and resolves it once. `table` is the known table offered to the resolver (nullptr: none). nullptr result: the image could not be read.
    std::function<const ResolvedBuild*(const GameBuild* table, double* resolveMs)> resolve;
    std::function<void(const StartupLog&)> log;
    std::function<void(const GameBuild&, const GateReport&)> publish;
};

inline void RunStartup(const StartupEnv& env)
{
    static const Registry noRegistry {};
    GateSettings settings;
    GateInput in;
    std::string exe;
    const ResolvedBuild* rb = nullptr;
    const Registry* registry = nullptr;
    double ms = 0.0;
    std::string failure;
    bool threw = false; // the startup threw: the decision depends on THIS, never on the text (a custom exception type may have an empty what())
    StartupOutcome out;
    try
    {
        settings = env.readSettings();
        const std::wstring path = env.exePath();
        in = MakeGateInput(path, env.stampSizeMatchesKnown());
        exe = FileNameOf(NarrowAscii(path));
        // Not the game, or the rollback key: no image is opened, nothing is scanned, no registry is built. The compile-time table stays the build.
        if (in.exeIsFfxiv && !settings.legacyGameBuild)
        {
            registry = &env.registry();
            in.registry = registry;
            // Offering the table is safe: the resolver uses it only when its stamp AND size equal the running image's. The debug key withholds it, so
            // the whole patch-day path (resolver values only, nothing filled from the table) can be played on today's executable.
            rb = env.resolve(settings.forceUnknownBuild ? nullptr : &kCurrentBuild, &ms);
            in.rb = rb;
        }
        out = DecideStartup(in, settings);
    }
    catch (const std::exception& e)
    {
        threw = true;
        const char* what = e.what();
        failure = what != nullptr ? what : "";
    }
    catch (...)
    {
        threw = true;
        failure = "unknown error";
    }
    if (threw)
    {
        if (failure.empty())
            failure = "exception without a message"; // the fallback reason and the log line always carry a text
        rb = nullptr;
        out = FailedStartup(in, settings, failure);
    }

    // the log never changes the decision
    try
    {
        StartupLog entry;
        entry.gate = &out.gate;
        entry.rb = rb;
        entry.registry = registry != nullptr ? registry : &noRegistry;
        entry.stamp = rb != nullptr ? rb->timeDateStamp : (in.exactKnownBuild ? kCurrentBuild.timeDateStamp : 0u);
        entry.size = rb != nullptr ? rb->sizeOfImage : (in.exactKnownBuild ? kCurrentBuild.sizeOfImage : 0u);
        entry.resolveMs = ms;
        entry.exe = exe;
        entry.failure = failure;
        if (in.exeIsFfxiv || threw)
            env.log(entry);
    }
    catch (...)
    {
    }
    try
    {
        env.publish(out.build, out.gate);
    }
    catch (...)
    {
    }
}
} // namespace ext::ffxiv::resolve
