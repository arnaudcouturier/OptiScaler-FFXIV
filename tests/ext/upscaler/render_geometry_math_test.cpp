// C2/C3: pure decision math behind IRenderGeometry::ResolveRenderExtent / ext::upscaler::ResolveRenderGeometry
// (docs/SEAMS-catalog.md "upscaler"; the oracle's adfbea9d per-axis-min correction). No upstream types, so this is
// a standalone unit test: no NVNGX, no Config, no link against the product.
#include "ext/upscaler/RenderGeometry.h"

#include <cassert>
#include <cstdio>

using namespace ext::upscaler;

int main()
{
    // Disabled gate: always a no-op, whatever the inputs say, including the "should obviously correct" shape.
    {
        unsigned int w = 3840, h = 2160;
        assert(!ResolveRenderExtentValue(false, 2258u, 1270u, w, h));
        assert(w == 3840 && h == 2160);
    }

    // Enabled, but both params are absent (0,0) -- treated as "unreadable", no-op.
    {
        unsigned int w = 3840, h = 2160;
        assert(!ResolveRenderExtentValue(true, 0u, 0u, w, h));
        assert(w == 3840 && h == 2160);
    }

    // Enabled, params already agree with the subrect (the common case for every game that isn't FFXIV, and for
    // FFXIV once its buffer matches its frame) -- a no-op wherever they already agree.
    {
        unsigned int w = 1920, h = 1080;
        assert(!ResolveRenderExtentValue(true, 1920u, 1080u, w, h));
        assert(w == 1920 && h == 1080);
    }

    // Enabled, params are larger than the subrect on both axes -- Width/Height cannot be larger than the render,
    // so this never happens for a well-behaved game, but the function must still stand down (no axis shrank).
    {
        unsigned int w = 1920, h = 1080;
        assert(!ResolveRenderExtentValue(true, 3840u, 2160u, w, h));
        assert(w == 1920 && h == 1080);
    }

    // The FFXIV case this correction exists for: the game reports a 3840x2160 subrect (its colour buffer) but
    // Width/Height of 2258x1270 (what it genuinely rendered). Both axes shrink -> take the smaller pair.
    {
        unsigned int w = 3840, h = 2160;
        assert(ResolveRenderExtentValue(true, 2258u, 1270u, w, h));
        assert(w == 2258 && h == 1270);
    }

    // Per axis, not both together: letterboxing (width shrinks, height matches the subrect) must not drag the
    // height down to the width's own value, and must not leave the width untouched either.
    {
        unsigned int w = 1920, h = 1080;
        assert(ResolveRenderExtentValue(true, 1440u, 1080u, w, h));
        assert(w == 1440 && h == 1080);
    }

    // Pillarboxing: height shrinks, width matches the subrect.
    {
        unsigned int w = 1920, h = 1080;
        assert(ResolveRenderExtentValue(true, 1920u, 810u, w, h));
        assert(w == 1920 && h == 810);
    }

    // Exact equality on the shrinking axis is not "less than" -- stands down (matches the oracle's strict `<`,
    // no slack tolerance: that is a different, unrelated piece of logic in ForcedQuality.h).
    {
        unsigned int w = 1920, h = 1080;
        assert(!ResolveRenderExtentValue(true, 1920u, 1080u, w, h));
        assert(w == 1920 && h == 1080);
    }

    std::puts("RenderGeometry: per-axis-min correction math passed");
    return 0;
}
