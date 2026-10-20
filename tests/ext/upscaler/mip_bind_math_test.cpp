// [MIP-BIND] The pure numbers of the bind-time mipmap bias (ext/upscaler/MipBind.h): the target from an evaluate's sizes
// (DLAA 0, clamps, NaN and zero sizes), the size picks, which samplers are eligible, and the bias of a variant in the two
// modes.
#include "ext/upscaler/MipBind.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using namespace ext::upscaler::mipbind;

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

static bool Near(double a, double b, double tolerance = 1e-6) { return std::fabs(a - b) <= tolerance; }

static constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

int main()
{
    // ---- target from sizes -------------------------------------------------------------------------------------------
    {
        const auto quality = BaseFromEvaluate(2560, 3840);
        CHECK(quality && Near(*quality, -0.584963));
        const auto balanced = BaseFromEvaluate(2258, 3840);
        CHECK(balanced && Near(*balanced, -0.766061));
        const auto perf = BaseFromEvaluate(1920, 3840);
        CHECK(perf && Near(*perf, -1.0));
        const auto dlaa = BaseFromEvaluate(3840, 3840);
        CHECK(dlaa && *dlaa == 0.0 && !std::signbit(*dlaa));
        const auto bigger = BaseFromEvaluate(5120, 3840);
        CHECK(bigger && *bigger == 0.0); // more pixels than the output need no negative bias
        const auto scaled = BaseFromEvaluate(2560, 3840, 1.5);
        CHECK(scaled && Near(*scaled, -1.169925)); // output scaling enlarges the output the ratio is taken against
        const auto badScaling = BaseFromEvaluate(2560, 3840, std::nan(""));
        CHECK(badScaling && Near(*badScaling, -0.584963));
    }
    {
        // not a game scene: nothing, so the caller keeps the previous target
        CHECK(!BaseFromEvaluate(0, 3840));
        CHECK(!BaseFromEvaluate(2560, 0));
        CHECK(!BaseFromEvaluate(19, 3840));
        CHECK(!BaseFromEvaluate(2560, 16385));
        CHECK(!BaseFromEvaluate(16385, 16384));
        CHECK(!BaseFromEvaluate(300, 639)); // display narrower than 640
        CHECK(BaseFromEvaluate(320, 640).has_value());
    }
    {
        // the offset is added, the result stays inside +-15 and is never -0
        CHECK(Near(Target(-0.584963, 0.0), -0.584963));
        CHECK(Near(Target(-0.584963, -0.18), -0.764963));
        CHECK(Target(0.0, 0.0) == 0.0 && !std::signbit(Target(0.0, 0.0)));
        CHECK(Near(Target(-14.0, -3.0), -15.0));
        CHECK(Near(Target(0.0, 100.0), 3.0)); // the offset itself is limited to +-3
        CHECK(Target(std::nan(""), 0.0) == 0.0);
    }

    // ---- size picks --------------------------------------------------------------------------------------------------
    CHECK(PickRenderWidth(3840, 2258) == 2258); // FFXIV: the subrect is the buffer, Width is the render
    CHECK(PickRenderWidth(1920, 3840) == 1920);
    CHECK(PickRenderWidth(2560, 2560) == 2560);
    CHECK(PickRenderWidth(0, 2560) == 2560);
    CHECK(PickRenderWidth(3840, 0) == 3840);
    CHECK(PickRenderWidth(0, 0) == 0);
    CHECK(PickOutputWidth(3840, 1920, 2560) == 3840);
    CHECK(PickOutputWidth(0, 1920, 2560) == 1920);
    CHECK(PickOutputWidth(0, 0, 2560) == 2560);
    CHECK(PickOutputWidth(0, 0, 0) == 0);

    // ---- eligibility -------------------------------------------------------------------------------------------------
    {
        constexpr unsigned kLinear = 0x15;     // D3D11_FILTER_MIN_MAG_MIP_LINEAR
        constexpr unsigned kAniso = 0x55;      // D3D11_FILTER_ANISOTROPIC
        constexpr unsigned kComparison = 0x95; // D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR
        constexpr unsigned kMinimum = 0x115;   // D3D11_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR
        constexpr unsigned kMaximum = 0x195;   // D3D11_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR
        constexpr unsigned kCompAniso = 0xD5;  // D3D11_FILTER_COMPARISON_ANISOTROPIC
        constexpr float kMax = 3.4e38f;

        CHECK(IsEligible(-1.0f, 0.0f, kMax, kLinear));
        CHECK(IsEligible(-0.5f, 0.0f, kMax, kAniso));
        CHECK(!IsEligible(0.0f, 0.0f, kMax, kLinear)); // bias must be negative
        CHECK(!IsEligible(0.5f, 0.0f, kMax, kLinear));
        CHECK(!IsEligible(-0.0f, 0.0f, kMax, kLinear));
        CHECK(!IsEligible(kNaN, 0.0f, kMax, kLinear));
        CHECK(!IsEligible(-1.0f, 0.0f, 0.0f, kLinear)); // not mipmapped (MinLOD == MaxLOD)
        CHECK(!IsEligible(-1.0f, 0.0f, kMax, kComparison));
        CHECK(!IsEligible(-1.0f, 0.0f, kMax, kMinimum));
        CHECK(!IsEligible(-1.0f, 0.0f, kMax, kMaximum));
        CHECK(!IsEligible(-1.0f, 0.0f, kMax, kCompAniso));
    }

    // ---- variant bias: the real numbers of the game's tracking family (-1 + log2(ratio), STUDY 2.3) ----------------------
    {
        constexpr double kQ = -0.584963; // Quality 1.5x: log2(2560 / 3840)
        constexpr double kP = -1.0;      // Performance 2x

        // REPLACE only applies the change since the sampler was made: the game already put the mode into its own bias.
        // A sampler made at DLAA with -1:
        CHECK(Near(VariantBias(Mode::Replace, -1.0f, 0.0, 0.0, 0.0), -1.0));       // still at DLAA: untouched, NOT 0
        CHECK(Near(VariantBias(Mode::Replace, -1.0f, kQ, 0.0, 0.0), -1.584963));   // the mode moved to Quality: -1.585
        CHECK(Near(VariantBias(Mode::Replace, -1.0f, kP, 0.0, 0.0), -2.0));
        // A sampler made at Quality with -1.5625 (the game's own value there):
        CHECK(Near(VariantBias(Mode::Replace, -1.5625f, kQ, kQ, 0.0), -1.5625));   // same mode: bit-identical to the game
        CHECK(!NeedsVariant(-1.5625f, VariantBias(Mode::Replace, -1.5625f, kQ, kQ, 0.0)));
        CHECK(Near(VariantBias(Mode::Replace, -1.5625f, 0.0, kQ, 0.0), -0.977537, 1e-5)); // back to DLAA: sharpening goes away again
        // the brief's case: a stale -1 becomes -1.585 at Quality, one made at Quality with -1.585 stays, DLAA returns -1
        CHECK(Near(VariantBias(Mode::Replace, -1.0f, kQ, 0.0, 0.0), -1.584963));
        CHECK(Near(VariantBias(Mode::Replace, -1.584963f, kQ, kQ, 0.0), -1.584963));
        CHECK(Near(VariantBias(Mode::Replace, -1.0f, 0.0, 0.0, 0.0), -1.0));
        // the offset is added on top in both modes, whatever the creation target was
        CHECK(Near(VariantBias(Mode::Replace, -1.0f, kQ, 0.0, -0.18), -1.764963));
        CHECK(Near(VariantBias(Mode::Replace, -1.5625f, kQ, kQ, -0.18), -1.7425));
        CHECK(Near(VariantBias(Mode::Replace, -1.0f, 0.0, 0.0, -0.18), -1.18));
        // the output of a replace never depends on the game's -1 being dropped: DLAA with offset 0 changes nothing
        CHECK(!NeedsVariant(-1.0f, VariantBias(Mode::Replace, -1.0f, 0.0, 0.0, 0.0)));

        // ADD is the manual setting: the game's bias plus the target now (plus the offset), whatever the creation target was
        CHECK(Near(VariantBias(Mode::Add, -0.5f, kQ, 0.0, 0.0), -1.084963));          // art -0.5 stays -0.5 + target
        CHECK(Near(VariantBias(Mode::Add, -0.5f, kQ, kQ, 0.0), -1.084963));
        CHECK(Near(VariantBias(Mode::Add, -1.0f, kQ, 0.0, 0.0), -1.584963));
        CHECK(Near(VariantBias(Mode::Add, -1.5f, 0.0, 0.0, 0.0), -1.5));             // DLAA: add keeps the game's value
        CHECK(Near(VariantBias(Mode::Add, -1.0f, kQ, 0.0, -0.18), -1.764963));        // the old module's number: base + offset
        CHECK(Near(VariantBias(Mode::Add, -1.0f, 0.0, 0.0, -0.18), -1.18));

        // the clamps of D3D11 and numbers that are not numbers
        CHECK(Near(VariantBias(Mode::Add, -15.5f, -2.0, 0.0, 0.0), -16.0));
        CHECK(Near(VariantBias(Mode::Add, 15.0f, 20.0, 0.0, 0.0), 15.99, 1e-5)); // the upper one (the target itself is limited to +-15)
        CHECK(VariantBias(Mode::Add, -1.0f, std::nan(""), 0.0, 0.0) == -1.0f);
        CHECK(VariantBias(Mode::Replace, -1.0f, 0.0, std::nan(""), 0.0) == -1.0f);
        CHECK(!NeedsVariant(-1.0f, -1.0f));
        CHECK(NeedsVariant(-1.0f, -1.5f));
    }

    // ---- a steady target ---------------------------------------------------------------------------------------------
    {
        TargetGate g;
        // the first evaluate is taken
        auto r = g.OnEvaluate(3840, -0.584963, 1000);
        CHECK(r.kind == GateResult::Kind::Applied && r.first && Near(r.base, -0.584963));
        // noise inside the dead band of 1/64 is not a change
        r = g.OnEvaluate(3840, -0.584963 + 0.005, 1100);
        CHECK(r.kind == GateResult::Kind::Unchanged);
        r = g.OnEvaluate(3840, -0.584963 - 0.0155, 1200);
        CHECK(r.kind == GateResult::Kind::Unchanged); // just under one step
        // a real move after the rate limit
        r = g.OnEvaluate(3840, -1.0, 1250 - 1);
        CHECK(r.kind == GateResult::Kind::Deferred); // less than 250 ms after the first change
        r = g.OnEvaluate(3840, -1.0, 1000 + kMinApplyIntervalMs);
        CHECK(r.kind == GateResult::Kind::Applied && !r.first && Near(r.base, -1.0));
        CHECK(Near(g.applied(), -1.0));

        // the same rate limit under a flood of changes: at most four per second
        TargetGate flood;
        int applied = 0;
        for (int i = 0; i < 200; ++i) // 200 evaluates at 5 ms with a base moving by 0.05 each time
        {
            const auto res = flood.OnEvaluate(3840, 0.05 * i * ((i & 1) ? 1 : -1), 10000 + 5ull * i);
            applied += res.kind == GateResult::Kind::Applied ? 1 : 0;
        }
        CHECK(applied >= 3 && applied <= 5); // 1 s of evaluates: the first, then one per 250 ms

        // another feature at another size (a second window, a preview) alternating with the main one is ignored
        TargetGate two;
        two.OnEvaluate(3840, -0.584963, 0);
        int changes = 0, ignored = 0;
        for (int i = 1; i <= 400; ++i)
        {
            const bool main = (i & 1) == 0;
            const auto res = two.OnEvaluate(main ? 3840 : 1152, main ? -0.584963 : -1.7, 16ull * i);
            changes += res.kind == GateResult::Kind::Applied ? 1 : 0;
            ignored += res.kind == GateResult::Kind::Ignored ? 1 : 0;
        }
        CHECK(changes == 0 && ignored == 200);
        CHECK(Near(two.applied(), -0.584963) && two.anchor() == 3840);

        // a real resolution change (the other size is now the only one) moves the anchor after two seconds
        TargetGate resize;
        resize.OnEvaluate(3840, -0.584963, 0);
        GateResult last;
        std::uint64_t t = 0;
        for (int i = 0; i < 100; ++i)
        {
            t += 20;
            last = resize.OnEvaluate(2560, -0.2, t);
            CHECK(last.kind == GateResult::Kind::Ignored || t >= 2000);
            if (last.kind != GateResult::Kind::Ignored)
                break;
        }
        CHECK(last.kind == GateResult::Kind::Applied && last.anchorMoved && resize.anchor() == 2560 && Near(last.base, -0.2));

        // ... or after 120 evaluates in a row
        TargetGate fast;
        fast.OnEvaluate(3840, -0.584963, 0);
        GateResult r120;
        for (unsigned i = 1; i <= kReanchorEvaluates; ++i)
            r120 = fast.OnEvaluate(2560, -0.2, i); // 1 ms apart: far under 2 s
        CHECK(r120.kind == GateResult::Kind::Applied && r120.anchorMoved);

        // bad input
        TargetGate bad;
        CHECK(bad.OnEvaluate(0, -0.5, 0).kind == GateResult::Kind::Ignored);
        CHECK(bad.OnEvaluate(3840, std::nan(""), 0).kind == GateResult::Kind::Ignored);
        CHECK(!bad.haveApplied());
    }

    // ---- the family of a sampler (everything but the bias) -----------------------------------------------------------
    {
        const float border[4] = { 0, 0, 0, 0 };
        const float border2[4] = { 1, 0, 0, 0 };
        const auto base = FamilyHash(0x15, 1, 1, 1, 1, 1, border, 0.0f, 1000.0f);
        CHECK(base == FamilyHash(0x15, 1, 1, 1, 1, 1, border, 0.0f, 1000.0f));
        CHECK(base != FamilyHash(0x55, 1, 1, 1, 1, 1, border, 0.0f, 1000.0f)); // filter
        CHECK(base != FamilyHash(0x15, 3, 1, 1, 1, 1, border, 0.0f, 1000.0f)); // address mode
        CHECK(base != FamilyHash(0x15, 1, 3, 1, 1, 1, border, 0.0f, 1000.0f));
        CHECK(base != FamilyHash(0x15, 1, 1, 3, 1, 1, border, 0.0f, 1000.0f));
        CHECK(base != FamilyHash(0x15, 1, 1, 1, 8, 1, border, 0.0f, 1000.0f)); // anisotropy
        CHECK(base != FamilyHash(0x15, 1, 1, 1, 1, 2, border, 0.0f, 1000.0f)); // comparison
        CHECK(base != FamilyHash(0x15, 1, 1, 1, 1, 1, border2, 0.0f, 1000.0f)); // border colour
        CHECK(base != FamilyHash(0x15, 1, 1, 1, 1, 1, border, 1.0f, 1000.0f));  // LOD range
        CHECK(base != FamilyHash(0x15, 1, 1, 1, 1, 1, border, 0.0f, 10.0f));
    }
    CHECK(std::string(ModeText(Mode::Add)) == "add");
    CHECK(std::string(ModeText(Mode::Replace)) == "replace");

    if (g_failed == 0)
        std::printf("mip_bind_math: all passed\n");
    return g_failed == 0 ? 0 : 1;
}
