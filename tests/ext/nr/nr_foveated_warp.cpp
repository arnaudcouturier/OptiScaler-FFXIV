// Foveated NR profile: the host maths (FoveatedWarp.h). Everything here is checked against references written in this file:
// the packed distance against a numerical integral of the density, the round trip against both directions, the sizes against
// hand-worked numbers for 3840x2160.
#include "ext/nr/spatial/FoveatedWarp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
int failures = 0;
void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

using namespace DlssNr::Foveated;

bool Near(double a, double b, double tolerance) { return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance; }

// ---- references ---------------------------------------------------------------------------------------------------------
double RefDensity(double r, double h, double F, double e)
{
    if (r <= h)
        return 1.0;
    if (r >= h + F)
        return e;
    const double t = (r - h) / F;
    return 1.0 - (1.0 - e) * (t * t * (3.0 - 2.0 * t));
}

// Composite Simpson integral of the density from 0 to r: the packed distance without the closed form.
double RefPacked(double r, double h, double F, double e)
{
    const int n = 40000;
    const double step = r / n;
    double sum = RefDensity(0, h, F, e) + RefDensity(r, h, F, e);
    for (int i = 1; i < n; ++i)
        sum += RefDensity(i * step, h, F, e) * (i % 2 ? 4.0 : 2.0);
    return sum * step / 3.0;
}

AxisInput Input(uint32_t extent, float centre, float falloff, float edge, float offset = 0)
{
    AxisInput in;
    in.extent = extent;
    in.centre = centre;
    in.falloff = falloff;
    in.edge = edge;
    in.offsetPercent = offset;
    return in;
}

Side MakeSide(float span, float h, float F, float e)
{
    Side s;
    s.span = span;
    s.centre = h;
    s.falloff = F;
    s.edge = e;
    s.packedFalloffEnd = static_cast<float>(h + F - (1.0 - e) * F / 2.0);
    return s;
}

// ---- tests --------------------------------------------------------------------------------------------------------------
void PackedDistanceAgainstTheIntegral()
{
    struct Case { double h, F, e; };
    const Case cases[] = { { 1200, 400, .5 }, { 675, 400, .5 }, { 300, 800, .25 }, { 0, 500, .25 }, { 50, 100, .9 }, { 1000, 1, .3 } };
    for (const Case& c : cases)
    {
        const Side side = MakeSide(5000, (float) c.h, (float) c.F, (float) c.e);
        for (double r : { 0.0, c.h * .5, c.h, c.h + c.F * .1, c.h + c.F * .25, c.h + c.F * .5, c.h + c.F * .9, c.h + c.F,
                          c.h + c.F + 1, c.h + c.F + 123.5 })
        {
            if (r <= 0)
                continue;
            const double want = RefPacked(r, c.h, c.F, c.e);
            const double got = PackedDistance(side, (float) r);
            CHECK(Near(got, want, 2e-3));
        }
    }
}

void PackedDistanceIsContinuousAndMonotone()
{
    const Side side = MakeSide(1920, 1200, 400, .5f);
    // the three pieces meet at h and h + F
    CHECK(Near(PackedDistance(side, 1200), 1200, 1e-4));
    CHECK(Near(PackedDistance(side, 1600), 1500, 1e-3)); // h + F - (1 - e) F / 2
    CHECK(Near(PackedDistance(side, 1600 - 1e-2f), 1500 - 1e-2 * .5, 2e-3));
    CHECK(Near(PackedDistance(side, 1600 + 1e-2f), 1500 + 1e-2 * .5, 2e-3));
    CHECK(Near(PackedDistance(side, 1200 + 1e-2f), 1200 + 1e-2, 2e-3));
    CHECK(Near(PackedDistance(side, 1920), 1660, 1e-3)); // plateau slope e after h + F
    float previous = -1;
    for (int i = 0; i <= 4000; ++i)
    {
        const float r = i * .5f;
        const float g = PackedDistance(side, r);
        CHECK(g > previous || i == 0);
        if (i > 0)
            CHECK((g - previous) >= .5f * .5f * .99f); // never flatter than the edge density
        previous = g;
    }
    // the smallest edge density keeps the packing strictly increasing
    const Side tight = MakeSide(2000, 100, 500, .25f);
    previous = -1;
    for (int i = 0; i <= 2000; ++i)
    {
        const float g = PackedDistance(tight, (float) i);
        CHECK(g > previous || i == 0);
        previous = g;
    }
    // no falloff: a hard step from density 1 to e
    const Side step = MakeSide(1000, 400, 0, .5f);
    CHECK(Near(PackedDistance(step, 400), 400, 1e-4) && Near(PackedDistance(step, 600), 500, 1e-4));
}

void UnpackedDistanceInvertsThePackedDistance()
{
    const Side sides[] = { MakeSide(1920, 1200, 400, .5f), MakeSide(1080, 675, 400, .5f), MakeSide(336 + 1200, 1200, 336, .25f),
                           MakeSide(2000, 100, 500, .25f), MakeSide(900, 400, 0, .5f),
                           // a long falloff at the lowest edge density: the case that needs all four Newton steps (three leave
                           // about 6e-3 px at F = 1920, e = 0.25, above the 3e-3 bound)
                           MakeSide(3840, 100, 1920, .25f), MakeSide(3840, 100, 3000, .25f) };
    for (const Side& side : sides)
        for (int i = 0; i <= 2400; ++i)
        {
            const float r = i * side.span * 1.05f / 2400.0f; // includes points beyond the frame edge
            CHECK(Near(UnpackedDistance(side, PackedDistance(side, r)), r, 3e-3));
        }
}

void AxisRoundTripAndShape()
{
    const Axis ax = BuildAxis(Input(3840, 2400, 400, .5f), 1.0f);
    CHECK(ax.warped && ax.nativeExtent == 3840);
    for (float x = -120; x <= 3840 + 120; x += 7.3f)
    {
        CHECK(Near(Unpack(Pack(x, ax), ax), x, 3e-3));
    }
    for (float w = -120; w <= ax.workExtent + 120; w += 5.9f)
    {
        CHECK(Near(Pack(Unpack(w, ax), ax), w, 3e-3));
    }
    // the ends of the frame land on the ends of the working extent
    CHECK(Near(Pack(0, ax), 0, 2e-3) && Near(Pack(3840, ax), ax.workExtent, 2e-3));
    // centre zone: exactly the uniform resolution times the scale; plateau: the edge density times the scale
    CHECK(Near(Pack(2000, ax) - Pack(1900, ax), 100 * ax.scale, 1e-3));
    CHECK(Near(Pack(1920 + 1150, ax) - Pack(1920 + 1050, ax), 100 * ax.scale, 1e-3));
    CHECK(Near(Pack(3800, ax) - Pack(3700, ax), 50 * ax.scale, 1e-3));
    CHECK(Near(Pack(100, ax) - Pack(0, ax), 50 * ax.scale, 1e-3));
    CHECK(Near(Pack(1920, ax), ax.workCenter, 1e-3));
}

void IdentityConditions()
{
    // centre >= extent
    CHECK(!BuildAxis(Input(3840, 3840, 400, .5f), 1.0f).warped);
    CHECK(!BuildAxis(Input(3840, 5000, 400, .5f), 1.0f).warped);
    CHECK(BuildAxis(Input(3840, 3839, 400, .5f), 1.0f).warped);
    // edge density 1 (and anything above it)
    CHECK(!BuildAxis(Input(3840, 2400, 400, 1.0f), 1.0f).warped);
    CHECK(!BuildAxis(Input(3840, 2400, 400, 7.0f), 1.0f).warped);
    CHECK(BuildAxis(Input(3840, 2400, 400, .9999f), 1.0f).warped);
    // an axis shorter than 16 pixels
    CHECK(!BuildAxis(Input(15, 4, 4, .5f), 1.0f).warped);
    CHECK(BuildAxis(Input(16, 4, 4, .5f), 1.0f).warped);
    // an identity axis packs one to one (times the working scale), centred
    const Axis id = BuildAxis(Input(3840, 4000, 400, .5f), 1.0f);
    CHECK(id.workExtent == 3840 && Near(id.scale, 1.0, 1e-6) && Near(id.meanDensity, 1.0, 1e-6));
    CHECK(Near(Pack(123.5f, id), 123.5, 1e-3) && Near(Unpack(2000.25f, id), 2000.25, 1e-3));
    const Axis idScaled = BuildAxis(Input(3840, 4000, 400, .5f), .8f);
    CHECK(idScaled.workExtent == 3072 && Near(Pack(1920, idScaled), 1536, 1e-2) && Near(Unpack(1536, idScaled), 1920, 1e-2));
    // the working extent is made even BEFORE it goes on the 16 grid (the 16 grid hides the even rounding almost everywhere): 2160 * 0.27 = 583.2
    // is 584, which lands on 592; rounding 583 straight to the grid would give 576. 2160 * 0.44 = 950.4 -> 952 -> 960 (not 944).
    CHECK(BuildAxis(Input(2160, 4000, 400, .5f), .27f).workExtent == 592 && BuildAxis(Input(2160, 4000, 400, .5f), .44f).workExtent == 960);
    // a scale that is not usable (zero, negative, not a number) works as 100%, never as a larger one
    {
        const AxisInput warped = Input(2160, 1300, 400, .5f);
        const uint32_t atOne = BuildAxis(warped, 1.0f).workExtent;
        CHECK(atOne < 2160 && BuildAxis(warped, 0.0f).workExtent == atOne && BuildAxis(warped, -1.0f).workExtent == atOne);
        CHECK(BuildAxis(warped, NAN).workExtent == atOne && BuildAxis(warped, INFINITY).workExtent == atOne);
        CHECK(BuildAxis(warped, 2.0f).workExtent > atOne);
    }
    // non-finite or absurd input fails open
    CHECK(!BuildAxis(Input(3840, NAN, 400, .5f), 1.0f).warped);
    CHECK(!BuildAxis(Input(3840, 2400, 400, NAN), 1.0f).warped);
    CHECK(!BuildAxis(Input(3840, 2400, NAN, .5f), 1.0f).warped);
    CHECK(!BuildAxis(Input(3840, 2400, 400, .5f, NAN), 1.0f).warped);
}

void ClampRules()
{
    AxisClamp c = ClampAxis(Input(3840, 2400, 400, .5f, 0));
    CHECK(!c.identity && c.centre == 2400 && c.falloff == 400 && c.edge == .5f && c.offsetPercent == 0);
    // edge density clamps to 0.25..1
    c = ClampAxis(Input(3840, 2400, 400, .1f));
    CHECK(!c.identity && c.edge == .25f);
    c = ClampAxis(Input(3840, 2400, 400, .25f));
    CHECK(!c.identity && c.edge == .25f);
    c = ClampAxis(Input(3840, 2400, 400, -3.f));
    CHECK(!c.identity && c.edge == .25f);
    // a negative falloff is no falloff; a negative centre is an empty centre
    c = ClampAxis(Input(3840, 2400, -10, .5f));
    CHECK(!c.identity && c.falloff == 0);
    c = ClampAxis(Input(3840, -50, 400, .5f));
    CHECK(!c.identity && c.centre == 0);
    // the offset keeps the centre zone inside the frame: (100 - centre%) / 2 - 0.5
    CHECK(Near(MaxOffsetPercent(62.5f), 18.25, 1e-5) && Near(MaxOffsetPercent(80), 9.5, 1e-5));
    CHECK(MaxOffsetPercent(100) == 0 && MaxOffsetPercent(150) == 0);
    c = ClampAxis(Input(3840, 2400, 400, .5f, 30));
    CHECK(Near(c.offsetPercent, 18.25, 1e-4));
    c = ClampAxis(Input(3840, 2400, 400, .5f, -30));
    CHECK(Near(c.offsetPercent, -18.25, 1e-4));
    c = ClampAxis(Input(3840, 2400, 400, .5f, 10));
    CHECK(c.offsetPercent == 10);
    c = ClampAxis(Input(3840, 2400, 400, .5f, -18.25f));
    CHECK(Near(c.offsetPercent, -18.25, 1e-4));
    // percent helper
    CHECK(Near(PercentOf(2400, 3840), 62.5, 1e-4) && PercentOf(100, 0) == 0);
}

void SideCapsAndOffsetAsymmetry()
{
    // N = 1000, centre 600 -> h = 300, offset 18 -> the band centre sits at 680: spans 680 and 320
    const Axis a = BuildAxis(Input(1000, 600, 300, .5f, 18), 1.0f);
    CHECK(Near(a.bandCenter, 680, 1e-3) && Near(a.side[0].span, 680, 1e-3) && Near(a.side[1].span, 320, 1e-3));
    CHECK(Near(a.side[0].centre, 300, 1e-3) && Near(a.side[1].centre, 300, 1e-3));
    CHECK(Near(a.side[0].falloff, 300, 1e-3));                 // room for the whole falloff on the wide side
    CHECK(Near(a.side[1].falloff, 20, 1e-3));                  // capped so h + F <= span on the narrow one
    CHECK(Near(a.side[1].packedSpan, 300 + 20 - .5 * 20 / 2, 1e-3)); // the falloff is cut by the frame edge: no plateau
    CHECK(Near(a.side[1].packedFalloffEnd, a.side[1].packedSpan, 1e-4));
    // never wider than the side, for a sweep of offsets
    for (float off = -18; off <= 18; off += 1.5f)
    {
        const Axis s = BuildAxis(Input(1000, 600, 300, .5f, off), 1.0f);
        for (const Side& side : s.side)
            CHECK(side.centre <= side.span + 1e-3f && side.centre + side.falloff <= side.span + 1e-3f);
        CHECK(Near(Unpack(Pack(333.3f, s), s), 333.3, 3e-3) && Near(Unpack(Pack(910.9f, s), s), 910.9, 3e-3));
    }
    // inside the centre zone the two sides are mirror images about the centre
    CHECK(Near(a.workCenter - Pack(680 - 250, a), Pack(680 + 250, a) - a.workCenter, 1e-3));
    // beyond it they are not: the narrow side is cut short
    CHECK(std::abs((a.workCenter - Pack(680 - 600, a)) - (Pack(680 + 300, a) - a.workCenter)) > 1.0);
    // a positive offset moves the centre zone toward the positive edge
    const Axis b = BuildAxis(Input(1000, 600, 300, .5f, 0), 1.0f);
    CHECK(Near(b.bandCenter, 500, 1e-3) && a.bandCenter > b.bandCenter);
    // the ends still land on the ends
    CHECK(Near(Pack(0, a), 0, 2e-3) && Near(Pack(1000, a), a.workExtent, 2e-3));
}

void SizesOfTheDefaults()
{
    // 3840x2160, centre 2400x1350, falloff 400, edge 0.5: hand worked. G(h) = h, G(h + F) = h + F - (1 - e) F / 2, then slope e.
    // X: 1200 + 400 - 100 = 1500, plus 0.5 * (1920 - 1600) = 160 -> 1660 per side, 3320 in all.
    // Y: 675 + 400 - 100 = 975, plus 0.5 * (1080 - 1075) = 2.5 -> 977.5 per side, 1955 in all.
    const Axis x = BuildAxis(Input(3840, 2400, 400, .5f), 1.0f);
    const Axis y = BuildAxis(Input(2160, 1350, 400, .5f), 1.0f);
    CHECK(Near(x.side[0].packedFalloffEnd, 1500, 1e-3) && Near(x.side[0].packedSpan, 1660, 1e-3));
    CHECK(Near(y.side[0].packedFalloffEnd, 975, 1e-3) && Near(y.side[0].packedSpan, 977.5, 1e-3));
    CHECK(Near(x.meanDensity, 3320.0 / 3840.0, 1e-5) && Near(y.meanDensity, 1955.0 / 2160.0, 1e-5));
    // even, on the 16 grid like every other working size: 3320 -> 3328, 1955 -> 1956 -> 1952
    CHECK(x.workExtent == 3328 && y.workExtent == 1952);
    CHECK(Near(x.scale, 3328.0 / 3320.0, 1e-5) && Near(y.scale, 1952.0 / 1955.0, 1e-5));
    Inputs in;
    in.frameW = in.outputW = 3840;
    in.frameH = in.outputH = 2160;
    const Layout l = BuildLayout(in);
    CHECK(l.active && std::strcmp(l.reason, "active") == 0);
    CHECK(l.workW == 3328 && l.workH == 1952 && l.ordinaryW == 3840 && l.ordinaryH == 2160);
    CHECK(Near(l.share, 3328.0 * 1952.0 / (3840.0 * 2160.0), 1e-6) && l.share > .78 && l.share < .79);
    // the global working scale is applied before the even rounding: 0.8 * 3320 = 2656, 0.8 * 1955 = 1564
    in.globalScale = .8f;
    const Layout s = BuildLayout(in);
    CHECK(s.active && s.workW == 2656 && s.workH == 1568); // 1564 -> 1564 even -> 1568 on the grid
    CHECK(s.ordinaryW == 3072 && s.ordinaryH == 1728);
    CHECK(Near(s.share, 2656.0 * 1568.0 / (3072.0 * 1728.0), 1e-6));
}

void OutputToFrameConversion()
{
    CHECK(Near(OutputToFrame(2400, 1920, 3840), 1200, 1e-4));
    CHECK(Near(OutputToFrame(1350, 1080, 2160), 675, 1e-4));
    CHECK(Near(OutputToFrame(2400, 3840, 3840), 2400, 1e-4));
    CHECK(Near(OutputToFrame(2400, 2560, 3840), 1600, 1e-3));
    CHECK(Near(OutputToFrame(1000, 800, 0), 1000, 1e-4)); // no output size known: as is
    // the same defaults on a 1920x1080 frame inside a 3840x2160 output give the same share as a native 3840x2160 frame
    Inputs native;
    native.frameW = native.outputW = 3840;
    native.frameH = native.outputH = 2160;
    Inputs inside = native;
    inside.frameW = 1920;
    inside.frameH = 1080;
    const Layout a = BuildLayout(native), b = BuildLayout(inside);
    CHECK(a.active && b.active);
    CHECK(Near(a.share, b.share, 1e-4) && b.workW == 1664 && b.workH == 976);
    CHECK(Near(b.x.centre, 1200, 1e-3) && Near(b.y.centre, 675, 1e-3) && Near(b.x.falloff, 200, 1e-3));
    // 2560x1440 inside the 4K output: about the same share too (within the 16 grid)
    inside.frameW = 2560;
    inside.frameH = 1440;
    const Layout c = BuildLayout(inside);
    CHECK(c.active && std::abs(c.share - a.share) < .01);
    CHECK(Near(c.x.centre, 1600, 1e-2) && Near(c.y.centre, 900, 1e-2));
    // the two axes convert with their own ratios (X by width, Y by height), and the falloff follows each axis
    inside.frameW = 1920;
    inside.frameH = 2160;
    const Layout d = BuildLayout(inside);
    CHECK(Near(d.x.centre, 1200, 1e-3) && Near(d.y.centre, 1350, 1e-3));
    CHECK(Near(d.x.falloff, 200, 1e-3) && Near(d.y.falloff, 400, 1e-3));
    // the same size as the output: the 2400 px centre in a 2560x1440 frame saves under 5%, so the profile stands down
    Inputs small;
    small.frameW = small.outputW = 2560;
    small.frameH = small.outputH = 1440;
    const Layout e = BuildLayout(small);
    CHECK(!e.active && std::strcmp(e.reason, "saving under 5%") == 0 && e.share > .95 && e.share < 1.0);
    CHECK(e.workW == e.x.workExtent && e.workH == e.y.workExtent); // the sizes are still reported
}

void AutoOffBoundary()
{
    // the saving must be at least 5%: a share of exactly 0.95 stays on, above it goes off
    CHECK(!ExceedsAutoOff(1900, 1000, 2000, 1000));   // 0.9500
    CHECK(ExceedsAutoOff(1901, 1000, 2000, 1000));    // 0.9505
    CHECK(!ExceedsAutoOff(9500, 10000, 10000, 10000)); // 0.95
    CHECK(ExceedsAutoOff(9501, 10000, 10000, 10000));  // 0.9501
    CHECK(!ExceedsAutoOff(500, 500, 1000, 1000) && !ExceedsAutoOff(1, 1, 3840, 2160));
    CHECK(ExceedsAutoOff(3840, 2160, 3840, 2160));
    // a 64-bit product: no overflow at 16k x 16k
    CHECK(!ExceedsAutoOff(15200, 16384, 16384, 16384) && ExceedsAutoOff(15600, 16384, 16384, 16384));
}

void CornersAndIndependentAxes()
{
    // different edge densities per axis: the corner density is e_x * e_y
    const Axis x = BuildAxis(Input(3840, 2400, 400, .5f), 1.0f);
    const Axis y = BuildAxis(Input(2160, 1350, 400, .25f), 1.0f);
    const double dx = (Pack(3839.0f, x) - Pack(3838.0f, x)) / x.scale;
    const double dy = (Pack(2159.0f, y) - Pack(2158.0f, y)) / y.scale;
    CHECK(Near(dx, .5, 1e-3) && Near(dy, .25, 1e-3) && Near(dx * dy, .125, 1e-3));
    CHECK(Near(x.side[0].edge, .5, 1e-6) && Near(y.side[0].edge, .25, 1e-6));
    // each axis has its own centre: changing one leaves the other alone
    const Axis y2 = BuildAxis(Input(2160, 900, 400, .25f), 1.0f);
    CHECK(y2.workExtent < y.workExtent);
    Inputs in;
    in.frameW = in.outputW = 3840;
    in.frameH = in.outputH = 2160;
    in.edgeX = .5f;
    in.edgeY = .25f;
    const Layout l = BuildLayout(in);
    CHECK(l.active && l.x.side[0].edge == .5f && l.y.side[0].edge == .25f);
    Inputs swapped = in;
    swapped.edgeX = .25f;
    swapped.edgeY = .5f;
    const Layout m = BuildLayout(swapped);
    CHECK(m.workW < l.workW && m.workH > l.workH);
}

void LayoutGuards()
{
    Inputs in;
    in.frameW = in.outputW = 3840;
    in.frameH = in.outputH = 2160;
    const Inputs before = in;
    const Layout l = BuildLayout(in);
    CHECK(l.active);
    CHECK(std::memcmp(&before, &in, sizeof(Inputs)) == 0); // the settings are never written back
    // an invalid frame or global scale
    Inputs bad = in;
    bad.frameW = 1;
    CHECK(!BuildLayout(bad).active);
    bad = in;
    bad.globalScale = .2f;
    CHECK(!BuildLayout(bad).active);
    bad.globalScale = 2.5f;
    CHECK(!BuildLayout(bad).active);
    bad.globalScale = NAN;
    CHECK(!BuildLayout(bad).active);
    // a model extent below 25% of the frame never reaches the model
    Inputs tiny = in;
    tiny.globalScale = .25f;
    tiny.edgeX = tiny.edgeY = .25f;
    tiny.width = 800;
    tiny.height = 450;
    tiny.falloff = 0;
    const Layout t = BuildLayout(tiny);
    CHECK(!t.active && std::strcmp(t.reason, "effective model extent below 25%") == 0);
    // both axes identity: nothing to save
    Inputs none = in;
    none.width = 4000;
    none.height = 3000;
    const Layout n = BuildLayout(none);
    CHECK(!n.active && !n.x.warped && !n.y.warped && std::strcmp(n.reason, "saving under 5%") == 0);
    // one axis warped is enough
    Inputs one = in;
    one.width = 4000;
    one.edgeY = .25f;
    one.height = 600;
    const Layout o = BuildLayout(one);
    CHECK(o.active && !o.x.warped && o.y.warped && o.workW == 3840 && o.workH < 2160);
}

void ShaderConstantsCarryThePerSideParameters()
{
    Inputs in;
    in.frameW = in.outputW = 3840;
    in.frameH = in.outputH = 2160;
    in.offsetX = 10;
    in.offsetY = -4;
    const Layout l = BuildLayout(in);
    CHECK(l.active);
    const pw::ShaderConstants k = MakeShaderConstants(l);
    CHECK(k.nativeWidth == 3840 && k.nativeHeight == 2160 && k.workWidth == (float) l.workW && k.workHeight == (float) l.workH);
    CHECK((k.flags & kFlagFoveated) != 0 && (k.flags & 1u) != 0 && k.mode == 2u);
    CHECK(k.bandCenterX == l.x.bandCenter && k.bandCenterY == l.y.bandCenter);
    CHECK(k.workCenterX == l.x.workCenter && k.workCenterY == l.y.workCenter);
    CHECK(k.workScaleX == l.x.scale && k.workScaleY == l.y.scale);
    CHECK(k.halfSpanNegX == l.x.side[0].span && k.halfSpanPosX == l.x.side[1].span);
    CHECK(k.halfSpanNegY == l.y.side[0].span && k.halfSpanPosY == l.y.side[1].span);
    CHECK(k.sideCenterNegX == l.x.side[0].centre && k.sideCenterPosX == l.x.side[1].centre);
    CHECK(k.sideCenterNegY == l.y.side[0].centre && k.sideCenterPosY == l.y.side[1].centre);
    CHECK(k.sideWorkNegX == l.x.side[0].falloff && k.sideWorkPosX == l.x.side[1].falloff);
    CHECK(k.sideWorkNegY == l.y.side[0].falloff && k.sideWorkPosY == l.y.side[1].falloff);
    CHECK(k.sideCompressionNegX == l.x.side[0].edge && k.sideCompressionPosY == l.y.side[1].edge);
    CHECK(k.sideEdgeSlopeNegX == l.x.side[0].packedFalloffEnd && k.sideEdgeSlopePosX == l.x.side[1].packedFalloffEnd);
    CHECK(k.sideEdgeSlopeNegY == l.y.side[0].packedFalloffEnd && k.sideEdgeSlopePosY == l.y.side[1].packedFalloffEnd);
    CHECK(k.sideWorkNegX != k.sideWorkPosX || k.halfSpanNegX != k.halfSpanPosX); // the offset made the sides differ
}

// An axis that is the identity says so with its whole extent as the centre and no falloff, whichever rule made it so.
void IdentityAxisReportsItsExtent()
{
    const AxisInput inputs[] = { Input(3840, 3840, 400, .5f),   // the centre fills the axis
                                 Input(3840, 5000, 400, .5f),   // ... or more
                                 Input(3840, 2400, 400, 1.0f),  // edge density 1
                                 Input(3840, 2400, 400, 7.0f),
                                 Input(15, 4, 4, .5f),          // shorter than 16 pixels
                                 Input(3840, NAN, 400, .5f),    // not a number
                                 Input(3840, 2400, 400, NAN),
                                 Input(3840, 2400, NAN, .5f),
                                 Input(3840, 2400, 400, .5f, NAN) };
    for (const AxisInput& in : inputs)
    {
        const AxisClamp c = ClampAxis(in);
        CHECK(c.identity && c.centre == (float) in.extent && c.falloff == 0.0f && c.edge == 1.0f && c.offsetPercent == 0.0f);
        const Axis a = BuildAxis(in, 1.0f);
        CHECK(!a.warped && a.centre == (float) in.extent && a.falloff == 0.0f && a.edge == 1.0f && a.offsetPercent == 0.0f);
        // every pixel is its own packed position, so the axis is flat all the way
        CHECK(a.workExtent == in.extent && Near(Pack(in.extent * .3f, a), in.extent * .3, 1e-3));
    }
}

// The frame and size rules of the layout: what is too small to warp, and the model extent that is too small to be worth it.
void FrameAndExtentRules()
{
    Inputs base;
    base.frameW = base.outputW = 3840;
    base.frameH = base.outputH = 2160;
    CHECK(BuildLayout(base).active);

    // a frame shorter than two pixels in either direction is not a frame (the width rule alone cannot catch a flat one)
    for (uint32_t extent : { 0u, 1u })
    {
        Inputs flat = base;
        flat.frameH = flat.outputH = extent;
        Layout l = BuildLayout(flat);
        CHECK(!l.active && std::strcmp(l.reason, "invalid frame or global scale") == 0);
        Inputs thin = base;
        thin.frameW = thin.outputW = extent;
        l = BuildLayout(thin);
        CHECK(!l.active && std::strcmp(l.reason, "invalid frame or global scale") == 0);
    }
    Inputs two = base;
    two.frameH = two.outputH = 2;
    CHECK(std::strcmp(BuildLayout(two).reason, "invalid frame or global scale") != 0);

    // The model extent may not fall under 25% of the frame on either axis. An edge density of 0.25 with no centre and no falloff
    // gives a quarter of the frame at a global scale of 1; a global scale below 1 takes it under.
    Inputs lowX = base;
    lowX.width = 0;
    lowX.falloff = 0;
    lowX.edgeX = .25f;
    lowX.globalScale = .8f;
    Layout l = BuildLayout(lowX);
    CHECK(l.workW < lowX.frameW * .25f && l.workH >= lowX.frameH * .25f); // only the width is under
    CHECK(!l.active && std::strcmp(l.reason, "effective model extent below 25%") == 0);

    Inputs lowY = base;
    lowY.height = 0;
    lowY.falloff = 0;
    lowY.edgeY = .25f;
    lowY.globalScale = .8f;
    l = BuildLayout(lowY);
    CHECK(l.workH < lowY.frameH * .25f && l.workW >= lowY.frameW * .25f); // only the height is under
    CHECK(!l.active && std::strcmp(l.reason, "effective model extent below 25%") == 0);

    // exactly a quarter is allowed, on each axis
    Inputs quarterX = base;
    quarterX.width = 0;
    quarterX.falloff = 0;
    quarterX.edgeX = .25f;
    l = BuildLayout(quarterX);
    CHECK(l.workW == 960 && (float) l.workW == quarterX.frameW * .25f && l.workH >= quarterX.frameH * .25f);
    CHECK(l.active && std::strcmp(l.reason, "active") == 0);
    Inputs quarterY = base;
    quarterY.frameH = quarterY.outputH = 2048;
    quarterY.height = 0;
    quarterY.falloff = 0;
    quarterY.edgeY = .25f;
    l = BuildLayout(quarterY);
    CHECK(l.workH == 512 && (float) l.workH == quarterY.frameH * .25f && l.workW >= quarterY.frameW * .25f);
    CHECK(l.active && std::strcmp(l.reason, "active") == 0);
}

// The shader constants carry every per-side value in its own slot. The sides of one axis are given different values in every
// field here, which the layouts the maths builds never do for the centre (the offset limit keeps the zone inside both sides).
void ShaderConstantSlotsEachHoldTheirOwnValue()
{
    Layout l;
    l.x.nativeExtent = 1000;
    l.y.nativeExtent = 600;
    l.x.workExtent = 800;
    l.y.workExtent = 480;
    l.x.centre = 300;
    l.y.centre = 150;
    l.x.edge = .4f;
    l.y.edge = .6f;
    l.x.bandCenter = 480;
    l.y.bandCenter = 310;
    l.x.workCenter = 390;
    l.y.workCenter = 250;
    l.x.scale = .81f;
    l.y.scale = .82f;
    l.x.side[0] = { 480, 101, 102, .31f, 103, 104 };
    l.x.side[1] = { 520, 201, 202, .32f, 203, 204 };
    l.y.side[0] = { 310, 111, 112, .33f, 113, 114 };
    l.y.side[1] = { 290, 211, 212, .34f, 213, 214 };
    const pw::ShaderConstants k = MakeShaderConstants(l);
    CHECK(k.nativeWidth == 1000 && k.nativeHeight == 600 && k.workWidth == 800 && k.workHeight == 480);
    CHECK(Near(k.centerFractionX, .3, 1e-6) && Near(k.centerFractionY, .25, 1e-6));
    CHECK(Near(k.workFractionX, .8, 1e-6) && Near(k.workFractionY, .8, 1e-6));
    CHECK(k.compressionX == l.x.edge && k.compressionY == l.y.edge && k.edgeSlopeX == l.x.edge && k.edgeSlopeY == l.y.edge);
    CHECK(k.mode == 2u && k.colorFilter == 1u && k.flags == (1u | kFlagFoveated) && k.reserved == 0u);
    CHECK(k.bandCenterX == 480 && k.bandCenterY == 310 && k.workCenterX == 390 && k.workCenterY == 250);
    CHECK(k.workScaleX == l.x.scale && k.workScaleY == l.y.scale);
    // negative side, then positive side, X then Y, every slot
    CHECK(k.halfSpanNegX == 480 && k.sideCenterNegX == 101 && k.sideWorkNegX == 102 && k.sideCompressionNegX == l.x.side[0].edge &&
          k.sideEdgeSlopeNegX == 103);
    CHECK(k.halfSpanPosX == 520 && k.sideCenterPosX == 201 && k.sideWorkPosX == 202 && k.sideCompressionPosX == l.x.side[1].edge &&
          k.sideEdgeSlopePosX == 203);
    CHECK(k.halfSpanNegY == 310 && k.sideCenterNegY == 111 && k.sideWorkNegY == 112 && k.sideCompressionNegY == l.y.side[0].edge &&
          k.sideEdgeSlopeNegY == 113);
    CHECK(k.halfSpanPosY == 290 && k.sideCenterPosY == 211 && k.sideWorkPosY == 212 && k.sideCompressionPosY == l.y.side[1].edge &&
          k.sideEdgeSlopePosY == 213);
    CHECK(Near(k.centerOffsetX, 480.0 / 1000.0 - .5, 1e-6) && Near(k.centerOffsetY, 310.0 / 600.0 - .5, 1e-6));
}

// centerOffset is the band centre as a signed fraction of the extent from the middle.
void CentreOffsetCarriesItsSign()
{
    Inputs in;
    in.frameW = in.outputW = 3840;
    in.frameH = in.outputH = 2160;
    const auto offsets = [&](float x, float y)
    {
        in.offsetX = x;
        in.offsetY = y;
        const Layout l = BuildLayout(in);
        CHECK(l.active);
        return MakeShaderConstants(l);
    };
    pw::ShaderConstants k = offsets(10, -4);
    CHECK(Near(k.centerOffsetX, .1, 1e-5) && Near(k.centerOffsetY, -.04, 1e-5));
    k = offsets(-10, 4);
    CHECK(Near(k.centerOffsetX, -.1, 1e-5) && Near(k.centerOffsetY, .04, 1e-5));
    k = offsets(0, 0);
    CHECK(k.centerOffsetX == 0 && k.centerOffsetY == 0);
    // the band centre moves the same way
    k = offsets(10, -4);
    CHECK(k.bandCenterX > 1920 && k.bandCenterY < 1080 && k.halfSpanNegX > k.halfSpanPosX && k.halfSpanNegY < k.halfSpanPosY);
}

// The layouts the maths builds never need the cap of the full-density half-length at the half-span: whatever the offset asked for,
// the limit keeps the zone strictly inside the narrow side (half a percent of the extent to spare, or what is left when the zone
// fills more than 99% of the axis), so both sides keep the same h.
void SideCentreIsNeverCappedByTheSpan()
{
    for (float centre : { 100.0f, 600.0f, 960.0f, 990.0f, 999.0f })
        for (float offset = -80; offset <= 80; offset += 7.5f)
        {
            const Axis a = BuildAxis(Input(1000, centre, 300, .5f, offset), 1.0f);
            CHECK(a.warped);
            for (const Side& side : a.side)
            {
                CHECK(Near(side.centre, centre * .5, 1e-3));
                CHECK(side.span - side.centre >= std::min(0.005 * 1000, (1000.0 - centre) / 2) - 1e-2 && side.span > side.centre);
            }
        }
}
} // namespace

int main()
{
    PackedDistanceAgainstTheIntegral();
    PackedDistanceIsContinuousAndMonotone();
    UnpackedDistanceInvertsThePackedDistance();
    AxisRoundTripAndShape();
    IdentityConditions();
    ClampRules();
    SideCapsAndOffsetAsymmetry();
    SizesOfTheDefaults();
    OutputToFrameConversion();
    AutoOffBoundary();
    CornersAndIndependentAxes();
    LayoutGuards();
    ShaderConstantsCarryThePerSideParameters();
    IdentityAxisReportsItsExtent();
    FrameAndExtentRules();
    ShaderConstantSlotsEachHoldTheirOwnValue();
    CentreOffsetCarriesItsSign();
    SideCentreIsNeverCappedByTheSpan();
    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: foveated NR warp maths (packed distance vs integral, round trip, clamps, identity, sizes, share, auto-off, constants)");
    return 0;
}
