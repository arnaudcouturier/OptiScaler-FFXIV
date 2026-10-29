// Per-axis model resolution ([DlssNr] WorkingScaleX / WorkingScaleY): the pure layer. The effective scale of each axis, the geometry and
// layout built from two scales, the motion-vector scale per axis and the rule that decides whether the guides are matched to the model.
#include "ext/nr/NrModelSize.h"
#include "ext/nr/presr/DlssNr_Guides.h"
#include "ext/nr/spatial/DlssNr_Spatial.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

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

using namespace DlssNr::Spatial;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

bool Near(double a, double b, double tolerance) { return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance; }

Settings Foveated()
{
    Settings s;
    s.enabled = true;
    s.profile = Profile::Foveated;
    return s;
}

Settings Peripheral()
{
    Settings s;
    s.enabled = true;
    return s;
}

bool SameGeometry(const Geometry& a, const Geometry& b)
{
    return a.workScale == b.workScale && a.workScaleX == b.workScaleX && a.workScaleY == b.workScaleY && a.frameW == b.frameW &&
           a.frameH == b.frameH && a.ordinaryW == b.ordinaryW && a.ordinaryH == b.ordinaryH && a.workW == b.workW &&
           a.workH == b.workH && a.spatial == b.spatial && a.layout == b.layout && a.layout.modelW == b.layout.modelW &&
           a.layout.modelH == b.layout.modelH && a.layout.pixelShare == b.layout.pixelShare;
}

void EffectiveAxisScaleSelectsOverrideOrFollows()
{
    using DlssNr::EffectiveAxisScale;
    // 0 follows the uniform scale, a positive override wins
    CHECK(EffectiveAxisScale(0.0f, .8f) == .8f && EffectiveAxisScale(0.0f, 1.0f) == 1.0f);
    CHECK(EffectiveAxisScale(.5f, .8f) == .5f && EffectiveAxisScale(1.5f, .8f) == 1.5f);
    CHECK(EffectiveAxisScale(.5f, .8f) != EffectiveAxisScale(.8f, .5f)); // the arguments are not interchangeable
    // anything that is not a positive number follows it
    CHECK(EffectiveAxisScale(-.5f, .8f) == .8f && EffectiveAxisScale(kNaN, .8f) == .8f && EffectiveAxisScale(kInf, .8f) == .8f);
    CHECK(EffectiveAxisScale(-kInf, .8f) == .8f && EffectiveAxisScale(-0.0f, .8f) == .8f);
    // both come out in 25%..200%
    CHECK(EffectiveAxisScale(.1f, .8f) == .25f && EffectiveAxisScale(.2499f, .8f) == .25f && EffectiveAxisScale(.25f, .8f) == .25f);
    CHECK(EffectiveAxisScale(2.0f, .8f) == 2.0f && EffectiveAxisScale(2.5f, .8f) == 2.0f);
    CHECK(EffectiveAxisScale(0.0f, .1f) == .25f && EffectiveAxisScale(0.0f, 3.0f) == 2.0f && EffectiveAxisScale(0.0f, kNaN) == 1.0f);
    CHECK(DlssNr::SanitizeWorkScale(.25f) == .25f && DlssNr::SanitizeWorkScale(2.0f) == 2.0f && DlssNr::SanitizeWorkScale(kNaN) == 1.0f);
}

void MenuPercentConversion()
{
    using DlssNr::AxisPercentOfScale;
    using DlssNr::AxisScaleFromPercent;
    // 0 is "same as Model resolution"; 1..24 become 25%; 25..200 are exact; above 200 is 200%
    CHECK(AxisScaleFromPercent(0) == 0.0f && AxisScaleFromPercent(-5) == 0.0f);
    CHECK(AxisScaleFromPercent(1) == .25f && AxisScaleFromPercent(24) == .25f && AxisScaleFromPercent(25) == .25f && AxisScaleFromPercent(26) == .26f);
    CHECK(AxisScaleFromPercent(60) == .6f && AxisScaleFromPercent(200) == 2.0f && AxisScaleFromPercent(201) == 2.0f && AxisScaleFromPercent(1000) == 2.0f);
    CHECK(AxisPercentOfScale(0.0f) == 0 && AxisPercentOfScale(-1.0f) == 0 && AxisPercentOfScale(kNaN) == 0 && AxisPercentOfScale(kInf) == 0);
    CHECK(AxisPercentOfScale(.6f) == 60 && AxisPercentOfScale(.25f) == 25 && AxisPercentOfScale(2.0f) == 200);
    CHECK(AxisPercentOfScale(.1f) == 25 && AxisPercentOfScale(5.0f) == 200);
    for (int percent = 25; percent <= 200; ++percent)
        CHECK(AxisPercentOfScale(AxisScaleFromPercent(percent)) == percent);
}

void MotionScaleFollowsEachAxis()
{
    using DlssNr::ModelMotionScales;
    // guides matched to a 960x1080 model of a 1920x1080 reference: half the pixels across, the same down
    const auto a = ModelMotionScales(960, 1080, 1920, 1080);
    CHECK(a.x == .5f && a.y == 1.0f);
    const auto b = ModelMotionScales(1920, 544, 1920, 1080);
    CHECK(b.x == 1.0f && Near(b.y, 544.0 / 1080.0, 1e-6));
    const auto c = ModelMotionScales(1280, 540, 1920, 1080);
    CHECK(Near(c.x, 2.0 / 3.0, 1e-6) && c.y == .5f);
    // a swapped pair of arguments gives a different answer (the axes are not interchangeable)
    const auto d = ModelMotionScales(540, 1280, 1080, 1920);
    CHECK(d.x == .5f && Near(d.y, 2.0 / 3.0, 1e-6) && d.x != c.x);
    // equal axes: both the same, as the single-axis helper has always given
    const auto e = ModelMotionScales(960, 540, 1920, 1080);
    CHECK(e.x == DlssNr::ModelMotionScale(960, 1920) && e.y == DlssNr::ModelMotionScale(540, 1080) && e.x == e.y);
    CHECK(ModelMotionScales(100, 100, 0, 0).x == 1.0f && ModelMotionScales(100, 100, 0, 50).x == 1.0f && ModelMotionScales(100, 100, 0, 50).y == 2.0f);
}

void GuideMatchingRule()
{
    using DlssNr::MatchGuidesToWork;
    // the rule that has always applied: both axes of the model below the frame
    CHECK(MatchGuidesToWork(false, 960, 540, 1920, 1080, false));
    CHECK(!MatchGuidesToWork(false, 960, 1080, 1920, 1080, false) && !MatchGuidesToWork(false, 1920, 540, 1920, 1080, false));
    CHECK(!MatchGuidesToWork(false, 1920, 1080, 1920, 1080, false) && !MatchGuidesToWork(false, 2880, 1620, 1920, 1080, false));
    // a uniform scale just under 100% leaves the 16-grid on the frame size on one axis: that keeps the old answer
    CHECK(DlssNr::OrdinaryWorkSize(1085, .9955f) == 1085 && DlssNr::OrdinaryWorkSize(1920, .9955f) < 1920);
    CHECK(!MatchGuidesToWork(false, DlssNr::OrdinaryWorkSize(1920, .9955f), DlssNr::OrdinaryWorkSize(1085, .9955f), 1920, 1085, false));
    // the spatial warp packs its own guides
    CHECK(!MatchGuidesToWork(true, 960, 540, 1920, 1080, false) && !MatchGuidesToWork(true, 960, 1080, 1920, 1080, true));
    // an anamorphic model with one axis at the frame size is reduced and still gets matched guides
    CHECK(MatchGuidesToWork(false, 960, 1080, 1920, 1080, true) && MatchGuidesToWork(false, 1920, 544, 1920, 1080, true));
    CHECK(MatchGuidesToWork(false, 960, 540, 1920, 1080, true));
    // ... unless an axis is enlarged (the mixed case keeps the full-size guides), or nothing is reduced
    CHECK(!MatchGuidesToWork(false, 960, 1620, 1920, 1080, true) && !MatchGuidesToWork(false, 2880, 540, 1920, 1080, true));
    CHECK(!MatchGuidesToWork(false, 1920, 1080, 1920, 1080, true) && !MatchGuidesToWork(false, 2880, 1620, 1920, 1080, true));
}

void GeometryOfDifferentAxisScales()
{
    const Geometry xHalf = MakeGeometry(Settings {}, 1920, 1080, 1920, 1080, .5f, 1.0f, false);
    CHECK(xHalf.workScaleX == .5f && xHalf.workScaleY == 1.0f && xHalf.workScale == 1.0f && xHalf.Anamorphic());
    CHECK(xHalf.ordinaryW == 960 && xHalf.ordinaryH == 1080 && xHalf.workW == 960 && xHalf.workH == 1080);
    CHECK(xHalf.layout.ordinaryW == 960 && xHalf.layout.ordinaryH == 1080 && !xHalf.spatial);
    const Geometry yHalf = MakeGeometry(Settings {}, 1920, 1080, 1920, 1080, 1.0f, .5f, false);
    CHECK(yHalf.workScaleX == 1.0f && yHalf.workScaleY == .5f && yHalf.ordinaryW == 1920 && yHalf.ordinaryH == 544);
    CHECK(yHalf.workW == 1920 && yHalf.workH == 544 && yHalf.Anamorphic());
    // the identity the settle compares follows each axis on its own
    const auto idX = IdentityOf(xHalf), idY = IdentityOf(yHalf);
    CHECK(idX.workW == 960 && idX.workH == 1080 && idX.ordW == 960 && idX.ordH == 1080);
    CHECK(idY.workW == 1920 && idY.workH == 544 && idY.ordW == 1920 && idY.ordH == 544);
    CHECK(!(idX == idY) && DlssNr::Settle::WorkSizeDiffers(idX, idY));
    // a change of only the vertical scale is a working-size change
    const Geometry yThird = MakeGeometry(Settings {}, 1920, 1080, 1920, 1080, .5f, .75f, false);
    CHECK(yThird.workW == xHalf.workW && yThird.workH != xHalf.workH && DlssNr::Settle::WorkSizeDiffers(IdentityOf(xHalf), IdentityOf(yThird)));
    // enlarged on one axis: supersampling, and the spatial test supports up to 100% on every axis
    const Geometry mixed = MakeGeometry(Foveated(), 3840, 2160, 3840, 2160, .5f, 1.5f, false);
    CHECK(mixed.workScale == 1.5f && !mixed.spatial && StatusText(mixed, false, false) == "inactive: spatial test supports model resolution up to 100%");
    const Geometry mixedOther = MakeGeometry(Foveated(), 3840, 2160, 3840, 2160, 1.5f, .5f, false);
    CHECK(!mixedOther.spatial && StatusText(mixedOther, false, false) == StatusText(mixed, false, false));
    // exactly 100% on both axes still allows spatial; one axis above does not
    CHECK(MakeGeometry(Foveated(), 3840, 2160, 3840, 2160, 1.0f, 1.0f, false).spatial);
    CHECK(!MakeGeometry(Foveated(), 3840, 2160, 3840, 2160, 1.0f, 1.01f, false).spatial && !MakeGeometry(Foveated(), 3840, 2160, 3840, 2160, 1.01f, 1.0f, false).spatial);
    CHECK(MakeGeometry(Foveated(), 3840, 2160, 3840, 2160, .5f, 1.0f, false).spatial);
    // scales are kept in range per axis
    const Geometry range = MakeGeometry(Settings {}, 1920, 1080, 1920, 1080, .1f, 5.0f, false);
    CHECK(range.workScaleX == .25f && range.workScaleY == 2.0f && range.workScale == 2.0f);
}

void EqualAxesAreTheOldBehaviour()
{
    for (const Settings& s : { Settings {}, Foveated(), Peripheral() })
        for (float scale : { 0.25f, 0.5f, 0.8f, 0.91f, 1.0f, 1.5f, 2.0f })
        {
            for (bool failed : { false, true })
            {
                CHECK(SameGeometry(MakeGeometry(s, 3840, 2160, 3840, 2160, scale, failed),
                                   MakeGeometry(s, 3840, 2160, 3840, 2160, scale, scale, failed)));
                CHECK(SameGeometry(MakeGeometry(s, 1920, 1080, 3840, 2160, scale, failed),
                                   MakeGeometry(s, 1920, 1080, 3840, 2160, scale, scale, failed)));
            }
            const Layout one = Build(s, 3840, 2160, scale, 3840, 2160), two = BuildAxes(s, 3840, 2160, scale, scale, 3840, 2160);
            CHECK(one == two && one.Key() == two.Key() && one.active == two.active && one.modelW == two.modelW && one.modelH == two.modelH);
            CHECK(one.globalScale == scale && one.globalScaleY == scale && one.pixelShare == two.pixelShare);
            CHECK(!MakeGeometry(s, 3840, 2160, 3840, 2160, scale, false).Anamorphic());
        }
    // the equal-axes foveated numbers are the ones that were pinned before (3328x1952; at 0.8: 2656x1568)
    CHECK(BuildAxes(Foveated(), 3840, 2160, 1.0f, 1.0f, 3840, 2160).modelW == 3328 && BuildAxes(Foveated(), 3840, 2160, 1.0f, 1.0f, 3840, 2160).modelH == 1952);
    CHECK(BuildAxes(Foveated(), 3840, 2160, .8f, .8f, 3840, 2160).modelW == 2656 && BuildAxes(Foveated(), 3840, 2160, .8f, .8f, 3840, 2160).modelH == 1568);
}

void EachAxisUsesItsOwnScale()
{
    // Foveated: the working extent of an axis takes the scale of that axis (3328x1952 at 100%, 2656x1568 at 80%)
    const Layout fx = BuildAxes(Foveated(), 3840, 2160, .8f, 1.0f, 3840, 2160);
    CHECK(fx.active && fx.modelW == 2656 && fx.modelH == 1952 && fx.ordinaryW == 3072 && fx.ordinaryH == 2160);
    const Layout fy = BuildAxes(Foveated(), 3840, 2160, 1.0f, .8f, 3840, 2160);
    CHECK(fy.active && fy.modelW == 3328 && fy.modelH == 1568 && fy.ordinaryW == 3840 && fy.ordinaryH == 1728);
    CHECK(fx != fy && fx.Key() != fy.Key());
    CHECK(fx.globalScale == .8f && fx.globalScaleY == 1.0f && fy.globalScale == 1.0f && fy.globalScaleY == .8f);
    CHECK(Near(fx.pixelShare, 2656.0 * 1952.0 / (3072.0 * 2160.0), 1e-6) && Near(fy.pixelShare, 3328.0 * 1568.0 / (3840.0 * 1728.0), 1e-6));
    // Peripheral: the model extent of an axis follows that axis' scale only
    const Layout ref = Build(Peripheral(), 3840, 2160, 1.0f), half = Build(Peripheral(), 3840, 2160, .5f);
    const Layout px = BuildAxes(Peripheral(), 3840, 2160, .5f, 1.0f), py = BuildAxes(Peripheral(), 3840, 2160, 1.0f, .5f);
    CHECK(ref.active && half.active && px.active && py.active);
    CHECK(half.modelW < ref.modelW && half.modelH < ref.modelH);
    CHECK(px.modelW == half.modelW && px.modelH == ref.modelH);
    CHECK(py.modelW == ref.modelW && py.modelH == half.modelH);
    CHECK(Near(px.pixelShare, (double) px.modelW * px.modelH / ((double) DlssNr::OrdinaryWorkSize(3840, .5f) * DlssNr::OrdinaryWorkSize(2160, 1.0f)), 1e-6));
    CHECK(Near(py.pixelShare, (double) py.modelW * py.modelH / ((double) DlssNr::OrdinaryWorkSize(3840, 1.0f) * DlssNr::OrdinaryWorkSize(2160, .5f)), 1e-6));
    // the ordinary grid of a disabled layout is per axis too
    const Layout off = BuildAxes(Settings {}, 1920, 1080, .5f, 1.0f);
    CHECK(!off.requested && off.ordinaryW == 960 && off.ordinaryH == 1080 && off.modelW == 960 && off.modelH == 1080);
    const Layout offY = BuildAxes(Settings {}, 1920, 1080, 1.0f, .5f);
    CHECK(offY.ordinaryW == 1920 && offY.ordinaryH == 540 && offY.modelW == 1920 && offY.modelH == 540);
    // either scale out of range makes the layout inactive
    for (const auto& pair : { std::pair<float, float> { 1.0f, .2f }, { .2f, 1.0f }, { 1.0f, 2.5f }, { 2.5f, 1.0f }, { 1.0f, kNaN }, { kNaN, 1.0f } })
    {
        const Layout l = BuildAxes(Foveated(), 3840, 2160, pair.first, pair.second, 3840, 2160);
        CHECK(!l.active && std::strcmp(l.reason.c_str(), "invalid frame or global scale") == 0);
    }
    // the foveated maths' own inputs: a vertical scale left at 0 follows the horizontal one
    DlssNr::Foveated::Inputs in;
    in.frameW = in.outputW = 3840;
    in.frameH = in.outputH = 2160;
    in.globalScale = .8f;
    const auto follow = DlssNr::Foveated::BuildLayout(in);
    in.globalScaleY = .8f;
    const auto explicitY = DlssNr::Foveated::BuildLayout(in);
    CHECK(follow.workW == explicitY.workW && follow.workH == explicitY.workH && follow.ordinaryH == explicitY.ordinaryH && follow.workH == 1568);
    in.globalScaleY = 1.0f;
    const auto apart = DlssNr::Foveated::BuildLayout(in);
    CHECK(apart.workW == 2656 && apart.workH == 1952 && apart.ordinaryW == 3072 && apart.ordinaryH == 2160);
    in.globalScaleY = .2f;
    CHECK(!DlssNr::Foveated::BuildLayout(in).active);
}

void MixedAxesAreRaisedToFullSize()
{
    using DlssNr::AxisScales;
    using DlssNr::RaiseMixedAxes;
    // either sign of the mix: the lower axis is raised to 100%, the higher one is never touched
    CHECK((RaiseMixedAxes({ 1.5f, .5f }) == AxisScales { 1.5f, 1.0f }));
    CHECK((RaiseMixedAxes({ .5f, 1.5f }) == AxisScales { 1.0f, 1.5f }));
    CHECK((RaiseMixedAxes({ 2.0f, .25f }) == AxisScales { 2.0f, 1.0f }) && (RaiseMixedAxes({ .25f, 2.0f }) == AxisScales { 1.0f, 2.0f }));
    // the lower axis just under 100% is raised too; just above the boundary on the higher axis counts as above
    CHECK((RaiseMixedAxes({ 2.0f, .9999f }) == AxisScales { 2.0f, 1.0f }) && (RaiseMixedAxes({ .9999f, 2.0f }) == AxisScales { 1.0f, 2.0f }));
    CHECK((RaiseMixedAxes({ 1.0001f, .5f }) == AxisScales { 1.0001f, 1.0f }) && (RaiseMixedAxes({ .5f, 1.0001f }) == AxisScales { 1.0f, 1.0001f }));
    // exactly 100% is not above: native on one axis and reduced on the other is the ordinary anamorphic case and stays
    CHECK((RaiseMixedAxes({ 1.0f, .5f }) == AxisScales { 1.0f, .5f }) && (RaiseMixedAxes({ .5f, 1.0f }) == AxisScales { .5f, 1.0f }));
    // both above, both below, equal: unchanged
    CHECK((RaiseMixedAxes({ 1.5f, 1.25f }) == AxisScales { 1.5f, 1.25f }) && (RaiseMixedAxes({ .5f, .75f }) == AxisScales { .5f, .75f }));
    CHECK((RaiseMixedAxes({ .8f, .8f }) == AxisScales { .8f, .8f }) && (RaiseMixedAxes({ 1.5f, 1.5f }) == AxisScales { 1.5f, 1.5f }));
    CHECK((RaiseMixedAxes({ 1.0f, 1.0f }) == AxisScales { 1.0f, 1.0f }));
    // the numbers come out of the keys that way: a key set with Y following WorkingScale 50% and X at 150% runs 150% x 100%
    const DlssNr::AnamorphicLatch none {};
    CHECK((DlssNr::EffectiveScales(none, 1.5f, 0.0f, .5f) == AxisScales { 1.5f, 1.0f }));
    CHECK((DlssNr::EffectiveScales(none, 0.0f, .5f, 1.5f) == AxisScales { 1.5f, 1.0f }));
    CHECK((DlssNr::EffectiveScales(none, 0.0f, 1.5f, .5f) == AxisScales { 1.0f, 1.5f }));
    // the geometry built from them sees the raised axis (one number for the model, the guides, the motion scale and the menu)
    const AxisScales mixed = DlssNr::EffectiveScales(none, 1.5f, 0.0f, .5f);
    const Geometry g = MakeGeometry(Settings {}, 1920, 1080, 1920, 1080, mixed.x, mixed.y, false);
    CHECK(g.workScaleX == 1.5f && g.workScaleY == 1.0f && g.workW == 2880 && g.workH == 1080 && g.workScale == 1.5f && g.Anamorphic());
    CHECK(!DlssNr::MatchGuidesToWork(false, g.workW, g.workH, 1920, 1080, g.Anamorphic())); // nothing is reduced, so the guides stay full size
}

void TheAnamorphicLatch()
{
    using DlssNr::AnamorphicLatch;
    using DlssNr::AnamorphicLatchHolds;
    using DlssNr::AxisScales;
    using DlssNr::EffectiveScales;
    using DlssNr::LatchAnamorphic;
    using DlssNr::RefreshAnamorphicLatch;
    // a fresh latch holds for nothing, whatever the keys
    const AnamorphicLatch none {};
    CHECK(!none.failed && !AnamorphicLatchHolds(none, 0.0f, 0.0f, 0.0f) && !AnamorphicLatchHolds(none, .5f, 1.0f, .8f));
    CHECK((EffectiveScales(none, .5f, 1.0f, .8f) == AxisScales { .5f, 1.0f }));
    // the latch remembers the three key values it failed with
    const AnamorphicLatch latch = LatchAnamorphic(.5f, 1.0f, .8f);
    CHECK(latch.failed && latch.keyX == .5f && latch.keyY == 1.0f && latch.keyUniform == .8f);
    CHECK(AnamorphicLatchHolds(latch, .5f, 1.0f, .8f));
    // it holds only for exactly those keys: any one of them moving releases it
    CHECK(!AnamorphicLatchHolds(latch, .6f, 1.0f, .8f) && !AnamorphicLatchHolds(latch, .5f, .9f, .8f) && !AnamorphicLatchHolds(latch, .5f, 1.0f, .7f));
    CHECK(!AnamorphicLatchHolds(latch, 0.0f, 1.0f, .8f) && !AnamorphicLatchHolds(latch, .5f, 0.0f, .8f) && !AnamorphicLatchHolds(latch, .5f, 1.0f, 0.0f));
    // while it holds the model works at the uniform WorkingScale on both axes (the keys' own sizes are not asked for)
    CHECK((EffectiveScales(latch, .5f, 1.0f, .8f) == AxisScales { .8f, .8f }));
    CHECK((EffectiveScales(LatchAnamorphic(0.0f, .5f, 1.0f), 0.0f, .5f, 1.0f) == AxisScales { 1.0f, 1.0f }));
    // ... sanitised like any scale, and a latched mixed key set lands on the uniform scale, not on the mixed rule's numbers
    CHECK((EffectiveScales(LatchAnamorphic(0.0f, .5f, 3.0f), 0.0f, .5f, 3.0f) == AxisScales { 2.0f, 2.0f }));
    CHECK((EffectiveScales(LatchAnamorphic(1.5f, .5f, .6f), 1.5f, .5f, .6f) == AxisScales { .6f, .6f }));
    // once a key moved, the keys are used again (the latch does not hold, and Refresh clears it)
    CHECK((EffectiveScales(latch, .6f, 1.0f, .8f) == AxisScales { .6f, 1.0f }));
    CHECK((EffectiveScales(latch, .5f, 1.0f, .7f) == AxisScales { .5f, 1.0f }));
    const AnamorphicLatch same = RefreshAnamorphicLatch(latch, .5f, 1.0f, .8f);
    CHECK(same.failed && same.keyX == .5f && same.keyY == 1.0f && same.keyUniform == .8f);
    for (const auto& moved : { std::array<float, 3> { .6f, 1.0f, .8f }, std::array<float, 3> { .5f, .9f, .8f }, std::array<float, 3> { .5f, 1.0f, .9f } })
    {
        const AnamorphicLatch cleared = RefreshAnamorphicLatch(latch, moved[0], moved[1], moved[2]);
        CHECK(!cleared.failed && cleared.keyX == 0.0f && cleared.keyY == 0.0f && cleared.keyUniform == 0.0f);
        // and once cleared it stays cleared when the keys go back (a new attempt is made, nothing remembers the refusal)
        CHECK(!AnamorphicLatchHolds(RefreshAnamorphicLatch(cleared, .5f, 1.0f, .8f), .5f, 1.0f, .8f));
    }
    CHECK(!RefreshAnamorphicLatch(none, .5f, 1.0f, .8f).failed);
    // a NaN key must not release the latch on every frame (the refused size would be retried forever); two NaNs are the same value
    const AnamorphicLatch nanLatch = LatchAnamorphic(kNaN, .5f, 1.0f);
    CHECK(AnamorphicLatchHolds(nanLatch, kNaN, .5f, 1.0f) && RefreshAnamorphicLatch(nanLatch, kNaN, .5f, 1.0f).failed);
    CHECK((EffectiveScales(nanLatch, kNaN, .5f, 1.0f) == AxisScales { 1.0f, 1.0f }));
    CHECK(!AnamorphicLatchHolds(nanLatch, .5f, .5f, 1.0f) && !AnamorphicLatchHolds(LatchAnamorphic(.5f, .5f, 1.0f), kNaN, .5f, 1.0f));
    CHECK(DlssNr::SameKeyValue(0.0f, -0.0f) && DlssNr::SameKeyValue(kNaN, kNaN) && !DlssNr::SameKeyValue(kNaN, 0.0f) && !DlssNr::SameKeyValue(1.0f, 1.0001f));
    // the two keys of one axis are not interchangeable
    CHECK(!AnamorphicLatchHolds(LatchAnamorphic(.5f, 1.0f, .8f), 1.0f, .5f, .8f));
    // the latched geometry is uniform, so it is not anamorphic and a second refusal at that size ends the model for the session
    const AxisScales latched = EffectiveScales(latch, .5f, 1.0f, .8f);
    CHECK(!MakeGeometry(Settings {}, 1920, 1080, 1920, 1080, latched.x, latched.y, false).Anamorphic());
    CHECK(MakeGeometry(Settings {}, 1920, 1080, 1920, 1080, .5f, 1.0f, false).Anamorphic());
}

void ThePercentShownIsThePercentApplied()
{
    using DlssNr::AxisPercentApplied;
    CHECK(AxisPercentApplied(0) == 0 && AxisPercentApplied(-7) == 0);
    CHECK(AxisPercentApplied(1) == 25 && AxisPercentApplied(12) == 25 && AxisPercentApplied(24) == 25 && AxisPercentApplied(25) == 25);
    CHECK(AxisPercentApplied(26) == 26 && AxisPercentApplied(100) == 100 && AxisPercentApplied(200) == 200);
    CHECK(AxisPercentApplied(201) == 200 && AxisPercentApplied(999) == 200);
    // the number on the slider is the number the stored scale reads back as, and applying it again changes nothing
    for (int percent = -5; percent <= 260; ++percent)
    {
        const int expected = percent <= 0 ? 0 : (percent < 25 ? 25 : (percent > 200 ? 200 : percent));
        CHECK(AxisPercentApplied(percent) == expected && AxisPercentApplied(AxisPercentApplied(percent)) == expected);
        CHECK(DlssNr::AxisPercentOfScale(DlssNr::AxisScaleFromPercent(percent)) == expected);
    }
}

void PinnedAbsoluteNumbers()
{
    // The expected sizes are written out, not computed in the test: a scale times the extent rounded half up onto the 16 grid (never
    // past the frame when reducing); a spatial model is the even extent of work fraction times scale, on the same grid. The Peripheral
    // and disabled numbers were worked out by hand from those rules; the Foveated pre-SR numbers (1328x976, 1664x784) are snapshots of
    // the code as it stands (the equal-axes Foveated numbers they extend were pinned before the per-axis keys existed).
    const Geometry perX = MakeGeometry(Peripheral(), 3840, 2160, 3840, 2160, .5f, 1.0f, false);
    CHECK(perX.spatial && perX.ordinaryW == 1920 && perX.ordinaryH == 2160 && perX.workW == 1728 && perX.workH == 1952);
    CHECK(Near(perX.layout.pixelShare, 1728.0 * 1952.0 / (1920.0 * 2160.0), 1e-6) && perX.layout.modelW == 1728 && perX.layout.modelH == 1952);
    const Geometry perY = MakeGeometry(Peripheral(), 3840, 2160, 3840, 2160, 1.0f, .5f, false);
    CHECK(perY.spatial && perY.ordinaryW == 3840 && perY.ordinaryH == 1088 && perY.workW == 3456 && perY.workH == 976);
    // a pre-SR frame: 1920x1080 inside a 3840x2160 allocation, the axes differ
    const Geometry preP = MakeGeometry(Peripheral(), 1920, 1080, 3840, 2160, .6f, .9f, false);
    CHECK(preP.spatial && preP.ordinaryW == 1152 && preP.ordinaryH == 976 && preP.workW == 1040 && preP.workH == 880);
    CHECK(Near(preP.layout.pixelShare, 1040.0 * 880.0 / (1152.0 * 976.0), 1e-6) && preP.layout.ordinaryW == 1152 && preP.layout.ordinaryH == 976);
    const Geometry preFx = MakeGeometry(Foveated(), 1920, 1080, 3840, 2160, .8f, 1.0f, false);
    CHECK(preFx.spatial && preFx.ordinaryW == 1536 && preFx.ordinaryH == 1080 && preFx.workW == 1328 && preFx.workH == 976);
    const Geometry preFy = MakeGeometry(Foveated(), 1920, 1080, 3840, 2160, 1.0f, .8f, false);
    CHECK(preFy.spatial && preFy.ordinaryW == 1920 && preFy.ordinaryH == 864 && preFy.workW == 1664 && preFy.workH == 784);
    CHECK(Near(preFx.layout.pixelShare, 1328.0 * 976.0 / (1536.0 * 1080.0), 1e-6) && Near(preFy.layout.pixelShare, 1664.0 * 784.0 / (1920.0 * 864.0), 1e-6));
    // a layout that is not asked for works at the ordinary size, per axis (the pre-SR frame and the full one)
    const Geometry offPre = MakeGeometry(Settings {}, 1920, 1080, 3840, 2160, .6f, .9f, false);
    CHECK(!offPre.spatial && offPre.ordinaryW == 1152 && offPre.ordinaryH == 976 && offPre.workW == 1152 && offPre.workH == 976);
    CHECK(offPre.layout.ordinaryW == 1152 && offPre.layout.ordinaryH == 976);
    const Geometry offBig = MakeGeometry(Settings {}, 3840, 2160, 3840, 2160, .5f, 1.0f, false);
    CHECK(!offBig.spatial && offBig.ordinaryW == 1920 && offBig.ordinaryH == 2160 && offBig.workW == 1920 && offBig.workH == 2160);
    // an active layout and different scales: the foveated model takes each axis' own scale (the same numbers EachAxisUsesItsOwnScale pins)
    const Geometry fov = MakeGeometry(Foveated(), 3840, 2160, 3840, 2160, .8f, 1.0f, false);
    CHECK(fov.spatial && fov.layout.globalScale == .8f && fov.layout.globalScaleY == 1.0f && fov.workW == 2656 && fov.workH == 1952);
    const Geometry fovY = MakeGeometry(Foveated(), 3840, 2160, 3840, 2160, 1.0f, .8f, false);
    CHECK(fovY.spatial && fovY.layout.globalScale == 1.0f && fovY.layout.globalScaleY == .8f && fovY.workW == 3328 && fovY.workH == 1568);
}

void ScaleValidationPerAxis()
{
    // a scale that is not a number leaves the ordinary size of EACH axis at the frame's (it is not borrowed from the other axis)
    CHECK(BuildAxes(Settings {}, 1920, 1080, 1.0f, kNaN).ordinaryH == 1080 && BuildAxes(Settings {}, 1920, 1080, 1.0f, kNaN).ordinaryW == 1920);
    CHECK(BuildAxes(Settings {}, 1920, 1080, kNaN, 1.0f).ordinaryW == 1920 && BuildAxes(Settings {}, 1920, 1080, kNaN, 1.0f).ordinaryH == 1080);
    CHECK(BuildAxes(Settings {}, 1920, 1080, .5f, kNaN).ordinaryW == 960 && BuildAxes(Settings {}, 1920, 1080, kNaN, .5f).ordinaryH == 540);
    // out-of-range scales are refused by the Peripheral profile too (not only by the Foveated maths), with the one reason
    for (const auto& pair : { std::pair<float, float> { 2.5f, 1.0f }, { 1.0f, 2.5f }, { .2f, 1.0f }, { 1.0f, .2f }, { kNaN, 1.0f }, { 1.0f, kNaN } })
    {
        const Layout l = BuildAxes(Peripheral(), 3840, 2160, pair.first, pair.second, 3840, 2160);
        CHECK(!l.active && l.requested && std::strcmp(l.reason.c_str(), "invalid frame or global scale") == 0);
    }
    // and at the bounds they are accepted
    CHECK(BuildAxes(Peripheral(), 3840, 2160, .25f, 2.0f).reason != "invalid frame or global scale");
    CHECK(BuildAxes(Peripheral(), 3840, 2160, 2.0f, .25f).reason != "invalid frame or global scale");
    // the Foveated maths' own checks, one axis at a time, with the one reason
    DlssNr::Foveated::Inputs in;
    in.frameW = in.outputW = 3840;
    in.frameH = in.outputH = 2160;
    for (const auto& pair : { std::pair<float, float> { 2.5f, 1.0f }, { .2f, 1.0f }, { 1.0f, 2.5f }, { 1.0f, .2f } })
    {
        in.globalScale = pair.first;
        in.globalScaleY = pair.second;
        const auto l = DlssNr::Foveated::BuildLayout(in);
        CHECK(!l.active && std::strcmp(l.reason, "invalid frame or global scale") == 0);
    }
    in.globalScale = 2.0f;
    in.globalScaleY = 2.0f;
    CHECK(std::strcmp(DlssNr::Foveated::BuildLayout(in).reason, "invalid frame or global scale") != 0);
}

void AxisEqualityNeedsBothAxesAndEffectiveFollowsUniform()
{
    using DlssNr::AxisScales;
    using DlssNr::EffectiveScales;
    const DlssNr::AnamorphicLatch none {};
    // RM15/RM17/RM18: AxisScales == needs both axes (each mutant wrongly reports one of these as equal).
    CHECK(!(EffectiveScales(none, .5f, 1.0f, .8f) == AxisScales { .5f, .9f }));
    CHECK(!(EffectiveScales(none, .5f, 1.0f, .8f) == AxisScales { .9f, 1.0f }));
    // EF16: a Y key of 0 follows the uniform scale (the mutant leaves Y at 1.0).
    CHECK((EffectiveScales(none, .5f, 0.0f, .8f) == AxisScales { .5f, .8f }));
    // EF18: the uniform fallback and the 25%..200% clamp happen before the mixed-axes raise (a +Inf key follows the uniform
    // scale; the mutant raises the raw Inf first, contaminating the other axis to 1.0, then sanitises to (1.0, 1.0)).
    CHECK((EffectiveScales(none, kInf, .5f, .6f) == AxisScales { .6f, .5f }));
}

void LayoutsDifferingOnlyByTheSwappedScales()
{
    // LY20: KeyFields slots 3 and 4 are the X scale then the Y scale (bits of globalScale, then bits of globalScaleY).
    uint64_t halfBits = 0, fullBits = 0;
    {
        double half = .5, full = 1.0;
        std::memcpy(&halfBits, &half, sizeof halfBits);
        std::memcpy(&fullBits, &full, sizeof fullBits);
    }
    for (const Settings& s : { Settings {}, Peripheral(), Foveated() })
    {
        const Layout a = BuildAxes(s, 1920, 1080, .5f, 1.0f, 1920, 1080), b = BuildAxes(s, 1920, 1080, 1.0f, .5f, 1920, 1080);
        CHECK(a != b && a.Key() != b.Key() && a.KeyFields() != b.KeyFields());
        CHECK(a.globalScale == b.globalScaleY && a.globalScaleY == b.globalScale);
        CHECK(a.KeyFields()[3] == halfBits && a.KeyFields()[4] == fullBits);
        CHECK(b.KeyFields()[3] == fullBits && b.KeyFields()[4] == halfBits);
    }
}
} // namespace

int main()
{
    EffectiveAxisScaleSelectsOverrideOrFollows();
    MenuPercentConversion();
    MotionScaleFollowsEachAxis();
    GuideMatchingRule();
    GeometryOfDifferentAxisScales();
    EqualAxesAreTheOldBehaviour();
    EachAxisUsesItsOwnScale();
    MixedAxesAreRaisedToFullSize();
    TheAnamorphicLatch();
    ThePercentShownIsThePercentApplied();
    PinnedAbsoluteNumbers();
    ScaleValidationPerAxis();
    AxisEqualityNeedsBothAxesAndEffectiveFollowsUniform();
    LayoutsDifferingOnlyByTheSwappedScales();
    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: per-axis model resolution (effective scale, motion scale and guide matching per axis, geometry and layout from two scales, equal axes unchanged)");
    return 0;
}
