#include "check.h"

// a failed REQUIRE_TRUE records the failure and leaves the test function (later lines index into what it guards)
#define REQUIRE_TRUE(cond) \
    do \
    { \
        CHECK(cond); \
        if (!(cond)) \
            return; \
    } while (0)

#include "ext/ffxiv/GameBuild.h"
#include "ext/ffxiv/jitter/FfxivJitterSourceDetours.h"

#include <cmath>
#include <optional>
#include <cstdint>
#include <cstring>

using namespace ext::ffxiv::jitter::source;

// The two constants the source-fix core assumed as literals (the strength floor 0.001f and the NDC y sign flip) are now read from the build
// (GameBuild.h strengthFloorBits / ndcSignMaskBits; the seams jitterSource.strengthFloor and jitterSource.ndcSignMask check the live executable and
// the gate refuses the fix where they differ). With the build's own values every number is exactly what the literals gave; with other values the
// core follows them or refuses, it never silently keeps the old assumption.

namespace
{
bool Near(float a, float b, float tol = 1e-6f) { return std::fabs(a - b) <= tol; }

float FromBits(std::uint32_t bits)
{
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void TestTheBuildHoldsTheCoreDefaults()
{
    const ext::ffxiv::JitterSourceBuild b;
    CHECK(FromBits(b.strengthFloorBits) == kStrengthFloor);
    CHECK(kStrengthFloor == 0.001f);
    CHECK(b.ndcSignMaskBits == kNdcYSignMask && kNdcYSignMask == 0x80000000u);
}

void TestStrengthFollowsTheFloor()
{
    // the build's own floor: identical to the default (and to the old literal)
    const float floorBuild = FromBits(ext::ffxiv::JitterSourceBuild {}.strengthFloorBits);
    const std::optional<float> a = NativeStrength(0.6f, 1.0f, 100, 100);
    const std::optional<float> b = NativeStrength(0.6f, 1.0f, 100, 100, floorBuild);
    REQUIRE_TRUE(a.has_value() && b.has_value());
    CHECK(*a == *b);
    CHECK(Near(*a, 0.6f));
    // another floor: the strength is the executable's own expression with THAT constant (0.5 * 0.5 + 0.2 at ratio 1... (amount - f) + f = amount at ratio 1;
    // a ratio below 1 shows the floor)
    const std::optional<float> low = NativeStrength(0.8f, 0.5f, 100, 100, 0.001f);
    const std::optional<float> high = NativeStrength(0.8f, 0.5f, 100, 100, 0.1f);
    REQUIRE_TRUE(low.has_value() && high.has_value());
    CHECK(Near(*low, (0.8f - 0.001f) * 0.25f + 0.001f));
    CHECK(Near(*high, (0.8f - 0.1f) * 0.25f + 0.1f));
    CHECK(*low != *high);
    // a floor that is not a finite number is refused
    CHECK(!NativeStrength(0.8f, 0.5f, 100, 100, std::nanf("")).has_value());
    CHECK(!NativeStrength(0.8f, 0.5f, 100, 100, INFINITY).has_value());
    // the plan carries the floor of its inputs
    GeneratorInputs in;
    in.dlssMode = true;
    in.amount = 0.8f;
    in.scale = 0.5f;
    in.extent0 = in.extent2 = 100;
    in.renderW = in.renderH = 1000;
    in.outW = in.outH = 2000;
    in.phases = kNativePhases;
    CHECK(in.strengthFloor == kStrengthFloor);
    const GeneratorPlan p1 = PlanGenerate(in);
    in.strengthFloor = 0.1f;
    const GeneratorPlan p2 = PlanGenerate(in);
    CHECK(p1.why == PlanWhy::Normalize && p2.why == PlanWhy::Normalize);
    CHECK(Near(p1.strength, *low) && Near(p2.strength, *high));
    in.strengthFloor = std::nanf("");
    CHECK(PlanGenerate(in).why == PlanWhy::StrengthInvalid);
}

void TestSignMask()
{
    CHECK(ApplySignMask(1.5f, 0x80000000u) == -1.5f);
    CHECK(ApplySignMask(-1.5f, 0x80000000u) == 1.5f);
    CHECK(ApplySignMask(1.5f, 0u) == 1.5f);
    CHECK(std::signbit(ApplySignMask(0.0f, 0x80000000u)) && !std::signbit(ApplySignMask(-0.0f, 0x80000000u)));

    // a frame at strength 0.6 on 1920x1080: raw (0.5, -0.25), pixels (0.3, -0.15), ndc (2 * px / w, -2 * py / h)
    Observed o;
    o.rawX = 0.5f;
    o.rawY = -0.25f;
    o.ndcX = 0.0003125f;
    o.ndcY = 0.00027777778f;
    o.width = 1920;
    o.height = 1080;
    const std::optional<Pair> flipped = ProjectOffset(o); // the default mask: the executable negates the y
    REQUIRE_TRUE(flipped.has_value());
    CHECK(Near(flipped->x, 0.3f) && Near(flipped->y, -0.15f));
    o.ndcYSignMask = 0x80000000u; // the build's own mask: the same
    const std::optional<Pair> same = ProjectOffset(o);
    REQUIRE_TRUE(same.has_value());
    CHECK(same->x == flipped->x && same->y == flipped->y);
    // another convention (no flip): the pair would describe a different direction; the sanity check refuses it instead of projecting it wrongly
    o.ndcYSignMask = 0;
    CHECK(!ProjectOffset(o).has_value());
}

void TestObserveCarriesTheMask()
{
    for (const std::uint32_t mask : { 0x80000000u, 0u })
    {
        SharedState state;
        state.Enable(true);
        float object[4] = { 0.5f, -0.25f, 0.0003125f, 0.00027777778f };
        ObserveArgs a;
        a.caller = 0x7ff600374122ull;
        a.expectedCaller = 0x7ff600374122ull;
        a.camera = 0x5555;
        a.ndcYSignMask = mask;
        void* r = RunObserve(
            state, a, [&](std::uintptr_t) -> void* { return object; },
            [&](const void* p, float* out)
            {
                std::memcpy(out, p, sizeof(object));
                return true;
            },
            [](std::uint32_t* size)
            {
                size[0] = 1920;
                size[1] = 1080;
                return true;
            },
            [] { return std::uint64_t { 50000 }; });
        CHECK(r == object);
        const std::optional<Match> m = state.Pair(0.5f, -0.25f, 1920, 1080, 50000);
        // the default mask records a sample that pairs; a mask that contradicts the data records nothing
        CHECK(m.has_value() == (mask == 0x80000000u));
        if (m)
            CHECK(m->observed.ndcYSignMask == mask && Near(m->pixels.y, -0.15f));
    }
    ObserveArgs defaults;
    CHECK(defaults.ndcYSignMask == kNdcYSignMask);
}
} // namespace

int main()
{
    TestTheBuildHoldsTheCoreDefaults();
    TestStrengthFollowsTheFloor();
    TestSignMask();
    TestObserveCarriesTheMask();
    return TEST_RESULT("jitter_source_constants");
}
