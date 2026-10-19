// Dynamic MFG gating table, target and ceiling arithmetic, and the DLSS-G options guard (ext/fg/DynamicMfgGate.h).
#include "dynamic_mfg_harness.h"

#include "ext/fg/DynamicMfgGate.h"

#include <set>
#include <string>

using namespace ext::fg;

namespace
{
GateInputs Open()
{
    GateInputs in;
    in.keyOn = true;
    in.dx12 = true;
    in.dlssgOutput = true;
    in.nvidia = true;
    in.archId = 0x190; // Ada
    in.maxGenerated = 5;
    return in;
}

void GatingTable()
{
    CHECK(EvaluateGate(Open()) == AutoMfgGate::Open);

    // every gate false once, all the others true
    struct Case
    {
        AutoMfgGate expect;
        void (*break_)(GateInputs&);
    };
    const Case cases[] = {
        { AutoMfgGate::KeyOff, [](GateInputs& g) { g.keyOn = false; } },
        { AutoMfgGate::Stopped, [](GateInputs& g) { g.stopped = true; } },
        { AutoMfgGate::NotDx12, [](GateInputs& g) { g.dx12 = false; } },
        { AutoMfgGate::NotDlssg, [](GateInputs& g) { g.dlssgOutput = false; } },
        { AutoMfgGate::NotNvidia, [](GateInputs& g) { g.nvidia = false; } },
        { AutoMfgGate::Blackwell, [](GateInputs& g) { g.archId = 0x1B0; } },
        { AutoMfgGate::Blackwell, [](GateInputs& g) { g.archId = 0x1C0; } },
        { AutoMfgGate::FollowsGame, [](GateInputs& g) { g.followsGame = true; } },
        { AutoMfgGate::DynamicMode, [](GateInputs& g) { g.dynamicMode = true; } },
        { AutoMfgGate::MaxBelow3x, [](GateInputs& g) { g.maxGenerated = 1; } },
        { AutoMfgGate::MaxBelow3x, [](GateInputs& g) { g.maxGenerated = 0; } },
    };
    for (const auto& c : cases)
    {
        GateInputs in = Open();
        c.break_(in);
        CHECK_MSG(EvaluateGate(in) == c.expect, "expected gate %d got %d", (int) c.expect, (int) EvaluateGate(in));
    }

    // boundaries that must stay open
    GateInputs ada = Open();
    ada.archId = 0x1AF;
    CHECK(EvaluateGate(ada) == AutoMfgGate::Open);
    ada = Open();
    ada.maxGenerated = 2; // 3x is enough to choose between 2x and 3x
    CHECK(EvaluateGate(ada) == AutoMfgGate::Open);

    // the first failing gate is reported when several fail
    GateInputs all;
    CHECK(EvaluateGate(all) == AutoMfgGate::KeyOff);
    all.keyOn = true;
    all.stopped = true;
    CHECK(EvaluateGate(all) == AutoMfgGate::Stopped);
    all.stopped = false;
    CHECK(EvaluateGate(all) == AutoMfgGate::NotDx12);

    // texts exist and differ
    std::set<std::string> texts;
    for (auto g : { AutoMfgGate::Open, AutoMfgGate::KeyOff, AutoMfgGate::Stopped, AutoMfgGate::NotDx12,
                    AutoMfgGate::NotDlssg, AutoMfgGate::NotNvidia, AutoMfgGate::Blackwell, AutoMfgGate::FollowsGame,
                    AutoMfgGate::DynamicMode, AutoMfgGate::MaxBelow3x })
    {
        const std::string t = GateText(g);
        CHECK(!t.empty() && t != "unknown");
        texts.insert(t);
    }
    CHECK(texts.size() == 10);
}

void TargetResolution()
{
    CHECK(ResolveTarget(200, 144, 240) == 200); // an explicit target wins, even over a lower limit
    CHECK(ResolveTarget(0, 224, 240) == 224);   // the lower of limit and refresh
    CHECK(ResolveTarget(0, 300, 240) == 240);
    CHECK(ResolveTarget(0, 0, 240) == 240);     // no limit: the refresh
    CHECK(ResolveTarget(0, 144, 0) == 144);     // refresh unknown: the limit
    CHECK(ResolveTarget(0, 0, 0) == 0);         // nothing known: inert
    CHECK(ResolveTarget(-5, 0, 0) == 0);
    CHECK(ResolveTarget(NAN, NAN, NAN) == 0);
    CHECK(ResolveTarget(0, INFINITY, 240) == 240);
    CHECK(ResolveTarget(0, 10, 240) == 30);     // clamped to 30..500
    CHECK(ResolveTarget(900, 0, 0) == 500);
    CHECK(ResolveTarget(0, 0, 1000) == 500);
    CHECK(ResolveTarget(30, 0, 0) == 30);
}

void RefreshIsUnknownBelow24Hz()
{
    // L2: Windows reports 0 or 1 for "unspecified"; 1 Hz must not become a 30 fps target.
    CHECK(UsableRefresh(0) == 0);
    CHECK(UsableRefresh(1) == 0);
    CHECK(UsableRefresh(23.9) == 0);
    CHECK(UsableRefresh(-60) == 0);
    CHECK(UsableRefresh(NAN) == 0);
    CHECK(UsableRefresh(INFINITY) == 0);
    CHECK(UsableRefresh(24) == 24);
    CHECK(UsableRefresh(59.94) == 59.94);
    CHECK(UsableRefresh(240) == 240);
    // end to end: an "unspecified" refresh leaves the limit as the target, and a missing limit stays inert
    CHECK(ResolveTarget(0, 224, UsableRefresh(1)) == 224);
    CHECK(ResolveTarget(0, 0, UsableRefresh(1)) == 0);
    CHECK(ResolveTarget(0, 224, UsableRefresh(240)) == 224);
}

void MenuCardVisibility()
{
    // The card is for the DLSS-G output on every card but RTX 50 (NVIDIA's own dynamic mode is the tool there).
    CHECK(AutoMfgCardVisible(true, false));
    CHECK(!AutoMfgCardVisible(false, false)); // another frame generation output is active, or none
    CHECK(!AutoMfgCardVisible(true, true));   // Blackwell
    CHECK(!AutoMfgCardVisible(false, true));
    // the Blackwell test of the card and the one of the gate are the same threshold
    CHECK(IsBlackwellArchitecture(0x1B0) && IsBlackwellArchitecture(0x1C0) && !IsBlackwellArchitecture(0x1AF));
    CHECK(!IsBlackwellArchitecture(0x190));
}

void PermanentGates()
{
    // Gates that cannot reopen without a change of the keys: the glue stops ticking on them (DynamicMfgGlue.h).
    for (auto g : { AutoMfgGate::NotNvidia, AutoMfgGate::Blackwell, AutoMfgGate::NotDx12, AutoMfgGate::Stopped })
        CHECK(IsPermanentlyClosed(g));
    for (auto g : { AutoMfgGate::Open, AutoMfgGate::KeyOff, AutoMfgGate::NotDlssg, AutoMfgGate::FollowsGame,
                    AutoMfgGate::DynamicMode, AutoMfgGate::MaxBelow3x })
        CHECK(!IsPermanentlyClosed(g)); // the user or the game can change these at any time
}

void CeilingArithmetic()
{
    CHECK(HardMaxFactor(4, 5) == 4); // never 5x/6x, whatever the unlock allows
    CHECK(HardMaxFactor(4, 3) == 4);
    CHECK(HardMaxFactor(4, 2) == 3);
    CHECK(HardMaxFactor(4, 1) == 2);
    CHECK(HardMaxFactor(2, 5) == 2); // AutoMFGMax below the DLSS-G maximum wins
    CHECK(HardMaxFactor(3, 5) == 3);
    CHECK(HardMaxFactor(9, 5) == 4);
    CHECK(HardMaxFactor(0, 5) == 2);
    CHECK(HardMaxFactor(-1, 0) == 2);
}

void OptionsGuard()
{
    OptionsGuardIn in;
    // fixed mode, manual count 6x on an unlocked Ada: untouched (the slider keeps its range)
    in.adaUnlockActive = true;
    in.generated = 5;
    auto out = GuardDlssgOptions(in);
    CHECK(!out.dynamicToOn && !out.ceilingClamped && out.generated == 5);

    // NVIDIA's dynamic mode, supported, on the unlocked Ada: ceiling 4x
    in.dynamicMode = true;
    in.dmfgSupported = true;
    out = GuardDlssgOptions(in);
    CHECK(!out.dynamicToOn && out.ceilingClamped && out.generated == 3);
    in.generated = 3;
    out = GuardDlssgOptions(in);
    CHECK(!out.ceilingClamped && out.generated == 3); // already at the ceiling
    in.generated = 4;
    out = GuardDlssgOptions(in);
    CHECK(out.ceilingClamped && out.generated == 3); // 5x

    // dynamic mode unsupported: falls back to the fixed mode; the count is the user's, not clamped
    in.dmfgSupported = false;
    in.generated = 5;
    out = GuardDlssgOptions(in);
    CHECK(out.dynamicToOn && !out.ceilingClamped && out.generated == 5);

    // no unlock (Blackwell, or an Ada without it): nothing is clamped
    in.dmfgSupported = true;
    in.adaUnlockActive = false;
    out = GuardDlssgOptions(in);
    CHECK(!out.dynamicToOn && !out.ceilingClamped && out.generated == 5);

    // the selector's choice on the unlocked Ada
    in = {};
    in.adaUnlockActive = true;
    in.autoMfgControls = true;
    in.generated = 4;
    out = GuardDlssgOptions(in);
    CHECK(out.ceilingClamped && out.generated == 3);
    in.generated = 2;
    out = GuardDlssgOptions(in);
    CHECK(!out.ceilingClamped && out.generated == 2);
    in.autoMfgControls = false; // the selector is not in control: manual choice
    in.generated = 4;
    out = GuardDlssgOptions(in);
    CHECK(!out.ceilingClamped && out.generated == 4);
}
} // namespace

int main()
{
    GatingTable();
    TargetResolution();
    RefreshIsUnknownBelow24Hz();
    MenuCardVisibility();
    PermanentGates();
    CeilingArithmetic();
    OptionsGuard();
    if (g_failures)
    {
        std::printf("dynamic_mfg_gate_unit: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("dynamic_mfg_gate_unit: all passed\n");
    return 0;
}
