// Dynamic MFG controller (ext/fg/DynamicMfgControl.h) with a fake clock and a fake world: gating and its log lines,
// the kill-switch outcomes, activation and gap restarts, state that survives them, the self-test and its one result line.
#include "dynamic_mfg_harness.h"

#include "ext/fg/DynamicMfgControl.h"

#include <string>

using namespace ext::fg;
using Why = DynamicMfg::Why;

namespace
{
struct Rig
{
    AutoMfgController c;
    Plant plant;
    uint64_t t = 1000;
    int factor = 2;
    double target = 224.0; // AutoMFGTarget; the plant's limit and refresh are passed along too
    bool selfTest = false;
    int maxCfg = 4;
    // the fake world
    bool recreate = false; // a switch costs a stalled frame
    double stallMs = 190.0;
    double pendingStall = 0.0;
    int detectedOverride = -1; // the plugin applies another count than requested
    uint32_t status = 0;
    bool stuckAtOne = false;
    // an unrelated long frame `hitchDelay` ms after each of the next `hitchLeft` switches (a game hitch, not a re-creation)
    double hitchLen = 0.0;
    uint64_t hitchDelay = 800;
    int hitchLeft = 0;
    uint64_t hitchDue = 0;
    int stallLeft = -1; // with `recreate`: how many switches still stall (-1 = all)
    bool stallUpOnly = false; // with `recreate`: only a switch to a higher count stalls (a plugin that grows buffers)
    // the plugin applies a new count `lagMs` after the switch (the old count is still reported until then)
    uint64_t lagMs = 150;
    int prevFactor = 2;
    uint64_t switchT = 0;
    uint64_t zeroFrom = 0, zeroUntil = 0;   // detectedGenerated reads 0 in [from, until)
    uint64_t pauseFrom = 0, pauseUntil = 0; // frame generation is paused in [from, until)
    // a game stall (a loading screen, a feature re-creation): every real interval in [stallFrom, stallUntil) is at least
    // stallFrameMs long (55 ms = the 18 fps of the two in-game runs)
    uint64_t stallFrom = 0, stallUntil = 0;
    double stallFrameMs = 55.0;
    GateInputs gate;
    std::vector<AutoMfgLine> lines;
    std::vector<uint64_t> lineT; // the time of each line
    std::vector<Switch> switches;
    int recoveries = 0;
    uint64_t doneAt = 0; // when the first self-test result line appeared

    Rig()
    {
        plant.base2x = 18.2;
        gate.keyOn = true;
        gate.dx12 = true;
        gate.dlssgOutput = true;
        gate.nvidia = true;
        gate.archId = 0x190;
        gate.maxGenerated = 5;
    }

    AutoMfgOut Step()
    {
        AutoMfgTick in;
        in.nowMs = t;
        double ms = pendingStall > 0.0 ? pendingStall : plant.Ms(factor);
        if (hitchDue != 0 && t >= hitchDue && pendingStall <= 0.0)
        {
            ms = hitchLen;
            hitchDue = 0;
        }
        pendingStall = 0.0;
        if (stallUntil != 0 && t >= stallFrom && t < stallUntil)
            ms = std::max(ms, stallFrameMs);
        in.realMs = ms;
        in.appliedFactor = factor;
        in.selfTest = selfTest;
        in.maxCfg = maxCfg;
        in.targetCfg = target;
        in.limit = plant.limit;
        in.refresh = plant.refresh;
        in.gate = gate;
        in.stateValid = true;
        in.slStatus = status;
        in.presented = stuckAtOne ? 1u : (uint32_t) factor;
        int detected = switchT != 0 && t - switchT < lagMs ? prevFactor - 1 : factor - 1;
        if (detectedOverride >= 0)
            detected = detectedOverride;
        const bool inPause = pauseUntil != 0 && t >= pauseFrom && t < pauseUntil;
        if (inPause || (zeroUntil != 0 && t >= zeroFrom && t < zeroUntil))
            detected = 0;
        in.detectedGenerated = detected;
        in.paused = inPause;
        const AutoMfgOut out = c.Tick(in);
        for (const auto& l : out.lines)
        {
            lines.push_back(l);
            lineT.push_back(t);
        }
        if (doneAt == 0 && ResultLines() > 0)
            doneAt = t;
        if (out.setFactor)
        {
            switches.push_back({ t, factor, out.factor, Why::None });
            prevFactor = factor;
            switchT = t;
            const bool up = out.factor > factor;
            factor = out.factor;
            if (recreate && stallLeft != 0 && (up || !stallUpOnly))
            {
                pendingStall = stallMs;
                if (stallLeft > 0)
                    --stallLeft;
            }
            if (hitchLeft > 0)
            {
                hitchDue = t + hitchDelay;
                --hitchLeft;
            }
        }
        if (out.recover)
        {
            ++recoveries;
            factor = out.factor;
            c.OnActivated(); // the toggle path reactivates DLSS-G
        }
        t += (uint64_t) std::max(1.0, std::floor(ms));
        return out;
    }
    void Run(uint64_t ms)
    {
        const uint64_t end = t + ms;
        while (t < end)
            Step();
    }
    // The self-test starts only after the game has been calm for 20 s, and the selector would move the count meanwhile.
    // Calm(): the target sits just above the presented rate at the count in force (0.95: no step up, no probe, no step down),
    // and 20.5 s pass without the self-test being asked for; then it is asked for and starts at once.
    void Calm()
    {
        target = plant.Presented(factor) / 0.95;
        const bool asked = selfTest;
        selfTest = false;
        Run(20500);
        selfTest = asked;
    }
    int Count(const std::string& prefix) const
    {
        int n = 0;
        for (const auto& l : lines)
            n += l.text.rfind(prefix, 0) == 0;
        return n;
    }
    const AutoMfgLine* Find(const std::string& needle) const
    {
        for (const auto& l : lines)
            if (l.text.find(needle) != std::string::npos)
                return &l;
        return nullptr;
    }
    // Time of the first line that contains `needle` (0 = none).
    uint64_t TimeOf(const std::string& needle) const
    {
        for (size_t i = 0; i < lines.size(); ++i)
            if (lines[i].text.find(needle) != std::string::npos)
                return lineT[i];
        return 0;
    }
    int ResultLines() const { return Count("AutoMFG self-test: PASS") + Count("AutoMFG self-test: FAIL"); }
    int LinesWith(const std::string& needle) const
    {
        int n = 0;
        for (const auto& l : lines)
            n += l.text.find(needle) != std::string::npos;
        return n;
    }
};

void GatingLogsTheReasonOnce()
{
    struct Case
    {
        const char* name;
        void (*apply)(Rig&);
        const char* text;
    };
    const Case cases[] = {
        { "follows game", [](Rig& r) { r.gate.followsGame = true; }, "Frame Gen is on Default" },
        { "dynamic mode", [](Rig& r) { r.gate.dynamicMode = true; }, "Force Dynamic MFG is on" },
        { "blackwell", [](Rig& r) { r.gate.archId = 0x1B0; }, "RTX 50" },
        { "max 1", [](Rig& r) { r.gate.maxGenerated = 1; }, "below 3x" },
        { "not dlssg", [](Rig& r) { r.gate.dlssgOutput = false; }, "not DLSS-G" },
        { "vulkan", [](Rig& r) { r.gate.dx12 = false; }, "not the D3D12" },
        { "not nvidia", [](Rig& r) { r.gate.nvidia = false; }, "not an NVIDIA" },
    };
    for (const auto& cs : cases)
    {
        Rig r;
        cs.apply(r);
        for (int i = 0; i < 1000; ++i)
        {
            const auto out = r.Step();
            CHECK_MSG(!out.controls && !out.setFactor, "%s: acted on a closed gate", cs.name);
        }
        CHECK_MSG(r.lines.size() == 1 && r.lines[0].warn && r.lines[0].text.rfind("AutoMFG: inactive, ", 0) == 0 &&
                      r.lines[0].text.find(cs.text) != std::string::npos,
                  "%s: %zu lines", cs.name, r.lines.size());
        CHECK(r.switches.empty());
    }
    // key off is silent, and so is a controller that was never on
    Rig off;
    off.gate.keyOn = false;
    off.Run(5000);
    CHECK(off.lines.empty() && off.switches.empty());
    // on -> off logs once, then off -> on logs the activation once
    Rig r;
    r.Run(2000);
    CHECK(r.Count("AutoMFG: active (highest 4x, target 224 fps, display 240 Hz, limit 224)") == 1);
    r.gate.keyOn = false;
    r.Run(3000);
    CHECK(r.Count("AutoMFG: switched off") == 1);
    r.gate.keyOn = true;
    r.Run(2000);
    CHECK(r.Count("AutoMFG: active") == 2);
}

void CleanSwitchLogsAndSettles()
{
    Rig r;
    r.Run(20000); // heavy scene: 2x -> 3x -> 4x
    CHECK(r.factor == 4 && r.switches.size() == 2);
    CHECK(r.Count("AutoMFG: 2x -> 3x (real ") == 1 && r.Count("AutoMFG: 3x -> 4x (real ") == 1);
    CHECK(r.Count("AutoMFG: 3x settled (worst interval ") == 1 && r.Count("AutoMFG: 4x settled") == 1);
    CHECK(r.Find("target 224, up)") != nullptr);
    CHECK(r.c.CostlyCount() == 0 && !r.c.Stopped());
    CHECK(r.c.Selector().HoldMs() == DynamicMfg::kDefaultHoldMs);
}

void FreshControllerProbesOnARefreshBoundScene()
{
    // The main case: the output is pinned at the refresh from the start, no switch ever happened, nothing was learned.
    Rig r;
    r.plant.base2x = 8.0;
    r.factor = 4;
    r.Run(45000);
    CHECK_MSG(r.factor == 2, "stuck at %dx", r.factor);
    CHECK(r.Count("AutoMFG: 4x -> 3x (real ") == 1 && r.Find("probe: output stayed at the target for 12 s") != nullptr);
    // the probes are watched as switches too and come out clean
    CHECK(r.c.CostlyCount() == 0);
}

void CostlyOutcomeSlowsDownThenStops()
{
    Rig r;
    r.recreate = true;
    r.Run(8000);
    CHECK(r.switches.size() == 1 && r.c.CostlyCount() == 1);
    CHECK(r.Count("AutoMFG: switch 2x -> 3x cost a 190 ms frame") == 1);
    CHECK(r.c.Selector().HoldMs() == 12000 && !r.c.Stopped());
    const uint64_t first = r.switches[0].t;
    r.Run(30000);
    CHECK(r.c.CostlyCount() == 2 && r.c.Stopped());
    CHECK_MSG(r.switches.size() == 2 && r.switches[1].t - first >= 11900, "second switch %llu ms after the first",
              r.switches.size() > 1 ? r.switches[1].t - first : 0ull);
    CHECK(r.Count("AutoMFG: changing the count recreates DLSS-G on this setup; automatic MFG stopped") == 1);
    const size_t before = r.switches.size();
    r.Run(60000);
    CHECK(r.switches.size() == before && r.recoveries == 0); // stopped means stopped, and no recovery for a costly one
    CHECK(r.Count("AutoMFG: inactive, stopped for this session") == 1);

    // no probes once trust is reduced: the light refresh-bound scene probes once, the probe stalls, and that is it
    Rig p;
    p.plant.base2x = 8.0;
    p.factor = 4;
    p.recreate = true;
    p.Run(45000);
    CHECK(p.c.CostlyCount() == 1); // the probe stalled
    const size_t n = p.switches.size();
    p.Run(120000);
    CHECK(p.c.CostlyCount() == 1 && p.switches.size() == n); // no second probe, no other switch
}

// The two in-game runs (RTX 4090, 4K): a loading screen and a DLSS preset change stalled the game at 18 fps while the count
// was being changed, and the kill-switch took the stall for a failed change and stopped the mode for the session. While the
// real interval is above 30 ms the controller is in a stall state (the rules are below, one test each).

// Decision 1, first part: nothing new starts in a stall. The selector is not asked at all (it would count a switch that the
// controller then drops) and starts over when the stall ends: no change for the hold after it.
void ANewChangeIsNotStartedInAStall()
{
    Rig r; // 2x at 18 ms real would go up at once without the stall
    r.stallFrom = 0;
    r.stallUntil = r.t + 20000;
    r.Run(20000);
    CHECK_MSG(r.switches.empty(), "%zu switches during a stall", r.switches.size());
    CHECK(r.Count("AutoMFG: 2x ->") == 0 && r.Count("AutoMFG: 3x ->") == 0);
    CHECK(r.c.Selector().RecentSwitches(r.t) == 0); // the selector did not record a switch that never happened
    const uint64_t over = r.stallUntil;
    r.Run(15000);
    CHECK(!r.switches.empty());
    CHECK_MSG(r.switches.empty() || r.switches.front().t >= over + 2900,
              "first change %llu ms after the stall (the hold starts over when it ends)",
              r.switches.empty() ? 0ull : r.switches.front().t - over);
    CHECK(r.Count("AutoMFG: inactive") == 0 && r.c.CostlyCount() == 0 && !r.c.Stopped());
}

// The boundary is the smoothed interval at 30 ms: a scene at 28 ms real (35 fps) is not a stall, one at 32 ms is.
void TheStallThresholdIs30Ms()
{
    for (const double base : { 28.0, 32.0 })
    {
        Rig r;
        r.plant.base2x = base;
        r.plant.refresh = 0;
        r.plant.limit = 0;
        r.Run(20000);
        CHECK_MSG(r.switches.empty() == (base > 30.0), "%.0f ms scene: %zu switches", base, r.switches.size());
        CHECK(r.c.CostlyCount() == 0 && !r.c.Stopped() && r.recoveries == 0);
    }
}

// One long frame is no stall: the estimate clips a sample, so the re-creation signature still reaches the strike rules.
void OneLongFrameIsNoStall()
{
    Rig r;
    r.recreate = true; // every switch costs one 190 ms frame
    r.Run(8000);
    CHECK_MSG(r.switches.size() == 1 && r.c.CostlyCount() == 1, "%zu switches, %d strikes", r.switches.size(),
              r.c.CostlyCount());
    CHECK(r.Count("AutoMFG: switch 2x -> 3x cost a 190 ms frame") == 1);
    CHECK(r.Count("skipped, the game was stalled") == 0);
}

// A tick-by-tick helper: run until the first switch, return its time.
uint64_t RunToFirstSwitch(Rig& r)
{
    while (r.switches.empty())
        r.Step();
    return r.switches.back().t;
}

void TransientZeroAtTheVerdictIsNotAFailure()
{
    Rig r;
    const uint64_t s = RunToFirstSwitch(r);
    r.zeroFrom = s + 1000; // the plugin's field reads 0 for the one tick that closes the watch
    r.zeroUntil = s + 1040;
    r.Run(5000);
    CHECK_MSG(r.recoveries == 0 && !r.c.Stopped(), "recoveries %d stopped %d", r.recoveries, (int) r.c.Stopped());
    CHECK(r.Find("AutoMFG: 3x settled (worst interval ") != nullptr && r.Find("applied 3x)") != nullptr);
    CHECK(r.Count("AutoMFG: inactive") == 0);
}

void ThreeZeroSamplesAtTheEndAreStillAFailure()
{
    Rig r;
    const uint64_t s = RunToFirstSwitch(r);
    r.zeroFrom = s + 800; // 0 for the last 200 ms of the watch and on (a shorter run than kBadRunMs is not a failure)
    r.zeroUntil = s + 60000;
    r.Run(20000);
    CHECK_MSG(r.recoveries == 1 && r.c.Stopped(), "recoveries %d stopped %d", r.recoveries, (int) r.c.Stopped());
    CHECK(r.Find("applied count 0 != requested 2") != nullptr);
}

void ANonZeroStatusBeforeTheSwitchIsNotTheSwitchsFault()
{
    Rig r;
    r.status = 0x4; // present all the time, also before and across every switch
    r.Run(30000);
    CHECK_MSG(r.switches.size() >= 2 && r.recoveries == 0 && !r.c.Stopped(), "switches %zu recoveries %d", r.switches.size(),
              r.recoveries);
}

void ABriefBadStatusIsNotEnough()
{
    Rig r;
    const uint64_t s = RunToFirstSwitch(r);
    while (r.t < s + 985)
        r.Step();
    r.status = 0x2;
    r.Step();
    r.Step(); // at most two samples, the one that closes the watch among them
    r.status = 0;
    r.Run(10000);
    CHECK_MSG(r.recoveries == 0 && !r.c.Stopped(), "recoveries %d", r.recoveries);
}

void AShortPauseAcrossTheWatchEndIsNotAFailure()
{
    {
        // The paused flag: a state the DLL cannot produce today (Dispatch returns before SetOptions while paused), kept for
        // callers that do tick while paused. The case that matches the DLL is the silent 600 ms one below.
        Rig r; // the pause is reported (paused flag) around the end of the watch
        const uint64_t s = RunToFirstSwitch(r);
        r.pauseFrom = s + 900;
        r.pauseUntil = s + 1300;
        r.Run(20000);
        CHECK_MSG(r.recoveries == 0 && !r.c.Stopped(), "paused flag: recoveries %d stopped %d", r.recoveries,
                  (int) r.c.Stopped());
        CHECK(r.factor == 4); // and it goes on working
    }
    {
        Rig r; // no tick for 600 ms (under the 1 s gap rule), the first tick after it still reads 0
        const uint64_t s = RunToFirstSwitch(r);
        while (r.t < s + 950)
            r.Step();
        r.t += 600;
        r.zeroFrom = r.t;
        r.zeroUntil = r.t + 30;
        r.Run(20000);
        CHECK_MSG(r.recoveries == 0 && !r.c.Stopped(), "silent 600 ms: recoveries %d stopped %d", r.recoveries,
                  (int) r.c.Stopped());
    }
}

void UnrelatedHitchesDoNotStopTheMode()
{
    // Two 150 ms game hitches, five minutes apart, each 0.8 s after a switch: nothing to do with a count change.
    Rig r;
    r.hitchLen = 150.0;
    r.hitchLeft = 1;
    r.Run(20000); // heavy scene: 2x -> 3x -> 4x, the first switch has the hitch inside its watch
    CHECK(r.factor == 4 && r.switches.size() == 2);
    CHECK_MSG(r.c.CostlyCount() == 0 && !r.c.Stopped(), "a hitch 0.8 s after a switch counted as a strike (%d)",
              r.c.CostlyCount());
    r.Run(300000);        // five minutes of steady play
    r.plant.base2x = 4.0; // light scene: only the probe-down finds 2x (probes are off after a strike)
    r.hitchLeft = 1;
    r.Run(60000);
    CHECK_MSG(!r.c.Stopped() && r.factor == 2, "stopped %d, factor %dx", (int) r.c.Stopped(), r.factor);
    CHECK(r.c.CostlyCount() == 0);
}

void LongFrameLaterInTheWatchIsNotTheSignature()
{
    // 150 ms, 0.8 s after the switch, normal intervals around it: no strike, no reduced trust, the settled line says so.
    Rig r;
    r.hitchLen = 150.0;
    r.hitchLeft = 1;
    r.Run(8000);
    CHECK(!r.switches.empty());
    CHECK(r.c.CostlyCount() == 0 && !r.c.Stopped() && r.c.Selector().HoldMs() == DynamicMfg::kDefaultHoldMs);
    CHECK(r.Count("AutoMFG: switch") == 0);
    CHECK(r.Find("AutoMFG: 3x settled (worst interval 150 ms") != nullptr);
}

void TheSignatureHasToRepeatInARow()
{
    // The first switch stalls, the second is clean (the row breaks), a stall much later is a single one again.
    Rig r;
    r.plant.refresh = 0;
    r.plant.limit = 0;
    r.recreate = true;
    r.stallLeft = 1;
    r.Run(8000);
    CHECK(r.c.CostlyCount() == 1 && !r.c.Stopped() && r.c.Selector().HoldMs() == 12000);
    r.Run(60000);
    CHECK_MSG(r.factor == 4 && r.c.CostlyCount() == 1 && !r.c.Stopped(), "factor %dx stopped %d", r.factor,
              (int) r.c.Stopped());
    r.plant.base2x = 4.0; // light: 4x -> 3x -> 2x by the rate rules
    r.stallLeft = 1;
    r.Run(90000);
    CHECK_MSG(r.c.CostlyCount() == 2 && !r.c.Stopped(), "two strikes but not in a row: stopped %d", (int) r.c.Stopped());
    CHECK_MSG(r.factor == 2, "factor %dx", r.factor);
}

void ARecreationOnUpStepsOnlyStillStopsOnTheThirdStrike()
{
    // A plugin that re-creates DLS-G only when the count grows: the down steps are clean and break the "two in a row" row,
    // so the row alone would never stop it. Heavy scene (3x is right) and light scene (2x is right) alternate.
    Rig r;
    r.plant.refresh = 0;
    r.plant.limit = 0;
    r.recreate = true;
    r.stallUpOnly = true;
    int phase = 0;
    auto runPhase = [&r, &phase]
    {
        r.plant.base2x = phase % 2 == 0 ? 10.0 : 4.0;
        ++phase;
        r.Run(40000);
    };
    runPhase(); // up: strike 1
    CHECK_MSG(r.switches.size() == 1 && r.c.CostlyCount() == 1 && !r.c.Stopped(), "switches %zu strikes %d", r.switches.size(),
              r.c.CostlyCount());
    runPhase(); // down, clean
    runPhase(); // up: strike 2
    runPhase(); // down, clean
    CHECK_MSG(r.switches.size() == 4 && r.c.CostlyCount() == 2 && !r.c.Stopped(), "switches %zu strikes %d stopped %d",
              r.switches.size(), r.c.CostlyCount(), (int) r.c.Stopped());
    {
        // The same history, then the self-test: its first up step stalls (strike 3), the second is clean (that breaks the
        // row), so only the total stops the mode when the test ends.
        Rig s = r;
        s.selfTest = true;
        s.stallLeft = 1;
        s.Run(60000);
        CHECK(s.ResultLines() == 1 && s.Find("AutoMFG self-test: FAIL (re-created") != nullptr);
        CHECK_MSG(s.c.CostlyCount() == 3 && s.c.Stopped(), "self-test: strikes %d stopped %d", s.c.CostlyCount(),
                  (int) s.c.Stopped());
    }
    runPhase(); // up: strike 3
    CHECK_MSG(r.c.Stopped() && r.c.CostlyCount() == 3, "strikes %d stopped %d", r.c.CostlyCount(), (int) r.c.Stopped());
    CHECK(r.Count("AutoMFG: changing the count recreates DLSS-G on this setup; automatic MFG stopped") == 1);
    CHECK(r.switches.size() == 5 && r.recoveries == 0);
    const size_t n = r.switches.size();
    runPhase();
    runPhase();
    CHECK(r.switches.size() == n); // stopped means stopped
}

void StrikesAreForgottenAfterTenCleanSwitches()
{
    Rig r;
    r.plant.refresh = 0;
    r.plant.limit = 0;
    r.recreate = true;
    r.stallLeft = 1;
    r.Run(8000);
    CHECK(r.c.CostlyCount() == 1 && r.c.Selector().HoldMs() == 12000);
    r.recreate = false;
    for (int i = 0; i < 14; ++i)
    {
        r.plant.base2x = i % 2 == 0 ? 4.0 : 22.0; // alternating scene (22 ms is under the stall limit)
        r.Run(90000);
        const size_t clean = r.switches.size() - 1; // every switch after the first was clean
        if (clean < 9)
            CHECK_MSG(r.c.CostlyCount() == 1, "forgot after %zu clean switches", clean);
        if (clean >= 11)
            break;
    }
    CHECK_MSG(r.c.CostlyCount() == 0 && !r.c.Stopped(), "strikes %d after %zu switches", r.c.CostlyCount(),
              r.switches.size());
    CHECK(r.c.Selector().HoldMs() == DynamicMfg::kDefaultHoldMs);
}

// Runs the alternating scene until the rig has made `n` switches in all, then a few seconds more so that the last one has its
// verdict. The scene changes every 90 s; one second steps so that the count is hit exactly.
void RunToSwitchCount(Rig& r, size_t n, int& scene)
{
    for (int guard = 0; guard < 400 && r.switches.size() < n; ++guard)
    {
        r.plant.base2x = scene % 2 == 0 ? 4.0 : 22.0;
        for (int s = 0; s < 90 && r.switches.size() < n; ++s)
            r.Run(1000);
        ++scene;
    }
    r.Run(3000);
}

// One switch whose first interval stalls (the re-creation signature), then back to clean switches.
void OneStrike(Rig& r, int& scene)
{
    const size_t n = r.switches.size();
    r.recreate = true;
    r.stallLeft = 1;
    RunToSwitchCount(r, n + 1, scene);
    r.recreate = false;
}

// The strikes are forgotten by the NUMBER of clean switches, not by time (the third review, L1): after a strike the selector
// is in reduced trust and switches rarely, a strike restarts the count, so nine clean switches between two strikes forget
// nothing (the third strike stops the mode) and ten forget the earlier one (the third strike is then a first one).
void NineCleanSwitchesDoNotForgetAStrikeButTenDo()
{
    for (const int clean : { 9, 10 })
    {
        Rig r;
        r.plant.refresh = 0;
        r.plant.limit = 0;
        int scene = 0;
        OneStrike(r, scene);
        CHECK_MSG(r.switches.size() == 1 && r.c.CostlyCount() == 1, "clean %d: switches %zu strikes %d", clean, r.switches.size(),
                  r.c.CostlyCount());
        RunToSwitchCount(r, r.switches.size() + clean, scene);
        const int afterClean = r.c.CostlyCount();
        CHECK_MSG(afterClean == (clean == 9 ? 1 : 0), "clean %d: strikes %d after the clean switches", clean, afterClean);
        OneStrike(r, scene);
        CHECK_MSG(r.c.CostlyCount() == (clean == 9 ? 2 : 1) && !r.c.Stopped(), "clean %d: strikes %d stopped %d", clean,
                  r.c.CostlyCount(), (int) r.c.Stopped());
        RunToSwitchCount(r, r.switches.size() + clean, scene);
        CHECK_MSG(r.c.CostlyCount() == (clean == 9 ? 2 : 0), "clean %d: strikes %d before the last strike", clean,
                  r.c.CostlyCount());
        OneStrike(r, scene);
        if (clean == 9)
        {
            CHECK_MSG(r.c.Stopped() && r.c.CostlyCount() == 3, "clean 9: strikes %d stopped %d", r.c.CostlyCount(),
                      (int) r.c.Stopped());
            CHECK(r.Count("AutoMFG: changing the count recreates DLSS-G on this setup; automatic MFG stopped") == 1);
        }
        else
        {
            CHECK_MSG(!r.c.Stopped() && r.c.CostlyCount() == 1, "clean 10: strikes %d stopped %d", r.c.CostlyCount(),
                      (int) r.c.Stopped());
        }
    }
}

void BrokenOutcomesRecoverOnceAndStop()
{
    struct Case
    {
        const char* name;
        void (*apply)(Rig&);
        const char* text;
    };
    const Case cases[] = {
        { "status", [](Rig& r) { r.status = 0x2; }, "Streamline status 0x2" }, // raised after the switch (see below)
        { "count not applied", [](Rig& r) { r.detectedOverride = 1; }, "applied count 1 != requested 2" },
        { "presented stuck", [](Rig& r) { r.stuckAtOne = true; }, "presented frames stuck at 1" },
    };
    for (const auto& cs : cases)
    {
        Rig r;
        r.Run(2500); // 2x in force, accepted as the last good count
        if (std::string(cs.name) == "status")
        {
            while (r.switches.empty()) // a status that was already bad before the switch is not the switch's fault
                r.Step();
        }
        cs.apply(r);
        r.Run(30000);
        CHECK_MSG(r.recoveries == 1, "%s: %d recoveries", cs.name, r.recoveries);
        CHECK_MSG(r.Find(cs.text) != nullptr, "%s: no '%s' line", cs.name, cs.text);
        CHECK_MSG(r.factor == 2 && r.c.Stopped(), "%s: factor %dx stopped %d", cs.name, r.factor, (int) r.c.Stopped());
        CHECK_MSG(r.switches.size() == 1, "%s: %zu switches (one, then stopped)", cs.name, r.switches.size());
        CHECK(r.Count("AutoMFG: inactive, stopped for this session") == 1);
        CHECK(r.Find("going back to 2x, automatic mode stopped") != nullptr);
        const size_t n = r.switches.size();
        r.Run(60000);
        CHECK(r.switches.size() == n && r.recoveries == 1);
    }
}

void ActivationAndGapCloseTheWatchWithoutAVerdict()
{
    for (int kind = 0; kind < 2; ++kind)
    {
        Rig r;
        r.recreate = false;
        while (r.switches.empty())
            r.Step();
        // the toggle path (or a pause) follows the switch with a long frame
        r.pendingStall = 190.0;
        if (kind == 0)
            r.c.OnActivated();
        else
            r.t += 3000; // gap
        r.Run(15000);
        CHECK_MSG(r.c.CostlyCount() == 0 && !r.c.Stopped(), "kind %d: blamed on the selector", kind);
        CHECK(r.Count("AutoMFG: switch") == 0);
    }

    // what was learned survives an activation
    Rig r;
    r.recreate = true;
    r.Run(8000);
    CHECK(r.c.CostlyCount() == 1);
    r.c.OnActivated();
    r.Run(2000);
    CHECK(r.c.CostlyCount() == 1 && r.c.Selector().HoldMs() == 12000 && !r.c.Stopped());
    Rig b;
    b.Run(2500);
    b.detectedOverride = 0;
    b.Run(5000);
    CHECK(b.c.Stopped());
    b.c.OnActivated();
    b.Run(1000);
    CHECK(b.c.Stopped() && b.Count("AutoMFG: inactive, stopped") == 1);
}

void GapStartsANewHold()
{
    Rig r;
    r.Run(2500);
    const uint64_t resume = r.t + 5000;
    r.t = resume; // the game paused
    r.Run(2500);
    CHECK_MSG(r.switches.empty() || r.switches.back().t < resume, "switched %llu ms after a pause",
              r.switches.empty() ? 0ull : r.switches.back().t - resume);
    r.Run(6000);
    CHECK(!r.switches.empty() && r.switches.back().t - resume >= 2900);
}

// Decision 1, second part (the logs of both runs): a stall right after a switch, the new count applied late. The change is
// not judged while the stall lasts, no strike, no stop; once the average has been at or below 30 ms for a second it is judged
// by the usual rules (here: the count is there by then, clean).
void ALateCountInAStallIsNoStrike()
{
    struct Case
    {
        uint64_t off; // the stall begins this long after the switch
        double frameMs;
    };
    // with the switch (the logs) / in the middle of its watch / long frames from the first one after the switch: the
    // 200 ms frames are the re-creation signature if they were the switch's own, and are not when a stall brings them
    for (const Case c : { Case{ 50, 55.0 }, Case{ 600, 55.0 }, Case{ 0, 200.0 } })
    {
        Rig r;
        const uint64_t s = RunToFirstSwitch(r);
        r.lagMs = 3500; // the plugin applies the new count 3.5 s after the switch
        r.stallFrom = s + c.off;
        r.stallUntil = s + c.off + 2400;
        r.stallFrameMs = c.frameMs;
        r.Run(5000); // the next change of the selector would come later: the slow count belongs to this one only
        CHECK_MSG(r.recoveries == 0 && !r.c.Stopped(), "off %llu: recoveries %d stopped %d", c.off, r.recoveries,
                  (int) r.c.Stopped());
        CHECK_MSG(r.c.CostlyCount() == 0 && r.Count("AutoMFG: inactive") == 0 && r.Count("applied count") == 0,
                  "off %llu: strikes %d", c.off, r.c.CostlyCount());
        CHECK(r.Count("skipped, the game was stalled") == 0);
        const uint64_t at = r.TimeOf("AutoMFG: 3x settled");
        CHECK_MSG(at >= r.stallUntil + 1000 && at <= r.stallUntil + 1700, "off %llu: judged %lld ms after the stall", c.off,
                  (long long) at - (long long) r.stallUntil);
        CHECK(r.Find("applied 3x)") != nullptr);
        CHECK_MSG(r.Find("(worst interval 5") == nullptr && r.Find("(worst interval 190 ms") == nullptr &&
                      r.Find("(worst interval 200 ms") == nullptr,
                  "off %llu: the stall's frames are in the worst interval of the calm second", c.off);
    }
}

// A real fault does not hide behind a stall that is over: the usual rules judge it on the calm second after it.
void ARealFaultAfterAStallIsStillConvicted()
{
    Rig r;
    const uint64_t s = RunToFirstSwitch(r);
    r.detectedOverride = r.prevFactor - 1; // the old count stays
    r.stallFrom = s + 50;
    r.stallUntil = s + 2000;
    r.Run(10000);
    CHECK_MSG(r.recoveries == 1 && r.c.Stopped(), "recoveries %d stopped %d", r.recoveries, (int) r.c.Stopped());
    const uint64_t at = r.TimeOf("AutoMFG: applied count 1 != requested 2 (generated frames), switch 2x -> 3x");
    CHECK_MSG(at >= r.stallUntil + 1000 && at <= r.stallUntil + 1700, "convicted %lld ms after the stall",
              (long long) at - (long long) r.stallUntil);
    CHECK(r.Count("skipped, the game was stalled") == 0);
}

// The same faults at 200 fps (no stall) are convicted exactly as before: one second after the switch, the strict rules.
void ARealFaultAt200FpsIsConvictedAsBefore()
{
    for (int kind = 0; kind < 2; ++kind)
    {
        Rig r;
        r.plant.base2x = 5.0;
        r.plant.refresh = 0;
        r.plant.limit = 0;
        r.target = 600.0;
        const uint64_t s = RunToFirstSwitch(r);
        if (kind == 0)
            r.detectedOverride = r.prevFactor - 1;
        else
            r.status = 0x2;
        r.Run(5000);
        CHECK_MSG(r.recoveries == 1 && r.c.Stopped(), "kind %d: recoveries %d stopped %d", kind, r.recoveries,
                  (int) r.c.Stopped());
        const uint64_t at = r.TimeOf(", switch ");
        CHECK_MSG(at >= s + 1000 && at <= s + 1300, "kind %d: convicted %lld ms after the switch", kind,
                  (long long) at - (long long) s);
        CHECK(r.Count("skipped, the game was stalled") == 0);
    }
}

// Decision 2: 5 s from the SWITCH. A watch that a stall kept from being judged until then ends as unknown: no strike, no
// stop, the count stays, one INFO line, the selector starts over (nothing changes for its hold after the stall).
void AWatchThatCannotBeJudgedWithinFiveSecondsEndsAsUnknown()
{
    for (const uint64_t stallLen : { 9000ull, 4150ull }) // 4.15 s: calm again at about 4.4 s, so the second would end after 5 s
    {
        Rig r;
        const uint64_t s = RunToFirstSwitch(r);
        r.detectedOverride = r.prevFactor - 1; // a real fault, but it cannot be judged in time
        r.stallFrom = s + 50;
        r.stallUntil = s + 50 + stallLen;
        r.Run(7000); // the selector's next change would come later
        const uint64_t at = r.TimeOf("AutoMFG: judging the change at 3x skipped, the game was stalled");
        CHECK_MSG(r.Count("AutoMFG: judging the change at 3x skipped, the game was stalled") == 1 && at >= s + 5000 &&
                      at <= s + 5100,
                  "stall %llu ms: %d lines, at %lld ms after the switch", stallLen,
                  r.Count("AutoMFG: judging the change at"), (long long) at - (long long) s);
        CHECK(r.recoveries == 0 && !r.c.Stopped() && r.c.CostlyCount() == 0);
        CHECK(r.Count("AutoMFG: 3x settled") == 0 && r.Count("applied count") == 0 && r.Count("AutoMFG: inactive") == 0);
        CHECK(r.Find("AutoMFG: judging the change at 3x skipped") != nullptr && !r.Find("AutoMFG: judging")->warn);
        // the selector starts over, at the end of the stall and again when the watch ends as unknown: the next change comes
        // no earlier than its hold after the later of the two
        r.detectedOverride = -1;
        r.Run(20000);
        const uint64_t from = std::max(r.stallUntil, at);
        CHECK_MSG(r.switches.size() >= 2 && r.switches[1].t >= from + 2900, "stall %llu ms: next change %lld ms after it",
                  stallLen, r.switches.size() < 2 ? -1ll : (long long) r.switches[1].t - (long long) from);
    }
    // the fault is judged when the second ends within the five seconds (stall to 3.2 s: calm at about 3.5, judged at 4.5)
    Rig r;
    const uint64_t s = RunToFirstSwitch(r);
    r.detectedOverride = r.prevFactor - 1;
    r.stallFrom = s + 50;
    r.stallUntil = s + 3200;
    r.Run(10000);
    CHECK_MSG(r.recoveries == 1 && r.c.Stopped() && r.Count("skipped, the game was stalled") == 0, "recoveries %d",
              r.recoveries);
}

// An unknown watch is neither a strike nor a clean switch: strike, unknown, strike are still two in a row.
void AnUnknownWatchIsNeitherAStrikeNorACleanSwitch()
{
    Rig r;
    r.recreate = true; // every switch costs one 190 ms frame
    while (r.switches.size() < 1)
        r.Step();
    r.Run(2000);
    CHECK(r.c.CostlyCount() == 1 && !r.c.Stopped());
    while (r.switches.size() < 2) // the next change, 12 s later, meets a long stall
        r.Step();
    r.stallFrom = r.t + 50;
    r.stallUntil = r.t + 9000;
    r.Run(10000);
    CHECK_MSG(r.Count("AutoMFG: judging the change at") == 1 && r.c.CostlyCount() == 1 && !r.c.Stopped(),
              "unknown lines %d, strikes %d, stopped %d", r.Count("AutoMFG: judging the change at"), r.c.CostlyCount(),
              (int) r.c.Stopped());
    r.target = 100.0; // the output is far above this target: the selector goes down, and that switch re-creates DLS-G again
    r.Run(40000);
    CHECK_MSG(r.c.CostlyCount() == 2 && r.c.Stopped() && r.recoveries == 0, "strikes %d stopped %d recoveries %d",
              r.c.CostlyCount(), (int) r.c.Stopped(), r.recoveries);
    CHECK(r.Count("AutoMFG: changing the count recreates DLSS-G on this setup; automatic MFG stopped") == 1);
}

// Decision 5: the self-test starts only after 20 s without a stall (any stall starts the wait over), and a stall during it
// ends it as "interrupted" (the FAIL line with that label, as for a pause), never as a failed re-creation test.
void TheSelfTestWaitsForTwentySecondsOfCalm()
{
    {
        Rig r;
        r.selfTest = true;
        r.Run(19800);
        CHECK(r.Count("AutoMFG self-test: started") == 0);
        r.Run(1500);
        const uint64_t at = r.TimeOf("AutoMFG self-test: started");
        CHECK_MSG(at >= 1000 + 20000 && at <= 1000 + 20100, "started %lld ms after the first tick", (long long) at - 1000);
    }
    {
        Rig r; // a stall in the middle of the wait starts it over
        r.selfTest = true;
        r.stallFrom = 8000;
        r.stallUntil = 9000;
        r.Run(40000);
        const uint64_t at = r.TimeOf("AutoMFG self-test: started");
        CHECK_MSG(at >= r.stallUntil + 20000 && at <= r.stallUntil + 21000, "started %lld ms after the stall",
                  (long long) at - (long long) r.stallUntil);
    }
}

void AStallInterruptsTheSelfTestWithoutAFailure()
{
    for (const uint64_t stallLen : { 2000ull, 6000ull }) // the second one ends the open watch as unknown on the way
    {
        Rig r;
        r.selfTest = true;
        r.factor = 3;
        r.Calm();
        while (r.switches.size() < 3) // three changes of the cycle are made (3x to 2x, 3x, 4x), the last one is being watched
            r.Step();
        r.stallFrom = r.t + 50;
        r.stallUntil = r.t + 50 + stallLen;
        const uint64_t until = r.stallUntil;
        r.Run(stallLen + 9000);
        CHECK_MSG(r.ResultLines() == 1 && r.Count("AutoMFG self-test: FAIL (interrupted: the game stalled)") == 1,
                  "stall %llu ms: %d result lines", stallLen, r.ResultLines());
        CHECK(r.Find("re-created") == nullptr && r.Count("AutoMFG self-test: PASS") == 0);
        CHECK_MSG(r.recoveries == 0 && !r.c.Stopped() && r.c.CostlyCount() == 0, "stall %llu ms: recoveries %d stopped %d",
                  stallLen, r.recoveries, (int) r.c.Stopped());
        CHECK(r.c.SelfTestDone() && r.Count("AutoMFG self-test: started") == 1); // it does not start again
        // the count in use before the test comes back, as a change of its own, and not before the stall is over
        CHECK(r.LinesWith("(back to the count in use before the test)") == 1);
        const uint64_t back = r.TimeOf("(back to the count in use before the test)");
        CHECK_MSG(back > until, "stall %llu ms: restored %lld ms after the stall", stallLen,
                  (long long) back - (long long) until);
        size_t n = 0;
        for (const auto& s : r.switches)
            if (s.t >= until)
            {
                CHECK_MSG(n++ > 0 || s.to == 3, "stall %llu ms: first change after the stall is %dx, not the 3x of before", stallLen,
                          s.to);
            }
    }
}

// A stall while nothing has changed yet (the 3 s of warmup): the test goes back to waiting for a quiet game, it is not over.
void AStallBeforeTheFirstChangeSendsTheSelfTestBackToWaiting()
{
    Rig r;
    r.selfTest = true;
    r.plant.base2x = 8.0;
    r.factor = 3;
    while (r.Count("AutoMFG self-test: started") == 0)
        r.Step();
    r.stallFrom = r.t + 500; // inside the warmup
    r.stallUntil = r.t + 1500;
    r.Run(2500);
    CHECK(r.ResultLines() == 0 && !r.c.SelfTestDone());
    const uint64_t until = r.stallUntil;
    r.Run(120000);
    CHECK_MSG(r.Count("AutoMFG self-test: started") == 2, "started %d times", r.Count("AutoMFG self-test: started"));
    CHECK(r.ResultLines() == 1 && r.Count("AutoMFG self-test: PASS") == 1);
    uint64_t second = 0;
    int seen = 0;
    for (size_t i = 0; i < r.lines.size(); ++i)
        if (r.lines[i].text.rfind("AutoMFG self-test: started", 0) == 0 && ++seen == 2)
            second = r.lineT[i];
    CHECK_MSG(second >= until + 20000, "started again %lld ms after the stall", (long long) second - (long long) until);
}

void SelfTestPasses(int startFactor, int expectedChanges, const char* field)
{
    Rig r;
    r.selfTest = true;
    r.factor = startFactor;
    r.plant.base2x = field[0] == 'r' ? 8.0 : 20.0; // light: pinned at the refresh at 4x; heavy: GPU-bound
    r.Calm();
    const uint64_t t0 = r.t;
    r.Run(60000);
    CHECK_MSG(r.ResultLines() == 1, "%d result lines", r.ResultLines());
    CHECK_MSG(r.Count("AutoMFG self-test: PASS (" + std::to_string(expectedChanges) +
                      " in-place changes, worst interval ") == 1,
              "start %dx", startFactor);
    size_t made = 0; // switches until the result line; the selector may go on afterwards
    for (const auto& s : r.switches)
        made += s.t <= r.doneAt;
    CHECK_MSG(made == (size_t) expectedChanges, "%zu changes", made);
    const int want[] = { 2, 3, 4, 3, 2 };
    for (size_t i = 0; i < made && i < 5; ++i)
        CHECK(r.switches[i].to == want[5 - made + i]);
    CHECK_MSG(r.Find(std::string("-> ") + (field[0] == 'r' ? "refresh-bound" : "GPU-bound")) != nullptr, "field %s", field);
    CHECK(r.Find("AutoMFG self-test: at 4x real interval ") != nullptr);
    CHECK_MSG(r.c.SelfTestDone() && r.doneAt != 0 && r.doneAt < t0 + 45000, "done after %llu ms", r.doneAt - t0);
}

void SelfTestFailures()
{
    {
        Rig r; // re-created: every change stalls
        r.selfTest = true;
        r.factor = 3;
        r.recreate = true;
        r.Calm();
        r.Run(80000);
        CHECK(r.ResultLines() == 1 && r.Find("AutoMFG self-test: FAIL (re-created: worst interval 190 ms") != nullptr);
        CHECK(r.c.CostlyCount() >= 2 && r.c.Stopped());
        // The test failed and stopped the mode: the count in force before it comes back, once, and nothing moves it afterwards
        // (the restore lands through the closed gate on the tick after the stop).
        CHECK_MSG(r.factor == 3, "the failed, stopped self-test left %dx (3x before the test)", r.factor);
        CHECK(r.LinesWith("(back to the count in use before the test)") == 1);
        const size_t n = r.switches.size();
        r.Run(60000);
        CHECK(r.switches.size() == n && r.factor == 3);
    }
    {
        Rig r; // slow but not re-created
        r.selfTest = true;
        r.factor = 3;
        r.recreate = true;
        r.stallMs = 60.0;
        r.Calm();
        r.Run(80000);
        CHECK(r.ResultLines() == 1 &&
              r.Find("AutoMFG self-test: FAIL (worst interval 60 ms, expected under 40") != nullptr);
        CHECK(r.c.CostlyCount() == 0);
    }
    {
        Rig r; // not run: key off
        r.selfTest = true;
        r.gate.keyOn = false;
        r.Run(5000);
        CHECK(r.ResultLines() == 1 && r.Find("AutoMFG self-test: FAIL (not run: AutoMFG is off)") != nullptr);
    }
    {
        Rig r; // not run: maximum below 4x
        r.selfTest = true;
        r.gate.maxGenerated = 2;
        r.Run(5000);
        CHECK(r.ResultLines() == 1 &&
              r.Find("FAIL (not run: the DLSS-G maximum is 3x and the cycle needs 4x") != nullptr);
        Rig q; // not run: nothing to choose between
        q.selfTest = true;
        q.gate.maxGenerated = 1;
        q.Run(5000);
        CHECK(q.ResultLines() == 1 && q.Find("FAIL (not run: the DLSS-G maximum is below 3x") != nullptr);
    }
    {
        Rig r; // interrupted by a reactivation after the first change
        r.selfTest = true;
        r.factor = 3;
        r.Calm();
        while (r.switches.empty())
            r.Step();
        r.c.OnActivated();
        r.Run(60000);
        CHECK(r.ResultLines() == 1 &&
              r.Find("AutoMFG self-test: FAIL (interrupted: frame generation was reactivated)") != nullptr);
    }
    {
        Rig r; // interrupted before the first change: the timeline starts over and the test still ends in one line
        r.selfTest = true;
        r.factor = 3;
        r.Calm();
        r.Run(1500);
        r.c.OnActivated();
        r.Run(80000);
        CHECK(r.ResultLines() == 1 && r.Count("AutoMFG self-test: PASS") == 1);
    }
    {
        Rig r; // the gate closes mid-run
        r.selfTest = true;
        r.factor = 3;
        r.Calm();
        while (r.switches.size() < 2)
            r.Step();
        r.gate.followsGame = true;
        r.Run(5000);
        CHECK(r.ResultLines() == 1 && r.Find("FAIL (interrupted: Frame Gen is on Default") != nullptr);
    }
    {
        Rig r; // the plugin applies another count: Fail B, one recovery
        r.selfTest = true;
        r.factor = 3;
        r.Calm();
        r.detectedOverride = 3;
        r.Run(40000);
        CHECK(r.ResultLines() == 1 && r.Find("AutoMFG self-test: FAIL (applied count 3 != requested 1") != nullptr);
        CHECK(r.recoveries == 1 && r.c.Stopped());
    }
    {
        Rig r; // after a finished self-test the selector works normally
        r.selfTest = true;
        r.factor = 3;
        r.Calm();
        r.Run(60000);
        CHECK(r.Count("AutoMFG self-test: PASS") == 1);
        r.target = 224.0; // the selector is free again
        const size_t n = r.switches.size();
        r.Run(60000);
        CHECK(r.ResultLines() == 1 && r.switches.size() >= n && r.factor == 4);
    }
}

void SelfTestEndStates()
{
    // A normal end: back to the count in force before the test, as the first change after the result line.
    for (int start : { 3, 4 })
    {
        Rig r;
        r.selfTest = true;
        r.factor = start;
        r.plant.base2x = 20.0;
        r.Calm();
        r.Run(60000);
        CHECK(r.Count("AutoMFG self-test: PASS") == 1);
        CHECK(r.Find("(back to the count in use before the test)") != nullptr);
        int first = 0;
        for (const auto& s : r.switches)
            if (s.t > r.doneAt)
            {
                first = s.to;
                break;
            }
        CHECK_MSG(first == start, "start %dx: first change after the result went to %dx", start, first);
    }
    {
        Rig r; // the key goes off while the cycle is at 4x: the count in force before the test comes back
        r.selfTest = true;
        r.factor = 3;
        r.plant.base2x = 20.0;
        r.Calm();
        while (r.factor != 4)
            r.Step();
        r.gate.keyOn = false;
        r.Step();
        CHECK_MSG(r.factor == 3, "key off mid-test at 4x left %dx", r.factor);
        CHECK(r.Find("AutoMFG self-test: FAIL (interrupted: AutoMFG is off)") != nullptr);
        r.Run(20000);
        CHECK(r.factor == 3 && r.ResultLines() == 1);
    }
    {
        Rig r; // the user drags the slider mid-test: interrupted, no restore, the selector owns the next factor
        r.selfTest = true;
        r.factor = 3;
        r.Calm();
        while (r.switches.empty())
            r.Step();
        r.target = 224.0; // the selector owns the count from here
        r.factor = 4;
        r.c.OnActivated();
        r.Run(40000);
        CHECK(r.ResultLines() == 1 && r.Find("FAIL (interrupted: frame generation was reactivated)") != nullptr);
        CHECK(r.Find("back to the count in use before the test") == nullptr);
        CHECK_MSG(r.factor == 4 && r.switches.size() == 1, "factor %dx after %zu switches", r.factor, r.switches.size());
    }
    {
        Rig r; // a broken change in the middle goes back to the count before the test, not to the last cycle step
        r.selfTest = true;
        r.factor = 4;
        r.plant.base2x = 20.0;
        r.Calm();
        while (r.switches.size() < 2)
            r.Step();
        r.detectedOverride = 7;
        r.Run(20000);
        CHECK_MSG(r.recoveries == 1 && r.factor == 4, "recoveries %d, factor %dx", r.recoveries, r.factor);
        CHECK(r.Find("going back to 4x, automatic mode stopped") != nullptr);
    }
}

void FormatsAreStable()
{
    DynamicMfg::Info i;
    i.why = Why::Up;
    i.from = 2;
    i.to = 3;
    i.avgMs = 18.2;
    i.presented = 109.9;
    CHECK(FormatSwitch(i, 224) == "AutoMFG: 2x -> 3x (real 18.2 ms = 55 fps, presented 110, target 224, up)");
    i.why = Why::FastDown;
    i.from = 4;
    i.to = 3;
    CHECK(FormatSwitch(i, 224).find(", down, output far above the target)") != std::string::npos);
    i.why = Why::Probe;
    i.probeMs = 12000;
    CHECK(FormatSwitch(i, 224).find("probe: output stayed at the target for 12 s)") != std::string::npos);
    i.why = Why::ProbeRevert;
    i.probeMs = 24000;
    CHECK(FormatSwitch(i, 224).find("probe failed, presented 110 is under 0.93 x target, next probe in 24 s)") !=
          std::string::npos);
    i.why = Why::RegretRevert;
    i.presentedBefore = 200;
    CHECK(FormatSwitch(i, 224).find("step gained too little: presented 110 against 200 before, 4x refused for 30 s)") !=
          std::string::npos);
    i.why = Why::Clamp;
    CHECK(FormatSwitch(i, 224) == "AutoMFG: 4x -> 3x (outside the allowed range)");
}
} // namespace

int main()
{
    GatingLogsTheReasonOnce();
    CleanSwitchLogsAndSettles();
    FreshControllerProbesOnARefreshBoundScene();
    CostlyOutcomeSlowsDownThenStops();
    ANewChangeIsNotStartedInAStall();
    TheStallThresholdIs30Ms();
    OneLongFrameIsNoStall();
    TransientZeroAtTheVerdictIsNotAFailure();
    ThreeZeroSamplesAtTheEndAreStillAFailure();
    ANonZeroStatusBeforeTheSwitchIsNotTheSwitchsFault();
    ABriefBadStatusIsNotEnough();
    AShortPauseAcrossTheWatchEndIsNotAFailure();
    UnrelatedHitchesDoNotStopTheMode();
    LongFrameLaterInTheWatchIsNotTheSignature();
    TheSignatureHasToRepeatInARow();
    ARecreationOnUpStepsOnlyStillStopsOnTheThirdStrike();
    StrikesAreForgottenAfterTenCleanSwitches();
    NineCleanSwitchesDoNotForgetAStrikeButTenDo();
    BrokenOutcomesRecoverOnceAndStop();
    ActivationAndGapCloseTheWatchWithoutAVerdict();
    GapStartsANewHold();
    ALateCountInAStallIsNoStrike();
    ARealFaultAfterAStallIsStillConvicted();
    ARealFaultAt200FpsIsConvictedAsBefore();
    AWatchThatCannotBeJudgedWithinFiveSecondsEndsAsUnknown();
    AnUnknownWatchIsNeitherAStrikeNorACleanSwitch();
    TheSelfTestWaitsForTwentySecondsOfCalm();
    AStallInterruptsTheSelfTestWithoutAFailure();
    AStallBeforeTheFirstChangeSendsTheSelfTestBackToWaiting();
    SelfTestPasses(3, 5, "refresh");
    SelfTestPasses(4, 5, "gpu");
    SelfTestPasses(2, 4, "refresh");
    SelfTestFailures();
    SelfTestEndStates();
    FormatsAreStable();
    if (g_failures)
    {
        std::printf("dynamic_mfg_control_unit: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("dynamic_mfg_control_unit: all passed\n");
    return 0;
}
