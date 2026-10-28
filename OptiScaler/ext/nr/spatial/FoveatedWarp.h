#pragma once

// Foveated spatial profile for the NR pass. The design follows maximedgoulet/FoveatedDLSS5 (MIT, copyright Carlos Lopez Jr. 2025,
// from RenoDX): full density in a centre zone, a smoothstep falloff, then a flat edge plateau, separately per axis. The
// maths below is written independently from that description (see docs/ATTRIBUTION.md).
//
// Per axis and per side of the band centre, with r the distance from the band centre, h the half-length of the full-density
// zone, F the falloff length and e the edge density:
//   density d(r) = 1 up to h, 1 - (1 - e) * smoothstep((r - h) / F) across the falloff, e beyond it;
//   packed distance G(r) = integral of d from 0 to r, in closed form (the smoothstep integrates to t^3 - t^4 / 2);
//   Pack(x) = workCenter +- G(|x - bandCenter|) * scale, Unpack is the inverse (closed form outside the falloff, four Newton
//   steps inside it).
// The shader mirrors these functions (dlssnr_spatial_warp.hlsli); keep the two in step.
//
// Units: every length is in pixels of the frame the model processes. The pass converts the output-pixel settings with
// OutputToFrame before they get here.

#include "ext/nr/NrModelSize.h"
#include "ext/nr/third_party/peripheral_warp/SpatialWarp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace DlssNr::Foveated
{

constexpr float kDefaultWidth = 2400.0f, kDefaultHeight = 1350.0f, kDefaultFalloff = 400.0f, kDefaultEdge = 0.5f;
constexpr float kMinEdge = 0.25f, kMaxEdge = 1.0f;
constexpr float kMinAxisExtent = 16.0f;
constexpr uint32_t kAutoOffPercent = 95;         // a share above this goes back to ordinary NR (the saving is under 5%)
constexpr uint32_t kFlagFoveated = 2u;           // bit of pw::ShaderConstants::flags
constexpr uint32_t kResolveFoveatedBit = 0x100u; // bit of the main shader's gSpatialResidual

// One side of the band centre.
struct Side
{
    float span = 0;             // band centre to the frame edge
    float centre = 0;           // full-density half-length h, capped at the span
    float falloff = 0;          // F, capped so centre + falloff <= span
    float edge = 1;             // edge density e
    float packedFalloffEnd = 0; // G(h + F)
    float packedSpan = 0;       // G(span): the packed length of the whole side
};

struct Axis
{
    bool warped = false; // false: the axis is the identity (only the global working scale applies)
    uint32_t nativeExtent = 0, workExtent = 0;
    float centre = 0, falloff = 0, edge = 1, offsetPercent = 0; // effective inputs, after the clamp
    float bandCenter = 0, workCenter = 0, scale = 1, meanDensity = 1;
    Side side[2] {};
};

struct AxisInput
{
    uint32_t extent = 0;
    float centre = 0, falloff = 0, edge = 1, offsetPercent = 0;
};

struct AxisClamp
{
    bool identity = true;
    float centre = 0, falloff = 0, edge = 1, offsetPercent = 0;
};

struct Inputs
{
    uint32_t frameW = 0, frameH = 0;   // the frame the model processes
    uint32_t outputW = 0, outputH = 0; // the output the pixel settings are written against (0 = same as the frame)
    float width = kDefaultWidth, height = kDefaultHeight, falloff = kDefaultFalloff;
    float edgeX = kDefaultEdge, edgeY = kDefaultEdge;
    float offsetX = 0, offsetY = 0; // percent of the frame extent
    float globalScale = 1;
};

struct Layout
{
    Axis x, y;
    bool active = false;
    const char* reason = "disabled";
    uint32_t workW = 0, workH = 0, ordinaryW = 0, ordinaryH = 0;
    float share = 1; // workW * workH / (ordinaryW * ordinaryH)
};

// How far the full-density zone may move from the middle of the frame, in percent of the extent, so it stays inside it.
inline float MaxOffsetPercent(float centrePercent) { return std::max(0.0f, (100.0f - centrePercent) * .5f - .5f); }

// A pixel setting written against the output, in pixels of the frame the model processes.
inline float OutputToFrame(float outputPixels, uint32_t frameExtent, uint32_t outputExtent)
{
    if (outputExtent == 0)
        return outputPixels;
    return static_cast<float>(static_cast<double>(outputPixels) * frameExtent / outputExtent);
}

inline float PercentOf(float pixels, uint32_t extent)
{
    return extent == 0 ? 0.0f : static_cast<float>(static_cast<double>(pixels) * 100.0 / extent);
}

// The saving must be at least 5%: a share above 95% (compared in integers, exactly) goes back to ordinary NR.
inline bool ExceedsAutoOff(uint64_t workW, uint64_t workH, uint64_t ordinaryW, uint64_t ordinaryH)
{
    return workW * workH * 100u > ordinaryW * ordinaryH * kAutoOffPercent;
}

inline double PackedDistanceD(double r, double h, double F, double e)
{
    if (r <= h)
        return r;
    const double d = r - h;
    if (d < F)
    {
        const double t = d / F, t3 = t * t * t;
        return h + d - (1.0 - e) * F * (t3 - 0.5 * t3 * t);
    }
    return h + F - (1.0 - e) * F * 0.5 + e * (d - F);
}

// The inverse of PackedDistanceD. Inside the falloff band it solves in the normalised coordinate t in [0, 1]: the normalised
// packed distance t - (1 - e) * (t^3 - t^4 / 2) is increasing and concave, the chord is the starting guess and four Newton steps
// (each clamped back into [0, 1]) bring it well below a thousandth of a pixel.
inline double UnpackedDistanceD(double g, double h, double F, double e)
{
    if (g <= h)
        return g;
    const double q = 1.0 - (1.0 - e) * 0.5; // the normalised packed length of the whole falloff band
    const double target = F > 0 ? (g - h) / F : 0.0;
    if (F <= 0 || target >= q)
        return h + F + (g - (h + F * q)) / e;
    double t = target / q;
    for (int step = 0; step < 4; ++step)
    {
        const double t2 = t * t;
        const double packed = t - (1.0 - e) * (t2 * t - 0.5 * t2 * t2);
        const double slope = 1.0 - (1.0 - e) * t2 * (3.0 - 2.0 * t);
        t = std::clamp(t - (packed - target) / slope, 0.0, 1.0);
    }
    return h + F * t;
}

inline float PackedDistance(const Side& side, float r)
{
    return static_cast<float>(PackedDistanceD(r, side.centre, side.falloff, side.edge));
}

inline float UnpackedDistance(const Side& side, float g)
{
    return static_cast<float>(UnpackedDistanceD(g, side.centre, side.falloff, side.edge));
}

// The rules that make the pixel inputs safe for the frame the model processes this frame. Only the effective values are
// clamped; the stored settings are never touched.
inline AxisClamp ClampAxis(const AxisInput& in)
{
    AxisClamp c;
    c.centre = static_cast<float>(in.extent);
    c.falloff = 0.0f;
    c.edge = 1.0f;
    c.offsetPercent = 0.0f;
    if (!std::isfinite(in.centre) || !std::isfinite(in.falloff) || !std::isfinite(in.edge) || !std::isfinite(in.offsetPercent))
        return c;
    if (static_cast<float>(in.extent) < kMinAxisExtent)
        return c;
    const float edge = std::clamp(in.edge, kMinEdge, kMaxEdge);
    if (edge >= kMaxEdge)
        return c;
    if (in.centre >= static_cast<float>(in.extent))
        return c;
    c.identity = false;
    c.edge = edge;
    c.centre = std::max(0.0f, in.centre);
    c.falloff = std::max(0.0f, in.falloff);
    const float limit = MaxOffsetPercent(PercentOf(c.centre, in.extent));
    c.offsetPercent = std::clamp(in.offsetPercent, -limit, limit);
    return c;
}

inline Axis BuildAxis(const AxisInput& in, float globalScale)
{
    Axis a {};
    const AxisClamp c = ClampAxis(in);
    const double n = in.extent;
    const double scaleIn = std::isfinite(globalScale) && globalScale > 0 ? globalScale : 1.0;
    a.warped = !c.identity;
    a.nativeExtent = in.extent;
    a.centre = c.centre;
    a.falloff = c.falloff;
    a.edge = c.edge;
    a.offsetPercent = c.offsetPercent;
    const double bandCenter = (0.5 + static_cast<double>(c.offsetPercent) * 0.01) * n;
    const double span[2] = { bandCenter, n - bandCenter };
    double packedSpan[2] = {};
    for (int i = 0; i != 2; ++i)
    {
        Side& side = a.side[i];
        side.span = static_cast<float>(span[i]);
        if (c.identity)
        {
            side.centre = side.span;
            side.falloff = 0.0f;
            side.edge = 1.0f;
        }
        else
        {
            side.centre = static_cast<float>(std::min(static_cast<double>(c.centre) * 0.5, span[i]));
            side.falloff = static_cast<float>(std::min(static_cast<double>(c.falloff), std::max(0.0, span[i] - side.centre)));
            side.edge = c.edge;
        }
        const double h = side.centre, F = side.falloff, e = side.edge;
        side.packedFalloffEnd = static_cast<float>(PackedDistanceD(h + F, h, F, e));
        packedSpan[i] = PackedDistanceD(span[i], h, F, e);
        side.packedSpan = static_cast<float>(packedSpan[i]);
    }
    const double total = packedSpan[0] + packedSpan[1];
    a.meanDensity = static_cast<float>(total / n);
    a.workExtent = AlignWorkSize(EvenExtent(in.extent, scaleIn * total / n), in.extent);
    const double scale = a.workExtent / total;
    a.scale = static_cast<float>(scale);
    a.bandCenter = static_cast<float>(bandCenter);
    a.workCenter = static_cast<float>(packedSpan[0] * scale);
    return a;
}

inline float Pack(float pixel, const Axis& axis)
{
    const double delta = static_cast<double>(pixel) - axis.bandCenter;
    const Side& side = axis.side[delta < 0 ? 0 : 1];
    const double g = PackedDistanceD(std::abs(delta), side.centre, side.falloff, side.edge);
    return static_cast<float>(axis.workCenter + (delta < 0 ? -1.0 : 1.0) * g * axis.scale);
}

inline float Unpack(float workPixel, const Axis& axis)
{
    const double delta = static_cast<double>(workPixel) - axis.workCenter;
    const Side& side = axis.side[delta < 0 ? 0 : 1];
    const double r = UnpackedDistanceD(std::abs(delta) / axis.scale, side.centre, side.falloff, side.edge);
    return static_cast<float>(axis.bandCenter + (delta < 0 ? -1.0 : 1.0) * r);
}

inline Layout BuildLayout(const Inputs& in)
{
    Layout l {};
    l.reason = "invalid frame or global scale";
    if (in.frameW < 2 || in.frameH < 2 || !std::isfinite(in.globalScale) || in.globalScale < .25f || in.globalScale > 2.0f)
        return l;
    AxisInput ax;
    ax.extent = in.frameW;
    ax.centre = OutputToFrame(in.width, in.frameW, in.outputW);
    ax.falloff = OutputToFrame(in.falloff, in.frameW, in.outputW);
    ax.edge = in.edgeX;
    ax.offsetPercent = in.offsetX;
    AxisInput ay;
    ay.extent = in.frameH;
    ay.centre = OutputToFrame(in.height, in.frameH, in.outputH);
    ay.falloff = OutputToFrame(in.falloff, in.frameH, in.outputH);
    ay.edge = in.edgeY;
    ay.offsetPercent = in.offsetY;
    l.x = BuildAxis(ax, in.globalScale);
    l.y = BuildAxis(ay, in.globalScale);
    l.workW = l.x.workExtent;
    l.workH = l.y.workExtent;
    l.ordinaryW = OrdinaryWorkSize(in.frameW, in.globalScale);
    l.ordinaryH = OrdinaryWorkSize(in.frameH, in.globalScale);
    l.share = static_cast<float>(static_cast<double>(l.workW) * l.workH / (static_cast<double>(l.ordinaryW) * l.ordinaryH));
    if (l.workW < in.frameW * .25f || l.workH < in.frameH * .25f)
    {
        l.reason = "effective model extent below 25%";
        return l;
    }
    if (ExceedsAutoOff(l.workW, l.workH, l.ordinaryW, l.ordinaryH))
    {
        l.reason = "saving under 5%";
        return l;
    }
    l.active = true;
    l.reason = "active";
    return l;
}

// The shader constants. The per-side slots of the peripheral profile carry the foveated parameters instead (centre -> h,
// work -> F, compression -> e, edgeSlope -> G(h + F)); a bit in `flags` tells the shader which reading applies.
inline pw::ShaderConstants MakeShaderConstants(const Layout& layout)
{
    const Axis& x = layout.x;
    const Axis& y = layout.y;
    pw::ShaderConstants k {};
    k.nativeWidth = static_cast<float>(x.nativeExtent);
    k.nativeHeight = static_cast<float>(y.nativeExtent);
    k.workWidth = static_cast<float>(x.workExtent);
    k.workHeight = static_cast<float>(y.workExtent);
    k.centerFractionX = x.centre / k.nativeWidth;
    k.centerFractionY = y.centre / k.nativeHeight;
    k.workFractionX = k.workWidth / k.nativeWidth;
    k.workFractionY = k.workHeight / k.nativeHeight;
    k.compressionX = x.edge;
    k.compressionY = y.edge;
    k.edgeSlopeX = x.edge;
    k.edgeSlopeY = y.edge;
    k.mode = 2u;
    k.colorFilter = 1u;
    k.flags = 1u | kFlagFoveated;
    k.reserved = 0u;
    k.bandCenterX = x.bandCenter;
    k.bandCenterY = y.bandCenter;
    k.workCenterX = x.workCenter;
    k.workCenterY = y.workCenter;
    k.halfSpanNegX = x.side[0].span;
    k.halfSpanNegY = y.side[0].span;
    k.sideCenterNegX = x.side[0].centre;
    k.sideCenterNegY = y.side[0].centre;
    k.halfSpanPosX = x.side[1].span;
    k.halfSpanPosY = y.side[1].span;
    k.sideCenterPosX = x.side[1].centre;
    k.sideCenterPosY = y.side[1].centre;
    k.sideWorkNegX = x.side[0].falloff;
    k.sideWorkNegY = y.side[0].falloff;
    k.sideCompressionNegX = x.side[0].edge;
    k.sideCompressionNegY = y.side[0].edge;
    k.sideWorkPosX = x.side[1].falloff;
    k.sideWorkPosY = y.side[1].falloff;
    k.sideCompressionPosX = x.side[1].edge;
    k.sideCompressionPosY = y.side[1].edge;
    k.sideEdgeSlopeNegX = x.side[0].packedFalloffEnd;
    k.sideEdgeSlopeNegY = y.side[0].packedFalloffEnd;
    k.sideEdgeSlopePosX = x.side[1].packedFalloffEnd;
    k.sideEdgeSlopePosY = y.side[1].packedFalloffEnd;
    k.workScaleX = x.scale;
    k.workScaleY = y.scale;
    k.centerOffsetX = x.bandCenter / k.nativeWidth - .5f;
    k.centerOffsetY = y.bandCenter / k.nativeHeight - .5f;
    return k;
}

} // namespace DlssNr::Foveated
