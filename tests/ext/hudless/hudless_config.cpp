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
    // the developer probes are off by default (and the seam bit that serves them starts clear)
    assert(!defaults.plateDiagnostics.value_or_default() && !defaults.plateOverlay.value_or_default());
    assert(!ext::Active(F::PlateProbe));

    // Exactly the documented keys of section 11 (Diagnostics is bound by package A), in the documented order.
    config::Binder binder;
    defaults.Bind(binder);
    const std::vector<std::string> expectedKeys { "Mode",         "MaskThreshold", "MaskDilation", "PasteCleanup",
                                                  "SteadyTiming", "CoverageGuard", "ShowDetectedUi", "GuardMode", "HdrPaste",
                                                  "PlateDiagnostics", "PlateOverlay" };
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
    assert(hudless::CurrentMode() == Mode::UiFree);
    assert(hudless::Wanted());
    assert(ext::Active(ext::F::Hudless));

    // Save round-trips the values; the old keys are not written.
    CSimpleIniA out;
    config::Save(out);
    assert(std::string(out.GetValue("HudLess", "Mode", "")) == "UI_Free");
    assert(std::string(out.GetValue("HudLess", "SteadyTiming", "")) == "true");
    assert(std::string(out.GetValue("HudLess", "PasteCleanup", "")) == "false");
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

    // PlateDiagnostics publishes its own seam bit at load; unset or false clears it again. The overlay key is stored as written.
    assert(!cfg.plateDiagnostics.value_or_default() && !ext::Active(F::PlateProbe));
    CSimpleIniA probe;
    probe.SetValue("HudLess", "PlateDiagnostics", "true");
    probe.SetValue("HudLess", "PlateOverlay", "true");
    config::Load(probe);
    assert(cfg.plateDiagnostics.value_or_default() && cfg.plateOverlay.value_or_default() && ext::Active(F::PlateProbe));
    config::Save(out);
    assert(std::string(out.GetValue("HudLess", "PlateDiagnostics", "")) == "true");
    CSimpleIniA none;
    cfg.plateDiagnostics = false; // a CustomOptional takes the first value only, so the menu-style assignment is what switches it off
    config::Load(none);
    assert(!ext::Active(F::PlateProbe));

    // An unknown Mode string loads as off and stays off.
    CSimpleIniA bad;
    cfg.mode = std::string("full speed ahead");
    config::Load(bad);
    assert(cfg.GetMode() == Mode::Off && hudless::CurrentMode() == Mode::Off && !hudless::Wanted());

    std::puts("PASS: [HudLess] keys, order, defaults, clamps, mode parse and published state");
    return 0;
}
