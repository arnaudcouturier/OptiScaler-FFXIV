#include "pch.h"

#include "ext/ffxiv/resolve/ResolveStartup.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/ActiveBuild.h"
#include "ext/ffxiv/GameBuild.h"
#include "ext/ffxiv/resolve/Gate.h"
#include "ext/ffxiv/resolve/GateLog.h"
#include "ext/ffxiv/resolve/LiveImage.h"
#include "ext/ffxiv/resolve/ModulePath.h"
#include "ext/ffxiv/resolve/Registry.h"
#include "ext/ffxiv/resolve/ResolveCache.h"
#include "ext/ffxiv/resolve/ResolveLog.h"
#include "ext/ffxiv/resolve/StartupDecision.h"

#include <chrono>
#include <mutex>
#include <string>

// The product call site of the pattern-search resolver (replaces ResolveCompile.cpp of P1/P2). The contract of ResolveOnce (ResolveCache.h):
// once, before the fork installs any Detour over the executable, not under the loader lock, from one thread, never again.
//
// WHEN: StartOnce() is called at the top of ext::game::OnD3D11DeviceHooked, the first moment the fork is given the game's D3D11 device. The
// log of every session (builds\game-logs-*) shows that moment about 8 s after the DLL loaded and before the first Present (about 9.5 s): the
// game's startup thread creating its device, not DllMain (no loader lock) and not a frame-loop thread (no frame exists yet; a one-time pause of
// about 100 to 300 ms in Release, measured in the P1 and P2 golden tests, lands in the loading phase). Everything the fork installs before it
// patches D3D11 vtables or system DLL functions, never the executable's code; the Detours over the executable (companion::Install, a few lines
// below the call in Seams.cpp, and the native-quality / jitter-source hooks, installed lazily by the upscaler's per-evaluate path) all come
// after it. It is not called from the game selector (Game.cpp): Group 1 must not depend on the resolver, and the legacy path must never
// construct a LiveImage.
//
// WHAT: every decision (the keys, the gate input, the table offered to the resolver, the legacy fallback after a resolver failure, which build is
// published) is in StartupDecision.h and unit-tested; this file only supplies the environment of RunStartup: the config, the exe path, the PE stamp
// and size of the running image, the live image and the resolver, the log and the publish.
namespace ext::ffxiv::resolve
{
namespace
{
void LogBlock(const StartupLog& entry)
{
    if (!entry.failure.empty())
        LOG_ERROR("[ffxiv-gate] startup failed ({}); the gate decided without a resolve result", entry.failure);
    for (const GateLine& line :
         FormatGateBlock(*entry.gate, entry.rb, *entry.registry, entry.stamp, entry.size, entry.resolveMs, entry.exe.c_str()))
    {
        switch (line.level)
        {
        case LogLevel::Info:
            LOG_INFO("{}", line.text);
            break;
        case LogLevel::Warn:
            LOG_WARN("{}", line.text);
            break;
        case LogLevel::Error:
            LOG_ERROR("{}", line.text);
            break;
        }
    }
}

const Registry& ProductRegistry()
{
    static const Registry registry = BuildRegistry();
    return registry;
}

StartupEnv MakeProductEnv()
{
    StartupEnv env;
    // The three persistent keys of [FFXIV] (ext/ini/FFXIV.ini), read ONCE here: ext::config::Load ran when the ini was read at DLL start, long
    // before the first device hook. They are latched into the gate report and never read again.
    env.readSettings = [] { return ReadSettings(ext::Cfg().ffxiv); };
    env.exePath = [] { return ReadModulePath([](wchar_t* buffer, std::uint32_t size) { return GetModuleFileNameW(nullptr, buffer, size); }); };
    env.stampSizeMatchesKnown = [] { return MatchCurrentProcess() != nullptr; };
    env.registry = []() -> const Registry& { return ProductRegistry(); };
    env.resolve = [](const GameBuild* table, double* resolveMs) -> const ResolvedBuild*
    {
        ResolveOptions options;
        options.knownTable = table;
        options.sink = MakeDefaultSink();
        const auto image = LiveImage::OpenMainModule();
        if (!image)
            return nullptr; // no line here: the gate block says why (ONE warning on the exact known build, which then runs on the table)
        const auto t0 = std::chrono::steady_clock::now();
        const ResolvedBuild& rb = ResolveOnce(*image, ProductRegistry(), options);
        *resolveMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return &rb;
    };
    env.log = [](const StartupLog& entry) { LogBlock(entry); };
    env.publish = [](const GameBuild& build, const GateReport& gate) { PublishActive(build, gate); };
    return env;
}
} // namespace

void StartOnce()
{
    static std::once_flag once;
    std::call_once(once,
                   []
                   {
                       // RunStartup catches everything itself (a failure becomes the legacy fallback on the exact known build and "every game-address
                       // feature off" anywhere else) and always publishes once; this net is for the construction of the environment.
                       try
                       {
                           static const StartupEnv env = MakeProductEnv();
                           RunStartup(env);
                       }
                       catch (...)
                       {
                           LOG_ERROR("[ffxiv-gate] startup failed before it could decide; every game-address feature stays off");
                           GateReport off = ext::ffxiv::detail::MakeNotStartedReport();
                           off.started = true;
                           for (Decision& d : off.features)
                               d.reason = "the startup failed before it could decide";
                           PublishActive(ZeroedBuild(), off);
                       }
                   });
}
} // namespace ext::ffxiv::resolve
