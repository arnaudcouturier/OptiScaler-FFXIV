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

    // ---- variant bias ------------------------------------------------------------------------------------------------
    CHECK(Near(VariantBias(Mode::Add, -1.0f, -0.584963), -1.584963, 1e-5));
    CHECK(Near(VariantBias(Mode::Replace, -1.0f, -0.584963), -0.584963, 1e-5));
    CHECK(Near(VariantBias(Mode::Add, -1.5f, 0.0), -1.5)); // DLAA: add keeps the game's value
    CHECK(Near(VariantBias(Mode::Replace, -1.5f, 0.0), 0.0) && !std::signbit(VariantBias(Mode::Replace, -1.5f, 0.0)));
    CHECK(Near(VariantBias(Mode::Add, -15.5f, -2.0), -16.0));          // D3D11's lower clamp
    CHECK(Near(VariantBias(Mode::Replace, -1.0f, 20.0), 15.99, 1e-5)); // and its upper one
    CHECK(VariantBias(Mode::Add, -1.0f, std::nan("")) == -1.0f);       // a target that is not a number leaves the original
    CHECK(!NeedsVariant(-1.0f, -1.0f));
    CHECK(NeedsVariant(-1.0f, -1.5f));

    CHECK(std::string(ModeText(Mode::Add)) == "add");
    CHECK(std::string(ModeText(Mode::Replace)) == "replace");

    if (g_failed == 0)
        std::printf("mip_bind_math: all passed\n");
    return g_failed == 0 ? 0 : 1;
}
