// Pure rules of the GPU stage timers (OptiScaler/ext/present/GpuTimerStats.h): the 4-of-16 sampling, the 10 s
// window with mean/p95/max, the per-slot pending book with its fenced recycle and first-writer frames, the frame-scoped
// effects begin (a stale span is reaped, a same-frame second begin keeps out), the
// "Bridge excl. NR" subtraction (clamp at 0, missing side reads empty), the log line text, the GpuTimers=auto
// resolution (with the real trace decision), and the one-warning latch. No D3D type, no clock of its own: times,
// frames and fence values are passed in.
#include "ext/present/GpuTimerStats.h"
#include "ext/present/FramePacingCore.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <vector>

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

using namespace ext::present;
using namespace ext::present::gputimers;

namespace
{
constexpr int64_t kSec = 1'000'000; // microseconds

void SlotSelection()
{
    // 4 of 16: frames 0-3 sample, 4-15 do not, then the cycle repeats exactly
    for (uint64_t f = 0; f < 16; ++f)
        CHECK(ShouldSample(f) == (f < 4));
    for (uint64_t f = 16; f < 32; ++f)
        CHECK(ShouldSample(f) == ((f % 16) < 4));
    unsigned sampled = 0;
    for (uint64_t f = 0; f < 16; ++f)
        sampled += ShouldSample(f) ? 1 : 0;
    CHECK(sampled == 4);
    CHECK(kTimerSlots == 16 && kSampledSlots == 4);
    // a wider mask (5 of 16) or a shorter cycle (4 of 8) would both change this count
    CHECK(!ShouldSample(4) && !ShouldSample(15) && ShouldSample(16) && !ShouldSample(23));
}

void P95Positions()
{
    CHECK(P95Index(0) == 0);
    CHECK(P95Index(1) == 0); // one sample: p95 is that sample
    CHECK(P95Index(2) == 1);
    CHECK(P95Index(19) == 18);
    CHECK(P95Index(20) == 18); // ceil(19.0) - 1
    CHECK(P95Index(100) == 94);
    CHECK(P95Index(1000) == 949);
    // the index always addresses a real sample (a rounding-down p95 would read past the end, a -1 underflow wraps)
    for (size_t n = 1; n <= 300; ++n)
        CHECK(P95Index(n) < n);
}

void Summaries()
{
    // empty: zeros, no division by zero and no NaN
    {
        const GpuSummary s = GpuWindow::Summarize({});
        CHECK(s.samples == 0);
        CHECK(s.meanMs == 0.0 && s.p95Ms == 0.0 && s.maxMs == 0.0);
        CHECK(!std::isnan(s.meanMs) && !std::isnan(s.p95Ms) && !std::isnan(s.maxMs));
    }
    // one sample: every statistic is that sample
    {
        const GpuSummary s = GpuWindow::Summarize({ 2.5 });
        CHECK(s.samples == 1);
        CHECK(s.meanMs == 2.5 && s.p95Ms == 2.5 && s.maxMs == 2.5);
    }
    // mean divides by the count (a missing division sums, a wrong one averages over windows)
    {
        const GpuSummary s = GpuWindow::Summarize({ 1.0, 2.0, 3.0, 4.0 });
        CHECK(s.samples == 4);
        CHECK(std::fabs(s.meanMs - 2.5) < 1e-9);
        CHECK(s.maxMs == 4.0);
        CHECK(s.p95Ms == 4.0); // index 3 of {1,2,3,4}
    }
    // p95 is order-independent (the sort) and reads slot 94 of 1..100
    {
        std::vector<double> v;
        for (int i = 100; i >= 1; --i)
            v.push_back(static_cast<double>(i));
        const GpuSummary s = GpuWindow::Summarize(v);
        CHECK(s.samples == 100);
        CHECK(std::fabs(s.meanMs - 50.5) < 1e-9);
        CHECK(s.p95Ms == 95.0);
        CHECK(s.maxMs == 100.0);
    }
    // the max keeps the larger value when a smaller sample follows
    CHECK(GpuWindow::Summarize({ 9.0, 1.0 }).maxMs == 9.0);
}

void WindowTiming()
{
    GpuSummary s;
    {
        GpuWindow w;
        // nothing closes before 10 s
        w.Add(0.5, 1000 * kSec);
        CHECK(!w.CloseIfDue(1000 * kSec + 9'999'999, s));
        // exactly 10 s: the window closes with every sample so far, then resets
        w.Add(0.6, 1000 * kSec + 9'999'999);
        w.Add(0.7, 1000 * kSec + 10'000'000);
        CHECK(w.CloseIfDue(1000 * kSec + 10'000'000, s));
        CHECK(s.samples == 3);
        CHECK(std::fabs(s.meanMs - 0.6) < 1e-9);
        CHECK(s.maxMs == 0.7);
        // after the summary the window is empty: the next samples open a new one
        CHECK(!w.CloseIfDue(1001 * kSec, s));
        w.Add(1.0, 1011 * kSec);
        CHECK(!w.CloseIfDue(1011 * kSec + 9'999'999, s));
        CHECK(w.CloseIfDue(1021 * kSec, s));
        CHECK(s.samples == 1 && s.meanMs == 1.0);
    }
    // the window length is the trace's 10 s
    CHECK(kGpuWindowUs == 10'000'000);
    // a clock that goes backwards restarts the window instead of spanning it; the sample is kept
    {
        GpuWindow b;
        b.Add(1.0, 5000 * kSec);
        b.Add(1.0, 4000 * kSec);
        b.Add(1.0, 4000 * kSec + 9'999'999);
        b.Add(1.0, 4010 * kSec);
        CHECK(b.CloseIfDue(4010 * kSec, s));
        CHECK(s.samples == 3);
    }
    // an idle thread still reports its window once, with zeros rather than old samples
    {
        GpuWindow e;
        CHECK(!e.CloseIfDue(999 * kSec, s)); // never opened: nothing to report
    }
    // switching the timers off forgets the window, so samples of the previous
    // on-period never report after the next on
    {
        GpuWindow w;
        w.Add(5.0, 1000 * kSec);
        w.Reset();
        w.Add(1.0, 2000 * kSec);
        CHECK(w.CloseIfDue(2000 * kSec + 10'000'000, s));
        CHECK(s.samples == 1 && s.meanMs == 1.0);
        // reset on an empty window is a no-op, not a report
        GpuWindow e;
        e.Reset();
        CHECK(!e.CloseIfDue(3000 * kSec + 10'000'000, s));
    }
}

void ExclNrClamp()
{
    // above: the difference; equal and below: timing noise, clamped at 0 (never negative)
    CHECK(BridgeExclNr(5.0, 3.0) == std::optional<double>(2.0));
    CHECK(BridgeExclNr(4.0, 4.0) == std::optional<double>(0.0));
    CHECK(BridgeExclNr(3.0, 5.0) == std::optional<double>(0.0));
    CHECK(BridgeExclNr(0.001, 99.0) == std::optional<double>(0.0));
    // a NaN input is not a number: it reads 0, never NaN
    {
        const auto r = BridgeExclNr(std::numeric_limits<double>::quiet_NaN(), 1.0);
        CHECK(r.has_value() && *r == 0.0 && !std::isnan(*r));
    }
}

void ExclNrMissing()
{
    const std::optional<double> none;
    // either side missing is not a measurement (not zero: zero would claim the bridge cost nothing)
    CHECK(!BridgeExclNr(none, 8.0).has_value());
    CHECK(!BridgeExclNr(9.0, none).has_value());
    CHECK(!BridgeExclNr(none, none).has_value());
    // both present, even zeros: a value (0.0 minus 0.0 is timing noise, still a value)
    CHECK(BridgeExclNr(0.0, 0.0) == std::optional<double>(0.0));
    CHECK(BridgeExclNr(9.0, 8.0) == std::optional<double>(1.0));
}

void StageNames()
{
    CHECK(std::string(GpuStageName(GpuStage::PresentCopyIn)) == "present copy-in");
    CHECK(std::string(GpuStageName(GpuStage::ReshadeEffects)) == "ReShade effects");
    CHECK(std::string(GpuStageName(GpuStage::InteropCopy)) == "interop copy");
    CHECK(static_cast<unsigned>(GpuStage::Count) == 3);
    CHECK(GpuStageName(GpuStage::Count)[0] == '?');
}

void LogLines()
{
    GpuSummary s;
    s.samples = 42;
    s.meanMs = 0.3124;
    s.p95Ms = 0.4015;
    s.maxMs = 0.5126;
    CHECK(FormatGpuLine("present copy-in", s) ==
          "Frame pacing GPU [present copy-in]: samples 42, mean 0.312 ms, p95 0.402 ms, max 0.513 ms");
    // an empty window still formats (zeros, not NaN)
    const GpuSummary e;
    CHECK(FormatGpuLine("interop copy", e) == "Frame pacing GPU [interop copy]: samples 0, mean 0.000 ms, p95 0.000 ms, max 0.000 ms");
    // every stage has a loggable name (the in-game proof greps them)
    for (unsigned i = 0; i < static_cast<unsigned>(GpuStage::Count); ++i)
    {
        const std::string line = FormatGpuLine(GpuStageName(static_cast<GpuStage>(i)), e);
        CHECK(line.rfind("Frame pacing GPU [", 0) == 0 && line.find("]: samples 0,") != std::string::npos);
    }
}

void KeyResolution()
{
    using Opt = std::optional<bool>;
    // explicit true/false win over the trace either way
    CHECK(ResolveGpuTimers(Opt { true }, false));
    CHECK(ResolveGpuTimers(Opt { true }, true));
    CHECK(!ResolveGpuTimers(Opt { false }, true));
    CHECK(!ResolveGpuTimers(Opt { false }, false));
    // auto (unset): on only together with the trace
    CHECK(ResolveGpuTimers(Opt {}, true));
    CHECK(!ResolveGpuTimers(Opt {}, false));
    // end to end with the real trace decision: file logging at Debug turns the trace on (so auto timers run),
    // at the normal level it stays off (so they do not)
    CHECK(Decide(Opt {}, true, 0, true).on && ResolveGpuTimers(Opt {}, Decide(Opt {}, true, 0, true).on));
    CHECK(Decide(Opt {}, true, 1, true).on && ResolveGpuTimers(Opt {}, Decide(Opt {}, true, 1, true).on));
    CHECK(!Decide(Opt {}, true, 2, true).on && !ResolveGpuTimers(Opt {}, Decide(Opt {}, true, 2, true).on));
    CHECK(!Decide(Opt {}, false, 0, true).on && !ResolveGpuTimers(Opt {}, Decide(Opt {}, false, 0, true).on));
    // an explicit Trace=false keeps auto timers off even at Debug level; Trace=true turns them on
    CHECK(!ResolveGpuTimers(Opt {}, Decide(Opt { false }, true, 0, true).on));
    CHECK(ResolveGpuTimers(Opt {}, Decide(Opt { true }, true, 2, true).on));
    // outside FFXIV the trace is off whatever the key says, so auto timers stay off there too
    CHECK(!ResolveGpuTimers(Opt {}, Decide(Opt {}, true, 0, false).on));
}

void OneWarningLatch()
{
    WarnLatch latch;
    CHECK(latch.Claim()); // the creation failure logs once ...
    for (int i = 0; i < 100; ++i)
        CHECK(!latch.Claim()); // ... and never again
    WarnLatch other;
    CHECK(other.Claim()); // one latch per timer: another timer still gets its own line
    // a new device restarts the timer, so it gets its own warning again
    latch.Reset();
    CHECK(latch.Claim());
    CHECK(!latch.Claim());
}

void SlotBookBasics()
{
    // one book slot per timer slot of the 16-frame cycle
    CHECK(SlotBook::kSlots == 16 && SlotBook::kSlots == kTimerSlots);
    SlotBook book;
    for (unsigned s = 0; s < SlotBook::kSlots; ++s)
    {
        CHECK(!book.Pending(s));
        CHECK(!book.Ready(s, 100, 100));
        CHECK(!book.Consume(s));
        CHECK(book.TryBegin(s, 7, 100, 100, true) == SlotBook::Verdict::Open);
        CHECK(book.TryBegin(s, 7, 100, 100, false) == SlotBook::Verdict::Open);
    }
    // out of range is inert, never a crash and never a report
    CHECK(!book.Pending(SlotBook::kSlots));
    CHECK(!book.Ready(SlotBook::kSlots, 100, 100));
    CHECK(!book.Consume(SlotBook::kSlots));
    CHECK(book.TryBegin(SlotBook::kSlots, 7, 100, 100, true) == SlotBook::Verdict::KeepOut);
    book.Arm(SlotBook::kSlots, 7, 0, 0);
    CHECK(!book.Pending(SlotBook::kSlots));
    // slots are independent: arming one leaves the others free
    book.Arm(3, 40, 10, 2);
    CHECK(book.Pending(3));
    CHECK(!book.Pending(2) && !book.Pending(4));
    CHECK(book.TryBegin(2, 40, 11, 11, true) == SlotBook::Verdict::Open);
    CHECK(book.Tag(3) == 2);
}

void SlotBookExactlyOnce()
{
    SlotBook book;
    // the recorded fence value is the one already covering the list: only a
    // later fence value proves the pair finished (PreSrTiming recycle rule)
    book.Arm(1, 40, 10, 0);
    CHECK(!book.Ready(1, 10, 10)); // nothing new submitted since the recording
    CHECK(!book.Ready(1, 9, 11)); // a newer covering value whose execution has not retired
    CHECK(book.Ready(1, 11, 11)); // the covering execution retired: report once ...
    CHECK(book.Consume(1));
    CHECK(!book.Ready(1, 11, 11)); // ... and never again without a new recording
    CHECK(!book.Consume(1));
    // equality on the covering side alone is not retirement either
    book.Arm(1, 41, 11, 0);
    CHECK(!book.Ready(1, 11, 12));
    CHECK(book.Ready(1, 12, 12));
    CHECK(book.Consume(1));
}

void SlotBookNeverCompletes()
{
    SlotBook book;
    book.Arm(5, 40, 10, 1);
    // the GPU work never finishes: no fence value ever reports it, however often asked
    for (uint64_t completed = 0; completed <= 10; ++completed)
        CHECK(!book.Ready(5, completed, 11));
    CHECK(book.Consume(5)); // draining without reporting still clears the slot
    CHECK(!book.Pending(5));
}

void SlotBookRecycle()
{
    SlotBook book;
    // a full cycle later the slot is reused: when the old sample finished it is
    // reported first, otherwise it is forgotten (its resolve never retired)
    book.Arm(2, 40, 10, 0);
    CHECK(book.TryBegin(2, 56, 11, 11, true) == SlotBook::Verdict::Recycle);
    CHECK(book.Consume(2)); // the wiring reports the old sample between the verdict and the reuse
    book.Arm(2, 56, 11, 0);
    CHECK(book.Pending(2));

    book.Arm(3, 40, 10, 0);
    CHECK(book.TryBegin(3, 56, 10, 10, true) == SlotBook::Verdict::Drop);
    book.Arm(3, 56, 12, 1); // the old recording is gone: only the new fence value can report it
    CHECK(!book.Ready(3, 12, 11));
    CHECK(book.Tag(3) == 1);
    CHECK(!book.Ready(3, 12, 12)); // the covering value must be exceeded, not merely reached
    CHECK(!book.Ready(3, 13, 12)); // completed 13 means nothing while the covering value is still 12
    CHECK(book.Ready(3, 13, 13));
}

void SlotBookCancelAndClear()
{
    SlotBook book;
    // the failed-conversion path: the list is closed and never executed, so the
    // slot is forgotten instead of reported
    book.Arm(0, 40, 10, 0);
    book.Cancel(0);
    CHECK(!book.Pending(0));
    CHECK(!book.Ready(0, 11, 11));
    CHECK(!book.Consume(0));
    CHECK(book.TryBegin(0, 41, 11, 11, true) == SlotBook::Verdict::Open);
    // cancelling an idle slot is a no-op
    book.Cancel(7);
    CHECK(book.TryBegin(7, 41, 11, 11, true) == SlotBook::Verdict::Open);
    // switching the timers off forgets every slot: nothing of the previous
    // on-period is reported after the next on
    book.Arm(0, 40, 10, 0);
    book.Arm(4, 40, 9, 1);
    book.Clear();
    for (unsigned s = 0; s < SlotBook::kSlots; ++s)
    {
        CHECK(!book.Pending(s));
        CHECK(!book.Ready(s, 100, 100));
        CHECK(!book.Consume(s));
    }
}

void SlotBookFirstWriterWins()
{
    SlotBook book;
    // unfenced pairs (the D3D11 effects span): the first runtime of the frame
    // wins; a second runtime in the same frame keeps out instead of discarding it
    CHECK(book.TryBegin(1, 40, 0, 0, false) == SlotBook::Verdict::Open);
    book.Arm(1, 40, 0, 0);
    CHECK(book.TryBegin(1, 40, 0, 0, false) == SlotBook::Verdict::KeepOut);
    CHECK(book.Pending(1)); // the first sample is still there, untouched
    CHECK(book.Consume(1)); // the Present read reports it ...
    CHECK(book.TryBegin(1, 40, 0, 0, false) == SlotBook::Verdict::Open); // ... then the slot is free
    // a later frame reuses the slot even when the old sample never completed
    book.Arm(1, 40, 0, 0);
    CHECK(book.TryBegin(1, 41, 0, 0, false) == SlotBook::Verdict::Drop);
    book.Arm(1, 41, 0, 0);
    CHECK(book.Pending(1));
}

void EffectsStaleSpan()
{
    // no span open: the begin proceeds and records
    CHECK(DecideEffectsBegin(false, 0, 40) == EffectsBegin::Proceed);
    // a span of this frame is already open: the begin keeps out, so the first runtime wins and its pair stays intact
    CHECK(DecideEffectsBegin(true, 40, 40) == EffectsBegin::KeepOut);
    // a span of an earlier frame is still open (its finish never ran): the begin reaps the stale span first and
    // then proceeds, so one missed finish never blocks later frames
    CHECK(DecideEffectsBegin(true, 40, 41) == EffectsBegin::ReapThenProceed);
    CHECK(DecideEffectsBegin(true, 40, 56) == EffectsBegin::ReapThenProceed);
    // the verdict follows the open span, not the call order: an older call never outranks a newer frame
    CHECK(DecideEffectsBegin(true, 41, 41) == EffectsBegin::KeepOut);
}
} // namespace

int main()
{
    SlotSelection();
    P95Positions();
    Summaries();
    WindowTiming();
    ExclNrClamp();
    ExclNrMissing();
    StageNames();
    LogLines();
    KeyResolution();
    OneWarningLatch();
    SlotBookBasics();
    SlotBookExactlyOnce();
    SlotBookNeverCompletes();
    SlotBookRecycle();
    SlotBookCancelAndClear();
    SlotBookFirstWriterWins();
    EffectsStaleSpan();
    if (g_failed != 0)
    {
        std::printf("gpu_timer_stats: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("gpu_timer_stats: all passed\n");
    return 0;
}
