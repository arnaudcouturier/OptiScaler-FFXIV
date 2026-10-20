#include "check.h"
#include "engine_fixture.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace ext::ffxiv;
using namespace ext::ffxiv::jitter;
using namespace jitter_fakes;

// Calibrate-and-scale through the engine (FfxivJitterCore.h) on the fake environment: a game that applies 60% of the jitter it reports
// (s = 1.2: ratio applied/reported = s / 2 = 0.6), learned from snapshots of its object and applied to the NGX pair of EVERY evaluate.

namespace
{
bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

const std::string kKey = "render 1920x1080, output 3840x2160, quality 2"; // MakeEnv(): 1920x1080 -> 3840x2160, PerfQualityValue 2

void Sequence(unsigned n, float& jx, float& jy)
{
    auto fraction = [](double v) { return v - std::floor(v); };
    jx = 0.05f + 0.4f * static_cast<float>(fraction(n * 0.6180339887498949));
    jy = -(0.05f + 0.4f * static_cast<float>(fraction(n * 0.7548776662466927)));
}

// One game session: a jitter sequence (or the constant pair 0.3 / -0.2), the object of a game with scale `s`, a clock, and the way the
// game fills the NGX block.
struct Session
{
    FakeEnv e = MakeEnv();
    Engine engine;
    float s = 1.2f;                // ratio 0.6
    int counter = 100;
    std::uint64_t t = 0, stepMs = 16;
    unsigned n = 0;
    bool sequence = true;          // false: the constant pair (0.3, -0.2)
    bool lag = false;              // the NGX block carries the PREVIOUS frame's jitter: the object is a frame ahead (never equal)
    bool refresh = true;           // false: the game never sets Jitter.Offset again after the first evaluate
    bool probe = false, correct = true;
    float prevJx = 0.0f, prevJy = 0.0f;
    float objectX = 0.0f, objectY = 0.0f, reportedX = 0.0f, reportedY = 0.0f; // of the last frame made

    Frame Make()
    {
        ++n;
        ++counter;
        t += stepMs;
        float jx = 0.3f, jy = -0.2f;
        if (sequence)
            Sequence(n, jx, jy);
        Frame f = At(counter, s, t, jx, jy);
        if (lag && n > 1)
        {
            f.ngxJx = prevJx;
            f.ngxJy = prevJy;
        }
        f.noRefresh = !refresh && n > 1;
        objectX = jx;
        objectY = jy;
        reportedX = f.ngxJx.value_or(jx);
        reportedY = f.ngxJy.value_or(jy);
        prevJx = jx;
        prevJy = jy;
        return f;
    }
    Outcome Next() { return Evaluate(e, engine, Make(), probe, correct); }
};

// Exactly two writes, X then Y, with these values.
bool Wrote(const Outcome& o, double x, double y, double tol = 1e-4)
{
    return o.sets.size() == 2 && o.sets[0].param == Param::JitterX && o.sets[1].param == Param::JitterY && Near(o.sets[0].value, x, tol) &&
           Near(o.sets[1].value, y, tol);
}

// The next evaluate of the session, and whether it wrote the reported pair scaled by `ratio` (the reported pair of THAT evaluate).
bool ScaledNext(Session& g, double ratio)
{
    const Outcome o = g.Next();
    return Wrote(o, g.reportedX * ratio, g.reportedY * ratio);
}

// The status line of an outcome ("" when none).
std::string StatusOf(const Outcome& o)
{
    for (const std::string& line : o.logs)
        if (Has(line, "Jitter correction: on;"))
            return line;
    return "";
}

// ---- the session-2 situation -----------------------------------------------------------------------------------------------------

// The NGX block carries another frame's jitter than the object (the proof of the old correction failed on 89% of the evaluates): after
// the fifth accepted snapshot EVERY evaluate is scaled, from the NGX values, not from the object's.
void TestTheObjectNeverMatchesNgxAndEveryEvaluateIsScaled()
{
    Session g;
    g.lag = true;
    for (int i = 1; i <= 60; ++i)
    {
        const Outcome o = g.Next();
        if (i >= 2)
            CHECK(std::fabs(g.reportedX - g.objectX) > 0.001f); // the object never equals the NGX pair
        if (i < 5)
        {
            CHECK_MSG(o.sets.empty(), "evaluate %d wrote before the fifth snapshot", i);
            continue;
        }
        CHECK_MSG(Wrote(o, g.reportedX * 0.6, g.reportedY * 0.6), "evaluate %d: %zu sets", i, o.sets.size());
        if (o.sets.size() == 2)
            CHECK(!Near(o.sets[0].value, g.objectX * 0.6, 1e-4)); // scaled from what NGX carries, not from the object
    }
    CHECK(g.e.setsEver == 2 * 56);
}

// The same game with the object and the block in step: no difference.
void TestAMatchingGameIsScaledTheSameWay()
{
    Session g;
    for (int i = 1; i <= 30; ++i)
    {
        const Outcome o = g.Next();
        CHECK(i < 5 ? o.sets.empty() : Wrote(o, g.reportedX * 0.6, g.reportedY * 0.6));
    }
}

// ---- classification ----------------------------------------------------------------------------------------------------------

void TestAConsistentGameIsNeverWritten()
{
    Session g;
    g.s = 2.0f; // applies exactly what it reports
    std::vector<std::string> all;
    for (int i = 0; i < 300; ++i)
    {
        const Outcome o = g.Next();
        all.insert(all.end(), o.logs.begin(), o.logs.end());
    }
    CHECK(g.e.setsEver == 0);
    CHECK(CountLines(all, "calibrated ratio") == 0);
    CHECK(std::count(all.begin(), all.end(),
                     "Jitter correction: the game already hands DLSS the jitter it applies (ratio 1.000/1.000 from 5 snapshots for " + kKey +
                         "); nothing is applied") == 1);
    // the pair stays the game's own
    CHECK(g.e.floats[Param::JitterX] == g.reportedX && g.e.floats[Param::JitterY] == g.reportedY);
}

void TestARefusedRatioIsNeverWrittenAndLoggedOnce()
{
    for (float s : { 4.0f, 0.4f, 2.1f }) // ratios 2.0, 0.2, 1.05
    {
        Session g;
        g.s = s;
        std::vector<std::string> all;
        for (int i = 0; i < 200; ++i)
        {
            const Outcome o = g.Next();
            all.insert(all.end(), o.logs.begin(), o.logs.end());
        }
        CHECK(g.e.setsEver == 0);
        CHECK(CountLines(all, "refused (a ratio above 1.02 or below 0.25 is not believed); nothing is applied, the game's values pass through") == 1);
        CHECK(CountLines(all, "calibrated ratio") == 0);
    }
    Session two;
    two.s = 4.0f;
    for (int i = 0; i < 5; ++i)
        two.Next();
    CHECK(two.e.setsEver == 0);
    // a ratio just inside the tolerance of 1 is consistent, not refused
    Session edge;
    edge.s = 2.0f * 1.015f;
    for (int i = 0; i < 20; ++i)
        edge.Next();
    CHECK(edge.e.setsEver == 0);
}

// ---- re-learning -------------------------------------------------------------------------------------------------------------

// A game that changes its ratio when the configuration changes (another preset): the old ratio is dropped, the evaluate that sees the new
// key passes the game's value through, five agreeing snapshots later the new ratio is applied.
void TestAKeyChangeIsRelearned()
{
    Session g;
    for (int i = 1; i <= 20; ++i)
        g.Next();
    CHECK(g.e.setsEver == 2 * 16);
    g.e.perfQuality = 3; // the key follows PerfQualityValue
    g.s = 1.6f;          // ratio 0.8 in the new configuration
    std::vector<std::string> logs;
    for (int i = 1; i <= 9; ++i)
    {
        const Outcome o = g.Next();
        logs.insert(logs.end(), o.logs.begin(), o.logs.end());
        if (i < 5)
            CHECK_MSG(o.sets.empty(), "evaluate %d of the new configuration was written", i); // the evaluate that sees the key passes through
        else
            CHECK_MSG(Wrote(o, g.reportedX * 0.8, g.reportedY * 0.8), "evaluate %d of the new configuration", i);
    }
    const std::string key3 = "render 1920x1080, output 3840x2160, quality 3";
    CHECK(CountLines(logs, ("configuration changed to " + key3 + "; calibration dropped, learning again").c_str()) == 1);
    CHECK(CountLines(logs, ("calibrated ratio 0.800/0.800 from 5 snapshots for " + key3 + "; scaling the reported jitter").c_str()) == 1);
    // back to the first configuration: dropped again (one slot, no cache), learned again at 0.6
    g.e.perfQuality = 2;
    g.s = 1.2f;
    for (int i = 1; i <= 9; ++i)
    {
        const Outcome o = g.Next();
        CHECK(i < 5 ? o.sets.empty() : Wrote(o, g.reportedX * 0.6, g.reportedY * 0.6));
    }
}

// Every field of the key counts; a second, differently sized evaluate in the same frame flips the key at every evaluate: inert, fail open.
void TestAFlippingKeyStaysInert()
{
    Session g;
    int lines = 0;
    for (int i = 0; i < 200; ++i)
    {
        g.e.perfQuality = i % 2 == 0 ? 2 : 3;
        const Outcome o = g.Next();
        lines += CountLines(o.logs, "configuration changed to");
    }
    CHECK(g.e.setsEver == 0);
    CHECK(lines == static_cast<int>(EpisodeLog::kMaxLines)); // the 32-line budget holds
}

// The ratio of a game can change within one configuration: five contradicting snapshots replace it.
void TestARatioThatChangesIsReplacedAfterFiveContradictions()
{
    Session g;
    g.sequence = false;
    for (int i = 1; i <= 10; ++i)
        g.Next();
    g.s = 1.6f; // ratio 0.8 from now on, same configuration
    std::vector<std::string> logs;
    for (int i = 1; i <= 8; ++i)
    {
        const Outcome o = g.Next();
        logs.insert(logs.end(), o.logs.begin(), o.logs.end());
        // four contradicting snapshots change nothing; the fifth replaces the ratio and is scaled with the new one at once
        CHECK_MSG(i < 5 ? Wrote(o, 0.18, -0.12) : Wrote(o, 0.24, -0.16), "evaluate %d after the change", i);
    }
    CHECK(CountLines(logs, ("Jitter correction: ratio changed after 5 contradicting snapshots: was 0.600, now 0.800/0.800 for " + kKey +
                            "; scaling the reported jitter").c_str()) == 1);
    // the read-back line comes again for the new calibration, and only for its first pair
    CHECK(CountLines(logs, "first scaled pair of this calibration: reported (0.3000,-0.2000) scaled to (0.2400,-0.1600) by ratio 0.800") == 1);
}

// ---- torn snapshots, failed reads, stale verification ------------------------------------------------------------------------------

void TestATornSnapshotDoesNotDisturbAnAdoptedRatio()
{
    Session g;
    for (int i = 1; i <= 10; ++i)
        g.Next();
    for (int bad = 0; bad < 3; ++bad)
    {
        // one object whose NDC belongs to another jitter (the frame it lags): ratio 0.9 instead of 0.6
        Frame f = g.Make();
        f.ndcFrom = std::make_pair(f.jx * 1.5f, f.jy * 1.5f);
        const Outcome o = Evaluate(g.e, g.engine, f, false, true);
        CHECK(Wrote(o, g.reportedX * 0.6, g.reportedY * 0.6)); // scaled with the adopted ratio, this very evaluate too
        for (int i = 0; i < 6; ++i)
            CHECK(ScaledNext(g, 0.6));
    }
    // the ratio is still 0.6, the three outliers were counted as disagreements, nothing was replaced
    g.t = 10000;
    const std::string status = StatusOf(g.Next());
    CHECK_MSG(Has(status, "state: calibrated ratio 0.600 for") && Has(status, "3 disagreed with the ratio, 0 ratio changes"), "%s", status.c_str());
}

void TestFailedReadsKeepTheRatioAndStaleVerificationIsLoggedOnce()
{
    Session g;
    g.stepMs = 700;
    for (int i = 1; i <= 5; ++i)
        g.Next();
    CHECK(g.e.setsEver == 2);
    const std::uint64_t pointer = kBase + kCurrentBuild.jitter.cameraGlobalRva;
    g.e.mem.PutU64(pointer, 0); // the camera global is gone
    std::vector<std::string> logs;
    int scaled = 0, staleAt = -1;
    for (int i = 1; i <= 30; ++i) // 21 seconds of failed reads
    {
        const Outcome o = g.Next();
        logs.insert(logs.end(), o.logs.begin(), o.logs.end());
        scaled += Wrote(o, g.reportedX * 0.6, g.reportedY * 0.6) ? 1 : 0;
        if (staleAt < 0 && CountLines(logs, "verification stale") > 0)
            staleAt = i;
    }
    CHECK_MSG(scaled == 30, "%d of 30 evaluates scaled while the object could not be read", scaled); // a failed read never disables a ratio
    CHECK_MSG(staleAt == 15, "stale logged at evaluate %d", staleAt);                              // 15 x 700 ms = 10.5 s since the last accepted snapshot
    CHECK(CountLines(logs, "Jitter correction: verification stale, no usable jitter snapshot for 10 s; keeping the current state") == 1);
    CHECK(CountLines(logs, "arriving again") == 0);
    g.t += 20000;
    const std::string status = StatusOf(g.Next());
    CHECK_MSG(Has(status, "verification stale") && Has(status, "state: calibrated ratio 0.600 for " + kKey), "%s", status.c_str());
    // the object can be read again: one line, and the status says fresh
    g.e.mem.PutU64(pointer, kGlobal);
    const Outcome back = g.Next();
    CHECK(CountLines(back.logs, "Jitter correction: jitter snapshots are arriving again, verification is fresh") == 1);
    CHECK(Wrote(back, g.reportedX * 0.6, g.reportedY * 0.6));
    CHECK(CountLines(g.Next().logs, "arriving again") == 0);
    g.t += 10000;
    CHECK(Has(StatusOf(g.Next()), "verification fresh"));
}

void TestABuildMismatchStaysIdle()
{
    Session g;
    g.e.build = nullptr;
    for (int i = 0; i < 20; ++i)
        g.Next();
    CHECK(g.e.setsEver == 0 && g.e.mem.reads == 0 && g.e.ops.empty());
    CHECK(g.e.logs.size() == 1 && Has(g.e.logs[0], "idle for this session"));
}

// ---- switching the correction off and on -----------------------------------------------------------------------------------------

void TestSwitchingTheCorrectionOffAndOnStartsLearningAgain()
{
    Session g;
    for (int i = 1; i <= 8; ++i)
        g.Next();
    CHECK(g.e.setsEver == 2 * 4);
    g.correct = false;
    g.probe = true; // the probe alone is on: the calibration is dropped
    g.Next();
    g.probe = false;
    g.correct = true;
    std::vector<std::string> logs;
    for (int i = 1; i <= 9; ++i)
    {
        const Outcome o = g.Next();
        logs.insert(logs.end(), o.logs.begin(), o.logs.end());
        CHECK_MSG(i < 5 ? o.sets.empty() : Wrote(o, g.reportedX * 0.6, g.reportedY * 0.6), "evaluate %d after switching on", i);
    }
    CHECK(CountLines(logs, ("learning the game's jitter ratio for " + kKey).c_str()) == 1);
    CHECK(CountLines(logs, "calibrated ratio 0.600/0.600") == 1);
}

// ---- exceptions ---------------------------------------------------------------------------------------------------------------------

void TestAnExceptionMidLearningDropsTheRunButAnAdoptedRatioSurvives()
{
    {
        Session g;
        for (int i = 1; i <= 3; ++i)
            g.Next();
        g.e.fault = [](const char* where)
        {
            if (std::string(where) == "GetFloat")
                throw std::runtime_error("block exploded");
        };
        g.Next(); // the step is abandoned: the run of 3 is dropped
        g.e.fault = nullptr;
        for (int i = 1; i <= 8; ++i)
        {
            const Outcome o = g.Next();
            // 4 snapshots after the exception are not 5; the run (3 + 4 = 7) must not be adopted early
            CHECK_MSG(i < 5 ? o.sets.empty() : Wrote(o, g.reportedX * 0.6, g.reportedY * 0.6), "evaluate %d after the exception", i);
        }
    }
    {
        Session g;
        for (int i = 1; i <= 8; ++i)
            g.Next();
        g.e.fault = [](const char* where)
        {
            if (std::string(where) == "GetFloat")
                throw std::runtime_error("block exploded");
        };
        g.Next();
        CHECK(CountLines(g.e.logs, "step abandoned after an exception") == 1);
        g.e.fault = nullptr;
        for (int i = 0; i < 5; ++i)
            CHECK(ScaledNext(g, 0.6)); // the ratio survived, no new learning
        // and a contradiction candidate does not survive it
        g.s = 1.6f;
        for (int i = 0; i < 4; ++i)
            g.Next();
        g.e.fault = [](const char* where)
        {
            if (std::string(where) == "GetFloat")
                throw std::runtime_error("again");
        };
        g.Next();
        g.e.fault = nullptr;
        for (int i = 0; i < 4; ++i)
            CHECK(ScaledNext(g, 0.6)); // 4 + abandon + 4 contradictions: still 0.6
        CHECK(ScaledNext(g, 0.8));     // the fifth after the abandon replaces it
    }
}

// ---- the write: X then Y, put-back, and what is remembered ------------------------------------------------------------------------

void TestAHalfWrittenPairIsPutBackAndWhatTheBlockHoldsIsRemembered()
{
    auto prime = [](Session& g) {
        g.sequence = false;
        g.refresh = false; // from the second evaluate on the game does NOT refresh the block: it keeps what we wrote
        for (int i = 1; i <= 4; ++i)
            g.Next();
        CHECK(g.e.setsEver == 0);
    };
    auto exceptions = [](const Session& g) { return CountLines(g.e.logs, "step abandoned after an exception"); };

    {
        // Y refused, the put-back of X works: the block holds the game's raw pair again and nothing is remembered, so the block that is
        // not refreshed is scaled ONCE at the next evaluate
        Session g;
        prime(g);
        int sets = 0;
        g.e.fault = [&sets](const char* where)
        {
            if (std::string(where) == "SetFloat" && ++sets == 2)
                throw std::runtime_error("Y refused");
        };
        const Outcome o = g.Next();
        CHECK(exceptions(g) == 1 && g.e.lockDepth == 0);
        CHECK(o.sets.size() == 2 && Near(o.sets[0].value, 0.18) && o.sets[1].param == Param::JitterX && Near(o.sets[1].value, 0.3)); // X, then X put back (the refused Y is not recorded)
        CHECK(Near(g.e.floats[Param::JitterX], 0.3) && Near(g.e.floats[Param::JitterY], -0.2));
        g.e.fault = nullptr;
        const Outcome next = g.Next();
        CHECK_MSG(Wrote(next, 0.18, -0.12), "the raw block was not scaled once (%zu sets)", next.sets.size());
        CHECK(Near(g.e.floats[Param::JitterX], 0.18) && Near(g.e.floats[Param::JitterY], -0.12));
        CHECK(g.Next().sets.empty()); // and then it is already scaled: never x * r^2
    }
    {
        // Y refused AND the put-back fails: the block holds (x*r, y raw); that exact pair is remembered, so the next evaluate does not scale
        // X a second time (fail open: Y stays raw)
        Session g;
        prime(g);
        int sets = 0;
        g.e.fault = [&sets](const char* where)
        {
            if (std::string(where) == "SetFloat" && ++sets >= 2)
                throw std::runtime_error("refused");
        };
        g.Next();
        CHECK(exceptions(g) == 1);
        CHECK(Near(g.e.floats[Param::JitterX], 0.18) && Near(g.e.floats[Param::JitterY], -0.2));
        g.e.fault = nullptr;
        for (int i = 0; i < 5; ++i)
        {
            const Outcome next = g.Next();
            CHECK_MSG(next.sets.empty(), "evaluate %d scaled the mixed pair again", i);
        }
        CHECK(Near(g.e.floats[Param::JitterX], 0.18) && Near(g.e.floats[Param::JitterY], -0.2));
    }
    {
        // X refused: nothing was written, nothing is put back, nothing is remembered
        Session g;
        prime(g);
        g.e.fault = [](const char* where)
        {
            if (std::string(where) == "SetFloat")
                throw std::runtime_error("X refused");
        };
        const Outcome o = g.Next();
        CHECK(o.sets.empty() && exceptions(g) == 1);
        CHECK(Near(g.e.floats[Param::JitterX], 0.3) && Near(g.e.floats[Param::JitterY], -0.2));
        g.e.fault = nullptr;
        CHECK(Wrote(g.Next(), 0.18, -0.12));
    }
    {
        // a pair that was written completely stays remembered across a step that throws later (the block still holds it)
        Session g;
        prime(g);
        g.Next(); // scaled
        g.e.fault = [](const char* where)
        {
            if (std::string(where) == "GetFloat")
                throw std::runtime_error("read");
        };
        g.Next();
        g.e.fault = nullptr;
        CHECK(g.Next().sets.empty() && Near(g.e.floats[Param::JitterX], 0.18));
    }
    for (int variant = 0; variant < 2; ++variant)
    {
        // Y refused and the put-back works: the block holds the game's pair again and NOTHING is remembered. A pair remembered by mistake
        // could match by accident: the game's next raw pair equals what the failed evaluate tried to write (variant 0: x*r with the raw Y;
        // variant 1: x*r with y*r). That is a new value of the game and it is scaled.
        Session g;
        prime(g);
        int sets = 0;
        g.e.fault = [&sets](const char* where)
        {
            if (std::string(where) == "SetFloat" && ++sets == 2)
                throw std::runtime_error("Y refused");
        };
        g.Next();
        g.e.fault = nullptr;
        CHECK(g.e.attempted.size() >= 3);
        const float triedX = g.e.attempted[0].value, triedY = g.e.attempted[1].value;
        Frame next = g.Make();
        next.jx = triedX;
        next.jy = variant == 0 ? -0.2f : triedY;
        next.noRefresh = false;
        const Outcome o = Evaluate(g.e, g.engine, next, false, true);
        CHECK_MSG(Wrote(o, next.jx * 0.6, next.jy * 0.6), "variant %d: %zu sets", variant, o.sets.size());
    }
}

// A new ratio forgets the remembered pair (R6): the block of a game that never refreshes it holds the pair written with the OLD ratio, and
// it is scaled once more with the new one. (The price of forgetting; keeping the pair would hold the block at the old ratio instead.)
void TestARatioChangeForgetsTheRememberedPair()
{
    Session g;
    g.sequence = false;
    g.refresh = false;
    for (int i = 1; i <= 4; ++i)
        g.Next();
    CHECK(Wrote(g.Next(), 0.18, -0.12)); // evaluate 5: adopted, the block holds the game's pair of evaluate 1
    g.s = 1.6f;                          // the game's ratio becomes 0.8
    for (int i = 6; i <= 9; ++i)
        CHECK_MSG(g.Next().sets.empty(), "evaluate %d: already scaled, the contradiction is not yet a new ratio", i);
    const Outcome o = g.Next(); // the fifth contradicting snapshot: the ratio is replaced, the remembered pair is forgotten
    CHECK_MSG(Wrote(o, 0.18 * 0.8, -0.12 * 0.8), "%zu sets", o.sets.size());
    CHECK(g.Next().sets.empty()); // and the new result is remembered
}

// Reads of the NGX pair come before the first write; the write is X then Y; the read-back belongs to the first pair of a calibration only.
void TestReadsComeBeforeTheWriteAndTheReadBackIsOnceAndShowsTheBlock()
{
    Session g;
    g.sequence = false;
    for (int i = 1; i <= 4; ++i)
        g.Next();
    const Outcome first = g.Next();
    CHECK(Wrote(first, 0.18, -0.12));
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
    std::vector<Op> after;
    for (std::size_t i = firstSet; i < first.ops.size(); ++i)
        if (!first.ops[i].set)
            after.push_back(first.ops[i]);
    CHECK(after.size() == 2 && after[0].param == Param::JitterX && after[1].param == Param::JitterY); // the read-back, X then Y, nothing else
    CHECK(CountLines(first.logs, "Jitter correction: first scaled pair of this calibration: reported (0.3000,-0.2000) scaled to (0.1800,-0.1200) by ratio 0.600; parameter block read back: (0.1800,-0.1200)") == 1);
    const Outcome second = g.Next();
    CHECK(Wrote(second, 0.18, -0.12));
    int jitterReads = 0;
    for (const Op& op : second.ops)
        jitterReads += !op.set && (op.param == Param::JitterX || op.param == Param::JitterY) ? 1 : 0;
    CHECK(jitterReads == 2); // the two reads at the start of the evaluate: no read-back
    CHECK(CountLines(second.logs, "first scaled pair") == 0);

    // a block that does not take the write: the line shows what it holds
    Session d;
    d.sequence = false;
    d.e.dropWrites = true;
    for (int i = 1; i <= 4; ++i)
        d.Next();
    CHECK(CountLines(d.Next().logs, "scaled to (0.1800,-0.1200) by ratio 0.600; parameter block read back: (0.3000,-0.2000)") == 1);
    Session u;
    u.sequence = false;
    u.e.eraseOnWrite = true;
    for (int i = 1; i <= 4; ++i)
        u.Next();
    CHECK(CountLines(u.Next().logs, "parameter block read back: unavailable") == 1);
}

// ---- the stale-block guard ----------------------------------------------------------------------------------------------------------

void TestAGameThatNeverRefreshesTheBlockIsScaledOnce()
{
    Session g;
    g.sequence = false;
    g.refresh = false;
    for (int i = 1; i <= 50; ++i)
        g.Next();
    CHECK(g.e.setsEver == 2); // one scaling, at the fifth evaluate
    CHECK(Near(g.e.floats[Param::JitterX], 0.18) && Near(g.e.floats[Param::JitterY], -0.12)); // x * r, never x * r^2
    g.t = 10000;
    const std::string status = StatusOf(g.Next());
    CHECK_MSG(Has(status, "51 evaluates seen, 1 scaled, 4 passed through while learning, 0 passed through because the jitter pair was not usable, 46 already scaled (block not refreshed)"), "%s", status.c_str());
}

void TestAGameThatRefreshesEveryEvaluateIsScaledEveryTime()
{
    Session g;
    for (int i = 1; i <= 40; ++i)
    {
        const Outcome o = g.Next();
        CHECK(i < 5 ? o.sets.empty() : Wrote(o, g.reportedX * 0.6, g.reportedY * 0.6));
    }
    g.t = 10000;
    const std::string status = StatusOf(g.Next());
    CHECK_MSG(Has(status, "37 scaled, ") && Has(status, " 0 already scaled (block not refreshed)"), "%s", status.c_str());
}

void TestAGameThatRefreshesEverySecondEvaluate()
{
    Session g;
    g.sequence = false;
    // the block is refreshed on even evaluates only
    int scaled = 0, skipped = 0;
    for (int i = 1; i <= 40; ++i)
    {
        Frame f = g.Make();
        f.noRefresh = i > 1 && i % 2 == 1;
        const Outcome o = Evaluate(g.e, g.engine, f, false, true);
        if (i < 5)
            CHECK(o.sets.empty());
        else if (i == 5)
            CHECK(Wrote(o, 0.18, -0.12)); // the block holds the game's pair of evaluate 4: scaled
        else if (i % 2 == 0)
            CHECK_MSG(Wrote(o, 0.18, -0.12), "evaluate %d (refreshed)", i);
        else
            CHECK_MSG(o.sets.empty(), "evaluate %d (not refreshed) was scaled again", i);
        scaled += o.sets.empty() ? 0 : 1;
        skipped += o.sets.empty() ? 1 : 0;
    }
    CHECK(scaled == 1 + 18 && skipped == 4 + 17);
    CHECK(Near(g.e.floats[Param::JitterX], 0.18) && Near(g.e.floats[Param::JitterY], -0.12));
}

void TestTheSeamRunningTwicePerEvaluateScalesOnce()
{
    Session g;
    g.sequence = false;
    for (int i = 1; i <= 4; ++i)
        g.Next();
    for (int i = 0; i < 10; ++i)
    {
        Frame f = g.Make();
        const Outcome first = Evaluate(g.e, g.engine, f, false, true);
        Frame again = f;
        again.noRefresh = true; // the same evaluate, seam called a second time: the block holds our result
        const Outcome second = Evaluate(g.e, g.engine, again, false, true);
        if (g.n >= 5)
        {
            CHECK(Wrote(first, 0.18, -0.12));
            CHECK_MSG(second.sets.empty(), "evaluate %u: second seam call scaled again", g.n);
            CHECK(Near(g.e.floats[Param::JitterX], 0.18) && Near(g.e.floats[Param::JitterY], -0.12));
        }
    }
}

// A new calibration forgets the remembered pair: a ratio change and a key change both scale a block that happens to hold the same numbers.
void TestTheRememberedPairIsForgottenWithTheRatio()
{
    Session g;
    g.sequence = false;
    for (int i = 1; i <= 8; ++i)
        g.Next(); // adopted, the game refreshes (0.3, -0.2)
    // the key changes: the calibration and the remembered pair go; the game stops refreshing, the block holds our last (0.18, -0.12)
    g.e.perfQuality = 3;
    g.refresh = false;
    for (int i = 1; i <= 4; ++i)
        CHECK(g.Next().sets.empty());
    // the fifth snapshot of the new key adopts 0.6 again: the block holds 0.18/-0.12 which is NOT the remembered pair any more, so the
    // evaluate scales it (the price of forgetting: once more); had the pair been kept, it would be recognised as already scaled
    const Outcome o = g.Next();
    CHECK_MSG(Wrote(o, 0.18 * 0.6, -0.12 * 0.6), "%zu sets", o.sets.size());
}

// ---- pairs that are not usable --------------------------------------------------------------------------------------------------------

void TestAPairThatIsNotUsableIsPassedThroughAndLoggedPerEpisode()
{
    Session g;
    g.sequence = false;
    for (int i = 1; i <= 8; ++i)
        g.Next();
    std::vector<std::string> logs;
    auto frame = [&](auto spoil)
    {
        Frame f = g.Make();
        spoil(f);
        const Outcome o = Evaluate(g.e, g.engine, f, false, true);
        logs.insert(logs.end(), o.logs.begin(), o.logs.end());
        return o;
    };
    for (int i = 0; i < 3; ++i)
        CHECK(frame([](Frame& f) { f.ngxMissing = true; }).sets.empty());
    CHECK(Wrote(frame([](Frame&) {}), 0.18, -0.12));
    for (int i = 0; i < 3; ++i)
        CHECK(frame([](Frame& f) { f.ngxJx = 0.6f; }).sets.empty()); // above the 0.55 limit
    CHECK(frame([](Frame& f) { f.ngxJy = std::nanf(""); }).sets.empty());
    CHECK(Wrote(frame([](Frame&) {}), 0.18, -0.12));
    CHECK(CountLines(logs, "Jitter correction: jitter pair not usable, game values passed through unchanged: NGX jitter parameters not readable") == 1);
    CHECK(CountLines(logs, "Jitter correction: jitter pair not usable, game values passed through unchanged: NGX jitter value above 0.55 pixel") == 1);
    g.t = 10000;
    CHECK(Has(StatusOf(g.Next()), "7 passed through because the jitter pair was not usable"));
}

// ---- the probe next to the correction ----------------------------------------------------------------------------------------------------

// The probe line says what NGX was told BEFORE the write: with both keys on and a scaled write in the same evaluate it still reads "NGX saw
// the same values".
void TestTheProbeLineShowsTheValuesBeforeTheWrite()
{
    Session g;
    g.sequence = false;
    g.probe = true;
    for (int i = 1; i <= 6; ++i)
        g.Next();
    g.t = 20000;
    const Outcome o = g.Next();
    CHECK(Wrote(o, 0.18, -0.12));
    bool seen = false;
    for (const std::string& line : o.logs)
        if (Has(line, "Game jitter: counter"))
        {
            seen = true;
            CHECK(Has(line, "NGX saw the same values") && Has(line, "reported jx/jy 0.3000/-0.2000") && Has(line, "applied ax/ay 0.1800/-0.1200"));
        }
    CHECK(seen);
}

// ---- the lines -------------------------------------------------------------------------------------------------------------------------

void TestTheLinesOfASession()
{
    Session g;
    g.sequence = false;
    const Outcome first = g.Next();
    CHECK(first.logs.size() == 2);
    if (first.logs.size() == 2)
    {
        CHECK(first.logs[0] == "Jitter correction: learning the game's jitter ratio for " + kKey + "; the game's values pass through until 5 agreeing snapshots are seen");
        CHECK(first.logs[1] ==
              "Jitter correction: on; totals since the start of the session: 1 evaluates seen, 0 scaled, 1 passed through while learning, 0 passed "
              "through because the jitter pair was not usable, 0 already scaled (block not refreshed), 0 left alone (nothing to apply); "
              "state: learning for " + kKey + "; snapshots: 1 accepted, 0 disagreed with the ratio, 0 ratio changes, 0 restarts after a "
              "configuration change; verification fresh; frame generation active no (input Upscaler)");
    }
    for (int i = 2; i <= 4; ++i)
        CHECK(g.Next().logs.empty());
    const Outcome fifth = g.Next();
    CHECK(fifth.logs.size() == 2);
    if (fifth.logs.size() == 2)
    {
        CHECK(fifth.logs[0] == "Jitter correction: calibrated ratio 0.600/0.600 from 5 snapshots for " + kKey + "; scaling the reported jitter");
        CHECK(fifth.logs[1] == "Jitter correction: first scaled pair of this calibration: reported (0.3000,-0.2000) scaled to (0.1800,-0.1200) by ratio 0.600; parameter block read back: (0.1800,-0.1200)");
    }
    for (int i = 6; i <= 20; ++i)
        CHECK(g.Next().logs.empty());
    g.t = 10000;
    const Outcome tick = g.Next();
    CHECK(tick.logs.size() == 1);
    CHECK(StatusOf(tick) ==
          "Jitter correction: on; totals since the start of the session: 21 evaluates seen, 17 scaled, 4 passed through while learning, 0 passed "
          "through because the jitter pair was not usable, 0 already scaled (block not refreshed), 0 left alone (nothing to apply); "
          "state: calibrated ratio 0.600 for " + kKey + "; snapshots: 21 accepted, 0 disagreed with the ratio, 0 ratio changes, 0 restarts "
          "after a configuration change; verification fresh; frame generation active no (input Upscaler)");

    // frame generation and its input are named (only the Upscaler input reads the pair from the block)
    for (int mode = 0; mode < 4; ++mode)
    {
        Session h;
        h.e.frameGeneration = mode == 0 ? std::optional<bool> { true } : mode == 1 ? std::optional<bool> { false } : mode == 2 ? std::nullopt : std::optional<bool> { true };
        const char* inputs[] = { "Upscaler", "NoFG", "", "DLSSG" };
        const char* expected[] = { "frame generation active yes (input Upscaler)", "frame generation active no (input NoFG)", "frame generation active unknown",
                                   "frame generation active yes (input DLSSG)" };
        h.e.frameGenerationInput = inputs[mode];
        const std::string line = StatusOf(h.Next());
        CHECK(Has(line, expected[mode]) && (mode != 2 || !Has(line, "(input")));
        CHECK(line.size() > 100 && line.compare(line.size() - std::string(expected[mode]).size(), std::string::npos, expected[mode]) == 0);
    }
}

void TestOtherStatesAreNamedInTheStatusLine()
{
    {
        Session g;
        g.sequence = false;
        g.s = 2.0f;
        for (int i = 1; i <= 6; ++i)
            g.Next();
        g.t = 10000;
        const std::string status = StatusOf(g.Next());
        CHECK_MSG(Has(status, "state: consistent (ratio 1.000), nothing applied, for " + kKey) && Has(status, "0 scaled, 4 passed through while learning") &&
                      Has(status, "3 left alone (nothing to apply)"), "%s", status.c_str());
    }
    {
        Session g;
        g.sequence = false;
        g.s = 4.0f;
        for (int i = 1; i <= 6; ++i)
            g.Next();
        g.t = 10000;
        CHECK(Has(StatusOf(g.Next()), "state: refused (ratio 2.000), nothing applied, for " + kKey));
    }
    {
        Session g;
        g.sequence = false;
        for (int i = 1; i <= 6; ++i)
            g.Next();
        g.e.perfQuality = 3;
        g.t = 10000;
        const std::string status = StatusOf(g.Next());
        CHECK_MSG(Has(status, "state: dropped after a configuration change, learning again for render 1920x1080, output 3840x2160, quality 3") &&
                      Has(status, "1 restarts after a configuration change"), "%s", status.c_str());
    }
}

// The 32-line budget of every kind of episode, and the report of what was not logged.
void TestNewEpisodesAreCountedInTheReport()
{
    Session g;
    g.sequence = false;
    // configuration flips: 100 changes of key, each a restart; the report carries the calibration events that had no line left
    for (int i = 0; i < 100; ++i)
    {
        g.e.perfQuality = i % 2 == 0 ? 2 : 3;
        g.Next();
    }
    g.t = 100000;
    std::string report;
    for (int i = 0; i < 6; ++i)
    {
        g.t += 10000;
        for (const std::string& line : g.Next().logs)
            if (Has(line, "log line budget used up"))
                report = line;
    }
    CHECK_MSG(Has(report, "0 probe failures, 0 pass-throughs, 0 replacements, 0 abandoned steps, 67 calibration events"), "%s", report.c_str());
}
} // namespace

int main()
{
    TestTheObjectNeverMatchesNgxAndEveryEvaluateIsScaled();
    TestAMatchingGameIsScaledTheSameWay();
    TestAConsistentGameIsNeverWritten();
    TestARefusedRatioIsNeverWrittenAndLoggedOnce();
    TestAKeyChangeIsRelearned();
    TestAFlippingKeyStaysInert();
    TestARatioThatChangesIsReplacedAfterFiveContradictions();
    TestATornSnapshotDoesNotDisturbAnAdoptedRatio();
    TestFailedReadsKeepTheRatioAndStaleVerificationIsLoggedOnce();
    TestABuildMismatchStaysIdle();
    TestSwitchingTheCorrectionOffAndOnStartsLearningAgain();
    TestAnExceptionMidLearningDropsTheRunButAnAdoptedRatioSurvives();
    TestAHalfWrittenPairIsPutBackAndWhatTheBlockHoldsIsRemembered();
    TestARatioChangeForgetsTheRememberedPair();
    TestReadsComeBeforeTheWriteAndTheReadBackIsOnceAndShowsTheBlock();
    TestAGameThatNeverRefreshesTheBlockIsScaledOnce();
    TestAGameThatRefreshesEveryEvaluateIsScaledEveryTime();
    TestAGameThatRefreshesEverySecondEvaluate();
    TestTheSeamRunningTwicePerEvaluateScalesOnce();
    TestTheRememberedPairIsForgottenWithTheRatio();
    TestAPairThatIsNotUsableIsPassedThroughAndLoggedPerEpisode();
    TestTheProbeLineShowsTheValuesBeforeTheWrite();
    TestTheLinesOfASession();
    TestOtherStatesAreNamedInTheStatusLine();
    TestNewEpisodesAreCountedInTheReport();
    return TEST_RESULT("jitter_calibrated");
}
