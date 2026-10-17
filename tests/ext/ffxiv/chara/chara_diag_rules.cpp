#include "check.h"
#include "ext/ffxiv/chara/CharaDiagRules.h"

#include <cmath>
#include <limits>
#include <string>

using namespace ext::ffxiv::chara::diag;

namespace
{
void TestSizes()
{
    constexpr uint64_t pixels = 576ull * 960;
    CHECK(FileBytes(Role::Native, 576, 960) == kHeaderBytes + pixels * 4);
    CHECK(FileBytes(Role::Input, 576, 960) == kHeaderBytes + pixels * 4);
    CHECK(FileBytes(Role::Dlss, 576, 960) == kHeaderBytes + pixels * 4 * 8);       // 2x in both directions, 8 bytes per pixel
    CHECK(FileBytes(Role::Composed, 576, 960) == kHeaderBytes + pixels * 4 * 4);
    CHECK(FileBytes(Role::Depth, 576, 960) == kHeaderBytes + pixels * 4);
    CHECK(FileBytes(Role::Motion, 576, 960) == kHeaderBytes + pixels * 4);
    CHECK(DumpBytes(576, 960) == 6 * kHeaderBytes + pixels * (4 + 4 + 32 + 16 + 4 + 4));
    CHECK(std::string(RoleName(Role::Native)) == "native" && std::string(RoleName(Role::Motion)) == "motion");

    CHECK(SizeAllowed(576, 960));
    CHECK(SizeAllowed(1448, 1448));   // 2,096,704 pixels <= 0x200000
    CHECK(!SizeAllowed(1449, 1449));
    CHECK(!SizeAllowed(0, 960) && !SizeAllowed(576, 0));
}

void TestBudget()
{
    Budget b;
    const uint64_t dump = DumpBytes(576, 960);
    CHECK(b.Fits(6, dump));
    b.Add(6, dump);
    CHECK(b.Fits(6, dump));   // the second dump fits: 12 files, about 67 MiB
    b.Add(6, dump);
    CHECK(b.files == 12);
    CHECK(!b.Fits(6, dump));  // a third would be 18 files
    CHECK(!b.Fits(1, 1));     // not even one more file

    // the byte cap is checked before the write, and a dump that would cross it is refused as a whole
    Budget big;
    big.Add(0, kMaxBytes - 100);
    CHECK(big.Fits(1, 100));
    CHECK(!big.Fits(1, 101));
    CHECK(!big.Fits(1, std::numeric_limits<uint64_t>::max())); // no overflow
    Budget one;
    CHECK(!one.Fits(6, kMaxBytes + 1));
}

void TestPhases()
{
    PhaseGate g;
    CHECK(g.Next(1, 1000) == 0);  // the first evaluation
    g.Started(0, 1000);
    CHECK(g.Next(2, 1100) == -1); // in flight
    CHECK(g.Next(500, 2000) == -1); // the second phase waits for the first to finish
    CHECK(g.Next(2, 6000) == 0);  // never finished: armed again after 5 s
    g.Finished(0);
    CHECK(g.Next(2, 6100) == -1);
    CHECK(g.Next(120, 6100) == -1); // 120 evaluations are not "passed 120"
    CHECK(g.Next(121, 6100) == 1);
    g.Started(1, 6100);
    CHECK(g.Next(122, 6200) == -1);
    g.Finished(1);
    CHECK(g.Done());
    CHECK(g.Next(100000, 99999) == -1);
}

void TestHalfAndStats()
{
    CHECK(HalfToFloat(0x3C00) == 1.0f);
    CHECK(HalfToFloat(0xBC00) == -1.0f);
    CHECK(HalfToFloat(0x0000) == 0.0f);
    CHECK(HalfToFloat(0x3800) == 0.5f);
    CHECK(HalfToFloat(0xC000) == -2.0f);
    CHECK(std::fabs(HalfToFloat(0x0001) - 5.9604645e-8f) < 1e-12f); // smallest subnormal
    CHECK(std::isinf(HalfToFloat(0x7C00)) && HalfToFloat(0x7C00) > 0);
    CHECK(std::isnan(HalfToFloat(0x7E00)));

    MotionStats s;
    s.Add(0.0f, 0.0f);
    s.Add(1.5f, -0.5f);
    s.Add(-2.0f, 0.25f);
    s.Add(std::numeric_limits<float>::quiet_NaN(), 0.0f);
    s.Add(0.0f, std::numeric_limits<float>::infinity());
    CHECK(s.total == 5 && s.invalid == 2 && s.nonZero == 2);
    CHECK(s.minX == -2.0f && s.maxX == 1.5f && s.minY == -0.5f && s.maxY == 0.25f);

    MotionStats first; // the first valid sample sets the range, even when it is negative
    first.Add(-1.0f, -3.0f);
    CHECK(first.minX == -1.0f && first.maxX == -1.0f && first.minY == -3.0f && first.maxY == -3.0f);
}

void TestTraceLimiter()
{
    TraceLimiter t;
    CHECK(t.Allow(0, 1000));
    CHECK(!t.Allow(0, 1499));   // one line per 500 ms per category
    CHECK(t.Allow(0, 1500));
    CHECK(t.Allow(1, 1000));    // another category is independent
    CHECK(!t.Allow(99, 1000));  // an unknown category is refused
    TraceLimiter flood;
    uint32_t lines = 0;
    for (uint64_t now = 0; now < 1000000; now += 500)
        lines += flood.Allow(0, now) ? 1 : 0;
    CHECK(lines == TraceLimiter::kMaxLines); // capped overall
}
} // namespace

int main()
{
    TestSizes();
    TestBudget();
    TestPhases();
    TestHalfAndStats();
    TestTraceLimiter();
    return TEST_RESULT("chara_diag_rules");
}
