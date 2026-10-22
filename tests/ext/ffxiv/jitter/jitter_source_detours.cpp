#include "check.h"

#include "ext/ffxiv/jitter/FfxivJitterSourceDetours.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

using namespace ext::ffxiv::jitter::source;

// The code that writes the game's memory, on a fake generator: the bodies of the two detours (FfxivJitterSourceHooks.cpp keeps only the
// Windows glue around them). A fake generator records the arguments it was called with and writes a known, asymmetric Halton pair, so an
// argument swap, a write of *y into *x, a normalisation after the record or a missing caller test cannot pass by coincidence.

namespace
{
constexpr std::uintptr_t kExpectedCaller = 0x7ff600025f79eull;
constexpr std::uintptr_t kProducerCaller = 0x7ff600374122ull;
constexpr std::uintptr_t kOpaque = 0x1234;
constexpr std::uintptr_t kResult = 0xABCDEF;
constexpr float kGenX = 0.3125f;   // 5/16
constexpr float kGenY = -0.1875f;  // -3/16
constexpr std::uint64_t kNow = 50000;

struct FakeGenerator
{
    int calls = 0;
    std::uintptr_t opaque = 0;
    float* x = nullptr;
    float* y = nullptr;
    unsigned frame = 0, phases = 0;
    float writeX = kGenX, writeY = kGenY;
    std::uintptr_t operator()(std::uintptr_t o, float* px, float* py, unsigned f, unsigned p)
    {
        ++calls;
        opaque = o;
        x = px;
        y = py;
        frame = f;
        phases = p;
        *px = writeX;
        *py = writeY;
        return kResult;
    }
};

// The performance setup of the game: DLSS mode, amount 0.6, scale 1, extents equal (ratio 1) -> strength 0.6; 1920x1080 -> 3840x2160.
struct FakeInputs
{
    int calls = 0;
    bool ok = true;
    GeneratorInputs values;
    FakeInputs()
    {
        values.dlssMode = true;
        values.amount = 0.6f;
        values.scale = 1.0f;
        values.extent0 = values.extent2 = 100;
        values.renderW = 1920;
        values.renderH = 1080;
        values.outW = 3840;
        values.outH = 2160;
    }
    bool operator()(std::uint32_t phases, GeneratorInputs& in)
    {
        ++calls;
        if (!ok)
            return false;
        in = values;
        in.phases = phases;
        return true;
    }
};

struct Rig
{
    SharedState state;
    FakeGenerator generator;
    FakeInputs inputs;
    float x = 9.0f, y = 8.0f; // the jitter object P: x = P+0, y = P+4 (the game's storage; the generator overwrites both)
    std::uintptr_t caller = kExpectedCaller;
    unsigned frame = 77, phases = kNativePhases;

    std::uintptr_t ObjectX() const { return reinterpret_cast<std::uintptr_t>(&x); }
    std::uintptr_t Run(float* px = nullptr, float* py = nullptr, bool useDefaultPointers = true)
    {
        GenerateArgs a;
        a.caller = caller;
        a.expectedCaller = kExpectedCaller;
        a.opaque = kOpaque;
        a.x = useDefaultPointers ? &x : px;
        a.y = useDefaultPointers ? &y : py;
        a.frame = frame;
        a.phases = phases;
        return RunGenerate(
            state, a, [&](std::uintptr_t o, float* gx, float* gy, unsigned f, unsigned p) { return generator(o, gx, gy, f, p); },
            [&](std::uint32_t p, GeneratorInputs& in) { return inputs(p, in); }, [] { return kNow; });
    }
    // The world camera's jitter object is known (the producer called the getter for it).
    void LearnWorld() { state.LearnWorldObject(ObjectX()); }
    void EnableAndLearn()
    {
        state.Enable(true);
        LearnWorld();
    }
};

bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

// ---- M0: only the world camera's jitter object is touched -------------------------------------------------------------------------

void TestUnknownWorldIsPassThrough()
{
    Rig r;
    r.state.Enable(true); // enabled, but the producer has not yet shown which object is the world camera
    const std::uintptr_t result = r.Run();
    CHECK(result == kResult);
    CHECK(r.generator.calls == 1);
    CHECK(r.generator.phases == kNativePhases);       // the game's own unbounded phase count, untouched
    CHECK(r.generator.frame == 77 && r.generator.opaque == kOpaque);
    CHECK(r.generator.x == &r.x && r.generator.y == &r.y);
    CHECK(r.x == kGenX && r.y == kGenY);              // the output is the game's own
    CHECK(r.inputs.calls == 0);                       // no game memory read for an object that is not known to be the world camera
    CHECK(!r.state.FindGenerated(kGenX, kGenY).has_value()); // nothing recorded
    CHECK(r.state.Status(kNow).lastNormalizedMs == 0 && !r.state.Status(kNow).recent);
    // but the object is noted for the one diagnostic line
    const SeenObjects seen = r.state.Seen();
    CHECK(seen.count == 1 && seen.items[0].pointer == r.ObjectX() && seen.items[0].calls == 1);
}

void TestForeignObjectIsLeftAlone()
{
    Rig r;
    r.EnableAndLearn();
    float otherX = 7.0f, otherY = 6.0f; // another jittered object (a second camera) reaching the same call site
    const std::uintptr_t result = r.Run(&otherX, &otherY, false);
    CHECK(result == kResult && r.generator.calls == 1);
    CHECK(r.generator.phases == kNativePhases);       // not even the phase count is replaced
    CHECK(r.generator.x == &otherX && r.generator.y == &otherY);
    CHECK(otherX == kGenX && otherY == kGenY);        // not divided
    CHECK(r.inputs.calls == 0);
    CHECK(!r.state.FindGenerated(kGenX, kGenY).has_value());
    CHECK(!r.state.Status(kNow).recent);
    // and it is in the set of objects seen at the call site, next to the world camera's
    r.Run(); // the world camera
    const SeenObjects seen = r.state.Seen();
    CHECK(seen.count == 2);
    // the world camera is normalised as before
    CHECK(Near(r.x, kGenX / 0.6f) && Near(r.y, kGenY / 0.6f));
}

void TestWorldObjectIsNormalised()
{
    Rig r;
    r.EnableAndLearn();
    const std::uintptr_t result = r.Run();
    CHECK(result == kResult && r.generator.calls == 1);
    CHECK(r.inputs.calls == 1);
    // arguments in the game's order: opaque, x, y, frame, phases; the unbounded phase count is replaced by 32 at Performance
    CHECK(r.generator.opaque == kOpaque && r.generator.x == &r.x && r.generator.y == &r.y);
    CHECK(r.generator.frame == 77);
    CHECK(r.generator.phases == 32);
    // output = h / strength on each axis, x from x and y from y
    CHECK(Near(r.x, kGenX / 0.6f));
    CHECK(Near(r.y, kGenY / 0.6f));
    // the record: expected = the generator's own value BEFORE the division, out = after
    const std::optional<Generated> g = r.state.FindGenerated(r.x, r.y);
    CHECK(g.has_value());
    if (g)
    {
        CHECK(g->expectedX == kGenX && g->expectedY == kGenY);
        CHECK(Near(g->outX, kGenX / 0.6f) && Near(g->outY, kGenY / 0.6f));
        CHECK(g->normalized);
        CHECK(Near(g->strength, 0.6f));
        CHECK(g->phases == 32);
        CHECK(g->renderW == 1920 && g->renderH == 1080 && g->outW == 3840 && g->outH == 2160);
        CHECK(g->timeMs == kNow);
    }
    const GeneratorStatus s = r.state.Status(kNow);
    CHECK(s.recent && s.phases == 32 && Near(s.strength, 0.6f) && s.lastWhy == PlanWhy::Normalize);
}

void TestPhaseCountReplacedOnlyWhenUnbounded()
{
    Rig r;
    r.EnableAndLearn();
    r.phases = 18; // a bounded count the game chose: kept
    r.Run();
    CHECK(r.generator.phases == 18 && r.generator.frame == 77);
    CHECK(Near(r.x, kGenX / 0.6f)); // still normalised
    r.x = r.y = 0.0f;
    r.phases = 0xFFFFFFFEu; // one short of the unbounded marker: kept
    r.Run();
    CHECK(r.generator.phases == 0xFFFFFFFEu);
    r.phases = kNativePhases;
    r.frame = 5;
    r.Run();
    CHECK(r.generator.phases == 32 && r.generator.frame == 5); // frame and phases are not swapped
    // sizes that make no phase count: the game's own count is kept, the output is still normalised
    r.inputs.values.renderW = 4000; // wider than the output
    r.Run();
    CHECK(r.generator.phases == kNativePhases);
}

void TestCallerAndSwitch()
{
    {
        Rig r;
        r.EnableAndLearn();
        r.caller = kExpectedCaller + 1; // another caller of the generator
        r.Run();
        CHECK(r.generator.calls == 1 && r.generator.phases == kNativePhases && r.x == kGenX && r.y == kGenY && r.inputs.calls == 0);
        CHECK(r.state.Seen().count == 0); // not even noted
        CHECK(!r.state.Status(kNow).recent);
    }
    {
        Rig r;
        r.LearnWorld(); // not enabled (the key off, or fallen back)
        r.Run();
        CHECK(r.generator.calls == 1 && r.generator.phases == kNativePhases && r.x == kGenX && r.y == kGenY && r.inputs.calls == 0);
        CHECK(r.state.Seen().count == 0);
    }
    {
        Rig r;
        r.EnableAndLearn();
        GenerateArgs a;
        a.caller = 0;
        a.expectedCaller = 0; // no build: nothing to compare with, the call is the game's own
        a.x = &r.x;
        a.y = &r.y;
        a.phases = kNativePhases;
        RunGenerate(
            r.state, a, [&](std::uintptr_t o, float* gx, float* gy, unsigned f, unsigned p) { return r.generator(o, gx, gy, f, p); },
            [&](std::uint32_t p, GeneratorInputs& in) { return r.inputs(p, in); }, [] { return kNow; });
        CHECK(r.generator.calls == 1 && r.generator.phases == kNativePhases && r.x == kGenX && r.inputs.calls == 0);
    }
    {
        Rig r;
        r.EnableAndLearn();
        // a null output pointer: the game's call goes through, nothing is dereferenced
        GenerateArgs a;
        a.caller = kExpectedCaller;
        a.expectedCaller = kExpectedCaller;
        a.opaque = kOpaque;
        a.x = nullptr;
        a.y = &r.y;
        a.frame = 3;
        a.phases = kNativePhases;
        std::uintptr_t seenOpaque = 0;
        unsigned seenPhases = 0;
        const std::uintptr_t result = RunGenerate(
            r.state, a, [&](std::uintptr_t o, float*, float*, unsigned, unsigned p) { seenOpaque = o; seenPhases = p; return kResult; },
            [&](std::uint32_t p, GeneratorInputs& in) { return r.inputs(p, in); }, [] { return kNow; });
        CHECK(result == kResult && seenOpaque == kOpaque && seenPhases == kNativePhases && r.inputs.calls == 0);
    }
}

void TestPlanGatesThroughTheDetour()
{
    // not DLSS mode: left alone, the reason is kept for the "idle" text
    {
        Rig r;
        r.EnableAndLearn();
        r.inputs.values.dlssMode = false;
        r.Run();
        CHECK(r.generator.phases == kNativePhases && r.x == kGenX && r.y == kGenY);
        CHECK(!r.state.FindGenerated(kGenX, kGenY).has_value());
        CHECK(r.state.Status(kNow).lastWhy == PlanWhy::NotDlssMode && !r.state.Status(kNow).recent);
    }
    // strength outside [0.125, 4]: left alone
    for (float amount : { 0.05f, 9.0f })
    {
        Rig r;
        r.EnableAndLearn();
        r.inputs.values.amount = amount;
        r.Run();
        CHECK(r.generator.phases == kNativePhases && r.x == kGenX && r.y == kGenY);
        CHECK(r.state.Status(kNow).lastWhy == PlanWhy::StrengthInvalid && !r.state.Status(kNow).recent);
    }
    // a failed read of the game's memory: the call is left alone (never a guess)
    {
        Rig r;
        r.EnableAndLearn();
        r.inputs.ok = false;
        r.Run();
        CHECK(r.generator.phases == kNativePhases && r.x == kGenX && r.y == kGenY && !r.state.Status(kNow).recent);
    }
    // an output that is not a Halton value: the phase count was replaced, the output stays, the record says not normalised
    {
        Rig r;
        r.EnableAndLearn();
        r.generator.writeX = 0.7f;
        r.Run();
        CHECK(r.generator.phases == 32);
        CHECK(r.x == 0.7f && r.y == kGenY);
        const std::optional<Generated> g = r.state.FindGenerated(0.7f, kGenY);
        CHECK(g.has_value() && !g->normalized);
        CHECK(!r.state.Status(kNow).recent);
    }
}

// L1: a strength within 1e-6 of 1 has nothing to divide; the phase count is replaced, so the call counts as normalised (one predicate for
// both decisions).
void TestStrengthOneCountsAsNormalised()
{
    Rig r;
    r.EnableAndLearn();
    r.inputs.values.amount = 1.0f; // (1 - 0.001) * 1 + 0.001 = 1
    r.Run();
    CHECK(r.generator.phases == 32);
    CHECK(r.x == kGenX && r.y == kGenY);
    const std::optional<Generated> g = r.state.FindGenerated(kGenX, kGenY);
    CHECK(g.has_value() && g->normalized && g->expectedX == kGenX);
    CHECK(r.state.Status(kNow).recent);
}

// ---- the camera getter -----------------------------------------------------------------------------------------------------------

struct ObserveRig
{
    SharedState state;
    float object[4] = { 0.5f, -0.25f, 0.0003125f, 0.00027777778f }; // { jx, jy, ndcX, ndcY } of a 1920x1080 frame at strength 0.6: pixels (0.3, -0.15)
    int getterCalls = 0;
    std::uintptr_t getterArg = 0;
    void* getterResult = nullptr;
    int objectReads = 0;
    bool torn = false;
    bool sizeOk = true;
    std::uintptr_t caller = kProducerCaller;

    ObserveRig() { getterResult = object; }
    void* Run(std::uintptr_t camera = 0x5555)
    {
        ObserveArgs a;
        a.caller = caller;
        a.expectedCaller = kProducerCaller;
        a.camera = camera;
        return RunObserve(
            state, a,
            [&](std::uintptr_t c) -> void*
            {
                ++getterCalls;
                getterArg = c;
                return getterResult;
            },
            [&](const void* p, float* out)
            {
                ++objectReads;
                std::memcpy(out, p, sizeof(object));
                if (torn && objectReads % 2 == 0)
                    out[2] += 1.0f; // the game wrote the object between the two reads
                return true;
            },
            [&](std::uint32_t* size)
            {
                size[0] = 1920;
                size[1] = 1080;
                return sizeOk;
            },
            [] { return kNow; });
    }
};

void TestObserveLearnsTheWorldObject()
{
    ObserveRig r;
    r.state.Enable(true);
    CHECK(r.state.WorldObject() == 0);
    void* result = r.Run(0x7777);
    CHECK(result == r.object && r.getterCalls == 1 && r.getterArg == 0x7777); // the game's pointer, unchanged, called with the game's argument
    CHECK(r.state.WorldObject() == reinterpret_cast<std::uintptr_t>(r.object));
    // the sample is recorded: raw pair, projected offset ndc * size / 2
    const std::optional<Match> m = r.state.Pair(0.5f, -0.25f, 1920, 1080, kNow);
    CHECK(m.has_value());
    if (m)
    {
        CHECK(m->observed.width == 1920 && m->observed.height == 1080 && m->observed.timeMs == kNow);
        CHECK(Near(m->pixels.x, 0.3f) && Near(m->pixels.y, -0.15f));
    }
    // the world object follows a new pointer (the game rebuilt its camera)
    float other[4] = { 0.5f, -0.25f, 0.0003125f, 0.00027777778f };
    r.getterResult = other;
    r.Run();
    CHECK(r.state.WorldObject() == reinterpret_cast<std::uintptr_t>(other));
}

void TestObserveOnlyForTheProducerWhenEnabled()
{
    {
        ObserveRig r;
        r.state.Enable(true);
        r.caller = kProducerCaller + 1;
        void* result = r.Run();
        CHECK(result == r.object && r.getterCalls == 1);
        CHECK(r.state.WorldObject() == 0 && r.objectReads == 0 && !r.state.Pair(0.5f, -0.25f, 1920, 1080, kNow).has_value());
    }
    {
        ObserveRig r; // not enabled
        void* result = r.Run();
        CHECK(result == r.object && r.getterCalls == 1 && r.state.WorldObject() == 0 && r.objectReads == 0);
    }
    {
        ObserveRig r;
        r.state.Enable(true);
        r.getterResult = nullptr;
        CHECK(r.Run() == nullptr);
        CHECK(r.state.WorldObject() == 0 && r.objectReads == 0);
    }
}

void TestObserveTornReadAndSize()
{
    {
        ObserveRig r;
        r.state.Enable(true);
        r.torn = true; // a torn read is not recorded (the object is still the world camera's)
        r.Run();
        CHECK(!r.state.Pair(0.5f, -0.25f, 1920, 1080, kNow).has_value());
        CHECK(r.state.WorldObject() == reinterpret_cast<std::uintptr_t>(r.object));
    }
    {
        ObserveRig r;
        r.state.Enable(true);
        r.sizeOk = false; // the render size could not be read
        r.Run();
        CHECK(!r.state.Pair(0.5f, -0.25f, 1920, 1080, kNow).has_value());
    }
}

// ---- the shared state's new pieces -------------------------------------------------------------------------------------------------

void TestWorldObjectLifetime()
{
    SharedState s;
    s.LearnWorldObject(0x100);
    CHECK(s.WorldObject() == 0x100);
    s.LearnWorldObject(0); // a null pointer is never learned
    CHECK(s.WorldObject() == 0x100);
    s.Enable(true);        // switching on forgets a pointer learned before (the camera may have been rebuilt while the hooks were passive)
    s.LearnWorldObject(0x200);
    s.Enable(true);
    CHECK(s.WorldObject() == 0x200);
    s.Enable(false);
    s.Enable(true);
    CHECK(s.WorldObject() == 0);
    s.LearnWorldObject(0x300);
    s.Clear();
    CHECK(s.WorldObject() == 0 && s.Seen().count == 0);
}

void TestSeenObjects()
{
    SharedState s;
    for (int i = 0; i < 5; ++i)
        s.NoteCallSiteObject(0x10);
    s.NoteCallSiteObject(0x20);
    SeenObjects seen = s.Seen();
    CHECK(seen.count == 2 && !seen.overflow);
    CHECK(seen.items[0].pointer == 0x10 && seen.items[0].calls == 5 && seen.items[1].pointer == 0x20 && seen.items[1].calls == 1);
    // bounded: more distinct pointers than the table holds set the overflow marker, the table does not grow
    for (std::uintptr_t p = 0x1000; p < 0x1000 + 32; ++p)
        s.NoteCallSiteObject(p);
    seen = s.Seen();
    CHECK(seen.count == kMaxSeenObjects && seen.overflow);
    // the text: every pointer, the world camera marked, a second one called out
    SeenObjects a;
    a.items[0] = { 0x10, 5 };
    a.count = 1;
    const std::string one = FormatSeenObjects(a, 0x10);
    CHECK(one.find("1 distinct") != std::string::npos && one.find("world camera") != std::string::npos);
    a.items[1] = { 0x20, 1 };
    a.count = 2;
    const std::string two = FormatSeenObjects(a, 0x10);
    CHECK(two.find("2 distinct") != std::string::npos && two.find("second") != std::string::npos);
}
} // namespace

int main()
{
    TestUnknownWorldIsPassThrough();
    TestForeignObjectIsLeftAlone();
    TestWorldObjectIsNormalised();
    TestPhaseCountReplacedOnlyWhenUnbounded();
    TestCallerAndSwitch();
    TestPlanGatesThroughTheDetour();
    TestStrengthOneCountsAsNormalised();
    TestObserveLearnsTheWorldObject();
    TestObserveOnlyForTheProducerWhenEnabled();
    TestObserveTornReadAndSize();
    TestWorldObjectLifetime();
    TestSeenObjects();
    return TEST_RESULT("jitter_source_detours");
}
