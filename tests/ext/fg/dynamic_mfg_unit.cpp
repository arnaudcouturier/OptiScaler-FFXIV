// Dynamic MFG selector (ext/fg/DynamicMfg.h), open loop: scripted frame-time streams against each rule.
// The closed-loop plant tests are dynamic_mfg_closed_loop.cpp.
#include "dynamic_mfg_harness.h"

using ext::fg::DynamicMfg;
using Why = DynamicMfg::Why;

namespace
{
void UpAfterHoldAndDwell()
{
    Sim s;
    s.target = 138;
    s.Step(40); // starts the observation window and the first hold
    const uint64_t start = s.t;
    s.Run(20000, 40.0); // 25 fps real against 138: needs more factor
    CHECK(s.factor == 4);
    CHECK(s.log.size() == 2);
    if (s.log.size() == 2)
    {
        CHECK(s.log[0].from == 2 && s.log[0].to == 3 && s.log[0].why == Why::Up);
        CHECK(s.log[1].from == 3 && s.log[1].to == 4 && s.log[1].why == Why::Up);
        CHECK_MSG(s.log[0].t - start >= 2950 && s.log[0].t - start <= 3300, "first up at +%llu", s.log[0].t - start);
        CHECK_MSG(s.log[1].t - s.log[0].t >= 2950 && s.log[1].t - s.log[0].t <= 3300, "second up %llu after the first",
                  s.log[1].t - s.log[0].t);
    }
    s.Run(30000, 40.0);
    CHECK(s.factor == 4 && s.log.size() == 2); // never above the maximum, nothing else happens

    Sim capped;
    capped.target = 138;
    capped.maxFactor = 3;
    capped.Step(40);
    capped.Run(30000, 40.0);
    CHECK(capped.factor == 3); // the caller's maximum wins
}

void NothingBeforeTheHoldEnds()
{
    Sim s;
    s.target = 138;
    s.Step(40);
    s.Run(2900, 40.0);
    CHECK(s.log.empty()); // 1 s of evidence is not enough inside the 3 s hold
}

void DownDwells()
{
    // Strong evidence (one step less still presents far above the target): 0.5 s dwell.
    Sim strong;
    strong.target = 100;
    strong.factor = 3;
    strong.Step(25);
    strong.Run(6000, 25.0); // neutral: presented 120, one step less 80
    CHECK(strong.log.empty());
    const uint64_t t0 = strong.t;
    strong.Run(8000, 8.0); // 125 fps real: 3x is far too much
    CHECK(strong.log.size() == 1); // 3x -> 2x, then nothing (2 is the floor)
    if (!strong.log.empty())
    {
        const uint64_t delay = strong.log[0].t - t0;
        CHECK(strong.log[0].why == Why::FastDown && strong.log[0].to == 2);
        CHECK_MSG(delay >= 400 && delay <= 1500, "strong down after %llu ms", delay);
    }
}

void DownWeakCandidateKeepsFullDwell()
{
    // A limiter caps the real rate: at 3x the real rate is 50 fps (one step less would present 100 = the target, the
    // plain rule is silent) but the output is 150 (> 1.30x). That weaker evidence needs the 2 s dwell.
    Sim s;
    s.target = 100;
    s.factor = 3;
    s.Step(25);
    s.Run(6000, 25.0);
    CHECK(s.log.empty());
    const uint64_t t0 = s.t;
    s.Run(8000, 20.0);
    CHECK(!s.log.empty());
    if (!s.log.empty())
    {
        const uint64_t delay = s.log[0].t - t0;
        CHECK(s.log[0].to == 2 && s.log[0].why == Why::Down);
        CHECK_MSG(delay >= 2000 && delay <= 3800, "weak down after %llu ms", delay);
    }
}

void SpikeIsClipped()
{
    // 3x at 12.8 ms real = 78 fps: presented 234, inside the dead band of target 224 (up below 208).
    Sim s;
    s.factor = 3;
    s.Step(12.8);
    s.Run(5000, 12.8);
    CHECK(s.log.empty());
    s.Step(190.0); // one stall (a feature re-creation)
    s.Run(10000, 12.8);
    CHECK_MSG(s.log.empty(), "a single 190 ms sample caused %zu switches", s.log.size());

    // A real scene change is not clipped away.
    Sim slow;
    slow.factor = 3;
    slow.Step(12.8);
    slow.Run(5000, 12.8);
    const uint64_t t0 = slow.t;
    slow.Run(10000, 40.0);
    CHECK(slow.factor == 4);
    CHECK(slow.First(t0) != 0 && slow.First(t0) - t0 < 6000);
}

void BadSamplesRestartTheWindow()
{
    Sim steady;
    steady.target = 138;
    steady.Step(40);
    steady.Run(8000, 40.0);
    CHECK(steady.Count(0) >= 1); // the same stream does switch

    Sim s;
    s.target = 138;
    s.Step(40);
    const double bad[] = { NAN, 0.0, -3.0, 300.0, INFINITY };
    for (int i = 0; i < 40; ++i)
    {
        s.Run(2000, 40.0);
        s.Step(bad[i % 5]); // a pause every 2 s: the hold starts over each time
    }
    CHECK_MSG(s.log.empty(), "%zu switches despite a pause every 2 s", s.log.size());
}

void UnknownTargetIsInert()
{
    const double targets[] = { 0.0, -5.0, NAN, INFINITY };
    for (double target : targets)
    {
        Sim s;
        s.target = target;
        s.Step(40);
        s.Run(30000, 40.0);
        CHECK(s.log.empty() && s.factor == 2);
    }
}

void RangeIsClamped()
{
    DynamicMfg d;
    CHECK(d.Select(40, 138, 1000, 4, 2, true) == 2);
    CHECK(d.Last().why == Why::Clamp && d.Last().from == 4 && d.Last().to == 2);
    CHECK(d.Select(40, 138, 1100, 1, 4, true) == 2);
    CHECK(d.Select(40, 138, 1200, 6, 4, true) == 4);
    CHECK(d.Select(40, 138, 1300, 3, 9, true) == 3); // maximum is clamped to 4
    CHECK(d.Select(40, 138, 1400, 3, 0, true) == 2); // and to at least 2
    CHECK(d.Last().why == Why::Clamp);
}

void FlapGuardRaisesTheHold()
{
    // Alternating load every 3 s: the hold alone would allow a switch every ~3 s.
    Sim s;
    s.target = 138;
    s.Step(40);
    uint64_t holdSeen = 0;
    for (int slot = 0; slot < 40; ++slot) // 2 minutes
    {
        const bool heavy = slot % 2 == 0;
        const uint64_t end = s.t + 3000;
        while (s.t < end)
        {
            s.Step(heavy ? 40.0 : 6.0);
            holdSeen = std::max(holdSeen, s.sel.EffectiveHoldMs(s.t));
        }
    }
    CHECK_MSG(holdSeen > 3000, "the hold never went above the base (%llu)", holdSeen);
    CHECK(holdSeen <= DynamicMfg::kMaxHoldMs);
    const uint64_t minute2 = 1000 + 60000;
    CHECK_MSG(s.Count(minute2) <= 6, "%d switches in the second minute", s.Count(minute2));
    CHECK_MSG(s.Count(0) < 40, "the guard did not limit the first minute (%d switches in 2 min)", s.Count(0));
    std::printf("  info: alternating load every 3 s, 2 min: %d switches (first minute %d, second minute %d), highest hold %llu ms\n",
                s.Count(0), s.Count(0) - s.Count(minute2), s.Count(minute2), holdSeen);

    // The escalation itself: base 3 s, doubled for every switch beyond the fourth in the window, capped.
    DynamicMfg d;
    d.Select(40, 138, 100, 2, 4, false); // starts the window
    CHECK(d.EffectiveHoldMs(100) == 3000 && d.RecentSwitches(100) == 0);
}

void ProbeTimingAndBackoff()
{
    // Refresh-bound light scene at 4x: 60 fps real, 240 presented against 224. No rate rule can step down.
    const double ms4 = 1000.0 / 60.0;
    Sim off;
    off.probe = false;
    off.factor = 4;
    off.Step(ms4);
    off.Run(200000, ms4);
    CHECK(off.log.empty()); // allowProbe is false: nothing happens, ever

    Sim s;
    s.factor = 4;
    s.Step(ms4);
    const uint64_t start = s.t;
    // A probe that fails: at 3x the presented rate is 180.
    s.Run(15000, ms4); // constant interval whatever the factor: at 3x it would present 180 (< 208)
    CHECK(s.CountWhy(Why::Probe) == 1);
    if (!s.log.empty())
    {
        const uint64_t at = s.log[0].t - start;
        CHECK(s.log[0].why == Why::Probe && s.log[0].from == 4 && s.log[0].to == 3);
        CHECK_MSG(at >= 11900 && at <= 12500, "first probe after %llu ms", at);
    }
    s.Run(4000, ms4);
    CHECK(s.CountWhy(Why::ProbeRevert) == 1 && s.factor == 4);
    CHECK(s.sel.ProbeMs() == 24000); // doubled
    if (s.log.size() >= 2)
        CHECK_MSG(s.log[1].t - s.log[0].t >= 1950 && s.log[1].t - s.log[0].t <= 2200, "judged after %llu ms",
                  s.log[1].t - s.log[0].t);

    // The back-off doubles up to the cap.
    s.Run(300000, ms4);
    CHECK(s.sel.ProbeMs() == DynamicMfg::kProbeMaxMs);
    int probes = s.CountWhy(Why::Probe);
    CHECK_MSG(probes >= 4 && probes <= 8, "%d probes in 5 minutes", probes);

    // A rule switch ends the back-off: after a failed probe the scene gets very light and the plain down rule fires.
    Sim p;
    p.factor = 4;
    p.Step(ms4);
    p.Run(19000, ms4);
    CHECK(p.sel.ProbeMs() == 24000);
    p.Run(10000, 5.0);
    CHECK(p.CountWhy(Why::FastDown) >= 1);
    CHECK(p.sel.ProbeMs() == DynamicMfg::kProbeBaseMs);
}

void ProbeSuccessWalksDownAndResets()
{
    // 4x -> 3x -> 2x when each lower factor still reaches the target: refresh 240, light scene.
    Plant plant;
    plant.base2x = 8.0;
    plant.refresh = 240.0;
    plant.limit = 224.0;
    Sim s;
    s.factor = 4;
    s.Step(plant.Ms(4));
    s.Run(60000, [&](int f) { return plant.Ms(f); });
    CHECK(s.factor == 2);
    CHECK(s.CountWhy(Why::Probe) == 2 && s.CountWhy(Why::ProbeRevert) == 0);
    CHECK(s.sel.ProbeMs() == DynamicMfg::kProbeBaseMs);
    const uint64_t t2 = s.log.empty() ? 0 : s.log.back().t;
    s.Run(200000, [&](int f) { return plant.Ms(f); });
    CHECK(s.First(t2 + 1) == 0); // 2x is the floor: no probe below it
}

void ProbeTimerRestartsWhenTheRateDrops()
{
    const double ms4 = 1000.0 / 60.0;
    Sim s;
    s.factor = 4;
    s.Step(ms4);
    s.Run(9000, ms4);
    s.Run(1500, 19.0); // presented 210 for 1.5 s: under 0.97 x 224 = 217
    s.Run(3000, ms4);
    CHECK(s.CountWhy(Why::Probe) == 0); // the 12 s clock restarted
    s.Run(16000, ms4);
    CHECK(s.CountWhy(Why::Probe) >= 1);
}

void RegretGuardRevertsAndRefuses()
{
    // 6 ms per step on a 10 ms base: 2x presents 200, 3x only 187.5, 4x 181. Going up gains nothing.
    Plant plant;
    plant.base2x = 10.0;
    plant.perStep = 6.0;
    plant.refresh = 0;
    plant.limit = 0;
    Sim s;
    s.target = 224;
    s.Step(plant.Ms(2));
    s.Run(40000, [&](int f) { return plant.Ms(f); });
    CHECK(s.CountWhy(Why::Up) >= 1);
    CHECK(s.CountWhy(Why::RegretRevert) >= 1);
    if (s.log.size() >= 2)
    {
        CHECK(s.log[0].why == Why::Up && s.log[0].to == 3);
        CHECK(s.log[1].why == Why::RegretRevert && s.log[1].to == 2);
        CHECK_MSG(s.log[1].t - s.log[0].t >= 1950 && s.log[1].t - s.log[0].t <= 2200, "reverted after %llu",
                  s.log[1].t - s.log[0].t);
        // refused for 30 s: no new up step inside the window
        for (const auto& sw : s.log)
            if (sw.why == Why::Up && sw.t > s.log[1].t)
            {
                CHECK_MSG(sw.t - s.log[1].t >= 29900, "went up again %llu ms after the revert", sw.t - s.log[1].t);
                break;
            }
    }
    CHECK(s.Count(0) <= 4); // up + revert, once the refusal ran out maybe once more

    // A step that pays (gain >= 4 %) is kept: the plain 40 ms stream of UpAfterHoldAndDwell never reverts.
    Sim keep;
    keep.target = 138;
    keep.Step(40);
    keep.Run(30000, 40.0);
    CHECK(keep.CountWhy(Why::RegretRevert) == 0);
}

void RestartKeepsWhatWasLearned()
{
    const double ms4 = 1000.0 / 60.0;
    Sim s;
    s.factor = 4;
    s.Step(ms4);
    s.Run(19000, ms4);
    CHECK(s.sel.ProbeMs() == 24000);
    s.sel.Restart(s.t);
    CHECK(s.sel.ProbeMs() == 24000 && s.sel.AverageMs() == 0.0 && !s.sel.TrialPending());
    s.sel.Reset();
    CHECK(s.sel.ProbeMs() == DynamicMfg::kProbeBaseMs);
    s.sel.SetHoldMs(12000);
    s.sel.Reset();
    CHECK(s.sel.HoldMs() == 12000); // the hold setting survives a reset
}

void FirstIntervalAfterASwitchIsDropped()
{
    // The interval right after a switch (or a restart) can hold a re-creation stall; it must not seed the average.
    DynamicMfg d;
    d.Select(40, 138, 1000, 2, 4, false); // starts the window
    d.Restart(2000);
    d.Select(190, 138, 2010, 2, 4, false);
    CHECK(d.AverageMs() == 0.0);
    d.Select(40, 138, 2050, 2, 4, false);
    CHECK(d.AverageMs() == 40.0);

    Sim s;
    s.target = 138;
    s.Step(40);
    while (s.log.empty())
        s.Step(40.0);
    CHECK(s.sel.AverageMs() == 0.0); // reset by the switch
    s.Step(190.0);                    // the stall of a re-creation
    CHECK(s.sel.AverageMs() == 0.0);
    s.Step(40.0);
    CHECK(s.sel.AverageMs() == 40.0);
}

void ExternalChangeVoidsATrial()
{
    // A probe is running and somebody else (the user's slider) changes the factor: no verdict, no revert.
    const double ms4 = 1000.0 / 60.0;
    Sim s;
    s.factor = 4;
    s.Step(ms4);
    s.Run(13000, ms4);
    CHECK(s.CountWhy(Why::Probe) == 1 && s.sel.TrialPending());
    s.factor = 2; // slider
    s.Run(5000, ms4);
    CHECK(s.CountWhy(Why::ProbeRevert) == 0 && !s.sel.TrialPending());
}
} // namespace

int main()
{
    UpAfterHoldAndDwell();
    NothingBeforeTheHoldEnds();
    DownDwells();
    DownWeakCandidateKeepsFullDwell();
    SpikeIsClipped();
    BadSamplesRestartTheWindow();
    UnknownTargetIsInert();
    RangeIsClamped();
    FlapGuardRaisesTheHold();
    ProbeTimingAndBackoff();
    ProbeSuccessWalksDownAndResets();
    ProbeTimerRestartsWhenTheRateDrops();
    RegretGuardRevertsAndRefuses();
    RestartKeepsWhatWasLearned();
    ExternalChangeVoidsATrial();
    FirstIntervalAfterASwitchIsDropped();
    if (g_failures)
    {
        std::printf("dynamic_mfg_unit: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("dynamic_mfg_unit: all passed\n");
    return 0;
}
