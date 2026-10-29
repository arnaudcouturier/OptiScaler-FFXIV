#include "ext/nr/NrModelSize.h"
#include <cassert>
#include <initializer_list>
#include <cstdio>

namespace {
int failures = 0;
void Check(bool ok, const char* what, int line) {
    if (!ok) {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

// The working size the spatial layouts are built on: even, never above the even part of the native size when reducing, and
// untouched at exactly native.
void EvenExtentRules() {
    using DlssNr::EvenExtent;
    // a fraction of exactly one is the native size, odd or not
    CHECK(EvenExtent(1081, 1.0) == 1081 && EvenExtent(1080, 1.0) == 1080 && EvenExtent(3, 1.0) == 3);
    // rounded UP to an even count: ceil first (400.5 -> 401), then up to the next even (402); floor would give 400
    CHECK(EvenExtent(1000, .4005) == 402);
    CHECK(EvenExtent(1000, .4001) == 402 && EvenExtent(1000, .4009) == 402);
    CHECK(EvenExtent(1000, .4) == 400 && EvenExtent(999, .4) == 400); // 399.6 -> 400 (already even)
    CHECK(EvenExtent(1001, 1.2) == 1202 && EvenExtent(1000, 1.5) == 1500); // above native there is no cap
    // within rounding noise of a whole number it stays on that number (400.0000001 does not become 401 -> 402)
    CHECK(EvenExtent(1000, .4000000001) == 400);
    CHECK(EvenExtent(1000, .3999999999) == 400);
    CHECK(EvenExtent(2000, .2000000001) == 400);
    // ... and a real fraction of a pixel (400.001) still rounds up
    CHECK(EvenExtent(1000, .400001) == 402);
    CHECK(EvenExtent(1000, .4 + 1e-6) == 402);
    // a reduction never goes above the even part of the native size (1080.89 -> 1081 -> 1082 -> 1080)
    CHECK(EvenExtent(1081, .9999) == 1080);
    CHECK(EvenExtent(1080, .9999) == 1080 && EvenExtent(1080, .99999) == 1080);
    CHECK(EvenExtent(3, .9) == 2 && EvenExtent(5, .95) == 4);
    // never below two
    CHECK(EvenExtent(1, .1) == 2 && EvenExtent(3, .1) == 2 && EvenExtent(2, .01) == 2 && EvenExtent(0, .5) == 2);
    CHECK(EvenExtent(3840, .8) == 3072 && EvenExtent(2160, .5) == 1080);
}

void OrdinaryWorkSizeRoundsHalfUp() {
    using DlssNr::OrdinaryWorkSize;
    // 1007 * 0.5 = 503.5: rounded half up it is 504 and lands on the 512 of the 16 grid; truncated, 503 lands on 496
    CHECK(OrdinaryWorkSize(1007, .5f) == 512);
    CHECK(OrdinaryWorkSize(1001, .5f) == 496); // 500.5 -> 501 and 500 both fall on 496, so this one cannot tell the two apart
    CHECK(OrdinaryWorkSize(1920, .91f) == 1744 && OrdinaryWorkSize(1080, .91f) == 976 && OrdinaryWorkSize(1080, .9f) == 976);
    CHECK(OrdinaryWorkSize(1080, 1.0f) == 1080 && OrdinaryWorkSize(1081, 1.0f) == 1081);
    CHECK(OrdinaryWorkSize(3840, .8f) == 3072 && OrdinaryWorkSize(2160, .8f) == 1728);
}

void AlignWorkSizeNeverRoundsAReductionUpPastNative() {
    using DlssNr::AlignWorkSize;
    // 1080 would round to 1088, which is above the 1081 it was meant to reduce
    CHECK(AlignWorkSize(1080, 1081) == 1081);
    CHECK(AlignWorkSize(1079, 1081) == 1072 && AlignWorkSize(1075, 1081) == 1072);
    CHECK(AlignWorkSize(1081, 1081) == 1081);
    // an enlargement may go up to the grid
    CHECK(AlignWorkSize(1084, 1081) == 1088 && AlignWorkSize(1100, 1081) == 1104);
    CHECK(AlignWorkSize(1, 1081) == 16 && AlignWorkSize(7, 1081) == 16);
}


void WorkScaleAndGuideRulesOfTheAxes() {
    using DlssNr::EffectiveAxisScale;
    using DlssNr::SanitizeWorkScale;
    CHECK(SanitizeWorkScale(.24f) == .25f && SanitizeWorkScale(2.01f) == 2.0f && SanitizeWorkScale(.8f) == .8f);
    CHECK(EffectiveAxisScale(0.0f, .6f) == .6f && EffectiveAxisScale(.9f, .6f) == .9f && EffectiveAxisScale(-1.0f, .6f) == .6f);
    // the guides are matched only when the model is reduced on both axes, unless the axes differ (axis_scale has the cases)
    CHECK(DlssNr::MatchGuidesToWork(false, 960, 540, 1920, 1080, false) && !DlssNr::MatchGuidesToWork(false, 960, 1080, 1920, 1080, false));
}
} // namespace

int main() {
    using DlssNr::AlignWorkSize;
    for (unsigned native : {720u, 1080u, 1440u, 2160u, 1081u, 3u}) {
        assert(AlignWorkSize(native, native) == native);
        for (unsigned scaled = 1; scaled <= native * 2; ++scaled) {
            const auto actual = AlignWorkSize(scaled, native);
            assert(actual > 0);
            assert(actual == native || actual % 16 == 0);
            if (scaled < native) assert(actual <= native);
        }
    }
    assert(AlignWorkSize(723, 1080) == 720);
    assert(AlignWorkSize(1447, 2160) == 1440);
    std::puts("PASS: native sizes unchanged; resampled sizes aligned without overshooting native on reduction");

    EvenExtentRules();
    OrdinaryWorkSizeRoundsHalfUp();
    AlignWorkSizeNeverRoundsAReductionUpPastNative();
    WorkScaleAndGuideRulesOfTheAxes();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: even extents (round up to even, noise tolerance, reduction cap, floor of two, exact native), ordinary size rounding, align cap");
    return 0;
}
