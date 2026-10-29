#pragma once

#include "ext/nr/presr/DlssNr_Guides.h"
#include "ext/nr/NrModelSize.h"
#include "ext/nr/NrSettle.h"
#include "ext/nr/spatial/FoveatedWarp.h"
#include "ext/nr/third_party/peripheral_warp/SpatialWarp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace DlssNr::Spatial
{

// Which spatial profile the master switch (SpatialCompression) drives.
enum class Profile : uint32_t
{
    Peripheral = 0, // the compression curve of PeripheralWarp: centre and work budget as percentages
    Foveated = 1,   // full-density centre zone, smoothstep falloff, flat edge plateau; sizes in output pixels
};

struct Settings
{
    bool enabled = false;
    Profile profile = Profile::Peripheral;
    float centerX = 80, centerY = 80;
    float workX = 90, workY = 90;
    float offsetX = 0, offsetY = 0; // both profiles: percent of the frame extent
    float shiftX = 0, shiftY = 0;   // Peripheral only
    // Foveated only. Pixels of the output (what the user sees), not of the frame the model processes.
    float foveaWidth = Foveated::kDefaultWidth, foveaHeight = Foveated::kDefaultHeight;
    float foveaFalloff = Foveated::kDefaultFalloff;
    float foveaEdgeX = Foveated::kDefaultEdge, foveaEdgeY = Foveated::kDefaultEdge;
    bool operator==(const Settings& o) const
    {
        const auto same = [](float a, float b) { return a == b || (std::isnan(a) && std::isnan(b)); };
        return enabled == o.enabled && profile == o.profile && same(centerX, o.centerX) && same(centerY, o.centerY) &&
               same(workX, o.workX) && same(workY, o.workY) && same(offsetX, o.offsetX) && same(offsetY, o.offsetY) &&
               same(shiftX, o.shiftX) && same(shiftY, o.shiftY) && same(foveaWidth, o.foveaWidth) &&
               same(foveaHeight, o.foveaHeight) && same(foveaFalloff, o.foveaFalloff) && same(foveaEdgeX, o.foveaEdgeX) &&
               same(foveaEdgeY, o.foveaEdgeY);
    }
    bool operator!=(const Settings& o) const { return !(*this == o); }
};

template <class ConfigLike> Settings ReadSettings(const ConfigLike& nr)
{
    Settings s;
    s.enabled = nr.spatialCompression.value_or_default();
    s.profile = nr.spatialProfile.value_or_default() == static_cast<uint32_t>(Profile::Foveated) ? Profile::Foveated
                                                                                                 : Profile::Peripheral;
    s.centerX = nr.spatialCenterX.value_or_default();
    s.centerY = nr.spatialCenterY.value_or_default();
    s.workX = nr.spatialWorkX.value_or_default();
    s.workY = nr.spatialWorkY.value_or_default();
    s.offsetX = nr.spatialOffsetX.value_or_default();
    s.offsetY = nr.spatialOffsetY.value_or_default();
    s.shiftX = nr.spatialShiftX.value_or_default();
    s.shiftY = nr.spatialShiftY.value_or_default();
    s.foveaWidth = nr.foveaWidth.value_or_default();
    s.foveaHeight = nr.foveaHeight.value_or_default();
    s.foveaFalloff = nr.foveaFalloff.value_or_default();
    s.foveaEdgeX = nr.foveaEdgeX.value_or_default();
    s.foveaEdgeY = nr.foveaEdgeY.value_or_default();
    return s;
}

inline float MaxCenterOffset(float centerPercent) { return Foveated::MaxOffsetPercent(centerPercent); }

inline float MinimumWorkPercent(float globalScale)
{
    return std::isfinite(globalScale) && globalScale > 0 ? std::clamp(25.0f / globalScale, 25.0f, 100.0f) : 100.0f;
}

inline std::pair<float, float> WorkShiftLimits(float centerPercent, float workPercent, float offsetPercent)
{
    const float c = centerPercent * .01f, w = workPercent * .01f;
    const float halfBand = c * .5f, bandCenter = .5f + offsetPercent * .01f;
    const float periphery[2] = { std::max(0.0f, bandCenter - halfBand), std::max(0.0f, 1.0f - bandCenter - halfBand) };
    const float budget = std::max(0.0f, w - c);
    const int narrow = periphery[0] <= periphery[1] ? 0 : 1;
    float base[2] = {};
    base[narrow] = std::min(.5f * budget, periphery[narrow]);
    base[1 - narrow] = std::min(budget - base[narrow], periphery[1 - narrow]);
    return { -100.0f * std::max(0.0f, std::min(base[1], periphery[0] - base[0])),
             100.0f * std::max(0.0f, std::min(base[0], periphery[1] - base[1])) };
}

inline std::pair<float, float> WorkShiftLimits(const Settings& s, bool yAxis)
{
    return yAxis ? WorkShiftLimits(s.centerY, s.workY, s.offsetY) : WorkShiftLimits(s.centerX, s.workX, s.offsetX);
}

struct Rect
{
    float left = 0, top = 0, right = 1, bottom = 1;
};

struct Layout
{
    Settings settings {};
    Profile profile = Profile::Peripheral;
    bool requested = false, active = false;
    std::string reason;
    uint32_t nativeW = 0, nativeH = 0, ordinaryW = 0, ordinaryH = 0, modelW = 0, modelH = 0;
    float globalScale = 1;  // the model's working scale of the horizontal axis
    float globalScaleY = 1; // ... and of the vertical axis (the same unless the model runs at a different scale per axis)
    float pixelShare = 1; // the share of the ordinary NR pixels the model actually processes (1 when the layout is inactive)
    pw::Layout warp {};
    Foveated::Layout fovea {};
    Rect centerBounds {}, workBounds {}; // Native-frame normalized extents, including offset and shift.

    // Everything that makes two layouts run differently, as one fixed list of exact values (a NaN is one value, -0 is 0). Equality
    // and the settle key both come from it, so a field cannot be in one and forgotten in the other. The Foveated profile
    // contributes its effective values (after the clamp against the frame), the Peripheral one its settings; a disabled layout
    // contributes only that it is disabled.
    static constexpr size_t kKeyFields = 19;
    std::array<uint64_t, kKeyFields> KeyFields() const
    {
        std::array<uint64_t, kKeyFields> k {};
        const auto bits = [](double v)
        {
            if (std::isnan(v))
                return uint64_t { 0x7ff8000000000000ull };
            if (v == 0)
                v = 0;
            uint64_t u = 0;
            std::memcpy(&u, &v, sizeof u);
            return u;
        };
        size_t i = 0;
        k[i++] = requested ? 1 : 0;
        k[i++] = nativeW;
        k[i++] = nativeH;
        k[i++] = bits(globalScale);
        k[i++] = bits(globalScaleY);
        if (!requested)
            return k;
        k[i++] = active ? 1 : 0;
        k[i++] = static_cast<uint64_t>(profile);
        if (profile == Profile::Peripheral)
        {
            for (float v : { settings.centerX, settings.centerY, settings.workX, settings.workY, settings.offsetX,
                             settings.offsetY, settings.shiftX, settings.shiftY })
                k[i++] = bits(v);
            return k;
        }
        for (const Foveated::Axis* axis : { &fovea.x, &fovea.y })
        {
            k[i++] = axis->warped ? 1 : 0;
            k[i++] = bits(axis->centre);
            k[i++] = bits(axis->falloff);
            k[i++] = bits(axis->edge);
            k[i++] = bits(axis->offsetPercent);
            k[i++] = axis->workExtent;
        }
        return k;
    }
    bool operator==(const Layout& o) const { return KeyFields() == o.KeyFields(); }
    bool operator!=(const Layout& o) const { return !(*this == o); }
    // A 64-bit digest of KeyFields for the settle decision (FNV-1a).
    uint64_t Key() const
    {
        uint64_t h = 1469598103934665603ull;
        for (uint64_t v : KeyFields())
            for (int shift = 0; shift < 64; shift += 8)
                h = (h ^ ((v >> shift) & 0xffu)) * 1099511628211ull;
        return h;
    }
};

using DlssNr::EvenExtent;
using DlssNr::SanitizeWorkScale;

// The layout for a frame of nativeW x nativeH that the model processes. outputW x outputH is the output the Foveated pixel settings
// are written against: the pre-SR pass runs on an active rectangle inside a larger colour allocation, so the two differ there
// (0 means the same as the frame). The settings themselves are never changed.
inline Layout BuildAxes(const Settings& s, uint32_t nativeW, uint32_t nativeH, float globalScale, float globalScaleY,
                        uint32_t outputW = 0, uint32_t outputH = 0)
{
    Layout result {};
    result.settings = s;
    result.profile = s.profile;
    result.requested = s.enabled;
    result.nativeW = nativeW;
    result.nativeH = nativeH;
    result.globalScale = globalScale;
    result.globalScaleY = globalScaleY;
    const float ordinaryScaleX = std::isfinite(globalScale) ? std::clamp(globalScale, .25f, 2.0f) : 1.0f;
    const float ordinaryScaleY = std::isfinite(globalScaleY) ? std::clamp(globalScaleY, .25f, 2.0f) : 1.0f;
    result.ordinaryW = std::max(1u, static_cast<uint32_t>(std::lround(nativeW * ordinaryScaleX)));
    result.ordinaryH = std::max(1u, static_cast<uint32_t>(std::lround(nativeH * ordinaryScaleY)));
    result.modelW = result.ordinaryW;
    result.modelH = result.ordinaryH;
    if (!s.enabled)
    {
        result.reason = "disabled";
        return result;
    }
    if (nativeW < 2 || nativeH < 2 || !std::isfinite(globalScale) || globalScale < .25f || globalScale > 2.0f ||
        !std::isfinite(globalScaleY) || globalScaleY < .25f || globalScaleY > 2.0f)
    {
        result.reason = "invalid frame or global scale";
        return result;
    }
    if (s.profile == Profile::Foveated)
    {
        Foveated::Inputs in;
        in.frameW = nativeW;
        in.frameH = nativeH;
        in.outputW = outputW;
        in.outputH = outputH;
        in.width = s.foveaWidth;
        in.height = s.foveaHeight;
        in.falloff = s.foveaFalloff;
        in.edgeX = s.foveaEdgeX;
        in.edgeY = s.foveaEdgeY;
        in.offsetX = s.offsetX;
        in.offsetY = s.offsetY;
        in.globalScale = globalScale;
        in.globalScaleY = globalScaleY;
        result.fovea = Foveated::BuildLayout(in);
        result.reason = result.fovea.reason;
        if (result.fovea.active)
        {
            result.modelW = result.fovea.workW;
            result.modelH = result.fovea.workH;
            result.pixelShare = result.fovea.share;
            result.active = true;
        }
        return result;
    }
    const auto validAxis = [](float center, float work, float offset, float shift)
    {
        if (!std::isfinite(center) || !std::isfinite(work) || !std::isfinite(offset) || !std::isfinite(shift) ||
            center <= 0 || work < 25 || work > 100 || center >= work ||
            std::abs(offset) > MaxCenterOffset(center) + 1e-4f)
            return false;
        const auto limits = WorkShiftLimits(center, work, offset);
        return shift >= limits.first - 1e-4f && shift <= limits.second + 1e-4f;
    };
    if (!validAxis(s.centerX, s.workX, s.offsetX, s.shiftX) || !validAxis(s.centerY, s.workY, s.offsetY, s.shiftY))
    {
        result.reason = "invalid centre, work, offset or shift";
        return result;
    }
    const uint32_t rawW = EvenExtent(nativeW, s.workX * .01);
    const uint32_t rawH = EvenExtent(nativeH, s.workY * .01);
    const uint32_t modelW = AlignWorkSize(EvenExtent(nativeW, static_cast<double>(globalScale) * s.workX * .01), nativeW);
    const uint32_t modelH = AlignWorkSize(EvenExtent(nativeH, static_cast<double>(globalScaleY) * s.workY * .01), nativeH);
    if (modelW < nativeW * .25f || modelH < nativeH * .25f || s.centerX * .01f >= static_cast<float>(rawW) / nativeW ||
        s.centerY * .01f >= static_cast<float>(rawH) / nativeH)
    {
        result.reason = "effective model extent below 25% or centre outside work";
        return result;
    }
    if (s.workX == 100 && s.workY == 100)
    {
        result.reason = "work is 100% on both axes";
        return result;
    }
    result.warp.x = pw::BuildAxis(nativeW, rawW, modelW, s.centerX * .01f, s.offsetX * .01f, s.shiftX * .01f);
    result.warp.y = pw::BuildAxis(nativeH, rawH, modelH, s.centerY * .01f, s.offsetY * .01f, s.shiftY * .01f);
    const auto& x = result.warp.x;
    const auto& y = result.warp.y;
    const auto invertible = [](const pw::Axis& axis)
    {
        // A peripheral side needs at least one raw Work texel. At a shift endpoint
        // the reference split may assign zero; snapping can leave a fraction of one.
        return (axis.periphery[0] <= 0 || axis.allotted[0] >= 1.0f) &&
               (axis.periphery[1] <= 0 || axis.allotted[1] >= 1.0f);
    };
    if (!invertible(x) || !invertible(y))
    {
        result.reason = "work shift leaves less than one work pixel on one side";
        return result;
    }
    result.centerBounds = { (x.bandCenter - x.halfBand) / nativeW, (y.bandCenter - y.halfBand) / nativeH,
                            (x.bandCenter + x.halfBand) / nativeW, (y.bandCenter + y.halfBand) / nativeH };
    result.workBounds = { (x.bandCenter - x.halfBand - x.allotted[0]) / nativeW,
                          (y.bandCenter - y.halfBand - y.allotted[0]) / nativeH,
                          (x.bandCenter + x.halfBand + x.allotted[1]) / nativeW,
                          (y.bandCenter + y.halfBand + y.allotted[1]) / nativeH };
    result.modelW = modelW;
    result.modelH = modelH;
    result.pixelShare = static_cast<float>(static_cast<double>(modelW) * modelH /
                                           (static_cast<double>(OrdinaryWorkSize(nativeW, globalScale)) *
                                            OrdinaryWorkSize(nativeH, globalScaleY)));
    result.active = true;
    result.reason = "active";
    return result;
}

// One scale for both axes (the common case); BuildAxes takes one per axis.
inline Layout Build(const Settings& s, uint32_t nativeW, uint32_t nativeH, float globalScale, uint32_t outputW = 0,
                    uint32_t outputH = 0)
{
    return BuildAxes(s, nativeW, nativeH, globalScale, globalScale, outputW, outputH);
}

struct alignas(16) Constants
{
    uint32_t mode = 0;
    float unused = 0;
    uint32_t width = 0, height = 0;
    pw::ShaderConstants warp {};
    float depthRect[4] = {}, motionRect[4] = {}, motionScale[2] = {}, padding[6] = {};
};
static_assert(sizeof(Constants) == 256);
static_assert(offsetof(Constants, warp) == 16);

// The shader constants of the active profile.
inline pw::ShaderConstants MakeShaderConstants(const Layout& layout)
{
    return layout.profile == Profile::Foveated ? Foveated::MakeShaderConstants(layout.fovea)
                                               : pw::MakeShaderConstants(layout.warp);
}

// What the fused native resolve (the main NR shader warping directly) is given: the selector word and the 28 floats of the
// seven float4 fields that follow the model size in its constant buffer. The profile rides in a high bit of the selector.
struct ResolveWarp
{
    uint32_t residual = 0;
    float workSize[2] = {};
    float fields[28] = {};
};

inline ResolveWarp MakeResolveWarp(const Layout& layout, bool fused)
{
    ResolveWarp r;
    r.residual = fused ? 2u : 1u;
    if (!fused)
        return r;
    if (layout.profile == Profile::Foveated)
        r.residual |= Foveated::kResolveFoveatedBit;
    const auto warp = MakeShaderConstants(layout);
    r.workSize[0] = warp.workWidth;
    r.workSize[1] = warp.workHeight;
    const float* fields[] = { &warp.bandCenterX, &warp.halfSpanNegX, &warp.halfSpanPosX, &warp.sideWorkNegX,
                              &warp.sideWorkPosX, &warp.sideEdgeSlopeNegX, &warp.workScaleX };
    for (unsigned i = 0; i < 7; ++i)
        std::memcpy(r.fields + i * 4, fields[i], 4 * sizeof(float));
    return r;
}

inline Constants MakeConstants(const Layout& layout, uint32_t mode, const GuideRegions& regions, float mvX, float mvY,
                               uint32_t nativeW, uint32_t nativeH)
{
    Constants c {};
    c.mode = mode;
    c.width = mode == 102 ? layout.ordinaryW : layout.modelW;
    c.height = mode == 102 ? layout.ordinaryH : layout.modelH;
    c.warp = MakeShaderConstants(layout);
    c.depthRect[0] = static_cast<float>(regions.depth.x);
    c.depthRect[1] = static_cast<float>(regions.depth.y);
    c.depthRect[2] = static_cast<float>(regions.depth.width);
    c.depthRect[3] = static_cast<float>(regions.depth.height);
    c.motionRect[0] = static_cast<float>(regions.motion.x);
    c.motionRect[1] = static_cast<float>(regions.motion.y);
    c.motionRect[2] = static_cast<float>(regions.motion.width);
    c.motionRect[3] = static_cast<float>(regions.motion.height);
    (void) nativeW;
    (void) nativeH;
    // The caller supplies source-motion units converted to native pixel deltas.
    c.motionScale[0] = mvX;
    c.motionScale[1] = mvY;
    return c;
}

// Everything the pass derives from one set of requests: the working scale, the layout, the sizes the model and the unpack work at.
// The pass makes one for what is asked for and keeps one for what is running; every consumer reads the one it was told to.
struct Geometry
{
    float workScale = 1.0f; // the larger of the two axis scales: above 1.0 means the model supersamples on some axis
    float workScaleX = 1.0f, workScaleY = 1.0f;
    Layout layout {}; // ordinaryW/H hold the aligned ordinary working size, which is what the unpack pass is told
    uint32_t frameW = 0, frameH = 0, ordinaryW = 0, ordinaryH = 0, workW = 0, workH = 0;
    bool spatial = false; // the spatial warp is in use
    bool Anamorphic() const { return workScaleX != workScaleY; }
};

// Spatial packing is supported through 100%; larger model scales use ordinary NR, and so does a spatial fallback.
inline Geometry MakeGeometry(const Settings& s, uint32_t frameW, uint32_t frameH, uint32_t outputW, uint32_t outputH,
                             float workScaleX, float workScaleY, bool spatialFailed)
{
    Geometry g;
    g.workScaleX = SanitizeWorkScale(workScaleX);
    g.workScaleY = SanitizeWorkScale(workScaleY);
    g.workScale = std::max(g.workScaleX, g.workScaleY);
    g.frameW = frameW;
    g.frameH = frameH;
    g.layout = BuildAxes(s, frameW, frameH, g.workScaleX, g.workScaleY, outputW, outputH);
    g.spatial = g.layout.active && g.workScale <= 1.0f && !spatialFailed;
    g.ordinaryW = OrdinaryWorkSize(frameW, g.workScaleX);
    g.ordinaryH = OrdinaryWorkSize(frameH, g.workScaleY);
    g.workW = g.spatial ? g.layout.modelW : g.ordinaryW;
    g.workH = g.spatial ? g.layout.modelH : g.ordinaryH;
    g.layout.ordinaryW = g.ordinaryW;
    g.layout.ordinaryH = g.ordinaryH;
    return g;
}

// One scale for both axes: what the geometry was before the per-axis keys.
inline Geometry MakeGeometry(const Settings& s, uint32_t frameW, uint32_t frameH, uint32_t outputW, uint32_t outputH,
                             float workScale, bool spatialFailed)
{
    return MakeGeometry(s, frameW, frameH, outputW, outputH, workScale, workScale, spatialFailed);
}

// The model answer is resolved straight from the packed grid (no unpack rasters) when the frame is the ordinary size.
inline bool FusedNative(const Geometry& g, bool compareSideBySide, bool formatIsFp16)
{
    return g.spatial && g.ordinaryW == g.frameW && g.ordinaryH == g.frameH && !compareSideBySide && formatIsFp16;
}

inline std::string StatusText(const Geometry& g, bool spatialFailed, bool fused)
{
    if (fused)
        return "active (fused native resolve)";
    if (!g.layout.requested)
        return "disabled";
    if (spatialFailed)
        return "fallback: spatial processing failed";
    if (g.workScale > 1.0f)
        return "inactive: spatial test supports model resolution up to 100%";
    return g.layout.reason;
}

inline Settle::Identity IdentityOf(const Geometry& g)
{
    Settle::Identity id;
    id.frameW = g.frameW;
    id.frameH = g.frameH;
    id.workW = g.workW;
    id.workH = g.workH;
    id.layoutKey = g.layout.Key();
    id.ordW = g.ordinaryW;
    id.ordH = g.ordinaryH;
    return id;
}

} // namespace DlssNr::Spatial
