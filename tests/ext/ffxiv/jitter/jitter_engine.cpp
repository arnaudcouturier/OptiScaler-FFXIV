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
    // L2 (fix round 2): the field also names the frame-generation INPUT, because only the Upscaler input reads the corrected pair from the
    // parameter block; for another input "active yes" would be true while the pair never reaches it.
    const char* inputs[] = { "Upscaler", "NoFG", "", "DLSSG" };
    for (int mode = 0; mode < 4; ++mode)
    {
        FakeEnv e = MakeEnv();
        e.frameGeneration = mode == 0 ? std::optional<bool> { true } : mode == 1 ? std::optional<bool> { false } : mode == 2 ? std::nullopt : std::optional<bool> { true };
        e.frameGenerationInput = inputs[mode];
        Engine engine;
        int statusLines = 0;
        for (int i = 0; i < 40; ++i)
        {
            const Outcome o = Evaluate(e, engine, At(100 + i, 1, static_cast<std::uint64_t>(i) * 1000), false, true);
            for (const std::string& line : o.logs)
                if (Has(line, "Jitter correction: on;"))
                {
                    ++statusLines;
                    const char* expected[] = { "frame generation active yes (input Upscaler)", "frame generation active no (input NoFG)",
                                               "frame generation active unknown", "frame generation active yes (input DLSSG)" };
                    CHECK(Has(line, expected[mode]));
                    CHECK(mode != 2 || !Has(line, "(input")); // an input that could not be told is left out, not printed empty
                }
        }
        CHECK(statusLines == 4 && e.fgQueries == 4 && e.fgInputQueries == 4); // one every 10 s of the fake clock, and nothing asked in between
    }
}

// M2: what the status line says about it, so a session shows whether the correction is inert.
void TestStatusLineCountsHeldBackFrames()
{
    FakeEnv e = MakeEnv();
    Engine engine;
    // The first status line comes with the first evaluate (a sample is due); the second one is the last evaluate below. The counts are
    // TOTALS since the start, not since the previous line.
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
    CHECK(Has(status, "5 evaluates seen") && Has(status, "2 replaced") && Has(status, "3 passed through unchanged"));
    CHECK(Has(status, "1 frame counter did not increase") && Has(status, "1 ratio not steady") && Has(status, "1 first frame after a gap"));
}

// L3: the sample lines are capped per session (an hour of 10-second samples), then one line says so.
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
// L1 (fix round 2): the status lines are thinned, never silenced. The first 60 come with every sample tick (about every 10 s), then one
// per 60 s, and every line carries the TOTALS since the start, so the last line of a long session still tells the whole story.
void TestStatusLinesAreThinnedAndCarryTotals()
{
    FakeEnv e = MakeEnv();
    Engine engine;
    std::vector<int> statusAtTick;
    std::string last;
    for (int i = 0; i < 420; ++i) // 70 minutes, one evaluate per 10 s
    {
        const Outcome o = Evaluate(e, engine, At(100 + i, 1, static_cast<std::uint64_t>(i) * 10000), false, true);
        for (const std::string& line : o.logs)
            if (Has(line, "Jitter correction: on;"))
            {
                statusAtTick.push_back(i);
                last = line;
                if (i == 1)
                    CHECK(Has(line, "2 evaluates seen")); // totals since the start, not since the previous line
            }
    }
    CHECK(statusAtTick.size() == 120);
    for (std::size_t k = 0; k < statusAtTick.size() && k < 60; ++k)
        CHECK(statusAtTick[k] == static_cast<int>(k)); // the first 60 lines: every tick
    for (std::size_t k = 60; k < statusAtTick.size(); ++k)
        CHECK(statusAtTick[k] == 65 + 6 * static_cast<int>(k - 60)); // then one per 60 s: the first tick a minute after line 60 (tick 59)
    CHECK(!statusAtTick.empty() && statusAtTick.back() == 419);
    // nothing was zeroed after line 60: the last line says everything since the start
    CHECK(Has(last, "420 evaluates seen") && Has(last, "419 replaced") && Has(last, "1 passed through unchanged"));
    CHECK(Has(last, "1 first frame after a gap"));
    CHECK(CountLines(e.logs, "budget") == 0); // thinned, not cut off: no budget line any more
    CHECK(e.fgQueries == 120);                 // frame generation is asked only when a line is written
}

// L1: the report of the episodes that were not logged is thinned the same way, and a change is never lost: the last report carries the
// final total.
void TestSuppressedReportIsThinnedAndKeepsTheFinalTotal()
{
    FakeEnv e = MakeEnv();
    Engine engine;
    const std::uint64_t pointer = kBase + kCurrentBuild.jitter.cameraGlobalRva;
    std::vector<int> reportAtTick;
    std::string last;
    for (int i = 0; i < 500; ++i) // probe only, one sample per 10 s
    {
        e.mem.PutU64(pointer, i % 2 == 1 && i < 398 ? 0 : kGlobal); // 199 failing samples (odd ticks 1..397), fine afterwards
        const Outcome o = Evaluate(e, engine, At(100 + i, 1, static_cast<std::uint64_t>(i) * 10000), true, false);
        for (const std::string& line : o.logs)
            if (Has(line, "log line budget used up"))
            {
                reportAtTick.push_back(i);
                last = line;
            }
    }
    CHECK(CountLines(e.logs, "Game jitter: not read, guard failed") == 32);
    // 167 episodes had no line left (199 - 32); the total changes at every failing tick from the 33rd episode on (tick 65)
    for (std::size_t k = 0; k < reportAtTick.size() && k < 60; ++k)
        CHECK(reportAtTick[k] == 65 + 2 * static_cast<int>(k)); // the first 60 reports: at every change
    for (std::size_t k = 61; k < reportAtTick.size(); ++k)
        CHECK(reportAtTick[k] - reportAtTick[k - 1] >= 6); // then at most one per 60 s
    CHECK(reportAtTick.size() == 96);
    CHECK(Has(last, "167 probe failures, 0 pass-throughs, 0 replacements")); // the final total, cumulative, even though the change came between two permitted ticks
    CHECK(!reportAtTick.empty() && reportAtTick.back() == 399);
}

// L5 (fix round 2): a step that throws is abandoned (fail open) and says so ONCE per episode of exceptions; a good step ends the episode;
// the logging itself can never throw out of the step; the lock is released.
void TestAnExceptionAbandonsTheStepAndIsLoggedOncePerEpisode()
{
    auto exceptionLines = [](const std::vector<std::string>& logs) { return CountLines(logs, "step abandoned after an exception"); };
    {
        // ten evaluates that all throw while reading the block: no escape, no write, one line, the lock free after every one
        FakeEnv e = MakeEnv();
        Engine engine;
        e.fault = [](const char* where)
        {
            if (std::string(where) == "GetFloat")
                throw std::runtime_error("block exploded");
        };
        for (int i = 0; i < 10; ++i)
        {
            Evaluate(e, engine, At(100 + i, 1, static_cast<std::uint64_t>(i) * 16), false, true);
            CHECK(e.lockDepth == 0);
        }
        CHECK(e.setsEver == 0);
        CHECK(exceptionLines(e.logs) == 1 && CountLines(e.logs, "block exploded") == 1);
        // the game recovers: the correction works again without a restart, and a NEW failure is a new episode with a new line
        e.fault = nullptr;
        Evaluate(e, engine, At(110, 1, 200), false, true);
        Evaluate(e, engine, At(111, 1, 216), false, true);
        CHECK(e.setsEver == 2);
        e.fault = [](const char* where)
        {
            if (std::string(where) == "GetFloat")
                throw std::runtime_error("second failure");
        };
        Evaluate(e, engine, At(112, 1, 232), false, true);
        Evaluate(e, engine, At(113, 1, 248), false, true);
        CHECK(exceptionLines(e.logs) == 2 && CountLines(e.logs, "second failure") == 1);
    }
    {
        // not a std::exception
        FakeEnv e = MakeEnv();
        Engine engine;
        e.fault = [](const char*) { throw 42; };
        Evaluate(e, engine, At(100, 1, 0), false, true);
        CHECK(exceptionLines(e.logs) == 1 && CountLines(e.logs, "unknown exception") == 1);
    }
    {
        // a write that throws half way (X stored, Y throws) is abandoned too, and the next good step is untouched by it
        FakeEnv e = MakeEnv();
        Engine engine;
        Evaluate(e, engine, At(100, 1, 0), false, true);
        int sets = 0;
        e.fault = [&sets](const char* where)
        {
            if (std::string(where) == "SetFloat" && ++sets == 2)
                throw std::runtime_error("Y refused");
        };
        Evaluate(e, engine, At(101, 1, 16), false, true);
        CHECK(exceptionLines(e.logs) == 1 && CountLines(e.logs, "Y refused") == 1 && e.lockDepth == 0);
    }
    {
        // the log line cannot make things worse: a Log that throws is swallowed, the step still does not escape
        FakeEnv e = MakeEnv();
        Engine engine;
        e.logThrows = true;
        e.fault = [](const char* where)
        {
            if (std::string(where) == "GetFloat")
                throw std::runtime_error("boom");
        };
        for (int i = 0; i < 3; ++i)
            Evaluate(e, engine, At(100 + i, 1, static_cast<std::uint64_t>(i) * 16), false, true);
        CHECK(e.logs.empty() && e.setsEver == 0 && e.lockDepth == 0);
    }
    {
        // flapping between good and failing steps cannot flood the log: the episode budget applies (32 lines)
        FakeEnv e = MakeEnv();
        Engine engine;
        bool failing = false;
        e.fault = [&failing](const char* where)
        {
            if (failing && std::string(where) == "GetFloat")
                throw std::runtime_error("flap");
        };
        for (int i = 0; i < 100; ++i)
        {
            failing = i % 2 == 1;
            Evaluate(e, engine, At(100 + i, 1, static_cast<std::uint64_t>(i) * 16), false, true);
        }
        CHECK(exceptionLines(e.logs) == 32);
    }
    {
        // probe only: the same, and nothing is ever written
        FakeEnv e = MakeEnv();
        Engine engine;
        e.fault = [](const char*) { throw std::runtime_error("probe side"); };
        for (int i = 0; i < 5; ++i)
            Evaluate(e, engine, At(100 + i, 1, static_cast<std::uint64_t>(i) * 16), true, false);
        CHECK(exceptionLines(e.logs) == 1 && e.setsEver == 0);
    }
}

// L4 (fix round 2): switching the correction off (a step with the key off, the probe still on) forgets the same-frame reference, so the
// first evaluate after it is a first frame again instead of being compared with one from before the pause.
void TestSwitchingTheCorrectionOffForgetsTheReference()
{
    auto replacementLines = [](const Outcome& o) { return CountLines(o.logs, "replaced by applied"); };
    FakeEnv e = MakeEnv();
    Engine engine;
    Evaluate(e, engine, At(100, 1, 0), false, true);
    const Outcome on = Evaluate(e, engine, At(101, 1, 16), false, true);
    CHECK(on.sets.size() == 2 && replacementLines(on) == 1);
    CHECK(Evaluate(e, engine, At(102, 1, 32), false, true).sets.size() == 2); // the same episode, no new line
    // the key is off for a while (probe only), then back on: the counter went up and the ratio is the same, and still it must not replace
    Evaluate(e, engine, At(103, 1, 48), true, false);
    const Outcome back = Evaluate(e, engine, At(104, 1, 64), false, true);
    CHECK(back.sets.empty());
    const Outcome again = Evaluate(e, engine, At(105, 1, 80), false, true);
    CHECK(again.sets.size() == 2);
    CHECK(replacementLines(again) == 1); // a new episode: the read-back line is written again
}

} // namespace

int main()
{
    TestStatusLineCountsHeldBackFrames();
    TestStatusLineSaysWhetherFrameGenerationIsActive();
    TestLogLinesAreCappedPerSession();
    TestAnExceptionAbandonsTheStepAndIsLoggedOncePerEpisode();
    TestSwitchingTheCorrectionOffForgetsTheReference();
    TestStatusLinesAreThinnedAndCarryTotals();
    TestSuppressedReportIsThinnedAndKeepsTheFinalTotal();
    TestSuppressedEpisodesAreReportedFromEveryMode();
    return TEST_RESULT("jitter_engine");
}
