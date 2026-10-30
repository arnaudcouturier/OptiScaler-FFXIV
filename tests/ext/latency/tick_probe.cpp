// The probe's rules (ext/latency/TickProbeCore.h) with injected events: the histogram edges and percentile ranks, the signed split, the 10 s window
// and its restart rules, the frame-number pairing (one numbering for ticks and presents, no lag search, no guess), the two pairing rules (a present
// inside the tick on its thread, and the two-thread case the queue wait was built for), the metrics of one window (period, body, age, the present
// wait, the queue wait, slack, sleep->tick, the Reflex sleep's own duration, the share of presents that found their tick, the thread table and the
// pairing word) and the exact text of the one line the sink logs.
//
// The pairing scenarios are the independent reviewer's: ticks every 7 ms with a small jitter, a serial render pipeline two frames deep (a present ends
// 14 ms after the start of the tick whose frame number it carries), which the reviewer measured as an age of 13.9 ms and a queue wait W of +4.9 ms.
// The old index-based lag search reported 6.9 ms and -1.9 ms on the same stream; the frame numbers make the pair exact and the numbers survive a
// window boundary and arming in the middle of the pipeline.
//
// Buckets are 0.25 ms wide and a percentile is the midpoint of its bucket, so every expected value below is written as Bucket(us) rather than as the
// value that went in: a test that expected the raw input would hide a change of the bucket width.
#include "../ffxiv/chara/check.h"

#include "ext/latency/TickProbeCore.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace ext::latency;

namespace
{
// The midpoint of the bucket `us` lands in, which is what a percentile reports.
std::int64_t Bucket(std::int64_t us)
{
    const std::int64_t magnitude = us < 0 ? -us : us;
    const std::int64_t mid = (magnitude / kBucketUs) * kBucketUs + kBucketUs / 2;
    return us < 0 ? -mid : mid;
}

void TestPercentileRanks()
{
    CHECK(PercentileRank(0, 0.5) == 0);
    CHECK(PercentileRank(1, 0.99) == 0);
    CHECK(PercentileRank(2, 0.5) == 1); // round(0.5) = 1
    CHECK(PercentileRank(4, 0.05) == 0);
    CHECK(PercentileRank(21, 0.05) == 1); // round(1.0)
    CHECK(PercentileRank(10, 0.5) == 5);
    CHECK(PercentileRank(10, 0.95) == 9);
    CHECK(PercentileRank(10, 1.0) == 9);
    CHECK(PercentileRank(10, 0.0) == 0);
    CHECK(PercentileRank(100, 0.95) == 94);
}

void TestHistogram()
{
    Histogram h;
    CHECK(h.Empty() && h.Total() == 0 && h.ValueAt(0) == 0); // an empty histogram reports no value, never a bucket
    h.Add(0);
    h.Add(kBucketUs - 1); // same bucket as 0
    CHECK(h.Total() == 2 && h.ValueAt(0) == kBucketUs / 2 && h.ValueAt(1) == kBucketUs / 2);
    h.Add(kBucketUs); // the next bucket
    CHECK(h.ValueAt(2) == kBucketUs + kBucketUs / 2);
    // the overflow bucket: everything at or above 64 ms reads as its floor
    h.Add(kMaxReportedUs);
    h.Add(kMaxReportedUs * 10);
    CHECK(h.Total() == 5 && h.ValueAt(4) == kMaxReportedUs && h.ValueAt(3) == kMaxReportedUs);
    // a negative value cannot exist in a non-negative metric: it lands in the first bucket instead of underflowing the index
    h.Add(-5000);
    CHECK(h.ValueAt(2) == kBucketUs / 2); // ranks 0..2 are bucket 0, rank 3 the second bucket, ranks 4..5 the overflow bucket
    // CountIn works on bucket ranges
    h.Clear();
    for (std::int64_t us : { 100, 300, 700, 2000 })
        h.Add(us);
    CHECK(h.CountIn(0, 1000) == 3 && h.CountIn(0, 400) == 2 && h.CountIn(1000, 3000) == 1 && h.CountIn(5000, 6000) == 0);
}

void TestSignedHistogram()
{
    SignedHistogram h;
    h.Add(-3000);
    h.Add(-1000);
    h.Add(0);
    h.Add(2000);
    h.Add(4000);
    CHECK(h.Total() == 5);
    CHECK(h.ValueAt(0) == -Bucket(3000)); // ranks ascend: the most negative first
    CHECK(h.ValueAt(1) == -Bucket(1000));
    CHECK(h.ValueAt(2) == Bucket(0));
    CHECK(h.ValueAt(3) == Bucket(2000));
    CHECK(h.ValueAt(4) == Bucket(4000));
    CHECK(Percentile(h, 0.5) == Bucket(0));
    // CountIn across zero, and entirely on one side
    CHECK(h.CountIn(-3000, 4000) == 5);
    CHECK(h.CountIn(-2000, -500) == 1);
    CHECK(h.CountIn(1500, 5000) == 2);
    CHECK(h.CountIn(100'000, 200'000) == 0);
}

// A stream of frame-numbered events with the timestamps a game frame would produce. Every tick numbers the frame it simulates (`counter before` + 1)
// and every present carries the frame number of the tick whose frame it presents, which is the one thing a present cannot invent.
struct Stream
{
    struct Event
    {
        std::int64_t t;
        int kind; // 0 tick begin, 1 tick end, 2 present (begin and end at the same moment), 3 sleep end, 4 present with a span
        std::uint32_t frame;
        bool counter;
        std::int64_t body;
        std::int64_t tEnd = 0; // kind 4: when the present ends
    };
    std::vector<Event> events;
    std::uint32_t nextFrame = 1; // the frame number of the next tick

    // One tick: Framecounter reads nextFrame - 1 before the original and nextFrame after it, and the tick lasts `body` microseconds.
    void Tick(std::int64_t begin, std::int64_t body = 2000, bool counter = true)
    {
        events.push_back({ begin, 0, nextFrame - 1, counter, body });
        events.push_back({ begin + body, 1, nextFrame, counter, 0 });
        ++nextFrame;
    }
    void Present(std::int64_t end, std::uint32_t frame, bool counter = true) { events.push_back({ end, 2, frame, counter, 0 }); }
    // A present that begins and ends apart, on the thread the stream's ticks run on: the FFXIV shape, the present inside the tick body.
    void PresentSpan(std::int64_t begin, std::int64_t end, std::uint32_t frame, bool counter = true)
    {
        events.push_back({ begin, 4, frame, counter, 0, end });
    }
    void Sleep(std::int64_t end) { events.push_back({ end, 3, 0, false, 0 }); }

    // Feeds every event oldest first (a stable sort keeps the insertion order of equal timestamps: a tick's end before a present at the same
    // moment) and returns the windows that closed, in order.
    std::vector<Summary> Run(Probe& probe, std::uint32_t tid = 0x1a3c)
    {
        std::vector<Event> sorted = events;
        std::stable_sort(sorted.begin(), sorted.end(), [](const Event& a, const Event& b) { return a.t < b.t; });
        std::vector<Summary> windows;
        for (const Event& e : sorted)
        {
            std::int64_t now = e.t;
            switch (e.kind)
            {
            case 0:
                probe.TickBegin(e.t, tid, e.frame, e.counter);
                break;
            case 1:
                probe.TickEnd(e.t, e.frame);
                break;
            case 2:
                probe.PresentBegin(e.t, tid + 1, e.frame, e.counter);
                probe.PresentEnd(e.t, e.frame);
                break;
            case 4:
                probe.PresentBegin(e.t, tid, e.frame, e.counter); // the tick's own thread
                probe.PresentEnd(e.tEnd, e.frame);
                now = e.tEnd;
                break;
            default:
                probe.SleepEnd(e.t);
                break;
            }
            Summary s;
            if (probe.Due(now, s))
                windows.push_back(s);
        }
        return windows;
    }
};

// The reviewer's stream: a tick every 7 ms (a small deterministic jitter that leaves the median gap at exactly 7 ms), a serial render pipeline two
// frames deep, so present M ends 14 ms after the start of tick M and carries M's frame number, 120 us inside the 13.9 ms bucket. The truth is age
// 13.88 ms and W = 7 - 2 - 0.12 = 4.88 ms; the bucket midpoints are 13.875 and 4.875, which the line prints as 13.9 and 4.9.
constexpr std::int64_t kPeriodUs = 7000;
constexpr std::int64_t kBodyUs = 2000;
constexpr std::int64_t kAgeUs = 14000;
constexpr std::int64_t kPresentSlackUs = 120;
constexpr std::int64_t kTrueAgeUs = kAgeUs - kPresentSlackUs;
constexpr std::int64_t kTrueWaitUs = kAgeUs - kPresentSlackUs - kPeriodUs - kBodyUs;
void FeedTwoFramePipeline(Stream& stream, std::int64_t durationUs)
{
    constexpr std::int64_t kJitter[5] = { 0, 120, 60, 0, 0 }; // median tick gap is exactly kPeriodUs
    for (std::int64_t n = 0, t = 0; t < durationUs; ++n, t += kPeriodUs)
    {
        const std::int64_t begin = t + kJitter[n % 5];
        const std::uint32_t frame = stream.nextFrame;
        stream.Tick(begin, kBodyUs);
        stream.Present(begin + kAgeUs - kPresentSlackUs, frame);
    }
}

// The two-frame pipeline gives the true numbers, and it keeps giving them in every window: a window close resets the histograms, never the numbering.
void TestTwoFramePipelineAcrossWindows()
{
    Stream stream;
    FeedTwoFramePipeline(stream, 40'000'000);
    Probe probe;
    const std::vector<Summary> windows = stream.Run(probe);
    if (windows.size() < 3)
        std::printf("FAIL: %zu windows closed over 40 s, expected at least 3\n", windows.size());
    CHECK(windows.size() >= 3);
    if (!windows.empty())
        std::printf("two-frame pipeline: %zu windows, window 0 age p50 %.1f ms W p50 %.1f ms paired %.1f%% counter %s\n", windows.size(),
                    static_cast<double>(windows[0].ageP50) / 1000.0, static_cast<double>(windows[0].waitP50) / 1000.0,
                    windows[0].pairedShare, CounterText(windows[0].counter));
    for (std::size_t i = 0; i < windows.size(); ++i)
    {
        const Summary& s = windows[i];
        if (!(s.hasAge && s.ageP50 == Bucket(kTrueAgeUs) && s.waitP50 == Bucket(kTrueWaitUs) && s.counter == CounterState::Ok &&
              s.paired == s.presents))
            std::printf("FAIL: window %zu: age p50 %lld, W p50 %lld, counter %s, paired %llu of %llu\n", i, (long long) s.ageP50,
                        (long long) s.waitP50, CounterText(s.counter), (unsigned long long) s.paired, (unsigned long long) s.presents);
        CHECK(s.hasAge && s.ageP50 == Bucket(kTrueAgeUs)); // the reviewer's true age, about 13.9 ms
        CHECK(s.ageP5 == Bucket(kTrueAgeUs) && s.ageP95 == Bucket(kTrueAgeUs));
        CHECK(s.hasWait && s.waitP50 == Bucket(kTrueWaitUs)); // the reviewer's true queue wait, about +4.9 ms
        CHECK(s.counter == CounterState::Ok);
        CHECK(s.hasPaired && s.paired == s.presents); // every present found its tick, including the one that straddles the boundary
        CHECK(s.pairedShare == 100.0);
        CHECK(s.hasPeriod && s.periodP50 == Bucket(7000));
        CHECK(s.hasBody && s.bodyP50 == Bucket(2000));
        // the render thread is not the tick's thread: the two-thread rule produces the number, and the inside-the-tick W has no sample
        CHECK(s.pairing == Pairing::Split && s.presentsInsideTick == 0);
        CHECK(s.pairedSplit == s.presents && s.pairedSame == 0);
        CHECK(!s.hasPresentWait);
    }
}

// The single-thread loop, which is the shape FFXIV runs: one thread, the bridge present inside the tick body, and nothing left over between the
// tick's end and the next tick's start. The present pairs with the tick it runs inside, age is the tick's start to the present's start and W is the
// present's own duration; the queue wait, which describes a finished simulation waiting for a render thread, has no sample and reads n/a.
constexpr std::int64_t kSinglePeriodUs = 15000;
constexpr std::int64_t kSingleBodyUs = 14500;
constexpr std::int64_t kPresentBeginOffsetUs = 4000;
constexpr std::int64_t kPresentDurationUs = 7000;
constexpr std::int64_t kSameThreadAgeUs = kPresentBeginOffsetUs;
void TestSingleThreadLoopAcrossWindows()
{
    Stream stream;
    for (std::int64_t n = 0, t = 0; t < 40'000'000; ++n, t += kSinglePeriodUs)
    {
        const std::uint32_t frame = stream.nextFrame;
        stream.Tick(t, kSingleBodyUs);
        stream.PresentSpan(t + kPresentBeginOffsetUs, t + kPresentBeginOffsetUs + kPresentDurationUs, frame);
    }
    Probe probe;
    const std::vector<Summary> windows = stream.Run(probe);
    CHECK(windows.size() >= 3);
    for (const Summary& s : windows)
    {
        if (!(s.pairing == Pairing::Same && s.paired == s.presents && s.hasPresentWait && !s.hasWait))
            std::printf("FAIL: same-thread window: pairing %s, paired %llu of %llu, W %lld, queue %lld\n", PairingText(s.pairing),
                        (unsigned long long) s.paired, (unsigned long long) s.presents, (long long) s.presentWaitP50, (long long) s.waitP50);
        CHECK(s.pairing == Pairing::Same && s.presentsInsideTick == s.presents);
        CHECK(s.pairedSame == s.presents && s.pairedSplit == 0);
        CHECK(s.pairedShare == 100.0 && s.counter == CounterState::Ok);
        CHECK(s.hasAge && s.ageP5 == Bucket(kSameThreadAgeUs) && s.ageP50 == Bucket(kSameThreadAgeUs) && s.ageP95 == Bucket(kSameThreadAgeUs));
        CHECK(s.hasPresentWait && s.presentWaitP50 == Bucket(kPresentDurationUs) && s.presentWaitP95 == Bucket(kPresentDurationUs));
        CHECK(!s.hasWait); // no finished simulation waited for a render thread
        CHECK(s.hasPeriod && s.periodP50 == Bucket(kSinglePeriodUs));
        CHECK(s.hasBody && s.bodyP50 == Bucket(kSingleBodyUs));
        const std::string line = FormatSummary(s);
        CHECK(line.find("W p50=7.1 p95=7.1") != std::string::npos);
        CHECK(line.find("queue p50=n/a p95=n/a") != std::string::npos);
        CHECK(line.find("age p5=4.1 p50=4.1 p95=4.1") != std::string::npos);
        CHECK(line.find("threads tick=0x1a3c bridge=0x1a3c same") != std::string::npos);
    }
}

// A present inside a tick on the tick's own thread that carries another frame number pairs with nothing: it never falls back to an older finished
// tick, where the age would be a different frame's. The window still says it ran the same-thread rule and the paired share shows the failure.
void TestInsideTheTickNeedsItsFrame()
{
    Probe probe;
    probe.TickBegin(0, 0x10, 4, true); // frame 5
    probe.PresentBegin(1000, 0x10, 3, true); // same thread, inside the tick, carries frame 3
    probe.PresentEnd(4000, 3);
    Summary s = probe.Build();
    CHECK(s.presents == 1 && s.presentsInsideTick == 1 && s.pairing == Pairing::Same);
    CHECK(s.paired == 0 && s.pairedSame == 0 && s.pairedSplit == 0);
    CHECK(!s.hasAge && !s.hasPresentWait && !s.hasWait);

    // and the healthy control: the same span carrying the open tick's own number pairs
    Probe ok;
    ok.TickBegin(0, 0x10, 4, true);
    ok.PresentBegin(1000, 0x10, 5, true);
    ok.PresentEnd(4000, 5);
    s = ok.Build();
    CHECK(s.pairedSame == 1 && s.hasAge && s.ageP50 == Bucket(1000) && s.presentWaitP50 == Bucket(3000));
}

// One window can hold both shapes (a provider switch mid-window): it says mixed, and each number is reported under its own name.
void TestMixedWindowReportsBothNumbers()
{
    Probe probe;
    probe.TickBegin(0, 0x10, 0, true); // frame 1, inside-the-tick present
    probe.PresentBegin(1000, 0x10, 1, true);
    probe.PresentEnd(5000, 1);
    probe.TickEnd(6000, 1);
    probe.TickBegin(15000, 0x10, 1, true); // frame 2, present on another thread after the tick ended
    probe.TickEnd(17000, 2);
    probe.PresentBegin(20000, 0x20, 2, true);
    probe.PresentEnd(21000, 2);
    const Summary s = probe.Build();
    CHECK(s.presents == 2 && s.presentsInsideTick == 1 && s.pairing == Pairing::Mixed);
    CHECK(s.pairedSame == 1 && s.pairedSplit == 1 && s.paired == 2);
    CHECK(s.hasAge && s.hasPresentWait && s.hasWait);
    const std::string line = FormatSummary(s);
    CHECK(line.find("threads tick=0x10 bridge=0x10(2 threads) mixed") != std::string::npos);
}

// Arming in the middle of the pipeline: the probe starts recording after a tick has begun, so that tick was never recorded. Its present finds no
// tick and contributes nothing; the next tick and present pair normally, and a tick end without a recorded begin is dropped rather than half-recorded.
void TestArmMidPipeline()
{
    Probe probe;
    probe.PresentBegin(0, 0x10, 10, true); // carries frame 10, whose tick the probe never saw
    probe.PresentEnd(100, 10);
    probe.TickEnd(200, 11); // the end of a tick that began while the probe was not recording: dropped
    Summary s = probe.Build();
    CHECK(s.presents == 1 && s.paired == 0 && s.hasPaired && s.pairedShare == 0.0);
    CHECK(!s.hasAge && !s.hasWait);
    CHECK(!s.hasBody); // the lone end was not turned into a body sample

    probe.TickBegin(300, 0x1a3c, 11, true); // frame 12; the counter read 11 before the original
    probe.TickEnd(500, 12);
    probe.PresentBegin(600, 0x10, 12, true);
    probe.PresentEnd(700, 12);
    s = probe.Build();
    CHECK(s.ticks == 1 && s.presents == 2 && s.paired == 1);
    CHECK(s.hasBody && s.bodyP50 == Bucket(200));
    CHECK(s.hasAge && s.ageP50 == Bucket(400)); // present end 700 - tick begin 300
    CHECK(s.hasWait && s.waitP50 == -Bucket(400)); // present end 100 - tick end 500: the render thread was waiting for the simulation
}

// The counter never resolved: age and W read n/a, never a guess. Everything that does not need the counter still reports.
void TestMissingCounterIsNotGuessed()
{
    Stream stream;
    stream.Tick(0, 2000, false);
    stream.Tick(7000, 2000, false);
    stream.Present(14000, 0, false);
    stream.Present(21000, 0, false);
    Probe probe;
    stream.Run(probe);
    const Summary s = probe.Build();
    CHECK(s.ticks == 2 && s.presents == 2);
    CHECK(s.counter == CounterState::Missing);
    CHECK(!s.hasAge && !s.hasWait && !s.hasPaired);
    CHECK(s.hasBody); // the body does not need the counter
    CHECK(s.hasPeriod);
    const std::string line = FormatSummary(s);
    CHECK(line.find("age p5=n/a p50=n/a p95=n/a") != std::string::npos);
    CHECK(line.find("W p50=n/a p95=n/a") != std::string::npos);
    CHECK(line.find("paired n/a") != std::string::npos);
    CHECK(line.find("counter missing") != std::string::npos);
}

// The counter resolved but does not advance by exactly one per tick: the numbers would be guesses, so age, W and the paired share read n/a.
void TestInconsistentCounterIsNotGuessed()
{
    // a tick whose counter moved by two inside one tick
    Probe inside;
    inside.TickBegin(0, 1, 5, true);
    inside.TickEnd(2000, 7); // expected 6
    inside.TickBegin(7000, 1, 7, true);
    inside.TickEnd(9000, 8);
    inside.PresentBegin(14000, 2, 8, true);
    inside.PresentEnd(14000, 8);
    Summary s = inside.Build();
    CHECK(s.counter == CounterState::Inconsistent);
    CHECK(!s.hasAge && !s.hasWait && !s.hasPaired);
    CHECK(s.hasBody); // the ticks still measured
    CHECK(FormatSummary(s).find("counter inconsistent") != std::string::npos);

    // two ticks with a gap between their counter values
    Probe gap;
    gap.TickBegin(0, 1, 1, true);
    gap.TickEnd(2000, 2);
    gap.TickBegin(7000, 1, 4, true); // the previous tick left the counter at 2
    gap.TickEnd(9000, 5);
    s = gap.Build();
    CHECK(s.counter == CounterState::Inconsistent);
    CHECK(!s.hasAge && !s.hasWait);

    // consecutive ticks and a valid present: the healthy control for the two cases above
    Probe ok;
    ok.TickBegin(0, 1, 1, true);
    ok.TickEnd(2000, 2);
    ok.TickBegin(7000, 1, 2, true);
    ok.TickEnd(9000, 3);
    ok.PresentBegin(14000, 2, 3, true);
    ok.PresentEnd(14000, 3);
    s = ok.Build();
    CHECK(s.counter == CounterState::Ok && s.paired == 1 && s.hasAge);

    // a present that pairs normally while the counter is inconsistent still keeps W out of the window: no number rests on a broken numbering
    Probe pairedButBroken;
    pairedButBroken.TickBegin(0, 1, 5, true);
    pairedButBroken.TickEnd(2000, 7); // expected 6
    pairedButBroken.TickBegin(7000, 1, 7, true); // frame 8
    pairedButBroken.PresentBegin(8000, 1, 8, true); // inside that tick, on its thread
    pairedButBroken.PresentEnd(12000, 8);
    pairedButBroken.TickEnd(13000, 8);
    s = pairedButBroken.Build();
    CHECK(s.counter == CounterState::Inconsistent);
    CHECK(!s.hasPresentWait && !s.hasAge); // the samples exist, the flag keeps them off the line
    CHECK(!s.hasPaired);
}

// A present pairs only with a tick the probe actually recorded and finished when it began outside a tick: a missing record (arming, an evicted ring,
// a frame the game never ticked) contributes nothing, and a tick still running cannot be the frame a present carries on another thread.
void TestPairingNeedsTheFinishedTick()
{
    Stream stream;
    stream.Tick(0, 2000); // frame 1
    stream.Present(14000, 1); // pairs
    stream.Present(21000, 1); // the same frame twice: both presents pair with the one tick (the ring keeps it)
    stream.Present(28000, 9); // no such tick
    Probe probe;
    stream.Run(probe);
    const Summary s = probe.Build();
    CHECK(s.ticks == 1 && s.presents == 3 && s.paired == 2);
    CHECK(s.hasPaired && s.pairedShare > 66.0 && s.pairedShare < 67.0);

    Probe running;
    running.TickBegin(0, 1, 4, true); // frame 5
    running.PresentBegin(1000, 2, 5, true); // another thread, carrying a tick that has not ended yet
    running.PresentEnd(1500, 5);
    const Summary r = running.Build();
    CHECK(r.presents == 1 && r.paired == 0 && !r.hasAge && !r.hasWait && !r.hasPresentWait);
    CHECK(r.pairing == Pairing::Split); // outside the tick, so the two-thread rule applies and simply has nothing to pair

    // a present on the tick's own thread but after the tick ended: outside the tick, so the two-thread rule applies to it as well
    Probe serial;
    serial.TickBegin(0, 0x10, 4, true); // frame 5
    serial.TickEnd(2000, 5);
    serial.PresentBegin(3000, 0x10, 5, true);
    serial.PresentEnd(6000, 5);
    const Summary ser = serial.Build();
    CHECK(ser.presentsInsideTick == 0 && ser.pairing == Pairing::Split);
    CHECK(ser.pairedSplit == 1 && ser.hasAge && ser.ageP50 == Bucket(6000));
    CHECK(!ser.hasPresentWait); // the inside-the-tick wait is not measured for a present that began outside the tick
}

// The window: the event 10 s or more after the window opened carries its summary, and the next one opens a new window.
void TestWindowRolloverAndBackwardsClock()
{
    Probe probe;
    Summary s;
    probe.TickBegin(0, 1, 0, true);
    CHECK(!probe.Due(0, s));
    probe.TickBegin(9'000'000, 1, 1, true);
    CHECK(!probe.Due(9'000'000, s)); // a sample 9 s into the window does not close it
    probe.TickBegin(10'000'000, 1, 2, true);
    CHECK(probe.Due(10'000'000, s));
    CHECK(s.ticks == 3); // the event that closes the window is part of it
    CHECK(!probe.Due(10'000'001, s)); // a fresh window is open
    CHECK(probe.Build().ticks == 0);

    // a clock that stepped back restarts the window instead of closing a wrong one
    Probe back;
    back.TickBegin(1'000'000, 1, 0, true);
    CHECK(!back.Due(1'000'000, s));
    back.TickBegin(500, 1, 1, true);
    CHECK(!back.Due(500, s));
    CHECK(back.Build().ticks == 0); // the event that showed the step restarts the window and is dropped with the old one
    back.TickBegin(1500, 1, 2, true);
    CHECK(!back.Due(1500, s));
    CHECK(back.Build().ticks == 1);
}

// A window with presents but no tick: nothing can pair, and the line says so instead of inventing a zero.
void TestNoTicks()
{
    Stream stream;
    for (std::int64_t end = 5000; end < 30 * 10000; end += 10000)
        stream.Present(end, 0);
    Probe probe;
    stream.Run(probe, 0x2b10);
    const Summary s = probe.Build();
    CHECK(s.ticks == 0 && s.presents > 0); // presents, no tick: the pairing has nothing to work with
    CHECK(!s.hasAge && !s.hasPeriod);
    CHECK(s.hasPaired && s.paired == 0 && s.pairedShare == 0.0); // a usable counter, and not one present found a tick
    CHECK(s.pairing == Pairing::None); // no tick, so no rule ran: the line must say none, not one of the two rules
    const std::string line = FormatSummary(s);
    CHECK(line.find("period p50=n/a p95=n/a") != std::string::npos);
    CHECK(line.find("paired 0.0%") != std::string::npos);
    CHECK(line.find("threads tick=none") != std::string::npos);
    CHECK(line.find("none | paired 0.0%") != std::string::npos);

    // a probe that recorded nothing at all is not a window that ran a rule either
    Probe untouched;
    CHECK(untouched.Build().pairing == Pairing::None);
}

// sleep->tick only uses a sleep that ended recently: the one before the tick, not the one before the loading screen.
void TestStaleSleep()
{
    Stream stale;
    stale.Sleep(0);
    stale.Tick(600'000, 1000); // 600 ms after the sleep: too old to describe this tick
    Probe a;
    stale.Run(a);
    CHECK(!a.Build().hasSleep);

    Stream fresh;
    fresh.Sleep(570'000);
    fresh.Tick(600'000, 1000); // 30 ms after the sleep ended
    Probe b;
    fresh.Run(b);
    const Summary s = b.Build();
    CHECK(s.hasSleep && s.sleepP50 == Bucket(30'000) && s.sleeps == 1);

    // a metric past the 64 ms the buckets cover reads as that floor, never as a wrapped number
    Stream huge;
    huge.Sleep(100'000);
    huge.Tick(300'000, 1000); // 200 ms after the sleep
    Probe c;
    huge.Run(c);
    const Summary h = c.Build();
    CHECK(h.hasSleep && h.sleepP50 == kMaxReportedUs);
}

// The thread table: one id per kind normally, and the count when a kind ran on more than one thread.
void TestThreadTable()
{
    ThreadSet set;
    CHECK(set.First() == 0 && set.count == 0);
    set.Add(0);
    CHECK(set.count == 0); // an unknown id says nothing
    set.Add(0x10);
    set.Add(0x10);
    CHECK(set.count == 1 && set.First() == 0x10);
    set.Add(0x20);
    CHECK(set.count == 2 && set.First() == 0x10);
    for (std::uint32_t i = 0; i < 10; ++i)
        set.Add(0x100 + i);
    CHECK(set.count == 2 + 10 && set.ids[0] == 0x10); // distinct ids are counted, the first four are kept
    CHECK(ThreadText(set) == "0x10(12 threads)");
    CHECK(ThreadText(ThreadSet {}) == "none");
}

// The line itself: the exact text, with n/a where a metric had no sample and one decimal on every number.
void TestLineText()
{
    Summary s;
    s.ticks = 1432;
    s.presents = 1440;
    s.paired = 1439;
    s.hasPaired = true;
    s.pairedShare = 100.0 * 1439.0 / 1440.0;
    s.pairing = Pairing::Same;
    s.hasPeriod = s.hasBody = s.hasAge = s.hasPresentWait = s.hasSlack = s.hasSleep = s.hasSleepDur = true;
    s.periodP50 = 7100;
    s.periodP95 = 7900;
    s.bodyP50 = 2400;
    s.ageP5 = 9600;
    s.ageP50 = 13800;
    s.ageP95 = 19200;
    s.presentWaitP50 = 6200;
    s.presentWaitP95 = 8000;
    s.slackP50 = 600;
    s.sleepP50 = 600;
    s.sleepDurP50 = kBucketUs / 2;
    s.sleepDurP95 = 2500;
    s.tickThreads.Add(0x6554);
    s.presentThreads.Add(0x6554);
    const std::string want = "Tick probe: ticks=1432 presents=1440 period p50=7.1 p95=7.9 body p50=2.4 | age p5=9.6 p50=13.8 p95=19.2 | W p50=6.2 "
                             "p95=8.0 | queue p50=n/a p95=n/a | slack p50=0.6 | sleep->tick p50=0.6 | sleep p50=0.1 p95=2.5 (DLSS-G) | threads "
                             "tick=0x6554 bridge=0x6554 same | paired 99.9% | counter ok";
    const std::string got = FormatSummary(s);
    if (got != want)
        std::printf("FAIL: the line is\n  %s\nexpected\n  %s\n", got.c_str(), want.c_str());
    CHECK(got == want);

    // the two-thread shape: the queue wait is the number, and the inside-the-tick wait reads n/a
    Summary split;
    split.pairing = Pairing::Split;
    split.hasWait = true;
    split.waitP50 = 4900;
    split.waitP95 = 5000;
    const std::string splitLine = FormatSummary(split);
    CHECK(splitLine.find("W p50=n/a p95=n/a") != std::string::npos);
    CHECK(splitLine.find("queue p50=4.9 p95=5.0") != std::string::npos);
    CHECK(splitLine.find("split |") != std::string::npos);
    CHECK(std::string(PairingText(Pairing::Mixed)) == "mixed" && std::string(PairingText(Pairing::None)) == "none");

    // nothing measured: every number that needs a sample reads n/a, so a zero cannot be mistaken for a measurement
    const std::string empty = FormatSummary(Summary {});
    CHECK(empty == "Tick probe: ticks=0 presents=0 period p50=n/a p95=n/a body p50=n/a | age p5=n/a p50=n/a p95=n/a | W p50=n/a p95=n/a | queue "
                   "p50=n/a p95=n/a | slack p50=n/a | sleep->tick p50=n/a | sleep p50=n/a p95=n/a (DLSS-G) | threads tick=none bridge=none none | "
                   "paired n/a | counter ok");

    // a counter that did not resolve keeps every pairing number out of the line, whatever samples the histograms hold
    Summary missing;
    missing.counter = CounterState::Missing;
    missing.presents = 10;
    missing.hasAge = false;
    missing.hasWait = false;
    missing.hasPresentWait = false;
    missing.hasPaired = false;
    const std::string noCounter = FormatSummary(missing);
    CHECK(noCounter.find("age p5=n/a p50=n/a p95=n/a") != std::string::npos);
    CHECK(noCounter.find("W p50=n/a p95=n/a") != std::string::npos);
    CHECK(noCounter.find("paired n/a") != std::string::npos);
    CHECK(noCounter.find("counter missing") != std::string::npos);
    CHECK(std::string(CounterText(CounterState::Inconsistent)) == "inconsistent");

    // a percentile inside the overflow bucket reads as its floor, never as a wrapped or negative number
    Summary big;
    big.hasPeriod = true;
    big.periodP50 = kMaxReportedUs;
    CHECK(FormatSummary(big).find("period p50=64.0") != std::string::npos);
}

// The Reflex sleep's own duration: measured from its begin and end taps, reported next to sleep->tick, and never invented when the probe armed in
// the middle of one.
void TestSleepDuration()
{
    Probe probe;
    probe.SleepBegin(1000);
    probe.SleepEnd(1500);
    Summary s = probe.Build();
    CHECK(s.sleeps == 1 && s.hasSleepDur && s.sleepDurP50 == Bucket(500) && s.sleepDurP95 == Bucket(500));
    const std::string line = FormatSummary(s);
    CHECK(line.find("sleep p50=0.6 p95=0.6 (DLSS-G)") != std::string::npos);

    // an end without a begin (armed mid-sleep) contributes no duration, and the sleep still ends for sleep->tick
    Probe mid;
    mid.SleepEnd(2000);
    s = mid.Build();
    CHECK(s.sleeps == 1 && !s.hasSleepDur);

    // a clock that stepped back inside the sleep is not a duration at all, rather than a negative one clipped to zero
    Probe back;
    back.SleepBegin(5000);
    back.SleepEnd(4000);
    CHECK(back.Build().sleeps == 1 && !back.Build().hasSleepDur);
}
} // namespace

int main()
{
    TestPercentileRanks();
    TestHistogram();
    TestSignedHistogram();
    TestTwoFramePipelineAcrossWindows();
    TestSingleThreadLoopAcrossWindows();
    TestInsideTheTickNeedsItsFrame();
    TestMixedWindowReportsBothNumbers();
    TestArmMidPipeline();
    TestMissingCounterIsNotGuessed();
    TestInconsistentCounterIsNotGuessed();
    TestPairingNeedsTheFinishedTick();
    TestWindowRolloverAndBackwardsClock();
    TestNoTicks();
    TestStaleSleep();
    TestSleepDuration();
    TestThreadTable();
    TestLineText();
    return TEST_RESULT("tick_probe");
}
