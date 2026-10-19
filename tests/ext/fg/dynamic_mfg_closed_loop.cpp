// Dynamic MFG selector against the closed-loop plant of the study (5.1): the frame time depends on the factor, the
// display refresh pins the presented rate and the limiter caps the real rate. XeFG's selector was never tested this way.
#include "dynamic_mfg_harness.h"

using ext::fg::DynamicMfg;
using Why = DynamicMfg::Why;

namespace
{
constexpr uint64_t kPlain = 15000;  // convergence deadline for the plain rules
constexpr uint64_t kProbed = 40000; // when the probe-down has to walk 4x -> 3x -> 2x

// Smallest factor whose presented rate reaches 0.93 x target, else the maximum.
int Ideal(const Plant& p, double target, int maxFactor)
{
    for (int f = 2; f <= maxFactor; ++f)
        if (p.Presented(f) >= 0.93 * target)
            return f;
    return maxFactor;
}

struct Outcome
{
    uint64_t reached = 0; // ms after the start at which the ideal factor was first in force (0 = never)
    int finalFactor = 0;
    int ruleSwitchesAfter = 0; // rule switches in the 60 s after `reached`
    int lowestAfter = 99;
};

Outcome Converge(const Plant& p, double target, int start, int ideal, bool probe, uint64_t horizon)
{
    Sim s;
    s.target = target;
    s.factor = start;
    s.probe = probe;
    s.Step(p.Ms(start));
    const uint64_t t0 = s.t;
    Outcome o;
    const uint64_t end = t0 + horizon + 60000;
    while (s.t < end)
    {
        s.Step(p.Ms(s.factor));
        if (o.reached == 0 && s.factor == ideal)
            o.reached = s.t - t0 + 1; // +1: a start at the ideal factor counts as reached at once
        if (o.reached != 0 && s.t - t0 >= o.reached)
            o.lowestAfter = std::min(o.lowestAfter, s.factor);
    }
    o.finalFactor = s.factor;
    if (o.reached != 0)
        o.ruleSwitchesAfter = s.Count(t0 + o.reached, false);
    return o;
}

void LightSceneAtFourXProbesDown()
{
    // base 8 ms, 240 Hz, limit 224, start at 4x: 60 fps real, 240 presented. Only the probe can find 2x (224).
    Plant p;
    p.base2x = 8.0;
    Sim s;
    s.target = 224;
    s.factor = 4;
    s.Step(p.Ms(4));
    const uint64_t t0 = s.t;
    s.Run(kProbed, [&](int f) { return p.Ms(f); });
    CHECK_MSG(s.factor == 2, "factor %d after 40 s", s.factor);
    CHECK(s.CountWhy(Why::Probe) == 2 && s.CountWhy(Why::ProbeRevert) == 0);
    CHECK(s.CountWhy(Why::Up) == 0 && s.CountWhy(Why::Down) + s.CountWhy(Why::FastDown) == 0);
    s.Run(120000, [&](int f) { return p.Ms(f); });
    CHECK(s.log.size() == 2 && s.factor == 2); // and it stays
    (void) t0;
}

void HeavySceneDoesNotProbe()
{
    // base 18.2 ms: 4x presents 188.7, under 0.97 x 224: not a probe candidate.
    Plant p;
    p.base2x = 18.2;
    Sim s;
    s.target = 224;
    s.factor = 4;
    s.Step(p.Ms(4));
    s.Run(300000, [&](int f) { return p.Ms(f); });
    CHECK(s.log.empty() && s.factor == 4);
}

void FailedProbeRevertsAndDoublesTheInterval()
{
    // At 3x only 202 presented (< 0.93 x 224), at 4x the output is pinned at the 240 Hz refresh: a probe has to fail.
    Plant p;
    p.base2x = 13.3;
    p.limit = 0;
    CHECK(p.Presented(3) < 0.93 * 224 && p.Presented(4) >= 0.97 * 224);
    Sim s;
    s.target = 224;
    s.factor = 4;
    s.Step(p.Ms(4));
    const uint64_t t0 = s.t;
    s.Run(60000, [&](int f) { return p.Ms(f); });
    CHECK(s.factor == 4);
    CHECK(s.log.size() >= 2);
    if (s.log.size() >= 4)
    {
        // trials at ~12 s, then ~24 s after the first revert, then ~48 s after the second one
        const uint64_t first = s.log[0].t - t0;
        const uint64_t gap1 = s.log[2].t - s.log[1].t;
        CHECK_MSG(first >= 11900 && first <= 12600, "first trial at %llu ms", first);
        CHECK_MSG(gap1 >= 23900 && gap1 <= 24700, "second trial %llu ms after the first revert", gap1);
    }
    for (size_t i = 0; i + 1 < s.log.size(); i += 2)
        CHECK(s.log[i].why == Why::Probe && s.log[i + 1].why == Why::ProbeRevert);
    CHECK(s.CountWhy(Why::Up) == 0);
    // every trial is a short dip: 2 s at the lower factor, no more
    for (size_t i = 0; i + 1 < s.log.size(); i += 2)
        CHECK_MSG(s.log[i + 1].t - s.log[i].t <= 2300, "trial lasted %llu ms", s.log[i + 1].t - s.log[i].t);
    s.Run(400000, [&](int f) { return p.Ms(f); });
    CHECK(s.sel.ProbeMs() == DynamicMfg::kProbeMaxMs);
}

void NoProbeWhenNotAllowed()
{
    Plant p;
    p.base2x = 8.0;
    Sim s;
    s.target = 224;
    s.factor = 4;
    s.probe = false;
    s.Step(p.Ms(4));
    s.Run(300000, [&](int f) { return p.Ms(f); });
    CHECK(s.log.empty() && s.factor == 4);
}

void ConvergenceGrid()
{
    const double bases[] = { 10.0, 12.0, 14.4, 16.3, 18.2, 22.0 };
    const double targets[] = { 144.0, 165.0, 224.0, 240.0 };
    int cases = 0, probeCases = 0, failures = 0;
    for (const bool limited : { false, true })
        for (double base : bases)
            for (double target : targets)
                for (int start = 2; start <= 4; ++start)
                {
                    Plant p;
                    p.base2x = base;
                    p.refresh = 240.0;
                    p.limit = limited ? target : 0.0; // the limiter is what the target is derived from
                    const int ideal = Ideal(p, target, 4);
                    const Outcome plain = Converge(p, target, start, ideal, false, kPlain);
                    const bool plainReaches = plain.reached != 0 && plain.reached <= kPlain;
                    const Outcome o = Converge(p, target, start, ideal, true, plainReaches ? kPlain : kProbed);
                    ++cases;
                    probeCases += plainReaches ? 0 : 1;
                    const uint64_t deadline = plainReaches ? kPlain : kProbed;
                    const bool ok = o.reached != 0 && o.reached <= deadline && o.ruleSwitchesAfter <= 2 &&
                                    o.lowestAfter >= ideal - 1;
                    if (!ok)
                    {
                        ++failures;
                        std::printf("  grid miss: base %.1f target %.0f limit %s start %dx ideal %dx -> reached %llu ms "
                                    "(plain %llu), rule switches after %d, lowest %dx, final %dx\n",
                                    base, target, limited ? "target" : "none", start, ideal, o.reached, plain.reached,
                                    o.ruleSwitchesAfter, o.lowestAfter, o.finalFactor);
                    }
                }
    std::printf("  info: grid %d cases (%d need the probe), %d missed\n", cases, probeCases, failures);
    CHECK(failures == 0);
}

void SceneSteps(double refresh, bool expectProbePath)
{
    // heavy (55 fps real at 2x) -> light -> heavy. Limit 224: the real rate is capped at 112 in the light scene.
    Plant heavy, light;
    heavy.base2x = 18.2;
    light.base2x = 4.0;
    heavy.refresh = light.refresh = refresh;
    Sim s;
    s.target = 224;
    s.factor = 2;
    s.Step(heavy.Ms(2));
    const uint64_t t0 = s.t;
    s.Run(10000, [&](int f) { return heavy.Ms(f); });
    CHECK_MSG(s.factor == 4, "heavy scene: %dx after 10 s", s.factor);
    CHECK_MSG(s.log.size() == 2 && s.log.back().t - t0 <= 8000, "up to 4x took %llu ms",
              s.log.empty() ? 0ull : s.log.back().t - t0);

    const uint64_t t1 = s.t;
    const size_t before = s.log.size();
    s.Run(expectProbePath ? 45000 : 12000, [&](int f) { return light.Ms(f); });
    CHECK_MSG(s.factor == 2, "light scene (refresh %.0f): %dx", refresh, s.factor);
    uint64_t back = 0;
    for (size_t i = before; i < s.log.size(); ++i)
        if (s.log[i].to == 2)
            back = s.log[i].t - t1;
    const uint64_t limit = expectProbePath ? kProbed + 5000 : 8000;
    CHECK_MSG(back != 0 && back <= limit, "back to 2x took %llu ms (refresh %.0f)", back, refresh);
    CHECK(expectProbePath == (s.CountWhy(Why::Probe) > 0));
    std::printf("  info: scene step refresh %.0f: 2x -> 4x in %llu ms, back to 2x in %llu ms\n", refresh,
                s.log[1].t - t0, back);

    const uint64_t t2 = s.t;
    s.Run(12000, [&](int f) { return heavy.Ms(f); });
    CHECK_MSG(s.factor == 4, "heavy again: %dx", s.factor);
    uint64_t up = 0;
    for (const auto& sw : s.log)
        if (sw.t > t2 && sw.to == 4)
            up = sw.t - t2;
    CHECK_MSG(up != 0 && up <= 8000, "up to 4x again took %llu ms", up);
}

void AlternatingLoadIsDamped()
{
    Plant heavy, light;
    heavy.base2x = 30.0; // 3x presents about 95 fps real x 3: far under the target
    light.base2x = 4.0;  // and then 250 fps real: far over it
    heavy.refresh = light.refresh = 0; // nothing pins the output: every rule can fire
    heavy.limit = light.limit = 0;
    Sim s;
    s.target = 224;
    s.Step(heavy.Ms(2));
    uint64_t slot = s.t;
    bool isHeavy = true;
    const uint64_t t0 = s.t;
    while (s.t < t0 + 180000)
    {
        if (s.t - slot >= 3000)
        {
            slot = s.t;
            isHeavy = !isHeavy;
        }
        s.Step((isHeavy ? heavy : light).Ms(s.factor));
    }
    std::printf("  info: alternating plant load every 3 s, 3 min: %d switches (minute 1 %d, minute 2 %d, minute 3 %d)\n",
                s.Count(0), s.Count(0) - s.Count(t0 + 60000), s.Count(t0 + 60000) - s.Count(t0 + 120000),
                s.Count(t0 + 120000));
    CHECK(s.Count(t0 + 60000) - s.Count(t0 + 120000) <= 6);
    CHECK(s.Count(t0 + 120000) <= 6);
    CHECK(s.Count(0) - s.Count(t0 + 60000) <= 8);
}

void ExpensivePlantReverts()
{
    // 6 ms per step on a 10 ms base: each step presents less than the one before.
    Plant p;
    p.base2x = 10.0;
    p.perStep = 6.0;
    p.refresh = 0;
    p.limit = 0;
    Sim s;
    s.target = 224;
    s.Step(p.Ms(2));
    s.Run(25000, [&](int f) { return p.Ms(f); });
    CHECK(s.CountWhy(Why::Up) == 1 && s.CountWhy(Why::RegretRevert) == 1 && s.factor == 2);
    // refused for 30 s after the revert, so nothing happens until then
    const uint64_t revert = s.log.size() >= 2 ? s.log[1].t : 0;
    s.Run(60000, [&](int f) { return p.Ms(f); });
    if (s.log.size() >= 3)
        CHECK_MSG(s.log[2].t - revert >= 29900, "tried again %llu ms after the revert", s.log[2].t - revert);
}

void LargeStableScenesDoNotMove()
{
    // targets already met, from every start: no more than the single step to the ideal factor
    Plant p;
    p.base2x = 6.0;
    p.limit = 0;
    p.refresh = 0;
    for (int start = 2; start <= 4; ++start)
    {
        Sim s;
        s.target = 100;
        s.factor = start;
        s.Step(p.Ms(start));
        s.Run(120000, [&](int f) { return p.Ms(f); });
        CHECK_MSG(s.factor == 2, "start %dx ended at %dx", start, s.factor);
        CHECK(s.log.size() == (size_t) (start - 2));
    }
}
} // namespace

int main()
{
    LightSceneAtFourXProbesDown();
    HeavySceneDoesNotProbe();
    FailedProbeRevertsAndDoublesTheInterval();
    NoProbeWhenNotAllowed();
    ConvergenceGrid();
    SceneSteps(480.0, false);
    SceneSteps(240.0, true);
    AlternatingLoadIsDamped();
    ExpensivePlantReverts();
    LargeStableScenesDoNotMove();
    if (g_failures)
    {
        std::printf("dynamic_mfg_closed_loop: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("dynamic_mfg_closed_loop: all passed\n");
    return 0;
}
