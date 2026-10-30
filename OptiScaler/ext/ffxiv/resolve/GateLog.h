#pragma once
// The log block the startup writes after the resolver's own per-seam lines: build identity and verdict, one line per feature with its reason,
// the four cross-group checks, the "not self-healing" line. Pure (returns the lines), so a test pins the format and the bounded volume.
//
//   [ffxiv-gate] build stamp=0x6ad10bfd size=0x381f000 known=yes forced_unknown=no legacy=no unknown_build_hooks=off exe=ffxiv_dx11.exe resolve_ms=97.123
//   [ffxiv-gate] feature qualityHooks ON  all 41 required checks pass
//   [ffxiv-gate] feature jitterSource OFF unknown build and FfxivUnknownBuildHooks is off (...)
//   [ffxiv-gate] cross-check chara.managerPointer == jitterSource.rendererPointer: agree (0x290e490)
//   [ffxiv-gate] not self-healing: ...
//
// Level: Info for an enabled feature, an agreeing check and a known-build summary; Warn for a disabled Group 2 feature (known or unknown build:
// a disabled feature on the known build is unexpected and wants a line a player can send), a mismatch, a not-compared check and the
// not-self-healing line. Group 1 features are all on or all off together and are summarised in one line when they agree.

#include "Gate.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace ext::ffxiv::resolve
{
struct GateLine
{
    LogLevel level;
    std::string text;
};

// `rb` is the resolver's result, or nullptr when no resolve ran (legacy, a failure). `resolveMs` is the wall time of ResolveOnce (the resolver also
// prints its own scan and total times in its `total` line).
inline std::vector<GateLine> FormatGateBlock(const GateReport& g, const ResolvedBuild* rb, const Registry& registry, std::uint32_t stamp,
                                             std::uint32_t size, double resolveMs, const char* exeName)
{
    std::vector<GateLine> out;
    char head[320];
    std::snprintf(head, sizeof(head),
                  "[ffxiv-gate] build stamp=0x%x size=0x%x known=%s forced_unknown=%s legacy=%s unknown_build_hooks=%s exe=%s resolve_ms=%.3f", stamp, size,
                  g.known ? "yes" : "no", g.forcedUnknown ? "yes" : "no", g.legacy ? "yes" : "no", g.unknownBuildHooks ? "on" : "off", exeName,
                  resolveMs);
    out.push_back({ LogLevel::Info, head });
    // ONE warning when the resolver itself failed on the exact known build and the legacy decision took over
    if (g.legacyFallback)
        out.push_back({ LogLevel::Warn, "[ffxiv-gate] resolver infrastructure failure on the exact known build (" + g.fallbackWhy +
                                            "): falling back to the legacy decision, the compile-time table and the old byte guards decide as with FfxivLegacyGameBuild" });

    // Group 1: one line when they agree (they always do: the same rule), else one line each
    bool allSame = true;
    for (std::size_t i = 1; i < static_cast<std::size_t>(kFirstGroup2Feature); ++i)
        allSame = allSame && g.features[i].enabled == g.features[0].enabled;
    if (allSame)
    {
        std::string names;
        for (std::size_t i = 0; i < static_cast<std::size_t>(kFirstGroup2Feature); ++i)
            names += (i ? "," : "") + std::string(FeatureName(static_cast<Feature>(i)));
        out.push_back({ g.features[0].enabled ? LogLevel::Info : LogLevel::Warn,
                        std::string("[ffxiv-gate] group1 ") + (g.features[0].enabled ? "ON " : "OFF ") + "(" + names + "): " + g.features[0].reason });
    }
    else
    {
        for (std::size_t i = 0; i < static_cast<std::size_t>(kFirstGroup2Feature); ++i)
            out.push_back({ g.features[i].enabled ? LogLevel::Info : LogLevel::Warn,
                            std::string("[ffxiv-gate] feature ") + FeatureName(static_cast<Feature>(i)) + (g.features[i].enabled ? " ON " : " OFF ") +
                                g.features[i].reason });
    }
    for (std::size_t i = static_cast<std::size_t>(kFirstGroup2Feature); i < kFeatureCount; ++i)
        out.push_back({ g.features[i].enabled ? LogLevel::Info : LogLevel::Warn,
                        std::string("[ffxiv-gate] feature ") + FeatureName(static_cast<Feature>(i)) + (g.features[i].enabled ? " ON " : " OFF ") +
                            g.features[i].reason });

    for (const CrossCheckResult& c : g.crossChecks)
    {
        char buf[200];
        if (!c.compared)
            std::snprintf(buf, sizeof(buf), ": not compared (a side is not usable)");
        else if (c.agree)
            std::snprintf(buf, sizeof(buf), ": agree (0x%x)", c.left);
        else
            std::snprintf(buf, sizeof(buf), ": MISMATCH (0x%x vs 0x%x)", c.left, c.right);
        out.push_back({ c.compared && c.agree ? LogLevel::Info : LogLevel::Warn, "[ffxiv-gate] cross-check " + c.name + buf });
    }

    // only where a resolve ran and the gate does not call the build known, or the legacy fallback runs on the exact known build: the fixed offsets are a risk the player should be able to read. The verdict is the gate's, not the failed result's, so a cached resolver exception on the exact known build still reads as the known build the legacy fallback runs on.
    if (rb != nullptr && (!g.known || g.legacyFallback))
        out.push_back({ LogLevel::Warn, FormatNotSelfHealing(*rb, registry, g.known) });
    return out;
}
} // namespace ext::ffxiv::resolve
