#pragma once
// Sub-pixel jitter of a preview render: a Halton (2, 3) sequence over 16 phases, applied as a shift of the viewport's
// top-left corner. The same offset goes to the upscaler and to the alpha re-composition.
//
// Phase n = (pass & 15) + 1, so n runs 1..16. jx = RadicalInverse2(n) - 0.5, jy = RadicalInverse3(n) - 0.5 (pixels).
// RadicalInverse_b(n) = sum over the digits d_k of n in base b of d_k * b^-(k+1).

#include <cstdint>

namespace ext::ffxiv::chara
{
inline constexpr uint32_t kJitterPhases = 16;

constexpr double RadicalInverse(uint32_t n, uint32_t base)
{
    double result = 0.0;
    double scale = 1.0 / base;
    while (n > 0)
    {
        result += static_cast<double>(n % base) * scale;
        n /= base;
        scale /= base;
    }
    return result;
}

struct Jitter
{
    float x = 0.0f; // pixels
    float y = 0.0f;
};

// The jitter of pass number `pass` (any counter; only its low four bits select the phase).
constexpr Jitter PassJitter(uint32_t pass)
{
    const uint32_t n = (pass & (kJitterPhases - 1)) + 1;
    return { static_cast<float>(RadicalInverse(n, 2) - 0.5), static_cast<float>(RadicalInverse(n, 3) - 0.5) };
}

// Viewport top-left corner after the jitter (width and height are unchanged).
struct ViewportShift
{
    float topLeftX;
    float topLeftY;
};
constexpr ViewportShift ShiftViewport(float topLeftX, float topLeftY, Jitter j)
{
    return { topLeftX + j.x, topLeftY + j.y };
}

// The content of the jittered image is displaced by +jitter: the native alpha is read at uv + jitter / size.
struct UvOffset
{
    float u;
    float v;
};
constexpr UvOffset AlphaUvOffset(Jitter j, float width, float height)
{
    return { width > 0.0f ? j.x / width : 0.0f, height > 0.0f ? j.y / height : 0.0f };
}
} // namespace ext::ffxiv::chara
