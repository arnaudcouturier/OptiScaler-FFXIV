#include "check.h"
#include "ext/ffxiv/chara/CharaDlssNgx.h"
#include "ext/ffxiv/chara/CharaJitter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

using namespace ext::ffxiv::chara;

namespace
{
bool Near(float a, double b) { return std::fabs(a - b) < 1e-6; }

// The test's own radical inverse, independent of the header's: the digits of n in `base` reversed, kept as an exact
// fraction numerator / denominator (denominator = base^digits) so the value is not accumulated in floating point.
struct Fraction
{
    uint64_t numerator = 0;
    uint64_t denominator = 1;
    double Value() const { return static_cast<double>(numerator) / static_cast<double>(denominator); }
};

Fraction ReverseDigits(uint32_t n, uint32_t base)
{
    Fraction f;
    uint64_t rest = n;
    while (rest > 0)
    {
        f.numerator = f.numerator * base + (rest % base); // appends the lowest remaining digit
        f.denominator *= base;
        rest /= base;
    }
    return f;
}

// What PassJitter(pass) must return, written from the rule in the header comment: n = pass % phases + 1, (RI2(n) - 0.5, RI3(n) - 0.5).
std::pair<double, double> Expected(uint32_t pass, uint32_t phases)
{
    const uint32_t n = pass % phases + 1;
    return { ReverseDigits(n, 2).Value() - 0.5, ReverseDigits(n, 3).Value() - 0.5 };
}

// The old behaviour, kept only as a negative control for the distinctness check: 16 phases selected by a mask.
Jitter MaskedJitter15(uint32_t pass)
{
    const uint32_t n = (pass & 15u) + 1;
    return { static_cast<float>(ReverseDigits(n, 2).Value() - 0.5), static_cast<float>(ReverseDigits(n, 3).Value() - 0.5) };
}

template <class F> size_t DistinctPairs(F jitterOf, uint32_t passes)
{
    std::set<std::pair<float, float>> seen;
    for (uint32_t p = 0; p < passes; ++p)
        seen.insert({ jitterOf(p).x, jitterOf(p).y });
    return seen.size();
}

void TestRadicalInverse()
{
    // base 2: 1/2, 1/4, 3/4, 1/8, 5/8, 3/8, 7/8, 1/16 ...
    const double base2[] = { 0.5, 0.25, 0.75, 0.125, 0.625, 0.375, 0.875, 0.0625, 0.5625, 0.3125, 0.8125, 0.1875, 0.6875, 0.4375, 0.9375, 0.03125 };
    for (uint32_t n = 1; n <= 16; ++n)
        CHECK(std::fabs(RadicalInverse(n, 2) - base2[n - 1]) < 1e-12);

    // base 3: 1/3, 2/3, 1/9, 4/9, 7/9, 2/9, 5/9, 8/9, 1/27, 10/27, 19/27, 4/27, 13/27, 22/27, 7/27, 16/27
    const double base3[] = { 1.0 / 3, 2.0 / 3, 1.0 / 9, 4.0 / 9, 7.0 / 9, 2.0 / 9, 5.0 / 9, 8.0 / 9,
                             1.0 / 27, 10.0 / 27, 19.0 / 27, 4.0 / 27, 13.0 / 27, 22.0 / 27, 7.0 / 27, 16.0 / 27 };
    for (uint32_t n = 1; n <= 16; ++n)
        CHECK(std::fabs(RadicalInverse(n, 3) - base3[n - 1]) < 1e-12);

    CHECK(RadicalInverse(0, 2) == 0.0);

    // the test's own exact-fraction loop agrees with the header's for every n the preview can use, in both bases
    for (uint32_t n = 1; n <= 128; ++n)
    {
        CHECK(std::fabs(RadicalInverse(n, 2) - ReverseDigits(n, 2).Value()) < 1e-12);
        CHECK(std::fabs(RadicalInverse(n, 3) - ReverseDigits(n, 3).Value()) < 1e-12);
    }
}

void TestPassJitter()
{
    CHECK(kJitterPhases == 32);

    // pass 0 uses phase n = 1: (0.5 - 0.5, 1/3 - 0.5)
    CHECK(Near(PassJitter(0).x, 0.0) && Near(PassJitter(0).y, 1.0 / 3 - 0.5));
    // pass 1: n = 2: (0.25 - 0.5, 2/3 - 0.5)
    CHECK(Near(PassJitter(1).x, -0.25) && Near(PassJitter(1).y, 2.0 / 3 - 0.5));
    // pass 2: n = 3: (0.75 - 0.5, 1/9 - 0.5)
    CHECK(Near(PassJitter(2).x, 0.25) && Near(PassJitter(2).y, 1.0 / 9 - 0.5));
    // pass 15: n = 16
    CHECK(Near(PassJitter(15).x, 0.03125 - 0.5) && Near(PassJitter(15).y, 16.0 / 27 - 0.5));
    // pass 16: n = 17: RI2 = 1/32, RI3 = 17 = 122 in base 3 -> 2/3^1... = 1/3 + 2/9 ... written as 25/27
    CHECK(Near(PassJitter(16).x, 0.03125) && Near(PassJitter(16).y, 25.0 / 27 - 0.5));
    // pass 31: n = 32: RI2 = 1/64 -> 1/64 - 0.5 = -0.484375, RI3(32) = 64/81
    CHECK(Near(PassJitter(31).x, 1.0 / 64 - 0.5) && Near(PassJitter(31).y, 64.0 / 81 - 0.5));
    CHECK(Near(PassJitter(31).x, -0.484375) && Near(PassJitter(31).y, 64.0 / 81 - 0.5));

    // n = pass % 32 + 1 against the independent exact loop, over four cycles
    for (uint32_t p = 0; p < 128; ++p)
    {
        const auto e = Expected(p, 32);
        CHECK(Near(PassJitter(p).x, e.first) && Near(PassJitter(p).y, e.second));
    }

    // 32 phases, then it repeats
    for (uint32_t p = 0; p < 256; ++p)
    {
        CHECK(PassJitter(p).x == PassJitter(p + 32).x && PassJitter(p).y == PassJitter(p + 32).y);
        CHECK(PassJitter(p).x > -0.5f && PassJitter(p).x < 0.5f && PassJitter(p).y > -0.5f && PassJitter(p).y < 0.5f);
    }
    // a pass counter near the top of the range wraps like any other (no mask assumption on the phase count)
    CHECK(PassJitter(0xFFFFFFFFu).x == PassJitter(0xFFFFFFFFu % 32).x);

    // the 32 phases are distinct (a version that still masks with 15 only has 16: negative control)
    CHECK(DistinctPairs([](uint32_t p) { return PassJitter(p); }, 32) == 32);
    CHECK(DistinctPairs([](uint32_t p) { return PassJitter(p); }, 1024) == 32);
    CHECK(DistinctPairs(MaskedJitter15, 32) == 16);
}

void TestMeanOffset()
{
    // the sequence is centred: the mean over a full cycle is close to zero (study values: N = 16 (-0.0293, -0.0370), N = 32 (-0.0151, -0.0185))
    double sx = 0, sy = 0;
    for (uint32_t p = 0; p < 16; ++p)
    {
        const auto e = Expected(p, 16); // the first 16 phases of the old cycle
        sx += e.first;
        sy += e.second;
    }
    CHECK(std::fabs(sx / 16 - -0.0293) < 5e-4 && std::fabs(sy / 16 - -0.0370) < 5e-4);

    sx = sy = 0;
    for (uint32_t p = 0; p < kJitterPhases; ++p)
    {
        sx += PassJitter(p).x;
        sy += PassJitter(p).y;
    }
    CHECK(std::fabs(sx / kJitterPhases - -0.0151) < 5e-4 && std::fabs(sy / kJitterPhases - -0.0185) < 5e-4);
    CHECK(std::fabs(sx / kJitterPhases) < 0.05 && std::fabs(sy / kJitterPhases) < 0.05);
}

// Coverage: split the jitter square [-0.5, 0.5) in k x k cells, cell = floor((v + 0.5) * k) (a value on a cell edge belongs to the
// cell above it), and count how many of the first `phases` samples fall in each cell.
struct Coverage
{
    uint32_t minCount = 0;
    uint32_t maxCount = 0;
    uint32_t emptyCells = 0;
};

Coverage CoverageOf(uint32_t phases, uint32_t k)
{
    std::vector<uint32_t> counts(static_cast<size_t>(k) * k, 0);
    for (uint32_t p = 0; p < phases; ++p)
    {
        const auto e = Expected(p, phases);
        auto cell = [k](double v) { return std::min<uint32_t>(k - 1, static_cast<uint32_t>(std::floor((v + 0.5) * k))); };
        ++counts[static_cast<size_t>(cell(e.second)) * k + cell(e.first)];
    }
    Coverage c;
    c.minCount = counts[0];
    for (uint32_t v : counts)
    {
        c.minCount = std::min(c.minCount, v);
        c.maxCount = std::max(c.maxCount, v);
        c.emptyCells += v == 0 ? 1u : 0u;
    }
    return c;
}

void TestCoverage()
{
    // 2x upscale: one render pixel covers 2 x 2 output pixels. The numbers are the ones the study derived: 16 phases give 3..5 samples per
    // output pixel per cycle, 32 phases give 7..9 (the rule's target is 8).
    const Coverage c16 = CoverageOf(16, 2), c32 = CoverageOf(32, 2);
    CHECK(c16.minCount == 3 && c16.maxCount == 5 && c16.emptyCells == 0);
    CHECK(c32.minCount == 7 && c32.maxCount == 9 && c32.emptyCells == 0);

    // finer, half-output-pixel grid: 16 phases leave 2 empty cells, 32 leave none (1..3 per cell)
    const Coverage f16 = CoverageOf(16, 4), f32 = CoverageOf(32, 4);
    CHECK(f16.emptyCells == 2);
    CHECK(f32.emptyCells == 0 && f32.minCount == 1 && f32.maxCount == 3);

    // other ratios: 2x2 cells at 1.5x / 1.7x, 3x3 cells at 3x
    CHECK(CoverageOf(18, 2).minCount == 4);
    CHECK(CoverageOf(23, 2).minCount == 5);
    CHECK(CoverageOf(16, 3).minCount == 1);
    CHECK(CoverageOf(32, 3).minCount == 3);
    const Coverage c72 = CoverageOf(72, 3);
    CHECK(c72.minCount == 7);
}

// An independent copy of FSR's rule (external/FidelityFX-SDK-v2 ffx_fsr3upscaler.cpp: int32_t(8 * pow(display / render, 2)), widths only).
int32_t FsrPhaseCount(int32_t renderWidth, int32_t displayWidth)
{
    const float ratio = static_cast<float>(displayWidth) / static_cast<float>(renderWidth);
    return static_cast<int32_t>(8.0f * std::pow(ratio, 2.0f));
}

void TestRecommendedPhases()
{
    // table: 1:1 -> 8, 4:3 -> 14, 3:2 -> 18, 2:1 -> 32, 3:1 -> 72 (integer floor of 8 * out^2 / render^2)
    CHECK(RecommendedPhases(1000, 1000) == 8);
    CHECK(RecommendedPhases(1440, 1920) == 14);
    CHECK(RecommendedPhases(1280, 1920) == 18);
    CHECK(RecommendedPhases(1920, 3840) == 32);
    CHECK(RecommendedPhases(1, 2) == 32);
    CHECK(RecommendedPhases(1280, 3840) == 72);
    // 1.7x (Balanced): 23
    CHECK(RecommendedPhases(1000, 1700) == 23);

    // zero sizes give 0 (no recommendation), and so does a width beyond what the integer maths covers
    CHECK(RecommendedPhases(0, 1920) == 0 && RecommendedPhases(1920, 0) == 0 && RecommendedPhases(0, 0) == 0);
    CHECK(RecommendedPhases(1, (1u << 20) + 1) == 0 && RecommendedPhases((1u << 20) + 1, 1) == 0);
    CHECK(RecommendedPhases(0xFFFFFFFFu, 0xFFFFFFFFu) == 0);
    CHECK(RecommendedPhases((1u << 20) + 1, 1u << 20) == 0); // the render-width bound is its own condition (this ratio would give 7)
    // the widest accepted ratio saturates instead of wrapping
    CHECK(RecommendedPhases(1, 1u << 20) == 0xFFFFFFFFu);
    // large equal widths do not overflow the 64-bit intermediate: 1:1 is still 8
    CHECK(RecommendedPhases(1u << 20, 1u << 20) == 8);
    CHECK(RecommendedPhases(500000, 1000000) == 32);

    // sweep against the FSR rule (a float computation): equal, or off by exactly one where the exact value 8 * out^2 / render^2 sits
    // within float precision of the integer k = the larger of the two answers (the float result landed on the other side of it)
    uint32_t compared = 0, differing = 0;
    for (uint32_t render = 640; render <= 3840; render += 160)
    {
        for (uint32_t out = render; out <= 4 * render; out += 40)
        {
            const int32_t fsr = FsrPhaseCount(static_cast<int32_t>(render), static_cast<int32_t>(out));
            const int64_t mine = RecommendedPhases(render, out);
            const int64_t diff = mine - fsr;
            ++compared;
            CHECK(diff >= -1 && diff <= 1);
            if (diff != 0)
            {
                ++differing;
                const int64_t k = mine > fsr ? mine : fsr;
                const int64_t exact = static_cast<int64_t>(8ull * out * out);
                const int64_t boundary = k * static_cast<int64_t>(render) * render;
                const int64_t gap = exact > boundary ? exact - boundary : boundary - exact;
                CHECK(gap * 1000000 <= boundary); // within 1e-6 of the integer boundary
                // and the integer maths is the exact one: floor(exact / render^2)
                CHECK(mine == exact / (static_cast<int64_t>(render) * render));
            }
        }
    }
    CHECK(compared > 1000);
    CHECK(differing < compared / 20); // the differences are the exception

    // monotonic in the ratio (fixed render width, growing output width) and in the render width (shrinking render width)
    uint32_t last = 0;
    for (uint32_t out = 1000; out <= 4000; ++out)
    {
        const uint32_t v = RecommendedPhases(1000, out);
        CHECK(v >= last);
        last = v;
    }
    last = 0xFFFFFFFFu;
    for (uint32_t render = 400; render <= 4000; ++render)
    {
        const uint32_t v = RecommendedPhases(render, 3840);
        CHECK(v <= last);
        last = v;
    }
}

void TestTiedToPreviewScale()
{
    // The preview's DLSS output is kOutputScale times its render size; the phase count follows from that ratio. If the preview scale
    // changes without kJitterPhases, this fails.
    static_assert(ext::ffxiv::chara::dlss::ngx::kOutputScale == 2);
    CHECK(kJitterPhases == RecommendedPhases(1, ext::ffxiv::chara::dlss::ngx::kOutputScale));
    CHECK(kJitterPhases == RecommendedPhases(576, 576 * ext::ffxiv::chara::dlss::ngx::kOutputScale));
}

void TestViewportAndAlpha()
{
    const Jitter j { 0.25f, -0.125f };
    const auto v = ShiftViewport(0.0f, 0.0f, j);
    CHECK(v.topLeftX == 0.25f && v.topLeftY == -0.125f);
    const auto w = ShiftViewport(10.0f, 20.0f, j);
    CHECK(w.topLeftX == 10.25f && w.topLeftY == 19.875f);

    // the alpha is read at uv + jitter / size: the content of the jittered image is displaced by +jitter
    const auto uv = AlphaUvOffset(Jitter { 0.5f, -0.25f }, 576.0f, 960.0f);
    CHECK(Near(uv.u, 0.5 / 576) && Near(uv.v, -0.25 / 960));
    const auto zero = AlphaUvOffset(Jitter { 0.5f, 0.5f }, 0.0f, 0.0f);
    CHECK(zero.u == 0.0f && zero.v == 0.0f);
}
} // namespace

int main()
{
    TestRadicalInverse();
    TestPassJitter();
    TestMeanOffset();
    TestCoverage();
    TestRecommendedPhases();
    TestTiedToPreviewScale();
    TestViewportAndAlpha();
    return TEST_RESULT("chara_jitter");
}
