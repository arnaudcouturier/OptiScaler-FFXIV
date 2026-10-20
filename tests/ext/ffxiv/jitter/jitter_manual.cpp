#include "check.h"
#include "engine_fixture.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace ext::ffxiv;
using namespace ext::ffxiv::jitter;
using namespace jitter_fakes;

// [FFXIV] JitterScaleOverride: a manual scale for in-game experiments (1.0, 0.8, 0.6, 0.5). Only while the correction key is on; a valid value
// (0.25 to 1.0) takes precedence over the calibration and goes through the same application path (reads before the write, X then Y,
// put-back, the 0.55 limit, the cap, the stale-block guard); 0 is OFF and is never a scale; anything else is ignored with one line.

namespace
{
const std::string kKey = "render 1920x1080, output 3840x2160, quality 2";

void Sequence(unsigned n, float& jx, float& jy)
{
    auto fraction = [](double v) { return v - std::floor(v); };
    jx = 0.05f + 0.4f * static_cast<float>(fraction(n * 0.6180339887498949));
    jy = -(0.05f + 0.4f * static_cast<float>(fraction(n * 0.7548776662466927)));
}

// A game that applies 60% of what it reports (s = 1.2); `manual` is the value of the ini key at every evaluate.
struct Session
{
    FakeEnv e = MakeEnv();
    Engine engine;
    float s = 1.2f;
    float manual = 0.0f;
    int counter = 100;
    std::uint64_t t = 0, stepMs = 16;
    unsigned n = 0;
    bool sequence = true, refresh = true, probe = false, correct = true;
    float reportedX = 0.0f, reportedY = 0.0f;

    Frame Make()
    {
        ++n;
        ++counter;
        t += stepMs;
        float jx = 0.3f, jy = -0.2f;
        if (sequence)
            Sequence(n, jx, jy);
        Frame f = At(counter, s, t, jx, jy);
        f.noRefresh = !refresh && n > 1;
        reportedX = jx;
        reportedY = jy;
        return f;
    }
    Outcome Next() { return Evaluate(e, engine, Make(), probe, correct, manual); }
};

bool Wrote(const Outcome& o, double x, double y, double tol = 1e-4)
{
    return o.sets.size() == 2 && o.sets[0].param == Param::JitterX && o.sets[1].param == Param::JitterY && Near(o.sets[0].value, x, tol) &&
           Near(o.sets[1].value, y, tol);
}

bool ScaledNext(Session& g, double ratio)
{
    const Outcome o = g.Next();
    return Wrote(o, g.reportedX * ratio, g.reportedY * ratio);
}

std::string StatusOf(const Outcome& o)
{
    for (const std::string& line : o.logs)
        if (Has(line, "Jitter correction: on;"))
            return line;
    return "";
}

// ---- classification of the key's value -----------------------------------------------------------------------------------------------

void TestTheValuesOfTheKey()
{
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    CHECK(ClassifyManualScale(0.0f) == ManualScaleKind::Off && ClassifyManualScale(-0.0f) == ManualScaleKind::Off);
    for (float v : { 0.25f, 0.26f, 0.5f, 0.6f, 0.8f, 0.99f, 1.0f })
        CHECK(ClassifyManualScale(v) == ManualScaleKind::Valid);
    for (float v : { std::nextafter(0.25f, 0.0f), 0.2499f, 0.1f, 1e-30f, std::nextafter(1.0f, 2.0f), 1.0001f, 1.5f, 100.0f, -0.5f, -1.0f,
                     -0.0001f, nan, inf, -inf })
        CHECK_MSG(ClassifyManualScale(v) == ManualScaleKind::Ignored, "value %g", v);
    // within the tolerance of 1 the baseline: nothing to write
    CHECK(ManualScaleIsBaseline(1.0f) && ManualScaleIsBaseline(0.99f) && ManualScaleIsBaseline(0.981f) && ManualScaleIsBaseline(0.98f));
    CHECK(!ManualScaleIsBaseline(0.979f) && !ManualScaleIsBaseline(0.8f) && !ManualScaleIsBaseline(0.25f));
}

// ---- the four values to try --------------------------------------------------------------------------------------------------------

void TestTheValuesToTryScaleEveryEvaluateFromTheFirst()
{
    for (float v : { 0.8f, 0.6f, 0.5f, 0.25f })
    {
        Session g;
        g.manual = v;
        std::vector<std::string> logs;
        for (int i = 1; i <= 40; ++i)
        {
            const Outcome o = g.Next();
            logs.insert(logs.end(), o.logs.begin(), o.logs.end());
            CHECK_MSG(Wrote(o, g.reportedX * v, g.reportedY * v), "value %.2f, evaluate %d: %zu sets", v, i, o.sets.size());
        }
        char expected[160];
        std::snprintf(expected, sizeof(expected), "Jitter correction: manual scale %.3f in use, calibration off", v);
        CHECK(CountLines(logs, expected) == 1);
        CHECK(CountLines(logs, "learning the game's jitter ratio") == 0 && CountLines(logs, "calibrated ratio") == 0);
    }
    // 1.0 is the baseline: nothing is written, and the status says so
    Session base;
    base.manual = 1.0f;
    for (int i = 1; i <= 40; ++i)
        base.Next();
    CHECK(base.e.setsEver == 0);
    CHECK(CountLines(base.e.logs, "Jitter correction: manual scale 1.000 in use, calibration off") == 1);
    base.t = 10000;
    const std::string status = StatusOf(base.Next());
    CHECK_MSG(Has(status, "41 evaluates seen, 0 scaled, 0 passed through while learning, 0 passed through because the jitter pair was not usable, 0 already scaled "
                          "(block not refreshed), 41 left alone (nothing to apply); state: manual scale 1.000 (calibration off);"),
              "%s", status.c_str());
    // within the tolerance of 1 it is the baseline too
    Session near;
    near.manual = 0.99f;
    for (int i = 1; i <= 10; ++i)
        near.Next();
    CHECK(near.e.setsEver == 0);
}

void TestTheStatusLineNamesTheManualScale()
{
    Session g;
    g.manual = 0.8f;
    g.sequence = false;
    for (int i = 1; i <= 20; ++i)
        g.Next();
    g.t = 10000;
    const std::string status = StatusOf(g.Next());
    CHECK_MSG(status ==
                  "Jitter correction: on; totals since the start of the session: 21 evaluates seen, 21 scaled, 0 passed through while learning, 0 passed "
                  "through because the jitter pair was not usable, 0 already scaled (block not refreshed), 0 left alone (nothing to apply); state: "
                  "manual scale 0.800 (calibration off); snapshots: 0 accepted, 0 disagreed with the ratio, 0 ratio changes, 0 restarts after a "
                  "configuration change; verification fresh; frame generation active no (input Upscaler)",
              "%s", status.c_str());
}

// ---- 0 is off ------------------------------------------------------------------------------------------------------------------------

void TestZeroIsOffAndNeverAScale()
{
    Session g; // manual = 0: the calibration decides exactly as without the key
    int zeroWrites = 0;
    for (int i = 1; i <= 40; ++i)
    {
        const Outcome o = g.Next();
        if (i < 5)
            CHECK(o.sets.empty());
        else
            CHECK(Wrote(o, g.reportedX * 0.6, g.reportedY * 0.6));
        for (const Op& op : o.sets)
            zeroWrites += op.value == 0.0f ? 1 : 0;
    }
    CHECK(zeroWrites == 0);
    CHECK(CountLines(g.e.logs, "manual scale") == 0); // no line for an unset key
    // a negative zero is off too
    Session z;
    z.manual = -0.0f;
    for (int i = 1; i <= 10; ++i)
        z.Next();
    CHECK(z.e.setsEver == 2 * 6 && CountLines(z.e.logs, "manual scale") == 0);
}

// ---- values that are ignored ----------------------------------------------------------------------------------------------------------

void TestInvalidValuesAreIgnoredWithOneLinePerEpisode()
{
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    struct Case
    {
        float value;
        const char* shown;
    };
    for (const Case& c : { Case { nan, "nan" }, Case { -0.5f, "-0.500" }, Case { 0.2f, "0.200" }, Case { 1.01f, "1.010" }, Case { inf, "inf" },
                           Case { -inf, "-inf" }, Case { 100.0f, "100.000" } })
    {
        Session g;
        g.manual = c.value;
        std::vector<std::string> logs;
        for (int i = 1; i <= 20; ++i)
        {
            const Outcome o = g.Next();
            logs.insert(logs.end(), o.logs.begin(), o.logs.end());
            // the calibration goes on as if the key were 0
            CHECK_MSG(i < 5 ? o.sets.empty() : Wrote(o, g.reportedX * 0.6, g.reportedY * 0.6), "value %s, evaluate %d", c.shown, i);
        }
        const std::string line = std::string("Jitter correction: manual scale value ") + c.shown +
                                 " ignored (valid values are 0.25 to 1.0, 0 turns it off); the calibration goes on";
        CHECK_MSG(CountLines(logs, line.c_str()) == 1, "value %s", c.shown);
        CHECK(CountLines(logs, "in use, calibration off") == 0);
    }
    // a new invalid value is a new line, the same value is not
    Session g;
    g.manual = 1.5f;
    std::vector<std::string> logs;
    auto run = [&](int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const Outcome o = g.Next();
            logs.insert(logs.end(), o.logs.begin(), o.logs.end());
        }
    };
    run(5);
    g.manual = 2.0f;
    run(5);
    g.manual = 2.0f;
    run(5);
    CHECK(CountLines(logs, "manual scale value 1.500 ignored") == 1 && CountLines(logs, "manual scale value 2.000 ignored") == 1);
    // valid, then invalid again, then the same invalid value: a new episode, a new line
    g.manual = 0.8f;
    run(2);
    g.manual = 2.0f;
    run(2);
    CHECK(CountLines(logs, "manual scale value 2.000 ignored") == 2);
}

// ---- precedence and switching -------------------------------------------------------------------------------------------------------

void TestTheOverrideTakesPrecedenceOverAnAdoptedRatioAndSwitchingOffLearnsFromScratch()
{
    Session g;
    for (int i = 1; i <= 10; ++i)
        g.Next(); // calibrated at 0.6
    CHECK(g.e.setsEver == 2 * 6);
    g.manual = 0.8f; // switched on: the calibration state is dropped, every evaluate is scaled by 0.8 at once
    std::vector<std::string> logs;
    for (int i = 1; i <= 10; ++i)
    {
        const Outcome o = g.Next();
        logs.insert(logs.end(), o.logs.begin(), o.logs.end());
        CHECK_MSG(Wrote(o, g.reportedX * 0.8, g.reportedY * 0.8), "evaluate %d with the override on", i);
    }
    CHECK(CountLines(logs, "Jitter correction: manual scale 0.800 in use, calibration off") == 1);
    g.manual = 0.0f; // switched off: learning from scratch (not the old 0.6 at once)
    logs.clear();
    for (int i = 1; i <= 9; ++i)
    {
        const Outcome o = g.Next();
        logs.insert(logs.end(), o.logs.begin(), o.logs.end());
        CHECK_MSG(i < 5 ? o.sets.empty() : Wrote(o, g.reportedX * 0.6, g.reportedY * 0.6), "evaluate %d after switching the override off", i);
    }
    CHECK(CountLines(logs, ("learning the game's jitter ratio for " + kKey).c_str()) == 1);
    CHECK(CountLines(logs, "calibrated ratio 0.600/0.600") == 1);
}

void TestTheValueCanBeChangedLive()
{
    Session g;
    std::vector<std::string> logs;
    for (float v : { 0.8f, 0.6f, 0.5f, 0.8f })
    {
        g.manual = v;
        for (int i = 0; i < 4; ++i)
        {
            const Outcome o = g.Next();
            logs.insert(logs.end(), o.logs.begin(), o.logs.end());
            CHECK(Wrote(o, g.reportedX * v, g.reportedY * v));
        }
    }
    // one line per change of the value (0.8 appears twice: two lines)
    CHECK(CountLines(logs, "manual scale 0.800 in use, calibration off") == 2);
    CHECK(CountLines(logs, "manual scale 0.600 in use, calibration off") == 1 && CountLines(logs, "manual scale 0.500 in use, calibration off") == 1);
    CHECK(CountLines(logs, "first scaled pair") == 1); // announced when switched on from off, not at every change of the value
}

// A change of the value never compounds (F1): the block of a game that never refreshes it holds the pair written with the old value; the new
// value is applied to the game's RAW pair remembered with it, and a baseline (1.0) or 0 puts the raw pair back.
void TestAChangeOfTheValueIsAppliedToTheRawPair()
{
    Session g;
    g.manual = 0.8f;
    g.sequence = false;
    g.refresh = false;
    CHECK(Wrote(g.Next(), 0.24, -0.16));
    for (int i = 0; i < 3; ++i)
        CHECK(g.Next().sets.empty());
    g.manual = 0.5f;
    CHECK(Wrote(g.Next(), 0.15, -0.1)); // 0.3 * 0.5, not 0.3 * 0.8 * 0.5
    CHECK(g.Next().sets.empty());       // and that result is remembered
    g.manual = 1.0f;                     // the baseline writes nothing new, but the raw pair is put back
    CHECK(Wrote(g.Next(), 0.3, -0.2));
    CHECK(g.Next().sets.empty());
    g.manual = 0.6f;
    CHECK(Wrote(g.Next(), 0.18, -0.12));
    g.manual = 0.6005f; // a step far below any tolerance is still a different pair and is written
    CHECK(Wrote(g.Next(), 0.3 * 0.6005, -0.2 * 0.6005, 1e-6));
    g.manual = 0.0f; // off: the calibration is learning, the raw pair is put back
    CHECK(Wrote(g.Next(), 0.3, -0.2));
    CHECK(g.Next().sets.empty());
}

// A 100-value slider sweep on a block the game never refreshes: every evaluate ends at raw * value (or raw for 0 and the baseline).
void TestASweepOnAnUnrefreshedBlockAlwaysEndsAtRawTimesValue()
{
    Session g;
    g.sequence = false;
    g.refresh = false;
    for (int i = 0; i < 100; ++i)
    {
        const float v = i % 10 == 9 ? 0.0f : (i % 10 == 8 ? 1.0f : 0.30f + 0.006f * static_cast<float>(i));
        g.manual = v;
        g.Next();
        const double k = v == 0.0f || v == 1.0f ? 1.0 : v;
        CHECK_MSG(Near(g.e.floats[Param::JitterX], 0.3 * k) && Near(g.e.floats[Param::JitterY], -0.2 * k), "value %.3f ends at %f/%f", v,
                  g.e.floats[Param::JitterX], g.e.floats[Param::JitterY]);
    }
}

// Switching the correction off (the probe still on) and on again starts the manual scale fresh: the line and the read-back come again.
void TestSwitchingTheCorrectionOffAndOnStartsTheManualScaleFresh()
{
    Session g;
    g.manual = 0.8f;
    g.sequence = false;
    for (int i = 0; i < 5; ++i)
        g.Next();
    CHECK(CountLines(g.e.logs, "manual scale 0.800 in use, calibration off") == 1 && CountLines(g.e.logs, "first scaled pair") == 1);
    g.correct = false;
    g.probe = true;
    for (int i = 0; i < 3; ++i)
        CHECK(g.Next().sets.empty());
    g.correct = true;
    g.probe = false;
    for (int i = 0; i < 3; ++i)
        CHECK(ScaledNext(g, 0.8));
    CHECK(CountLines(g.e.logs, "manual scale 0.800 in use, calibration off") == 2 && CountLines(g.e.logs, "first scaled pair") == 2);
}

// The menu slider writes the key at every frame of a drag, so every value in between is a "new value" for the engine. A sweep must not use
// up the line budgets the calibration needs (the first-scaled-pair line of the calibrated mode shares its budget with the manual mode),
// and only the switch-on from off announces the first scaled pair of a manual scale.
void TestASweepOfTheValueDoesNotUseUpTheCalibrationLines()
{
    Session g;
    g.sequence = false;
    for (int i = 0; i < 100; ++i)
    {
        g.manual = 0.30f + 0.006f * static_cast<float>(i); // a new value at every evaluate, all valid
        CHECK(!g.Next().sets.empty());
    }
    CHECK(CountLines(g.e.logs, "first scaled pair") == 1); // the switch-on from off, not every value
    CHECK(CountLines(g.e.logs, "in use, calibration off") == 32); // one per value, under the 32-line budget
    g.manual = 0.0f;
    std::vector<std::string> logs;
    for (int i = 0; i < 6; ++i)
    {
        const Outcome o = g.Next();
        logs.insert(logs.end(), o.logs.begin(), o.logs.end());
    }
    CHECK(CountLines(logs, "calibrated ratio 0.600/0.600") == 1);
    CHECK_MSG(CountLines(logs, "first scaled pair of this calibration") == 1, "the calibrated mode lost its read-back line"); // R9
    // switching the manual scale off and on again announces again
    g.manual = 0.8f;
    CHECK(CountLines(g.Next().logs, "first scaled pair") == 1);
}

// The menu snaps the slider to steps of 0.05 and turns anything below 0.25 into 0, so a drag can land on exactly 0.6 or 0.8 and cannot
// produce an ignored value.
void TestTheSnapOfTheSlider()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(SnapManualScale(0.0f) == 0.0f && SnapManualScale(0.03f) == 0.0f && SnapManualScale(0.2f) == 0.0f && SnapManualScale(0.22f) == 0.0f);
    CHECK(SnapManualScale(0.23f) == 0.25f && SnapManualScale(0.25f) == 0.25f && SnapManualScale(0.27f) == 0.25f);
    CHECK(SnapManualScale(0.5f) == 0.5f && SnapManualScale(0.504f) == 0.5f && SnapManualScale(0.476f) == 0.5f);
    CHECK(SnapManualScale(0.6f) == 0.6f && SnapManualScale(0.604f) == 0.6f && SnapManualScale(0.589f) == 0.6f);
    CHECK(SnapManualScale(0.8f) == 0.8f && SnapManualScale(0.78f) == 0.8f && SnapManualScale(0.83f) == 0.85f);
    CHECK(SnapManualScale(0.97f) == 0.95f && SnapManualScale(0.99f) == 1.0f && SnapManualScale(1.0f) == 1.0f);
    CHECK(SnapManualScale(1.2f) == 1.0f && SnapManualScale(100.0f) == 1.0f);
    CHECK(SnapManualScale(-0.5f) == 0.0f && SnapManualScale(nan) == 0.0f);
    // whatever the slider hands over, the result is a value the engine accepts or 0
    for (int i = 0; i <= 1000; ++i)
    {
        const float s = SnapManualScale(static_cast<float>(i) / 1000.0f);
        CHECK(ClassifyManualScale(s) != ManualScaleKind::Ignored);
    }
}

void TestTheOverrideChangesNothingWhileTheCorrectionKeyIsOff()
{
    Session g;
    g.manual = 0.8f;
    g.probe = true;
    g.correct = false;
    for (int i = 1; i <= 30; ++i)
        g.Next();
    CHECK(g.e.setsEver == 0 && CountLines(g.e.logs, "manual scale") == 0 && CountLines(g.e.logs, "Jitter correction") == 0);
    // both keys off: zero cost, whatever the override says
    FakeEnv e = MakeEnv();
    Engine engine;
    for (int i = 0; i < 20; ++i)
    {
        Frame f = At(100 + i, 1.2f, static_cast<std::uint64_t>(i) * 16);
        const Outcome o = Evaluate(e, engine, f, false, false, 0.8f);
        CHECK(o.ops.empty() && o.logs.empty());
    }
    CHECK(e.locks == 0 && e.buildLookups == 0 && e.clockReads == 0 && e.mem.reads == 0);
    // switching the correction on later starts from the override, not from a stale one
    g.correct = true;
    CHECK(ScaledNext(g, 0.8));
    g.manual = 0.8f;
    const Outcome on = g.Next();
    CHECK(Wrote(on, g.reportedX * 0.8, g.reportedY * 0.8));
}

void TestABuildMismatchStaysIdleWithTheOverride()
{
    Session g;
    g.e.build = nullptr;
    g.manual = 0.8f;
    for (int i = 0; i < 20; ++i)
        g.Next();
    CHECK(g.e.setsEver == 0 && g.e.mem.reads == 0 && g.e.ops.empty());
    CHECK(g.e.logs.size() == 1 && Has(g.e.logs[0], "idle for this session"));
}

// ---- the same application path ------------------------------------------------------------------------------------------------------------

void TestReadsComeBeforeTheWriteAndTheReadBackIsOnce()
{
    Session g;
    g.manual = 0.8f;
    g.sequence = false;
    const Outcome first = g.Next();
    CHECK(Wrote(first, 0.24, -0.16));
    std::size_t firstSet = first.ops.size(), readX = first.ops.size(), readY = first.ops.size();
    for (std::size_t i = 0; i < first.ops.size(); ++i)
    {
        if (first.ops[i].set && firstSet == first.ops.size())
            firstSet = i;
        if (!first.ops[i].set && first.ops[i].param == Param::JitterX && readX == first.ops.size())
            readX = i;
        if (!first.ops[i].set && first.ops[i].param == Param::JitterY && readY == first.ops.size())
            readY = i;
    }
    CHECK(readX < firstSet && readY < firstSet);
    CHECK(first.sets[0].param == Param::JitterX && first.sets[1].param == Param::JitterY);
    CHECK(CountLines(first.logs, "first scaled pair of this calibration: reported (0.3000,-0.2000) scaled to (0.2400,-0.1600) by ratio 0.800; parameter block read back: (0.2400,-0.1600)") == 1);
    const Outcome second = g.Next();
    CHECK(Wrote(second, 0.24, -0.16) && CountLines(second.logs, "first scaled pair") == 0);
    int jitterReads = 0;
    for (const Op& op : second.ops)
        jitterReads += !op.set && (op.param == Param::JitterX || op.param == Param::JitterY) ? 1 : 0;
    CHECK(jitterReads == 2);
}

void TestAPairThatIsNotUsableIsNotScaled()
{
    Session g;
    g.manual = 0.8f;
    g.sequence = false;
    for (int i = 1; i <= 3; ++i)
        CHECK(Wrote(g.Next(), 0.24, -0.16));
    Frame big = g.Make();
    big.ngxJx = 0.6f; // above the 0.55 limit
    CHECK(Evaluate(g.e, g.engine, big, false, true, 0.8f).sets.empty());
    Frame missing = g.Make();
    missing.ngxMissing = true;
    CHECK(Evaluate(g.e, g.engine, missing, false, true, 0.8f).sets.empty());
    CHECK(CountLines(g.e.logs, "jitter pair not usable, game values passed through unchanged: NGX jitter value above 0.55 pixel") == 1);
    CHECK(Wrote(g.Next(), 0.24, -0.16));
}

void TestTheStaleBlockGuardWorksWithTheOverride()
{
    Session g;
    g.manual = 0.8f;
    g.sequence = false;
    g.refresh = false; // the game never sets Jitter.Offset again after the first evaluate
    for (int i = 1; i <= 50; ++i)
        g.Next();
    CHECK(g.e.setsEver == 2);
    CHECK(Near(g.e.floats[Param::JitterX], 0.24) && Near(g.e.floats[Param::JitterY], -0.16)); // x * v, never x * v^2
    g.t = 10000;
    const std::string status = StatusOf(g.Next());
    CHECK_MSG(Has(status, "51 evaluates seen, 1 scaled, 0 passed through while learning, 0 passed through because the jitter pair was not usable, 50 already scaled "
                          "(block not refreshed)"),
              "%s", status.c_str());
    // a game that refreshes every second evaluate: scaled on the refreshed ones only
    Session h;
    h.manual = 0.8f;
    h.sequence = false;
    int scaled = 0;
    for (int i = 1; i <= 40; ++i)
    {
        Frame f = h.Make();
        f.noRefresh = i > 1 && i % 2 == 1;
        const Outcome o = Evaluate(h.e, h.engine, f, false, true, 0.8f);
        CHECK_MSG(f.noRefresh ? o.sets.empty() : Wrote(o, 0.24, -0.16), "evaluate %d", i);
        scaled += o.sets.empty() ? 0 : 1;
    }
    CHECK(scaled == 21);
    // the seam called twice for one evaluate
    Session d;
    d.manual = 0.8f;
    d.sequence = false;
    for (int i = 0; i < 10; ++i)
    {
        Frame f = d.Make();
        CHECK(Wrote(Evaluate(d.e, d.engine, f, false, true, 0.8f), 0.24, -0.16));
        f.noRefresh = true;
        CHECK(Evaluate(d.e, d.engine, f, false, true, 0.8f).sets.empty());
    }
}

void TestAHalfWrittenPairIsHandledLikeTheCalibratedOne()
{
    auto exceptions = [](const Session& g) { return CountLines(g.e.logs, "step abandoned after an exception"); };
    {
        // Y refused, the put-back of X works: the block holds the game's pair again, nothing is remembered, the unrefreshed block is scaled once
        Session g;
        g.manual = 0.8f;
        g.sequence = false;
        g.refresh = false;
        g.Next(); // n = 1 writes (refresh on the first evaluate): block 0.24/-0.16 ... so reset the block to the game's raw pair
        g.e.floats[Param::JitterX] = 0.3f;
        g.e.floats[Param::JitterY] = -0.2f;
        int sets = 0;
        g.e.fault = [&sets](const char* where)
        {
            if (std::string(where) == "SetFloat" && ++sets == 2)
                throw std::runtime_error("Y refused");
        };
        const Outcome o = g.Next();
        CHECK(exceptions(g) == 1 && o.sets.size() == 2 && o.sets[1].param == Param::JitterX && Near(o.sets[1].value, 0.3));
        CHECK(Near(g.e.floats[Param::JitterX], 0.3) && Near(g.e.floats[Param::JitterY], -0.2));
        g.e.fault = nullptr;
        CHECK(Wrote(g.Next(), 0.24, -0.16));
        CHECK(g.Next().sets.empty());
    }
    {
        // Y refused AND the put-back fails: the mixed pair (x*v, y raw) is remembered, the next evaluate does not scale X again
        Session g;
        g.manual = 0.8f;
        g.sequence = false;
        g.refresh = false;
        g.Next();
        g.e.floats[Param::JitterX] = 0.3f;
        g.e.floats[Param::JitterY] = -0.2f;
        int sets = 0;
        g.e.fault = [&sets](const char* where)
        {
            if (std::string(where) == "SetFloat" && ++sets >= 2)
                throw std::runtime_error("refused");
        };
        g.Next();
        CHECK(exceptions(g) == 1 && Near(g.e.floats[Param::JitterX], 0.24) && Near(g.e.floats[Param::JitterY], -0.2));
        g.e.fault = nullptr;
        CHECK(Wrote(g.Next(), 0.24, -0.16)); // the mixed pair is recognised, the raw pair re-derived: Y is repaired
        for (int i = 0; i < 4; ++i)
            CHECK(g.Next().sets.empty());
        CHECK(Near(g.e.floats[Param::JitterX], 0.24) && Near(g.e.floats[Param::JitterY], -0.16));
    }
}
} // namespace

int main()
{
    TestTheValuesOfTheKey();
    TestTheValuesToTryScaleEveryEvaluateFromTheFirst();
    TestTheStatusLineNamesTheManualScale();
    TestZeroIsOffAndNeverAScale();
    TestInvalidValuesAreIgnoredWithOneLinePerEpisode();
    TestTheOverrideTakesPrecedenceOverAnAdoptedRatioAndSwitchingOffLearnsFromScratch();
    TestTheValueCanBeChangedLive();
    TestAChangeOfTheValueIsAppliedToTheRawPair();
    TestASweepOnAnUnrefreshedBlockAlwaysEndsAtRawTimesValue();
    TestASweepOfTheValueDoesNotUseUpTheCalibrationLines();
    TestTheSnapOfTheSlider();
    TestSwitchingTheCorrectionOffAndOnStartsTheManualScaleFresh();
    TestTheOverrideChangesNothingWhileTheCorrectionKeyIsOff();
    TestABuildMismatchStaysIdleWithTheOverride();
    TestReadsComeBeforeTheWriteAndTheReadBackIsOnce();
    TestAPairThatIsNotUsableIsNotScaled();
    TestTheStaleBlockGuardWorksWithTheOverride();
    TestAHalfWrittenPairIsHandledLikeTheCalibratedOne();
    return TEST_RESULT("jitter_manual");
}
