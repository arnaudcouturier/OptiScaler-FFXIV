#pragma once
// The result types of the gate (Gate.h): which feature, a decision with its reason, the report one startup produces.
//
// Split from Gate.h so the consumers (ActiveBuild.h) can name a decision without pulling the registry and its string literals into every
// translation unit. Header-only, no <windows.h>, no logging macros, no namespace-scope object with a constructor.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ext::ffxiv::resolve
{
// Group 1: identifies the game by exe name or shader CRC and reads no address of the executable. Runs on ANY ffxiv_dx11.exe.
// Group 2: reads or hooks exe addresses. Known build: as before. Unknown build: only with [FFXIV] FfxivUnknownBuildHooks and usable seams.
enum class Feature : std::uint8_t
{
    // Group 1
    SceneHdr,
    Lighting,
    UiHudless,
    Geometry,
    Markers,
    MipBind,
    QuirkIsolatesShutdown,
    QuirkBridgedNativeDx11,
    CatalogHealth,
    // Group 2
    QualityRead,     // the native DLAA gate: reads the game's settings struct (no hook)
    QualityHooks,    // the two Detours (query size, renderer update), the direct resize call and the 8-byte display-size cache write
    JitterProbe,     // read-only camera jitter object probe (and the correction that rides on it)
    JitterSource,    // the two Detours of the source-level jitter fix
    CharaViewAccessor,
    LatencyTick,     // the vtable-slot lease of the game's simulation tick (the probe; nothing else depends on it)
    Count
};

inline constexpr std::size_t kFeatureCount = static_cast<std::size_t>(Feature::Count);
inline constexpr Feature kFirstGroup2Feature = Feature::QualityRead;

inline bool IsGroup2(Feature f) { return static_cast<std::uint8_t>(f) >= static_cast<std::uint8_t>(kFirstGroup2Feature); }

inline const char* FeatureName(Feature f)
{
    switch (f)
    {
    case Feature::SceneHdr:
        return "sceneHdr";
    case Feature::Lighting:
        return "lighting";
    case Feature::UiHudless:
        return "uiHudless";
    case Feature::Geometry:
        return "geometry";
    case Feature::Markers:
        return "markers";
    case Feature::MipBind:
        return "mipBind";
    case Feature::QuirkIsolatesShutdown:
        return "quirkIsolatesShutdown";
    case Feature::QuirkBridgedNativeDx11:
        return "quirkBridgedNativeDx11";
    case Feature::CatalogHealth:
        return "catalogHealth";
    case Feature::QualityRead:
        return "qualityRead";
    case Feature::QualityHooks:
        return "qualityHooks";
    case Feature::JitterProbe:
        return "jitterProbe";
    case Feature::JitterSource:
        return "jitterSource";
    case Feature::CharaViewAccessor:
        return "charaViewAccessor";
    case Feature::LatencyTick:
        return "latencyTick";
    case Feature::Count:
        break;
    }
    return "unknown";
}

struct Decision
{
    bool enabled = false;
    std::string reason; // always set: why it is on or off (bounded: the first failures, never a dump of every seam)
};

// Which of the simulation-tick lease's telemetry offsets resolved (GateReport::latencyOffsetsResolved). Each bit says the seam named after it is
// usable (or, on the known build, filled from the table), so its value may be read; a clear bit means the value is not a measurement of this
// executable and the probe must not read it. The offsets are NOT required seams: none of them can turn the feature off.
inline constexpr std::uint8_t kLatencyFrameCounterResolved = 1u;
inline constexpr std::uint8_t kLatencyPerfValueResolved = 2u;
inline constexpr std::uint8_t kLatencyPerfFreqResolved = 4u;
inline constexpr std::uint8_t kLatencyAllOffsetsResolved =
    kLatencyFrameCounterResolved | kLatencyPerfValueResolved | kLatencyPerfFreqResolved;

// One runtime consistency check between two groups that each derived the same game global (a group may not name another group's seams, so
// the gate compares them). Compared only when both sides are usable; a mismatch turns off the features on both sides.
struct CrossCheckResult
{
    std::string name;     // "chara.managerPointer == jitterSource.rendererPointer"
    bool compared = false; // false: a side was not usable, nothing to compare (the feature of that side is off by its own rule)
    bool agree = true;
    std::uint32_t left = 0, right = 0;
};

struct GateReport
{
    bool started = false;           // false: the report of "startup has not run yet" (every Group 2 feature off)
    bool exeIsFfxiv = false;
    bool legacy = false;            // [FFXIV] FfxivLegacyGameBuild: exact stamp+size, the compile-time table, no resolve
    bool known = false;             // the build is the one in GameBuild.h (legacy: the exact stamp and size; otherwise the resolver's verdict)
    bool forcedUnknown = false;     // [FFXIV] FfxivForceUnknownBuild: the table was ignored
    bool unknownBuildHooks = false; // [FFXIV] FfxivUnknownBuildHooks
    // The resolver itself failed (image unreadable, a cached exception, a scan that aborted) on the EXACT known build: the legacy
    // decision runs for Group 2 (the compile-time table and the old byte guards decide, exactly as under FfxivLegacyGameBuild). `fallbackWhy` says what failed.
    bool legacyFallback = false;
    std::string fallbackWhy {};
    std::array<Decision, kFeatureCount> features {};
    std::vector<CrossCheckResult> crossChecks {};
    // Which latency telemetry offsets resolved (the bits above). 0 when no resolve ran: the lease then installs without reading any of them.
    std::uint8_t latencyOffsetsResolved = 0;

    bool Enabled(Feature f) const { return features[static_cast<std::size_t>(f)].enabled; }
    const std::string& Reason(Feature f) const { return features[static_cast<std::size_t>(f)].reason; }
    Decision& At(Feature f) { return features[static_cast<std::size_t>(f)]; }
};
} // namespace ext::ffxiv::resolve
