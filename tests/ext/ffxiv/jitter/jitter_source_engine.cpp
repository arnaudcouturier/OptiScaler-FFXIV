#include "check.h"
#include "engine_fixture.h"

#include <string>
#include <vector>

using namespace ext::ffxiv;
using namespace ext::ffxiv::jitter;
using namespace jitter_fakes;

// How the calibrate-and-scale engine and the probe behave next to the source fix: while the fix is active the generator output is divided by
// the native strength, so the camera object holds up to 0.5 / strength (0.83 at 0.6), above the 0.5 pixel snapshot guard and the 0.55 pixel
// NGX pair limit. The wrapper makes the engine stand down (correct = false), tells it the larger bound, and the probe says what it sees.

namespace
{
// A frame of a game with the fix active: the object holds the generator value divided by the strength, the game multiplies it back.
Frame Divided(float h, float k, float strength, int counter, std::uint64_t t)
{
    return At(counter, 2.0f * strength, t, h / strength, k / strength); // ObjectFor(jx, jy, s): ndc = s * jx / W, applied = s/2 * jx = strength * jx
}

void TestReadChainBound()
{
    RawObject out;
    // the default bound is the old 0.5
    auto m = Image(ObjectFor(0.8f, -0.7f, 1.2f, 1920, 1080, 10));
    CHECK(Read(m, out) == Guard::JitterOutOfRange);
    // a larger bound lets the divided value through; a value above the larger bound is still refused
    auto m2 = Image(ObjectFor(0.8f, -0.7f, 1.2f, 1920, 1080, 10));
    CHECK(ReadChain(m2.Reader(), &kCurrentBuild, kBase, out, 0.9f) == Guard::None && out.jx == 0.8f && out.jy == -0.7f);
    auto m3 = Image(ObjectFor(0.95f, 0.0f, 1.2f, 1920, 1080, 10));
    CHECK(ReadChain(m3.Reader(), &kCurrentBuild, kBase, out, 0.9f) == Guard::JitterOutOfRange);
    // non-finite values are refused whatever the bound
    const float nan = std::numeric_limits<float>::quiet_NaN();
    auto m4 = Image({ nan, 0.1f, 0.001f, 0.001f, 1 });
    CHECK(ReadChain(m4.Reader(), &kCurrentBuild, kBase, out, 4.0f) == Guard::JitterOutOfRange);
    // the NDC guard is not moved by the bound
    auto m5 = Image({ 0.1f, 0.1f, 1.5f, 0.0f, 1 });
    CHECK(ReadChain(m5.Reader(), &kCurrentBuild, kBase, out, 4.0f) == Guard::NdcOutOfRange);
}

void TestProbeWithTheFixActive()
{
    const SourceNote note { true, 0.9f };
    // without the note the probe cannot read an object above 0.5: it logs the guard
    {
        FakeEnv e = MakeEnv();
        Engine engine;
        const Outcome o = Evaluate(e, engine, Divided(0.4f, -0.3f, 0.6f, 100, 0), true, false, 0.0f, SourceNote {});
        CHECK(CountLines(o.logs, "Game jitter: not read, guard failed: jitter value not finite or above 0.5 pixel") == 1);
        CHECK(CountLines(o.logs, "Game jitter: counter") == 0);
    }
    // with the note the same object is read, and the line says what the numbers are
    {
        FakeEnv e = MakeEnv();
        Engine engine;
        const Outcome o = Evaluate(e, engine, Divided(0.4f, -0.3f, 0.6f, 100, 0), true, false, 0.0f, note);
        CHECK(CountLines(o.logs, "not read") == 0);
        CHECK(CountLines(o.logs, "Game jitter: counter 100") == 1);
        CHECK(CountLines(o.logs, "jitter source fix active") == 1);
        CHECK(CountLines(o.logs, "generator value before the game's multiplier") == 1);
        // the misleading claims of the passive probe are not made: no "another frame" accusation, no "differ" verdict for the multiplier
        CHECK(CountLines(o.logs, "belongs to another frame") == 0);
        CHECK(CountLines(o.logs, "reported and applied jitter differ") == 0);
        // applied is what was drawn: 0.4 and -0.3 (the generator's own value)
        bool applied = false;
        for (const std::string& l : o.logs)
            applied = applied || (Has(l.c_str(), "applied ax/ay 0.4000/-0.3000") && Has(l.c_str(), "rx/ry 0.600/0.600"));
        CHECK(applied);
    }
    // the passive probe line is unchanged without a note (a game with the fix off)
    {
        FakeEnv e = MakeEnv();
        Engine engine;
        const Outcome o = Evaluate(e, engine, At(100, 1.2f, 0, 0.3f, -0.2f), true, false);
        CHECK(CountLines(o.logs, "Game jitter: counter") == 1 && CountLines(o.logs, "jitter source fix") == 0);
    }
}

void TestStandDownWritesNothingAndKeepsTheRatio()
{
    // learn a ratio with the correction on (game strength 0.6 = s 1.2), then the fix goes active: the wrapper passes correct = false
    FakeEnv e = MakeEnv();
    Engine engine;
    std::uint64_t t = 0;
    int counter = 100;
    for (int i = 0; i < 12; ++i)
    {
        t += 16;
        Evaluate(e, engine, At(++counter, 1.2f, t, 0.1f + 0.03f * i, -0.2f + 0.02f * i), false, true);
    }
    // adopted: the next evaluate is scaled
    t += 16;
    Outcome o = Evaluate(e, engine, At(++counter, 1.2f, t, 0.3f, -0.2f), false, true);
    CHECK(o.sets.size() == 2);
    // stood down: probe off, correction off for the engine -> it touches nothing and reads nothing
    for (int i = 0; i < 50; ++i)
    {
        t += 16;
        o = Evaluate(e, engine, Divided(0.4f, -0.3f, 0.6f, ++counter, t), false, false, 0.0f, SourceNote { true, 0.9f });
        CHECK(o.sets.empty() && o.ops.empty());
    }
    // the fix falls back: calibrate-and-scale takes over at once, with the ratio it had (the camera object is back to the game's own value)
    t += 16;
    o = Evaluate(e, engine, At(++counter, 1.2f, t, 0.3f, -0.2f), false, true);
    CHECK(o.sets.size() == 2);
    CHECK(Near(o.sets[0].value, 0.18) && Near(o.sets[1].value, -0.12)); // 0.3 * 0.6, -0.2 * 0.6
}

void TestProbeOnStandDownKeepsTheRatio()
{
    // the same as above with the PROBE on: a stood-down evaluate must not make the engine forget what it learned (the fall-back takes over at once)
    FakeEnv e = MakeEnv();
    Engine engine;
    std::uint64_t t = 0;
    int counter = 100;
    for (int i = 0; i < 12; ++i)
    {
        t += 16;
        Evaluate(e, engine, At(++counter, 1.2f, t, 0.1f + 0.03f * i, -0.2f + 0.02f * i), true, true);
    }
    t += 16;
    Outcome o = Evaluate(e, engine, At(++counter, 1.2f, t, 0.3f, -0.2f), true, true);
    CHECK(o.sets.size() == 2); // adopted
    // active: the fix owns the pair; the correction key is on but the engine is told to stand down
    for (int i = 0; i < 50; ++i)
    {
        t += 16;
        o = Evaluate(e, engine, Divided(0.4f, -0.3f, 0.6f, ++counter, t), true, false, 0.0f, SourceNote { true, 0.9f, true });
        CHECK(o.sets.empty());
    }
    // a drain frame: the fix is no longer active but still owns this evaluate's pair
    t += 16;
    o = Evaluate(e, engine, Divided(0.4f, -0.3f, 0.6f, ++counter, t), true, false, 0.0f, SourceNote { false, 0.5f, true });
    CHECK(o.sets.empty());
    // the fix is gone: the first evaluate of the correction is scaled at once with the ratio it had
    t += 16;
    o = Evaluate(e, engine, At(++counter, 1.2f, t, 0.3f, -0.2f), true, true);
    CHECK(o.sets.size() == 2 && Near(o.sets[0].value, 0.18) && Near(o.sets[1].value, -0.12));
    // the correction key itself off (not a stand-down): the state IS dropped, and learning starts again when it is switched back on
    t += 16;
    Evaluate(e, engine, At(++counter, 1.2f, t, 0.3f, -0.2f), true, false);
    t += 16;
    o = Evaluate(e, engine, At(++counter, 1.2f, t, 0.3f, -0.2f), true, true);
    CHECK(o.sets.empty()); // learning: the game's pair passes through
}


void TestStandDownWithTheProbeOnlyNeverScales()
{
    // the probe stays on while the correction stands down: samples are logged, nothing is ever written
    FakeEnv e = MakeEnv();
    Engine engine;
    std::uint64_t t = 0;
    int samples = 0;
    for (int i = 0; i < 40; ++i)
    {
        t += 1000;
        const Outcome o = Evaluate(e, engine, Divided(0.3f, -0.2f, 0.6f, 100 + i, t), true, false, 0.0f, SourceNote { true, 0.9f });
        samples += CountLines(o.logs, "Game jitter: counter");
        CHECK(o.sets.empty());
    }
    CHECK(samples == 4); // one every 10 s
}
} // namespace

int main()
{
    TestReadChainBound();
    TestProbeWithTheFixActive();
    TestStandDownWritesNothingAndKeepsTheRatio();
    TestProbeOnStandDownKeepsTheRatio();
    TestStandDownWithTheProbeOnlyNeverScales();
    return TEST_RESULT("jitter_source_engine");
}
