#include "check.h"
#include "ext/ffxiv/chara/CharaJitter.h"

#include <cmath>
#include <set>
#include <utility>

using namespace ext::ffxiv::chara;

namespace
{
bool Near(float a, double b) { return std::fabs(a - b) < 1e-6; }

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
}

void TestPassJitter()
{
    // pass 0 uses phase n = 1: (0.5 - 0.5, 1/3 - 0.5)
    CHECK(Near(PassJitter(0).x, 0.0) && Near(PassJitter(0).y, 1.0 / 3 - 0.5));
    // pass 1: n = 2: (0.25 - 0.5, 2/3 - 0.5)
    CHECK(Near(PassJitter(1).x, -0.25) && Near(PassJitter(1).y, 2.0 / 3 - 0.5));
    // pass 2: n = 3: (0.75 - 0.5, 1/9 - 0.5)
    CHECK(Near(PassJitter(2).x, 0.25) && Near(PassJitter(2).y, 1.0 / 9 - 0.5));
    // pass 15: n = 16
    CHECK(Near(PassJitter(15).x, 0.03125 - 0.5) && Near(PassJitter(15).y, 16.0 / 27 - 0.5));

    // 16 phases, then it repeats
    for (uint32_t p = 0; p < 64; ++p)
    {
        CHECK(PassJitter(p).x == PassJitter(p + 16).x && PassJitter(p).y == PassJitter(p + 16).y);
        CHECK(PassJitter(p).x > -0.5f && PassJitter(p).x < 0.5f && PassJitter(p).y > -0.5f && PassJitter(p).y < 0.5f);
    }

    // the 16 phases are distinct
    std::set<std::pair<float, float>> seen;
    for (uint32_t p = 0; p < 16; ++p)
        seen.insert({ PassJitter(p).x, PassJitter(p).y });
    CHECK(seen.size() == 16);
    CHECK(kJitterPhases == 16);

    // the sequence is centred: the mean over a full cycle is close to zero
    double sx = 0, sy = 0;
    for (uint32_t p = 0; p < 16; ++p)
    {
        sx += PassJitter(p).x;
        sy += PassJitter(p).y;
    }
    CHECK(std::fabs(sx / 16) < 0.05 && std::fabs(sy / 16) < 0.05);
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
    TestViewportAndAlpha();
    return TEST_RESULT("chara_jitter");
}
