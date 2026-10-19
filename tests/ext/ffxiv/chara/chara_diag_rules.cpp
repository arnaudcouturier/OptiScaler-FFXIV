#include "check.h"
#include "ext/ffxiv/chara/CharaDiagRules.h"

#include <cmath>
#include <limits>
#include <string>

using namespace ext::ffxiv::chara::diag;

namespace
{
// DXGI format values (dxgiformat.h), written as numbers so the rules header stays free of D3D.
constexpr uint32_t kRgba32Float = 2, kRgba16Float = 10, kRg32Float = 16, kRgba8 = 28, kRg16Float = 34, kR32Float = 41, kBgra8 = 87;
// The formats a dump has in the normal case: BGRA8 game picture, RGBA8 colour, RGBA16F DLSS output, RGBA8 composed, R32F depth, RG16F motion.
constexpr DumpFormats kUsual { kBgra8, kRgba8, kRgba16Float, kRgba8, kR32Float, kRg16Float };

void TestSizes()
{
    constexpr uint64_t pixels = 576ull * 960;
    CHECK(FileBytes(Role::Native, 576, 960, kBgra8) == kHeaderBytes + pixels * 4);
    CHECK(FileBytes(Role::Input, 576, 960, kRgba8) == kHeaderBytes + pixels * 4);
    CHECK(FileBytes(Role::Dlss, 576, 960, kRgba16Float) == kHeaderBytes + pixels * 4 * 8);       // 2x in both directions, 8 bytes per pixel
    CHECK(FileBytes(Role::Composed, 576, 960, kRgba8) == kHeaderBytes + pixels * 4 * 4);
    CHECK(FileBytes(Role::Depth, 576, 960, kR32Float) == kHeaderBytes + pixels * 4);
    CHECK(FileBytes(Role::Motion, 576, 960, kRg16Float) == kHeaderBytes + pixels * 4);
    CHECK(DumpBytes(576, 960, kUsual) == 6 * kHeaderBytes + pixels * (4 + 4 + 32 + 16 + 4 + 4));
    CHECK(std::string(RoleName(Role::Native)) == "native" && std::string(RoleName(Role::Motion)) == "motion");

    CHECK(SizeAllowed(576, 960));
    CHECK(SizeAllowed(1448, 1448));   // 2,096,704 pixels <= 0x200000
    CHECK(!SizeAllowed(1449, 1449));
    CHECK(!SizeAllowed(0, 960) && !SizeAllowed(576, 0));
}

// The bytes of a file follow the real format of the copied image, not the usual one of its role: the NR before DLSS image
// (RGBA16F) can be the colour input, and its rows are 8 bytes per pixel. The row copy of CharaDiag.cpp and the budget both use
// these functions; a table by role alone wrote half-width rows under a header that said RGBA16F.
void TestBytesFollowTheRealFormat()
{
    CHECK(FormatBytesPerPixel(kRgba16Float, 4) == 8);
    // the 4 byte formats of the dump are 4 whatever the role's usual size is (no over-read of a smaller image)
    CHECK(FormatBytesPerPixel(kRgba8, 8) == 4 && FormatBytesPerPixel(kBgra8, 8) == 4 && FormatBytesPerPixel(kR32Float, 8) == 4 && FormatBytesPerPixel(kRg16Float, 8) == 4);
    CHECK(FormatBytesPerPixel(kRgba8, 4) == 4 && FormatBytesPerPixel(kBgra8, 4) == 4 && FormatBytesPerPixel(kR32Float, 4) == 4 && FormatBytesPerPixel(kRg16Float, 4) == 4);
    CHECK(FormatBytesPerPixel(61, 4) == 4 && FormatBytesPerPixel(61, 8) == 8);             // an unlisted format (R8_UNORM): the fallback
    CHECK(FormatBytesPerPixel(0, 4) == 4 && FormatBytesPerPixel(0, 8) == 8);               // UNKNOWN: the role's size
    CHECK(FormatBytesPerPixel(kRgba32Float, 4) == 16);
    CHECK(FormatBytesPerPixel(kRg32Float, 4) == 8);

    constexpr uint64_t pixels = 576ull * 960;
    // An 8 byte colour input: the file and the dump are bigger by exactly 4 bytes per pixel.
    CHECK(RoleBytesPerPixel(Role::Input, kRgba16Float) == 8 && RoleBytesPerPixel(Role::Input, kRgba8) == 4);
    CHECK(FileBytes(Role::Input, 576, 960, kRgba16Float) == kHeaderBytes + pixels * 8);
    DumpFormats withNrImage = kUsual;
    withNrImage[1] = kRgba16Float;
    CHECK(DumpBytes(576, 960, withNrImage) == DumpBytes(576, 960, kUsual) + pixels * 4);
    CHECK(DumpBytes(576, 960, withNrImage) == 6 * kHeaderBytes + pixels * (4 + 8 + 32 + 16 + 4 + 4));
    // The role is still used when the format is not known; the DLSS output keeps its 8 bytes even then.
    CHECK(RoleBytesPerPixel(Role::Dlss, 0) == 8 && RoleBytesPerPixel(Role::Native, 0) == 4);
    CHECK(RoleBytesPerPixel(Role::Native, kRgba16Float) == 8); // a native image in a bigger format is not cut short either
}

void TestBudget()
{
    Budget b;
    const uint64_t dump = DumpBytes(576, 960, kUsual);
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
    TestBytesFollowTheRealFormat();
    TestBudget();
    TestPhases();
    TestHalfAndStats();
    TestTraceLimiter();
    return TEST_RESULT("chara_diag_rules");
}
