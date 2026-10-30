// The NR frame-rate governor's pure decision core (ext/nr/NrGovernor.h) driven on a simulated clock: the pass
// is modelled the way the throwaway simulations did (a size change parks 500 ms after the tier moves, the retired
// textures release 48 ms later), two evaluations per 16 ms game frame, and the present counter advances with every
// game frame while presents flow. No config, clock or GPU is read: every behaviour below is decided by Tick alone.
#include "ext/nr/NrGovernor.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

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

using namespace DlssNr::Governor;

struct Tick
{
    uint64_t t = 0;
    int tierPct = 0;
    float cap = 0.0f;
    double avg = 0.0;
    double target = 0.0;
    uint64_t upDwell = 0;
    Reason reason = Reason::Off;
};

struct World
{
    Governor g;
    uint64_t t = 0; // simulated ms; starts at 0, proving the unset sentinels work on a zero-based clock
    uint64_t stepMs = 8; // two evaluations per 60 fps frame, like the pre-SR plus post-SR placements
    double rate = 51.0; // displayed fps the world renders at
    double target = 60.0;
    float ceiling = 1.0f;
    bool on = true;
    bool autoMfg = false, perAxis = false, spatial = false;
    // signal: an invalid interval reads as no signal; the frozen value below stands in for a stuck present time
    double intervalOverride = 0.0; // 0 = follow the rate
    bool hasIntervalOverride = false;
    // freshness counter: advances once per 16 ms game frame while presents flow and a feature owns presents
    bool fgFeature = false;
    bool presentsFlow = true;
    uint64_t fgFrame = 100;
    // frame generation state
    bool fgActive = false;
    double multiplier = 0.0;
    bool unsupportedFg = false;
    // VRAM gate: automatic parks release after 48 ms; a scenario can hold them instead
    bool autoVram = true;
    bool vramHeld = false;
    // pass model: the size the model renders now, and the size the governor output asks for
    double renderedEff = -1.0;
    uint64_t parkAt = UINT64_MAX;
    uint64_t releaseAt = UINT64_MAX;
    uint64_t appliedSeq = 0;
    std::vector<Tick> log;

    double Effective(int tierPct, float ceil) const
    {
        return tierPct > 0 ? tierPct / 100.0 : (double) ceil;
    }

    Output Step()
    {
        Input in;
        in.nowMs = t;
        if (hasIntervalOverride)
            in.intervalMs = intervalOverride;
        else
            in.intervalMs = rate > 0.0 ? 1000.0 / rate : 0.0;
        in.fgFrame = fgFrame;
        in.fgFeature = fgFeature;
        in.multiplier = multiplier;
        in.fgActive = fgActive;
        in.unsupportedFg = unsupportedFg;
        in.targetFps = target;
        in.userCeiling = ceiling;
        in.governorOn = on;
        in.autoMfgOn = autoMfg;
        in.perAxisSet = perAxis;
        in.spatialOn = spatial;
        in.texturesReleased = !vramHeld;
        in.appliedSeq = appliedSeq;
        in.appliedWorkW = appliedW_;
        in.appliedWorkH = appliedH_;
        const Output out = g.Tick(in);
        log.push_back({ t, out.tierPct, out.cap, out.measuredFps, out.targetFps, out.upDwellMs, out.reason });
        // the pass parks a changed size 500 ms after the tier moves, and the retired textures release 48 ms later
        const float saneCeil = std::isfinite(in.userCeiling) && in.userCeiling > 0.0f ? in.userCeiling : 1.0f;
        const double want = Effective(out.tierPct, saneCeil);
        if (renderedEff >= 0.0 && want != renderedEff && parkAt == UINT64_MAX)
            parkAt = t + 500;
        if (renderedEff < 0.0)
        {
            renderedEff = want; // first tick adopts what runs, like the running model size
            appliedW_ = (unsigned) (renderedEff * 1000.0);
            appliedH_ = (unsigned) (renderedEff * 1000.0) + 7;
        }
        if (parkAt != UINT64_MAX && t >= parkAt)
        {
            parkAt = UINT64_MAX;
            renderedEff = want;
            ++appliedSeq;
            appliedW_ = (unsigned) (renderedEff * 1000.0);
            appliedH_ = (unsigned) (renderedEff * 1000.0) + 7;
            if (autoVram)
            {
                vramHeld = true;
                releaseAt = t + 48;
            }
        }
        if (releaseAt != UINT64_MAX && t >= releaseAt)
        {
            releaseAt = UINT64_MAX;
            if (autoVram)
                vramHeld = false;
        }
        t += stepMs;
        if (fgFeature && presentsFlow && t % 16 == 0)
            ++fgFrame;
        return out;
    }

    void Run(uint64_t ms)
    {
        const uint64_t end = t + ms;
        while (t < end)
            Step();
    }

    std::vector<uint64_t> StepTimes() const
    {
        std::vector<uint64_t> times;
        for (size_t i = 1; i < log.size(); ++i)
            if (log[i].tierPct != log[i - 1].tierPct)
                times.push_back(log[i].t);
        return times;
    }

    const Tick* AtOrAfter(uint64_t at) const
    {
        for (const auto& e : log)
            if (e.t >= at)
                return &e;
        return nullptr;
    }

    unsigned appliedW_ = 1000, appliedH_ = 1007;
};

const char* Name(Reason r)
{
    switch (r)
    {
    case Reason::Off: return "Off";
    case Reason::Full: return "Full";
    case Reason::Holding: return "Holding";
    case Reason::BelowTarget: return "BelowTarget";
    case Reason::AboveTarget: return "AboveTarget";
    case Reason::AtFloor: return "AtFloor";
    case Reason::BelowRange: return "BelowRange";
    case Reason::TexturesPending: return "TexturesPending";
    case Reason::AutoMfg: return "AutoMfg";
    case Reason::PerAxis: return "PerAxis";
    case Reason::Spatial: return "Spatial";
    case Reason::NoTarget: return "NoTarget";
    case Reason::NoSignal: return "NoSignal";
    case Reason::WaitingForCount: return "WaitingForCount";
    case Reason::UnsupportedFg: return "UnsupportedFg";
    }
    return "?";
}

// A below-target rate steps down after 3 s, keeps a cadence of at least 3 s between steps, reaches the 50% floor
// and then holds it instead of stepping further. The zero-based clock proves the unset sentinels, not a zero time.
void DownStepsAndFloor()
{
    World w;
    w.rate = 51.0; // 0.85x a 60 fps target
    w.Run(40000);
    const std::vector<uint64_t> steps = w.StepTimes();
    CHECK(steps.size() == 5); // 90, 80, 70, 60, 50
    if (steps.size() == 5)
    {
        CHECK(steps[0] == 4512); // 1 s of dropped transition samples, 0.5 s of seed, 3 s of dwell
        CHECK(w.log.size() > 1 && w.AtOrAfter(steps[0])->tierPct == 90);
        for (size_t i = 1; i < steps.size(); ++i)
            CHECK(steps[i] - steps[i - 1] >= 3000); // never faster than the down dwell
        const Tick* last = w.AtOrAfter(steps.back());
        CHECK(last != nullptr && last->tierPct == 50);
        const Tick* end = &w.log.back();
        CHECK(end->tierPct == 50 && end->reason == Reason::AtFloor); // the floor holds with the reason said
        CHECK(end->avg >= 50.0 && end->avg <= 52.0); // the average tracks the pinned rate
        for (size_t i = 0; i + 1 < steps.size(); ++i)
            CHECK(w.AtOrAfter(steps[i])->tierPct == 90 - (int) i * 10);
    }
    for (size_t i = 1; i < w.log.size(); ++i)
        CHECK(w.log[i].t > w.log[i - 1].t); // simulated time runs forward
}

// A rate pinned at 0.99x the target holds for a full minute: the down step fires only below 0.95x.
void HoldsAtNinetyNinePercent()
{
    World w;
    w.rate = 59.4;
    w.Run(62000);
    CHECK(w.StepTimes().empty()); // never stepped in 62 s at 0.99x target
    const Tick* end = &w.log.back();
    CHECK(end->tierPct == 0 && end->cap == 0.0f); // the top state is no cap at all
    CHECK(end->avg >= 58.5 && end->avg <= 60.3); // the measured rate tracks the pin
}

// The down edge is exact: 0.95x holds, just under it steps after the dwell.
void DownThreshold()
{
    World hold;
    hold.rate = 57.06; // just above the 0.95 line
    hold.Run(30000);
    CHECK(hold.StepTimes().empty());
    CHECK(hold.log.back().reason == Reason::Full); // at the top inside the dead zone the numbers speak

    World step;
    step.rate = 56.94; // just under 0.95x
    step.Run(8000);
    const std::vector<uint64_t> steps = step.StepTimes();
    CHECK(steps.size() == 1 && steps[0] >= 4400 && steps[0] <= 4700);
}

// Between stepping down (below 0.95x) and probing up (at or above 0.97x) the tier holds, banking neither dwell:
// a 0.96x rate steps neither way, and a dwell banked before the dead zone does not survive it.
void DeadZone()
{
    World w;
    w.rate = 51.0;
    w.Run(5000); // one step down (90), banking nothing else yet
    CHECK(w.log.back().tierPct == 90);
    w.rate = 57.6; // 0.96x: inside the dead zone
    w.Run(30000);
    CHECK(w.StepTimes().size() == 1); // no probe, no step for 30 s
    CHECK(w.log.back().tierPct == 90 && w.log.back().reason == Reason::Holding);
    w.rate = 51.0; // back below: the full dwell runs again, not the banked remainder
    const uint64_t resume = w.t;
    w.Run(7000);
    const std::vector<uint64_t> steps = w.StepTimes();
    CHECK(steps.size() == 2);
    if (steps.size() == 2)
        // no transition restarts here (nothing applied), so the step is the whole dwell after the resume
        CHECK(steps[1] - resume >= 3000 && steps[1] - resume <= 3400);
}

// The up arm is exact: 0.969x never probes, 0.97x probes after the up dwell and keeps the probe when the rate holds.
void UpThreshold()
{
    World cold;
    cold.rate = 51.0;
    cold.Run(5000);
    CHECK(cold.log.back().tierPct == 90);
    cold.rate = 58.14; // 0.969x: under the probe-keep line
    cold.Run(40000);
    CHECK(cold.StepTimes().size() == 1); // never probes in 40 s

    World probe;
    probe.rate = 51.0;
    probe.Run(5000);
    probe.rate = 58.26; // just above the 0.97 probe-keep line
    probe.Run(30000);
    const std::vector<uint64_t> steps = probe.StepTimes();
    CHECK(steps.size() == 3); // down to 90, the probe to 100, then the probe home to the user value
    if (steps.size() == 3)
    {
        CHECK(probe.AtOrAfter(steps[0])->tierPct == 90);
        CHECK(steps[0] == 4512);
        CHECK(probe.AtOrAfter(steps[1])->tierPct == 100); // the probe climbs one tier
        CHECK(steps[1] == 16536); // after the up dwell, not before it
        CHECK(probe.AtOrAfter(steps[2])->tierPct == 0); // from 100 the probe goes home
        CHECK(probe.AtOrAfter(steps[2])->cap == 0.0f);
        CHECK(steps[2] == 30552);
        CHECK(probe.log.back().tierPct == 0); // the held rate keeps every verdict
        CHECK(probe.log.back().upDwell == kUpDwellBaseMs);
        CHECK(probe.log.back().reason == Reason::Full); // the top state never probes again
    }
}

// A failed probe reverts to the tier it came from and doubles the next wait: the probe fires while the rate sits
// at the target (the arm is the probe-keep line, not a band above it) and the verdict fails on a 0.92x rate.
void ProbeFiresAtTargetAndReverts()
{
    World w;
    w.rate = 51.0;
    w.Run(5000); // one step down to 90
    CHECK(w.log.back().tierPct == 90);
    w.rate = 60.0; // pinned at the target: the probe still fires
    const uint64_t armed = w.t;
    int guard = 0;
    while (w.log.back().tierPct == 90 && w.t - armed < 60000 && guard++ < 10000)
        w.Step();
    CHECK(w.log.back().tierPct == 100); // the probe climbs one tier
    const uint64_t probeAt = w.t;
    CHECK(probeAt - armed >= 10000 && probeAt - armed <= 16000); // after the up dwell, not before
    w.rate = 55.0; // 0.917x under judgment: the tier cannot hold the target
    guard = 0;
    while (w.log.back().tierPct == 100 && w.t - probeAt < 20000 && guard++ < 10000)
        w.Step();
    CHECK(w.log.back().tierPct == 90); // back to the tier the probe came from
    CHECK(w.log.back().upDwell == 20000); // the next probe waits twice as long
    // the doubled wait holds: no climb within 15 s of the revert, then the probe fires again on the doubled dwell
    const uint64_t reverted = w.t;
    w.rate = 60.0;
    w.Run(15000);
    CHECK(w.log.back().tierPct == 90);
    guard = 0;
    while (w.log.back().tierPct == 90 && w.t - reverted < 60000 && guard++ < 10000)
        w.Step();
    CHECK(w.log.back().tierPct == 100); // the second probe fires on the doubled wait
    CHECK(w.t - reverted >= 20000 && w.t - reverted <= 26000);
}

// Every failed probe doubles the wait until the 96 s cap; a kept probe and a ceiling change reset it to the base.
void ProbeBackoffDoublesCapsAndResets()
{
    World w;
    w.rate = 51.0;
    w.Run(30000); // to the 50 floor
    CHECK(w.log.back().tierPct == 50);
    const uint64_t expected[] = { 20000, 40000, 80000, 96000, 96000 };
    for (int i = 0; i < 5; ++i)
    {
        w.rate = 70.0; // well above the target: every probe is armed by the dwell in force
        const int from = w.log.back().tierPct;
        const uint64_t waitStart = w.t;
        int guard = 0;
        while (w.log.back().tierPct == from && w.t - waitStart < 200000 && guard++ < 30000)
            w.Step();
        CHECK(w.log.back().tierPct == from + 10); // the probe climbs one tier
        // collapse under judgment so the verdict fails
        w.rate = 40.0;
        const int probed = w.log.back().tierPct;
        const uint64_t judgeStart = w.t;
        guard = 0;
        while (w.log.back().tierPct == probed && w.t - judgeStart < 30000 && guard++ < 10000)
            w.Step();
        CHECK(w.log.back().tierPct == from); // the failed probe reverts
        CHECK(w.log.back().upDwell == expected[i]);
    }
    // a kept probe resets the doubled wait to the base
    w.rate = 70.0;
    {
        const int from = w.log.back().tierPct;
        const uint64_t waitStart = w.t;
        int guard = 0;
        while (w.log.back().tierPct == from && w.t - waitStart < 200000 && guard++ < 30000)
            w.Step();
        CHECK(w.log.back().tierPct == from + 10);
        const uint64_t probeAt = w.t;
        guard = 0;
        while (w.t - probeAt < 8000 && guard++ < 2000)
            w.Step(); // the 70 fps rate holds through the whole judgment
        CHECK(w.log.back().tierPct == from + 10); // kept: no revert
        CHECK(w.log.back().upDwell == kUpDwellBaseMs); // the wait is back at its base
    }
    // a ceiling change resets the doubled wait too, and clamps an overhanging tier at once
    World v;
    v.rate = 51.0;
    v.Run(8000); // one step down to 90
    v.rate = 70.0;
    {
        const uint64_t waitStart = v.t;
        int guard = 0;
        while (v.log.back().tierPct == 90 && v.t - waitStart < 60000 && guard++ < 10000)
            v.Step();
        CHECK(v.log.back().tierPct == 100);
        v.rate = 55.0; // fail the verdict
        const uint64_t judgeStart = v.t;
        guard = 0;
        while (v.log.back().tierPct == 100 && v.t - judgeStart < 30000 && guard++ < 10000)
            v.Step();
        CHECK(v.log.back().tierPct == 90 && v.log.back().upDwell == 20000);
    }
    v.ceiling = 0.8f; // a new regime: the tier clamps at once and the back-off is dropped
    v.Step();
    CHECK(v.log.back().tierPct == 80);
    CHECK(v.log.back().upDwell == kUpDwellBaseMs);
    CHECK(v.log.back().avg == 0.0); // the smoothing restarts with the ceiling
}

// The first step from the top goes to the highest tier strictly below the ceiling: 150% steps to 100%, an off-grid
// 85% to 80%, exactly 50% to the 50% tier, and a ceiling below the floor stands down as below range, uncapped.
void FirstStepAndCeilings()
{
    World hi;
    hi.ceiling = 1.5f;
    hi.rate = 42.0;
    hi.Run(12000);
    const std::vector<uint64_t> hiSteps = hi.StepTimes();
    CHECK(hiSteps.size() == 2);
    if (hiSteps.size() == 2)
    {
        CHECK(hi.AtOrAfter(hiSteps[0])->tierPct == 100); // the 100% tier is not skipped
        CHECK(hi.AtOrAfter(hiSteps[0])->cap == 1.0f);
        CHECK(hi.AtOrAfter(hiSteps[1])->tierPct == 90);
    }
    World off;
    off.ceiling = 0.85f;
    off.rate = 40.0;
    off.Run(12000);
    const std::vector<uint64_t> offSteps = off.StepTimes();
    CHECK(offSteps.size() == 2 && off.AtOrAfter(offSteps[0])->tierPct == 80);

    World exact;
    exact.ceiling = 0.5f;
    exact.rate = 40.0;
    exact.Run(12000);
    CHECK(exact.StepTimes().size() == 1); // exactly 50% steps to the 50% tier, which equals the ceiling
    CHECK(exact.log.back().tierPct == 50);

    World below;
    below.ceiling = 0.49f;
    below.rate = 40.0;
    below.Run(20000);
    CHECK(below.StepTimes().empty()); // below the floor the governor cannot step: uncapped
    CHECK(below.log.back().reason == Reason::BelowRange);
    CHECK(below.log.back().cap == 0.0f && below.log.back().tierPct == 0);

    World nan;
    nan.ceiling = std::numeric_limits<float>::quiet_NaN(); // a bad ceiling reads as 100%
    nan.rate = 42.0;
    nan.Run(12000);
    CHECK(nan.StepTimes().size() == 2 && nan.AtOrAfter(nan.StepTimes()[0])->tierPct == 90);
    World zero;
    zero.ceiling = 0.0f;
    zero.rate = 42.0;
    zero.Run(12000);
    CHECK(zero.StepTimes().size() == 2 && zero.AtOrAfter(zero.StepTimes()[0])->tierPct == 90);

    CHECK(Governor::FirstStepForCeiling(1.5f) == 0);
    CHECK(Governor::FirstStepForCeiling(1.0f) == 1);
    CHECK(Governor::FirstStepForCeiling(0.85f) == 2);
    CHECK(Governor::FirstStepForCeiling(0.5f) == 5);
    CHECK(Governor::FirstStepForCeiling(0.49f) == -1);
}

// Off is exactly the user value: no cap, and switching back on restarts the whole transition.
void GovernorOff()
{
    World w;
    w.rate = 40.0;
    w.Run(12000);
    CHECK(w.log.back().tierPct == 80);
    w.on = false;
    w.Run(3000);
    const Tick* end = &w.log.back();
    CHECK(end->reason == Reason::Off && end->cap == 0.0f && end->tierPct == 0);
    w.on = true; // back on: the ceiling is re-adopted, so nothing steps for a full transition
    const uint64_t back = w.t;
    w.Run(4000);
    CHECK(w.log.back().tierPct == 0);
    const std::vector<uint64_t> steps = w.StepTimes();
    CHECK(steps.back() - back >= 4400 || steps.size() == 2); // the re-armed governor steps on its own clock
}

// A ceiling change clamps an overhanging tier at once, restarts the smoothing and the dwells, and drops the next
// samples: no step banks the change.
void CeilingChange()
{
    World w;
    w.rate = 51.0;
    w.Run(16000); // down to 70
    CHECK(w.log.back().tierPct == 70);
    w.ceiling = 0.65f; // 70% overhangs the new ceiling: clamped to 60% on this tick
    w.Step();
    CHECK(w.log.back().tierPct == 60);
    CHECK(w.log.back().avg == 0.0);
    const uint64_t changed = w.t;
    w.Run(4000);
    CHECK(w.log.back().tierPct == 60); // the dwell and the transition restart: nothing instant
    w.Run(8000);
    const std::vector<uint64_t> steps = w.StepTimes();
    CHECK(!steps.empty() && steps.back() - changed >= 4400);
    // a ceiling that still covers the tier leaves it alone but still restarts the smoothing
    w.ceiling = 0.7f;
    w.Step();
    CHECK(w.log.back().tierPct == 50);
    CHECK(w.log.back().avg == 0.0); // a new ceiling is a new regime even when the tier fits
}

// The four real stand-downs reset to the top state at once: another controller owns the frame rate, or the governor
// must not touch the scale at all. Coming back restarts the transition, never steps instantly.
void RealStandDownsReset()
{
    for (int which = 0; which < 4; ++which)
    {
        World w;
        w.rate = 51.0;
        w.Run(8000); // one step down to 90
        CHECK(w.log.back().tierPct == 90);
        if (which == 0)
            w.autoMfg = true;
        else if (which == 1)
            w.perAxis = true;
        else if (which == 2)
            w.spatial = true;
        else
            w.on = false;
        w.Step();
        const Tick* down = &w.log.back();
        CHECK(down->tierPct == 0 && down->cap == 0.0f); // uncapped at once
        CHECK(down->avg == 0.0); // the average is dropped with the tier
        CHECK(down->upDwell == kUpDwellBaseMs);
        if (which == 0)
        {
            CHECK(down->reason == Reason::AutoMfg);
            w.autoMfg = false;
        }
        else if (which == 1)
        {
            CHECK(down->reason == Reason::PerAxis);
            w.perAxis = false;
        }
        else if (which == 2)
        {
            CHECK(down->reason == Reason::Spatial);
            w.spatial = false;
        }
        else
        {
            CHECK(down->reason == Reason::Off);
            w.on = true;
        }
        const uint64_t back = w.t;
        w.Run(4000);
        CHECK(w.log.back().tierPct == 0); // the transition restarts: nothing steps on stale timers
        (void) back;
    }
}

// A transient stand-down freezes the tier and pauses every timer past the gap: the banked dwell survives. Each of
// the four transient reasons is paired against an uninterrupted control run; the step lands exactly the gap later,
// which a restart would not do.
void TransientFreezeKeepsTier()
{
    for (int which = 0; which < 4; ++which)
    {
        World control;
        control.rate = 51.0;
        control.Run(30000);
        const std::vector<uint64_t> controlSteps = control.StepTimes();

        World w;
        w.rate = 51.0;
        w.Run(2500); // banks about a second of the first down dwell (the transition takes the first 1.5 s)
        if (which == 0)
        {
            w.fgActive = true; // generating, count not observed: WaitingForCount
            w.multiplier = 0.0;
        }
        else if (which == 1)
        {
            w.unsupportedFg = true; // a non-DLSS-G output while generating
            w.fgActive = true;
            w.multiplier = 2.0;
        }
        else if (which == 2)
        {
            w.target = 0.0; // no target: neither a manual value nor a limit/refresh to take it from
        }
        else
        {
            w.hasIntervalOverride = true; // a stuck present time: no frame-time signal
            w.intervalOverride = 0.0;
        }
        const uint64_t gapStart = w.t;
        w.Run(1000); // 1 s of transient
        const Reason why = w.log.back().reason;
        CHECK(w.log.back().tierPct == 0); // the top state holds through the transient
        if (which == 0)
            CHECK(why == Reason::WaitingForCount);
        else if (which == 1)
            CHECK(why == Reason::UnsupportedFg);
        else if (which == 2)
            CHECK(why == Reason::NoTarget);
        else
            CHECK(why == Reason::NoSignal);
        if (which == 0)
        {
            w.fgActive = false;
            w.multiplier = 0.0;
        }
        else if (which == 1)
        {
            w.unsupportedFg = false;
            w.fgActive = false;
        }
        else if (which == 2)
        {
            w.target = 60.0;
        }
        else
        {
            w.hasIntervalOverride = false;
        }
        w.Run(30000);
        const std::vector<uint64_t> steps = w.StepTimes();
        CHECK(!steps.empty() && !controlSteps.empty());
        if (!steps.empty() && !controlSteps.empty())
            // the pause shifted every timer past the gap: the step lands the gap later, to the tick
            CHECK(steps[0] >= controlSteps[0] + 992 && steps[0] <= controlSteps[0] + 1008);
        (void) gapStart;
    }
}

// While the previous size's retired textures still wait for the GPU the tier holds everywhere, including the top
// state; the average freezes with it. Fences that never resolve freeze the tier: the fail-safe.
void VramHoldFreezesTier()
{
    World w;
    w.rate = 51.0;
    w.autoVram = false;
    w.vramHeld = true; // the gate holds from the very first tick
    w.Run(20000);
    CHECK(w.StepTimes().empty()); // no step while the gate holds, not even the first one
    CHECK(w.log.back().reason == Reason::TexturesPending);
    w.vramHeld = false; // the fences pass: regulation resumes on the live average, not with a jump
    const uint64_t released = w.t;
    w.Run(2500);
    CHECK(w.log.back().tierPct == 0); // the dwell was paused, not banked: nothing instant
    w.Run(3000);
    CHECK(w.log.back().tierPct == 90);
    const std::vector<uint64_t> steps = w.StepTimes();
    CHECK(!steps.empty() && steps[0] - released >= 2500 && steps[0] - released <= 4000);

    World mid;
    mid.rate = 51.0;
    mid.Run(6000); // one step down to 90
    CHECK(mid.log.back().tierPct == 90);
    mid.vramHeld = true; // hold the gate for 10 s mid-run
    mid.autoVram = false;
    mid.Run(5000);
    CHECK(mid.StepTimes().size() == 1); // nothing further while the gate holds
    CHECK(mid.log.back().reason == Reason::TexturesPending);
    const double heldAvg = mid.log.back().avg;
    mid.rate = 70.0; // the world improves while the gate holds
    mid.Run(5000);
    CHECK(mid.StepTimes().size() == 1); // still nothing: the verdict waits for the release
    CHECK(mid.log.back().avg == heldAvg); // every frozen tick feeds zero time into the average
    mid.vramHeld = false; // the release unfreezes the average onto the live rate: no step down first
    mid.Run(6000);
    CHECK(mid.log.back().tierPct == 90); // half a second of the frozen average banks no dwell
    mid.Run(20000); // the live 70 fps rate probes back up once the size releases
    mid.Run(30000); // the live 70 fps rate probes back up once the size releases
    CHECK(mid.StepTimes().size() == 3); // 90, the probe to 100, then the probe home to the user value
    CHECK(mid.log.back().tierPct == 0);
    CHECK(mid.log.back().cap == 0.0f);
    CHECK(mid.log.back().upDwell == kUpDwellBaseMs);
}

// Freshness is a present counter, not a timestamp: an advancing counter never reads stale while a feature owns
// presents, a frozen one reads no-signal within a few ticks and holds the tier, and regulation resumes on flow.
// With no feature there is no check: an invalid interval is already no signal.
void CounterFreshness()
{
    World flow;
    flow.fgFeature = true; // a feature owns presents
    flow.rate = 51.0;
    flow.Run(12000);
    CHECK(flow.StepTimes().size() == 2); // an advancing counter regulates normally: no false stale
    CHECK(flow.log.back().tierPct == 80);

    World w;
    w.fgFeature = true;
    w.rate = 51.0;
    w.Run(6000); // one step down to 90
    CHECK(w.log.back().tierPct == 90);
    w.presentsFlow = false; // the signal freezes while presents stop advancing it
    w.Run(2000);
    CHECK(w.log.back().reason == Reason::NoSignal); // stale within a few ticks, not after a timeout
    CHECK(w.log.back().tierPct == 90); // the tier holds while the counter is frozen
    const size_t held = w.StepTimes().size();
    w.Run(8000);
    CHECK(w.StepTimes().size() == held); // held for 10 s, not a step
    w.presentsFlow = true; // presents flow again
    w.Run(6000);
    CHECK(w.StepTimes().size() > held); // regulation resumes without a fresh transition
    CHECK(w.log.back().tierPct == 80);
}

// A pass pause over a second reads no-signal instead of stepping on stale timers: every timer shifts past the gap
// and the rate reseeds from what renders after it.
void PassGap()
{
    World w;
    w.rate = 51.0;
    w.Run(2500); // banks about a second of the first down dwell
    const uint64_t before = w.t;
    w.t += 5000; // the pass does not tick for 5 s: focus pause, bridge suspended, loading
    w.Step(); // this tick reads the gap
    CHECK(w.log.back().reason == Reason::NoSignal);
    CHECK(w.log.back().tierPct == 0); // no step from stale timers
    const uint64_t resumed = w.t;
    w.Run(4000);
    CHECK(w.log.back().tierPct == 0); // the reseed runs first: nothing banks the pause
    w.Run(4000);
    const std::vector<uint64_t> steps = w.StepTimes();
    CHECK(steps.size() == 1); // regulation resumes with one step
    CHECK(steps[0] - resumed >= 4400 && steps[0] - resumed <= 4700); // the gap plus reseed plus dwell
    (void) before;
    // a short gap is not a pause: 900 ms between ticks ticks normally
    World short_;
    short_.rate = 51.0;
    short_.Run(2500);
    short_.t += 900;
    short_.Step();
    CHECK(short_.log.back().reason != Reason::NoSignal);
    short_.Run(30000);
    CHECK(short_.StepTimes().size() == 5); // stepped all the way down on schedule
}

// The transition window is counted in ms on the tick clock, not in evaluations: the pre-SR plus post-SR double
// tick cannot shorten it, and hitch samples inside it change nothing.
void TransitionWindowInMs()
{
    World single; // one evaluation per 16 ms game frame
    single.stepMs = 16;
    single.rate = 42.0;
    single.Run(30000);
    World dbl; // two evaluations per frame
    dbl.stepMs = 8;
    dbl.rate = 42.0;
    dbl.Run(30000);
    {
        const std::vector<uint64_t> slow = single.StepTimes(), fast = dbl.StepTimes();
        CHECK(slow.size() == 5 && fast.size() == 5);
        for (size_t i = 0; i < 5 && i < slow.size() && i < fast.size(); ++i)
        {
            // the double tick never steps earlier, and never a window later: counting evaluations instead
            // of ms would halve every window on the pre-SR plus post-SR path and halve the step count
            CHECK(slow[i] >= fast[i] && slow[i] - fast[i] < 500);
        }
    }

    World hitch; // 200 ms hitches right after each step, inside the transition window
    hitch.rate = 42.0;
    hitch.Run(30000);
    const std::vector<uint64_t> clean = hitch.StepTimes();
    World h;
    h.rate = 42.0;
    // drive manually: after every step, feed 200 ms samples for 300 ms, then continue at 42 fps
    uint64_t lastTierChange = 0;
    bool inHitch = false;
    uint64_t hitchEnd = 0;
    size_t seenSteps = 0;
    const uint64_t end = 30000;
    while (h.t < end)
    {
        if (h.log.size() > 1 && h.log.back().tierPct != h.log[h.log.size() - 2].tierPct && !inHitch)
        {
            ++seenSteps;
            inHitch = true;
            hitchEnd = h.t + 300;
            lastTierChange = h.t;
        }
        if (inHitch)
        {
            if (h.t >= hitchEnd)
                inHitch = false;
            else
            {
                h.hasIntervalOverride = true;
                h.intervalOverride = 200.0; // a rebuild hitch inside the transition window
                h.Step();
                h.hasIntervalOverride = false;
                continue;
            }
        }
        h.Step();
    }
    CHECK(seenSteps >= 3);
    (void) lastTierChange;
    CHECK(h.StepTimes() == clean); // hitch samples inside the window change nothing
}

// The smoothing clips both ways to a third/3x of the running average, never seeds from one raw sample, and never
// moves on a pause: a lone fast frame is not a faster scene, and a loading screen is not a slower one.
void SmoothingClipSeedAndPause()
{
    World w;
    w.rate = 51.0;
    w.Run(6000); // one step down to 90, the average settled near 51
    CHECK(w.log.back().tierPct == 90);
    const double settled = w.log.back().avg;
    CHECK(settled > 50.0 && settled < 52.0);
    // a lone frame 4x faster than the average counts at most 3x: the average barely moves, the tier holds
    w.hasIntervalOverride = true;
    w.intervalOverride = 1000.0 / (settled * 4.0);
    w.Step();
    w.hasIntervalOverride = false;
    CHECK(w.log.back().tierPct == 90);
    CHECK(w.log.back().avg < settled * 1.03); // clipped to 3x: one sample cannot lift the average by 3%
    // a lone frame 4x slower counts at least a third: the average barely moves, the tier holds
    w.intervalOverride = 1000.0 / (settled / 4.0);
    w.hasIntervalOverride = true;
    w.Step();
    w.hasIntervalOverride = false;
    CHECK(w.log.back().tierPct == 90);
    CHECK(w.log.back().avg > settled * 0.99);
    // a slower sample past 250 ms is a pause, never a frame time: the tier holds and every timer freezes
    w.intervalOverride = 300.0;
    w.hasIntervalOverride = true;
    const uint64_t paused = w.t;
    w.Run(2000);
    w.hasIntervalOverride = false;
    CHECK(w.log.back().reason == Reason::NoSignal);
    CHECK(w.log.back().tierPct == 90);
    CHECK(w.StepTimes().size() == 1); // two banked seconds of dwell did not step
    (void) paused;
    // an empty seed window extends instead of seeding from one raw sample: pauses covering the whole
    // window leave the average at zero and step nothing, and real samples seed it afterwards
    World seed;
    seed.rate = 51.0;
    seed.Run(500); // into the discard, before the seed window opens
    seed.hasIntervalOverride = true;
    seed.intervalOverride = 300.0; // pauses through the discard and the whole seed window
    seed.Run(5000);
    seed.hasIntervalOverride = false;
    CHECK(seed.log.back().avg == 0.0); // never seeded: the window waited for real samples
    CHECK(seed.StepTimes().empty());
    seed.Run(8000);
    CHECK(seed.StepTimes().size() == 1); // real samples seed it, then the dwell runs
    // a pause starting mid-seed extends the window past the pause instead of completing into it
    World seed2;
    seed2.rate = 51.0;
    seed2.Run(1100); // the discard ends, the seed window opens on real samples
    const uint64_t seedStart = seed2.t;
    seed2.hasIntervalOverride = true;
    seed2.intervalOverride = 300.0;
    seed2.Run(2000); // a 2 s pause inside the seed window
    seed2.hasIntervalOverride = false;
    uint64_t seededAt = 0;
    for (const auto& e : seed2.log)
        if (e.avg > 0.0)
        {
            seededAt = e.t;
            break;
        }
    CHECK(seededAt - seedStart >= 2000); // the window extended past the pause
    seed2.Run(7000);
    CHECK(seed2.StepTimes().size() == 1);
}

// The displayed rate multiplies the present rate while generating, the count is required only while generating,
// and an idle feature regulates on the present rate with a multiplier of 1.
void FrameGenerationMultiplier()
{
    World gen; // DLSS-G x2 at a 30 fps present rate: displayed 60, at a 60 fps target
    gen.fgFeature = true;
    gen.fgActive = true;
    gen.multiplier = 2.0;
    gen.rate = 30.0;
    gen.Run(60000);
    CHECK(gen.StepTimes().empty()); // 60 displayed holds the target
    CHECK(gen.log.back().avg >= 58.0 && gen.log.back().avg <= 62.0);

    World waiting; // generating but the count not observed yet: the tier is kept, the reason said
    waiting.fgFeature = true;
    waiting.fgActive = true;
    waiting.multiplier = 0.0;
    waiting.rate = 51.0;
    waiting.Run(20000);
    CHECK(waiting.StepTimes().empty());
    CHECK(waiting.log.back().reason == Reason::WaitingForCount);

    World idle; // selected but idle: the multiplier field is ignored, the present rate rules
    idle.fgFeature = true;
    idle.fgActive = false;
    idle.multiplier = 7.0;
    idle.rate = 51.0;
    idle.Run(12000);
    CHECK(idle.StepTimes().size() == 2); // regulates on 51 fps, not on 7x51
    CHECK(idle.log.back().tierPct == 80);
}

// A down step waits while a probe is open: a collapse under judgment fails the verdict, it never steps down
// through the probe. A late applied signal mid-judge restarts the verdict on the new size.
void ProbeOwnsTheVerdict()
{
    World w;
    w.rate = 51.0;
    w.Run(5000); // one step down to 90
    w.rate = 70.0;
    const uint64_t armed = w.t;
    int guard = 0;
    while (w.log.back().tierPct == 90 && w.t - armed < 60000 && guard++ < 10000)
        w.Step(); // the probe climbs to 100
    CHECK(w.log.back().tierPct == 100);
    w.rate = 40.0; // collapse while the verdict is open
    const uint64_t probeAt = w.t;
    guard = 0;
    while (w.log.back().tierPct == 100 && w.t - probeAt < 30000 && guard++ < 10000)
        w.Step();
    CHECK(w.log.back().tierPct == 90); // the verdict reverts: exactly one step back, never two
    const uint64_t reverted = w.t;
    CHECK(w.log.back().upDwell == 20000);
    w.rate = 40.0;
    w.Run(4000); // the revert is itself a step: its own transition owns the next samples
    CHECK(w.log.back().tierPct == 90); // no instant second step on the still-low rate
    (void) reverted;

    World late; // the judge restarts when a late applied signal lands mid-judge
    late.rate = 51.0;
    late.Run(5000);
    late.rate = 70.0;
    guard = 0;
    const uint64_t armedAt = late.t;
    while (late.log.back().tierPct == 90 && late.t - armedAt < 60000 && guard++ < 10000)
        late.Step();
    CHECK(late.log.back().tierPct == 100);
    const uint64_t climbed = late.t;
    late.rate = 40.0; // collapse into what would be the judge window
    late.Run(1000);
    ++late.appliedSeq; // a late size change is parked: the verdict restarts on the new size
    late.appliedW_ += 11;
    late.appliedH_ += 11;
    late.rate = 70.0; // the new size holds the target
    const uint64_t bumped = late.t;
    late.Run(3000); // past the original verdict time: no revert on the old window
    CHECK(late.AtOrAfter(climbed + 3600)->tierPct == 100);
    late.Run(8000);
    CHECK(late.log.back().tierPct == 100); // the restarted judge runs the full window on the new size: kept
    CHECK(late.log.back().upDwell == kUpDwellBaseMs);
    (void) bumped;
}

// A probe from the 100% tier with a ceiling above 100% goes home to the uncapped user value, and the verdict
// keeps it when the rate holds; from 100% with a 100% ceiling the same-size probe trivially keeps.
void ProbeFromFullTier()
{
    World w;
    w.ceiling = 1.5f; // first step down goes to the 100% tier, not past it
    w.rate = 42.0;
    w.Run(6000);
    CHECK(w.log.back().tierPct == 100);
    w.rate = 70.0; // well above the target: the probe goes home after the up dwell
    const uint64_t armed = w.t;
    int guard = 0;
    while (w.log.back().tierPct == 100 && w.t - armed < 60000 && guard++ < 10000)
        w.Step();
    CHECK(w.log.back().tierPct == 0); // home: the top state is exactly the user value
    CHECK(w.log.back().cap == 0.0f);
    const uint64_t home = w.t;
    guard = 0;
    while (w.t - home < 8000 && guard++ < 2000)
        w.Step();
    CHECK(w.log.back().tierPct == 0); // the verdict keeps the home tier: no revert
    CHECK(w.log.back().upDwell == kUpDwellBaseMs);

    World same; // the ceiling drops to exactly the 100% tier, then the same-size probe still resolves
    same.ceiling = 1.5f;
    same.rate = 42.0;
    same.Run(6000);
    CHECK(same.log.back().tierPct == 100);
    same.ceiling = 1.0f; // the tier equals the new ceiling, so it stays
    same.Step();
    CHECK(same.log.back().tierPct == 100);
    same.rate = 70.0;
    const uint64_t armedAt = same.t;
    guard = 0;
    while (same.log.back().tierPct == 100 && same.t - armedAt < 60000 && guard++ < 10000)
        same.Step();
    CHECK(same.log.back().tierPct == 0);
    guard = 0;
    const uint64_t homeAt = same.t;
    while (same.t - homeAt < 8000 && guard++ < 2000)
        same.Step();
    CHECK(same.log.back().tierPct == 0); // no size change, so the verdict trivially keeps
    CHECK(same.log.back().upDwell == kUpDwellBaseMs);
}

// The constants the dwells, windows and edges are built on, and the status line over every reason.
void ConstantsAndStatus()
{
    CHECK(kTierCount == 6);
    CHECK(kTiers[0] == 1.0f && kTiers[5] == 0.50f);
    CHECK(kStepDownDwellMs == 3000);
    CHECK(kUpDwellBaseMs == 10000 && kUpDwellMaxMs == 96000);
    CHECK(kDownBelow == 0.95 && kProbeKeep == 0.97);
    CHECK(kProbeJudgeMs == 2000 && kDiscardMs == 1000 && kSeedMs == 500);
    CHECK(kSmoothTauMs == 1000.0 && kSpikeClip == 3.0 && kMaxSampleMs == 250.0);
    CHECK(kFgStaleTicks == 4 && kPassGapMs == 1000);
    CHECK(ReasonText(Reason::Off) == std::string("off"));
    CHECK(ReasonText(Reason::WaitingForCount) == std::string("waiting for the frame-generation count"));
    CHECK(ReasonText(Reason::UnsupportedFg) == std::string("frame generation is not DLSS-G"));
    CHECK(ReasonText(Reason::NoSignal) == std::string("no frame-time signal"));
    Output off;
    off.reason = Reason::Off;
    CHECK(StatusLine(off) == "Governor: off");
    Output tier;
    tier.reason = Reason::Holding;
    tier.cap = 0.9f;
    tier.tierPct = 90;
    tier.targetFps = 60.0;
    tier.measuredFps = 51.0;
    CHECK(StatusLine(tier) == "Governor: 90% (target 60 fps, measured 51 fps)");
    Output down;
    down.reason = Reason::AutoMfg;
    down.cap = 0.9f;
    down.tierPct = 90;
    down.targetFps = 60.0;
    down.measuredFps = 51.0;
    const std::string autoLine = StatusLine(down);
    CHECK(autoLine.find("AutoMFG is on") != std::string::npos && autoLine.find("90%") != std::string::npos);
    Output full;
    full.reason = Reason::Full;
    full.targetFps = 60.0;
    full.measuredFps = 59.0;
    CHECK(StatusLine(full) == "Governor: full resolution (target 60 fps, measured 59 fps)");
}

} // namespace

int main()
{
    DownStepsAndFloor();
    HoldsAtNinetyNinePercent();
    DownThreshold();
    DeadZone();
    UpThreshold();
    ProbeFiresAtTargetAndReverts();
    ProbeBackoffDoublesCapsAndResets();
    FirstStepAndCeilings();
    GovernorOff();
    CeilingChange();
    RealStandDownsReset();
    TransientFreezeKeepsTier();
    VramHoldFreezesTier();
    CounterFreshness();
    PassGap();
    TransitionWindowInMs();
    SmoothingClipSeedAndPause();
    FrameGenerationMultiplier();
    ProbeOwnsTheVerdict();
    ProbeFromFullTier();
    ConstantsAndStatus();
    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: NR frame-rate governor pure core (simulated-time tiers, dwells, probes, stand-downs, gates)");
    return 0;
}






