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

// (b) The correction on with a game whose applied jitter differs (ratio 0.6): once the ratio is calibrated (the fifth snapshot) every
// read of Jitter.Offset precedes the first write, exactly two writes, X then Y, with the scaled values; the read-back after the write
// belongs to the first pair of the calibration only.
void TestCorrectionReadsBeforeItWrites()
{
    FakeEnv e = MakeEnv();
    Engine engine;
    int written = 0;
    for (int i = 0; i < 10; ++i)
    {
        Frame f; // s = 1.2: the game applies 60% of what it reports
        f.s = 1.2f;
        f.counter = 100 + i;
        f.timeMs = static_cast<std::uint64_t>(i) * 16;
        const Outcome o = Evaluate(e, engine, f, false, true);
        if (i < 4)
            CHECK(o.sets.empty()); // still learning: the game's values pass through
        else
        {
            ++written;
            CheckReadsThenWrite(o, 0.18, -0.12, i == 4);
        }
    }
    CHECK(written == 6);
    CHECK(e.locks == 10 && e.buildLookups == 10);
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

// Every spoiled input: no write, whatever else is true. The cases that spoil the SNAPSHOT keep the calibration from ever completing (the
// log has no "calibrated" line); the cases that spoil the NGX pair of an otherwise calibrated session are passed through and named. Each
// case spoils exactly one thing; the same rig without the spoiler writes (the control below).
void TestEveryFailedGuardWritesNothing()
{
    const JitterBuild& b = kCurrentBuild.jitter;
    enum class Expect
    {
        NeverCalibrated, // the snapshots are unusable: learning never completes
        NotUsable,       // calibrated, but the NGX pair of every evaluate cannot be scaled
        Consistent       // calibrated as "the game already hands NGX what it applies": nothing to apply
    };
    struct Case
    {
        const char* name;
        Expect expect;
        bool readFailure;  // the probe also reports it as a failure to read the object
        std::function<void(FakeEnv&, Frame&)> spoil;
    };
    const std::vector<Case> cases = {
        { "global unreadable", Expect::NeverCalibrated, true, [&](FakeEnv& e, Frame&) { e.mem.regions.erase(kBase + b.cameraGlobalRva); } },
        { "global null", Expect::NeverCalibrated, true, [&](FakeEnv& e, Frame&) { e.mem.PutU64(kBase + b.cameraGlobalRva, 0); } },
        { "camera unreadable", Expect::NeverCalibrated, true, [&](FakeEnv& e, Frame&) { e.mem.PutU64(kBase + b.cameraGlobalRva, 0x5000000000ull); } },
        { "camera null", Expect::NeverCalibrated, true, [&](FakeEnv& e, Frame&) { e.mem.PutU64(kGlobal + b.cameraOffset, 0); } },
        { "object pointer slot unreadable", Expect::NeverCalibrated, true,
          [&](FakeEnv& e, Frame&) { e.mem.regions.erase(kCamera + b.jitterPointerOffset); } },
        { "object pointer null", Expect::NeverCalibrated, true, [&](FakeEnv& e, Frame&) { e.mem.PutU64(kCamera + b.jitterPointerOffset, 0); } },
        { "object unreadable", Expect::NeverCalibrated, true,
          [&](FakeEnv& e, Frame&) { e.mem.PutU64(kCamera + b.jitterPointerOffset, 0x5000000000ull); } },
        { "object unstable", Expect::NeverCalibrated, true,
          [](FakeEnv& e, Frame&)
          {
              auto n = std::make_shared<int>(0);
              e.mem.beforeRead = [n](FakeMemory& m) { PutObject(m, { 0.3f, -0.2f, 0.00015f, 0.0001f, 5000 + (*n)++ }); };
          } },
        { "jitter out of range", Expect::NeverCalibrated, true, [](FakeEnv&, Frame& f) { f.jx = 0.9f; } },
        { "ndc out of range", Expect::NeverCalibrated, true,
          [](FakeEnv& e, Frame&) { e.mem.beforeRead = [](FakeMemory& m) { PutObject(m, { 0.3f, -0.2f, 1.5f, 0.0001f, 5000 }); }; } },
        { "render size unknown (no Width/Height)", Expect::NeverCalibrated, false,
          [](FakeEnv& e, Frame&)
          {
              e.uints.erase(Param::Width);
              e.uints.erase(Param::Height);
          } },
        { "Width present, Height missing", Expect::NeverCalibrated, false, [](FakeEnv& e, Frame&) { e.uints.erase(Param::Height); } },
        { "applied jitter implausible", Expect::NeverCalibrated, false,
          [](FakeEnv&, Frame& f)
          {
              f.jx = 0.4f;
              f.jy = 0.4f;
              f.s = 8.0f;
          } },
        { "sign mismatch", Expect::NeverCalibrated, false, [](FakeEnv&, Frame& f) { f.flipNdcY = true; } },
        { "jitter too small to tell", Expect::NeverCalibrated, false,
          [](FakeEnv&, Frame& f)
          {
              f.jx = 0.01f;
              f.jy = -0.01f;
          } },
        { "counter never increases", Expect::NeverCalibrated, false, [](FakeEnv&, Frame& f) { f.counter = 100; } },
        { "NGX jitter missing", Expect::NotUsable, false, [](FakeEnv&, Frame& f) { f.ngxMissing = true; } },
        { "only Jitter.Offset.X missing", Expect::NotUsable, false, [](FakeEnv&, Frame& f) { f.ngxMissingX = true; } },
        { "only Jitter.Offset.Y missing", Expect::NotUsable, false, [](FakeEnv&, Frame& f) { f.ngxMissingY = true; } },
        { "NGX jitter is NaN", Expect::NotUsable, false, [](FakeEnv&, Frame& f) { f.ngxJx = std::nanf(""); } },
        { "NGX jitter above 0.55", Expect::NotUsable, false, [](FakeEnv&, Frame& f) { f.ngxJy = 0.56f; } },
        { "game already consistent", Expect::Consistent, false, [](FakeEnv&, Frame& f) { f.s = 2.0f; } },
    };
    for (const Case& c : cases)
        for (int probe = 0; probe < 2; ++probe)
        {
            FakeEnv e = MakeEnv();
            Engine engine;
            std::vector<std::string> all;
            for (int i = 0; i < 12; ++i)
            {
                Frame f;
                f.counter = 100 + i;
                f.timeMs = static_cast<std::uint64_t>(i) * 16;
                f.s = 1.2f; // ratio 0.6
                c.spoil(e, f); // the spoiler of the memory or the block is applied before each evaluate (Evaluate rewrites the object)
                const Outcome o = Evaluate(e, engine, f, probe != 0, true);
                all.insert(all.end(), o.logs.begin(), o.logs.end());
            }
            if (e.setsEver != 0)
                std::printf("  case that wrote: %s (probe %d)\n", c.name, probe);
            CHECK(e.setsEver == 0);

            int wrongReason = 0;
            const int calibrated = CountLines(all, "calibrated ratio");
            const int notUsable = CountLines(all, "jitter pair not usable");
            const int consistent = CountLines(all, "already hands DLSS the jitter it applies");
            switch (c.expect)
            {
            case Expect::NeverCalibrated: wrongReason += calibrated == 0 && notUsable == 0 && consistent == 0 ? 0 : 1; break;
            case Expect::NotUsable: wrongReason += calibrated == 1 && notUsable == 1 && consistent == 0 ? 0 : 1; break;
            case Expect::Consistent: wrongReason += calibrated == 0 && notUsable == 0 && consistent == 1 ? 0 : 1; break;
            }
            if (probe != 0 && c.readFailure)
                wrongReason += CountLines(all, "Game jitter: not read, guard failed") == 1 ? 0 : 1;
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
        for (int i = 0; i < 12; ++i)
        {
            Frame f;
            f.counter = 100 + i;
            f.timeMs = static_cast<std::uint64_t>(i) * 16;
            f.s = 1.2f;
            Evaluate(e, engine, f, false, true);
        }
        CHECK(e.setsEver == 2 * 8); // evaluates 5 to 12
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
    TestProbeOnlyNeverWrites();
    TestProbeOnlyDoesNoLookupsBetweenSamples();
    TestProbeOnlyStillSamplesAConfigurationChangeEarly();
    TestSampleLineCarriesTheContextOfThisEvaluate();
    TestEveryConfigurationFieldIsSampledEarlyAndPrinted();
    TestEveryFailedGuardWritesNothing();
    return TEST_RESULT("jitter_glue");
}
