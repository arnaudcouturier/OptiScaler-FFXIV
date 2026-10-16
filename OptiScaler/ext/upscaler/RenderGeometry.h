#pragma once
// Pure decision logic behind IRenderGeometry::ResolveRenderExtent (docs/SEAMS-catalog.md "upscaler"; C2/C3,
// restoring the oracle's adfbea9d gate -- docs/SEAMS.md 5 "RenderSizeFromWidthHeight"). No upstream types here, so
// this header is cheaply unit-testable on its own (tests/ext/upscaler), same split as ext/upscaler/ForcedQuality.h.

#include <cstdint>

namespace ext::upscaler
{
// NGX means DLSS_Render_Subrect_Dimensions to be the part of the colour buffer that holds this frame -- the render
// size. FFXIV instead reports the buffer's own dimensions there: a 3840x2160 subrect alongside Width/Height of
// 2258x1270, which is what it genuinely rendered. Width/Height cannot be larger than the render, so the smaller of
// the two is the render size on whichever axis disagrees -- a no-op wherever they already agree.
//
// Per axis, not both together: letterboxing and pillarboxing scale one axis and not the other, so a game can
// legitimately report a smaller width beside an equal height. Replacing the pair whenever either shrank would take
// the larger of the two on the other axis and hand the runtime a frame taller or wider than the one that exists.
//
// `enabled` is the caller's gate (the RenderSizeFromWidthHeight config flag, default off); when false this is
// always a no-op. `paramWidth`/`paramHeight` are the game's own NVSDK_NGX_Parameter_Width/Height for this evaluate
// (0 = absent/unreadable, also a no-op). Returns whether `w`/`h` were corrected.
inline bool ResolveRenderExtentValue(bool enabled, unsigned int paramWidth, unsigned int paramHeight,
                                      unsigned int& w, unsigned int& h)
{
    if (!enabled || paramWidth == 0 || paramHeight == 0 || !(paramWidth < w || paramHeight < h))
        return false;

    w = paramWidth < w ? paramWidth : w;
    h = paramHeight < h ? paramHeight : h;
    return true;
}

// Pure decision logic behind IRenderGeometry::ColorIsDisplayAllocation (C4, docs/SEAMS-catalog.md "dx11bridge"
// PrepareUpscalerColor; the oracle's IFeature_Dx11wDx12::PrepareForcedQualityColor). No upstream types, same
// unit-testable split as ResolveRenderExtentValue above.
//
// `enabled` is the same RenderSizeFromWidthHeight config flag ResolveRenderExtentValue reads: the oracle gates both
// the geometry correction and this colour scale off a single !FfxivNativeQuality::Available() check, so this fork
// couples the two to the same flag instead (docs/SEAMS.md 5 already restored the explicit flag for the geometry
// side; this mirrors it rather than inventing a second, independent gate for the colour side). Provably redundant
// with the renderWidth/allocWidth comparison below in every case, not just steady state: ext::dx11bridge::
// PrepareUpscalerColor resolves `renderWidth`/`renderHeight` fresh for the CURRENT frame (ext::upscaler::
// ResolveRenderGeometry, seeded from this frame's subrect) rather than reading the feature's RenderWidth()/
// RenderHeight() cache, which used to still hold the previous frame's answer on the frame the flag flips -- the one
// transition the comparison alone could not have covered on its own. With that staleness gone, this flag read is
// kept anyway as a cheap, explicit mirror of the geometry gate: documenting the coupling here is cheaper than
// re-deriving "redundant now, for how long" from the comparison alone.
//
// `renderWidth`/`renderHeight` are the live feature's current render size (0 = not yet known -- a no-op).
// `allocWidth`/`allocHeight` are the colour resource's own allocation (GetDesc()); `allocWidth` is uint64_t to
// match D3D12_RESOURCE_DESC::Width exactly, avoiding a narrowing comparison. Scaling only makes sense when the
// render size is a real, in-bounds shrink of the allocation: not larger than it (nothing to scale up to) and not
// already equal to it (nothing to do).
//
// `subrectWidth`/`subrectHeight` are the game's own NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_* for this
// evaluate (0 = absent/unreadable). They must describe the whole allocation, not some other crop -- otherwise the
// game's subrect is doing something this feature should not second-guess.
inline bool ColorIsDisplayAllocationValue(bool enabled, unsigned int renderWidth, unsigned int renderHeight,
                                           uint64_t allocWidth, unsigned int allocHeight, unsigned int subrectWidth,
                                           unsigned int subrectHeight)
{
    if (!enabled || renderWidth == 0 || renderHeight == 0 || renderWidth > allocWidth || renderHeight > allocHeight ||
        (renderWidth == allocWidth && renderHeight == allocHeight))
        return false;

    return subrectWidth == allocWidth && subrectHeight == allocHeight;
}
} // namespace ext::upscaler
