// The Classic-enlargement sharpening rules: when the sharpen step may run, what it costs by default,
// and how its replay kind and saved presets behave. The runtime gate in the Direct3D 12 pass reads the
// same shared rule the menu greys the controls by, so the checkbox and the dispatch cannot disagree.
#include "ext/nr/NrHotkeys.h"
#include "ext/nr/NrConfig.h"
#include "ext/nr/diag/NrReplayCapture.h"
#include "ext/nr/presets/NrUserPresets.h"

#include <cmath>
#include <cstdio>

namespace
{
int failures = 0;
void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

using DlssNr::Hotkeys::ClassicApplies;
using DlssNr::Hotkeys::TransferToggleable;
using ext::nr::replay::KindName;

void SharpenAppliesOnlyToClassic()
{
    // a reduced model with Classic selected sharpens; Matched residual keeps the native frame and never sharpens
    CHECK(ClassicApplies(0.5f, 0.5f, 0));
    CHECK(!ClassicApplies(0.5f, 0.5f, 1));
    // a value the menu shows as Classic sharpens like Classic does
    CHECK(ClassicApplies(0.5f, 0.5f, 2));
    CHECK(ClassicApplies(0.5f, 0.5f, 3));
}

void SharpenAppliesOnlyWhileReduced()
{
    // at full size the two enlargement modes are identical, so there is nothing to sharpen
    CHECK(!ClassicApplies(1.0f, 1.0f, 0));
    // reduced on either axis alone is enough, matching the menu combo's grey rule
    CHECK(ClassicApplies(1.0f, 0.5f, 0));
    CHECK(ClassicApplies(0.5f, 1.0f, 0));
    // above full on any axis skips: supersampling composites its down-legged answer at native
    CHECK(!ClassicApplies(1.5f, 1.5f, 0));
    CHECK(!ClassicApplies(1.5f, 0.5f, 0));
    CHECK(!ClassicApplies(0.5f, 1.5f, 0));
    // the mixed above/below case the pass raises to full: the other axis is at full, so nothing applies
    CHECK(!ClassicApplies(2.0f, 0.25f, 2));
}

void SharpenThresholdMatchesTheMenu()
{
    // the same 0.999 boundary the Enlargement combo greys out at
    CHECK(!ClassicApplies(0.999f, 0.999f, 0));
    CHECK(ClassicApplies(0.998f, 1.0f, 0));
    CHECK(ClassicApplies(1.0f, 0.998f, 0));
    // the shared rule agrees with the toggle rule on every input: Classic applies exactly when the
    // combo is interactive and the mode is not Matched residual
    const float scales[][2] = { { 0.25f, 0.25f }, { 0.5f, 1.0f }, { 1.0f, 1.0f }, { 0.999f, 1.0f },
                                { 0.998f, 1.0f }, { 1.5f, 0.5f }, { 2.0f, 2.0f }, { 0.8f, 1.2f } };
    for (const auto& s : scales)
        for (uint32_t transfer = 0; transfer <= 3; ++transfer)
            CHECK(ClassicApplies(s[0], s[1], transfer) == (transfer != 1 && TransferToggleable(s[0], s[1])));
}

void SharpenKindNameComesFromTheShader()
{
    // the sharpen dispatch runs its own shader, not a mode of the main one, so the shader id alone
    // decides the replay kind whatever mode number the constants carry
    CHECK(KindName("classic_sharpen", 0) == "classic_sharpen");
    CHECK(KindName("classic_sharpen", 1) == "classic_sharpen");
    CHECK(KindName("classic_sharpen", 15) == "classic_sharpen");
    CHECK(KindName("classic_sharpen", 99) == "classic_sharpen");
    // the main shader keeps its own numbering: the sharpen mode number is not a resolve kind
    CHECK(KindName("dlssnr", 1) == "resolve");
    CHECK(KindName("dlssnr", 15) == "mode_15");
    CHECK(KindName("guide_match", 14) == "guide_match");
}

void SharpenDefaultsKeepTodayBehaviour()
{
    // off by default, so a frame with the key untouched is byte-identical to the build without the step;
    // the default strength is the filter's own upstream default
    const ext::NrConfig fresh;
    CHECK(!fresh.classicSharpen.value_or_default());
    CHECK(fresh.classicSharpenStrength.value_or_default() == 0.4f);
}

void SharpenPresetsRoundTrip()
{
    using namespace DlssNr::UserPresets;
    ext::NrConfig c;
    const auto defaults = Capture(c);
    CHECK(defaults["ClassicSharpen"] == false && defaults["ClassicSharpenStrength"].get<float>() == 0.4f);
    c.classicSharpen = true;
    c.classicSharpenStrength = 0.7f;
    const auto saved = Capture(c);
    CHECK(saved["ClassicSharpen"] == true && saved["ClassicSharpenStrength"].get<float>() == 0.7f);
    ext::NrConfig other;
    Apply(saved, other);
    CHECK(Capture(other) == saved && other.classicSharpen.value() && other.classicSharpenStrength.value() == 0.7f);
    // the edges of the strength range are accepted
    auto edges = saved;
    edges["ClassicSharpenStrength"] = 0.0;
    Apply(edges, other);
    CHECK(Capture(other) == edges);
    edges["ClassicSharpenStrength"] = 1.0;
    Apply(edges, other);
    CHECK(Capture(other) == edges);
    // a file written before these keys existed loads with the off behaviour and the default strength
    auto older = saved;
    older.erase("ClassicSharpen");
    older.erase("ClassicSharpenStrength");
    ext::NrConfig upgraded;
    Apply(older, upgraded);
    CHECK(!upgraded.classicSharpen.value() && upgraded.classicSharpenStrength.value() == 0.4f);
    // a file that has one of them keeps it
    auto partial = older;
    partial["ClassicSharpen"] = true;
    Apply(partial, upgraded);
    CHECK(upgraded.classicSharpen.value() && upgraded.classicSharpenStrength.value() == 0.4f);
}

void SharpenPresetsRejectOutOfRange()
{
    using namespace DlssNr::UserPresets;
    ext::NrConfig c;
    c.classicSharpen = true;
    c.classicSharpenStrength = 0.7f;
    const auto saved = Capture(c);
    const std::pair<const char*, Json> bad[] = { { "ClassicSharpen", 2 }, { "ClassicSharpen", "yes" },
                                                 { "ClassicSharpenStrength", 1.5 }, { "ClassicSharpenStrength", -0.1 },
                                                 { "ClassicSharpenStrength", "half" }, { "ClassicSharpenStrength", true } };
    for (const auto& [key, value] : bad)
    {
        auto broken = saved;
        broken[key] = value;
        bool refused = false;
        try { Apply(broken, c); } catch (...) { refused = true; }
        CHECK(refused && Capture(c) == saved);
    }
}
} // namespace

int main()
{
    SharpenAppliesOnlyToClassic();
    SharpenAppliesOnlyWhileReduced();
    SharpenThresholdMatchesTheMenu();
    SharpenKindNameComesFromTheShader();
    SharpenDefaultsKeepTodayBehaviour();
    SharpenPresetsRoundTrip();
    SharpenPresetsRejectOutOfRange();
    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: Classic sharpening rules (shared Classic-applies rule, replay kind, defaults, preset round trip and range checks)");
    return 0;
}
