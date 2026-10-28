// The Foveated profile inside the spatial layout (DlssNr_Spatial.h): the settings and their equality, the output-to-frame
// conversion of the pixel sizes, the layout key the settle decision uses, the shader constants of each profile, and the geometry
// the pass derives from one set of requests (including that a pending change leaves every consumer on the applied one).
#include "ext/nr/spatial/DlssNr_Spatial.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
namespace Settle = DlssNr::Settle;

bool Near(double a, double b, double tolerance) { return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance; }

Settings FoveatedSettings()
{
    Settings s;
    s.enabled = true;
    s.profile = Profile::Foveated;
    return s;
}

bool SameBytes(const Constants& a, const Constants& b) { return std::memcmp(&a, &b, sizeof(Constants)) == 0; }

float FloatFromBits(uint32_t bits)
{
    float v = 0;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

using SettingMember = float Settings::*;

void Defaults()
{
    const Settings s;
    CHECK(!s.enabled && s.profile == Profile::Peripheral);
    CHECK(s.foveaWidth == 2400 && s.foveaHeight == 1350 && s.foveaFalloff == 400 && s.foveaEdgeX == .5f && s.foveaEdgeY == .5f);
    CHECK(s.centerX == 80 && s.workX == 90 && s.offsetX == 0 && s.shiftX == 0);
    // a layout that was never built is a Peripheral one too (the profile every old setting means)
    const Layout blank {};
    CHECK(blank.profile == Profile::Peripheral && blank.settings.profile == Profile::Peripheral);
    CHECK(!blank.requested && !blank.active && blank.pixelShare == 1.0f);
}

// ---- equality: every new field is detected -----------------------------------------------------------------------------------
void SettingsEqualityDetectsEveryField()
{
    const Settings base = FoveatedSettings();
    const std::vector<std::function<void(Settings&)>> mutations = {
        [](Settings& s) { s.enabled = false; },     [](Settings& s) { s.profile = Profile::Peripheral; },
        [](Settings& s) { s.centerX = 70; },        [](Settings& s) { s.centerY = 70; },
        [](Settings& s) { s.workX = 80; },          [](Settings& s) { s.workY = 80; },
        [](Settings& s) { s.offsetX = 1; },         [](Settings& s) { s.offsetY = 1; },
        [](Settings& s) { s.shiftX = 1; },          [](Settings& s) { s.shiftY = 1; },
        [](Settings& s) { s.foveaWidth = 2300; },   [](Settings& s) { s.foveaHeight = 1300; },
        [](Settings& s) { s.foveaFalloff = 300; },  [](Settings& s) { s.foveaEdgeX = .6f; },
        [](Settings& s) { s.foveaEdgeY = .6f; },
    };
    for (const auto& mutate : mutations)
    {
        Settings changed = base;
        mutate(changed);
        CHECK(changed != base && !(changed == base) && base != changed);
    }
    Settings same = base, nan = base;
    CHECK(same == base);
    nan.foveaWidth = NAN;
    same.foveaWidth = NAN;
    CHECK(nan == same && nan != base);
}

void LayoutEqualityDetectsEveryEffectiveField()
{
    const Settings base = FoveatedSettings();
    const Layout reference = Build(base, 3840, 2160, 1.0f, 3840, 2160);
    CHECK(reference.active && reference == Build(base, 3840, 2160, 1.0f, 3840, 2160));
    const std::vector<std::function<void(Settings&)>> mutations = {
        [](Settings& s) { s.profile = Profile::Peripheral; },
        [](Settings& s) { s.foveaWidth = 2300; },
        [](Settings& s) { s.foveaHeight = 1300; },
        [](Settings& s) { s.foveaFalloff = 300; },
        [](Settings& s) { s.foveaEdgeX = .6f; },
        [](Settings& s) { s.foveaEdgeY = .6f; },
        [](Settings& s) { s.offsetX = 3; },
        [](Settings& s) { s.offsetY = -3; },
        [](Settings& s) { s.enabled = false; },
    };
    std::set<uint64_t> keys { reference.Key() };
    for (const auto& mutate : mutations)
    {
        Settings changed = base;
        mutate(changed);
        const Layout l = Build(changed, 3840, 2160, 1.0f, 3840, 2160);
        CHECK(l != reference && !(l == reference));
        CHECK(l.Key() != reference.Key());
        keys.insert(l.Key());
    }
    CHECK(keys.size() == mutations.size() + 1); // all distinct
    // the frame, the global scale and the output the sizes are written against all change the effective values
    CHECK(Build(base, 3840, 2161, 1.0f, 3840, 2161) != reference);
    CHECK(Build(base, 3840, 2160, .9f, 3840, 2160) != reference);
    CHECK(Build(base, 1920, 1080, 1.0f, 3840, 2160) != reference);
    CHECK(Build(base, 1920, 1080, 1.0f, 3840, 2160) != Build(base, 1920, 1080, 1.0f, 1920, 1080));
    // only what is effective counts: a setting beyond the frame behaves as the identity whatever its value
    Settings wide = base, wider = base;
    wide.foveaWidth = 4000;
    wider.foveaWidth = 4500;
    CHECK(Build(wide, 3840, 2160, 1.0f, 3840, 2160) == Build(wider, 3840, 2160, 1.0f, 3840, 2160));
    // the other profile's settings do not matter while a profile is not selected
    Settings peripheral;
    peripheral.enabled = true;
    Settings peripheralOther = peripheral;
    peripheralOther.foveaWidth = 1000;
    peripheralOther.foveaEdgeX = .3f;
    CHECK(Build(peripheral, 3840, 2160, 1.0f) == Build(peripheralOther, 3840, 2160, 1.0f));
    Settings foveatedOther = base;
    foveatedOther.centerX = 55;
    foveatedOther.workX = 70;
    foveatedOther.shiftX = 2;
    CHECK(Build(base, 3840, 2160, 1.0f, 3840, 2160) == Build(foveatedOther, 3840, 2160, 1.0f, 3840, 2160));
    // ... and the peripheral ones still count in the peripheral profile, as before
    Settings peripheralChanged = peripheral;
    peripheralChanged.centerX = 70;
    CHECK(Build(peripheral, 3840, 2160, 1.0f) != Build(peripheralChanged, 3840, 2160, 1.0f));
    peripheralChanged = peripheral;
    peripheralChanged.shiftX = 1;
    CHECK(Build(peripheral, 3840, 2160, 1.0f) != Build(peripheralChanged, 3840, 2160, 1.0f));
    // a disabled layout is the same whatever the rest says; equal layouts have equal keys
    Settings off, offOther;
    offOther.centerX = 20;
    offOther.profile = Profile::Foveated;
    CHECK(Build(off, 3840, 2160, 1.0f) == Build(offOther, 3840, 2160, 1.0f) && Build(off, 3840, 2160, 1.0f).Key() == Build(offOther, 3840, 2160, 1.0f).Key());
    CHECK(Build(off, 3840, 2160, 1.0f) != Build(base, 3840, 2160, 1.0f));
    // NaN settings compare equal to themselves, as before
    Settings nanCentre;
    nanCentre.enabled = true;
    nanCentre.centerX = NAN;
    CHECK(Build(nanCentre, 1920, 1080, 1.0f) == Build(nanCentre, 1920, 1080, 1.0f));
}

// ---- the key covers every field it is meant to, one at a time -----------------------------------------------------------------
// Each edit changes ONE field directly on a built layout (not through the settings, which move several fields at once). A key that
// forgets that field compares equal to the reference and fails here.
using Edit = std::pair<std::string, std::function<void(Layout&)>>;

void EachEditChangesEqualityAndKey(const Layout& ref, const std::vector<Edit>& edits, int line)
{
    std::set<uint64_t> keys { ref.Key() };
    for (const auto& edit : edits)
    {
        Layout copy = ref;
        edit.second(copy);
        const std::string what = "layout field changes equality and key: " + edit.first;
        Check(!(copy == ref) && copy != ref && ref != copy, what.c_str(), line);
        Check(copy.Key() != ref.Key(), what.c_str(), line);
        keys.insert(copy.Key());
    }
    Check(keys.size() == edits.size() + 1, "every edit gives its own key", line);
}

void EveryFieldOfTheLayoutChangesEqualityAndKey()
{
    using AxisMember = DlssNr::Foveated::Axis DlssNr::Foveated::Layout::*;
    const auto common = []
    {
        std::vector<Edit> edits;
        edits.push_back({ "requested", [](Layout& l) { l.requested = !l.requested; } });
        edits.push_back({ "active", [](Layout& l) { l.active = !l.active; } });
        edits.push_back({ "profile", [](Layout& l) { l.profile = l.profile == Profile::Foveated ? Profile::Peripheral : Profile::Foveated; } });
        edits.push_back({ "nativeW", [](Layout& l) { l.nativeW += 1; } });
        edits.push_back({ "nativeH", [](Layout& l) { l.nativeH += 1; } });
        edits.push_back({ "globalScale", [](Layout& l) { l.globalScale = .9f; } });
        return edits;
    };

    // the Foveated list: the six common fields and, per axis, six effective values
    const Layout foveated = Build(FoveatedSettings(), 3840, 2160, 1.0f, 3840, 2160);
    CHECK(foveated.active && foveated.requested && foveated.profile == Profile::Foveated);
    CHECK(foveated.fovea.x.warped && foveated.fovea.y.warped);
    std::vector<Edit> edits = common();
    for (const auto& axis : std::vector<std::pair<std::string, AxisMember>> { { "x", &DlssNr::Foveated::Layout::x },
                                                                               { "y", &DlssNr::Foveated::Layout::y } })
    {
        const AxisMember m = axis.second;
        edits.push_back({ axis.first + ".warped", [m](Layout& l) { (l.fovea.*m).warped = !(l.fovea.*m).warped; } });
        edits.push_back({ axis.first + ".centre", [m](Layout& l) { (l.fovea.*m).centre += 1; } });
        edits.push_back({ axis.first + ".falloff", [m](Layout& l) { (l.fovea.*m).falloff += 1; } });
        edits.push_back({ axis.first + ".edge", [m](Layout& l) { (l.fovea.*m).edge += .01f; } });
        edits.push_back({ axis.first + ".offsetPercent", [m](Layout& l) { (l.fovea.*m).offsetPercent += 1; } });
        edits.push_back({ axis.first + ".workExtent", [m](Layout& l) { (l.fovea.*m).workExtent += 2; } });
    }
    CHECK(edits.size() == Layout::kKeyFields); // a field added to the key needs an edit here
    EachEditChangesEqualityAndKey(foveated, edits, __LINE__);

    // the Peripheral list: the six common fields and the eight settings
    Settings ps;
    ps.enabled = true;
    const Layout peripheral = Build(ps, 3840, 2160, 1.0f);
    CHECK(peripheral.active && peripheral.profile == Profile::Peripheral);
    edits = common();
    const std::vector<std::pair<std::string, SettingMember>> members = {
        { "centerX", &Settings::centerX }, { "centerY", &Settings::centerY }, { "workX", &Settings::workX },
        { "workY", &Settings::workY },     { "offsetX", &Settings::offsetX }, { "offsetY", &Settings::offsetY },
        { "shiftX", &Settings::shiftX },   { "shiftY", &Settings::shiftY } };
    for (const auto& member : members)
    {
        const SettingMember m = member.second;
        edits.push_back({ "settings." + member.first, [m](Layout& l) { l.settings.*m += 1; } });
    }
    CHECK(edits.size() == 14);
    EachEditChangesEqualityAndKey(peripheral, edits, __LINE__);

    // the fields of the other profile are not in a profile's list (the rest of the list is the same)
    for (const auto& member : members)
    {
        Layout copy = foveated;
        copy.settings.*member.second += 1;
        CHECK(copy == foveated && copy.Key() == foveated.Key());
    }
    Layout other = peripheral;
    other.fovea.x.centre += 1;
    other.fovea.y.workExtent += 2;
    CHECK(other == peripheral && other.Key() == peripheral.Key());
}

// A layout whose list is all zeros apart from one slot: the slot that is the only difference cannot be hidden by another one.
Layout ZeroTailLayout(Profile profile, bool requested)
{
    Layout l;
    l.settings = Settings {};
    for (SettingMember m : { &Settings::centerX, &Settings::centerY, &Settings::workX, &Settings::workY, &Settings::offsetX,
                              &Settings::offsetY, &Settings::shiftX, &Settings::shiftY })
        l.settings.*m = 0;
    l.fovea.x.edge = l.fovea.y.edge = 0; // the Axis default is 1
    l.profile = profile;
    l.requested = requested;
    l.active = false;
    l.nativeW = 3840;
    l.nativeH = 2160;
    l.globalScale = 1.0f;
    return l;
}

size_t DifferingSlots(const Layout& a, const Layout& b)
{
    const auto ka = a.KeyFields(), kb = b.KeyFields();
    size_t n = 0;
    for (size_t i = 0; i < ka.size(); ++i)
        n += ka[i] != kb[i] ? 1 : 0;
    return n;
}

void ProfileAndRequestedCountOnTheirOwn()
{
    // The two profiles read different fields after the profile slot, so a changed profile is usually also seen elsewhere. With
    // every other slot zero it is the profile slot alone.
    const Layout peripheral = ZeroTailLayout(Profile::Peripheral, true), foveated = ZeroTailLayout(Profile::Foveated, true);
    CHECK(DifferingSlots(peripheral, foveated) == 1);
    CHECK(peripheral != foveated && !(peripheral == foveated) && peripheral.Key() != foveated.Key());
    // likewise a layout that is requested and one that is not differ in the first slot alone (a disabled layout stops there, and
    // everything after it is zero in the other one)
    const Layout disabled = ZeroTailLayout(Profile::Peripheral, false);
    CHECK(DifferingSlots(peripheral, disabled) == 1);
    CHECK(peripheral != disabled && !(peripheral == disabled) && peripheral.Key() != disabled.Key());
    // and the rest of a disabled layout does not matter
    Layout disabledOther = disabled;
    disabledOther.profile = Profile::Foveated;
    disabledOther.active = true;
    disabledOther.settings.centerX = 33;
    disabledOther.fovea.x.centre = 5;
    CHECK(disabled == disabledOther && disabled.Key() == disabledOther.Key());
}

// -0 is 0 and every NaN is the same value, whatever bits it carries; neither is any other number.
void FloatsInTheKeyHaveOneCanonicalForm()
{
    Settings ps;
    ps.enabled = true;
    const Layout peripheral = Build(ps, 3840, 2160, 1.0f);
    const Layout foveated = Build(FoveatedSettings(), 3840, 2160, 1.0f, 3840, 2160);
    CHECK(peripheral.settings.offsetX == 0 && foveated.fovea.x.offsetPercent == 0);

    const float negZero = -0.0f;
    CHECK(std::signbit(negZero) && negZero == 0.0f);
    Layout a = peripheral, b = peripheral;
    a.settings.offsetX = 0.0f;
    b.settings.offsetX = negZero;
    CHECK(std::signbit(b.settings.offsetX) && !std::signbit(a.settings.offsetX));
    CHECK(a == b && a.Key() == b.Key());
    a = foveated;
    b = foveated;
    a.fovea.y.offsetPercent = 0.0f;
    b.fovea.y.offsetPercent = negZero;
    CHECK(a == b && a.Key() == b.Key());
    a = peripheral;
    b = peripheral;
    a.settings.shiftY = 0.0f;
    b.settings.shiftY = negZero;
    CHECK(a == b && a.Key() == b.Key());
    // ... and zero is not the smallest number either
    b.settings.shiftY = 1e-30f;
    CHECK(a != b && a.Key() != b.Key());

    // NaNs with different sign and payload bits are one value
    const float nans[] = { FloatFromBits(0x7fc00000u), FloatFromBits(0xffc00000u), FloatFromBits(0x7fc00001u), FloatFromBits(0x7fffffffu) };
    for (float n : nans)
        CHECK(std::isnan(n));
    for (float one : nans)
        for (float other : nans)
        {
            Layout p1 = peripheral, p2 = peripheral;
            p1.settings.workX = one;
            p2.settings.workX = other;
            CHECK(p1 == p2 && p1.Key() == p2.Key());
            CHECK(p1 != peripheral && p1.Key() != peripheral.Key());
            Layout f1 = foveated, f2 = foveated;
            f1.fovea.x.centre = one;
            f2.fovea.x.centre = other;
            CHECK(f1 == f2 && f1.Key() == f2.Key());
            CHECK(f1 != foveated && f1.Key() != foveated.Key());
            Layout g1 = peripheral, g2 = peripheral;
            g1.globalScale = one;
            g2.globalScale = other;
            CHECK(g1 == g2 && g1.Key() == g2.Key());
            CHECK(g1 != peripheral && g1.Key() != peripheral.Key());
        }
}

// ---- the layout ----------------------------------------------------------------------------------------------------------------
void FoveatedLayoutSizes()
{
    Settings s = FoveatedSettings();
    const Settings before = s;
    Layout l = Build(s, 3840, 2160, 1.0f, 3840, 2160);
    CHECK(l.requested && l.active && l.profile == Profile::Foveated && l.reason == "active");
    CHECK(l.modelW == 3328 && l.modelH == 1952 && l.nativeW == 3840 && l.ordinaryW == 3840);
    CHECK(Near(l.pixelShare, 3328.0 * 1952.0 / (3840.0 * 2160.0), 1e-6) && l.pixelShare > .78 && l.pixelShare < .79);
    // the pre-SR case: the model processes a 1920x1080 active rectangle inside a 3840x2160 output; the same defaults engage and
    // give the same share
    const Layout inside = Build(s, 1920, 1080, 1.0f, 3840, 2160);
    CHECK(inside.active && inside.modelW == 1664 && inside.modelH == 976);
    CHECK(Near(inside.pixelShare, l.pixelShare, 1e-4));
    // without the conversion the same sizes would be clamped to the identity on that frame and nothing would engage
    const Layout noConversion = Build(s, 1920, 1080, 1.0f);
    CHECK(!noConversion.active && noConversion.reason == "saving under 5%" && noConversion.pixelShare == 1.0f);
    // the axes use their own ratios
    const Layout tall = Build(s, 1920, 2160, 1.0f, 3840, 2160);
    CHECK(Near(tall.fovea.x.centre, 1200, 1e-3) && Near(tall.fovea.y.centre, 1350, 1e-3));
    CHECK(Near(tall.fovea.x.falloff, 200, 1e-3) && Near(tall.fovea.y.falloff, 400, 1e-3));
    // nothing is written back to the settings, whatever the frame does to the effective values
    s.foveaWidth = 5000;
    s.foveaHeight = 40;
    s.foveaEdgeX = 3;
    s.foveaEdgeY = .01f;
    s.offsetX = 40;
    const Settings stored = s;
    l = Build(s, 3840, 2160, 1.0f, 3840, 2160);
    CHECK(s == stored && std::memcmp(&s, &stored, sizeof(Settings)) == 0);
    CHECK(l.settings == stored && !l.fovea.x.warped && l.fovea.y.warped && l.fovea.y.edge == .25f);
    (void) before;
    // an inactive Foveated layout runs the ordinary size and reports a full share and its reason
    Settings off = FoveatedSettings();
    off.foveaWidth = 4000;
    off.foveaHeight = 3000;
    l = Build(off, 3840, 2160, 1.0f, 3840, 2160);
    CHECK(l.requested && !l.active && l.reason == "saving under 5%" && l.modelW == 3840 && l.modelH == 2160 && l.pixelShare == 1.0f);
    CHECK(l.fovea.share > .95f);
    // the global working scale still shrinks the ordinary size and the foveated one together
    l = Build(s = FoveatedSettings(), 3840, 2160, .8f, 3840, 2160);
    CHECK(l.active && l.modelW == 2656 && l.modelH == 1568 && l.ordinaryW == 3072);
    // a model extent that would be below 25% never goes ahead
    s = FoveatedSettings();
    s.foveaWidth = 600;
    s.foveaHeight = 300;
    s.foveaFalloff = 0;
    s.foveaEdgeX = s.foveaEdgeY = .25f;
    l = Build(s, 3840, 2160, .25f, 3840, 2160);
    CHECK(!l.active && l.reason == "effective model extent below 25%");
    // invalid frame or scale
    CHECK(!Build(FoveatedSettings(), 1, 2160, 1.0f).active && !Build(FoveatedSettings(), 3840, 2160, 3.0f).active);
    // the peripheral profile is unchanged and now reports its share
    Settings p;
    p.enabled = true;
    l = Build(p, 3840, 2160, 1.0f);
    CHECK(l.active && l.profile == Profile::Peripheral && l.modelW == 3456 && l.modelH == 1952);
    CHECK(Near(l.pixelShare, 3456.0 * 1952.0 / (3840.0 * 2160.0), 1e-6));
    CHECK(Build(Settings {}, 3840, 2160, 1.0f).pixelShare == 1.0f);
}

void ShaderConstantsOfEachProfile()
{
    static_assert(sizeof(Constants) == 256 && sizeof(pw::ShaderConstants) == 176 && offsetof(Constants, warp) == 16);
    const DlssNr::GuideRegions regions { { 0, 0, 3840, 2160 }, { 0, 0, 3840, 2160 } };
    const Layout f = Build(FoveatedSettings(), 3840, 2160, 1.0f, 3840, 2160);
    Settings ps;
    ps.enabled = true;
    const Layout p = Build(ps, 3840, 2160, 1.0f);
    const Constants cf = MakeConstants(f, 100, regions, 1, 1, 3840, 2160), cp = MakeConstants(p, 100, regions, 1, 1, 3840, 2160);
    CHECK(cf.mode == 100 && cf.width == 3328 && cf.height == 1952);
    CHECK((cf.warp.flags & DlssNr::Foveated::kFlagFoveated) != 0 && (cp.warp.flags & DlssNr::Foveated::kFlagFoveated) == 0);
    CHECK((cf.warp.flags & 1u) != 0 && (cp.warp.flags & 1u) != 0);
    CHECK(cp.warp.workWidth == 3456 && cp.width == 3456);
    CHECK(SameBytes(MakeConstants(f, 102, regions, 1, 1, 3840, 2160), MakeConstants(f, 102, regions, 1, 1, 3840, 2160)));
    const Constants unpack = MakeConstants(f, 102, regions, 1, 1, 3840, 2160);
    CHECK(unpack.width == f.ordinaryW && unpack.height == f.ordinaryH);
    // the fused resolve: the selector word carries the profile in a high bit
    const ResolveWarp rf = MakeResolveWarp(f, true), rp = MakeResolveWarp(p, true);
    CHECK(rf.residual == (2u | DlssNr::Foveated::kResolveFoveatedBit) && rp.residual == 2u);
    CHECK(MakeResolveWarp(f, false).residual == 1u && MakeResolveWarp(p, false).residual == 1u);
    CHECK(DlssNr::Foveated::kResolveFoveatedBit == 0x100u && DlssNr::Foveated::kFlagFoveated == 2u);
    CHECK(rf.workSize[0] == 3328 && rf.workSize[1] == 1952 && rp.workSize[0] == 3456);
    // the 28 floats are the seven float4 fields in the order the shader's constant buffer declares them
    const pw::ShaderConstants k = MakeShaderConstants(f);
    CHECK(rf.fields[0] == k.bandCenterX && rf.fields[1] == k.bandCenterY && rf.fields[2] == k.workCenterX && rf.fields[3] == k.workCenterY);
    CHECK(rf.fields[4] == k.halfSpanNegX && rf.fields[6] == k.sideCenterNegX);
    CHECK(rf.fields[8] == k.halfSpanPosX && rf.fields[10] == k.sideCenterPosX);
    CHECK(rf.fields[12] == k.sideWorkNegX && rf.fields[14] == k.sideCompressionNegX);
    CHECK(rf.fields[16] == k.sideWorkPosX && rf.fields[18] == k.sideCompressionPosX);
    CHECK(rf.fields[20] == k.sideEdgeSlopeNegX && rf.fields[22] == k.sideEdgeSlopePosX);
    CHECK(rf.fields[24] == k.workScaleX && rf.fields[25] == k.workScaleY);
    const pw::ShaderConstants kp = pw::MakeShaderConstants(p.warp);
    CHECK(rp.fields[0] == kp.bandCenterX && rp.fields[4] == kp.halfSpanNegX && rp.fields[20] == kp.sideEdgeSlopeNegX);
    // the pass cannot tell the profiles apart except by those constants
    const pw::ShaderConstants viaSpatial = MakeShaderConstants(p);
    CHECK(std::memcmp(&viaSpatial, &kp, sizeof kp) == 0);
}

// ---- the geometry the pass works from ---------------------------------------------------------------------------------------------
void Geometries()
{
    const Settings f = FoveatedSettings();
    Geometry g = MakeGeometry(f, 3840, 2160, 3840, 2160, 1.0f, false);
    CHECK(g.spatial && g.workW == 3328 && g.workH == 1952 && g.ordinaryW == 3840 && g.ordinaryH == 2160 && g.frameW == 3840);
    CHECK(g.layout.ordinaryW == 3840 && g.layout.ordinaryH == 2160 && g.workScale == 1.0f);
    // a spatial failure, a working scale above 100% and a disabled switch all mean the ordinary size
    g = MakeGeometry(f, 3840, 2160, 3840, 2160, 1.0f, true);
    CHECK(!g.spatial && g.workW == 3840 && g.workH == 2160);
    g = MakeGeometry(f, 3840, 2160, 3840, 2160, 1.5f, false);
    CHECK(!g.spatial && g.workW == 5760 && g.workH == 3248 && g.workScale == 1.5f);
    g = MakeGeometry(Settings {}, 3840, 2160, 3840, 2160, 1.0f, false);
    CHECK(!g.spatial && g.workW == 3840);
    // the scale is sanitised exactly as before
    CHECK(SanitizeWorkScale(NAN) == 1.0f && SanitizeWorkScale(.1f) == .25f && SanitizeWorkScale(5) == 2.0f && SanitizeWorkScale(.8f) == .8f);
    g = MakeGeometry(f, 3840, 2160, 3840, 2160, .8f, false);
    CHECK(g.spatial && g.ordinaryW == 3072 && g.ordinaryH == 1728 && g.workW == 2656 && g.workH == 1568);
    // the fused native resolve needs the ordinary size to be the frame, no side-by-side compare and a float16 target
    g = MakeGeometry(f, 3840, 2160, 3840, 2160, 1.0f, false);
    CHECK(FusedNative(g, false, true) && !FusedNative(g, true, true) && !FusedNative(g, false, false));
    g = MakeGeometry(f, 3840, 2160, 3840, 2160, .8f, false);
    CHECK(!FusedNative(g, false, true));
    CHECK(!FusedNative(MakeGeometry(Settings {}, 3840, 2160, 3840, 2160, 1.0f, false), false, true));
    // status text
    g = MakeGeometry(f, 3840, 2160, 3840, 2160, 1.0f, false);
    CHECK(StatusText(g, false, true) == "active (fused native resolve)" && StatusText(g, false, false) == "active");
    CHECK(StatusText(g, true, false) == "fallback: spatial processing failed");
    CHECK(StatusText(MakeGeometry(Settings {}, 3840, 2160, 3840, 2160, 1.0f, false), false, false) == "disabled");
    CHECK(StatusText(MakeGeometry(f, 3840, 2160, 3840, 2160, 1.5f, false), false, false) == "inactive: spatial test supports model resolution up to 100%");
    // identity
    const Settle::Identity id = IdentityOf(g);
    CHECK(id.frameW == 3840 && id.frameH == 2160 && id.workW == 3328 && id.workH == 1952 && id.layoutKey == g.layout.Key());
    CHECK(id.ordW == g.ordinaryW && id.ordH == g.ordinaryH);
    // A scale step that changes only the ordinary grid (the unpack scratch size) is a work-size change, not a warp-only one.
    const Geometry g90 = MakeGeometry(f, 1920, 1080, 3840, 2160, .90f, false);
    const Geometry g91 = MakeGeometry(f, 1920, 1080, 3840, 2160, .91f, false);
    CHECK(g90.ordinaryW != g91.ordinaryW);
    CHECK(Settle::WorkSizeDiffers(IdentityOf(g90), IdentityOf(g91)));
    Settle::State step;
    CHECK(Settle::Decide(step, IdentityOf(g90), IdentityOf(g91), 1000) != Settle::Action::HistoryReset);
    CHECK(Settle::ParkScopeFor(IdentityOf(g90), IdentityOf(g91)) == Settle::ParkScope::WorkSize);
    // The output size reaches the maths through MakeGeometry: the pre-SR frame (1920x1080) inside the 3840x2160 output gets the same
    // share as a native 3840x2160 frame (the pixel inputs are output pixels), and the working size follows the frame.
    const Geometry gPre = MakeGeometry(f, 1920, 1080, 3840, 2160, 1.0f, false);
    const Geometry gNative = MakeGeometry(f, 3840, 2160, 3840, 2160, 1.0f, false);
    CHECK(gPre.spatial && gNative.spatial);
    CHECK(gPre.layout.pixelShare > gNative.layout.pixelShare - 0.01f && gPre.layout.pixelShare < gNative.layout.pixelShare + 0.01f);
    CHECK(gPre.workW == 1664 && gPre.workH == 976);
}

// The layout a geometry carries holds the aligned ordinary working size (what the unpack pass is told), not the raw rounded one
// Build starts from; and the offset limit is the one the Peripheral profile always had.
void OrdinaryGridOfTheLayoutAndTheOffsetLimit()
{
    const Settings f = FoveatedSettings();
    for (float scale : { 1.0f, .8f, .9f, .91f })
        for (const Settings& s : { f, Settings {} })
        {
            const Geometry g = MakeGeometry(s, 1920, 1080, 3840, 2160, scale, false);
            CHECK(g.ordinaryW == DlssNr::OrdinaryWorkSize(1920, scale) && g.ordinaryH == DlssNr::OrdinaryWorkSize(1080, scale));
            CHECK(g.layout.ordinaryW == g.ordinaryW && g.layout.ordinaryH == g.ordinaryH);
        }
    for (float scale : { 1.0f, .8f })
    {
        const Geometry g = MakeGeometry(f, 3840, 2160, 3840, 2160, scale, false);
        CHECK(g.layout.ordinaryW == g.ordinaryW && g.layout.ordinaryH == g.ordinaryH);
    }
    // the scale where the aligned size and the plain rounded one disagree, so that the check above can tell them apart
    CHECK(std::lround(1920 * .91f) == 1747 && DlssNr::OrdinaryWorkSize(1920, .91f) == 1744);
    CHECK(std::lround(1080 * .91f) == 983 && DlssNr::OrdinaryWorkSize(1080, .91f) == 976);
    CHECK(Build(f, 1920, 1080, .91f, 3840, 2160).ordinaryW == 1747);
    const Geometry g91 = MakeGeometry(f, 1920, 1080, 3840, 2160, .91f, false);
    CHECK(g91.layout.ordinaryW == 1744 && g91.layout.ordinaryH == 976);

    // MaxCenterOffset is the v0.4.0 formula: the zone stays inside the frame with half a percent to spare
    for (float c : { 10.0f, 50.0f, 80.0f, 95.0f })
        CHECK(MaxCenterOffset(c) == std::max(0.0f, (100.0f - c) * .5f - .5f));
    CHECK(MaxCenterOffset(10) == 44.5f && MaxCenterOffset(50) == 24.5f && MaxCenterOffset(80) == 9.5f && MaxCenterOffset(95) == 2.0f);
    CHECK(MaxCenterOffset(99.5f) == 0.0f && MaxCenterOffset(100) == 0.0f && MaxCenterOffset(120) == 0.0f);
}

// A pending change: every consumer reads the applied geometry, whatever the requested one says.
void WaitKeepsTheAppliedGeometryForEveryConsumer()
{
    const DlssNr::GuideRegions regions { { 0, 0, 3840, 2160 }, { 0, 0, 3840, 2160 } };
    const Geometry applied = MakeGeometry(FoveatedSettings(), 3840, 2160, 3840, 2160, 1.0f, false);
    Settings tweaked = FoveatedSettings();
    tweaked.foveaWidth = 2000; // a narrower centre: a different working size
    const Geometry requested = MakeGeometry(tweaked, 3840, 2160, 3840, 2160, 1.0f, false);
    CHECK(requested.workW != applied.workW && requested.layout != applied.layout);

    Settle::State state;
    const Settle::Identity appliedId = IdentityOf(applied), requestedId = IdentityOf(requested);
    const Settle::Action wait = Settle::Decide(state, appliedId, requestedId, 1000);
    CHECK(wait == Settle::Action::Wait);
    const Geometry& used = Settle::Pick(wait, applied, requested);
    CHECK(&used == &applied);
    // the working size, the model dimensions the menu reads, the fused choice, the status, and the three dispatches' constants
    CHECK(used.workW == applied.workW && used.workH == applied.workH);
    CHECK(((uint64_t(used.workW) << 32) | used.workH) == ((uint64_t(applied.workW) << 32) | applied.workH));
    CHECK(FusedNative(used, false, true) == FusedNative(applied, false, true));
    CHECK(StatusText(used, false, true) == StatusText(applied, false, true));
    for (uint32_t mode : { 100u, 101u, 102u })
    {
        const Constants got = MakeConstants(used.layout, mode, regions, 1, 1, 3840, 2160);
        CHECK(SameBytes(got, MakeConstants(applied.layout, mode, regions, 1, 1, 3840, 2160)));
        CHECK(!SameBytes(got, MakeConstants(requested.layout, mode, regions, 1, 1, 3840, 2160)));
    }
    const ResolveWarp rw = MakeResolveWarp(used.layout, true);
    const ResolveWarp rwApplied = MakeResolveWarp(applied.layout, true), rwRequested = MakeResolveWarp(requested.layout, true);
    CHECK(std::memcmp(&rw, &rwApplied, sizeof rw) == 0);
    CHECK(std::memcmp(&rw, &rwRequested, sizeof rw) != 0);
    // once the request has held for the settle time it is adopted by every consumer
    const Settle::Action rebuild = Settle::Decide(state, appliedId, requestedId, 1000 + Settle::kSettleMs);
    CHECK(rebuild == Settle::Action::Rebuild && &Settle::Pick(rebuild, applied, requested) == &requested);
    // a warp change that leaves the working size alone resets the history at once and is adopted
    Settings shifted = FoveatedSettings();
    shifted.foveaEdgeX = .5001f; // edge density moves, the model size does not
    const Geometry warpOnly = MakeGeometry(shifted, 3840, 2160, 3840, 2160, 1.0f, false);
    Settle::State fresh;
    CHECK(warpOnly.workW == applied.workW && warpOnly.layout != applied.layout);
    CHECK(Settle::Decide(fresh, appliedId, IdentityOf(warpOnly), 5) == Settle::Action::HistoryReset);
    // a frame-size change is never waited for
    const Geometry smaller = MakeGeometry(FoveatedSettings(), 2560, 1440, 3840, 2160, 1.0f, false);
    CHECK(Settle::Decide(fresh, appliedId, IdentityOf(smaller), 6) == Settle::Action::RebuildNow);
    CHECK(Settle::ParkScopeFor(appliedId, IdentityOf(smaller)) == Settle::ParkScope::All);
    CHECK(Settle::ParkScopeFor(appliedId, requestedId) == Settle::ParkScope::WorkSize);
    CHECK(Settle::ParkScopeFor(appliedId, IdentityOf(warpOnly)) == Settle::ParkScope::None);
}
} // namespace

int main()
{
    Defaults();
    SettingsEqualityDetectsEveryField();
    LayoutEqualityDetectsEveryEffectiveField();
    EveryFieldOfTheLayoutChangesEqualityAndKey();
    ProfileAndRequestedCountOnTheirOwn();
    FloatsInTheKeyHaveOneCanonicalForm();
    FoveatedLayoutSizes();
    ShaderConstantsOfEachProfile();
    Geometries();
    OrdinaryGridOfTheLayoutAndTheOffsetLimit();
    WaitKeepsTheAppliedGeometryForEveryConsumer();
    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: foveated spatial layout (settings and layout equality, output-to-frame sizes, constants per profile, geometry, applied layout while waiting)");
    return 0;
}
