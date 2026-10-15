#include "pch.h"

#include "ext/Seams.h"

#include <algorithm>
#include <cmath>

namespace ext::present
{
// FrameLimit::sleep. With NVIDIA frame generation the cap counts the generated frames too, so the interval between two
// rendered frames doubles (as in upstream). Negative, NaN and infinite ini values used to reach an unsigned cast
// (undefined behaviour); they now mean "no cap". 1 ns .. 100 s per frame.
std::optional<uint64_t> FrameInterval(float fpsCap, bool fgActive)
{
    if (!std::isfinite(fpsCap) || fpsCap <= 0)
        return std::nullopt;

    const double ns = std::clamp(1'000'000'000.0 / double(fpsCap), 1.0, 100'000'000'000.0);
    return uint64_t(ns) * (fgActive ? 2 : 1);
}
} // namespace ext::present
