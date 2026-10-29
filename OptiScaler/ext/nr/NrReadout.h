#pragma once

// Text of the live readouts in the NR card.

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

namespace DlssNr
{
// "NR time: 4.21 ms" from the GPU time of the last NR evaluation, in milliseconds. Not measured yet (nothing, not a finite number,
// or zero or less) reads "n/a".
inline std::string NrTimeText(std::optional<double> milliseconds)
{
    if (!milliseconds.has_value() || !std::isfinite(*milliseconds) || *milliseconds <= 0.0)
        return "NR time: n/a";
    char text[48] = {};
    std::snprintf(text, sizeof text, "NR time: %.2f ms", *milliseconds);
    return text;
}
} // namespace DlssNr
