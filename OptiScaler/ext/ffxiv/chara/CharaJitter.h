#pragma once
// Sub-pixel jitter of a preview render: a Halton (2, 3) sequence over 32 phases, applied as a shift of the viewport's
// top-left corner. The same offset goes to the upscaler and to the alpha re-composition.
//
// Why 32: the upscaler guides ask for a jitter cycle of at least 8 * (output width / render width)^2 phases (RecommendedPhases; the
// FSR reference code uses the same rule). The preview's DLSS output is always twice its render size, so the rule gives 8 * 4 = 32. A
// test ties kJitterPhases to the preview scale.
//
// Phase n = (pass % kJitterPhases) + 1, so n runs 1..32. jx = RadicalInverse2(n) - 0.5, jy = RadicalInverse3(n) - 0.5 (pixels).
// `pass` is the render count of the preview the render is for (SlotTable::JitterPhase), not a counter shared by all previews: two
// previews that render in a fixed order would otherwise get every second phase each, and in base 2 every second phase lies in the same
// half of the pixel in x.
// RadicalInverse_b(n) = sum over the digits d_k of n in base b of d_k * b^-(k+1).

#include <cstdint>

namespace ext::ffxiv::chara
{
inline constexpr uint32_t kJitterPhases = 32;

// The recommended length of a jitter cycle: 8 * (outputWidth / renderWidth)^2 phases, rounded down (18 at 3:2, 32 at 2:1, 72 at 3:1).
// Integer maths: no float rounding on exact boundaries. 0 when either width is 0 or above 2^20 (not meaningful; keeps the 64-bit
// intermediate from overflowing); the result saturates at the largest 32-bit value.
constexpr uint32_t RecommendedPhases(uint32_t renderWidth, uint32_t outputWidth)
{
    constexpr uint32_t kMaxWidth = 1u << 20;
    if (renderWidth == 0 || outputWidth == 0 || renderWidth > kMaxWidth || outputWidth > kMaxWidth)
        return 0;
    const uint64_t phases = 8ull * outputWidth * outputWidth / (static_cast<uint64_t>(renderWidth) * renderWidth);
    return phases > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(phases);
}

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

// The jitter of pass number `pass` (any counter; it selects phase pass % kJitterPhases).
constexpr Jitter PassJitter(uint32_t pass)
{
    const uint32_t n = (pass % kJitterPhases) + 1;
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
