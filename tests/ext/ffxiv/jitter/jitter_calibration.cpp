#include "check.h"
#include "fake_game.h"
#include "ext/ffxiv/jitter/FfxivJitterCalibration.h"

#include <cmath>
#include <limits>
#include <optional>
#include <string>

using namespace ext::ffxiv;
using namespace ext::ffxiv::jitter;
using namespace jitter_fakes;

// The pure core of calibrate-and-scale (FfxivJitterCalibration.h) on fake snapshots: no engine, no memory, no clock but the one passed in.

namespace
{
const RenderSize kHd { 1920, 1080, true, "test" };

bool Close(double a, double b, double tol = 1e-4) { return std::fabs(a - b) <= tol; }
bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// An object whose applied/reported ratios are exactly rx and ry (ndcX = 2 rx jx / W, ndcY = -2 ry jy / H, the inverse of Analyse).
RawObject WithRatios(float jx, float jy, float rx, float ry, int counter)
{
    return { jx, jy, 2.0f * rx * jx / 1920.0f, -2.0f * ry * jy / 1080.0f, counter };
}

// Feeds snapshots with a counter that goes up by one and a clock that moves 16 ms per snapshot.
struct Feeder
{
    Calibrator cal;
    int counter = 0;
    std::uint64_t now = 0;

    Observation Feed(float rx, float ry, float jx = 0.3f, float jy = -0.2f)
    {
        ++counter;
        now += 16;
        return cal.Observe(Guard::None, Analyse(WithRatios(jx, jy, rx, ry, counter), kHd), now);
    }
    Observation Feed(float r) { return Feed(r, r); }
    void FeedN(float r, int n)
    {
        for (int i = 0; i < n; ++i)
            Feed(r);
    }
};

// ---- classification --------------------------------------------------------------------------------------------------------------

void TestClassEdges()
{
    CHECK(kAdoptSnapshots == 5 && kReplaceSnapshots == 5 && kStaleMs == 10000);
    CHECK(kMinPlausibleRatio == 0.25f && kMaxReportedPixels == 0.55f);
    // the 0.25 edge: 0.25 itself is a scale, the float just below is refused
    CHECK(ClassifyRatio(0.25f) == Mode::Scale);
    CHECK(ClassifyRatio(std::nextafter(0.25f, 0.0f)) == Mode::Refused);
    CHECK(ClassifyRatio(0.2499f) == Mode::Refused && ClassifyRatio(0.2501f) == Mode::Scale);
    CHECK(ClassifyRatio(0.0f) == Mode::Refused);
    // the 1 - tolerance edge: 0.979 scales, 0.981 is already consistent; 0.98 itself (as a float) is within the tolerance
    CHECK(ClassifyRatio(0.979f) == Mode::Scale && ClassifyRatio(0.981f) == Mode::Neutral);
    CHECK(ClassifyRatio(0.98f) == Mode::Neutral);
    // the 1 + tolerance edge: 1.019 is consistent, 1.021 and everything above is refused (a scale up is not understood)
    CHECK(ClassifyRatio(1.019f) == Mode::Neutral && ClassifyRatio(1.021f) == Mode::Refused);
    CHECK(ClassifyRatio(1.02f) == Mode::Neutral);
    CHECK(ClassifyRatio(1.0f) == Mode::Neutral && ClassifyRatio(1.5f) == Mode::Refused && ClassifyRatio(3.0f) == Mode::Refused);
    CHECK(ClassifyRatio(0.6f) == Mode::Scale && ClassifyRatio(0.5f) == Mode::Scale && ClassifyRatio(0.9f) == Mode::Scale);
    CHECK(ClassifyRatio(std::numeric_limits<float>::quiet_NaN()) == Mode::Refused);
    CHECK(ClassifyRatio(std::numeric_limits<float>::infinity()) == Mode::Refused);
}

// ---- adoption --------------------------------------------------------------------------------------------------------------------

void TestAdoptionAtFiveNotAtFour()
{
    Feeder f;
    for (int i = 1; i <= 4; ++i)
    {
        const Observation o = f.Feed(0.6f);
        CHECK(o.accepted && o.skipped == Guard::None && o.change == CalibrationChange::None && !o.axesDisagree);
        CHECK(f.cal.mode() == Mode::Learning && !f.cal.adopted());
        CHECK(f.cal.runLength() == static_cast<unsigned>(i));
    }
    const Observation fifth = f.Feed(0.6f);
    CHECK(fifth.accepted && fifth.change == CalibrationChange::Adopted);
    CHECK(f.cal.mode() == Mode::Scale && f.cal.adopted());
    CHECK(Close(f.cal.ratio(), 0.6) && Close(f.cal.ratioX(), 0.6) && Close(f.cal.ratioY(), 0.6) && f.cal.adoptedFrom() == 5);
    CHECK(f.cal.runLength() == 0);
    CHECK(f.cal.counts().accepted == 5 && f.cal.counts().disagreed == 0 && f.cal.counts().changes == 0);
    // further agreeing snapshots change nothing and say they agreed
    const Observation more = f.Feed(0.6f);
    CHECK(more.accepted && more.agreed && !more.disagreed && more.change == CalibrationChange::None);
}

void TestAdoptedRatioIsTheMeanOfTheRun()
{
    Feeder f;
    for (float r : { 0.595f, 0.600f, 0.605f, 0.600f, 0.600f })
        f.Feed(r);
    CHECK(f.cal.mode() == Mode::Scale && Close(f.cal.ratio(), 0.60, 1e-5));
    // the two axes keep their own means (the log shows both); the shared ratio is their average
    Feeder g;
    for (int i = 0; i < 5; ++i)
        g.Feed(0.595f, 0.605f);
    CHECK(g.cal.mode() == Mode::Scale && Close(g.cal.ratioX(), 0.595, 1e-5) && Close(g.cal.ratioY(), 0.605, 1e-5) && Close(g.cal.ratio(), 0.60, 1e-5));
}

void TestARunRestartsOnADisagreeingSnapshot()
{
    Feeder f;
    f.FeedN(0.6f, 3);
    CHECK(f.cal.runLength() == 3);
    f.Feed(0.7f); // disagrees with the run (0.6): it restarts, the snapshot is the first member
    CHECK(f.cal.runLength() == 1 && f.cal.mode() == Mode::Learning);
    f.FeedN(0.7f, 3);
    CHECK(f.cal.mode() == Mode::Learning && f.cal.runLength() == 4);
    const Observation o = f.Feed(0.7f);
    CHECK(o.change == CalibrationChange::Adopted && Close(f.cal.ratio(), 0.7) && f.cal.adoptedFrom() == 5);
    // the first three never counted for the new ratio: 3 + 1 + 4 = 8 accepted snapshots, 5 of them in the adopted run
    CHECK(f.cal.counts().accepted == 8);

    // the tolerance edge, relative to the mean of the run: 2% of 0.6 is 0.012
    Feeder g;
    g.FeedN(0.6f, 2);
    g.Feed(0.6f * 1.019f);
    CHECK(g.cal.runLength() == 3); // inside the tolerance: the run goes on
    g.Feed(0.6f * 1.05f);
    CHECK(g.cal.runLength() == 1); // outside it: restarted
}

void TestAxesThatDisagreeAreNotUsedForLearning()
{
    Feeder f;
    f.FeedN(0.6f, 4);
    const Observation o = f.Feed(0.6f, 0.7f); // accepted, but there is no single ratio in it
    CHECK(o.accepted && o.axesDisagree && o.change == CalibrationChange::None && !o.disagreed);
    CHECK(f.cal.runLength() == 0 && f.cal.mode() == Mode::Learning); // the run is emptied (D1)
    f.FeedN(0.6f, 4);
    CHECK(f.cal.mode() == Mode::Learning);
    CHECK(f.cal.counts().disagreed == 0); // "disagreed" counts against an ADOPTED ratio only
    f.Feed(0.6f);
    CHECK(f.cal.mode() == Mode::Scale);
    // the axes agree when they are within the tolerance relative to the larger one
    Feeder g;
    g.Feed(0.6f, 0.6f * 1.019f);
    CHECK(g.cal.runLength() == 1);
    g.Feed(0.6f, 0.6f * 1.03f);
    CHECK(g.cal.runLength() == 0); // 3% apart: not one ratio
}

// The axes agree when they are within the tolerance relative to the LARGER one: 1.0 against 0.9801 (1.99% of the larger, 2.03% of the smaller)
// agree in either order; 1.0 against 0.9799 do not.
void TestTheAxesToleranceIsRelativeToTheLargerAxis()
{
    for (int order = 0; order < 2; ++order)
    {
        Feeder f;
        for (int i = 0; i < 5; ++i)
            order == 0 ? f.Feed(0.6f, 0.6f * 0.9801f) : f.Feed(0.6f * 0.9801f, 0.6f);
        CHECK_MSG(f.cal.mode() == Mode::Scale, "order %d: axes 1.99%% apart were not accepted", order);
        Feeder g;
        for (int i = 0; i < 5; ++i)
            order == 0 ? g.Feed(0.6f, 0.6f * 0.9799f) : g.Feed(0.6f * 0.9799f, 0.6f);
        CHECK_MSG(g.cal.mode() == Mode::Learning, "order %d: axes 2.01%% apart were accepted", order);
    }
}

// ---- verification and replacement ------------------------------------------------------------------------------------------------

Feeder Calibrated(float r = 0.6f)
{
    Feeder f;
    f.FeedN(r, 5);
    return f;
}

void TestOneOutlierIsIgnored()
{
    Feeder f = Calibrated();
    const Observation o = f.Feed(0.8f);
    CHECK(o.accepted && o.disagreed && !o.agreed && o.change == CalibrationChange::None);
    CHECK(f.cal.mode() == Mode::Scale && Close(f.cal.ratio(), 0.6) && f.cal.counts().disagreed == 1 && f.cal.counts().changes == 0);
    // an agreeing snapshot resets the contradiction: four more outliers after it still do not replace
    f.Feed(0.6f);
    f.FeedN(0.8f, 4);
    CHECK(f.cal.mode() == Mode::Scale && Close(f.cal.ratio(), 0.6) && f.cal.counts().changes == 0);
    f.Feed(0.6f);
    f.FeedN(0.8f, 4);
    CHECK(Close(f.cal.ratio(), 0.6) && f.cal.counts().changes == 0);
}

void TestFourContradictionsDoNotReplaceFiveDo()
{
    Feeder f = Calibrated();
    for (int i = 1; i <= 4; ++i)
    {
        const Observation o = f.Feed(0.8f);
        CHECK(o.disagreed && o.change == CalibrationChange::None);
        CHECK(Close(f.cal.ratio(), 0.6));
    }
    const Observation fifth = f.Feed(0.8f);
    CHECK(fifth.disagreed && fifth.change == CalibrationChange::Replaced);
    CHECK(f.cal.mode() == Mode::Scale && Close(f.cal.ratio(), 0.8) && f.cal.counts().changes == 1 && f.cal.counts().disagreed == 5);
    // and the new ratio is verified like the first one
    CHECK(f.Feed(0.8f).agreed);
    CHECK(!f.Feed(0.6f).agreed);
}

void TestContradictionsThatDisagreeWithEachOtherDoNotReplace()
{
    Feeder f = Calibrated();
    for (float r : { 0.5f, 0.7f, 0.9f, 0.4f, 0.8f, 0.5f, 0.7f })
        f.Feed(r);
    CHECK(f.cal.mode() == Mode::Scale && Close(f.cal.ratio(), 0.6) && f.cal.counts().changes == 0 && f.cal.counts().disagreed == 7);
    // four that agree with each other, one stray, then five that agree: the stray restarts the candidate run
    Feeder g = Calibrated();
    g.FeedN(0.8f, 4);
    g.Feed(0.5f);
    CHECK(Close(g.cal.ratio(), 0.6));
    g.FeedN(0.5f, 3);
    CHECK(Close(g.cal.ratio(), 0.6)); // 0.5 x4 so far
    const Observation o = g.Feed(0.5f);
    CHECK(o.change == CalibrationChange::Replaced && Close(g.cal.ratio(), 0.5));
}

void TestAxesDisagreeAgainstAnAdoptedRatio()
{
    Feeder f = Calibrated();
    f.FeedN(0.8f, 4);
    const Observation o = f.Feed(0.6f, 0.9f); // counts as a disagreement, cannot join the candidate run
    CHECK(o.accepted && o.axesDisagree && o.disagreed && o.change == CalibrationChange::None);
    f.Feed(0.8f);
    CHECK(Close(f.cal.ratio(), 0.6)); // the candidate run was emptied: one 0.8 is not five
    f.FeedN(0.8f, 4);
    CHECK(Close(f.cal.ratio(), 0.8));
}

void TestNeutralAndRefused()
{
    Feeder n = Calibrated(1.0f);
    CHECK(n.cal.mode() == Mode::Neutral && n.cal.adopted() && Close(n.cal.ratio(), 1.0));
    Feeder h = Calibrated(1.5f);
    CHECK(h.cal.mode() == Mode::Refused && h.cal.adopted());
    Feeder l = Calibrated(0.2f);
    CHECK(l.cal.mode() == Mode::Refused);
    Feeder e = Calibrated(0.98f);
    CHECK(e.cal.mode() == Mode::Neutral);
    // they keep verifying (D2): five contradictions move a consistent game to a scale and back
    n.FeedN(0.6f, 5);
    CHECK(n.cal.mode() == Mode::Scale && Close(n.cal.ratio(), 0.6) && n.cal.counts().changes == 1);
    n.FeedN(1.0f, 5);
    CHECK(n.cal.mode() == Mode::Neutral && n.cal.counts().changes == 2);
    h.FeedN(0.7f, 5);
    CHECK(h.cal.mode() == Mode::Scale);
    // the class of a REPLACEMENT is decided like an adoption
    Feeder s = Calibrated();
    s.FeedN(1.5f, 5);
    CHECK(s.cal.mode() == Mode::Refused && s.cal.counts().changes == 1);
}

// ---- keying ----------------------------------------------------------------------------------------------------------------------

void TestKeyChangeDropsEverything()
{
    Feeder f = Calibrated();
    f.FeedN(0.8f, 2); // a candidate run in progress
    CHECK(f.cal.UseKey(1) == Calibrator::KeyResult::First);
    CHECK(f.cal.UseKey(1) == Calibrator::KeyResult::Unchanged);
    CHECK(f.cal.mode() == Mode::Scale); // the first key does not drop what was learned before a key was known
    CHECK(f.cal.UseKey(2) == Calibrator::KeyResult::Changed);
    CHECK(f.cal.mode() == Mode::Learning && !f.cal.adopted() && f.cal.relearning() && f.cal.counts().restarts == 1);
    CHECK(f.cal.runLength() == 0);
    // the remembered counter went too: a camera that restarts at 0 in the new configuration is accepted at once
    f.counter = 0;
    const Observation o = f.Feed(0.6f);
    CHECK(o.accepted && o.skipped == Guard::None);
    // learning starts again: five agreeing snapshots adopt (the dropped candidate run does not count), and relearning ends
    f.FeedN(0.6f, 4);
    CHECK(f.cal.mode() == Mode::Scale && !f.cal.relearning());
    CHECK(f.cal.UseKey(2) == Calibrator::KeyResult::Unchanged && f.cal.mode() == Mode::Scale);
    CHECK(f.cal.UseKey(3) == Calibrator::KeyResult::Changed && f.cal.counts().restarts == 2 && f.cal.mode() == Mode::Learning);
    // a candidate run does not survive a key change either: 4 contradictions before, 1 after is not 5
    Feeder g = Calibrated();
    g.cal.UseKey(1);
    g.FeedN(0.8f, 4);
    g.cal.UseKey(2);
    g.FeedN(0.6f, 5);
    g.Feed(0.8f);
    CHECK(Close(g.cal.ratio(), 0.6));
    // Reset drops the key too: the next key is a first key again
    g.cal.Reset();
    CHECK(g.cal.mode() == Mode::Learning && !g.cal.relearning() && g.cal.UseKey(9) == Calibrator::KeyResult::First);
}

// ---- snapshot acceptance ---------------------------------------------------------------------------------------------------------

Observation Observe(Calibrator& c, Guard g, const RawObject& o, const RenderSize& size, std::uint64_t now = 0)
{
    return c.Observe(g, g == Guard::None ? Analyse(o, size) : Analysis {}, now);
}

void TestSnapshotsThatAreSkipped()
{
    // a failed read of the object: skipped for the read's own guard, nothing changes
    {
        Feeder f = Calibrated();
        const Observation o = Observe(f.cal, Guard::ObjectUnstable, RawObject {}, kHd);
        CHECK(!o.accepted && o.skipped == Guard::ObjectUnstable && f.cal.mode() == Mode::Scale && f.cal.counts().accepted == 5);
        // ... also half way through learning: the run stays
        Feeder g;
        g.FeedN(0.6f, 3);
        Observe(g.cal, Guard::GlobalInvalid, RawObject {}, kHd);
        CHECK(g.cal.runLength() == 3);
    }
    // an untrusted render size
    {
        Calibrator c;
        const Observation o = Observe(c, Guard::None, WithRatios(0.3f, -0.2f, 0.6f, 0.6f, 1), RenderSize { 1920, 1080, false, "subrect" });
        CHECK(!o.accepted && o.skipped == Guard::RenderSizeUnknown);
        CHECK(Observe(c, Guard::None, WithRatios(0.3f, -0.2f, 0.6f, 0.6f, 1), RenderSize {}).skipped == Guard::RenderSizeUnknown);
    }
    // one axis with too little jitter: skipped, the run and the counter are untouched
    {
        Feeder f;
        f.FeedN(0.6f, 3); // counters 1-3
        const Observation tinyY = Observe(f.cal, Guard::None, WithRatios(0.3f, 0.005f, 0.6f, 0.6f, 500), kHd);
        CHECK(!tinyY.accepted && tinyY.skipped == Guard::JitterTooSmall && f.cal.runLength() == 3);
        const Observation tinyX = Observe(f.cal, Guard::None, WithRatios(0.005f, -0.2f, 0.6f, 0.6f, 501), kHd);
        CHECK(!tinyX.accepted && tinyX.skipped == Guard::JitterTooSmall && f.cal.runLength() == 3);
        const Observation bothTiny = Observe(f.cal, Guard::None, RawObject { 0.0f, 0.0f, 0.0f, 0.0f, 502 }, kHd);
        CHECK(!bothTiny.accepted && bothTiny.skipped == Guard::JitterTooSmall);
        // D3: the skipped snapshots (counter 500 and up) did not move the remembered counter, so counter 4 is still an increase
        const Observation next = f.Feed(0.6f);
        CHECK(next.accepted && f.cal.runLength() == 4);
        // the smallest jitter that has a ratio: 1/64 itself counts, just below does not
        Calibrator edge;
        CHECK(Observe(edge, Guard::None, WithRatios(kMinRatioJitter, -0.2f, 0.6f, 0.6f, 1), kHd).accepted);
        Calibrator below;
        CHECK(!Observe(below, Guard::None, WithRatios(std::nextafter(kMinRatioJitter, 0.0f), -0.2f, 0.6f, 0.6f, 1), kHd).accepted);
    }
    // sign mismatch on either axis, and a ratio of exactly 0 (D4): skipped
    {
        Calibrator c;
        RawObject flipY = WithRatios(0.3f, -0.2f, 0.6f, 0.6f, 1);
        flipY.ndcY = -flipY.ndcY;
        CHECK(Observe(c, Guard::None, flipY, kHd).skipped == Guard::SignMismatch);
        RawObject flipX = WithRatios(0.3f, -0.2f, 0.6f, 0.6f, 2);
        flipX.ndcX = -flipX.ndcX;
        CHECK(Observe(c, Guard::None, flipX, kHd).skipped == Guard::SignMismatch);
        RawObject zeroX = WithRatios(0.3f, -0.2f, 0.6f, 0.6f, 3);
        zeroX.ndcX = 0.0f;
        CHECK(Observe(c, Guard::None, zeroX, kHd).skipped == Guard::SignMismatch);
        Analysis nan = Analyse(WithRatios(0.3f, -0.2f, 0.6f, 0.6f, 4), kHd);
        nan.rx = std::numeric_limits<float>::quiet_NaN();
        CHECK(c.Observe(Guard::None, nan, 0).skipped == Guard::SignMismatch);
        CHECK(c.counts().accepted == 0);
    }
    // an applied jitter above 1 pixel, and one that is not a number
    {
        Calibrator c;
        CHECK(Observe(c, Guard::None, WithRatios(0.45f, -0.2f, 2.5f, 2.5f, 1), kHd).skipped == Guard::AppliedImplausible); // ax = 1.125
        CHECK(Observe(c, Guard::None, WithRatios(0.4f, -0.4f, 1.2f, 1.2f, 2), kHd).accepted);                         // 0.48: fine
        Analysis a = Analyse(WithRatios(0.3f, -0.2f, 0.6f, 0.6f, 3), kHd);
        a.ax = std::numeric_limits<float>::quiet_NaN();
        CHECK(c.Observe(Guard::None, a, 0).skipped == Guard::AppliedImplausible);
        a = Analyse(WithRatios(0.3f, -0.2f, 0.6f, 0.6f, 4), kHd);
        a.ay = std::numeric_limits<float>::infinity();
        CHECK(c.Observe(Guard::None, a, 0).skipped == Guard::AppliedImplausible);
        a = Analyse(WithRatios(0.3f, -0.2f, 0.6f, 0.6f, 5), kHd);
        a.ay = -1.0001f;
        CHECK(c.Observe(Guard::None, a, 0).skipped == Guard::AppliedImplausible);
        a.ay = -1.0f; // exactly the limit is fine
        a.object.counter = 6;
        CHECK(c.Observe(Guard::None, a, 0).accepted);
    }
}

void TestCounterRebases()
{
    // equal and lower counters are skipped and re-base; the next increase is accepted again
    Calibrator c;
    auto at = [&](int counter) { return c.Observe(Guard::None, Analyse(WithRatios(0.3f, -0.2f, 0.6f, 0.6f, counter), kHd), 0); };
    CHECK(at(10).accepted); // the first snapshot has nothing to compare with
    CHECK(at(11).accepted);
    const Observation same = at(11);
    CHECK(!same.accepted && same.skipped == Guard::CounterNotIncreasing); // the same camera step counts once
    CHECK(at(12).accepted);
    CHECK(at(100).accepted); // the counter may jump
    // a camera that was re-created restarts at 0: skipped once, then accepted from 1 on, without passing 100 again
    CHECK(at(0).skipped == Guard::CounterNotIncreasing);
    CHECK(at(1).accepted && at(2).accepted && at(3).accepted);
    // a counter that goes down by one re-bases too
    CHECK(at(2).skipped == Guard::CounterNotIncreasing && at(3).accepted);
    // negative counters are just numbers
    Calibrator n;
    CHECK(n.Observe(Guard::None, Analyse(WithRatios(0.3f, -0.2f, 0.6f, 0.6f, -5), kHd), 0).accepted);
    CHECK(n.Observe(Guard::None, Analyse(WithRatios(0.3f, -0.2f, 0.6f, 0.6f, -4), kHd), 0).accepted);
    // a run is not broken by a skipped duplicate
    Feeder f;
    f.FeedN(0.6f, 3);
    f.counter -= 1; // the same counter as the last accepted snapshot
    CHECK(f.Feed(0.6f).skipped == Guard::CounterNotIncreasing && f.cal.runLength() == 3);
    f.FeedN(0.6f, 2);
    CHECK(f.cal.mode() == Mode::Scale);
}

void TestAbandonedRunsKeepTheAdoptedRatio()
{
    Feeder f;
    f.FeedN(0.6f, 3);
    f.cal.AbandonRun();
    CHECK(f.cal.runLength() == 0);
    f.FeedN(0.6f, 4);
    CHECK(f.cal.mode() == Mode::Learning); // 3 + abandon + 4 is not 5
    f.Feed(0.6f);
    CHECK(f.cal.mode() == Mode::Scale);
    // an adopted ratio survives, a candidate run does not
    f.FeedN(0.8f, 4);
    f.cal.AbandonRun();
    f.Feed(0.8f);
    CHECK(f.cal.mode() == Mode::Scale && Close(f.cal.ratio(), 0.6) && f.cal.counts().changes == 0);
    static_assert(noexcept(std::declval<Calibrator&>().AbandonRun()));
    static_assert(noexcept(std::declval<Calibrator&>().Reset()));
}

// ---- the stale clock -------------------------------------------------------------------------------------------------------------

void TestStaleClock()
{
    Calibrator c;
    CHECK(!c.Stale(0) && !c.Stale(1000000)); // nothing adopted: nothing to be stale about
    std::uint64_t now = 1000;
    for (int i = 1; i <= 5; ++i)
        c.Observe(Guard::None, Analyse(WithRatios(0.3f, -0.2f, 0.6f, 0.6f, i), kHd), now);
    CHECK(c.mode() == Mode::Scale);
    CHECK(!c.Stale(1000) && !c.Stale(10999) && c.Stale(11000) && c.Stale(50000));
    CHECK(!c.Stale(500)); // a clock that goes backwards is no time
    // a skipped snapshot does not refresh it, an accepted one does
    c.Observe(Guard::ObjectUnreadable, Analysis {}, 9000);
    CHECK(c.Stale(11000));
    c.Observe(Guard::None, Analyse(WithRatios(0.3f, -0.2f, 0.6f, 0.6f, 6), kHd), 12000);
    CHECK(!c.Stale(12000) && !c.Stale(21999) && c.Stale(22000));
    // a disagreeing snapshot is an accepted one: the object is being read
    c.Observe(Guard::None, Analyse(WithRatios(0.3f, -0.2f, 0.9f, 0.9f, 7), kHd), 20000);
    CHECK(!c.Stale(29999) && c.Stale(30000));
    // Neutral and Refused are verified too
    Calibrator n;
    for (int i = 1; i <= 5; ++i)
        n.Observe(Guard::None, Analyse(WithRatios(0.3f, -0.2f, 1.0f, 1.0f, i), kHd), 0);
    CHECK(n.mode() == Mode::Neutral && !n.Stale(9999) && n.Stale(10000));
    // learning is never stale, and a key change ends it
    Calibrator l;
    l.Observe(Guard::None, Analyse(WithRatios(0.3f, -0.2f, 0.6f, 0.6f, 1), kHd), 0);
    CHECK(!l.Stale(100000));
    c.UseKey(1);
    c.UseKey(2);
    CHECK(!c.Stale(100000));
}

// ---- the application -------------------------------------------------------------------------------------------------------------

void TestScaleArithmeticAndCap()
{
    const WrittenPair none;
    auto d = DecideScale(0.3f, -0.2f, 0.6f, none);
    CHECK(d.action == ScaleAction::Scale && d.guard == Guard::None && d.x == 0.3f * 0.6f && d.y == -0.2f * 0.6f);
    CHECK(Close(d.x, 0.18) && Close(d.y, -0.12));
    // each axis keeps its own value: X is not Y
    d = DecideScale(0.5f, 0.1f, 0.5f, none);
    CHECK(Close(d.x, 0.25) && Close(d.y, 0.05));
    // the limit of the reported value: 0.55 is fine, a hair above is not
    CHECK(DecideScale(0.55f, 0.55f, 0.6f, none).action == ScaleAction::Scale);
    CHECK(DecideScale(-0.55f, -0.55f, 0.6f, none).action == ScaleAction::Scale);
    for (float bad : { 0.5500001f, -0.5500001f, 0.6f, -1.0f, 100.0f })
    {
        const auto x = DecideScale(bad, 0.1f, 0.6f, none);
        CHECK(x.action == ScaleAction::NotUsable && x.guard == Guard::ReportedOutOfRange);
        const auto y = DecideScale(0.1f, bad, 0.6f, none);
        CHECK(y.action == ScaleAction::NotUsable && y.guard == Guard::ReportedOutOfRange);
    }
    // missing or not a number: not usable, for the "unavailable" reason
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    CHECK(DecideScale(std::nullopt, 0.1f, 0.6f, none).guard == Guard::NgxJitterUnavailable);
    CHECK(DecideScale(0.1f, std::nullopt, 0.6f, none).guard == Guard::NgxJitterUnavailable);
    CHECK(DecideScale(std::nullopt, std::nullopt, 0.6f, none).action == ScaleAction::NotUsable);
    CHECK(DecideScale(nan, 0.1f, 0.6f, none).guard == Guard::NgxJitterUnavailable);
    CHECK(DecideScale(0.1f, nan, 0.6f, none).guard == Guard::NgxJitterUnavailable);
    CHECK(DecideScale(inf, 0.1f, 0.6f, none).guard == Guard::NgxJitterUnavailable);
    CHECK(DecideScale(0.1f, -inf, 0.6f, none).guard == Guard::NgxJitterUnavailable);
    // a pair that is not usable names no values to write
    CHECK(DecideScale(nan, 0.1f, 0.6f, none).x == 0.0f);
    // zero is a usable value, and scaled stays zero
    d = DecideScale(0.0f, 0.0f, 0.6f, none);
    CHECK(d.action == ScaleAction::Scale && d.x == 0.0f && d.y == 0.0f);

    // F4: the ratio is guarded inside DecideScale (finite, 0 < r <= 1): anything else writes nothing, so the 1 pixel cap below is a second
    // line of defence that no ratio can reach; it is tested on CapApplied itself
    for (float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 0.0f,
                       -0.0f, -0.5f, 1.0000001f, 2.0f, 3.0f })
    {
        const auto r = DecideScale(0.3f, -0.2f, bad, none);
        CHECK_MSG(r.action == ScaleAction::NotUsable && r.guard == Guard::ScaleRatioInvalid && r.x == 0.0f && r.y == 0.0f, "ratio %g", bad);
    }
    CHECK(DecideScale(0.3f, -0.2f, 1.0f, none).action == ScaleAction::Scale && DecideScale(0.3f, -0.2f, 0.25f, none).action == ScaleAction::Scale);
    CHECK(DecideScale(0.3f, -0.2f, 1e-30f, none).action == ScaleAction::Scale);
    CHECK(CapApplied(1.5f) == kMaxAppliedPixels && CapApplied(-1.5f) == -kMaxAppliedPixels && CapApplied(0.9f) == 0.9f && CapApplied(1.0f) == 1.0f);
    // the ratio is judged before the pair: a bad ratio with a bad pair names the ratio
    CHECK(DecideScale(std::nullopt, 0.1f, 3.0f, none).guard == Guard::ScaleRatioInvalid);

    // a pair the function never writes beyond the range, whatever it is given
    for (float ratio : { 0.25f, 0.5f, 0.9f, 0.979f })
        for (float v : { -0.55f, -0.3f, 0.0f, 0.3f, 0.55f })
        {
            const auto s = DecideScale(v, v, ratio, none);
            CHECK(s.action == ScaleAction::Scale && std::fabs(s.x) <= std::fabs(v) && std::fabs(s.x) <= kMaxAppliedPixels);
        }
}

void TestTheStaleBlockGuard()
{
    WrittenPair last;
    CHECK(!last.has());
    last.Remember(0.18f, -0.12f);
    CHECK(last.has() && last.Matches(0.18f, -0.12f));
    // a pair equal bit for bit on BOTH axes is already scaled: not scaled again
    auto d = DecideScale(0.18f, -0.12f, 0.6f, last);
    CHECK(d.action == ScaleAction::AlreadyScaled && d.guard == Guard::None);
    // one axis equal is not enough, in either direction; one ulp off is a new value
    CHECK(DecideScale(0.18f, -0.1f, 0.6f, last).action == ScaleAction::Scale);
    CHECK(DecideScale(0.3f, -0.12f, 0.6f, last).action == ScaleAction::Scale);
    CHECK(DecideScale(std::nextafter(0.18f, 1.0f), -0.12f, 0.6f, last).action == ScaleAction::Scale);
    CHECK(DecideScale(0.18f, std::nextafter(-0.12f, -1.0f), 0.6f, last).action == ScaleAction::Scale);
    // swapped axes are not the pair
    last.Remember(0.1f, 0.2f);
    CHECK(DecideScale(0.2f, 0.1f, 0.6f, last).action == ScaleAction::Scale);
    // +0 and -0 are different bits
    last.Remember(0.0f, 0.1f);
    CHECK(DecideScale(-0.0f, 0.1f, 0.6f, last).action == ScaleAction::Scale && DecideScale(0.0f, 0.1f, 0.6f, last).action == ScaleAction::AlreadyScaled);
    // a pair that is not usable is reported as such even when it equals the remembered one (usable check first, D5)
    last.Remember(0.7f, 0.1f);
    CHECK(DecideScale(0.7f, 0.1f, 0.6f, last).action == ScaleAction::NotUsable);
    // Forget
    last.Remember(0.18f, -0.12f);
    last.Forget();
    CHECK(!last.has() && !last.Matches(0.18f, -0.12f));
    CHECK(DecideScale(0.18f, -0.12f, 0.6f, last).action == ScaleAction::Scale);
}

// ---- the values from the logs of the in-game session -----------------------------------------------------------------------------

void TestTheValuesFromTheRealLogs()
{
    // Session 1 log (builds\session1-probe-log-20261019), as printed (4 decimals for jitter, 6 for ndc, so the ratios carry the rounding):
    //   counter 4,    reported jx/jy -0.3750/-0.0556, ndc -0.000117/0.000031, render 3840x2160 (DLAA)        -> rx/ry 0.600/0.600
    //   counter 3438, reported jx/jy -0.0364/-0.4473, ndc -0.000023/0.000497, render 1920x1080 (Performance) -> rx/ry 0.600/0.600
    // Recomputed here from the printed numbers with Analyse's own formula: 0.5990/0.6022 and 0.6066/0.6000 (X of the second line is
    // 1.1% off 0.600 because 0.000023 has two significant digits). Both are inside the 2% tolerance of 0.600 and the axes agree.
    const RenderSize dlaa { 3840, 2160, true, "Width/Height" }, perf { 1920, 1080, true, "Width/Height" };
    const RawObject a { -0.3750f, -0.0556f, -0.000117f, 0.000031f, 4 };
    const RawObject b { -0.0364f, -0.4473f, -0.000023f, 0.000497f, 3438 };
    const Analysis analysisA = Analyse(a, dlaa), analysisB = Analyse(b, perf);
    CHECK(analysisA.haveRx && analysisA.haveRy && analysisB.haveRx && analysisB.haveRy);
    CHECK(Close(analysisA.rx, 0.5990, 1e-3) && Close(analysisA.ry, 0.6022, 1e-3));
    CHECK(Close(analysisB.rx, 0.6066, 1e-3) && Close(analysisB.ry, 0.6000, 1e-3));
    for (const Analysis* an : { &analysisA, &analysisB })
    {
        CHECK(std::fabs(an->rx - 0.6f) <= kRatioTolerance * 0.6f && std::fabs(an->ry - 0.6f) <= kRatioTolerance * 0.6f);
        CHECK(!an->signMismatchX && !an->signMismatchY && an->verdict == Verdict::Differ);
        CHECK(std::fabs(an->ax) <= kMaxAppliedPixels && std::fabs(an->ay) <= kMaxAppliedPixels);
    }
    for (const Analysis* an : { &analysisA, &analysisB })
    {
        Calibrator c;
        Analysis next = *an;
        for (int i = 0; i < 5; ++i)
        {
            next.object.counter = an->object.counter + i;
            const Observation o = c.Observe(Guard::None, next, 1000u + i * 16u);
            CHECK(o.accepted && !o.axesDisagree);
        }
        CHECK(c.mode() == Mode::Scale);
        CHECK_MSG(std::fabs(c.ratio() - 0.6f) <= kRatioTolerance * 0.6f, "adopted %f", c.ratio());
    }
    // Session 2 log: reported (0.3281,0.1049) became (0.1969,0.0630) at ratio 0.600
    const auto d = DecideScale(0.3281f, 0.1049f, 0.6f, WrittenPair {});
    CHECK(Close(d.x, 0.1969, 1e-4) && Close(d.y, 0.0629, 1e-4));
}

Context HdContext()
{
    Context c;
    c.subrectW = 3840;
    c.subrectH = 2160;
    c.widthParam = 1920;
    c.heightParam = 1080;
    c.outW = 3840;
    c.outH = 2160;
    c.perfQuality = 0;
    c.forcedQuality = 7; // informational for this text: not part of it
    return c;
}

void TestTheText()
{
    // the key text: render size, output size, quality value; no hash
    CHECK(KeyText(HdContext()) == "render 1920x1080, output 3840x2160, quality 0");
    Context none;
    CHECK(KeyText(none) == "render n/a, output n/a, quality n/a");
    Context subrectOnly = HdContext();
    subrectOnly.widthParam = subrectOnly.heightParam = 0; // the subrect alone is not a trusted render size
    CHECK(KeyText(subrectOnly) == "render n/a, output 3840x2160, quality 0");
    const std::string key = "render 1920x1080, output 3840x2160, quality 0";

    CHECK(FormatLearningStarted(key) ==
          "Jitter correction: learning the game's jitter ratio for render 1920x1080, output 3840x2160, quality 0; the game's values pass through until 5 agreeing snapshots are seen");

    Feeder scale = Calibrated(0.6f);
    CHECK(FormatAdopted(scale.cal, key) ==
          "Jitter correction: calibrated ratio 0.600/0.600 from 5 snapshots for render 1920x1080, output 3840x2160, quality 0; scaling the reported jitter");
    Feeder axes;
    for (int i = 0; i < 5; ++i)
        axes.Feed(0.595f, 0.605f);
    CHECK(FormatAdopted(axes.cal, "K") == "Jitter correction: calibrated ratio 0.595/0.605 from 5 snapshots for K; scaling the reported jitter");
    Feeder neutral = Calibrated(1.0f);
    CHECK(FormatAdopted(neutral.cal, "K") ==
          "Jitter correction: the game already hands DLSS the jitter it applies (ratio 1.000/1.000 from 5 snapshots for K); nothing is applied");
    Feeder refused = Calibrated(1.5f);
    CHECK(FormatAdopted(refused.cal, "K") ==
          "Jitter correction: ratio 1.500/1.500 from 5 snapshots for K refused (a ratio above 1.02 or below 0.25 is not believed); nothing is applied, the game's values pass through");

    scale.FeedN(0.8f, 5);
    CHECK(FormatRatioChanged(0.6f, scale.cal, "K") ==
          "Jitter correction: ratio changed after 5 contradicting snapshots: was 0.600, now 0.800/0.800 for K; scaling the reported jitter");
    scale.FeedN(1.0f, 5);
    CHECK(FormatRatioChanged(0.8f, scale.cal, "K") ==
          "Jitter correction: ratio changed after 5 contradicting snapshots: was 0.800, now 1.000/1.000 for K; the game now hands DLSS what it applies, nothing is applied");
    scale.FeedN(0.1f, 5);
    CHECK(FormatRatioChanged(1.0f, scale.cal, "K") ==
          "Jitter correction: ratio changed after 5 contradicting snapshots: was 1.000, now 0.100/0.100 for K; ratio refused, nothing is applied");

    CHECK(FormatConfigurationChanged(key) ==
          "Jitter correction: configuration changed to render 1920x1080, output 3840x2160, quality 0; calibration dropped, learning again");
    CHECK(FormatVerificationStale() == "Jitter correction: verification stale, no usable jitter snapshot for 10 s; keeping the current state");
    CHECK(FormatSnapshotsAgain() == "Jitter correction: jitter snapshots are arriving again, verification is fresh");
    CHECK(FormatNotUsable(Guard::NgxJitterUnavailable) ==
          "Jitter correction: jitter pair not usable, game values passed through unchanged: NGX jitter parameters not readable");
    CHECK(FormatNotUsable(Guard::ReportedOutOfRange) ==
          "Jitter correction: jitter pair not usable, game values passed through unchanged: NGX jitter value above 0.55 pixel");
    CHECK(FormatFirstScaled(0.3f, -0.2f, 0.18f, -0.12f, 0.6f, 0.18f, -0.12f) ==
          "Jitter correction: first scaled pair of this calibration: reported (0.3000,-0.2000) scaled to (0.1800,-0.1200) by ratio 0.600; parameter block read back: (0.1800,-0.1200)");
    CHECK(Has(FormatFirstScaled(0.3f, -0.2f, 0.18f, -0.12f, 0.6f, 0.3f, -0.2f), "read back: (0.3000,-0.2000)"));
    CHECK(Has(FormatFirstScaled(0.3f, -0.2f, 0.18f, -0.12f, 0.6f, std::nullopt, -0.2f), "read back: unavailable"));
    CHECK(Has(FormatFirstScaled(0.3f, -0.2f, 0.18f, -0.12f, 0.6f, 0.3f, std::nullopt), "read back: unavailable"));

    // the state field: every state, with the key
    Calibrator learning;
    CHECK(StateText(learning, "K") == "learning for K");
    learning.UseKey(1);
    learning.UseKey(2);
    CHECK(StateText(learning, "K") == "dropped after a configuration change, learning again for K");
    CHECK(StateText(Calibrated(0.6f).cal, "K") == "calibrated ratio 0.600 for K");
    CHECK(StateText(Calibrated(1.0f).cal, "K") == "consistent (ratio 1.000), nothing applied, for K");
    CHECK(StateText(Calibrated(2.0f).cal, "K") == "refused (ratio 2.000), nothing applied, for K");

    // the status line
    CorrectionCounts counts;
    counts.evaluated = 21;
    counts.scaled = 10;
    counts.learning = 4;
    counts.notUsable = 3;
    counts.alreadyScaled = 2;
    counts.idle = 1;
    Calibrator::Counts snapshots;
    snapshots.accepted = 17;
    snapshots.disagreed = 5;
    snapshots.changes = 2;
    snapshots.restarts = 1;
    CHECK(FormatStatus(counts, "calibrated ratio 0.600 for K", snapshots, false, true, "Upscaler") ==
          "Jitter correction: on; totals since the start of the session: 21 evaluates seen, 10 scaled, 4 passed through while learning, 3 passed "
          "through because the jitter pair was not usable, 2 already scaled (block not refreshed), 1 left alone (nothing to apply); "
          "state: calibrated ratio 0.600 for K; snapshots: 17 accepted, 5 disagreed with the ratio, 2 ratio changes, 1 restarts after a "
          "configuration change; verification fresh; frame generation active yes (input Upscaler)");
    CHECK(Has(FormatStatus(counts, "K", snapshots, true, false, "NoFG"), "verification stale; frame generation active no (input NoFG)"));
    CHECK(Has(FormatStatus(counts, "K", snapshots, false, true, "DLSSG"), "frame generation active yes (input DLSSG)"));
    CHECK(Has(FormatStatus(counts, "K", snapshots, false, std::nullopt, ""), "frame generation active unknown") &&
          !Has(FormatStatus(counts, "K", snapshots, false, std::nullopt, ""), "(input"));

    CHECK(FormatSuppressed(1, 2, 3, 4, 5) ==
          "Game jitter: log line budget used up, episodes not logged so far: 1 probe failures, 2 pass-throughs, 3 replacements, 4 abandoned steps, 5 calibration events");
}

void TestTextOfTheNewGuards()
{
    CHECK(std::string(GuardName(Guard::JitterTooSmall)).find("too small") != std::string::npos);
    CHECK(std::string(GuardName(Guard::ReportedOutOfRange)).find("0.55") != std::string::npos);
    CHECK(std::string(GuardName(Guard::ScaleRatioInvalid)).find("ratio") != std::string::npos);
}
} // namespace

int main()
{
    TestClassEdges();
    TestAdoptionAtFiveNotAtFour();
    TestAdoptedRatioIsTheMeanOfTheRun();
    TestARunRestartsOnADisagreeingSnapshot();
    TestAxesThatDisagreeAreNotUsedForLearning();
    TestTheAxesToleranceIsRelativeToTheLargerAxis();
    TestOneOutlierIsIgnored();
    TestFourContradictionsDoNotReplaceFiveDo();
    TestContradictionsThatDisagreeWithEachOtherDoNotReplace();
    TestAxesDisagreeAgainstAnAdoptedRatio();
    TestNeutralAndRefused();
    TestKeyChangeDropsEverything();
    TestSnapshotsThatAreSkipped();
    TestCounterRebases();
    TestAbandonedRunsKeepTheAdoptedRatio();
    TestStaleClock();
    TestScaleArithmeticAndCap();
    TestTheStaleBlockGuard();
    TestTheValuesFromTheRealLogs();
    TestTextOfTheNewGuards();
    TestTheText();
    return TEST_RESULT("jitter_calibration");
}
