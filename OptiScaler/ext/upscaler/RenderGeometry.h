#pragma once
// Pure decision logic behind IRenderGeometry::ResolveRenderExtent (docs/SEAMS-catalog.md "upscaler"; C2/C3,
// restoring the oracle's adfbea9d gate -- docs/SEAMS.md 5 "RenderSizeFromWidthHeight"). No upstream types here, so
// this header is cheaply unit-testable on its own (tests/ext/upscaler), same split as ext/upscaler/ForcedQuality.h.

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
} // namespace ext::upscaler
