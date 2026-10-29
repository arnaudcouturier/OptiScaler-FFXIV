#pragma once

// Depth-aware enlargement of the NR model's answer (a joint bilateral upsample). When the model works below the frame's size, its
// residual (or its picture) is enlarged onto the frame by a bilinear filter, which at a depth silhouette blends in the answer from
// the other side of the edge. Here each of the four taps of that filter is also weighted by how close the depth under the tap is to the
// depth under the output pixel, both read at native resolution from the game's depth by point loads:
//
//   w_tap = bilinear_tap * exp(-|d_pixel - d_tap| / (sigma * max(d_pixel, epsilon)))     normalised over the four taps
//
// d is the disparity of the depth sample (large = near in both depth conventions: the depth itself when it runs reversed, one
// minus it otherwise), so the same sigma means the same relative distance whatever the projection. When a depth is not usable or
// every weight vanishes, the plain bilinear weights are used.
//
// The shader (dlssnr.hlsl, DepthAwareTaps) mirrors these functions; keep the two in step.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace DlssNr::DepthAware
{

constexpr float kSigma = 0.1f;     // a tap whose disparity differs by 10% of the pixel's counts for 1/e
constexpr float kEpsilon = 1e-3f;  // the smallest disparity the relative difference is divided by (far plane and sky)
constexpr float kMinWeightSum = 1e-4f;
constexpr uint32_t kResolveBit = 0x200u; // bit of the main shader's gSpatialResidual (bit 8 is the foveated profile, the low byte the spatial mode)
constexpr unsigned kFieldCount = 28;     // the float slots the resolve shares with the spatial warp (unused while spatial is off)

inline bool ValidDepth(float depth) { return std::isfinite(depth) && depth >= 0.0f && depth <= 1.0f; }

inline float Disparity(float depth, bool inverted) { return inverted ? depth : 1.0f - depth; }

// How alike the depth under the pixel and the depth under one tap are, 1 for equal, towards 0 across an edge.
inline float Similarity(float pixelDepth, float tapDepth, bool inverted, float sigma = kSigma, float epsilon = kEpsilon)
{
    const float pixel = Disparity(pixelDepth, inverted);
    const float tap = Disparity(tapDepth, inverted);
    return std::exp(-std::abs(pixel - tap) / (sigma * std::max(pixel, epsilon)));
}

// The four taps of a bilinear read at (u, v) of a w x h texture, in the order (0,0) (1,0) (0,1) (1,1), with the bilinear weights
// the sampler would use. Taps outside the texture clamp to its edge.
struct Taps
{
    std::array<int, 4> x {}, y {};
    std::array<float, 4> bilinear {};
};

inline Taps BilinearTaps(float u, float v, uint32_t w, uint32_t h)
{
    const float px = u * static_cast<float>(w) - 0.5f, py = v * static_cast<float>(h) - 0.5f;
    const float bx = std::floor(px), by = std::floor(py);
    const float fx = px - bx, fy = py - by;
    const int x0 = std::clamp(static_cast<int>(bx), 0, static_cast<int>(w) - 1);
    const int x1 = std::clamp(static_cast<int>(bx) + 1, 0, static_cast<int>(w) - 1);
    const int y0 = std::clamp(static_cast<int>(by), 0, static_cast<int>(h) - 1);
    const int y1 = std::clamp(static_cast<int>(by) + 1, 0, static_cast<int>(h) - 1);
    Taps t;
    t.x = { x0, x1, x0, x1 };
    t.y = { y0, y0, y1, y1 };
    t.bilinear = { (1.0f - fx) * (1.0f - fy), fx * (1.0f - fy), (1.0f - fx) * fy, fx * fy };
    return t;
}

// The position of the centre of texel (ix, iy) of a w x h texture, as a fraction of its size.
inline float TapCentre(int index, uint32_t extent) { return (static_cast<float>(index) + 0.5f) / static_cast<float>(extent); }

// The texel of the game's depth for a position (u, v) of the frame: the depth's valid rectangle (x, y, width, height) is spread over the
// whole frame and read by a point load, whatever size the depth texture has.
inline void DepthPixel(float u, float v, const float rect[4], int& px, int& py)
{
    const int loX = static_cast<int>(rect[0]), loY = static_cast<int>(rect[1]);
    const int hiX = static_cast<int>(rect[0] + rect[2]) - 1, hiY = static_cast<int>(rect[1] + rect[3]) - 1;
    px = std::clamp(static_cast<int>(rect[0] + u * rect[2]), loX, hiX);
    py = std::clamp(static_cast<int>(rect[1] + v * rect[3]), loY, hiY);
}

// The weights of the four taps. `bilinear` are the sampler's weights; plain bilinear comes back when the pixel's or any tap's depth is
// not usable, or when no tap is anywhere near the pixel's depth.
inline std::array<float, 4> Weights(const std::array<float, 4>& bilinear, const std::array<float, 4>& tapDepth, float pixelDepth,
                                    bool inverted, float sigma = kSigma, float epsilon = kEpsilon)
{
    if (!ValidDepth(pixelDepth))
        return bilinear;
    for (float depth : tapDepth)
        if (!ValidDepth(depth))
            return bilinear;
    std::array<float, 4> w {};
    float sum = 0.0f;
    for (size_t i = 0; i < 4; ++i)
    {
        w[i] = bilinear[i] * Similarity(pixelDepth, tapDepth[i], inverted, sigma, epsilon);
        sum += w[i];
    }
    if (!(sum > kMinWeightSum))
        return bilinear;
    for (float& value : w)
        value /= sum;
    return w;
}

// The parameters as the resolve shader reads them: the first eight of the 28 float slots it shares with the spatial warp. The weight sum
// under which the shader falls back to plain bilinear travels with them (slot 7), so the host and the shader share the one constant.
inline void PackFields(float (&fields)[kFieldCount], const float rect[4], bool inverted, float sigma = kSigma, float epsilon = kEpsilon,
                       float minWeightSum = kMinWeightSum)
{
    for (float& f : fields)
        f = 0.0f;
    for (int i = 0; i < 4; ++i)
        fields[i] = rect[i];
    fields[4] = sigma;
    fields[5] = epsilon;
    fields[6] = inverted ? 1.0f : 0.0f;
    fields[7] = minWeightSum;
}

// The selector word a dispatch of the resolve shader is given. The depth-aware bit only travels with a depth: a dispatch without one binds
// the source in the sixth slot as a stand-in, and the shader would read that picture's colour as depth (a colour in 0..1 passes the
// validity test, so there is no fallback to the plain filter). Everything else in the word is left alone.
inline uint32_t ResolveSelector(uint32_t selector, bool haveDepth)
{
    return haveDepth ? selector : (selector & ~kResolveBit);
}

// Whether the model's answer is enlarged onto the frame at all: reduced on some axis and not enlarged on any (an enlarged model is
// brought back down by the supersample filter instead).
inline bool Enlarging(uint32_t workW, uint32_t workH, uint32_t frameW, uint32_t frameH, bool supersampled)
{
    return !supersampled && (workW < frameW || workH < frameH);
}

enum class State
{
    Off,                // the key is off
    Active,             // weighted by depth
    InactiveSpatial,    // the spatial warp is in use: the model is sampled in warped space, so the plain filter stays
    InactiveNotReduced, // nothing is enlarged (model resolution exactly 100% on both axes)
    InactiveAboveFull,  // the model works above 100% on an axis: it is brought back down by the supersample filter, not enlarged
    InactiveNoDepth,    // the game's depth is not available
};

inline State Decide(bool keyOn, bool spatialActive, bool enlarging, bool depthAvailable, bool supersampled = false)
{
    if (!keyOn)
        return State::Off;
    if (spatialActive)
        return State::InactiveSpatial;
    if (!enlarging)
        return supersampled ? State::InactiveAboveFull : State::InactiveNotReduced;
    if (!depthAvailable)
        return State::InactiveNoDepth;
    return State::Active;
}

inline const char* StatusText(State state)
{
    switch (state)
    {
    case State::Active: return "depth-aware resolve: active";
    case State::InactiveSpatial: return "depth-aware resolve: inactive with spatial compression";
    case State::InactiveNotReduced: return "depth-aware resolve: inactive at model resolution 100%";
    case State::InactiveAboveFull: return "depth-aware resolve: inactive: model resolution above 100% on an axis";
    case State::InactiveNoDepth: return "depth-aware resolve: inactive (no usable depth)";
    case State::Off: break;
    }
    return "";
}

} // namespace DlssNr::DepthAware
