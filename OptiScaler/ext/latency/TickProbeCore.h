#pragma once
// Pure rules of the simulation-tick probe: the fixed histograms, the 10 s window, the frame-number pairing, the numbers one window reports and the
// text of its log line. No clock, no thread, no logger and no allocation outside the line itself, so every rule here is unit-tested
// (tests/ext/latency/tick_probe.cpp). The clock, the threads and the key are in ext/latency/TickProbe.cpp.
//
// All times are int64 MICROSECONDS (the sink converts QueryPerformanceCounter once, see QpcMicros in TickProbe.cpp). What the numbers mean:
//
//   period       tickBegin(N+1) - tickBegin(N)              the tick-to-tick distance
//   body         tickEnd(N) - tickBegin(N)                  how long one tick took
//   age (inside) presentBegin(M) - tickBegin(N)             tick start to the start of the bridge present that runs inside tick N
//   age (outside) presentEnd(M) - tickBegin(N)              tick start to the end of the bridge present that carries frame N
//   W (inside)   presentEnd(M) - presentBegin(M)            the game thread's time inside the native present of tick N
//   queue        presentEnd(M-1) - tickEnd(N)               how long a finished tick waited for the previous present to end (outside only)
//   slack        tickBegin(N) - the last completed present end before it
//   sleep->tick  tickBegin(N) - the last Reflex sleep end before it
//   sleep        sleepEnd - sleepBegin                      how long the Reflex sleep itself lasted
//
// PAIRING. Ticks and presents are numbered by ONE source: the game's own frame counter (Framework.Framecounter, which the first instruction of
// Tick increments). The observer reads it before and after the original tick and hands both values in; a tick's frame number is the value the
// counter holds after the tick ran, and its two reads must differ by exactly one. The present taps read the same counter when the present begins
// and when it ends; a present carries the number it saw when it began, and the probe pairs it with the tick of that number. There is no lag search
// and no guessing: a present whose number has no recorded tick contributes nothing, and a window in which the counter did not resolve or did not
// advance by exactly one per tick reports age and W as n/a with `counter missing` or `counter inconsistent` on the line.
//
// TWO RULES, ONE PER SHAPE OF THE LOOP. When a present begins inside a tick that is still open on the present's own thread and carries that tick's
// frame number, the present runs inside the tick: FFXIV's main loop is one thread, and its bridge present is a piece of the tick body. That present
// pairs with the open tick, age is the tick's start to the present's start, and W is the present's own duration - the time the game thread spends
// inside the native present, the wait a sleep at tick start would move to before the input sampling. A present that begins anywhere else (on
// another thread, or after the tick ended) pairs with the finished tick whose frame number it carries, exactly as before: age is the tick's start
// to the present's end and `queue` is the old queue wait. A present that began inside an open tick never falls back to the other rule: if it does
// not carry that tick's number it contributes nothing, so a broken numbering shows as a low paired share instead of a wrong age.
//
// A window boundary never restarts the numbering: the tick ring, the in-flight tick and present and the last-event timestamps survive a window
// close, so a frame that straddles the boundary pairs the same way on either side. Only the histograms and the summary are cleared.
//
// W is the number a tick-side sleep could give back, and what it holds follows the rule that paired the present. Inside the tick it is the game
// thread's own time in the native present, an upper bound: that time also contains the bridge's copy and submit, which the probe cannot tell apart
// from a wait. Outside the tick it is the queue wait of a finished simulation: positive means the render thread was behind, negative means it was
// waiting for the simulation. age, queue, slack and sleep->tick are signed on purpose; a median below zero is a result, not a broken sample. W is a
// duration and is never negative.
//
// Every metric keeps 0.25 ms buckets (256 of them, 0..64 ms) plus an overflow bucket. A percentile is the midpoint of the bucket it lands in, so it
// is accurate to +-0.125 ms; a percentile inside the overflow bucket reads as the 64 ms floor, which is also the largest number this line can show.
// The line says "n/a" instead of a number only when the metric had no sample at all, or when the frame counter cannot carry the pairing.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ext::latency
{
inline constexpr std::int64_t kBucketUs = 250;         // 0.25 ms per bucket
inline constexpr std::size_t kBuckets = 256;           // 0..64 ms, plus one overflow bucket
inline constexpr std::int64_t kWindowUs = 10'000'000;  // 10 s, the same window rule as the frame-pacing trace
inline constexpr std::int64_t kStaleSleepUs = 500'000; // a sleep that ended longer ago than this does not describe this tick
inline constexpr std::size_t kTickRing = 8;            // ticks kept for pairing: enough for a pipeline of several frames and a window boundary
inline constexpr std::size_t kMaxThreads = 4;          // distinct thread ids remembered per event kind
inline constexpr std::int64_t kMaxReportedUs = kBucketUs * static_cast<std::int64_t>(kBuckets); // the overflow bucket's floor

// The pairing failure the window reports. `Ok` is the only state in which age and W may be printed.
enum class CounterState : std::uint8_t
{
    Ok,           // the counter resolved and advanced by exactly one per tick
    Missing,      // the build did not resolve Framecounter: nothing can be paired
    Inconsistent, // the counter did not advance by exactly one per tick: the numbers would be guesses
};

inline const char* CounterText(CounterState state)
{
    switch (state)
    {
    case CounterState::Ok:
        return "ok";
    case CounterState::Missing:
        return "missing";
    case CounterState::Inconsistent:
        return "inconsistent";
    }
    return "?";
}

// Which of the two pairing rules a window's presents ran under: `Same` when they began inside an open tick on its thread (FFXIV's main loop),
// `Split` when they began on another thread or after the tick ended, `Mixed` when both happened in one window, `None` when the window has no
// present or no tick to describe. The word is on the line next to the thread table, so a number is never read without the rule that produced it.
enum class Pairing : std::uint8_t
{
    None,
    Same,
    Split,
    Mixed,
};

inline const char* PairingText(Pairing pairing)
{
    switch (pairing)
    {
    case Pairing::None:
        return "none";
    case Pairing::Same:
        return "same";
    case Pairing::Split:
        return "split";
    case Pairing::Mixed:
        return "mixed";
    }
    return "?";
}

// A non-negative metric: 0.25 ms buckets over [0, 64 ms) plus one overflow bucket. Free of allocation, and of floating point except in a
// percentile.
class Histogram
{
  public:
    void Add(std::int64_t us)
    {
        if (us < 0)
            us = 0; // a non-negative metric cannot be below zero: a clock that stepped back reads as the first bucket
        const std::int64_t index = us / kBucketUs;
        if (index >= static_cast<std::int64_t>(kBuckets))
            ++_overflow;
        else
            ++_counts[static_cast<std::size_t>(index)];
        ++_total;
    }

    void Clear()
    {
        _counts.fill(0);
        _overflow = 0;
        _total = 0;
    }

    std::uint64_t Total() const { return _total; }
    bool Empty() const { return _total == 0; }

    // The value at 0-based rank `rank` (clamped): the midpoint of its bucket, kMaxReportedUs inside the overflow bucket, and 0 for a histogram
    // with no sample at all (the summary's flags are what keep a missing metric out of the line, not this zero).
    std::int64_t ValueAt(std::uint64_t rank) const
    {
        if (_total == 0)
            return 0;
        if (rank >= _total)
            rank = _total - 1;
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i)
        {
            seen += _counts[i];
            if (rank < seen)
                return static_cast<std::int64_t>(i) * kBucketUs + kBucketUs / 2;
        }
        return kMaxReportedUs;
    }

    // Samples in [lo, hi] (both inclusive, microseconds). Used by the tests for the histogram's own edges; the pairing no longer needs a band.
    std::uint64_t CountIn(std::int64_t lo, std::int64_t hi) const
    {
        if (lo < 0)
            lo = 0;
        std::uint64_t n = 0;
        for (std::size_t i = 0; i < kBuckets; ++i)
        {
            const std::int64_t from = static_cast<std::int64_t>(i) * kBucketUs, to = from + kBucketUs - 1;
            if (to >= lo && from <= hi)
                n += _counts[i];
        }
        if (kMaxReportedUs >= lo && hi >= kMaxReportedUs)
            n += _overflow;
        return n;
    }

  private:
    std::array<std::uint32_t, kBuckets> _counts {};
    std::uint32_t _overflow = 0;
    std::uint64_t _total = 0;
};

// A signed metric: the negative samples in a histogram of their magnitude and the non-negative ones in a second histogram. The percentile keeps
// its sign, which is the whole point for slack and sleep->tick.
class SignedHistogram
{
  public:
    void Add(std::int64_t us)
    {
        if (us < 0)
            _below.Add(-us);
        else
            _atLeastZero.Add(us);
    }
    void Clear()
    {
        _below.Clear();
        _atLeastZero.Clear();
    }
    std::uint64_t Total() const { return _below.Total() + _atLeastZero.Total(); }
    bool Empty() const { return Total() == 0; }

    std::int64_t ValueAt(std::uint64_t rank) const
    {
        const std::uint64_t negative = _below.Total();
        if (rank < negative)
            return -_below.ValueAt(negative - 1 - rank); // ranks ascend: the largest magnitude is the smallest value
        return _atLeastZero.ValueAt(rank - negative);
    }

    // Samples whose value lies in [lo, hi].
    std::uint64_t CountIn(std::int64_t lo, std::int64_t hi) const
    {
        std::uint64_t n = 0;
        if (hi < 0)
            return _below.CountIn(-hi, -lo);
        if (lo >= 0)
            return _atLeastZero.CountIn(lo, hi);
        n += _below.CountIn(1, -lo);
        n += _atLeastZero.CountIn(0, hi);
        return n;
    }

  private:
    Histogram _below;
    Histogram _atLeastZero;
};

// The nearest-rank percentile of `q` in [0, 1]: rank = round(q * (n - 1)), clamped.
inline std::uint64_t PercentileRank(std::uint64_t n, double q)
{
    if (n == 0)
        return 0;
    if (q <= 0.0)
        return 0;
    if (q >= 1.0)
        return n - 1;
    const double rank = q * static_cast<double>(n - 1);
    const auto r = static_cast<std::uint64_t>(rank + 0.5);
    return r >= n ? n - 1 : r;
}

inline std::int64_t Percentile(const Histogram& h, double q) { return h.ValueAt(PercentileRank(h.Total(), q)); }
inline std::int64_t Percentile(const SignedHistogram& h, double q) { return h.ValueAt(PercentileRank(h.Total(), q)); }

// One thread kind as the line reports it: the first id seen and how many distinct ids there were (the thread table answers "which thread runs
// the tick, and which runs the bridge present").
struct ThreadSet
{
    std::array<std::uint32_t, kMaxThreads> ids {};
    std::uint32_t count = 0;

    void Add(std::uint32_t tid)
    {
        if (tid == 0)
            return;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (ids[i] == tid)
                return;
        }
        if (count < kMaxThreads)
            ids[count] = tid;
        ++count;
    }
    std::uint32_t First() const { return count == 0 ? 0 : ids[0]; }
    void Clear() { *this = ThreadSet {}; }
};

// What one closed window reports. The flags say whether the metric had samples.
struct Summary
{
    std::uint64_t ticks = 0;
    std::uint64_t presents = 0;
    std::uint64_t presentsInsideTick = 0; // presents that began inside an open tick on its own thread, paired or not
    std::uint64_t sleeps = 0;
    std::uint64_t paired = 0;      // presents whose carried frame number found its tick
    std::uint64_t pairedSame = 0;  // ... through the open tick the present ran inside
    std::uint64_t pairedSplit = 0; // ... through a finished tick
    CounterState counter = CounterState::Ok;
    Pairing pairing = Pairing::None;
    bool hasPeriod = false, hasBody = false, hasAge = false, hasWait = false, hasPresentWait = false, hasSlack = false, hasSleep = false,
         hasSleepDur = false, hasPaired = false;
    std::int64_t periodP50 = 0, periodP95 = 0, bodyP50 = 0;
    std::int64_t ageP5 = 0, ageP50 = 0, ageP95 = 0;
    std::int64_t presentWaitP50 = 0, presentWaitP95 = 0; // W: the game thread's time inside the native present (inside the tick)
    std::int64_t waitP50 = 0, waitP95 = 0;               // queue: a finished tick waiting for the previous present to end (outside the tick)
    std::int64_t slackP50 = 0;
    std::int64_t sleepP50 = 0;
    std::int64_t sleepDurP50 = 0, sleepDurP95 = 0; // the Reflex sleep itself
    double pairedShare = 0.0;
    ThreadSet tickThreads;
    ThreadSet presentThreads;
};

// The probe's state machine. One thread drives it (the sink serialises); the events may come from different threads.
class Probe
{
  public:
    Probe() { ResetWindow(); }

    // A tick began. `counterBefore` is Framecounter before the original ran, `frameCounter` false when the build did not resolve it. The tick's
    // frame number is `counterBefore + 1` (the first instruction of the tick increments the counter), so consecutive ticks give consecutive
    // numbers. A tick that begins while the probe is not recording is never begun here (the taps drop it), which is what keeps arming mid-pipeline
    // from inventing a frame.
    void TickBegin(std::int64_t nowUs, std::uint32_t tid, std::uint32_t counterBefore, bool frameCounter)
    {
        if (_haveLastTickBegin)
            _period.Add(nowUs - _lastTickBeginUs);
        _lastTickBeginUs = nowUs;
        _haveLastTickBegin = true;
        if (_haveLastPresentEnd)
            _slack.Add(nowUs - _lastPresentEndUs);
        if (_haveLastSleepEnd && nowUs - _lastSleepEndUs <= kStaleSleepUs)
            _sleepToTick.Add(nowUs - _lastSleepEndUs);

        _pendingFrame = counterBefore + 1; // the frame this tick simulates (wraps with the game's u32 counter)
        _pendingUsable = frameCounter;
        _pendingBeginUs = nowUs;
        _pendingTid = tid;
        _pendingSlot = static_cast<std::uint8_t>(_nextSlot);
        _havePending = true;

        TickRec& rec = _ticks[_nextSlot % kTickRing];
        ++_nextSlot;
        rec = TickRec {};
        rec.frame = _pendingFrame;
        rec.beginUs = nowUs;
        rec.usable = frameCounter;
        rec.used = true;

        if (!frameCounter)
            NoteMissingCounter();
        else if (_haveLastTickAfter && counterBefore != _lastTickAfter)
            NoteInconsistentCounter(); // the counter skipped or repeated a value between two ticks: it cannot number frames
        _haveLastTickAfter = frameCounter;
        _lastTickAfter = _pendingFrame;

        ++_sum.ticks;
        _sum.tickThreads.Add(tid);
    }

    // The original tick returned. `counterAfter` is Framecounter after it ran and must be the value the begin call predicted.
    void TickEnd(std::int64_t nowUs, std::uint32_t counterAfter)
    {
        if (!_havePending)
            return; // a tick that began while the probe was not recording: dropped, never half-recorded
        if (_pendingUsable && counterAfter != _pendingFrame)
            NoteInconsistentCounter();
        TickRec& rec = _ticks[_pendingSlot % kTickRing];
        if (rec.used && rec.frame == _pendingFrame)
        {
            rec.endUs = nowUs;
            rec.ended = true;
        }
        _body.Add(nowUs - _pendingBeginUs);
        _havePending = false;
    }

    // The bridge present began. `counterAtBegin` is Framecounter at that moment: the frame number the present carries. A present that begins while a
    // tick is open on this same thread is running inside that tick, which is the shape of FFXIV's main loop; the end decides whether it pairs with
    // that tick, and the flag is kept here because the tick may have been finished by the time the present ends (another thread) or dropped by a
    // re-entrant begin.
    void PresentBegin(std::int64_t nowUs, std::uint32_t tid, std::uint32_t counterAtBegin, bool frameCounter)
    {
        _presentBeginUs = nowUs;
        _presentTid = tid;
        _presentFrame = counterAtBegin;
        _presentUsable = frameCounter;
        _presentInsideTick = frameCounter && _havePending && _pendingUsable && _pendingTid == tid;
        _havePresent = true;
        _sum.presentThreads.Add(tid);
        if (!frameCounter)
            NoteMissingCounter();
    }

    void PresentEnd(std::int64_t nowUs, std::uint32_t counterAfter)
    {
        ++_sum.presents;
        if (_presentInsideTick)
            ++_sum.presentsInsideTick;
        const bool usable = _havePresent && _presentUsable;
        if (usable)
        {
            if (counterAfter < _presentFrame)
                NoteInconsistentCounter(); // the counter went backwards inside one present
            if (_presentInsideTick)
            {
                // The same-thread rule: the present ran inside the open tick and carries its frame. The tick is still open (its end comes after the
                // present on this thread), so the age is the tick's start to the present's start and W is the present's own duration. A present that
                // began inside a tick but does not carry that tick's number pairs with nothing, never with an older finished tick.
                if (_havePending && _pendingUsable && _pendingTid == _presentTid && _pendingFrame == _presentFrame)
                {
                    _age.Add(_presentBeginUs - _pendingBeginUs);
                    _presentWait.Add(nowUs - _presentBeginUs);
                    ++_sum.pairedSame;
                    ++_sum.paired;
                }
            }
            else
            {
                const TickRec* rec = FindTick(_presentFrame);
                if (rec != nullptr && rec->ended && rec->usable)
                {
                    _age.Add(nowUs - rec->beginUs);
                    if (_haveLastPresentEnd)
                        _wait.Add(_lastPresentEndUs - rec->endUs);
                    ++_sum.pairedSplit;
                    ++_sum.paired;
                }
            }
        }
        _lastPresentEndUs = nowUs;
        _haveLastPresentEnd = true;
        _havePresent = false;
        _presentInsideTick = false;
    }

    void SleepBegin(std::int64_t nowUs)
    {
        _sleepBeginUs = nowUs;
        _haveSleepBegin = true;
    }

    void SleepEnd(std::int64_t nowUs)
    {
        // The sleep's own duration, reported separately from sleep->tick: it is a wait the game thread sits through inside the present path, and W
        // inside the tick contains it. A sleep whose begin the probe did not see (armed in the middle of one) contributes nothing.
        if (_haveSleepBegin && nowUs >= _sleepBeginUs)
            _sleepDur.Add(nowUs - _sleepBeginUs);
        _haveSleepBegin = false;
        _lastSleepEndUs = nowUs;
        _haveLastSleepEnd = true;
        ++_sum.sleeps;
    }

    // True when a 10 s window closed at `nowUs`: `out` holds it and a fresh window is open.
    bool Due(std::int64_t nowUs, Summary& out)
    {
        if (!_open)
        {
            _open = true;
            _windowStartUs = nowUs;
            return false;
        }
        if (nowUs < _windowStartUs)
        {
            // A clock that stepped back restarts the window instead of closing a wrong one. The event that showed the step is dropped with the
            // old window: its distance from either window start means nothing. The pairing state survives, as it does at every window close.
            ResetWindow();
            _open = true;
            _windowStartUs = nowUs;
            return false;
        }
        if (nowUs - _windowStartUs < kWindowUs)
            return false;
        out = Build();
        ResetWindow();
        return true;
    }

    // The window's numbers, without closing it (tests, and the sink after Due filled its summary).
    Summary Build() const
    {
        Summary s = _sum;
        s.counter = _counter;
        s.periodP50 = Percentile(_period, 0.50);
        s.periodP95 = Percentile(_period, 0.95);
        s.bodyP50 = Percentile(_body, 0.50);
        s.slackP50 = Percentile(_slack, 0.50);
        s.sleepP50 = Percentile(_sleepToTick, 0.50);
        s.hasPeriod = !_period.Empty();
        s.hasBody = !_body.Empty();
        s.hasSlack = !_slack.Empty();
        s.hasSleep = !_sleepToTick.Empty() && s.sleeps != 0;
        s.sleepDurP50 = Percentile(_sleepDur, 0.50);
        s.sleepDurP95 = Percentile(_sleepDur, 0.95);
        s.hasSleepDur = !_sleepDur.Empty() && s.sleeps != 0;
        // age and W exist only on top of a frame counter that can carry the pairing
        const bool counterOk = _counter == CounterState::Ok;
        s.ageP5 = Percentile(_age, 0.05);
        s.ageP50 = Percentile(_age, 0.50);
        s.ageP95 = Percentile(_age, 0.95);
        s.presentWaitP50 = Percentile(_presentWait, 0.50);
        s.presentWaitP95 = Percentile(_presentWait, 0.95);
        s.waitP50 = Percentile(_wait, 0.50);
        s.waitP95 = Percentile(_wait, 0.95);
        s.hasAge = counterOk && !_age.Empty();
        s.hasPresentWait = counterOk && !_presentWait.Empty();
        s.hasWait = counterOk && !_wait.Empty();
        // The pairing path of the window: which rule produced its age and W samples (counted per present, not per pair, so a window whose presents
        // ran inside the tick but carried a broken number still reads `same` with a low paired share).
        if (s.presents == 0 || s.ticks == 0)
            s.pairing = Pairing::None;
        else if (s.presentsInsideTick == 0)
            s.pairing = Pairing::Split;
        else if (s.presentsInsideTick == s.presents)
            s.pairing = Pairing::Same;
        else
            s.pairing = Pairing::Mixed;
        if (counterOk && s.presents > 0)
        {
            s.hasPaired = true;
            s.pairedShare = 100.0 * static_cast<double>(s.paired) / static_cast<double>(s.presents);
        }
        return s;
    }

    const SignedHistogram& AgeHistogram() const { return _age; }
    const SignedHistogram& WaitHistogram() const { return _wait; }
    const Histogram& PresentWaitHistogram() const { return _presentWait; }

  private:
    struct TickRec
    {
        std::uint32_t frame = 0;
        std::int64_t beginUs = 0;
        std::int64_t endUs = 0;
        bool usable = false;
        bool used = false;
        bool ended = false;
    };

    const TickRec* FindTick(std::uint32_t frame) const
    {
        for (const TickRec& rec : _ticks)
        {
            if (rec.used && rec.usable && rec.frame == frame)
                return &rec;
        }
        return nullptr;
    }

    void NoteMissingCounter() { _counter = CounterState::Missing; }
    void NoteInconsistentCounter()
    {
        if (_counter != CounterState::Missing)
            _counter = CounterState::Inconsistent;
    }

    // Clears the window's numbers and flags, and nothing that the pairing stands on: the tick ring, the tick and present in flight, the last tick
    // and present timestamps and the counter's last value all survive. A window boundary can fall inside a frame, and the next window must pair it
    // exactly as the previous one would have.
    void ResetWindow()
    {
        _sum = Summary {};
        _counter = CounterState::Ok;
        _period.Clear();
        _body.Clear();
        _slack.Clear();
        _sleepToTick.Clear();
        _sleepDur.Clear();
        _age.Clear();
        _presentWait.Clear();
        _wait.Clear();
        _open = false;
        _windowStartUs = 0;
        // not reset here: _ticks, _nextSlot, _havePending, _pending*, _havePresent, _present*, _haveSleepBegin, _haveLastTickBegin, _lastTickBeginUs,
        // _haveLastTickAfter, _lastTickAfter, _haveLastPresentEnd, _lastPresentEndUs, _haveLastSleepEnd, _lastSleepEndUs
    }

    Summary _sum;
    CounterState _counter = CounterState::Ok;
    std::array<TickRec, kTickRing> _ticks {};
    Histogram _period, _body, _sleepDur;
    SignedHistogram _slack, _sleepToTick;
    SignedHistogram _age;         // signed: a wrong pairing shows as a present before the tick it names
    Histogram _presentWait;       // W inside the tick: a present's own duration, never negative
    SignedHistogram _wait;        // signed: a negative queue wait means the render thread was waiting
    bool _open = false;
    std::int64_t _windowStartUs = 0;
    std::size_t _nextSlot = 0;
    bool _havePending = false;
    std::uint8_t _pendingSlot = 0;
    std::uint32_t _pendingFrame = 0;
    std::uint32_t _pendingTid = 0;
    bool _pendingUsable = false;
    std::int64_t _pendingBeginUs = 0;
    bool _havePresent = false;
    std::int64_t _presentBeginUs = 0;
    std::uint32_t _presentTid = 0;
    std::uint32_t _presentFrame = 0;
    bool _presentUsable = false;
    bool _presentInsideTick = false;
    std::int64_t _lastTickBeginUs = 0;
    bool _haveLastTickBegin = false;
    std::uint32_t _lastTickAfter = 0;
    bool _haveLastTickAfter = false;
    std::int64_t _lastPresentEndUs = 0;
    bool _haveLastPresentEnd = false;
    std::int64_t _lastSleepEndUs = 0;
    bool _haveLastSleepEnd = false;
    std::int64_t _sleepBeginUs = 0;
    bool _haveSleepBegin = false;
};

// ---- the line one window logs -----------------------------------------------------------------------------------------------------------

// "7.1" for 7100 us, "-0.4" for -400 us, "64.0" or more inside the overflow bucket. One decimal: the buckets are 0.25 ms wide, so a second
// digit would be noise.
inline std::string Milliseconds(std::int64_t us)
{
    const double ms = static_cast<double>(us) / 1000.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", ms);
    return std::string(buf);
}

inline std::string Value(bool has, std::int64_t us) { return has ? Milliseconds(us) : std::string("n/a"); }

inline std::string ThreadText(const ThreadSet& set)
{
    if (set.count == 0)
        return "none";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%x", set.First());
    std::string out(buf);
    if (set.count > 1)
        out += "(" + std::to_string(set.count) + " threads)"; // the thread table, when one kind ran on more than one thread
    return out;
}

//   Tick probe: ticks=1432 presents=1440 period p50=7.1 p95=7.9 body p50=2.4 | age p5=9.6 p50=13.8 p95=19.2 | W p50=6.2 p95=8.0 | queue
//   p50=n/a p95=n/a | slack p50=0.6 | sleep->tick p50=0.6 | sleep p50=0.0 p95=0.1 (DLSS-G) | threads tick=0x6554 bridge=0x6554 same | paired
//   99.9% | counter ok
//
// "n/a" replaces the number of a metric that had no sample in the window, never a zero: a zero age and "no pairing" must not read alike. age, W and
// queue read n/a together whenever the frame counter cannot carry the pairing, whether it never resolved (`counter missing`) or did not advance by
// exactly one per tick (`counter inconsistent`). W and queue also read n/a on their own when the window ran under the other pairing rule: W exists
// only for presents inside the tick, queue only for presents outside it.
inline std::string FormatSummary(const Summary& s)
{
    std::string out = "Tick probe: ticks=" + std::to_string(s.ticks) + " presents=" + std::to_string(s.presents);
    out += " period p50=" + Value(s.hasPeriod, s.periodP50) + " p95=" + Value(s.hasPeriod, s.periodP95);
    out += " body p50=" + Value(s.hasBody, s.bodyP50);
    out += " | age p5=" + Value(s.hasAge, s.ageP5) + " p50=" + Value(s.hasAge, s.ageP50) + " p95=" + Value(s.hasAge, s.ageP95);
    out += " | W p50=" + Value(s.hasPresentWait, s.presentWaitP50) + " p95=" + Value(s.hasPresentWait, s.presentWaitP95);
    out += " | queue p50=" + Value(s.hasWait, s.waitP50) + " p95=" + Value(s.hasWait, s.waitP95);
    out += " | slack p50=" + Value(s.hasSlack, s.slackP50);
    out += " | sleep->tick p50=" + Value(s.hasSleep, s.sleepP50);
    out += " | sleep p50=" + Value(s.hasSleepDur, s.sleepDurP50) + " p95=" + Value(s.hasSleepDur, s.sleepDurP95) + " (DLSS-G)";
    out += " | threads tick=" + ThreadText(s.tickThreads) + " bridge=" + ThreadText(s.presentThreads) + " " + PairingText(s.pairing);
    out += " | paired ";
    if (s.hasPaired)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f%%", s.pairedShare);
        out += buf;
    }
    else
        out += "n/a";
    out += " | counter ";
    out += CounterText(s.counter);
    return out;
}
} // namespace ext::latency
