// [PACE] Pure core of the frame-pacing CPU trace (ext/present/FramePacingCore.h): the accumulator thresholds, the 10 s window,
// the log line text and the enable decision with the reason text. No upstream type, no clock of its own: the window takes the
// time as an argument.
#include "ext/present/FramePacingCore.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <thread>

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

namespace
{
constexpr int64_t kSec = 1'000'000; // microseconds

void AccumulatorThresholds()
{
    struct Row
    {
        uint64_t us;
        uint64_t o8, o16, o33;
    };
    // thresholds are inclusive (>=), like the reference trace
    const Row rows[] = { { 0, 0, 0, 0 },         { 7999, 0, 0, 0 },     { 8000, 1, 0, 0 },     { 15999, 1, 0, 0 },
                         { 16000, 1, 1, 0 },     { 32999, 1, 1, 0 },    { 33000, 1, 1, 1 },    { 5'000'000, 1, 1, 1 } };
    for (const Row& r : rows)
    {
        Accumulator a;
        a.Add(r.us);
        CHECK(a.calls == 1);
        CHECK(a.over8 == r.o8);
        CHECK(a.over16 == r.o16);
        CHECK(a.over33 == r.o33);
        CHECK(a.maxUs == r.us);
        CHECK(a.sumUs == r.us);
    }

    // several samples: counts, sum, max
    Accumulator a;
    for (uint64_t us : { 100ull, 8000ull, 16000ull, 33000ull, 200ull })
        a.Add(us);
    CHECK(a.calls == 5);
    CHECK(a.sumUs == 100 + 8000 + 16000 + 33000 + 200);
    CHECK(a.maxUs == 33000);
    CHECK(a.over8 == 3 && a.over16 == 2 && a.over33 == 1);

    // the max keeps the larger value when a smaller sample follows
    Accumulator b;
    b.Add(900);
    b.Add(100);
    CHECK(b.maxUs == 900);
}

void WindowTiming()
{
    Window w;
    Summary s;

    // the first sample opens the window and never reports
    CHECK(!w.Add(500, 1000 * kSec, s));
    // 9.999 s after the first sample: no summary
    CHECK(!w.Add(600, 1000 * kSec + 9'999'999, s));
    // exactly 10.000 s: one summary holding every sample so far (inclusive), then a reset
    CHECK(w.Add(700, 1000 * kSec + 10'000'000, s));
    CHECK(s.acc.calls == 3);
    CHECK(s.acc.sumUs == 500 + 600 + 700);
    CHECK(s.acc.maxUs == 700);

    // after the summary the window is empty: the next sample opens a new one, 10 s later reports again, with that window only
    CHECK(!w.Add(10, 1011 * kSec, s));
    CHECK(!w.Add(20, 1011 * kSec + 9'999'999, s));
    CHECK(w.Add(30, 1021 * kSec, s));
    CHECK(s.acc.calls == 3);
    CHECK(s.acc.sumUs == 60);
}

void WindowEdgeCases()
{
    Summary s;

    // a clock that goes backwards resets the window instead of reporting a negative span; the sample is kept
    {
        Window w;
        CHECK(!w.Add(100, 5000 * kSec, s));
        CHECK(!w.Add(100, 4000 * kSec, s)); // earlier than the window start
        CHECK(!w.Add(100, 4000 * kSec + 9'999'999, s));
        CHECK(w.Add(100, 4010 * kSec, s));
        CHECK(s.acc.calls == 3); // the reset sample and the two after it
    }

    // a window that is far over 10 s when the next sample arrives (a thread that went idle) still reports once
    {
        Window w;
        CHECK(!w.Add(1, 100 * kSec, s));
        CHECK(w.Add(2, 500 * kSec, s));
        CHECK(s.acc.calls == 2);
        CHECK(!w.Add(3, 500 * kSec + 1, s));
    }

    // skipped counter: part of the window, reset with it
    {
        Window w;
        w.NoteSkipped();
        w.NoteSkipped();
        CHECK(!w.Add(10, 7 * kSec, s));
        w.NoteSkipped();
        CHECK(w.Add(10, 17 * kSec, s));
        CHECK(s.skipped == 3);
        CHECK(!w.Add(10, 18 * kSec, s));
        CHECK(w.Add(10, 28 * kSec, s));
        CHECK(s.skipped == 0);
    }
}

void ThreadsKeepSeparateWindows()
{
    // the real trace keeps one array of windows per thread (thread_local); the core only has to be free of shared state
    static thread_local Window tl;
    Summary s;
    CHECK(!tl.Add(1, 10 * kSec, s));
    bool otherReported = true;
    uint64_t otherCalls = 0;
    std::thread t(
        [&]
        {
            Summary s2;
            // this thread never saw the first sample: its window opens at 15 s, so 15 s is not yet 10 s past it
            const bool a = tl.Add(1, 15 * kSec, s2);
            const bool b = tl.Add(1, 25 * kSec, s2);
            otherReported = a;
            otherCalls = b ? s2.acc.calls : 0;
        });
    t.join();
    CHECK(!otherReported);
    CHECK(otherCalls == 2);
    // this thread's window is untouched by the other: its own second sample at 20 s closes the window opened at 10 s
    CHECK(tl.Add(1, 20 * kSec, s));
    CHECK(s.acc.calls == 2);
}

void SpanTracker()
{
    // begin/end of one span; End consumes it
    {
        Span sp;
        uint64_t us = 0;
        CHECK(!sp.End(100, 0, us)); // never begun
        sp.Begin(1000);
        CHECK(sp.End(1250, 0, us) && us == 250);
        CHECK(!sp.End(1300, 0, us)); // consumed
    }
    // a second Begin before End restarts the span (a failed dispatch never ended)
    {
        Span sp;
        uint64_t us = 0;
        sp.Begin(1000);
        sp.Begin(5000);
        CHECK(sp.End(5100, 0, us) && us == 100);
    }
    // a start older than the limit is discarded and cleared
    {
        Span sp;
        uint64_t us = 0;
        sp.Begin(1000);
        CHECK(!sp.End(1000 + 1'000'001, 1'000'000, us)); // older than 1 s
        CHECK(!sp.End(1000 + 1'000'002, 1'000'000, us)); // and gone
        sp.Begin(1000);
        CHECK(sp.End(1000 + 1'000'000, 1'000'000, us) && us == 1'000'000); // exactly the limit still counts
    }
    // a clock that went backwards discards
    {
        Span sp;
        uint64_t us = 0;
        sp.Begin(9000);
        CHECK(!sp.End(8000, 0, us));
    }
    // Cancel forgets an open span
    {
        Span sp;
        uint64_t us = 0;
        sp.Begin(100);
        sp.Cancel();
        CHECK(!sp.End(200, 0, us));
    }
    // time zero is a valid start (the span flag, not the value, says it is open)
    {
        Span sp;
        uint64_t us = 0;
        sp.Begin(0);
        CHECK(sp.End(40, 0, us) && us == 40);
    }
}

void FormatLines()
{
    Summary s;
    s.acc.calls = 3;
    s.acc.sumUs = 6000;
    s.acc.maxUs = 4321;
    s.acc.over8 = 1;
    s.acc.over16 = 2;
    s.acc.over33 = 3;
    CHECK(FormatLine(Phase::NativePresent, 39872, s, false) ==
          "Frame pacing CPU [native present] tid 39872: calls 3, mean 2.000 ms, max 4.321 ms, >=8ms 1, >=16ms 2, >=33ms 3");
    // the skipped counter only when asked for
    s.skipped = 7;
    CHECK(FormatLine(Phase::HiddenPresent, 5, s, true) ==
          "Frame pacing CPU [hidden DX11 present] tid 5: calls 3, mean 2.000 ms, max 4.321 ms, >=8ms 1, >=16ms 2, >=33ms 3, skipped 7");
    CHECK(FormatLine(Phase::HiddenPresent, 5, s, false).find("skipped") == std::string::npos);

    // no division by zero for an empty summary
    Summary empty;
    CHECK(FormatLine(Phase::OutputPresent, 1, empty, false) ==
          "Frame pacing CPU [output present] tid 1: calls 0, mean 0.000 ms, max 0.000 ms, >=8ms 0, >=16ms 0, >=33ms 0");

    // large numbers keep their digits
    Summary big;
    big.acc.calls = 4'000'000'000ull;
    big.acc.sumUs = 4'000'000'000ull * 250;
    big.acc.maxUs = 123'456'789;
    CHECK(FormatLine(Phase::FgDispatch, 4'000'000'000u, big, false).find("tid 4000000000: calls 4000000000, mean 0.250 ms, max 123456.789 ms") !=
          std::string::npos);

    // the names of all phases (the in-game proof greps them)
    CHECK(std::strcmp(PhaseName(Phase::NativePresent), "native present") == 0);
    CHECK(std::strcmp(PhaseName(Phase::HiddenPresent), "hidden DX11 present") == 0);
    CHECK(std::strcmp(PhaseName(Phase::FgDispatch), "FG dispatch") == 0);
    CHECK(std::strcmp(PhaseName(Phase::ReflexSleep), "Reflex sleep") == 0);
    CHECK(std::strcmp(PhaseName(Phase::OutputPresent), "output present") == 0);
    CHECK(std::strcmp(PhaseName(Phase::SceneNr), "scene NR recording") == 0);
    CHECK(std::strcmp(PhaseName(Phase::PreviewNr), "preview NR") == 0);
    CHECK(static_cast<unsigned>(Phase::Count) == 7);
    CHECK(PhaseName(Phase::Count)[0] == '?'); // out of range does not read past the table
}

void DecideTable()
{
    using Opt = std::optional<bool>;
    // not the FFXIV exe: off whatever the key says
    for (Opt key : { Opt {}, Opt { true }, Opt { false } })
        for (bool toFile : { false, true })
            for (int level : { 0, 1, 2 })
            {
                const Decision d = Decide(key, toFile, level, false);
                CHECK(!d.on);
                CHECK(d.reason == Reason::NotFfxiv);
            }

    // key true: on at every log level, with or without the file log
    for (bool toFile : { false, true })
        for (int level : { 0, 1, 2, 3 })
        {
            const Decision d = Decide(Opt { true }, toFile, level, true);
            CHECK(d.on && d.reason == Reason::KeyOn);
        }
    // key false: off even with the file log at Debug
    for (bool toFile : { false, true })
        for (int level : { 0, 1, 2 })
        {
            const Decision d = Decide(Opt { false }, toFile, level, true);
            CHECK(!d.on && d.reason == Reason::KeyOff);
        }
    // key unset (auto): the reference rule, file log and level <= 1
    {
        CHECK(Decide(Opt {}, true, 0, true).on && Decide(Opt {}, true, 0, true).reason == Reason::DebugLog);
        CHECK(Decide(Opt {}, true, 1, true).on && Decide(Opt {}, true, 1, true).reason == Reason::DebugLog);
        CHECK(!Decide(Opt {}, true, 2, true).on && Decide(Opt {}, true, 2, true).reason == Reason::AutoOff);
        CHECK(!Decide(Opt {}, true, 3, true).on);
        CHECK(!Decide(Opt {}, false, 0, true).on && Decide(Opt {}, false, 0, true).reason == Reason::AutoOff);
        CHECK(!Decide(Opt {}, false, 1, true).on);
        CHECK(!Decide(Opt {}, false, 2, true).on);
    }

    // the reason text of each outcome (the load-time line)
    CHECK(ReasonText(Reason::KeyOn, true, 2) == "on (key true)");
    CHECK(ReasonText(Reason::DebugLog, true, 1) == "on (Debug log)");
    CHECK(ReasonText(Reason::KeyOff, true, 1) == "off (key false)");
    CHECK(ReasonText(Reason::AutoOff, true, 2) == "off (key unset, log level 2)");
    CHECK(ReasonText(Reason::AutoOff, true, 3) == "off (key unset, log level 3)");
    CHECK(ReasonText(Reason::AutoOff, false, 0) == "off (key unset, no file log)");
    CHECK(ReasonText(Reason::NotFfxiv, true, 1) == "off (not FFXIV)");
    CHECK(DecisionLine(Reason::KeyOn, true, 2) == "Frame pacing trace: on (key true)");
    CHECK(DecisionLine(Reason::AutoOff, true, 2) == "Frame pacing trace: off (key unset, log level 2)");
}

// An atomic that counts how it is used: the trace asks "did the decision change?" at every scope (about 2000 times a second), so the
// unchanged case must be a plain load and the locked exchange must only run when the key really changed.
struct CountingAtomic
{
    int value;
    int loads = 0;
    int exchanges = 0;
    int load(std::memory_order) { ++loads; return value; }
    int exchange(int v, std::memory_order)
    {
        ++exchanges;
        const int old = value;
        value = v;
        return old;
    }
};

void AnnounceOnlyOnChange()
{
    CountingAtomic a { -1 };
    const int off = AnnounceKey(Reason::AutoOff, true, 2);
    CHECK(AnnounceChanged(a, off)); // the first decision is announced
    CHECK(a.exchanges == 1);
    int announcedAgain = 0;
    for (int i = 0; i < 1000; ++i)
        announcedAgain += AnnounceChanged(a, off) ? 1 : 0;
    CHECK(announcedAgain == 0);
    CHECK(a.exchanges == 1); // 1000 unchanged calls: loads only, no exchange
    CHECK(a.loads == 1001);

    const int on = AnnounceKey(Reason::KeyOn, true, 2); // the log level or the key changed in the menu: announced once more
    CHECK(AnnounceChanged(a, on));
    CHECK(!AnnounceChanged(a, on));
    CHECK(AnnounceChanged(a, off)); // and back
    CHECK(a.exchanges == 3);

    // lost race, deterministic: this thread loaded the old key, another thread stored the new one before our exchange. The exchange hands
    // back the new key, so this thread must NOT announce it again.
    {
        struct LostRace
        {
            int load(std::memory_order) { return -1; }
            int exchange(int v, std::memory_order) { return v; }
        } lost;
        CHECK(!AnnounceChanged(lost, on));
    }

    // two threads see the same new key at once: exactly one of them announces it
    for (int round = 0; round < 50; ++round)
    {
        std::atomic<int> last { -1 };
        std::atomic<int> announced { 0 };
        std::atomic<bool> go { false };
        std::thread t[4];
        for (auto& th : t)
            th = std::thread([&] {
                while (!go.load())
                    std::this_thread::yield();
                if (AnnounceChanged(last, on))
                    announced.fetch_add(1);
            });
        go.store(true);
        for (auto& th : t)
            th.join();
        CHECK(announced.load() == 1);
        CHECK(last.load() == on);
    }

    // every (reason, file log, level) is its own key, so any change of the decision is announced
    std::set<int> keys;
    for (Reason r : { Reason::KeyOn, Reason::DebugLog, Reason::KeyOff, Reason::AutoOff, Reason::NotFfxiv })
        for (bool toFile : { false, true })
            for (int level : { 0, 1, 2, 3 })
                keys.insert(AnnounceKey(r, toFile, level));
    CHECK(keys.size() == 5u * 2u * 4u);
    CHECK(AnnounceKey(Reason::KeyOn, false, 0) >= 0); // never equals the initial -1
}
} // namespace

int main()
{
    AnnounceOnlyOnChange();
    AccumulatorThresholds();
    WindowTiming();
    WindowEdgeCases();
    ThreadsKeepSeparateWindows();
    SpanTracker();
    FormatLines();
    DecideTable();
    if (g_failed != 0)
    {
        std::printf("frame_pacing_core: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("frame_pacing_core: all passed\n");
    return 0;
}
