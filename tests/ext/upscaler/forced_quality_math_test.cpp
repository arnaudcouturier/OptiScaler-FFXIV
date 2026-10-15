// C1/C5: pure decision math behind ext::upscaler::ApplyForcedQuality and EffectivePerfQuality
// (docs/SEAMS-catalog.md "upscaler"). No upstream types, so this is a standalone unit test: no NVNGX, no Config,
// no link against the product.
#include "ext/upscaler/ForcedQuality.h"

#include <cassert>
#include <cstdio>

using namespace ext::upscaler;

int main()
{
    // WithinSlack: inclusive 32px tolerance, either direction.
    assert(WithinSlack(1000u, 1000u));
    assert(WithinSlack(1000u, 1032u));
    assert(WithinSlack(1032u, 1000u));
    assert(!WithinSlack(1000u, 1033u));
    assert(!WithinSlack(1033u, 1000u));

    // MatchesLastAnswer: nothing to check yet (queries == 0) -> apply regardless of dimensions.
    {
        ForcedQualityStatus none {};
        assert(MatchesLastAnswer(none, 1920u, 1080u, 3840u, 2160u));
    }

    // Matching answer within slack on all four dims -> apply. render = 1920x1080, display = 3840x2160.
    {
        ForcedQualityStatus answer { 1, 2, 1920u, 1080u, 3840u, 2160u };
        assert(MatchesLastAnswer(answer, 1919u, 1081u, 3841u, 2159u));
    }

    // A render-axis mismatch (one axis outside slack) -> stand down.
    {
        ForcedQualityStatus answer { 1, 2, 1920u, 1080u, 3840u, 2160u };
        assert(!MatchesLastAnswer(answer, 1400u, 1080u, 3840u, 2160u));
    }

    // A display-axis mismatch -> stand down.
    {
        ForcedQualityStatus answer { 1, 2, 1920u, 1080u, 3840u, 2160u };
        assert(!MatchesLastAnswer(answer, 1920u, 1080u, 3840u, 1400u));
    }

    // Render and display are NOT interchangeable: feeding the display pair where the render pair is expected (the
    // exact NGX naming trap between GetOptimalSettings' OutWidth/OutHeight-as-render and SetInitParameters'
    // Width/Height-as-render) must fail, even though both pairs individually are "near" something in the answer.
    {
        ForcedQualityStatus answer { 1, 2, 1920u, 1080u, 3840u, 2160u };
        assert(!MatchesLastAnswer(answer, /*width=*/3840u, /*height=*/2160u, /*outWidth=*/1920u, /*outHeight=*/1080u));
    }

    // EffectivePerfQualityValue: DLAA wins unconditionally, regardless of what is forced.
    {
        bool invalid = false;
        assert(EffectivePerfQualityValue(true, -1, 1, 5, invalid) == 5);
        assert(!invalid);
        assert(EffectivePerfQualityValue(true, 2, 1, 5, invalid) == 5);
        assert(!invalid);
    }

    // Unset (<0) -> asked, untouched.
    {
        bool invalid = false;
        assert(EffectivePerfQualityValue(false, -1, 3, 5, invalid) == 3);
        assert(!invalid);
    }

    // Forced in range -> forced.
    {
        bool invalid = false;
        assert(EffectivePerfQualityValue(false, 0, 3, 5, invalid) == 0);
        assert(!invalid);
    }

    // Forced out of range -> asked, flagged invalid (an ini typo, not a preset).
    {
        bool invalid = false;
        assert(EffectivePerfQualityValue(false, 9, 3, 5, invalid) == 3);
        assert(invalid);
    }

    // ShouldApplyForcedQuality: stands down on native DLAA, unset, out of range, or already what was asked.
    assert(!ShouldApplyForcedQuality(true, 2, 1, 5));   // native DLAA already owns the answer elsewhere
    assert(!ShouldApplyForcedQuality(false, -1, 1, 5)); // unset
    assert(!ShouldApplyForcedQuality(false, 9, 1, 5));  // out of range
    assert(!ShouldApplyForcedQuality(false, 1, 1, 5));  // already what the game asked -- nothing to relabel
    assert(ShouldApplyForcedQuality(false, 2, 1, 5));

    std::puts("ForcedQuality: slack math, EffectivePerfQuality and ApplyForcedQuality decisions passed");
    return 0;
}
