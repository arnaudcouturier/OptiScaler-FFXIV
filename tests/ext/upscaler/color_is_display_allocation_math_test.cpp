// C4: pure decision math behind IRenderGeometry::ColorIsDisplayAllocation / ext::dx11bridge::PrepareUpscalerColor
// (docs/SEAMS-catalog.md "dx11bridge"; the oracle's IFeature_Dx11wDx12::PrepareForcedQualityColor). No upstream
// types, so this is a standalone unit test: no NVNGX, no Config, no link against the product -- same split as
// render_geometry_math_test.cpp for ResolveRenderExtentValue.
#include "ext/upscaler/RenderGeometry.h"

#include <cassert>
#include <cstdio>

using namespace ext::upscaler;

int main()
{
    // Disabled gate (RenderSizeFromWidthHeight off, the fork's default): always false, whatever the inputs say,
    // including the "should obviously scale" shape -- this is the deliberate coupling to C2/C3's own gate, not an
    // independent check that happens to agree with it.
    {
        assert(!ColorIsDisplayAllocationValue(false, 2258u, 1270u, 3840ull, 2160u, 3840u, 2160u));
    }

    // Enabled, but render size is not yet known (0 on either axis) -- a no-op, not a divide-by-zero or similar.
    {
        assert(!ColorIsDisplayAllocationValue(true, 0u, 1270u, 3840ull, 2160u, 3840u, 2160u));
        assert(!ColorIsDisplayAllocationValue(true, 2258u, 0u, 3840ull, 2160u, 3840u, 2160u));
    }

    // Enabled, render size larger than the allocation on either axis -- nonsensical (this feature only ever
    // shrinks), must stand down.
    {
        assert(!ColorIsDisplayAllocationValue(true, 3841u, 1270u, 3840ull, 2160u, 3840u, 2160u));
        assert(!ColorIsDisplayAllocationValue(true, 2258u, 2161u, 3840ull, 2160u, 3840u, 2160u));
    }

    // Enabled, render size already equals the allocation on both axes -- nothing to scale (the common case once
    // native quality is live, or whenever the game isn't lying about its buffer size).
    {
        assert(!ColorIsDisplayAllocationValue(true, 3840u, 2160u, 3840ull, 2160u, 3840u, 2160u));
    }

    // Enabled, render is a genuine shrink on both axes, but the game's subrect does not describe the full
    // allocation (some other crop is in play) -- don't second-guess it.
    {
        assert(!ColorIsDisplayAllocationValue(true, 2258u, 1270u, 3840ull, 2160u, 1920u, 1080u));
    }

    // The FFXIV case this feature exists for: render shrank on both axes (a forced quality preset), and the game's
    // subrect claims the whole 3840x2160 allocation -- scale.
    {
        assert(ColorIsDisplayAllocationValue(true, 2258u, 1270u, 3840ull, 2160u, 3840u, 2160u));
    }

    // Per-axis: one axis already matches the allocation, only the other shrank (letterbox/pillarbox-shaped), and
    // the subrect still claims the whole allocation -- the combined "both axes equal" early-out must not fire just
    // because one axis matches.
    {
        assert(ColorIsDisplayAllocationValue(true, 3840u, 1270u, 3840ull, 2160u, 3840u, 2160u));
        assert(ColorIsDisplayAllocationValue(true, 2258u, 2160u, 3840ull, 2160u, 3840u, 2160u));
    }

    // Subrect reported as absent/unreadable (0,0) with a genuine render shrink -- cannot confirm the subrect
    // describes the full allocation, so still a no-op.
    {
        assert(!ColorIsDisplayAllocationValue(true, 2258u, 1270u, 3840ull, 2160u, 0u, 0u));
    }

    std::puts("ColorIsDisplayAllocation: forced-quality colour-scale decision math passed");
    return 0;
}
