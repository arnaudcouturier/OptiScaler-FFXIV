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

}
