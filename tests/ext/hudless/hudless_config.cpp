// S10 package B: the [HudLess] config binder (docs/HUDLESS.md section 11). Defaults, key names and order, ini round-trip,
// the clamps of the advanced keys, the case-insensitive Mode parse with "unknown -> off", and the published state
// (cached mode, shared Wanted flag, seam feature bit).
#include "ext/ExtFeatures.h"
#include "ext/core/ExtConfig.h"
#include "ext/hudless/HudlessConfig.h"
#include "ext/hudless/HudlessState.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

int main()
{
    using namespace ext;
    using hudless::Mode;

    HudlessConfig defaults;
    assert(defaults.GetMode() == Mode::Off);
    assert(defaults.mode.value_or_default() == "off");
    assert(defaults.maskThreshold.value_or_default() == 0.008f);
    assert(defaults.maskDilation.value_or_default() == 1);
    assert(defaults.pasteCleanup.value_or_default());
    assert(!defaults.steadyTiming.value_or_default());
    assert(defaults.coverageGuard.value_or_default() == 0.60f);
    assert(!defaults.showDetectedUi.value_or_default());
    assert(defaults.Threshold() == 0.008f && defaults.Dilation() == 1 && defaults.CoverageLimit() == 0.60f);
    assert(defaults.hdrPaste.value_or_default() == "off" && !defaults.HdrPasteOn()); // the HDR paste is opt-in: untried in a game
    assert(defaults.guardMode.value_or_default() == "legacy" && defaults.GetGuardPolicy() == hudless::GuardPolicy::Legacy);
    // the two density-guard options are off by default (today's rule)
    assert(defaults.guardReleaseHolds.value_or_default() == 0 && defaults.ReleaseHolds() == 0);
    assert(!defaults.guardTripOnAnchorLater.value_or_default() && !defaults.TripOnAnchorLater());
    // a tripped guard probes every present by default (today's rule), and the blind A/B of the interval is off
    assert(defaults.probeInterval.value_or_default() == 1 && defaults.ProbeInterval() == 1);
    assert(defaults.probeAbSeconds.value_or_default() == 0 && defaults.ProbeAbSeconds() == 0);
    // the capture is taken at R1b by default (today's rule); the early anchor is opt-in
    assert(defaults.anchorRule.value_or_default() == "r1b" && defaults.GetAnchorRule() == hudless::AnchorRule::R1b);
    // the developer probes are off by default (and the seam bit that serves them starts clear)
    assert(!defaults.plateDiagnostics.value_or_default());
    assert(!ext::Active(F::PlateProbe));
    // native nameplates are off by default: the switch, the shift constant and the step cap
    assert(defaults.plateInterp.value_or_default() == "off" && defaults.GetPlateInterp() == hudless::PlateInterp::Off);
    assert(defaults.platePhaseOffset.value_or_default() == "auto" && defaults.GetPlatePhase() == 1);
    assert(defaults.plateMaxStep.value_or_default() == 24 && defaults.GetPlateStep() == 24);
    assert(!ext::Active(F::Plate));

    // Exactly the documented keys of section 11 (Diagnostics is bound by package A), in the documented order.
    config::Binder binder;
    defaults.Bind(binder);
    const std::vector<std::string> expectedKeys { "Mode",         "MaskThreshold", "MaskDilation", "PasteCleanup",
                                                  "SteadyTiming", "CoverageGuard", "ShowDetectedUi", "GuardMode", "GuardReleaseHolds",
                                                  "GuardTripOnAnchorLater", "ProbeInterval", "ProbeAbSeconds", "AnchorRule", "HdrPaste", "PlateDiagnostics",
                                                  "PlateInterp", "PlatePhaseOffset", "PlateMaxStep" };
    assert(binder.Table().size() == expectedKeys.size());
    for (size_t i = 0; i < expectedKeys.size(); ++i)
    {
        assert(std::string(binder.Table()[i].section) == "HudLess");
        assert(std::string(binder.Table()[i].key) == expectedKeys[i]);
    }
    for (const auto& entry : binder.Table())
        assert(std::string(entry.key) != "Diagnostics" && std::string(entry.key).rfind("ExternalUI", 0) != 0);

    // Mode parse: case-insensitive, whitespace tolerant, every unknown string is off.
    assert(hudless::ParseMode("off") == Mode::Off);
    assert(hudless::ParseMode("recomposition") == Mode::Recomposition);
    assert(hudless::ParseMode("Recomposition") == Mode::Recomposition);
    assert(hudless::ParseMode("ui_free") == Mode::UiFree);
    assert(hudless::ParseMode("UI_FREE") == Mode::UiFree);
    assert(hudless::ParseMode("  ui_free \t") == Mode::UiFree);
    assert(hudless::ParseMode("") == Mode::Off);
    assert(hudless::ParseMode("auto") == Mode::Off);
    assert(hudless::ParseMode("ui-free") == Mode::Off);
    assert(hudless::ParseMode("true") == Mode::Off);
    assert(hudless::ParseMode("2") == Mode::Off);
    assert(std::string(hudless::ModeName(Mode::Off)) == "off");
    assert(std::string(hudless::ModeName(Mode::Recomposition)) == "recomposition");
    assert(std::string(hudless::ModeName(Mode::UiFree)) == "ui_free");

    // HdrPaste: only "on" switches the HDR paste path on; every other text (and the ini value auto) is off, fail open
    assert(hudless::ParseHdrPaste("on") && hudless::ParseHdrPaste("ON") && hudless::ParseHdrPaste("  On \t"));
    for (const char* text : { "off", "auto", "", "true", "1", "yes", "enabled", "onn", "of" })
        assert(!hudless::ParseHdrPaste(text));

    // AnchorRule: only "early" (case-insensitive, whitespace tolerant) moves the capture; every other text is r1b, fail open
    assert(hudless::ParseAnchorRule("early") == hudless::AnchorRule::Early);
    assert(hudless::ParseAnchorRule("EARLY") == hudless::AnchorRule::Early);
    assert(hudless::ParseAnchorRule("  Early \t") == hudless::AnchorRule::Early);
    for (const char* text : { "r1b", "R1B", "auto", "", "earlyy", "earl", "r1", "true", "1", "on", "late" })
        assert(hudless::ParseAnchorRule(text) == hudless::AnchorRule::R1b);
    assert(std::string(hudless::AnchorRuleName(hudless::AnchorRule::R1b)) == "r1b");
    assert(std::string(hudless::AnchorRuleName(hudless::AnchorRule::Early)) == "early");

    // Nothing is published before the config loads.
    assert(hudless::CurrentMode() == Mode::Off);
    assert(!hudless::Wanted());

    // Load: values, clamps. 5 is above the dilation range, 7.5 above the threshold range.
    CSimpleIniA ini;
    ini.SetValue("HudLess", "Mode", "UI_Free");
    ini.SetValue("HudLess", "MaskThreshold", "7.5");
    ini.SetValue("HudLess", "MaskDilation", "5");
    ini.SetValue("HudLess", "PasteCleanup", "false");
    ini.SetValue("HudLess", "SteadyTiming", "true");
    ini.SetValue("HudLess", "CoverageGuard", "0.25");
    ini.SetValue("HudLess", "ShowDetectedUi", "true");
    ini.SetValue("HudLess", "HdrPaste", "On");
    ini.SetValue("HudLess", "GuardMode", "Density");
    ini.SetValue("HudLess", "GuardReleaseHolds", "4");
    ini.SetValue("HudLess", "GuardTripOnAnchorLater", "true");
    ini.SetValue("HudLess", "ProbeInterval", "8");
    ini.SetValue("HudLess", "ProbeAbSeconds", "60");
    ini.SetValue("HudLess", "AnchorRule", "Early");
    ini.SetValue("FrameGen", "ExternalUIFreeFrameGen", "false"); // the old keys are not read
    config::Load(ini);

    auto& cfg = Cfg().hudless;
    assert(cfg.GetMode() == Mode::UiFree);
    assert(cfg.maskThreshold.value_or_default() == 7.5f && cfg.Threshold() == 1.0f);
    assert(cfg.maskDilation.value_or_default() == 5 && cfg.Dilation() == 4);
    assert(!cfg.pasteCleanup.value_or_default());
    assert(cfg.steadyTiming.value_or_default());
    assert(std::abs(cfg.CoverageLimit() - 0.25f) < 1e-6f);
    assert(cfg.showDetectedUi.value_or_default());
    assert(cfg.HdrPasteOn());
    assert(cfg.GetGuardPolicy() == hudless::GuardPolicy::Density);
    assert(cfg.ReleaseHolds() == 4 && cfg.TripOnAnchorLater());
    assert(cfg.ProbeInterval() == 8 && cfg.ProbeAbSeconds() == 60);
    assert(cfg.GetAnchorRule() == hudless::AnchorRule::Early);
    assert(hudless::CurrentMode() == Mode::UiFree);
    assert(hudless::Wanted());
    assert(ext::Active(ext::F::Hudless));

    // Save round-trips the values; the old keys are not written.
    CSimpleIniA out;
    config::Save(out);
    assert(std::string(out.GetValue("HudLess", "Mode", "")) == "UI_Free");
    assert(std::string(out.GetValue("HudLess", "SteadyTiming", "")) == "true");
    assert(std::string(out.GetValue("HudLess", "PasteCleanup", "")) == "false");
    assert(std::string(out.GetValue("HudLess", "GuardReleaseHolds", "")) == "4");
    assert(std::string(out.GetValue("HudLess", "GuardTripOnAnchorLater", "")) == "true");
    assert(std::string(out.GetValue("HudLess", "ProbeInterval", "")) == "8");
    assert(std::string(out.GetValue("HudLess", "ProbeAbSeconds", "")) == "60");
    assert(std::string(out.GetValue("HudLess", "AnchorRule", "")) == "Early"); // the stored text is not rewritten
    assert(out.GetValue("FrameGen", "ExternalUIFreeFrameGen", nullptr) == nullptr);
    // [HudLess] Diagnostics is bound by package A's UiConfig (ext/ffxiv/ui) in the same table; HudlessConfig does not own it,
    // but once both packages are stacked the key is part of the saved section.
    assert(out.GetValue("HudLess", "Diagnostics", nullptr) != nullptr);

    // Menu path: SetMode rewrites the key and republishes everything.
    hudless::SetMode(Mode::Recomposition);
    assert(hudless::CurrentMode() == Mode::Recomposition && hudless::Wanted());
    assert(Cfg().hudless.mode.value_or_default() == "recomposition");
    hudless::SetMode(Mode::Off);
    assert(hudless::CurrentMode() == Mode::Off && !hudless::Wanted());
    assert(Cfg().hudless.mode.value_or_default() == "off");

    // Lower clamp, non-finite and negative input.
    cfg.maskThreshold = 0.0f;
    cfg.maskDilation = -3;
    cfg.coverageGuard = -1.0f;
    assert(cfg.Threshold() == 0.001f && cfg.Dilation() == 0 && cfg.CoverageLimit() == 0.0f);
    cfg.maskThreshold = std::nanf("");
    cfg.coverageGuard = std::nanf("");
    assert(cfg.Threshold() == 0.008f && cfg.CoverageLimit() == 0.60f);
    cfg.coverageGuard = 3.0f;
    assert(cfg.CoverageLimit() == 1.0f);

    // the guard options clamp the way the guard does: negative is off, above one summary window is the window
    cfg.guardReleaseHolds = -5;
    assert(cfg.ReleaseHolds() == 0);
    cfg.guardReleaseHolds = 100000;
    assert(cfg.ReleaseHolds() == hudless::kGuardReleaseHoldsMax);
    cfg.guardReleaseHolds = 3;
    assert(cfg.ReleaseHolds() == 3);
    cfg.guardTripOnAnchorLater = false;
    assert(!cfg.TripOnAnchorLater());

    // the probe interval: below 1 is every present (today), above the maximum is the maximum; the A/B block length: below 0 is off,
    // above the maximum is the maximum
    cfg.probeInterval = 0;
    assert(cfg.ProbeInterval() == 1);
    cfg.probeInterval = -7;
    assert(cfg.ProbeInterval() == 1);
    cfg.probeInterval = 1000;
    assert(cfg.ProbeInterval() == hudless::kProbeIntervalMax);
    cfg.probeInterval = 5;
    assert(cfg.ProbeInterval() == 5);
    cfg.probeAbSeconds = -5;
    assert(cfg.ProbeAbSeconds() == 0);
    cfg.probeAbSeconds = 100000;
    assert(cfg.ProbeAbSeconds() == hudless::kProbeAbSecondsMax);
    cfg.probeAbSeconds = 45;
    assert(cfg.ProbeAbSeconds() == 45);

    // AnchorRule is live: the menu-style assignment is what the boundary reads at the next present, and an unknown text reads as r1b
    cfg.anchorRule = std::string("r1b");
    assert(cfg.GetAnchorRule() == hudless::AnchorRule::R1b);
    cfg.anchorRule = std::string("early");
    assert(cfg.GetAnchorRule() == hudless::AnchorRule::Early);
    cfg.anchorRule = std::string("sideways");
    assert(cfg.GetAnchorRule() == hudless::AnchorRule::R1b);

    // PlateDiagnostics publishes its own seam bit at load; unset or false clears it again.
    assert(!cfg.plateDiagnostics.value_or_default() && !ext::Active(F::PlateProbe));
    CSimpleIniA probe;
    probe.SetValue("HudLess", "PlateDiagnostics", "true");
    probe.SetValue("HudLess", "PlateOverlay", "true"); // retired: an existing user ini that still carries it loads without error and changes nothing
    config::Load(probe);
    assert(cfg.plateDiagnostics.value_or_default() && ext::Active(F::PlateProbe));
    {
        config::Binder binder;
        Cfg().hudless.Bind(binder);
        for (const auto& entry : binder.Table())
            assert(std::string(entry.key) != "PlateOverlay"); // no row is bound for it anymore
    }
    config::Save(out);
    assert(std::string(out.GetValue("HudLess", "PlateDiagnostics", "")) == "true");
    assert(out.GetValue("HudLess", "PlateOverlay", nullptr) == nullptr); // and it is not written back
    CSimpleIniA none;
    cfg.plateDiagnostics = false; // a CustomOptional takes the first value only, so the menu-style assignment is what switches it off
    config::Load(none);
    assert(!ext::Active(F::PlateProbe));

    // Native nameplates: the three keys, their clamps, and the load-time order (snapshot first, bit second). With the key
    // off the bit stays as it was: it is the engine's to clear, so the menu and the load never take a hidden image away.
    {
        CSimpleIniA plates;
        plates.SetValue("HudLess", "PlateInterp", "Endpoint");
        plates.SetValue("HudLess", "PlatePhaseOffset", "0");
        plates.SetValue("HudLess", "PlateMaxStep", "1000"); // above the range: clamped
        config::Load(plates);
        assert(cfg.GetPlateInterp() == hudless::PlateInterp::Endpoint);
        assert(cfg.GetPlatePhase() == 0);
        assert(cfg.GetPlateStep() == ext::HudlessConfig::kPlateStepMax);
        const hudless::PlateSnapshot published = hudless::CurrentPlates();
        assert(published.interp == hudless::PlateInterp::Endpoint && published.phase == 0 &&
               published.step == ext::HudlessConfig::kPlateStepMax);
        assert(ext::Active(F::Plate)); // the key is on, so the load set the bit
        CSimpleIniA saved;
        config::Save(saved);
        assert(std::string(saved.GetValue("HudLess", "PlateInterp", "")) == "Endpoint");
        assert(std::string(saved.GetValue("HudLess", "PlatePhaseOffset", "")) == "0");
        assert(std::string(saved.GetValue("HudLess", "PlateMaxStep", "")) == "1000"); // the stored text is not rewritten

        // An unknown switch text is off, and a load with the key off leaves the bit alone (the engine's drain owns it).
        cfg.plateInterp = std::string("sideways");
        cfg.platePhaseOffset = std::string("2");
        cfg.plateMaxStep = -5;
        assert(cfg.GetPlateInterp() == hudless::PlateInterp::Off);
        assert(cfg.GetPlatePhase() == 1);
        assert(cfg.GetPlateStep() == 0);
        CSimpleIniA off;
        off.SetValue("HudLess", "PlateInterp", "off");
        config::Load(off);
        assert(cfg.GetPlateInterp() == hudless::PlateInterp::Off);
        assert(hudless::CurrentPlates().interp == hudless::PlateInterp::Off);
        assert(ext::Active(F::Plate));
        ext::SetActive(F::Plate, false);
    }

    // An unknown Mode string loads as off and stays off.
    CSimpleIniA bad;
    cfg.mode = std::string("full speed ahead");
    config::Load(bad);
    assert(cfg.GetMode() == Mode::Off && hudless::CurrentMode() == Mode::Off && !hudless::Wanted());

    std::puts("PASS: [HudLess] keys, order, defaults, clamps, mode parse and published state");
    return 0;
}
