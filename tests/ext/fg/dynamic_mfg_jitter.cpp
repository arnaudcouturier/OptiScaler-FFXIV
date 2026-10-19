// Dynamic MFG selector against a noisy plant: every real-frame interval is the plant's value times a seeded random factor of
// +-10 %, with the presented rate a few percent inside each decision threshold. The smoothed rate crosses a threshold now and
// then for a moment; the candidate / dwell logic must not turn those moments into switches. The noise-free plant of
// dynamic_mfg_closed_loop.cpp never crosses a threshold, so it cannot tell a working dwell from a missing one.
#include "dynamic_mfg_harness.h"

using ext::fg::DynamicMfg;
using Why = DynamicMfg::Why;

namespace
{
// Small linear congruential generator (Numerical Recipes constants): the same sequence on every machine.
struct Lcg
{
    uint32_t state;
    explicit Lcg(uint32_t seed) : state(seed) {}
    // Uniform in [0, 1).
    double Next()
    {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) * (1.0 / 16777216.0);
    }
};

constexpr double kTarget = 224.0;
constexpr double kJitter = 0.10; // +-10 % per sample
constexpr uint64_t kRunMs = 300000;
constexpr uint64_t kSettleMs = 20000; // the first seconds after the start are not "after convergence"

struct Scenario
{
    const char* name;
    int start;      // factor in force, also the factor the plant is tuned for
    double ratio;   // presented rate at `start` over the target, noise-free
    double expect;  // the factor that has to be in force at the end (equal to start: it must not move)
};

// The real rate does not depend on the factor here (a scene that costs the same per real frame): presented = real * factor.
//   up edge (presented < 0.93 x target steps up):      2x and 3x at 0.945 x target
//   wide down edge (> 1.30 x target and one step less >= 0.97 x target): 4x at 1.27 x, 3x at 1.42 x
const Scenario kScenarios[] = {
    { "2x at 0.945 x target (up edge)", 2, 0.945, 2 },
    { "3x at 0.945 x target (up edge)", 3, 0.945, 3 },
    { "4x at 1.285 x target (wide down edge)", 4, 1.285, 4 },
    { "3x at 1.44 x target (wide down edge)", 3, 1.44, 3 },
};
constexpr uint32_t kSeeds[] = { 1, 7, 42, 2024, 99991 };

struct Result
{
    int ruleSwitches = 0;  // everything but probe trials, after the settle time
    int allSwitches = 0;
    int finalFactor = 0;
};

Result Run(const Scenario& sc, uint32_t seed)
{
    Sim s;
    s.target = kTarget;
    s.factor = sc.start;
    s.probe = false; // the edge cases are about the rate rules; probing has its own tests
    const double realFps = sc.ratio * kTarget / sc.start;
    const double baseMs = 1000.0 / realFps;
    Lcg rng(seed);
    auto noisy = [&]() { return baseMs * (1.0 + kJitter * (2.0 * rng.Next() - 1.0)); };

    s.Step(baseMs);
    const uint64_t t0 = s.t;
    while (s.t < t0 + kRunMs)
        s.Step(noisy());
    Result r;
    r.finalFactor = s.factor;
    r.allSwitches = s.Count(0);
    r.ruleSwitches = s.Count(t0 + kSettleMs, false);
    return r;
}

void NoiseNearTheThresholdsDoesNotMoveTheFactor()
{
    for (const auto& sc : kScenarios)
        for (const uint32_t seed : kSeeds)
        {
            const Result r = Run(sc, seed);
            CHECK_MSG(r.ruleSwitches <= 2, "%s, seed %u: %d rule switches in 5 minutes (all %d)", sc.name, seed,
                      r.ruleSwitches, r.allSwitches);
            CHECK_MSG(r.finalFactor == (int) sc.expect, "%s, seed %u: ended at %dx", sc.name, seed, r.finalFactor);
        }
}

void ThePlantReallyCrossesTheThresholds()
{
    // Guard against a test that proves nothing: with the edge only 2 to 3 percent away, the smoothed rate has to wander
    // past the threshold at times, or the noise is too small to exercise the dwell. Count the samples at which the
    // average alone (no dwell, no hold) would have asked for a move.
    for (const auto& sc : kScenarios)
    {
        DynamicMfg sel;
        Lcg rng(42);
        const double baseMs = 1000.0 / (sc.ratio * kTarget / sc.start);
        uint64_t t = 1000;
        sel.Select(baseMs, kTarget, t, sc.start, 4, false);
        int beyond = 0, samples = 0;
        while (t < 1000 + kRunMs)
        {
            const double ms = baseMs * (1.0 + kJitter * (2.0 * rng.Next() - 1.0));
            t += (uint64_t) std::max(1.0, std::floor(ms));
            sel.Select(ms, kTarget, t, sc.start, 4, false);
            const double avg = sel.AverageMs();
            if (avg <= 0.0)
                continue;
            const double presented = 1000.0 / avg * sc.start;
            const double lower = 1000.0 / avg * (sc.start - 1);
            const bool up = sc.start < 4 && presented < kTarget * DynamicMfg::kUpBelow;
            const bool down = sc.start > 2 && presented > kTarget * DynamicMfg::kWideAbove &&
                              lower >= kTarget * DynamicMfg::kFastKeep;
            ++samples;
            beyond += (up || down);
        }
        CHECK_MSG(beyond > 0, "%s: the smoothed rate never reached the threshold (%d samples)", sc.name, samples);
    }
}
} // namespace

int main()
{
    NoiseNearTheThresholdsDoesNotMoveTheFactor();
    ThePlantReallyCrossesTheThresholds();
    if (g_failures)
    {
        std::printf("dynamic_mfg_jitter: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("dynamic_mfg_jitter: all passed\n");
    return 0;
}
