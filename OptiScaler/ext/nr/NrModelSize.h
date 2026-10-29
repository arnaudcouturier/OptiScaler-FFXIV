#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace DlssNr {
// Only a size we are already resampling to is rounded. A native-size pass (WorkingScale 1.0, or a scale
// that rounds back to native) is left alone: rounding 1080 to 1088 would turn a 1:1 pass into a
// resample of the frame, which is worse than a ragged border window.
inline unsigned int AlignWorkSize(unsigned int size, unsigned int native)
{
    constexpr unsigned int kGrid = 16;

    if (size == native)
        return size;

    unsigned int aligned = (size + kGrid / 2) / kGrid * kGrid;

    if (aligned < kGrid)
        aligned = kGrid;

    // Shrinking never rounds up past the native size (that would enlarge what was meant to be reduced).
    if (size < native && aligned > native)
        aligned = native;

    return aligned;
}

// The size the model works at without any spatial warp: the frame times the working scale, on the 16 grid.
inline unsigned int OrdinaryWorkSize(unsigned int native, float scale)
{
    return AlignWorkSize(static_cast<unsigned int>(native * scale + .5f), native);
}

// native * fraction rounded up to an even count (the model wants even extents). A result that is within rounding noise of
// a whole number stays on it; a reduction never exceeds the even part of the native size; never below 2.
inline uint32_t EvenExtent(uint32_t native, double fraction)
{
    if (fraction == 1.0)
        return native;
    double value = static_cast<double>(native) * fraction;
    const double nearest = std::round(value);
    if (std::abs(value - nearest) <= 1e-6 * std::max(1.0, std::abs(value)))
        value = nearest;
    uint32_t result = static_cast<uint32_t>(std::ceil(value));
    if (result & 1u)
        ++result;
    if (fraction < 1.0)
        result = std::min(result, native & ~1u);
    return std::max(2u, result);
}

// The model-resolution scale, kept in the range the menu offers (25% to 200%); anything not finite is 100%.
inline float SanitizeWorkScale(float scale)
{
    if (!std::isfinite(scale))
        scale = 1.0f;
    return scale < 0.25f ? 0.25f : (scale > 2.0f ? 2.0f : scale);
}

// The scale one axis of the model works at: the per-axis override when it is a positive number, otherwise the uniform
// WorkingScale (0, a negative or a non-finite override follows it). Both come out in the 25%..200% range.
inline float EffectiveAxisScale(float axisOverride, float uniformScale)
{
    if (std::isfinite(axisOverride) && axisOverride > 0.0f)
        return SanitizeWorkScale(axisOverride);
    return SanitizeWorkScale(uniformScale);
}

// The scale of each axis of the model.
struct AxisScales
{
    float x = 1.0f, y = 1.0f;
    bool operator==(const AxisScales& o) const { return x == o.x && y == o.y; }
};

// A model size that is above 100% on one axis and below on the other is not supported: the supersample path assumes both axes are
// magnified (or at the frame's size) and the resampler it uses is built for one direction. When either axis is above 100% the other
// is raised to at least 100%. Exactly 100% is not "above": a model that is native on one axis and reduced on the other is the
// ordinary anamorphic case and stays as it is.
inline AxisScales RaiseMixedAxes(AxisScales scales)
{
    if (scales.x > 1.0f || scales.y > 1.0f)
    {
        scales.x = std::max(scales.x, 1.0f);
        scales.y = std::max(scales.y, 1.0f);
    }
    return scales;
}

// The model runtime may refuse a model size that differs per axis (no size like that was ever tried on it). When it does, the pass
// remembers it here, with the three key values that asked for the size, and works at the uniform WorkingScale instead. The latch
// goes away as soon as any of the three keys changes.
struct AnamorphicLatch
{
    bool failed = false;
    float keyX = 0.0f, keyY = 0.0f, keyUniform = 0.0f; // WorkingScaleX, WorkingScaleY, WorkingScale when it failed
};

// Equal, and two NaNs count as equal: a NaN key must not release the latch on every frame (that would retry the refused size forever).
inline bool SameKeyValue(float a, float b) { return a == b || (std::isnan(a) && std::isnan(b)); }

inline AnamorphicLatch LatchAnamorphic(float keyX, float keyY, float keyUniform) { return { true, keyX, keyY, keyUniform }; }

inline bool AnamorphicLatchHolds(const AnamorphicLatch& latch, float keyX, float keyY, float keyUniform)
{
    return latch.failed && SameKeyValue(latch.keyX, keyX) && SameKeyValue(latch.keyY, keyY) && SameKeyValue(latch.keyUniform, keyUniform);
}

// The latch as it stands for these keys: unchanged while they are what they were, cleared once any of them moved.
inline AnamorphicLatch RefreshAnamorphicLatch(const AnamorphicLatch& latch, float keyX, float keyY, float keyUniform)
{
    return AnamorphicLatchHolds(latch, keyX, keyY, keyUniform) ? latch : AnamorphicLatch {};
}

// The scales the model works at: the per-axis keys (0 = follow the uniform one), or the uniform WorkingScale on both axes while the
// latch holds, then the mixed above/below 100% rule. The pass, the guides, the motion scale and the menu all read these numbers.
inline AxisScales EffectiveScales(const AnamorphicLatch& latch, float keyX, float keyY, float keyUniform)
{
    if (AnamorphicLatchHolds(latch, keyX, keyY, keyUniform))
    {
        const float uniform = SanitizeWorkScale(keyUniform);
        return RaiseMixedAxes({ uniform, uniform });
    }
    return RaiseMixedAxes({ EffectiveAxisScale(keyX, keyUniform), EffectiveAxisScale(keyY, keyUniform) });
}

// The menu's per-axis slider is a whole percentage and 0 means "same as Model resolution". These two convert between that and the
// stored override (0, or 25%..200% as a fraction): a percentage between 1 and 24 becomes 25%, anything above 200 becomes 200%.
inline float AxisScaleFromPercent(int percent)
{
    if (percent <= 0)
        return 0.0f;
    return static_cast<float>(percent < 25 ? 25 : (percent > 200 ? 200 : percent)) / 100.0f;
}

inline int AxisPercentOfScale(float axisOverride)
{
    if (!std::isfinite(axisOverride) || axisOverride <= 0.0f)
        return 0;
    const int percent = static_cast<int>(std::lround(axisOverride * 100.0f));
    return percent < 25 ? 25 : (percent > 200 ? 200 : percent);
}

// What the slider shows for a typed or dragged percentage: the value that will be applied (0 stays 0, 1..24 become 25, above 200
// becomes 200), so the number on screen is the number in use.
inline int AxisPercentApplied(int percent) { return AxisPercentOfScale(AxisScaleFromPercent(percent)); }

// Whether the depth and motion guides are resampled to the model's working size. The rule that has always applied is "both axes of
// the model are below the frame". With different scales per axis (an anamorphic model) one axis may stay at the frame size while
// the other is reduced; then the guides are still matched, unless an axis is enlarged (that mixed case keeps the full-size guides
// like any enlarged model).
inline bool MatchGuidesToWork(bool spatial, uint32_t workW, uint32_t workH, uint32_t frameW, uint32_t frameH, bool anamorphic)
{
    if (spatial)
        return false;
    if (!anamorphic)
        return workW < frameW && workH < frameH;
    return (workW < frameW || workH < frameH) && workW <= frameW && workH <= frameH;
}

}
