#include "ext/nr/NrConfig.h"
#include "ext/nr/presets/NrUserPresets.h"
#include <cassert>
#include <iostream>

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
    std::cout << "PASS: all-pass round trip, inheritance, atomic validation, NR-only scope, Unicode names, persistence and failed-save retention\n";
}
