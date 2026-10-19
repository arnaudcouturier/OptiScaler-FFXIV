#include "check.h"
#include "engine_fixture.h"
#include "ext/ffxiv/jitter/FfxivJitterCore.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace ext::ffxiv;
using namespace ext::ffxiv::jitter;
using namespace jitter_fakes;

// The decision and ordering core (FfxivJitterCore.h) against a fake parameter block, fake process memory, a fake clock and a fake log.
// The three rules the maintainer cares about most are pinned here, not by reading: nothing runs with both keys off, every read of the
// game's own values happens before the write, and a write happens only when every guard passed.

namespace
{
// Checks one evaluate's order: both Jitter.Offset reads come before the first write, and the write is exactly X then Y. After the write
// the only Jitter.Offset reads allowed are the read-back of the first replacement of an episode: X then Y, nothing else.
void CheckReadsThenWrite(const Outcome& o, double ax, double ay, bool readBack)
{
    CHECK(o.sets.size() == 2);
    if (o.sets.size() != 2)
        return;
    CHECK(o.sets[0].param == Param::JitterX && Near(o.sets[0].value, ax));
    CHECK(o.sets[1].param == Param::JitterY && Near(o.sets[1].value, ay));
    std::size_t firstSet = o.ops.size(), readX = o.ops.size(), readY = o.ops.size();
    for (std::size_t i = 0; i < o.ops.size(); ++i)
    {
        if (o.ops[i].set && firstSet == o.ops.size())
            firstSet = i;
        if (!o.ops[i].set && o.ops[i].param == Param::JitterX && readX == o.ops.size())
            readX = i;
        if (!o.ops[i].set && o.ops[i].param == Param::JitterY && readY == o.ops.size())
            readY = i;
    }
    CHECK(readX < firstSet && readY < firstSet);
    // after the write: the two Sets, then (episode start only) the read-back, and no other read of the jitter values at all
    std::vector<Op> after;
    for (std::size_t i = firstSet; i < o.ops.size(); ++i)
        if (!o.ops[i].set)
            after.push_back(o.ops[i]);
    if (readBack)
        CHECK(after.size() == 2 && after[0].param == Param::JitterX && after[1].param == Param::JitterY);
    else
        CHECK(after.empty());
    if (readBack && after.size() == 2)
        CHECK(o.ops[o.ops.size() - 1].param == Param::JitterY && o.ops[o.ops.size() - 2].param == Param::JitterX && !o.ops.back().set);
}

// (a) Both keys off: nothing at all happens.
void TestKeysOff()
{
    FakeEnv e = MakeEnv();
    Engine engine;
    for (int i = 0; i < 40; ++i)
    {
        Frame f;
        f.counter = 100 + i;
        f.timeMs = static_cast<std::uint64_t>(i) * 5000;
        const Outcome o = Evaluate(e, engine, f, false, false);
        CHECK(o.ops.empty() && o.logs.empty());
    }
    CHECK(e.ops.empty() && e.logs.empty());
    CHECK(e.mem.reads == 0);
    CHECK(e.locks == 0 && e.buildLookups == 0 && e.clockReads == 0 && e.qualityQueries == 0 && e.fgQueries == 0);
}

// (b) The correction on with a game whose applied jitter differs: every read of Jitter.Offset precedes the first write, exactly two
// writes, X then Y, with the applied values; the write happens at least once.
void TestCorrectionReadsBeforeItWrites()
{
    FakeEnv e = MakeEnv();
    Engine engine;
    int written = 0;
    for (int i = 0; i < 6; ++i)
    {
        Frame f; // s = 1: the game applies half of what it reports
        f.counter = 100 + i;
        f.timeMs = static_cast<std::uint64_t>(i) * 16;
        const Outcome o = Evaluate(e, engine, f, false, true);
        if (i == 0)
            CHECK(o.sets.empty()); // the first evaluate has no previous one to confirm it
        else
            CHECK(o.sets.size() == 2);
        if (!o.sets.empty())
        {
            ++written;
            CheckReadsThenWrite(o, 0.15, -0.1, i == 1); // the read-back belongs to the first replacement of the episode
        }
    }
    CHECK(written == 5);
    CHECK(e.locks == 6 && e.buildLookups == 6);
}

// M2: the pair is replaced only when the object's counter increased since the previous evaluate AND its ratio is the previous one's.
void TestSameFrameRules()
{
    using Flags = std::vector<bool>;
    {
        // a counter that stalls or goes back holds the value back; the first increase afterwards passes again
        FakeEnv e = MakeEnv();
        Engine engine;
        const Flags got = Replaced(e, engine, { At(100, 1, 0), At(101, 1, 16), At(101, 1, 32), At(102, 1, 48), At(100, 1, 64), At(101, 1, 80), At(102, 1, 96) });
        CHECK(got == (Flags { false, true, false, true, false, true, true }));
    }
    {
        // a ratio that changes between evaluates holds the value back; two evaluates with the same new ratio pass
        FakeEnv e = MakeEnv();
        Engine engine;
        const Flags got = Replaced(e, engine, { At(100, 1.0f, 0), At(101, 1.0f, 16), At(102, 1.5f, 32), At(103, 1.5f, 48), At(104, 1.0f, 64), At(105, 1.0f, 80) });
        CHECK(got == (Flags { false, true, false, true, false, true }));
    }
    {
        // a held-back evaluate leaves the game's values in the block (fail open) and does not touch anything else
        FakeEnv e = MakeEnv();
        Engine engine;
        Evaluate(e, engine, At(100, 1, 0), false, true);
        const Outcome held = Evaluate(e, engine, At(100, 1, 16), false, true); // the same counter again
        CHECK(held.sets.empty());
        CHECK(e.floats[Param::JitterX] == 0.3f && e.floats[Param::JitterY] == -0.2f);
    }
    {
        // a failed read forgets the previous evaluate: the one after the gap is a first frame again
        FakeEnv e = MakeEnv();
        Engine engine;
        Replaced(e, engine, { At(100, 1, 0), At(101, 1, 16) });
        const std::uint64_t pointer = kBase + kCurrentBuild.jitter.cameraGlobalRva;
        e.mem.PutU64(pointer, 0); // the camera global is gone for one evaluate
        Evaluate(e, engine, At(102, 1, 32), false, true);
        e.mem.PutU64(pointer, kGlobal);
        const Flags got = Replaced(e, engine, { At(103, 1, 48), At(104, 1, 64) });
        CHECK(got == (Flags { false, true }));
    }
    {
        // the NDC of the previous frame beside this frame's jitter (the equality proof passes, the ratios do not): nearly never replaced.
        // The sequence keeps its sign from frame to frame, so the sign guard cannot be what stops it.
        auto sequence = [](unsigned n, float& jx, float& jy)
        {
            auto fraction = [](double v) { return v - std::floor(v); };
            jx = 0.05f + 0.4f * static_cast<float>(fraction(n * 0.6180339887498949));
            jy = 0.05f + 0.4f * static_cast<float>(fraction(n * 0.7548776662466927));
        };
        FakeEnv e = MakeEnv();
        Engine engine;
        FakeEnv control = MakeEnv();
        Engine controlEngine;
        int lagged = 0, matched = 0;
        float px = 0, py = 0;
        for (unsigned n = 1; n <= 64; ++n)
        {
            float jx = 0, jy = 0;
            sequence(n, jx, jy);
            Frame f = At(static_cast<int>(n), 1.0f, n * 16ull, jx, jy);
            matched += Evaluate(control, controlEngine, f, false, true).sets.empty() ? 0 : 1;
            if (n > 1)
                f.ndcFrom = std::make_pair(px, py);
            lagged += Evaluate(e, engine, f, false, true).sets.empty() ? 0 : 1;
            px = jx;
            py = jy;
        }
        CHECK(matched == 63);
        CHECK(lagged == 0); // the sequence is deterministic: a lag of one frame is always caught, not "almost" always
    }
}
// L1: the first replacement of an episode asks the block what it holds and logs that next to the intended values.
void TestReadBackIsLoggedOnceAndShowsWhatTheBlockHolds()
{
    auto correctionLines = [](const Outcome& o)
    {
        std::vector<std::string> out;
        for (const std::string& line : o.logs)
            if (Has(line, "replaced by applied"))
                out.push_back(line);
        return out;
    };
    {
        FakeEnv e = MakeEnv();
        Engine engine;
        Evaluate(e, engine, At(100, 1, 0), false, true);
        const Outcome start = Evaluate(e, engine, At(101, 1, 100), false, true);
        CHECK(correctionLines(start).size() == 1 && Has(correctionLines(start)[0], "parameter block read back: (0.1500,-0.1000)"));
        const Outcome later = Evaluate(e, engine, At(102, 1, 200), false, true); // same episode: no line, no extra read
        CHECK(correctionLines(later).empty() && later.sets.size() == 2);
        // a pass-through ends the episode; the next replacement starts a new one with a new read-back
        Evaluate(e, engine, At(102, 1, 300), false, true); // counter did not increase
        const Outcome again = Evaluate(e, engine, At(103, 1, 400), false, true);
        CHECK(correctionLines(again).size() == 1 && Has(correctionLines(again)[0], "parameter block read back: (0.1500,-0.1000)"));
        CHECK(correctionLines(Evaluate(e, engine, At(104, 1, 500), false, true)).empty());
    }
    {
        // a block that does not take the write: the line shows the values it still holds, not the intended ones
        FakeEnv e = MakeEnv();
        e.dropWrites = true;
        Engine engine;
        Evaluate(e, engine, At(100, 1, 0), false, true);
        const Outcome start = Evaluate(e, engine, At(101, 1, 100), false, true);
        CHECK(correctionLines(start).size() == 1);
        CHECK(Has(correctionLines(start)[0], "applied (0.1500,-0.1000)") && Has(correctionLines(start)[0], "parameter block read back: (0.3000,-0.2000)"));
    }
    {
        // a block that returns nothing after the write: the line says so
        FakeEnv e = MakeEnv();
        e.eraseOnWrite = true;
        Engine engine;
        Evaluate(e, engine, At(100, 1, 0), false, true);
        const Outcome start = Evaluate(e, engine, At(101, 1, 100), false, true);
        CHECK(correctionLines(start).size() == 1 && Has(correctionLines(start)[0], "parameter block read back: unavailable"));
    }
}

// (e) The probe line says what NGX was told BEFORE the write. With both keys on and a write in the same evaluate, the sample line
// must still read "NGX saw the same values": a version that read the block after the write would see its own output.
void TestProbeLineShowsTheValuesBeforeTheWrite()
{
    FakeEnv e = MakeEnv();
    Engine engine;
    bool sampleSeen = false;
    for (int i = 0; i < 4; ++i)
    {
        Frame f;
        f.counter = 100 + i;
        f.timeMs = static_cast<std::uint64_t>(i) * 10000; // a sample is due at every evaluate
        const Outcome o = Evaluate(e, engine, f, true, true);
        if (o.sets.empty())
            continue;
        for (const std::string& line : o.logs)
            if (Has(line, "Game jitter: counter"))
            {
                sampleSeen = true;
                CHECK(Has(line, "NGX saw the same values"));
                CHECK(!Has(line, "NGX saw different values"));
                CHECK(Has(line, "reported jx/jy 0.3000/-0.2000"));
                CHECK(Has(line, "applied ax/ay 0.1500/-0.1000"));
            }
    }
    CHECK(sampleSeen);
}

// (d) The probe alone never writes, whatever the game does.
void TestProbeOnlyNeverWrites()
{
    FakeEnv e = MakeEnv();
    Engine engine;
    int samples = 0;
    for (int i = 0; i < 30; ++i)
    {
        Frame f; // a scaled game: the correction would write here
        f.counter = 100 + i;
        f.timeMs = static_cast<std::uint64_t>(i) * 4000;
        const Outcome o = Evaluate(e, engine, f, true, false);
        CHECK(o.sets.empty());
        for (const std::string& line : o.logs)
            samples += Has(line, "Game jitter: counter") ? 1 : 0;
    }
    CHECK(e.setsEver == 0);
    CHECK(samples >= 5); // the probe did run: about one sample per 10 s over 120 s
    CHECK(e.floats[Param::JitterX] == 0.3f && e.floats[Param::JitterY] == -0.2f); // the block still holds the game's values
}

// L2: with the probe alone the parameters are not even looked at between samples. The clock decides whether a sample can be due; only the
// configuration key (render size, preset) needs the parameters, and it is looked at on every 16th evaluate.
void TestProbeOnlyDoesNoLookupsBetweenSamples()
{
    constexpr std::size_t kLookups = 9; // Jitter X/Y, subrect W/H, Width, Height, OutWidth, OutHeight, PerfQualityValue
    FakeEnv e = MakeEnv();
    Engine engine;
    const Outcome first = Evaluate(e, engine, At(100, 1, 0), true, false);
    CHECK(first.ops.size() == kLookups);
    CHECK(first.logs.size() == 1 && Has(first.logs[0], "Game jitter: counter"));

    // the first 2 s: nothing at all is asked of the block, the process or the quality answer
    const int memoryReads = e.mem.reads, qualityQueries = e.qualityQueries;
    for (int i = 1; i <= 60; ++i)
    {
        const Outcome o = Evaluate(e, engine, At(100 + i, 1, static_cast<std::uint64_t>(i) * 30), true, false);
        CHECK(o.ops.empty() && o.logs.empty());
    }
    CHECK(e.mem.reads == memoryReads && e.qualityQueries == qualityQueries);

    // 2 s to 10 s, configuration unchanged: the parameters are read on every 16th evaluate only, and that is all. The cadence is pinned
    // exactly: the 60 evaluates above already counted (60 = 3 x 16 + 12, the counter stands at 12), so the first pass is the 4th evaluate
    // of this phase and the spacing is 16 (a key check every 15th or 17th evaluate gives a different first pass and a different spacing).
    int evaluates = 0;
    std::vector<int> passAt;
    for (std::uint64_t t = 2000; t < 10000; t += 100)
    {
        const Outcome o = Evaluate(e, engine, At(200 + evaluates, 1, t), true, false);
        ++evaluates;
        if (!o.ops.empty())
        {
            passAt.push_back(evaluates);
            CHECK(o.ops.size() == kLookups);
        }
        CHECK(o.logs.empty()); // and no sample: the configuration did not change
    }
    CHECK(evaluates == 80);
    CHECK(passAt == (std::vector<int> { 4, 20, 36, 52, 68 }));
    CHECK(e.mem.reads == memoryReads); // the process was not read either

    // the interval over: the very next evaluate samples
    const Outcome due = Evaluate(e, engine, At(300, 1, 10000), true, false);
    CHECK(due.ops.size() >= kLookups && due.logs.size() == 1 && Has(due.logs[0], "Game jitter: counter"));
}

// L2: a change of the render size is still sampled early, within 16 evaluates of the change (and not before the 2 s minimum).
void TestProbeOnlyStillSamplesAConfigurationChangeEarly()
{
    FakeEnv e = MakeEnv();
    Engine engine;
    Evaluate(e, engine, At(100, 1, 0), true, false);
    e.uints[Param::Width] = 1280; // the game changed its render size (Width/Height/subrect are what the key is made of)
    e.uints[Param::SubrectWidth] = 1280;
    int taken = 0;
    for (int i = 1; i <= 40; ++i)
    {
        // before 2 s since the last sample nothing can be taken, whatever changed
        const std::uint64_t t = i <= 10 ? static_cast<std::uint64_t>(i) * 100 : 3000 + static_cast<std::uint64_t>(i) * 10;
        const Outcome o = Evaluate(e, engine, At(100 + i, 1, t), true, false);
        if (!o.logs.empty())
        {
            ++taken;
            CHECK(t >= 2000);
            CHECK(i <= 10 + 16); // within 16 evaluates of the moment the 2 s have passed
            CHECK(Has(o.logs[0], "render 1280x1080") || Has(o.logs[0], "Width/Height 1280x1080"));
        }
    }
    CHECK(taken == 1);

    // with the correction on, nothing is skipped: every evaluate reads the parameters
    FakeEnv c = MakeEnv();
    Engine correction;
    for (int i = 0; i < 20; ++i)
        CHECK(Evaluate(c, correction, At(100 + i, 1, static_cast<std::uint64_t>(i) * 16), false, true).ops.size() >= 9);
}

// The sample line carries the context of THIS evaluate, field by field. Every parameter has its own value here, so reading a field from
// the wrong parameter (or leaving it out) shows in the text.
void TestSampleLineCarriesTheContextOfThisEvaluate()
{
    FakeEnv e = MakeEnv();
    e.uints[Param::SubrectWidth] = 1800;
    e.uints[Param::SubrectHeight] = 1000;
    e.uints[Param::Width] = 1920;
    e.uints[Param::Height] = 1080;
    e.uints[Param::OutWidth] = 3840;
    e.uints[Param::OutHeight] = 2160;
    e.perfQuality = 1;
    e.answer = { true, 3, 1280, 720, 2560, 1440 };
    Engine engine;
    const Outcome o = Evaluate(e, engine, At(100, 1, 0), true, false);
    CHECK(o.logs.size() == 1);
    CHECK(!o.logs.empty() &&
          Has(o.logs[0], "NGX subrect 1800x1000, Width/Height 1920x1080, OutWidth/OutHeight 3840x2160, PerfQualityValue 1; last forced-quality "
                         "answer: quality 3, render 1280x720, display 2560x1440"));

    // nothing answered yet and no quality parameter: n/a everywhere, never the value of another field
    FakeEnv none = MakeEnv();
    none.perfQuality.reset();
    Engine fresh;
    const Outcome blank = Evaluate(none, fresh, At(100, 1, 0), true, false);
    CHECK(blank.logs.size() == 1 && Has(blank.logs[0], "PerfQualityValue n/a; last forced-quality answer: quality n/a, render n/a, display n/a"));
}

// A change of any field of the configuration (render size, output size, preset, the answer the fork gave the game's quality query) is
// sampled early, inside the 2 s to 10 s window, and the new value is in the line that results.
void TestEveryConfigurationFieldIsSampledEarlyAndPrinted()
{
    struct Change
    {
        const char* name;
        std::function<void(FakeEnv&)> apply;
        const char* shownAs;
    };
    const std::vector<Change> changes = {
        { "subrect width", [](FakeEnv& e) { e.uints[Param::SubrectWidth] = 1600; }, "NGX subrect 1600x1080" },
        { "subrect height", [](FakeEnv& e) { e.uints[Param::SubrectHeight] = 900; }, "NGX subrect 1920x900" },
        { "width", [](FakeEnv& e) { e.uints[Param::Width] = 2000; }, "Width/Height 2000x1080" },
        { "height", [](FakeEnv& e) { e.uints[Param::Height] = 1100; }, "Width/Height 1920x1100" },
        { "output width", [](FakeEnv& e) { e.uints[Param::OutWidth] = 2560; }, "OutWidth/OutHeight 2560x2160" },
        { "output height", [](FakeEnv& e) { e.uints[Param::OutHeight] = 1440; }, "OutWidth/OutHeight 3840x1440" },
        { "PerfQualityValue", [](FakeEnv& e) { e.perfQuality = 3; }, "PerfQualityValue 3" },
        { "forced-quality answer", [](FakeEnv& e) { e.answer.quality = 4; }, "last forced-quality answer: quality 4" },
    };
    for (const Change& c : changes)
    {
        FakeEnv e = MakeEnv();
        e.answer = { true, 2, 1280, 720, 2560, 1440 };
        Engine engine;
        Evaluate(e, engine, At(100, 1, 0), true, false);
        c.apply(e);
        std::string line;
        for (int i = 1; i <= 40 && line.empty(); ++i)
        {
            const Outcome o = Evaluate(e, engine, At(100 + i, 1, 2000 + static_cast<std::uint64_t>(i) * 10), true, false);
            if (!o.logs.empty())
                line = o.logs[0];
        }
        if (!Has(line, c.shownAs))
            std::printf("  change not sampled or not shown: %s\n", c.name);
        CHECK(Has(line, c.shownAs));
    }
    // the control: nothing changed, nothing is sampled before the interval is over
    FakeEnv quiet = MakeEnv();
    Engine engine;
    Evaluate(quiet, engine, At(100, 1, 0), true, false);
    for (int i = 1; i <= 40; ++i)
        CHECK(Evaluate(quiet, engine, At(100 + i, 1, 2000 + static_cast<std::uint64_t>(i) * 10), true, false).logs.empty());
}

// M1 (fix round 2): an evaluate the proof REJECTED forgets the reference, like a failed read. The first replacement of a run needs two
// consecutive evaluates that PASSED the proof (read ok, trusted size, NGX equal, plausible, signs ok), not two that merely read the object.
void TestRejectedEvaluatesForgetTheReference()
{
    using Flags = std::vector<bool>;
    struct Case
    {
        const char* name;
        Guard guard; // what the rejected evaluate must be rejected for
        std::function<void(FakeEnv&, Frame&)> spoil;
        std::function<void(FakeEnv&)> restore;
    };
    const std::vector<Case> cases = {
        { "NGX jitter of another frame", Guard::ObjectNotThisEvaluate, [](FakeEnv&, Frame& f) { f.ngxJx = 0.29f; }, nullptr },
        { "NGX jitter missing", Guard::NgxJitterUnavailable, [](FakeEnv&, Frame& f) { f.ngxMissing = true; }, nullptr },
        { "untrusted render size", Guard::RenderSizeUnknown,
          [](FakeEnv& e, Frame&)
          {
              e.uints.erase(Param::Width);
              e.uints.erase(Param::Height);
          },
          [](FakeEnv& e)
          {
              e.uints[Param::Width] = 1920;
              e.uints[Param::Height] = 1080;
          } },
        { "applied jitter implausible", Guard::AppliedImplausible,
          [](FakeEnv&, Frame& f)
          {
              f.jx = 0.4f;
              f.jy = 0.4f;
              f.s = 8.0f;
          },
          nullptr },
        { "sign mismatch", Guard::SignMismatch, [](FakeEnv&, Frame& f) { f.flipNdcY = true; }, nullptr },
    };
    for (const Case& c : cases)
    {
        // good, REJECTED, good, good, good: the evaluate right after the rejection is a first frame again (held), the next ones pass
        FakeEnv e = MakeEnv();
        Engine engine;
        Flags got;
        got.push_back(!Evaluate(e, engine, At(100, 1, 0), false, true).sets.empty());
        Frame bad = At(101, 1, 16);
        c.spoil(e, bad);
        const Outcome rejected = Evaluate(e, engine, bad, false, true);
        got.push_back(!rejected.sets.empty());
        CHECK(CountLines(rejected.logs, GuardName(c.guard)) == 1); // rejected for the guard the case names
        if (c.restore)
            c.restore(e);
        got.push_back(!Evaluate(e, engine, At(102, 1, 32), false, true).sets.empty());
        got.push_back(!Evaluate(e, engine, At(103, 1, 48), false, true).sets.empty());
        got.push_back(!Evaluate(e, engine, At(104, 1, 64), false, true).sets.empty());
        if (got != (Flags { false, false, false, true, true }))
            std::printf("  case: %s\n", c.name);
        CHECK(got == (Flags { false, false, false, true, true }));
    }
    {
        // the review's reproduction: the very first evaluate carries the NGX jitter of another frame; the next one must pass through
        FakeEnv e = MakeEnv();
        Engine engine;
        Frame first = At(100, 1, 0);
        first.ngxJx = 0.29f;
        const Flags got = Replaced(e, engine, { first, At(101, 1, 16), At(102, 1, 32) });
        CHECK(got == (Flags { false, false, true }));
    }
    {
        // a second, differently sized DLSS evaluate in every game frame: it is rejected (its NGX jitter is not the object's) and
        // forgets, so the main feature is never replaced. That is the safe side (inert), shown in the status counts as first frames.
        FakeEnv e = MakeEnv();
        Engine engine;
        int replaced = 0, interferingLines = 0;
        for (int k = 0; k < 20; ++k)
        {
            replaced += Evaluate(e, engine, At(100 + k, 1, k * 32ull), false, true).sets.empty() ? 0 : 1; // the main feature
            e.uints[Param::Width] = e.uints[Param::SubrectWidth] = 1280;
            e.uints[Param::Height] = e.uints[Param::SubrectHeight] = 720;
            Frame second = At(100 + k, 1, k * 32ull + 16);
            second.ngxJx = 0.11f;
            second.ngxJy = 0.07f;
            const Outcome o = Evaluate(e, engine, second, false, true);
            replaced += o.sets.empty() ? 0 : 1;
            interferingLines += CountLines(o.logs, GuardName(Guard::ObjectNotThisEvaluate));
            e.uints[Param::Width] = e.uints[Param::SubrectWidth] = 1920;
            e.uints[Param::Height] = e.uints[Param::SubrectHeight] = 1080;
        }
        CHECK(replaced == 0);
        CHECK(interferingLines == 1); // one episode, one line
    }
}

// Every failed guard: no write, whatever else is true, and the evaluate is turned away for the guard the case NAMES (not merely by some
// rule: the first evaluate of every run is held by the frame rules anyway, so "no write" alone would pass for a wrong reason). Each case
// spoils exactly one thing; the same rig without the spoiler writes.
void TestEveryFailedGuardWritesNothing()
{
    const JitterBuild& b = kCurrentBuild.jitter;
    struct Case
    {
        const char* name;
        Guard expect;      // the guard the evaluate must be turned away for (None = nothing is wrong, nothing to replace)
        bool readFailure;  // the probe also reports it as a failure to read the object
        std::function<void(FakeEnv&, Frame&)> spoil;
    };
    const std::vector<Case> cases = {
        { "build mismatch", Guard::BuildMismatch, false, [](FakeEnv& e, Frame&) { e.build = nullptr; } },
        { "global unreadable", Guard::GlobalUnreadable, true, [&](FakeEnv& e, Frame&) { e.mem.regions.erase(kBase + b.cameraGlobalRva); } },
        { "global null", Guard::GlobalInvalid, true, [&](FakeEnv& e, Frame&) { e.mem.PutU64(kBase + b.cameraGlobalRva, 0); } },
        { "camera unreadable", Guard::CameraUnreadable, true, [&](FakeEnv& e, Frame&) { e.mem.PutU64(kBase + b.cameraGlobalRva, 0x5000000000ull); } },
        { "camera null", Guard::CameraInvalid, true, [&](FakeEnv& e, Frame&) { e.mem.PutU64(kGlobal + b.cameraOffset, 0); } },
        { "object pointer slot unreadable", Guard::ObjectPointerUnreadable, true,
          [&](FakeEnv& e, Frame&) { e.mem.regions.erase(kCamera + b.jitterPointerOffset); } },
        { "object pointer null", Guard::ObjectPointerInvalid, true, [&](FakeEnv& e, Frame&) { e.mem.PutU64(kCamera + b.jitterPointerOffset, 0); } },
        { "object unreadable", Guard::ObjectUnreadable, true,
          [&](FakeEnv& e, Frame&) { e.mem.PutU64(kCamera + b.jitterPointerOffset, 0x5000000000ull); } },
        { "object unstable", Guard::ObjectUnstable, true,
          [](FakeEnv& e, Frame&)
          {
              auto n = std::make_shared<int>(0);
              e.mem.beforeRead = [n](FakeMemory& m) { PutObject(m, { 0.3f, -0.2f, 0.00015f, 0.0001f, 5000 + (*n)++ }); };
          } },
        { "jitter out of range", Guard::JitterOutOfRange, true, [](FakeEnv&, Frame& f) { f.jx = 0.9f; } },
        { "ndc out of range", Guard::NdcOutOfRange, true,
          [](FakeEnv& e, Frame&)
          {
              // just above 1; with the 1 pixel cap also in the way, only the guard's NAME can tell which check stopped it
              e.mem.beforeRead = [](FakeMemory& m) { PutObject(m, { 0.3f, -0.2f, 1.5f, 0.0001f, 5000 }); };
          } },
        { "NGX jitter missing", Guard::NgxJitterUnavailable, false, [](FakeEnv&, Frame& f) { f.ngxMissing = true; } },
        { "only Jitter.Offset.X missing", Guard::NgxJitterUnavailable, false, [](FakeEnv&, Frame& f) { f.ngxMissingX = true; } },
        { "only Jitter.Offset.Y missing", Guard::NgxJitterUnavailable, false, [](FakeEnv&, Frame& f) { f.ngxMissingY = true; } },
        { "NGX jitter is NaN", Guard::NgxJitterUnavailable, false, [](FakeEnv&, Frame& f) { f.ngxJx = std::nanf(""); } },
        { "object belongs to another frame", Guard::ObjectNotThisEvaluate, false, [](FakeEnv&, Frame& f) { f.ngxJx = 0.29f; } },
        { "render size unknown (no Width/Height)", Guard::RenderSizeUnknown, false,
          [](FakeEnv& e, Frame&)
          {
              e.uints.erase(Param::Width);
              e.uints.erase(Param::Height);
          } },
        { "Width present, Height missing", Guard::RenderSizeUnknown, false, [](FakeEnv& e, Frame&) { e.uints.erase(Param::Height); } },
        { "applied jitter implausible", Guard::AppliedImplausible, false,
          [](FakeEnv&, Frame& f)
          {
              f.jx = 0.4f;
              f.jy = 0.4f;
              f.s = 8.0f;
          } },
        { "sign mismatch", Guard::SignMismatch, false, [](FakeEnv&, Frame& f) { f.flipNdcY = true; } },
        { "game already consistent", Guard::None, false, [](FakeEnv&, Frame& f) { f.s = 2.0f; } },
        { "jitter too small to tell", Guard::None, false,
          [](FakeEnv&, Frame& f)
          {
              f.jx = 0.01f;
              f.jy = -0.01f;
          } },
    };
    const std::string passThroughMarker = "passed through unchanged, guard failed";
    for (const Case& c : cases)
        for (int probe = 0; probe < 2; ++probe)
        {
            FakeEnv e = MakeEnv();
            Engine engine;
            std::vector<std::string> all;
            for (int i = 0; i < 5; ++i)
            {
                Frame f;
                f.counter = 100 + i;
                f.timeMs = static_cast<std::uint64_t>(i) * 16;
                c.spoil(e, f); // the spoiler of the memory or the block is applied before each evaluate (Evaluate rewrites the object)
                const Outcome o = Evaluate(e, engine, f, probe != 0, true);
                all.insert(all.end(), o.logs.begin(), o.logs.end());
            }
            if (e.setsEver != 0)
                std::printf("  case that wrote: %s (probe %d)\n", c.name, probe);
            CHECK(e.setsEver == 0);

            const auto count = [&](const std::string& line) { return static_cast<int>(std::count(all.begin(), all.end(), line)); };
            int passThroughLines = 0;
            for (const std::string& line : all)
                passThroughLines += Has(line, passThroughMarker.c_str()) ? 1 : 0;
            int wrongReason = 0;
            if (c.expect == Guard::BuildMismatch)
            {
                // nothing is read at all; the one note names the guard
                wrongReason += CountLines(all, GuardName(Guard::BuildMismatch)) == 1 && passThroughLines == 0 ? 0 : 1;
            }
            else if (c.expect != Guard::None)
            {
                wrongReason += count(FormatPassThrough(c.expect)) == 1 && passThroughLines == 1 ? 0 : 1; // one episode, named for this guard
                if (probe != 0 && c.readFailure)
                    wrongReason += count(FormatProbeFailure(c.expect)) == 1 ? 0 : 1;
            }
            else
            {
                wrongReason += passThroughLines == 0 ? 0 : 1; // nothing was refused, there was simply nothing to replace
                if (probe != 0)
                    wrongReason += CountLines(all, std::string(c.name) == "game already consistent" ? "(consistent)" : "undetermined: jitter too small") == 1 ? 0 : 1;
            }
            if (wrongReason != 0)
            {
                std::printf("  case stopped for another reason: %s (probe %d)\n", c.name, probe);
                for (const std::string& line : all)
                    std::printf("    log: %s\n", line.c_str());
            }
            CHECK(wrongReason == 0);
        }

    // the control: the same rig with nothing spoiled does write, so the cases above are not passing for a reason of their own
    {
        FakeEnv e = MakeEnv();
        Engine engine;
        for (int i = 0; i < 5; ++i)
        {
            Frame f;
            f.counter = 100 + i;
            f.timeMs = static_cast<std::uint64_t>(i) * 16;
            Evaluate(e, engine, f, false, true);
        }
        CHECK(e.setsEver >= 2);
    }

    // a build mismatch reads nothing from the process, asks nothing of the block, and says so once
    {
        FakeEnv e = MakeEnv();
        e.build = nullptr;
        Engine engine;
        for (int i = 0; i < 5; ++i)
        {
            Frame f;
            f.counter = 100 + i;
            Evaluate(e, engine, f, true, true);
        }
        CHECK(e.mem.reads == 0 && e.ops.empty());
        CHECK(e.logs.size() == 1 && Has(e.logs[0], "idle for this session"));
    }
}
} // namespace

int main()
{
    TestKeysOff();
    TestCorrectionReadsBeforeItWrites();
    TestSameFrameRules();
    TestReadBackIsLoggedOnceAndShowsWhatTheBlockHolds();
    TestProbeLineShowsTheValuesBeforeTheWrite();
    TestProbeOnlyNeverWrites();
    TestProbeOnlyDoesNoLookupsBetweenSamples();
    TestProbeOnlyStillSamplesAConfigurationChangeEarly();
    TestSampleLineCarriesTheContextOfThisEvaluate();
    TestEveryConfigurationFieldIsSampledEarlyAndPrinted();
    TestRejectedEvaluatesForgetTheReference();
    TestEveryFailedGuardWritesNothing();
    return TEST_RESULT("jitter_glue");
}
