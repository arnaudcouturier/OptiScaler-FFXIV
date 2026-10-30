#pragma once
// The plate-draw rule, game side: one pure function over the draw state the game side read, so every condition can be
// tried without a game, a device or a lock (tests/ext/ffxiv/plates/plate_classify.cpp).
//
// A nameplate draw is the game drawing its own plate triangles: one colour target, standard alpha blending, a depth test
// against the scene depth that never writes, and one of the game's nameplate shader pairs. Everything else is a native
// draw. The rule **fails closed**: a draw that does not match every condition below is drawn by the game and is never
// taken into a plate image, so a wrong condition costs the feature, never a plate.
//
// The rule is the first build's classifier (draw-level, no game memory): what it cannot tell apart are draws that share
// the whole state with a nameplate draw, and those are moved into the image whole. The plate diagnostics census is what
// decides whether that happens.

#include <cstdint>

namespace ext::ffxiv::plates
{
// D3D11_COMPARISON_GREATER_EQUAL. Spelled out so this header needs no d3d11.h and stays usable from a CPU test.
inline constexpr uint8_t kDepthGreaterEqual = 7;

// The state of one indexed draw, read once at the draw through the context's Get* calls.
//
// Every field is a plain truth value and every default is "not a plate draw", so a zeroed DrawState classifies as a
// native draw even if a future field is forgotten by the reader.
struct DrawState
{
    bool immediateContext = false;  // the draw ran on the game's immediate context, not on a deferred one
    bool singleTarget = false;      // exactly one colour target is bound
    bool sdrTarget = false;         // ... and its format is one of the 8-bit SDR formats a plate slot is made of
    bool singleSample = false;      // ... and it is not multisampled
    bool noUav = false;             // no unordered access view is bound (a compute-style pass is not a plate draw)
    bool depthView = false;         // a depth-stencil view is bound
    bool depthSameSize = false;     // ... and its texture is the colour target's size
    bool depthTested = false;       // the depth test is on
    bool depthWriteOff = false;     // ... and it writes no depth
    bool depthGreaterEqual = false; // ... and its function is GREATER_EQUAL
    bool stencilOff = false;        // ... and no stencil is involved
    bool standardBlend = false;     // colour SRC_ALPHA / INV_SRC_ALPHA (ADD), alpha ONE / INV_SRC_ALPHA (ADD), all channels
    bool nameplateShader = false;   // the pixel shader is one of the four nameplate variants
    bool nameplateLayout = false;   // the input layout carries a nameplate vertex-shader tag
};

// True only when every condition holds. Written as one conjunction of positives so a missing check can only make the
// rule refuse more draws, never fewer.
constexpr bool IsPlateDraw(const DrawState& s)
{
    return s.immediateContext && s.singleTarget && s.sdrTarget && s.singleSample && s.noUav && s.depthView &&
           s.depthSameSize && s.depthTested && s.depthWriteOff && s.depthGreaterEqual && s.stencilOff &&
           s.standardBlend && s.nameplateShader && s.nameplateLayout;
}

// The four 8-bit SDR formats a plate slot is created from (the bridge's backbuffer copy): B8G8R8A8_UNORM is 87,
// R8G8B8A8_UNORM is 28. Anything else is refused, so a slot of another format is never drawn into with a target of this
// one.
constexpr bool IsSdrFormat(unsigned dxgiFormat) { return dxgiFormat == 87u || dxgiFormat == 28u; }
} // namespace ext::ffxiv::plates
