// [MIP-BIND] The add/replace detector (ext/upscaler/MipBind.h, ModeDetector): ADD until the first change of the ratio,
// REPLACE when the game scales its own bias with the mode (a new value one change away from an old one), ADD when the
// new samplers are only constant art biases, and the cases in between.
#include "ext/upscaler/MipBind.h"

#include <cmath>
#include <cstdio>

using namespace ext::upscaler::mipbind;

static int g_failed = 0;

#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

static bool Near(double a, double b, double tolerance = 1e-6) { return std::fabs(a - b) <= tolerance; }

static constexpr double kQuality = -0.584963; // log2(2560 / 3840)
static constexpr double kPerf = -1.0;

int main()
{
    // Nothing known: ADD, and nothing decided by observations alone.
    {
        ModeDetector d;
        CHECK(d.mode() == Mode::Add && !d.decided() && d.phase() == ModeDetector::Phase::Baseline);
        CHECK(!d.OnEligibleCreated(-1.0f).decided);
        CHECK(!d.OnEligibleCreated(-0.5f).decided);
        CHECK(!d.OnRatio(0.0).decided); // the first ratio is only remembered
        CHECK(d.haveRatio() && d.phase() == ModeDetector::Phase::Baseline);
        CHECK(!d.decided() && d.mode() == Mode::Add);
    }

    // The logged FFXIV case: DLAA {-1, -0.5}, then Quality creates -1.5625 (-1 + log2(scale) in 1/16 steps), -1, -0.5, -2.
    {
        ModeDetector d;
        d.OnRatio(0.0);
        d.OnEligibleCreated(-1.0f);
        d.OnEligibleCreated(-0.5f);
        d.OnRatio(kQuality);
        CHECK(d.phase() == ModeDetector::Phase::Watching && d.mode() == Mode::Add);
        CHECK(!d.OnEligibleCreated(-1.0f).decided); // an old value again: no evidence
        CHECK(!d.OnEligibleCreated(-0.5f).decided);
        const auto r = d.OnEligibleCreated(-1.5625f); // -1 + (-0.585) = -1.585, within 1/16 of it
        CHECK(r.decided && r.mode == Mode::Replace);
        CHECK(Near(r.seenBefore, -1.0) && Near(r.seenAfter, -1.5625) && Near(r.delta, kQuality));
        CHECK(d.decided() && d.mode() == Mode::Replace);
        CHECK(!d.OnEligibleCreated(-2.0f).decided); // decided once, never again
        CHECK(!d.OnRatio(kPerf).decided && d.mode() == Mode::Replace);
    }

    // Constant art biases: new values that are no change-away pair; after kDecideAfter creations the answer is ADD.
    {
        ModeDetector d;
        d.OnRatio(0.0);
        d.OnEligibleCreated(-1.0f);
        d.OnEligibleCreated(-0.5f);
        d.OnRatio(kQuality);
        // -2, -1.5 are new but neither -1 + delta (-1.585, tolerance 0.08) nor -0.5 + delta (-1.085)
        CHECK(!d.OnEligibleCreated(-2.0f).decided);
        CHECK(!d.OnEligibleCreated(-1.5f).decided); // |-1.5 - (-1.585)| = 0.085 > 0.08
        for (unsigned i = 2; i + 1 < kDecideAfter; ++i)
            CHECK(!d.OnEligibleCreated(-1.0f).decided);
        const auto r = d.OnEligibleCreated(-0.5f);
        CHECK(r.decided && r.mode == Mode::Add && r.created == kDecideAfter);
        CHECK(d.decided() && d.mode() == Mode::Add);
        // a pair after the decision is too late
        CHECK(!d.OnEligibleCreated(-1.5625f).decided && d.mode() == Mode::Add);
    }

    // The rule is a pair relation, not "the set of values changed": the first change coincides with new content, which
    // brings new art values; a value that is not a change-away from an old one must not flip the mode.
    {
        ModeDetector d;
        d.OnRatio(0.0);
        d.OnEligibleCreated(-1.0f);
        d.OnRatio(kQuality);
        CHECK(!d.OnEligibleCreated(-3.0f).decided);
        CHECK(!d.OnEligibleCreated(-0.25f).decided);
        CHECK(!d.OnEligibleCreated(-1.1f).decided);
        CHECK(d.mode() == Mode::Add && !d.decided());
    }

    // An old value never counts as new (within 0.02), even when it is a change-away from another old one.
    {
        ModeDetector d;
        d.OnRatio(0.0);
        d.OnEligibleCreated(-1.0f);
        d.OnEligibleCreated(-2.0f);
        d.OnRatio(-1.0); // delta -1: -1 + (-1) = -2 is a change-away pair, but -2 was already used before
        CHECK(!d.OnEligibleCreated(-2.01f).decided);
        CHECK(!d.OnEligibleCreated(-2.0f).decided);
        CHECK(d.mode() == Mode::Add);
    }

    // Both directions: going back from Quality to DLAA is a change too (delta +0.585).
    {
        ModeDetector d;
        d.OnRatio(kQuality);
        d.OnEligibleCreated(-1.5625f);
        d.OnEligibleCreated(-0.5f);
        d.OnRatio(0.0);
        CHECK(d.phase() == ModeDetector::Phase::Watching);
        const auto r = d.OnEligibleCreated(-1.0f); // -1.5625 + 0.585 = -0.98
        CHECK(r.decided && r.mode == Mode::Replace && Near(r.delta, 0.584963));
    }

    // A move under 0.01 is size rounding (2560 vs 2558), not a change.
    {
        ModeDetector d;
        d.OnRatio(kQuality);
        d.OnEligibleCreated(-1.0f);
        d.OnRatio(kQuality + 0.003);
        CHECK(d.phase() == ModeDetector::Phase::Baseline);
        d.OnRatio(kQuality - 0.02);
        CHECK(d.phase() == ModeDetector::Phase::Watching);
    }

    // A second change while watching restarts the comparison from the newer ratio and keeps what was seen as "before".
    {
        ModeDetector d;
        d.OnRatio(0.0);
        d.OnEligibleCreated(-1.0f);
        d.OnRatio(kQuality);
        d.OnEligibleCreated(-0.75f); // a value seen after the first change, no pair with -1 + -0.585
        d.OnRatio(kPerf);            // quick second change: delta is now -0.415
        CHECK(d.phase() == ModeDetector::Phase::Watching && !d.decided());
        const auto r = d.OnEligibleCreated(-1.4f); // -1 + (-0.415) = -1.415
        CHECK(r.decided && r.mode == Mode::Replace && Near(r.delta, kPerf - kQuality));
    }

    // Ratios or values that are not numbers change nothing.
    {
        ModeDetector d;
        CHECK(!d.OnRatio(std::nan("")).decided && !d.haveRatio());
        d.OnRatio(0.0);
        d.OnRatio(std::nan(""));
        CHECK(d.phase() == ModeDetector::Phase::Baseline);
        CHECK(!d.OnEligibleCreated(std::nanf("")).decided);
    }

    // Reset (a new device) starts over.
    {
        ModeDetector d;
        d.OnRatio(0.0);
        d.OnEligibleCreated(-1.0f);
        d.OnRatio(kQuality);
        d.OnEligibleCreated(-1.5625f);
        CHECK(d.decided());
        d.Reset();
        CHECK(!d.decided() && d.mode() == Mode::Add && !d.haveRatio() && d.phase() == ModeDetector::Phase::Baseline);
    }

    // The value sets are bounded: beyond 64 distinct values new ones are dropped, nothing breaks.
    {
        ValueSet s;
        for (int i = 0; i < 100; ++i)
            s.Add(-0.1f - 0.05f * static_cast<float>(i));
        CHECK(s.size() == kMaxValues);
        CHECK(!s.Add(-100.0f));
        CHECK(s.Contains(-0.1f) && s.Contains(-0.105f));
    }

    if (g_failed == 0)
        std::printf("mip_bind_mode: all passed\n");
    return g_failed == 0 ? 0 : 1;
}
