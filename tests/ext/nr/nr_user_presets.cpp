#include "ext/nr/NrConfig.h"
#include "ext/nr/presets/NrUserPresets.h"
#include "ext/nr/spatial/DlssNr_Spatial.h"
#include <cassert>
#include <cstdlib>
#include <iostream>

// The foveated-profile checks below use this instead of assert, which an NDEBUG build would make vacuous.
static void Require(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::cerr << "FAIL line " << line << ": " << what << "\n";
        std::exit(1);
    }
}
#define REQUIRE(x) Require((x), #x, __LINE__)

// Exercise the real ext::NrConfig members/defaults without loading the game's INI or initializing graphics.

int main(int argc, char** argv)
{
    using namespace DlssNr::UserPresets;
    assert(argc == 2);
    ext::NrConfig c;
    const auto defaults = Capture(c);
    c.enabled = true;
    c.unlockPasses = true;
    c.passes = 30u;
    c.workingScale = 2.0f;
    c.style = 2u;
    c.pass2Intensity = 0.7f;
    c.pass3AutoMask = false;
    c.extraPasses[26].style = 1u;
    c.extraPasses[26].intensity = 1.75f;
    c.spatialCompression = true;
    c.spatialWorkX = 87.0f;
    c.vitEvery = 2u;
    c.passFeedback = 0.5f;
    const auto elaborate = Capture(c);
    Apply(defaults, c);
    assert(Capture(c) == defaults && !c.pass2Intensity.has_value());
    Apply(elaborate, c);
    assert(Capture(c) == elaborate && c.extraPasses[26].intensity.value() == 1.75f);
    assert(!c.extraPasses[25].intensity.has_value());
    c.toggleKey = 123;
    c.holdFrame = true;
    for (const auto& badValue : {Json(-1), Json(1.5), Json("two"), Json(1000)})
    {
        auto broken = defaults;
        broken["Pass30Style"] = badValue;
        bool failed = false;
        try { Apply(broken, c); } catch (...) { failed = true; }
        assert(failed && Capture(c) == elaborate);
    }
    auto missing = elaborate; missing.erase("Pass2Intensity");
    bool rejected = false;
    try { Apply(missing, c); } catch (...) { rejected = true; }
    assert(rejected && Capture(c) == elaborate);
    auto legacy = defaults; legacy.erase("VitEvery"); legacy.erase("PassFeedback");
    for (auto it = legacy.begin(); it != legacy.end();) { if (it.key().starts_with("Spatial")) it = legacy.erase(it); else ++it; }
    Apply(legacy, c);
    assert(!c.spatialCompression.value_or_default() && c.spatialWorkX.value_or_default() == 90.0f);
    assert(c.vitEvery.value_or_default() == 1 && c.passFeedback.value_or_default() == 1.0f);
    assert(c.toggleKey.value() == 123 && c.holdFrame.value());
    assert(Name("  Gameplay  ") == "Gameplay");
    assert(Name("Portrait / 日本語") == "Portrait / 日本語");
    rejected = false;
    try { Name("   "); } catch (...) { rejected = true; }
    assert(rejected);
    const std::filesystem::path path = std::filesystem::path(argv[1]) / L"preset-test.json";
    Json library{{"version", 1}, {"presets", {{"Gameplay", defaults}, {"Portrait / 日本語", elaborate}}}};
    Write(path, library);
    assert(Read(path) == library);
    library["presets"].erase("Gameplay");
    Write(path, library);
    assert(Read(path) == library);
    // An unsuccessful save must leave the existing file untouched.
    auto oversized = library; oversized["padding"] = std::string(1024 * 1024, 'x');
    rejected = false;
    try { Write(path, oversized); } catch (...) { rejected = true; }
    assert(rejected && Read(path) == library);
    std::filesystem::remove(path);
    // Foveated spatial profile keys: defaults equal today's behaviour, they round-trip, bad values are refused, old files get the defaults.
    {
        namespace S = DlssNr::Spatial;
        ext::NrConfig fresh;
        REQUIRE(fresh.spatialProfile.value_or_default() == 0u);
        REQUIRE(fresh.foveaWidth.value_or_default() == 2400.0f && fresh.foveaHeight.value_or_default() == 1350.0f);
        REQUIRE(fresh.foveaFalloff.value_or_default() == 400.0f);
        REQUIRE(fresh.foveaEdgeX.value_or_default() == 0.5f && fresh.foveaEdgeY.value_or_default() == 0.5f);
        REQUIRE(!fresh.spatialCompression.value_or_default()); // the master switch stays off
        // the configuration defaults and the layout defaults are the same numbers
        REQUIRE(S::ReadSettings(fresh) == S::Settings {});
        REQUIRE(fresh.foveaWidth.value_or_default() == DlssNr::Foveated::kDefaultWidth);
        REQUIRE(fresh.foveaHeight.value_or_default() == DlssNr::Foveated::kDefaultHeight);
        REQUIRE(fresh.foveaFalloff.value_or_default() == DlssNr::Foveated::kDefaultFalloff);
        REQUIRE(fresh.foveaEdgeX.value_or_default() == DlssNr::Foveated::kDefaultEdge);
        const auto freshJson = Capture(fresh);
        REQUIRE(freshJson["SpatialProfile"] == 0 && freshJson["FoveaWidth"] == 2400 && freshJson["FoveaHeight"] == 1350);
        REQUIRE(freshJson["FoveaFalloff"] == 400 && freshJson["FoveaEdgeX"] == 0.5 && freshJson["FoveaEdgeY"] == 0.5);
        // a value other than 1 is the peripheral profile
        fresh.spatialProfile = 7u;
        REQUIRE(S::ReadSettings(fresh).profile == S::Profile::Peripheral);
        fresh.spatialProfile = 1u;
        REQUIRE(S::ReadSettings(fresh).profile == S::Profile::Foveated);
        // round trip
        fresh.spatialCompression = true;
        fresh.foveaWidth = 2000.0f;
        fresh.foveaHeight = 900.0f;
        fresh.foveaFalloff = 250.0f;
        fresh.foveaEdgeX = 0.4f;
        fresh.foveaEdgeY = 0.75f;
        const auto saved = Capture(fresh);
        ext::NrConfig other;
        Apply(saved, other);
        REQUIRE(Capture(other) == saved);
        REQUIRE(other.spatialProfile.value() == 1u && other.foveaWidth.value() == 2000.0f && other.foveaHeight.value() == 900.0f);
        REQUIRE(other.foveaFalloff.value() == 250.0f && other.foveaEdgeX.value() == 0.4f && other.foveaEdgeY.value() == 0.75f);
        REQUIRE(S::ReadSettings(other) == S::ReadSettings(fresh));
        // the edges of every range are accepted, anything past them is refused and nothing changes
        auto edges = saved;
        edges["SpatialProfile"] = 1; edges["FoveaWidth"] = 16384; edges["FoveaHeight"] = 0; edges["FoveaFalloff"] = 0;
        edges["FoveaEdgeX"] = 0.25; edges["FoveaEdgeY"] = 1.0;
        Apply(edges, other);
        REQUIRE(Capture(other) == edges);
        const std::pair<const char*, Json> bad[] = { { "SpatialProfile", 2 }, { "SpatialProfile", -1 }, { "SpatialProfile", 0.5 },
                                                      { "FoveaWidth", -1 }, { "FoveaWidth", 16385 }, { "FoveaHeight", -0.5 },
                                                      { "FoveaFalloff", 16385 }, { "FoveaEdgeX", 0.2499 }, { "FoveaEdgeX", 1.01 },
                                                      { "FoveaEdgeY", 0.0 }, { "FoveaEdgeY", 1.5 }, { "FoveaWidth", "wide" },
                                                      { "FoveaEdgeY", true } };
        for (const auto& [key, value] : bad)
        {
            auto broken = saved;
            broken[key] = value;
            bool refused = false;
            try { Apply(broken, other); } catch (...) { refused = true; }
            REQUIRE(refused && Capture(other) == edges);
        }
        // a file written before these keys existed gets the defaults (and the other spatial keys of that file are kept)
        auto older = saved;
        for (auto it = older.begin(); it != older.end();)
        {
            if (it.key().starts_with("Spatial") || it.key().starts_with("Fovea")) it = older.erase(it); else ++it;
        }
        older["SpatialCompression"] = true;
        older["SpatialCenterX"] = 70.0;
        ext::NrConfig upgraded;
        Apply(older, upgraded);
        REQUIRE(upgraded.spatialProfile.value() == 0u && upgraded.foveaWidth.value() == 2400.0f && upgraded.foveaHeight.value() == 1350.0f);
        REQUIRE(upgraded.foveaFalloff.value() == 400.0f && upgraded.foveaEdgeX.value() == 0.5f && upgraded.foveaEdgeY.value() == 0.5f);
        REQUIRE(upgraded.spatialCompression.value() && upgraded.spatialCenterX.value() == 70.0f);
        // a file that has some of them keeps them
        auto partial = older;
        partial["FoveaWidth"] = 1800.0;
        Apply(partial, upgraded);
        REQUIRE(upgraded.foveaWidth.value() == 1800.0f && upgraded.foveaHeight.value() == 1350.0f && upgraded.spatialProfile.value() == 0u);
    }
    // Per-axis model resolution: 0 = follow WorkingScale (today), 0.25..2 otherwise; saved in the presets, old files get 0.
    {
        ext::NrConfig fresh;
        REQUIRE(fresh.workingScaleX.value_or_default() == 0.0f && fresh.workingScaleY.value_or_default() == 0.0f);
        REQUIRE(!fresh.depthAwareResolve.value_or_default() && Capture(fresh)["DepthAwareResolve"] == false);
        const auto freshJson = Capture(fresh);
        REQUIRE(freshJson["WorkingScaleX"] == 0 && freshJson["WorkingScaleY"] == 0);
        fresh.workingScale = 0.8f;
        fresh.workingScaleX = 0.6f;
        fresh.workingScaleY = 1.25f;
        const auto saved = Capture(fresh);
        ext::NrConfig other;
        Apply(saved, other);
        REQUIRE(Capture(other) == saved && other.workingScaleX.value() == 0.6f && other.workingScaleY.value() == 1.25f);
        REQUIRE(other.workingScale.value() == 0.8f);
        auto edges = saved;
        edges["WorkingScaleX"] = 0; edges["WorkingScaleY"] = 2.0;
        Apply(edges, other);
        REQUIRE(Capture(other) == edges);
        // ... and the other way round: the horizontal key reaches 2.0 as well, the vertical one 0
        auto swappedEdges = saved;
        swappedEdges["WorkingScaleX"] = 2.0; swappedEdges["WorkingScaleY"] = 0;
        Apply(swappedEdges, other);
        REQUIRE(Capture(other) == swappedEdges && other.workingScaleX.value() == 2.0f && other.workingScaleY.value() == 0.0f);
        Apply(edges, other);
        REQUIRE(Capture(other) == edges);
        const std::pair<const char*, Json> bad[] = { { "WorkingScaleX", -0.01 }, { "WorkingScaleX", 2.01 }, { "WorkingScaleY", -1 },
                                                      { "WorkingScaleY", 3 }, { "WorkingScaleX", "half" }, { "WorkingScaleY", true } };
        for (const auto& [key, value] : bad)
        {
            auto broken = saved;
            broken[key] = value;
            bool refused = false;
            try { Apply(broken, other); } catch (...) { refused = true; }
            REQUIRE(refused && Capture(other) == edges);
        }
        fresh.depthAwareResolve = true;
        {
            const auto withDepth = Capture(fresh);
            ext::NrConfig third;
            Apply(withDepth, third);
            REQUIRE(third.depthAwareResolve.value() && Capture(third) == withDepth);
            auto brokenDepth = withDepth;
            brokenDepth["DepthAwareResolve"] = 2;
            bool refusedDepth = false;
            try { Apply(brokenDepth, third); } catch (...) { refusedDepth = true; }
            REQUIRE(refusedDepth && third.depthAwareResolve.value());
            auto olderDepth = withDepth;
            olderDepth.erase("DepthAwareResolve");
            Apply(olderDepth, third);
            REQUIRE(!third.depthAwareResolve.value());
        }
        // a file written before these keys existed gets 0 (follow) and keeps its WorkingScale
        auto older = saved;
        older.erase("WorkingScaleX"); older.erase("WorkingScaleY");
        ext::NrConfig upgraded;
        Apply(older, upgraded);
        REQUIRE(upgraded.workingScaleX.value() == 0.0f && upgraded.workingScaleY.value() == 0.0f && upgraded.workingScale.value() == 0.8f);
        auto half = older;
        half["WorkingScaleX"] = 0.5;
        Apply(half, upgraded);
        REQUIRE(upgraded.workingScaleX.value() == 0.5f && upgraded.workingScaleY.value() == 0.0f);
    }
    std::cout << "PASS: all-pass round trip, inheritance, atomic validation, NR-only scope, Unicode names, persistence, failed-save retention and the foveated spatial keys\n";
}
