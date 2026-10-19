#include "check.h"
#include "fake_game.h"
#include "ext/ffxiv/jitter/FfxivJitterCore.h"

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
bool Near(float a, double b, double tol = 1e-5) { return std::fabs(a - b) <= tol; }
bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

struct Op
{
    bool set = false; // false = Get
    Param param = Param::JitterX;
    float value = 0.0f; // the value written (Set) or returned (float Get)
};

// The fake environment: a stateful parameter block (a Set changes what later Gets return), fake process memory, a clock and a log.
struct FakeEnv
{
    FakeMemory mem;
    const GameBuild* build = &kCurrentBuild;
    std::map<Param, float> floats;
    std::map<Param, std::uint32_t> uints;
    std::optional<int> perfQuality = 2;
    ForcedQualityAnswer answer;
    std::uint64_t now = 0;
    std::optional<bool> frameGeneration = false;

    std::vector<Op> ops;
    std::vector<std::string> logs;
    int locks = 0, buildLookups = 0, clockReads = 0, qualityQueries = 0, fgQueries = 0, setsEver = 0;

    struct Held
    {
        explicit Held(FakeEnv& e) { ++e.locks; }
    };
    Held Lock() { return Held(*this); }
    const GameBuild* Build()
    {
        ++buildLookups;
        return build;
    }
    std::uintptr_t ModuleBase() { return kBase; }
    bool ReadMemory(std::uintptr_t address, void* out, std::size_t bytes) { return mem.Read(address, out, bytes); }

    std::optional<float> GetFloat(Param p)
    {
        const auto it = floats.find(p);
        ops.push_back({ false, p, it == floats.end() ? 0.0f : it->second });
        if (it == floats.end())
            return std::nullopt;
        return it->second;
    }
    std::uint32_t GetUInt(Param p)
    {
        ops.push_back({ false, p, 0.0f });
        const auto it = uints.find(p);
        return it == uints.end() ? 0u : it->second;
    }
    std::optional<int> GetInt(Param p)
    {
        ops.push_back({ false, p, 0.0f });
        return perfQuality;
    }
    void SetFloat(Param p, float v)
    {
        ops.push_back({ true, p, v });
        ++setsEver;
        floats[p] = v;
    }
    ForcedQualityAnswer ForcedQuality()
    {
        ++qualityQueries;
        return answer;
    }
    std::uint64_t NowMs()
    {
        ++clockReads;
        return now;
    }
    void Log(const std::string& line) { logs.push_back(line); }
    std::optional<bool> FrameGenerationActive()
    {
        ++fgQueries;
        return frameGeneration;
    }

    std::size_t Gets() const
    {
        std::size_t n = 0;
        for (const Op& o : ops)
            n += o.set ? 0 : 1;
        return n;
    }
    std::vector<Op> Sets() const
    {
        std::vector<Op> out;
        for (const Op& o : ops)
            if (o.set)
                out.push_back(o);
        return out;
    }
};

// A game at 1920x1080 whose camera object is wired like the game's: memory chain plus a parameter block as the bridge hands it over.
FakeEnv MakeEnv()
{
    FakeEnv e;
    e.mem = Image(ObjectFor(0.3f, -0.2f, 1.0f, 1920, 1080, 100));
    e.uints[Param::SubrectWidth] = 1920;
    e.uints[Param::SubrectHeight] = 1080;
    e.uints[Param::Width] = 1920;
    e.uints[Param::Height] = 1080;
    e.uints[Param::OutWidth] = 3840;
    e.uints[Param::OutHeight] = 2160;
    return e;
}

// The game's side of one evaluate, as in the game: its camera object in memory (a game that applies jitter scaled by `s`) and the
// UNSCALED jitter it sets in the parameter block. `ngxJx/ngxJy` override what the block carries (another frame, say).
struct Frame
{
    float jx = 0.3f, jy = -0.2f, s = 1.0f;
    int counter = 100;
    std::uint64_t timeMs = 0;
    std::optional<float> ngxJx, ngxJy;
    bool ngxMissing = false; // the block has no Jitter.Offset at all
};

struct Outcome
{
    std::vector<Op> ops;
    std::vector<std::string> logs;
    std::vector<Op> sets;
};

Outcome Evaluate(FakeEnv& e, Engine& engine, const Frame& f, bool probe, bool correct)
{
    PutObject(e.mem, ObjectFor(f.jx, f.jy, f.s, 1920, 1080, f.counter));
    e.floats[Param::JitterX] = f.ngxJx.value_or(f.jx);
    e.floats[Param::JitterY] = f.ngxJy.value_or(f.jy);
    if (f.ngxMissing)
    {
        e.floats.erase(Param::JitterX);
        e.floats.erase(Param::JitterY);
    }
    e.now = f.timeMs;
    e.ops.clear();
    const std::size_t logsBefore = e.logs.size();
    engine.Step(e, probe, correct);
    Outcome o;
    o.ops = e.ops;
    o.logs.assign(e.logs.begin() + static_cast<std::ptrdiff_t>(logsBefore), e.logs.end());
    o.sets = e.Sets();
    return o;
}

// Checks one evaluate's order: both Jitter.Offset reads come before the first write, and the write is exactly X then Y.
void CheckReadsThenWrite(const Outcome& o, double ax, double ay)
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
    // and no read of the jitter values at all after the first write (the later reads would see our own output)
    for (std::size_t i = firstSet; i < o.ops.size(); ++i)
        CHECK(o.ops[i].set || (o.ops[i].param != Param::JitterX && o.ops[i].param != Param::JitterY));
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
        if (!o.sets.empty())
        {
            ++written;
            CheckReadsThenWrite(o, 0.15, -0.1);
        }
    }
    CHECK(written >= 1);
    CHECK(e.locks == 6 && e.buildLookups == 6);
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

// (c) Every failed guard: no write, whatever else is true. Each case spoils exactly one thing; the same rig without the spoiler writes.
void TestEveryFailedGuardWritesNothing()
{
    const JitterBuild& b = kCurrentBuild.jitter;
    struct Case
    {
        const char* name;
        std::function<void(FakeEnv&, Frame&)> spoil;
    };
    const std::vector<Case> cases = {
        { "build mismatch", [](FakeEnv& e, Frame&) { e.build = nullptr; } },
        { "global unreadable", [&](FakeEnv& e, Frame&) { e.mem.regions.erase(kBase + b.cameraGlobalRva); } },
        { "global null", [&](FakeEnv& e, Frame&) { e.mem.PutU64(kBase + b.cameraGlobalRva, 0); } },
        { "camera unreadable", [&](FakeEnv& e, Frame&) { e.mem.PutU64(kBase + b.cameraGlobalRva, 0x5000000000ull); } },
        { "camera null", [&](FakeEnv& e, Frame&) { e.mem.PutU64(kGlobal + b.cameraOffset, 0); } },
        { "object pointer null", [&](FakeEnv& e, Frame&) { e.mem.PutU64(kCamera + b.jitterPointerOffset, 0); } },
        { "object unreadable", [&](FakeEnv& e, Frame&) { e.mem.PutU64(kCamera + b.jitterPointerOffset, 0x5000000000ull); } },
        { "object unstable",
          [](FakeEnv& e, Frame&)
          {
              auto n = std::make_shared<int>(0);
              e.mem.beforeRead = [n](FakeMemory& m) { PutObject(m, { 0.3f, -0.2f, 0.00015f, 0.0001f, 5000 + (*n)++ }); };
          } },
        { "jitter out of range", [](FakeEnv&, Frame& f) { f.jx = 0.9f; } },
        { "ndc out of range",
          [](FakeEnv& e, Frame&)
          {
              e.mem.beforeRead = [](FakeMemory& m) { PutObject(m, { 0.3f, -0.2f, 5.0f, 0.0001f, 5000 }); };
          } },
        { "NGX jitter missing", [](FakeEnv&, Frame& f) { f.ngxMissing = true; } },
        { "object belongs to another frame", [](FakeEnv&, Frame& f) { f.ngxJx = 0.29f; } },
        { "render size unknown (no Width/Height)", [](FakeEnv& e, Frame&) { e.uints.erase(Param::Width); e.uints.erase(Param::Height); } },
        { "applied jitter implausible", [](FakeEnv&, Frame& f) { f.jx = 0.4f; f.jy = 0.4f; f.s = 8.0f; } },
        { "game already consistent", [](FakeEnv&, Frame& f) { f.s = 2.0f; } },
        { "jitter too small to tell", [](FakeEnv&, Frame& f) { f.jx = 0.01f; f.jy = -0.01f; } },
    };
    for (const Case& c : cases)
        for (int probe = 0; probe < 2; ++probe)
        {
            FakeEnv e = MakeEnv();
            Engine engine;
            for (int i = 0; i < 5; ++i)
            {
                Frame f;
                f.counter = 100 + i;
                f.timeMs = static_cast<std::uint64_t>(i) * 16;
                c.spoil(e, f); // the spoiler of the memory or the block is applied before each evaluate (Evaluate rewrites the object)
                Evaluate(e, engine, f, probe != 0, true);
            }
            if (e.setsEver != 0)
                std::printf("  case that wrote: %s (probe %d)\n", c.name, probe);
            CHECK(e.setsEver == 0);
        }

    // the sign guard needs a flipped axis in the object itself
    {
        FakeEnv e = MakeEnv();
        Engine engine;
        for (int i = 0; i < 5; ++i)
        {
            RawObject o = ObjectFor(0.3f, -0.2f, 1.0f, 1920, 1080, 100 + i);
            o.ndcY = -o.ndcY;
            PutObject(e.mem, o);
            e.floats[Param::JitterX] = 0.3f;
            e.floats[Param::JitterY] = -0.2f;
            e.ops.clear();
            engine.Step(e, false, true);
        }
        CHECK(e.setsEver == 0);
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
    TestProbeLineShowsTheValuesBeforeTheWrite();
    TestProbeOnlyNeverWrites();
    TestEveryFailedGuardWritesNothing();
    return TEST_RESULT("jitter_glue");
}
