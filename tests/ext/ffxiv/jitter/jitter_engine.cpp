#include "check.h"
#include "engine_fixture.h"

#include <string>
#include <vector>

using namespace ext::ffxiv;
using namespace ext::ffxiv::jitter;
using namespace jitter_fakes;

// What the engine writes to the log over a session, against the same fake environment as jitter_glue.cpp: the status line, the budgets
// of the sample, status and episode lines, and the report of the episodes that were not logged.

namespace
{
// M3: the status line says whether frame generation is active (the replaced pair feeds it too), and asks only when it writes the line.
void TestStatusLineSaysWhetherFrameGenerationIsActive()
{
    for (int mode = 0; mode < 3; ++mode)
    {
        FakeEnv e = MakeEnv();
        e.frameGeneration = mode == 0 ? std::optional<bool> { true } : mode == 1 ? std::optional<bool> { false } : std::nullopt;
        Engine engine;
        int statusLines = 0;
        for (int i = 0; i < 40; ++i)
        {
            const Outcome o = Evaluate(e, engine, At(100 + i, 1, static_cast<std::uint64_t>(i) * 1000), false, true);
            for (const std::string& line : o.logs)
                if (Has(line, "Jitter correction: on;"))
                {
                    ++statusLines;
                    CHECK(Has(line, mode == 0 ? "frame generation active yes" : mode == 1 ? "frame generation active no" : "frame generation active unknown"));
                }
        }
        CHECK(statusLines == 4 && e.fgQueries == 4); // one every 10 s of the fake clock, and nothing asked in between
    }
}

// M2: what the status line says about it, so a session shows whether the correction is inert.
void TestStatusLineCountsHeldBackFrames()
{
    FakeEnv e = MakeEnv();
    Engine engine;
    // first status line at the first evaluate (a sample is due); the second window is the four evaluates after it
    Evaluate(e, engine, At(100, 1.0f, 0), false, true);
    Evaluate(e, engine, At(101, 1.0f, 100), false, true);   // replaced
    Evaluate(e, engine, At(101, 1.0f, 200), false, true);   // the counter did not increase
    Evaluate(e, engine, At(102, 1.5f, 300), false, true);   // the ratio changed
    const Outcome last = Evaluate(e, engine, At(103, 1.5f, 10000), false, true); // replaced; a status line is due
    std::string status;
    for (const std::string& line : last.logs)
        if (Has(line, "Jitter correction: on;"))
            status = line;
    CHECK(!status.empty());
    CHECK(Has(status, "4 evaluates seen") && Has(status, "2 replaced") && Has(status, "2 passed through unchanged"));
    CHECK(Has(status, "1 frame counter did not increase") && Has(status, "1 ratio not steady") && Has(status, "0 first frame after a gap"));
}

// L3: the sample lines are capped per session (an hour of 10-second samples), then one line says so; the status lines likewise.
void TestLogLinesAreCappedPerSession()
{
    {
        FakeEnv e = MakeEnv();
        Engine engine;
        for (int i = 0; i < 420; ++i)
            Evaluate(e, engine, At(100 + i, 1, static_cast<std::uint64_t>(i) * 10000), true, false);
        CHECK(CountLines(e.logs, "Game jitter: counter") == 360);
        CHECK(CountLines(e.logs, "sample line budget of 360 lines used up") == 1);
        CHECK(e.logs.size() == 361); // the 360 samples and the one budget line, in that order
        CHECK(Has(e.logs.back(), "sample line budget"));
    }
    {
        FakeEnv e = MakeEnv();
        Engine engine;
        for (int i = 0; i < 420; ++i)
            Evaluate(e, engine, At(100 + i, 1, static_cast<std::uint64_t>(i) * 10000), false, true);
        CHECK(CountLines(e.logs, "Jitter correction: on;") == 360);
        CHECK(CountLines(e.logs, "status line budget of 360 lines used up") == 1);
    }
}

// L3: the suppressed episode counts of all three logs are printed from the probe-only branch too, and only when they change (not at
// every sample).
void TestSuppressedEpisodesAreReportedFromEveryMode()
{
    {
        // probe only: a state that flickers between failing and fine at every sample
        FakeEnv e = MakeEnv();
        Engine engine;
        const std::uint64_t pointer = kBase + kCurrentBuild.jitter.cameraGlobalRva;
        for (int i = 0; i < 100; ++i)
        {
            e.mem.PutU64(pointer, i % 2 == 1 && i < 80 ? 0 : kGlobal); // 40 failing samples among the first 80, fine afterwards
            Evaluate(e, engine, At(100 + i, 1, static_cast<std::uint64_t>(i) * 10000), true, false);
        }
        CHECK(CountLines(e.logs, "Game jitter: not read, guard failed") == 32);
        CHECK(CountLines(e.logs, "log line budget used up") == 8); // one per change of the total (33rd to 40th episode), not per sample
        CHECK(Has(e.logs[e.logs.size() - 1], "sample line budget") == false);
        std::string last;
        for (const std::string& line : e.logs)
            if (Has(line, "log line budget used up"))
                last = line;
        CHECK(Has(last, "8 probe failures, 0 pass-throughs, 0 replacements"));
    }
    {
        // correction: replacement and pass-through episodes alternate (a counter that stalls after every replacement)
        FakeEnv e = MakeEnv();
        Engine engine;
        std::uint64_t t = 0;
        Evaluate(e, engine, At(100, 1, t += 10000), false, true);
        for (int n = 101; n < 101 + 40; ++n)
        {
            Evaluate(e, engine, At(n, 1, t += 10000), false, true);
            Evaluate(e, engine, At(n, 1, t += 10000), false, true); // the same counter again: held back
        }
        std::string last;
        for (const std::string& line : e.logs)
            if (Has(line, "log line budget used up"))
                last = line;
        CHECK(!last.empty() && Has(last, "0 probe failures, 8 pass-throughs, 8 replacements"));
    }
}
} // namespace

int main()
{
    TestStatusLineCountsHeldBackFrames();
    TestStatusLineSaysWhetherFrameGenerationIsActive();
    TestLogLinesAreCappedPerSession();
    TestSuppressedEpisodesAreReportedFromEveryMode();
    return TEST_RESULT("jitter_engine");
}
