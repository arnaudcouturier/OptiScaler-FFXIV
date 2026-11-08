// God rays, the beams that follow the engine's own shadows (ext/ffxiv/engine/GodraysPass.cpp and its shaders) on the D3D11 WARP software adapter, on
// synthetic buffers this test builds itself.
//
// The frame is 128x96, the camera at its origin looking down -z with the capture's own projection shape (reversed Z, infinite far plane: a sky pixel's
// depth is 0 and its inverse projection has w = 0). The shadow light points up and forward. One real cascade maps view space into the synthetic atlas'
// first slice (a second one into the second slice in the cascade fixtures); degenerate cascades map everything outside the atlas, so their slice is
// unreadable and they never shadow. The atlas carries occluders in light space: a wall (a block of texels close to the light), a strip, or a flat plane.
// The reference march in this file computes the expected term per march pixel from the same formula the shader carries (the engine's own
// disassembled compare, the writer's tail, the Henyey-Greenstein phase, the in-scatter integral with extinction, the 0.002 air bias), and the reference
// composite computes the expected picture (bilateral upsample, the ShaftAdded bound, the sky ceiling) from the term the pass wrote.
//
// What a wrong projection, pick, compare, phase, integral or composite would break, and what is checked:
//   the term matches the reference at every march pixel (three softness settings, jitter on and off, the capture-shaped projection with w = 0 on sky),
//   each rule is SEEN by the fixture: the reference with the rule broken (first cascade wins, no slice test, no bias, no extinction, the cos^16 lobe)
//     differs from the correct one on the fixture that exercises it, so a shader that broke the rule would not match the reference,
//   the beams are shadowed behind occluders and lit beside them: with four alternately occluded 90-degree sectors around the light's axis the ring
//     structure around the light is >= 0.3 and >= 0.3 above the same pass with every texel lit (one strip, one shadow slab, shows a little),
//   the composite adds through the radial's ShaftAdded bound with the bilateral (depth-aware) upsample: no pixel past the white, an edge between a
//     near wall and the sky keeps its side's term,
//   the sky gets light up to its own 99th percentile and not past it (and 5 % of it crosses without the ceiling), no sky samples leave it alone,
//   the stats pass reads back the ceilings, the term peak and the sample count; the compute state and the graphics state come back after a run,
//   the fog read without the height profile still returns the sun row; the stolen views are own views on the game's resource.
#include "check.h"

#include "ext/ffxiv/engine/GodraysPass.h"
#include "ext/ffxiv/engine/GodraysSun.h"

#include <Godrays_Stats.h> // the stats compute shader's blob: the compute-state test binds it as "the game's" shader

#include <d3d11.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::godrays;
// The radial's own rules are qualified explicitly (ext::ffxiv::shafts::) to avoid the same-named ambiguity with the march's types.

#define CHECK_SUCCEEDED(call) CHECK(SUCCEEDED(call))

namespace
{
constexpr uint32_t kWidth = 128;
constexpr uint32_t kHeight = 96;
constexpr uint32_t kAtlasWidth = 256;
constexpr uint32_t kAtlasHeight = 1024; // four slices of 256 rows
constexpr uint32_t kSkyWidth = 64;
constexpr uint32_t kSkyHeight = 48;
constexpr float kWall = 0.50f;   // the scene's surface colour, exactly representable as a half
constexpr float kWhite = 0.838f; // the captured frame's measured white point, the fixture's bound
constexpr float kWallDistance = 50.0f;

// ---- half floats ----------------------------------------------------------------------------------------------------------------

uint16_t HalfOf(float value)
{
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exponent = int32_t((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0)
        return uint16_t(sign);
    if (exponent >= 31)
        return uint16_t(sign | 0x7C00u);
    return uint16_t(sign | (uint32_t(exponent) << 10) | (mantissa >> 13));
}

float FromHalf(uint16_t value)
{
    const float sign = (value & 0x8000u) != 0 ? -1.0f : 1.0f;
    const uint32_t exponent = (value >> 10) & 0x1Fu;
    const uint32_t mantissa = value & 0x3FFu;
    if (exponent == 0)
        return sign * std::ldexp(float(mantissa), -24);
    if (exponent == 31)
        return mantissa == 0 ? sign * INFINITY : NAN;
    return sign * std::ldexp(float(1024 + mantissa), int(exponent) - 25);
}

// The sky of the scene: a gradient down the frame (so its 99th percentile is not a single value) and a slight hue, exactly representable per channel.
float SkyChannel(uint32_t y, uint32_t channel)
{
    const float base = 0.20f + 0.15f * float(y) / float(kHeight - 1);
    return FromHalf(HalfOf(base * (channel == 0 ? 1.0f : channel == 1 ? 0.95f : 0.90f)));
}

// ---- the camera and the light ----------------------------------------------------------------------------------------------------
//
// The capture's own projection shape: reversed Z with the near plane at 0.1 and no far plane. The inverse maps (ndc.x, ndc.y, depth, 1) through the
// rows (ax, 0, 0, 0), (0, ay, 0, 0), (0, 0, 0, -1), (0, 0, 10, 0): view = (ax x, ay y, -1) / (10 depth), so a sky pixel (depth 0) has w = 0 and
// only a direction.
constexpr float kNear = 0.1f;
constexpr float kTanX = 1.0f;  // 90 degrees across
constexpr float kTanY = 0.75f; // 4:3

void FillInverseProjection(float out[16])
{
    out[0] = kTanX; out[1] = 0.0f; out[2] = 0.0f; out[3] = 0.0f;
    out[4] = 0.0f; out[5] = kTanY; out[6] = 0.0f; out[7] = 0.0f;
    out[8] = 0.0f; out[9] = 0.0f; out[10] = 0.0f; out[11] = -1.0f;
    out[12] = 0.0f; out[13] = 0.0f; out[14] = 1.0f / kNear; out[15] = 0.0f;
}

void FillProjection(float out[16])
{
    out[0] = 1.0f / kTanX; out[1] = 0.0f; out[2] = 0.0f; out[3] = 0.0f;
    out[4] = 0.0f; out[5] = 1.0f / kTanY; out[6] = 0.0f; out[7] = 0.0f;
    out[8] = 0.0f; out[9] = 0.0f; out[10] = 0.0f; out[11] = kNear;
    out[12] = 0.0f; out[13] = 0.0f; out[14] = -1.0f; out[15] = 0.0f;
}

float DepthAtDistance(float distance) { return kNear / distance; }

// The direction of the view ray through an ndc point (depth does not matter for a perspective projection) and, with a depth, the view point.
void RayDirection(const float ip[16], float ndcX, float ndcY, float out[3])
{
    out[0] = ip[0] * ndcX + ip[1] * ndcY + ip[3];
    out[1] = ip[4] * ndcX + ip[5] * ndcY + ip[7];
    out[2] = ip[8] * ndcX + ip[9] * ndcY + ip[11];
    const float length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    for (int i = 0; i < 3; ++i)
        out[i] /= length;
}

// The view point of a depth texel: the inverse projection's four dot products and the divide - or, at w = 0, the ray's direction and no point.
bool ViewPositionOf(const float ip[16], float ndcX, float ndcY, float depth, float out[3])
{
    float v[4];
    for (int row = 0; row < 4; ++row)
        v[row] = ip[row * 4 + 0] * ndcX + ip[row * 4 + 1] * ndcY + ip[row * 4 + 2] * depth + ip[row * 4 + 3];
    if (!(std::fabs(v[3]) > 1.0e-20f))
    {
        const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int i = 0; i < 3; ++i)
            out[i] = v[i] / length;
        return false; // a direction only
    }
    for (int i = 0; i < 3; ++i)
        out[i] = v[i] / v[3];
    return true;
}

// The engine's own per-pixel hash, copied from LightShafts.hlsl 93-103 (the march carries the same function with the origin named).
uint32_t HashBits(int x, int y)
{
    uint32_t h = uint32_t(x) * 1664525u + uint32_t(y) * 1013904223u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return h & 0xFFFFFFu;
}

float Hash01(int x, int y) { return float(HashBits(x, y)) * (1.0f / 16777216.0f); }

// ---- the reference march -----------------------------------------------------------------------------------------------------------
struct ReferenceAtlas
{
    uint32_t width = kAtlasWidth, height = kAtlasHeight;
    std::vector<uint16_t> texels; // R16_UNORM, one plane

    // The same texel the shader's Load takes: clamped into [0,1], scaled, and rounded down to the nearest texel centre.
    float At(float u, float v) const
    {
        const float cu = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
        const float cv = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        int x = int(cu * float(width) - 0.5f);
        int y = int(cv * float(height) - 0.5f);
        x = x < 0 ? 0 : (x >= int(width) ? int(width) - 1 : x);
        y = y < 0 ? 0 : (y >= int(height) ? int(height) - 1 : y);
        return float(texels[size_t(y) * width + x]) / 65535.0f;
    }
};

constexpr float kTapsNone[1][2] { { 0.0f, 0.0f } };
constexpr float kTapsTwo[2][2] { { 0.5f, 0.5f }, { -0.5f, -0.5f } };
constexpr float kTapsFour[4][2] { { 0.5f, 0.5f }, { -0.5f, 0.5f }, { 0.5f, -0.5f }, { -0.5f, -0.5f } };

constexpr float AtlasTexelX(uint32_t width, uint32_t height) { return float(width < height ? width : height) / float(width); }
constexpr float AtlasTexelY(uint32_t width, uint32_t height) { return float(width < height ? width : height) / float(height); }

// The rules a reference run can break, one at a time, so a fixture can prove it sees each of them.
struct RefOptions
{
    bool sliceTest = true;     // a cascade only owns a sample inside its own atlas slice
    bool minOverCascades = true; // false: the first cascade whose uv is inside [0,1]^2 owns the sample (the first march's pick)
    bool bias = true;          // the 0.002 air bias on the compare
    bool extinction = true;    // optical depth accrues
    bool henyey = true;        // false: the first march's pow(cos, 16) lobe
};

float ReferenceTerm(const Inputs& inputs, const ReferenceAtlas& atlas, int marchX, int marchY, const std::vector<float>& depthValues,
                    const RefOptions& options = RefOptions {})
{
    const FrameState& state = inputs.state;
    const Params& params = inputs.params;
    const float scale = params.resolution == Resolution::Half ? 2.0f : 4.0f;
    float windowX = (float(marchX) + 0.5f) * scale;
    float windowY = (float(marchY) + 0.5f) * scale;
    windowX = windowX < inputs.window.x ? inputs.window.x : windowX;
    windowY = windowY < inputs.window.y ? inputs.window.y : windowY;
    const float ndcX = windowX * state.ndcScale[0] + state.ndcOffset[0];
    const float ndcY = windowY * state.ndcScale[1] + state.ndcOffset[1];
    int depthX = int(windowX);
    int depthY = int(windowY);
    depthX = depthX < 0 ? 0 : (depthX >= int(inputs.depthWidth) ? int(inputs.depthWidth) - 1 : depthX);
    depthY = depthY < 0 ? 0 : (depthY >= int(inputs.depthHeight) ? int(inputs.depthHeight) - 1 : depthY);
    const float depth = depthValues[size_t(depthY) * inputs.depthWidth + depthX];
    float dir[3] {};
    const bool haveSurface = ViewPositionOf(state.inverseProjection, ndcX, ndcY, depth, dir);
    float length = 1.0e30f; // a sky ray has no surface: the cascades' reach bounds it
    if (haveSurface)
    {
        length = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (!(length > 1.0e-6f) || !(length == length))
            return 0.0f;
        for (float& value : dir)
            value /= length;
    }
    const float tFar = length < state.marchFar ? length : state.marchFar;
    if (!(tFar > 0.0f))
        return 0.0f;
    const float dt = tFar / float(params.steps);
    const float jitter = params.jitter ? Hash01(marchX, marchY) : 0.0f;
    float cosTheta = dir[0] * state.sunDirection[0] + dir[1] * state.sunDirection[1] + dir[2] * state.sunDirection[2];
    cosTheta = cosTheta < -1.0f ? -1.0f : (cosTheta > 1.0f ? 1.0f : cosTheta);
    const float phase = options.henyey ? HenyeyGreenstein(cosTheta, EffectiveAnisotropy(params.anisotropy, params.phaseBoost))
                                       : std::pow(cosTheta > 0.0f ? cosTheta : 0.0f, 16.0f);
    InscatterState integral;
    const float texel[2] { AtlasTexelX(atlas.width, atlas.height), AtlasTexelY(atlas.width, atlas.height) };
    for (uint32_t step = 0; step < params.steps; ++step)
    {
        const float t = (float(step) + jitter) * dt;
        const float p[3] { dir[0] * t, dir[1] * t, dir[2] * t };
        const float worldY = state.inverseViewRow1[0] * p[0] + state.inverseViewRow1[1] * p[1] + state.inverseViewRow1[2] * p[2] +
                             state.inverseViewRow1[3];
        const float rho = FogDensityAt(state.fog, worldY, params.heightLift) * params.fogBoost * state.invRhoCamera;
        float visibility = 1.0f;
        bool owned = false;
        for (uint32_t k = 0; k < state.cascadeCount; ++k)
        {
            const Cascade& cascade = state.cascades[k];
            const uint32_t slice = CascadeSlice(cascade.matrix, cascade.fade);
            if (options.sliceTest && slice == kNoSlice)
                continue;
            const float clipX = cascade.matrix[0] * p[0] + cascade.matrix[1] * p[1] + cascade.matrix[2] * p[2] + cascade.matrix[3];
            const float clipY = cascade.matrix[4] * p[0] + cascade.matrix[5] * p[1] + cascade.matrix[6] * p[2] + cascade.matrix[7];
            const float clipZ = cascade.matrix[8] * p[0] + cascade.matrix[9] * p[1] + cascade.matrix[10] * p[2] + cascade.matrix[11];
            const float clipW = cascade.matrix[12] * p[0] + cascade.matrix[13] * p[1] + cascade.matrix[14] * p[2] + cascade.matrix[15];
            if (!(clipW > 1.0e-6f) || !(clipW == clipW))
                continue;
            const float u = clipX / clipW;
            const float v = clipY / clipW;
            const float z = clipZ / clipW;
            if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f))
                continue;
            if (options.sliceTest && !SliceContains(v, slice))
                continue;
            if (!options.minOverCascades && owned)
                continue;
            owned = true;
            const uint32_t tapCount = params.softness == 0 ? 1u : (params.softness == 1 ? 2u : 4u);
            float shadowed = 0.0f;
            for (uint32_t tap = 0; tap < tapCount; ++tap)
            {
                const float* offset = params.softness == 0 ? kTapsNone[tap] : (params.softness == 1 ? kTapsTwo[tap] : kTapsFour[tap]);
                const float tapU = u + texel[0] * offset[0] * state.discScale;
                const float tapV = v + texel[1] * offset[1] * state.discScale;
                const float radius = std::sqrt(offset[0] * offset[0] + offset[1] * offset[1]);
                const float tapZ = z - radius * state.discScale - (options.bias ? kAirBias : 0.0f);
                if (ShadowedAt(tapZ, atlas.At(tapU, tapV)))
                    shadowed += 1.0f;
            }
            const float shadow = 1.0f - shadowed / float(tapCount);
            const float seen = CascadeVisibility(shadow, p[2], cascade.fade, cascade.distanceFar, cascade.rampScale, cascade.rampTarget);
            visibility = options.minOverCascades ? (seen < visibility ? seen : visibility) : seen;
        }
        InscatterStep(integral, MediumSigma(params.density, rho), phase, visibility, dt, options.extinction);
    }
    return TermClamped(integral.sum);
}

Params MarchParams(uint32_t softness = 0, bool jitter = true)
{
    return BuildParams(48, "quarter", 1.0f, 1.0f, 1.0f, "auto", int32_t(softness), jitter, 1.0f);
}

// ---- the reference composite and the reference stats --------------------------------------------------------------------------------------
struct SceneColour
{
    std::vector<float> rgb; // kWidth * kHeight * 3
};

float SceneAt(const SceneColour& scene, int x, int y, int channel) { return scene.rgb[(size_t(y) * kWidth + size_t(x)) * 3 + size_t(channel)]; }

// The pixel of the scene a march texel stands for (the shader's MarchPixelOf): the block's centre, clamped into the window.
void MarchPixelOf(const Inputs& inputs, float scale, int tx, int ty, int& px, int& py)
{
    float wx = (float(tx) + 0.5f) * scale;
    float wy = (float(ty) + 0.5f) * scale;
    wx = std::max(wx, inputs.window.x);
    wy = std::max(wy, inputs.window.y);
    wx = std::min(wx, inputs.window.x + inputs.window.width - 1.0f);
    wy = std::min(wy, inputs.window.y + inputs.window.height - 1.0f);
    px = std::min(int(wx), int(inputs.depthWidth) - 1);
    py = std::min(int(wy), int(inputs.depthHeight) - 1);
}

// The bilinear read of the quarter-res term at a full-resolution pixel, with the depth weights (the bilateral upsample) when asked.
float UpsampledTerm(const Inputs& inputs, const std::vector<float>& term, uint32_t termW, uint32_t termH, const std::vector<float>& depthValues,
                    int x, int y, bool bilateral)
{
    const float scale = inputs.params.resolution == Resolution::Half ? 2.0f : 4.0f;
    const float cx = (float(x) + 0.5f - inputs.window.x) / scale - 0.5f;
    const float cy = (float(y) + 0.5f - inputs.window.y) / scale - 0.5f;
    const int bx = int(std::floor(cx));
    const int by = int(std::floor(cy));
    const float fx = std::min(std::max(cx - float(bx), 0.0f), 1.0f);
    const float fy = std::min(std::max(cy - float(by), 0.0f), 1.0f);
    const float zPixel = LinearDistanceOf(inputs.state.inverseProjection, depthValues[size_t(y) * inputs.depthWidth + size_t(x)]);
    float sum = 0.0f, weights = 0.0f, plain = 0.0f;
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx)
        {
            const int tx = std::min(std::max(bx + dx, 0), int(termW) - 1);
            const int ty = std::min(std::max(by + dy, 0), int(termH) - 1);
            const float t = term[size_t(ty) * termW + size_t(tx)];
            const float bilinear = (dx ? fx : 1.0f - fx) * (dy ? fy : 1.0f - fy);
            plain += bilinear * t;
            float w = bilinear;
            if (bilateral)
            {
                int px = 0, py = 0;
                MarchPixelOf(inputs, scale, tx, ty, px, py);
                const float zTexel = LinearDistanceOf(inputs.state.inverseProjection, depthValues[size_t(py) * inputs.depthWidth + size_t(px)]);
                w *= BilateralDepthWeight(zPixel, zTexel, kUpsampleDepthSigma);
            }
            sum += w * t;
            weights += w;
        }
    return (bilateral && weights > 1.0e-4f) ? sum / weights : plain;
}

// The beam composite's local level (GodraysRules.h): the mean of the (2 * kBeamTapsPerSide + 1)^2 grid of march texels around the pixel's own nearest texel.
float BeamLowpassRef(const Inputs& inputs, const std::vector<float>& term, uint32_t termW, uint32_t termH, int x, int y)
{
    const float scale = inputs.params.resolution == Resolution::Half ? 2.0f : 4.0f;
    const int centreX = BeamCentreTexel((float(x) + 0.5f - inputs.window.x) / scale - 0.5f);
    const int centreY = BeamCentreTexel((float(y) + 0.5f - inputs.window.y) / scale - 0.5f);
    const int stride = BeamTapStride(scale);
    float sum = 0.0f;
    for (int dy = -kBeamTapsPerSide; dy <= kBeamTapsPerSide; ++dy)
        for (int dx = -kBeamTapsPerSide; dx <= kBeamTapsPerSide; ++dx)
        {
            const int tx = std::min(std::max(centreX + dx * stride, 0), int(termW) - 1);
            const int ty = std::min(std::max(centreY + dy * stride, 0), int(termH) - 1);
            sum += term[size_t(ty) * termW + size_t(tx)];
        }
    return sum / float((2 * kBeamTapsPerSide + 1) * (2 * kBeamTapsPerSide + 1));
}

// The expected output of one pixel of the composite from the pass's own term. beams = false is the v2 composite (the whole upsampled term), kept as the
// yardstick the haze tests measure the beam composite against.
void ReferenceComposite(const Inputs& inputs, const SceneColour& scene, const std::vector<float>& term, uint32_t termW, uint32_t termH,
                        const std::vector<float>& depthValues, const float ceilings[3], bool ceilingOn, bool bilateral, int x, int y, float out[3],
                        bool beams = true)
{
    const float gain = inputs.params.intensity * kMarchGain * inputs.lightGain;
    float t = UpsampledTerm(inputs, term, termW, termH, depthValues, x, y, bilateral);
    t = t > 0.0f ? t : 0.0f;
    if (beams && t > 0.0f)
        t = BeamTerm(t, BeamLowpassRef(inputs, term, termW, termH, x, y));
    const bool sky = depthValues[size_t(y) * inputs.depthWidth + size_t(x)] <= 1.0e-6f;
    for (int c = 0; c < 3; ++c)
    {
        const float s = SceneAt(scene, x, y, c);
        out[c] = s;
        if (!(t > 0.0f) || !(gain > 0.0f))
            continue;
        const float light = t * gain * inputs.tintRgb[c] * inputs.fade;
        const float head = std::max(inputs.whitePoint - s, 0.0f);
        const float raw = light * inputs.whitePoint;
        float add = head * raw / std::max(raw + head, 1.0e-6f);
        if (sky && ceilingOn)
            add = SkyLimited(add, s, ceilings[c]);
        out[c] = s + add;
    }
}

// The expected stats: the sky histogram's ceilings (every 4th pixel each way over the window) and the sample count.
void ReferenceStats(const Inputs& inputs, const SceneColour& scene, const std::vector<float>& depthValues, float ceilings[3], uint32_t& samples)
{
    std::vector<uint32_t> bins[3] { std::vector<uint32_t>(kSkyBins, 0), std::vector<uint32_t>(kSkyBins, 0), std::vector<uint32_t>(kSkyBins, 0) };
    samples = 0;
    const int ox = int(inputs.window.x), oy = int(inputs.window.y);
    const int nx = (int(inputs.window.width) + 3) / 4, ny = (int(inputs.window.height) + 3) / 4;
    for (int sy = 0; sy < ny; ++sy)
        for (int sx = 0; sx < nx; ++sx)
        {
            const int px = ox + sx * 4 + 1, py = oy + sy * 4 + 1;
            if (px >= ox + int(inputs.window.width) || py >= oy + int(inputs.window.height))
                continue;
            if (depthValues[size_t(py) * inputs.depthWidth + size_t(px)] > 1.0e-6f)
                continue;
            ++samples;
            for (int c = 0; c < 3; ++c)
                ++bins[c][SkyBinOf(SceneAt(scene, px, py, c), inputs.whitePoint, kSkyBins)];
        }
    for (int c = 0; c < 3; ++c)
        ceilings[c] = SkyCeilingOf(bins[c].data(), kSkyBins, inputs.whitePoint, samples);
}

// ---- the ring structure (the prototype's metric, on the small frame) ---------------------------------------------------------------------
double RingStructure(const std::vector<float>& added, const std::vector<float>& angleDegrees, const std::vector<float>& azimuth, double coneDegrees,
                     int rings, int bins, size_t minRingPixels, size_t minBinPixels)
{
    double numerator = 0.0, denominator = 0.0;
    for (int k = 0; k < rings; ++k)
    {
        const double low = coneDegrees * double(k) / double(rings), high = coneDegrees * double(k + 1) / double(rings);
        std::vector<double> sum(size_t(bins), 0.0);
        std::vector<size_t> count(size_t(bins), 0);
        double total = 0.0;
        size_t members = 0;
        for (size_t i = 0; i < added.size(); ++i)
        {
            if (!(angleDegrees[i] >= low && angleDegrees[i] < high))
                continue;
            const int bin = int(std::floor((azimuth[i] + 3.14159265358979) / (2.0 * 3.14159265358979) * bins)) % bins;
            sum[size_t(bin)] += added[i];
            ++count[size_t(bin)];
            total += added[i];
            ++members;
        }
        if (members < minRingPixels)
            continue;
        const double mean = total / double(members);
        if (mean <= 1.0e-9)
            continue;
        std::vector<double> means;
        for (int b = 0; b < bins; ++b)
            if (count[size_t(b)] >= minBinPixels)
                means.push_back(sum[size_t(b)] / double(count[size_t(b)]));
        if (means.size() < 4)
            continue;
        double m = 0.0;
        for (const double v : means)
            m += v;
        m /= double(means.size());
        double variance = 0.0;
        for (const double v : means)
            variance += (v - m) * (v - m);
        const double deviation = std::sqrt(variance / double(means.size()));
        numerator += mean * (deviation / mean);
        denominator += mean;
    }
    return denominator > 0.0 ? numerator / denominator : 0.0;
}

// ---- the device and the frame's buffers ------------------------------------------------------------------------------------------

enum class AtlasMode
{
    Wall,   // a block of close-to-the-light texels in the middle third of the reached window (the first march's fixture)
    Strip,  // a full-height strip of them: one shadow slab through the light's vanishing point
    Sectors, // four 90-degree sectors around the light's axis, alternately occluded: a fan of beams (every ray's shadow depends on its azimuth around the light)
    Lit,    // every texel far: nothing is shadowed (the control)
    Plane,  // every texel at one light-space depth: the bias band is crossed by the rays
    Dark,   // every texel near the light: everything is shadowed
};

struct Options
{
    AtlasMode atlas = AtlasMode::Wall;
    bool twoCascades = false;        // a second real cascade in the second slice: slice 0 has the near band, slice 1 the far one, the wall is in slice 1
    bool wrongSliceCascade = false;  // a second cascade whose far samples leave its slice into the first slice (which carries the wall)
    uint32_t skySamplesOnly = 0;     // when > 0: only a corner of this many columns and rows is sky (the "almost no sky samples" fixture)
    float wallDistance = kWallDistance;
};

struct Fixture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> sceneTexture, toneTexture, depthTexture, atlasTexture, skyTexture, lutTexture;
    ComPtr<ID3D11ShaderResourceView> sceneView, toneView, depthView, atlasView, skyView, lutView;
    ComPtr<ID3D11RenderTargetView> toneRtv;
    ReferenceAtlas atlas;
    SceneColour sceneColours;
    FrameState state;
    ext::ffxiv::shafts::Window window { 0.0f, 0.0f, float(kWidth), float(kHeight) };
    std::vector<float> depthValues;
    Options options;
    float uRange[2] {}, vRange[2] {};

    bool Create(const Options& requested = Options {})
    {
        options = requested;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                     &context)))
            return false;
        if (device == nullptr || context == nullptr)
            return false;

        // The scene: a flat wall colour on the left half (a surface in front of the camera) and a sky gradient on the right (the far plane). The depth
        // matches: the reversed-Z value of the wall, 0 on the sky.
        std::vector<uint16_t> scene(size_t(kWidth) * kHeight * 4);
        sceneColours.rgb.assign(size_t(kWidth) * kHeight * 3, 0.0f);
        depthValues.assign(size_t(kWidth) * kHeight, 0.0f);
        const float wallDepth = DepthAtDistance(options.wallDistance);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const bool sky = options.skySamplesOnly > 0 ? (x < options.skySamplesOnly && y < options.skySamplesOnly) : x >= kWidth / 2;
                const size_t at = (size_t(y) * kWidth + x) * 4;
                for (uint32_t c = 0; c < 3; ++c)
                {
                    const float value = sky ? SkyChannel(y, c) : kWall;
                    scene[at + c] = HalfOf(value);
                    sceneColours.rgb[(size_t(y) * kWidth + x) * 3 + c] = FromHalf(HalfOf(value));
                }
                scene[at + 3] = HalfOf(1.0f);
                depthValues[size_t(y) * kWidth + x] = sky ? 0.0f : wallDepth;
            }
        if (!CreateTexture(kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, scene, kWidth * 8, sceneTexture, sceneView, nullptr) ||
            !CreateTexture(kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, std::vector<uint16_t>(scene.size(), 0), kWidth * 8,
                           toneTexture, toneView, &toneRtv))
            return false;
        {
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = kWidth;
            desc.Height = kHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA initial { depthValues.data(), kWidth * sizeof(float), 0 };
            if (FAILED(device->CreateTexture2D(&desc, &initial, &depthTexture)) ||
                FAILED(device->CreateShaderResourceView(depthTexture.Get(), nullptr, &depthView)))
                return false;
        }

        // The march's state: the camera, the light, the fog and the cascades.
        FillInverseProjection(state.inverseProjection);
        state.ndcScale[0] = 2.0f / float(kWidth);
        state.ndcScale[1] = -2.0f / float(kHeight);
        state.ndcOffset[0] = -1.0f;
        state.ndcOffset[1] = 1.0f;
        float light[3] { 0.45f, 0.55f, -0.70f }; // toward the sun: up and forward
        const float lightLength = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
        for (float& value : light)
            value /= lightLength;
        state.sunDirection[0] = light[0];
        state.sunDirection[1] = light[1];
        state.sunDirection[2] = light[2];
        state.inverseViewRow1[0] = 0.0f;
        state.inverseViewRow1[1] = 1.0f;
        state.inverseViewRow1[2] = 0.0f;
        state.inverseViewRow1[3] = 2.0f; // the camera's own world height
        FillProjection(state.projection);
        state.fog.density1 = 0.02f;
        state.fog.fallOff1 = 0.0f;
        state.fog.refHeight1 = 10.0f;
        state.fog.density2 = 0.01f;
        state.fog.fallOff2 = 0.0f;
        state.fog.refHeight2 = 40.0f;
        state.fog.diExponent = 4.0f;
        state.fog.distA = 1.0f;
        state.fog.distB = 100.0f;
        state.fog.lutA = 0.10f;
        state.fog.lutB = 0.05f;
        state.cameraWorldY = CameraWorldY(state.inverseViewRow1);
        state.invRhoCamera = 1.0f / FogDensityAt(state.fog, state.cameraWorldY, 1.0f);
        state.marchFar = 60.0f;
        state.discScale = DiscScale(-0.000264f); // the captured instances' own shared map row, the march's disc scale

        // The light's own basis: u across, v up the light-space plane, the depth row the light's axis negated and scaled (the depth grows away from the
        // light, so the compare's shadowed-when-z>=gathered says a sample behind an occluder is shadowed).
        float u[3] { -light[2], 0.0f, light[0] }; // cross(light, (0,1,0))
        const float uLength = std::sqrt(u[0] * u[0] + u[2] * u[2]);
        u[0] /= uLength;
        u[2] /= uLength;
        float v[3] { light[1] * u[2] - light[2] * u[1], light[2] * u[0] - light[0] * u[2], light[0] * u[1] - light[1] * u[0] };
        const float vLength = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (float& value : v)
            value /= vLength;
        const float su = 0.02f, sz = 0.015f;
        const float sv = 0.002f;
        auto MakeCascade = [&](float scaleV, float bandNear, float bandFar, float sliceCentreV) {
            Cascade c;
            c.matrix[0] = u[0] * su; c.matrix[1] = u[1] * su; c.matrix[2] = u[2] * su; c.matrix[3] = 0.5f;
            c.matrix[4] = v[0] * scaleV; c.matrix[5] = v[1] * scaleV; c.matrix[6] = v[2] * scaleV;
            // The v row's offset puts the middle of the cascade's own band on the axis at the centre of its slice, whatever the scale.
            const float mid = 0.5f * (bandNear + bandFar);
            c.matrix[7] = sliceCentreV - scaleV * v[2] * -mid;
            c.matrix[8] = -light[0] * sz; c.matrix[9] = -light[1] * sz; c.matrix[10] = -light[2] * sz; c.matrix[11] = 0.70f;
            c.matrix[12] = 0.0f; c.matrix[13] = 0.0f; c.matrix[14] = 0.0f; c.matrix[15] = 1.0f;
            c.fade[0] = bandNear;
            c.fade[1] = bandFar;
            c.fade[2] = 100000.0f;
            c.fade[3] = 0.0f;
            c.distanceFar = 190.0f;
            c.rampScale = 0.052632f;
            c.rampTarget = 1.0f;
            c.shaped = ext::ffxiv::shafts::ShadowMatrixShape(c.matrix);
            return c;
        };
        std::vector<Cascade> real;
        if (options.twoCascades)
        {
            real.push_back(MakeCascade(sv, 0.10f, 25.0f, 0.125f));  // slice 0: the near band
            real.push_back(MakeCascade(sv, 22.0f, 100.0f, 0.375f)); // slice 1: the far band
        }
        else if (options.wrongSliceCascade)
        {
            real.push_back(MakeCascade(sv, 0.10f, 100.0f, 0.125f));  // slice 0, the whole range
            real.push_back(MakeCascade(0.010f, 22.0f, 100.0f, 0.375f)); // slice 1, with a v scale so large that its far samples leave the slice
            real.back().matrix[11] += 0.25f;                              // and a deeper depth row: a read of slice 0's wall shadows what a read of slice 1 leaves lit
        }
        else
        {
            real.push_back(MakeCascade(sv, 0.10f, 100.0f, 0.125f));
        }
        for (const Cascade& c : real)
            CHECK(c.shaped);
        CHECK_EQ(CascadeSlice(real[0].matrix, real[0].fade), 0u);
        if (real.size() > 1)
            CHECK_EQ(CascadeSlice(real[1].matrix, real[1].fade), 1u);
        // Degenerate cascades fill the rest of the four: shaped, but they map the whole frame far outside the atlas, so their slice is unreadable.
        std::vector<Cascade> all = real;
        while (all.size() < kCascadeCount)
        {
            Cascade degenerate {};
            degenerate.matrix[0] = 1000.0f; degenerate.matrix[5] = 1000.0f; degenerate.matrix[10] = 1000.0f; degenerate.matrix[15] = 1.0f;
            degenerate.matrix[3] = 500.0f;
            degenerate.matrix[7] = 500.0f;
            degenerate.matrix[11] = 500.0f;
            degenerate.fade[0] = 0.10f;
            degenerate.fade[1] = 190.0f;
            degenerate.fade[2] = 100000.0f;
            degenerate.distanceFar = 190.0f;
            degenerate.rampScale = 0.052632f;
            degenerate.rampTarget = 1.0f;
            degenerate.shaped = ext::ffxiv::shafts::ShadowMatrixShape(degenerate.matrix);
            CHECK(degenerate.shaped);
            CHECK_EQ(CascadeSlice(degenerate.matrix, degenerate.fade), kNoSlice);
            all.push_back(degenerate);
        }
        uint32_t order[kCascadeCount] {};
        Cascade padded[kCascadeCount] {};
        for (uint32_t i = 0; i < kCascadeCount; ++i)
            padded[i] = all[i];
        state.cascadeCount = CascadeOrder(padded, order, kCascadeCount);
        CHECK(state.cascadeCount == 4);
        for (uint32_t i = 0; i < state.cascadeCount; ++i)
            state.cascades[i] = padded[order[i]];

        // Where the frame's rays actually reach in the first real cascade's light-space window, so the occluders are placed inside the reached window.
        float uMin = 1.0f, uMax = 0.0f, vMin = 1.0f, vMax = 0.0f;
        const Cascade& first = real[0];
        const Params probe = MarchParams();
        for (uint32_t my = 0; my < MarchExtent(kHeight, probe.resolution); ++my)
            for (uint32_t mx = 0; mx < MarchExtent(kWidth, probe.resolution); ++mx)
            {
                const float scale = 4.0f;
                const float windowX = (float(mx) + 0.5f) * scale;
                const float windowY = (float(my) + 0.5f) * scale;
                const float ndcX = windowX * state.ndcScale[0] + state.ndcOffset[0];
                const float ndcY = windowY * state.ndcScale[1] + state.ndcOffset[1];
                float direction[3] {};
                RayDirection(state.inverseProjection, ndcX, ndcY, direction);
                for (float t = 2.0f; t <= 55.0f; t += 6.0f)
                {
                    const float p[3] { direction[0] * t, direction[1] * t, direction[2] * t };
                    const float clipX = first.matrix[0] * p[0] + first.matrix[1] * p[1] + first.matrix[2] * p[2] + first.matrix[3];
                    const float clipY = first.matrix[4] * p[0] + first.matrix[5] * p[1] + first.matrix[6] * p[2] + first.matrix[7];
                    const float clipZ = first.matrix[8] * p[0] + first.matrix[9] * p[1] + first.matrix[10] * p[2] + first.matrix[11];
                    const float lu = clipX, lv = clipY, lz = clipZ;
                    if (lu >= 0.0f && lu <= 1.0f && lv >= 0.0f && lv <= 0.25f && lz >= 0.0f && lz <= 1.0f)
                    {
                        uMin = lu < uMin ? lu : uMin;
                        uMax = lu > uMax ? lu : uMax;
                        vMin = lv < vMin ? lv : vMin;
                        vMax = lv > vMax ? lv : vMax;
                    }
                }
            }
        CHECK(uMax > uMin && vMax > vMin); // the rays reach a real window in the cascade
        uRange[0] = uMin; uRange[1] = uMax; vRange[0] = vMin; vRange[1] = vMax;

        // The synthetic atlas. The occluder's texels sit close to the light (depth 0.20); the rest is far (0.98). An occluder covers the reached window's
        // footprint in the cascade's own slice(s): a sample inside it and deeper than it is shadowed; one above or beside it is lit.
        atlas.texels.assign(size_t(kAtlasWidth) * kAtlasHeight, 0);
        const uint16_t farTexel = uint16_t(0.98f * 65535.0f + 0.5f);
        const uint16_t wallTexel = uint16_t(0.20f * 65535.0f + 0.5f);
        const uint16_t planeTexel = uint16_t(0.55f * 65535.0f + 0.5f);
        const uint16_t deepTexel = uint16_t(0.02f * 65535.0f + 0.5f); // the sectors' occluder: nearly the whole ray lies behind it
        const float spanU = uMax - uMin, spanV = vMax - vMin;
        for (uint32_t y = 0; y < kAtlasHeight; ++y)
            for (uint32_t x = 0; x < kAtlasWidth; ++x)
            {
                const float uu = (float(x) + 0.5f) / float(kAtlasWidth);
                const float vv = (float(y) + 0.5f) / float(kAtlasHeight);
                const uint32_t slice = y / (kAtlasHeight / 4);
                const float vInSlice = vv - 0.25f * float(slice); // the footprint is the same in every slice (the second cascade is the first one shifted)
                const float fu = (uu - uMin) / spanU;
                const float fv = (vInSlice - vMin) / spanV;
                uint16_t texel = farTexel;
                switch (options.atlas)
                {
                case AtlasMode::Wall:
                    if (slice == 0 && fu >= 0.40f && fu <= 0.60f && fv >= 0.40f && fv <= 0.60f)
                        texel = wallTexel;
                    break;
                case AtlasMode::Strip:
                    if (slice == 0 && fu >= 0.42f && fu <= 0.58f)
                        texel = wallTexel;
                    break;
                case AtlasMode::Sectors:
                {
                    // The camera sits at light-space (u, v) = (matrix[3], matrix[7]) of the first cascade; a ray p = t d lands at metres (t u.d, t v.d)
                    // from it, so a sector pattern in these metres is a pattern in the ray's azimuth around the light, whatever t.
                    const float mu = (uu - real[0].matrix[3]) / su;
                    const float mv = (vInSlice - real[0].matrix[7]) / sv;
                    const int sector = int(std::floor((std::atan2(mv, mu) + 3.14159265f) / (3.14159265f / 2.0f)));
                    if (slice == 0 && sector % 2 == 0 && std::sqrt(mu * mu + mv * mv) > 1.0f)
                        texel = deepTexel;
                    break;
                }
                case AtlasMode::Plane:
                    texel = planeTexel;
                    break;
                case AtlasMode::Dark:
                    texel = wallTexel;
                    break;
                case AtlasMode::Lit:
                    break;
                }
                if (options.twoCascades)
                    // the near slice is clear, the whole far slice is a wall
                    texel = slice == 1 ? wallTexel : farTexel;
                if (options.wrongSliceCascade)
                    // slice 0 is a wall everywhere, slice 1 is clear
                    texel = slice == 0 ? wallTexel : farTexel;
                atlas.texels[size_t(y) * kAtlasWidth + x] = texel;
            }
        {
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = kAtlasWidth;
            desc.Height = kAtlasHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA initial { atlas.texels.data(), kAtlasWidth * sizeof(uint16_t), 0 };
            if (FAILED(device->CreateTexture2D(&desc, &initial, &atlasTexture)) ||
                FAILED(device->CreateShaderResourceView(atlasTexture.Get(), nullptr, &atlasView)))
                return false;
        }

        // The engine's sky (half the frame, one colour) and its weight LUT (256x1, R16G16).
        std::vector<uint16_t> sky(size_t(kSkyWidth) * kSkyHeight * 4);
        for (size_t at = 0; at < sky.size(); at += 4)
        {
            sky[at + 0] = HalfOf(0.60f);
            sky[at + 1] = HalfOf(0.55f);
            sky[at + 2] = HalfOf(0.50f);
            sky[at + 3] = HalfOf(1.0f);
        }
        if (!CreateTexture(kSkyWidth, kSkyHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, sky, kSkyWidth * 8, skyTexture, skyView, nullptr))
            return false;
        {
            std::vector<uint16_t> lut(256 * 2);
            for (uint32_t i = 0; i < 256; ++i)
            {
                lut[i * 2 + 0] = HalfOf(0.25f); // .x: the fog's own weight
                lut[i * 2 + 1] = HalfOf(0.75f); // .y: the sky-lerp weight
            }
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = 256;
            desc.Height = 1;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16G16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA initial { lut.data(), 256 * 2 * sizeof(uint16_t), 0 };
            if (FAILED(device->CreateTexture2D(&desc, &initial, &lutTexture)) ||
                FAILED(device->CreateShaderResourceView(lutTexture.Get(), nullptr, &lutView)))
                return false;
        }
        return true;
    }

    template <class Element>
    bool CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, const std::vector<Element>& pixels, uint32_t rowPitch,
                       ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11ShaderResourceView>& view, ComPtr<ID3D11RenderTargetView>* rtv)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rtv != nullptr ? D3D11_BIND_RENDER_TARGET : 0u);
        D3D11_SUBRESOURCE_DATA initial { pixels.data(), rowPitch, 0 };
        if (FAILED(device->CreateTexture2D(&desc, &initial, &texture)) || !texture)
            return false;
        if (rtv != nullptr && FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, rtv->GetAddressOf())))
            return false;
        return SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view)) && view != nullptr;
    }

    Inputs MakeInputs(const Params& params, bool withSky = false)
    {
        Inputs inputs;
        inputs.scene = sceneView.Get();
        inputs.depth = depthView.Get();
        inputs.atlas = atlasView.Get();
        inputs.sky = withSky ? skyView.Get() : nullptr;
        inputs.lut = withSky ? lutView.Get() : nullptr;
        inputs.sceneFormat = 10; // DXGI_FORMAT_R16G16B16A16_FLOAT
        inputs.sceneWidth = kWidth;
        inputs.sceneHeight = kHeight;
        inputs.depthFormat = 41; // DXGI_FORMAT_R32_FLOAT
        inputs.depthWidth = kWidth;
        inputs.depthHeight = kHeight;
        inputs.window = window;
        // The sun's screen position: the light's direction projected through the fixture's own projection (the radial's own rule).
        const float clipX = state.projection[0] * state.sunDirection[0] + state.projection[2] * state.sunDirection[2];
        const float clipY = state.projection[5] * state.sunDirection[1] + state.projection[6] * state.sunDirection[2];
        const float clipW = state.projection[14] * state.sunDirection[2];
        inputs.sunScreenX = (clipX / clipW * 0.5f + 0.5f) * window.width;
        inputs.sunScreenY = (0.5f - clipY / clipW * 0.5f) * window.height;
        inputs.state = state;
        inputs.params = params;
        inputs.whitePoint = kWhite;
        inputs.fade = 1.0f;
        return inputs;
    }

    // A fixed tint, no colour pass: the composite reference needs only the numbers it is handed.
    Inputs MakeTintedInputs(const Params& params)
    {
        Inputs inputs = MakeInputs(params);
        inputs.tintFromSky = false;
        inputs.tintRgb[0] = 1.0f;
        inputs.tintRgb[1] = 0.8f;
        inputs.tintRgb[2] = 0.6f;
        return inputs;
    }

    ComPtr<ID3D11Texture2D> StagingOf(ID3D11Resource* source, D3D11_TEXTURE2D_DESC& desc)
    {
        ComPtr<ID3D11Texture2D> texture;
        source->QueryInterface(IID_PPV_ARGS(&texture));
        texture->GetDesc(&desc);
        ComPtr<ID3D11Texture2D> staging;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CHECK_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging));
        context->CopyResource(staging.Get(), source);
        return staging;
    }

    // Reads a single-channel R16F texture back to the CPU (half floats converted to floats).
    std::vector<float> ReadTerm(ID3D11ShaderResourceView* view, uint32_t width, uint32_t height)
    {
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        D3D11_TEXTURE2D_DESC desc {};
        ComPtr<ID3D11Texture2D> staging = StagingOf(resource.Get(), desc);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        CHECK_SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<float> out(size_t(width) * height);
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* rowBytes = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
            const uint16_t* row = reinterpret_cast<const uint16_t*>(rowBytes);
            for (uint32_t x = 0; x < width; ++x)
                out[size_t(y) * width + x] = FromHalf(row[x]);
        }
        context->Unmap(staging.Get(), 0);
        return out;
    }

    // Reads the pass's own scene copy (the returned SRV) back as half floats.
    std::vector<uint16_t> ReadScene(ID3D11ShaderResourceView* srv)
    {
        ComPtr<ID3D11Resource> resource;
        srv->GetResource(&resource);
        D3D11_TEXTURE2D_DESC desc {};
        ComPtr<ID3D11Texture2D> staging = StagingOf(resource.Get(), desc);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        CHECK_SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<uint16_t> out(size_t(desc.Width) * desc.Height * 4);
        for (uint32_t y = 0; y < desc.Height; ++y)
            std::memcpy(out.data() + size_t(y) * desc.Width * 4, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                        desc.Width * 4 * sizeof(uint16_t));
        context->Unmap(staging.Get(), 0);
        return out;
    }

    // The whole term against the reference, as the worst absolute difference.
    float WorstTermDifference(const Pass& pass, const Inputs& inputs, const RefOptions& ref = RefOptions {})
    {
        const uint32_t w = MarchExtent(window.width, inputs.params.resolution), h = MarchExtent(window.height, inputs.params.resolution);
        const std::vector<float> term = ReadTerm(pass.TermView(), w, h);
        float worst = 0.0f;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                worst = std::max(worst, std::fabs(ReferenceTerm(inputs, atlas, int(x), int(y), depthValues, ref) - term[size_t(y) * w + x]));
        return worst;
    }

    // How many march pixels two references disagree on by more than `threshold` (the fixture's sensitivity to a rule).
    uint32_t PixelsSeparated(const Inputs& inputs, const RefOptions& a, const RefOptions& b, float threshold)
    {
        const uint32_t w = MarchExtent(window.width, inputs.params.resolution), h = MarchExtent(window.height, inputs.params.resolution);
        uint32_t count = 0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                if (std::fabs(ReferenceTerm(inputs, atlas, int(x), int(y), depthValues, a) - ReferenceTerm(inputs, atlas, int(x), int(y), depthValues, b)) > threshold)
                    ++count;
        return count;
    }

    // The added luminance of a returned scene copy over the original scene, per full-resolution pixel.
    std::vector<float> AddedLuminance(const std::vector<uint16_t>& after)
    {
        std::vector<float> out(size_t(kWidth) * kHeight);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                float sum = 0.0f;
                const float weights[3] { 0.2126f, 0.7152f, 0.0722f };
                for (uint32_t c = 0; c < 3; ++c)
                    sum += weights[c] * (FromHalf(after[(size_t(y) * kWidth + x) * 4 + c]) - SceneAt(sceneColours, int(x), int(y), int(c)));
                out[size_t(y) * kWidth + x] = sum;
            }
        return out;
    }
};

// The angle (degrees) and azimuth of every pixel's view ray around the shadow light, for the ring structure.
void AnglesAroundLight(const Fixture& fx, std::vector<float>& angle, std::vector<float>& azimuth)
{
    angle.assign(size_t(kWidth) * kHeight, 0.0f);
    azimuth.assign(size_t(kWidth) * kHeight, 0.0f);
    const float* l = fx.state.sunDirection;
    float a0[3] { -l[1], l[0], 0.0f }; // cross(L, (0,0,-1))
    const float aLength = std::sqrt(a0[0] * a0[0] + a0[1] * a0[1] + a0[2] * a0[2]);
    for (float& value : a0)
        value /= aLength;
    const float b0[3] { l[1] * a0[2] - l[2] * a0[1], l[2] * a0[0] - l[0] * a0[2], l[0] * a0[1] - l[1] * a0[0] };
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const float ndcX = (float(x) + 0.5f) * fx.state.ndcScale[0] + fx.state.ndcOffset[0];
            const float ndcY = (float(y) + 0.5f) * fx.state.ndcScale[1] + fx.state.ndcOffset[1];
            float d[3] {};
            RayDirection(fx.state.inverseProjection, ndcX, ndcY, d);
            float c = d[0] * l[0] + d[1] * l[1] + d[2] * l[2];
            c = std::min(std::max(c, -1.0f), 1.0f);
            const float perp[3] { d[0] - c * l[0], d[1] - c * l[1], d[2] - c * l[2] };
            angle[size_t(y) * kWidth + x] = std::acos(c) * 57.29578f;
            azimuth[size_t(y) * kWidth + x] = std::atan2(perp[0] * b0[0] + perp[1] * b0[1] + perp[2] * b0[2], perp[0] * a0[0] + perp[1] * a0[1] + perp[2] * a0[2]);
        }
}
} // namespace

int main()
{
    // ---- the hash: the pass's jitter is the prototype's hash (golden values computed by the prototype's numpy copy) ----------------------
    CHECK_EQ(HashBits(0, 0), 0u);
    CHECK_EQ(HashBits(1, 0), 0xf4d86eu);
    CHECK_EQ(HashBits(12, 34), 0x2a8ccu);
    CHECK_EQ(HashBits(479, 269), 0xd19318u);

    Fixture fx;
    if (!fx.Create())
    {
        std::printf("godrays_warp: the fixture could not be created\n");
        return 1;
    }
    Pass pass;

    // ---- the lab driver's view of the interface: new members default to the full effect ---------------------------------------------------
    {
        const Inputs defaults {};
        CHECK_EQ(defaults.lightGain, 1.0f);
        CHECK(defaults.skyCeiling && defaults.bilateral);
        CHECK(pass.StatsTexture() == nullptr); // nothing is created before a run
    }

    // ---- the march's term against the CPU reference march, at the point compare ----------------------------------------------------------
    {
        const Params params = MarchParams();
        const Inputs inputs = fx.MakeInputs(params);
        ID3D11ShaderResourceView* ran = pass.Run(fx.context.Get(), inputs);
        CHECK(ran != nullptr);
        if (ran == nullptr)
        {
            std::printf("godrays_warp: the pass did not run (%s)\n", pass.Failure());
            return Finish("godrays_warp");
        }
        const uint32_t marchWidth = MarchExtent(fx.window.width, params.resolution);
        const uint32_t marchHeight = MarchExtent(fx.window.height, params.resolution);
        CHECK(marchWidth == 32u && marchHeight == 24u);
        const std::vector<float> term = fx.ReadTerm(pass.TermView(), marchWidth, marchHeight);
        float worst = 0.0f, best = 0.0f, least = 1.0e9f;
        uint32_t litPixels = 0, darkPixels = 0, midPixels = 0;
        for (uint32_t y = 0; y < marchHeight; ++y)
            for (uint32_t x = 0; x < marchWidth; ++x)
            {
                const float expected = ReferenceTerm(inputs, fx.atlas, int(x), int(y), fx.depthValues);
                const float got = term[size_t(y) * marchWidth + x];
                worst = std::max(worst, std::fabs(expected - got));
                best = std::max(best, expected);
                if (expected > 0.01f)
                {
                    least = std::min(least, expected);
                    ++litPixels;
                    if (got < 0.005f)
                        ++darkPixels; // a ray the reference marches as lit must not come back dark
                }
            }
        for (uint32_t y = 0; y < marchHeight; ++y)
            for (uint32_t x = 0; x < marchWidth; ++x)
            {
                const float expected = ReferenceTerm(inputs, fx.atlas, int(x), int(y), fx.depthValues);
                if (expected > 0.01f && expected < 0.7f * best)
                    ++midPixels; // the shadowed rays' terms are well under the lit ones'
            }
        CHECK(worst < 2.0e-3f); // WARP is deterministic; the float paths' rounding is the only difference
        CHECK(litPixels > 0);
        CHECK(darkPixels == 0);
        CHECK(midPixels > 0);
        CHECK(best > least * 1.4f); // the lit rays' terms and the shadowed ones' are apart by more than the noise
        // The sky rays (depth 0, w = 0 in the inverse projection) march too, to the cascades' reach, and the right half of the frame has a term.
        float skyMean = 0.0f;
        uint32_t skyCount = 0;
        for (uint32_t y = 0; y < marchHeight; ++y)
            for (uint32_t x = marchWidth / 2 + 1; x < marchWidth; ++x)
            {
                skyMean += term[size_t(y) * marchWidth + x];
                ++skyCount;
            }
        CHECK(skyMean / float(skyCount) > 0.01f);
    }

    // ---- the softness taps agree with the reference at both settings ----------------------------------------------------------------
    for (const uint32_t softness : { 1u, 2u })
    {
        const Inputs inputs = fx.MakeInputs(MarchParams(softness));
        CHECK(pass.Run(fx.context.Get(), inputs) != nullptr);
        CHECK(fx.WorstTermDifference(pass, inputs) < 2.0e-3f);
    }

    // ---- the jitter: off is deterministic and differs from on -----------------------------------------------------------------------
    {
        const Inputs off = fx.MakeInputs(MarchParams(0, false));
        CHECK(pass.Run(fx.context.Get(), off) != nullptr);
        const std::vector<float> firstTerm = fx.ReadTerm(pass.TermView(), 32, 24);
        CHECK(pass.Run(fx.context.Get(), off) != nullptr);
        const std::vector<float> secondTerm = fx.ReadTerm(pass.TermView(), 32, 24);
        CHECK(firstTerm == secondTerm); // two runs of the same frame are one picture
        CHECK(fx.WorstTermDifference(pass, off) < 2.0e-3f);
        CHECK(pass.Run(fx.context.Get(), fx.MakeInputs(MarchParams(0, true))) != nullptr);
        const std::vector<float> jitterTerm = fx.ReadTerm(pass.TermView(), 32, 24);
        CHECK(jitterTerm != firstTerm); // the jitter moved the samples
    }

    // ---- the extinction: a dense medium (density 100, tau 5 over the wall) is seen by the fixture and matched by the shader ------------------
    {
        Params dense = MarchParams(0, false);
        dense.density = 100.0f;
        const Inputs inputs = fx.MakeInputs(dense);
        CHECK(pass.Run(fx.context.Get(), inputs) != nullptr);
        CHECK(fx.WorstTermDifference(pass, inputs) < 2.0e-3f);
        RefOptions noExtinction;
        noExtinction.extinction = false;
        CHECK(fx.PixelsSeparated(inputs, RefOptions {}, noExtinction, 0.05f) > 100); // without extinction the wall rays' term is several times larger
        // And the lobe: the first march's cos^16 differs from Henyey-Greenstein almost everywhere on the frame.
        RefOptions oldLobe;
        oldLobe.henyey = false;
        CHECK(fx.PixelsSeparated(fx.MakeInputs(MarchParams(0, false)), RefOptions {}, oldLobe, 0.01f) > 200);
    }

    // ---- the air bias: a flat occluder plane crossed by the rays; the 0.002 band is lit ---------------------------------------------------------
    {
        Options plane;
        plane.atlas = AtlasMode::Plane;
        Fixture fp;
        CHECK(fp.Create(plane));
        Pass planePass;
        Params planeParams = MarchParams(0, true);
        planeParams.density = 100.0f;
        const Inputs inputs = fp.MakeInputs(planeParams);
        CHECK(planePass.Run(fp.context.Get(), inputs) != nullptr);
        CHECK(fp.WorstTermDifference(planePass, inputs) < 2.0e-3f);
        RefOptions noBias;
        noBias.bias = false;
        const uint32_t biasSeparated = fp.PixelsSeparated(inputs, RefOptions {}, noBias, 0.003f);
        std::printf("  bias fixture: %u march pixels separate the bias from no bias\n", biasSeparated);
        CHECK(biasSeparated >= 5); // samples inside the band exist: a shader without the bias moves those pixels past the tolerance
    }

    // ---- the cascade pick: the near cascade's window contains the far samples, the far cascade's wall must still shadow them -----------------
    {
        Options two;
        two.twoCascades = true;
        Fixture f2;
        CHECK(f2.Create(two));
        Pass twoPass;
        const Inputs inputs = f2.MakeInputs(MarchParams(0, false));
        CHECK(twoPass.Run(f2.context.Get(), inputs) != nullptr);
        CHECK(f2.WorstTermDifference(twoPass, inputs) < 2.0e-3f);
        RefOptions firstWins;
        firstWins.minOverCascades = false;
        CHECK(f2.PixelsSeparated(inputs, RefOptions {}, firstWins, 0.01f) > 50); // the first-cascade-wins pick hides the far wall's shadow
    }

    // ---- the slice test: a cascade does not read another cascade's slice -----------------------------------------------------------------------
    {
        Options wrong;
        wrong.wrongSliceCascade = true;
        Fixture fw;
        CHECK(fw.Create(wrong));
        Pass wrongPass;
        const Inputs inputs = fw.MakeInputs(MarchParams(0, false));
        CHECK(wrongPass.Run(fw.context.Get(), inputs) != nullptr);
        CHECK(fw.WorstTermDifference(wrongPass, inputs) < 2.0e-3f);
        RefOptions noSlice;
        noSlice.sliceTest = false;
        const uint32_t sliceSeparated = fw.PixelsSeparated(inputs, RefOptions {}, noSlice, 0.01f);
        std::printf("  slice fixture: %u march pixels separate the slice test from none\n", sliceSeparated);
        CHECK(sliceSeparated > 20); // without the slice test the far samples read slice 0's wall
    }

    // ---- the beams: occluders give the rings around the light a structure the all-lit control does not have ------------------------------------------
    // One strip is one shadow slab through the light's vanishing point (two of the twelve azimuth bins): a little structure above the control. A fan of
    // alternately occluded sectors around the light's axis is what tree crowns do to the air: every ray's shadow depends on its azimuth around the light.
    {
        Options strip;
        strip.atlas = AtlasMode::Strip;
        strip.skySamplesOnly = 1; // one uniform wall: the control's rings carry no wall/sky structure of their own
        Options sectors = strip;
        sectors.atlas = AtlasMode::Sectors;
        Options lit = strip;
        lit.atlas = AtlasMode::Lit;
        double structure[3] {};
        const Options variants[3] { strip, lit, sectors };
        const char* names[3] { "strip", "control", "sectors" };
        for (int which = 0; which < 3; ++which)
        {
            Fixture fb;
            CHECK(fb.Create(variants[which]));
            Pass beamPass;
            const Inputs inputs = fb.MakeTintedInputs(MarchParams(0, true));
            ID3D11ShaderResourceView* ran = beamPass.Run(fb.context.Get(), inputs);
            CHECK(ran != nullptr);
            const std::vector<float> added = fb.AddedLuminance(fb.ReadScene(ran));
            std::vector<float> angle, azimuth;
            AnglesAroundLight(fb, angle, azimuth);
            // The prototype's metric at the small frame's scale: this frame has 1/15 of the real one's pixels and a 32x24 term, so the rings are five over a
            // 20-degree cone and the azimuth is cut in eight bins (the real frame's metric is sixteen rings, 24 bins and a 30-degree cone).
            structure[which] = RingStructure(added, angle, azimuth, 20.0, 5, 8, 40, 4);
            std::printf("  ring structure (%s): %.4f\n", names[which], structure[which]);
        }
        CHECK(structure[0] >= structure[1] + 0.015); // one strip: its slab shows (a little: the beam composite keeps a single wedge's structure only above the local level)
        CHECK(structure[2] >= 0.3);                  // the fan: the brief's bar
        CHECK(structure[2] >= structure[1] + 0.3);   // and clearly above its own V = 1 control
    }

    // ---- the composite: the bound, the bilateral upsample and the sky ceiling against the reference composite ----------------------------------------
    {
        Pass compositePass;
        for (const bool bilateral : { true, false })
        {
            Inputs inputs = fx.MakeTintedInputs(MarchParams(0, true));
            inputs.bilateral = bilateral;
            ID3D11ShaderResourceView* ran = compositePass.Run(fx.context.Get(), inputs);
            CHECK(ran != nullptr);
            const std::vector<uint16_t> after = fx.ReadScene(ran);
            const uint32_t tw = MarchExtent(fx.window.width, inputs.params.resolution), th = MarchExtent(fx.window.height, inputs.params.resolution);
            const std::vector<float> term = fx.ReadTerm(compositePass.TermView(), tw, th);
            float ceilings[3] {};
            uint32_t samples = 0;
            ReferenceStats(inputs, fx.sceneColours, fx.depthValues, ceilings, samples);
            CHECK(samples > kMinSkySamples);
            float worst = 0.0f;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    float expected[3] {};
                    ReferenceComposite(inputs, fx.sceneColours, term, tw, th, fx.depthValues, ceilings, true, bilateral, int(x), int(y), expected);
                    for (int c = 0; c < 3; ++c)
                        worst = std::max(worst, std::fabs(expected[c] - FromHalf(after[(size_t(y) * kWidth + x) * 4 + size_t(c)])));
                    // The bound: no channel past the white, ever.
                    CHECK(FromHalf(after[(size_t(y) * kWidth + x) * 4]) <= kWhite + 2.0e-3f);
                }
            CHECK(worst < 3.0e-3f);
            // The bilateral and the bilinear upsample differ on the edge between the wall and the sky, and only there.
            if (bilateral)
            {
                Inputs plain = inputs;
                plain.bilateral = false;
                Pass plainPass;
                const std::vector<uint16_t> flat = fx.ReadScene(plainPass.Run(fx.context.Get(), plain));
                float edgeDifference = 0.0f, flatDifference = 0.0f;
                for (uint32_t y = 0; y < kHeight; ++y)
                    for (uint32_t x = 0; x < kWidth; ++x)
                    {
                        const float d = std::fabs(FromHalf(after[(size_t(y) * kWidth + x) * 4 + 1]) - FromHalf(flat[(size_t(y) * kWidth + x) * 4 + 1]));
                        if (x >= kWidth / 2 - 4 && x < kWidth / 2 + 4)
                            edgeDifference = std::max(edgeDifference, d);
                        else if (x < kWidth / 2 - 8 || x >= kWidth / 2 + 8)
                            flatDifference = std::max(flatDifference, d);
                    }
                std::printf("  bilateral vs plain: edge %.5f, flat %.5f\n", edgeDifference, flatDifference);
                CHECK(edgeDifference > 2.5e-4f);   // the edge keeps its side's term (was > 2e-3 for the v2 composite: next to the sky the wall's level is high, so the wall side passes only the kept share of the upsample's difference; measured 4.9e-4)
                CHECK(flatDifference < 1.0e-3f);   // away from the edge the two are the same picture
            }
        }
        // The bound's own zero rule: no light, no addition (the composite's expression, pinned at the host).
        CHECK(ext::ffxiv::shafts::ShaftAdded(0.5f, 0.0f, kWhite) == 0.0f);
    }

    // ---- the haze bar (Brief R3): an open field, no occluder at all, gets the kept share of the term and not the whole unshadowed in-scatter --------------
    // The v2 composite added the whole upsampled term, which on an open field is the smooth unshadowed integral: a haze (77-82 % of the no-shadow control's
    // lift on the captured coast). The beam composite adds the term's structure above its own local level plus kBeamKeep of the term.
    {
        Options open;
        open.atlas = AtlasMode::Lit;
        open.skySamplesOnly = 1; // one uniform ground: nothing to cast a shadow, no wall/sky edge in the lift
        Fixture fo;
        CHECK(fo.Create(open));
        Pass openPass;
        Inputs inputs = fo.MakeTintedInputs(MarchParams(0, true));
        ID3D11ShaderResourceView* ran = openPass.Run(fo.context.Get(), inputs);
        CHECK(ran != nullptr);
        const std::vector<uint16_t> after = fo.ReadScene(ran);
        const uint32_t tw = MarchExtent(fo.window.width, inputs.params.resolution), th = MarchExtent(fo.window.height, inputs.params.resolution);
        const std::vector<float> term = fo.ReadTerm(openPass.TermView(), tw, th);
        float ceilings[3] {};
        uint32_t samples = 0;
        ReferenceStats(inputs, fo.sceneColours, fo.depthValues, ceilings, samples);
        double sceneSum = 0.0, beamAdded = 0.0, veilAdded = 0.0, gpuAdded = 0.0;
        float worst = 0.0f;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                if (x < open.skySamplesOnly && y < open.skySamplesOnly)
                    continue; // the one sky pixel
                float beams[3] {}, veil[3] {};
                ReferenceComposite(inputs, fo.sceneColours, term, tw, th, fo.depthValues, ceilings, true, true, int(x), int(y), beams, true);
                ReferenceComposite(inputs, fo.sceneColours, term, tw, th, fo.depthValues, ceilings, true, true, int(x), int(y), veil, false);
                const float s = SceneAt(fo.sceneColours, int(x), int(y), 1);
                const float g = FromHalf(after[(size_t(y) * kWidth + x) * 4 + 1]);
                sceneSum += s;
                beamAdded += beams[1] - s;
                veilAdded += veil[1] - s;
                gpuAdded += g - s;
                worst = std::max(worst, std::fabs(beams[1] - g));
            }
        const double liftBeams = beamAdded / sceneSum, liftVeil = veilAdded / sceneSum, liftGpu = gpuAdded / sceneSum;
        std::printf("  open field: lift of the v2 composite %.4f, of the beam composite %.4f (GPU %.4f); worst GPU vs reference %.5f\n", liftVeil, liftBeams, liftGpu, worst);
        CHECK(worst < 3.0e-3f);           // the shader is the reference
        CHECK(liftVeil > 0.02);           // the fixture has a haze to remove (the old composite's lift)
        // The brief's literal bar (an open field adds at most 1 % on the ground) is measured at capture scale in the replay lab (castle +0.5 %, sea +0.9 %); this
        // 128 x 96 frame has a 32 x 24 term, smaller than the 25-texel grid's reach (+-12 texels), so the frame's own horizon gradient passes as structure here:
        // the check is relative (the beam composite lifts under 0.4 of what the old one did) with an absolute ceiling of 1.5 %. Stated, not tuned.
        CHECK(liftBeams < 0.4 * liftVeil);
        CHECK(std::fabs(liftGpu) <= 0.015);
    }

    // ---- the sky: it gets light up to its own 99th percentile, never past it; without the ceiling 5 % or more of it crosses ----------------------
    {
        Inputs inputs = fx.MakeTintedInputs(MarchParams(0, true));
        inputs.params.intensity = 2.0f; // the strongest key: the case the ceiling is for
        Pass capped, uncapped;
        const std::vector<uint16_t> withCeiling = fx.ReadScene(capped.Run(fx.context.Get(), inputs));
        Inputs without = inputs;
        without.skyCeiling = false;
        const std::vector<uint16_t> withoutCeiling = fx.ReadScene(uncapped.Run(fx.context.Get(), without));
        // The full-resolution 99th percentile of the scene's sky, per channel (the number the ceiling is judged against).
        float p99[3] {};
        for (int c = 0; c < 3; ++c)
        {
            std::vector<float> values;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = kWidth / 2; x < kWidth; ++x)
                    values.push_back(SceneAt(fx.sceneColours, int(x), int(y), c));
            std::sort(values.begin(), values.end());
            p99[c] = values[size_t(0.99 * double(values.size()))];
        }
        uint32_t crossedWith = 0, crossedWithout = 0, skyPixels = 0, brighter = 0;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = kWidth / 2; x < kWidth; ++x)
            {
                ++skyPixels;
                bool crossWith = false, crossWithout = false;
                for (int c = 0; c < 3; ++c)
                {
                    const float before = SceneAt(fx.sceneColours, int(x), int(y), c);
                    const float a = FromHalf(withCeiling[(size_t(y) * kWidth + x) * 4 + size_t(c)]);
                    const float b = FromHalf(withoutCeiling[(size_t(y) * kWidth + x) * 4 + size_t(c)]);
                    crossWith = crossWith || (a > p99[c] + 1.0e-3f && before <= p99[c] + 1.0e-3f);
                    crossWithout = crossWithout || (b > p99[c] + 1.0e-3f && before <= p99[c] + 1.0e-3f);
                    if (a > before + 1.0e-4f)
                        ++brighter;
                }
                crossedWith += crossWith ? 1u : 0u;
                crossedWithout += crossWithout ? 1u : 0u;
            }
        CHECK_EQ(crossedWith, 0u);                          // the ceiling: no sky pixel is pushed across the scene's own p99
        CHECK(crossedWithout * 20 >= skyPixels);            // without it 5 % or more of the sky crosses
        CHECK(brighter > 0);                                 // and the sky does get light (it is no longer left untouched)
        // The stats pass read back: ceilings, sample count and term peak equal the CPU's.
        float expectedCeilings[3] {};
        uint32_t expectedSamples = 0;
        ReferenceStats(inputs, fx.sceneColours, fx.depthValues, expectedCeilings, expectedSamples);
        StatsReader reader;
        CHECK(reader.Copy(fx.context.Get(), capped.StatsTexture(), 7));
        const StatsRead stats = reader.Read(fx.context.Get(), 8, true);
        CHECK(stats.landed);
        for (int c = 0; c < 3; ++c)
            CHECK(std::fabs(stats.skyCeiling[c] - expectedCeilings[c]) < 1.0e-4f);
        CHECK_EQ(stats.skySamples, expectedSamples);
        const std::vector<float> term = fx.ReadTerm(capped.TermView(), 32, 24);
        float peak = 0.0f;
        for (const float value : term)
            peak = std::max(peak, value);
        CHECK(peak > 0.0f && std::fabs(stats.termPeak - peak) < 1.0e-3f);
    }

    // ---- almost no sky samples: the ceiling is 0 and the sky is left alone -----------------------------------------------------------------------
    {
        Options corner;
        corner.skySamplesOnly = 12; // a 12x12 corner of sky: 9 samples, under kMinSkySamples
        Fixture fc;
        CHECK(fc.Create(corner));
        Pass cornerPass;
        Inputs inputs = fc.MakeTintedInputs(MarchParams(0, true));
        const std::vector<uint16_t> after = fc.ReadScene(cornerPass.Run(fc.context.Get(), inputs));
        float ceilings[3] {};
        uint32_t samples = 0;
        ReferenceStats(inputs, fc.sceneColours, fc.depthValues, ceilings, samples);
        CHECK(samples < kMinSkySamples && ceilings[0] == 0.0f);
        uint32_t changed = 0;
        for (uint32_t y = 0; y < 12; ++y)
            for (uint32_t x = 0; x < 12; ++x)
                for (int c = 0; c < 3; ++c)
                    if (after[(size_t(y) * kWidth + x) * 4 + size_t(c)] != HalfOf(SkyChannel(y, uint32_t(c))))
                        ++changed;
        CHECK_EQ(changed, 0u);
    }

    // ---- the enginefog colour: the composite reads the secured sky and LUT by the engine's own rule ----------------------------------
    {
        const Params params = BuildParams(48, "quarter", 1.0f, 1.0f, 1.0f, "enginefog", 0, true, 1.0f);
        const Inputs inputs = fx.MakeInputs(params, true);
        ID3D11ShaderResourceView* ranFog = pass.Run(fx.context.Get(), inputs);
        CHECK(ranFog != nullptr);
        const std::vector<uint16_t> composited = fx.ReadScene(ranFog);
        float added = 0.0f;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth / 2; ++x)
            {
                const float value = FromHalf(composited[(size_t(y) * kWidth + x) * 4]);
                if (value > kWall + 1.0e-6f)
                    added += value - kWall;
                CHECK(value <= kWhite + 2.0e-3f);
            }
        CHECK(added > 0.0f);
    }

    // ---- the game's graphics state comes back after a run: targets, views, constants, viewport, blend -----------------------------------------------
    {
        ComPtr<ID3D11RenderTargetView> gameTarget = fx.toneRtv;
        ID3D11RenderTargetView* targets[1] { gameTarget.Get() };
        fx.context->OMSetRenderTargets(1, targets, nullptr);
        ID3D11ShaderResourceView* gameViews[8] { fx.sceneView.Get(), nullptr, nullptr, nullptr, nullptr, fx.skyView.Get(), fx.lutView.Get(), fx.depthView.Get() };
        fx.context->PSSetShaderResources(0, 8, gameViews);
        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth = 64;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> gameBuffer;
        CHECK_SUCCEEDED(fx.device->CreateBuffer(&bd, nullptr, &gameBuffer));
        ID3D11Buffer* buffers[1] { gameBuffer.Get() };
        fx.context->PSSetConstantBuffers(0, 1, buffers);
        D3D11_BLEND_DESC blendDesc {};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D11BlendState> gameBlend;
        CHECK_SUCCEEDED(fx.device->CreateBlendState(&blendDesc, &gameBlend));
        const float factor[4] { 0.25f, 0.5f, 0.75f, 1.0f };
        fx.context->OMSetBlendState(gameBlend.Get(), factor, 0xffffffffu);
        const D3D11_VIEWPORT gameViewport { 3.0f, 5.0f, 17.0f, 19.0f, 0.0f, 1.0f };
        fx.context->RSSetViewports(1, &gameViewport);

        CHECK(pass.Run(fx.context.Get(), fx.MakeInputs(MarchParams())) != nullptr);

        ID3D11RenderTargetView* back[2] { nullptr, nullptr };
        fx.context->OMGetRenderTargets(2, back, nullptr);
        CHECK(back[0] == gameTarget.Get() && back[1] == nullptr);
        if (back[0] != nullptr)
            back[0]->Release();
        ID3D11ShaderResourceView* backViews[8] {};
        fx.context->PSGetShaderResources(0, 8, backViews);
        for (int i = 0; i < 8; ++i)
        {
            CHECK(backViews[i] == gameViews[i]);
            if (backViews[i] != nullptr)
                backViews[i]->Release();
        }
        ID3D11Buffer* backBuffer = nullptr;
        fx.context->PSGetConstantBuffers(0, 1, &backBuffer);
        CHECK(backBuffer == gameBuffer.Get());
        if (backBuffer != nullptr)
            backBuffer->Release();
        ID3D11BlendState* backBlend = nullptr;
        float backFactor[4] {};
        UINT backMask = 0;
        fx.context->OMGetBlendState(&backBlend, backFactor, &backMask);
        CHECK(backBlend == gameBlend.Get() && backFactor[1] == 0.5f && backMask == 0xffffffffu);
        if (backBlend != nullptr)
            backBlend->Release();
        D3D11_VIEWPORT backViewport {};
        UINT count = 1;
        fx.context->RSGetViewports(&count, &backViewport);
        CHECK(count == 1 && backViewport.TopLeftX == 3.0f && backViewport.Width == 17.0f && backViewport.Height == 19.0f);
        fx.context->OMSetBlendState(nullptr, factor, 0xffffffffu);
    }

    // ---- the game's compute state comes back too: shader, constants, views, the unordered-access slot -------------------------------------------------
    {
        ComPtr<ID3D11ComputeShader> gameShader;
        CHECK_SUCCEEDED(fx.device->CreateComputeShader(Godrays_Stats_cso, sizeof Godrays_Stats_cso, nullptr, &gameShader));
        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth = 64;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> gameBuffer;
        CHECK_SUCCEEDED(fx.device->CreateBuffer(&bd, nullptr, &gameBuffer));
        D3D11_TEXTURE2D_DESC td {};
        td.Width = 4;
        td.Height = 4;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> gameTexture;
        CHECK_SUCCEEDED(fx.device->CreateTexture2D(&td, nullptr, &gameTexture));
        ComPtr<ID3D11UnorderedAccessView> gameUav;
        CHECK_SUCCEEDED(fx.device->CreateUnorderedAccessView(gameTexture.Get(), nullptr, &gameUav));
        fx.context->CSSetShader(gameShader.Get(), nullptr, 0);
        ID3D11Buffer* buffers[1] { gameBuffer.Get() };
        fx.context->CSSetConstantBuffers(0, 1, buffers);
        ID3D11ShaderResourceView* views[4] { fx.sceneView.Get(), fx.atlasView.Get(), fx.depthView.Get(), fx.skyView.Get() };
        fx.context->CSSetShaderResources(0, 4, views);
        ID3D11UnorderedAccessView* uavs[1] { gameUav.Get() };
        fx.context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        CHECK(pass.Run(fx.context.Get(), fx.MakeInputs(MarchParams())) != nullptr);

        ID3D11ComputeShader* backShader = nullptr;
        fx.context->CSGetShader(&backShader, nullptr, nullptr);
        CHECK(backShader == gameShader.Get());
        if (backShader != nullptr)
            backShader->Release();
        ID3D11Buffer* backBuffer = nullptr;
        fx.context->CSGetConstantBuffers(0, 1, &backBuffer);
        CHECK(backBuffer == gameBuffer.Get());
        if (backBuffer != nullptr)
            backBuffer->Release();
        ID3D11ShaderResourceView* backViews[4] {};
        fx.context->CSGetShaderResources(0, 4, backViews);
        for (int i = 0; i < 4; ++i)
        {
            CHECK(backViews[i] == views[i]);
            if (backViews[i] != nullptr)
                backViews[i]->Release();
        }
        ID3D11UnorderedAccessView* backUav = nullptr;
        fx.context->CSGetUnorderedAccessViews(0, 1, &backUav);
        CHECK(backUav == gameUav.Get());
        if (backUav != nullptr)
            backUav->Release();
        // Unbind the sentinels again so the later blocks start from a clean compute state.
        ID3D11UnorderedAccessView* noneUav[1] {};
        fx.context->CSSetUnorderedAccessViews(0, 1, noneUav, nullptr);
        ID3D11ShaderResourceView* noneViews[4] {};
        fx.context->CSSetShaderResources(0, 4, noneViews);
        fx.context->CSSetShader(nullptr, nullptr, 0);
    }

    // ---- a binding the pass cannot read returns null with a reason ------------------------------------------------------------------
    {
        Pass refused;
        Inputs inputs = fx.MakeInputs(MarchParams());
        inputs.sceneFormat = 28; // R8G8B8A8_UNORM: not the FP16 the tone map reads
        CHECK(refused.Run(fx.context.Get(), inputs) == nullptr);
        CHECK(refused.Failure() != nullptr);
        Inputs noAtlas = fx.MakeInputs(MarchParams());
        noAtlas.atlas = nullptr;
        CHECK(refused.Run(fx.context.Get(), noAtlas) == nullptr);
        CHECK(refused.Failure() != nullptr);
        Inputs noDepth = fx.MakeInputs(MarchParams());
        noDepth.depth = nullptr;
        CHECK(refused.Run(fx.context.Get(), noDepth) == nullptr);
        Inputs emptyWindow = fx.MakeInputs(MarchParams());
        emptyWindow.window = ext::ffxiv::shafts::Window { 0.0f, 0.0f, 0.0f, 0.0f };
        CHECK(refused.Run(fx.context.Get(), emptyWindow) == nullptr);
    }

    // ---- the fog read without the height profile still returns the sun row (the night curve must not need the profile) ----------------------------------
    {
        float fogRow[32] {};
        fogRow[8] = 0.966f; fogRow[9] = 0.183f; fogRow[10] = 0.183f; fogRow[11] = 1.0f; // cFogParam[2]: the sun and the volume gate
        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth = kFogParamBytes;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial { fogRow, 0, 0 };
        ComPtr<ID3D11Buffer> fogBuffer;
        CHECK_SUCCEEDED(fx.device->CreateBuffer(&bd, &initial, &fogBuffer));
        FogReader reader;
        CHECK(reader.Copy(fx.context.Get(), fogBuffer.Get(), nullptr, nullptr, 5)); // no profile, no viewport buffer
        const FogRead read = reader.Read(fx.context.Get(), 6, true);
        CHECK(read.landed && !read.profile && !read.usable); // the march state is not usable...
        CHECK(read.sunLanded);                                 // ...and the sun still is
        CHECK(std::fabs(read.sun[1] - 0.183f) < 1.0e-6f && std::fabs(read.sun[0] - 0.966f) < 1.0e-6f);
        // A buffer whose sun row is not a unit vector publishes no sun.
        float badRow[32] {};
        badRow[9] = 0.3f;
        D3D11_SUBRESOURCE_DATA badInitial { badRow, 0, 0 };
        ComPtr<ID3D11Buffer> badBuffer;
        CHECK_SUCCEEDED(fx.device->CreateBuffer(&bd, &badInitial, &badBuffer));
        FogReader badReader;
        CHECK(badReader.Copy(fx.context.Get(), badBuffer.Get(), nullptr, nullptr, 5));
        const FogRead bad = badReader.Read(fx.context.Get(), 6, true);
        CHECK(bad.landed && !bad.sunLanded);
    }

    // ---- the stolen views: an own SRV when the bind flag allows it, an owned copy when it does not ------------------------------------
    {
        StolenViews views;
        bool ownView = false;
        uint32_t width = 0, height = 0, format = 0;
        ID3D11ShaderResourceView* atlas = views.SecureAtlas(fx.context.Get(), fx.atlasView.Get(), ownView, width, height, format);
        CHECK(atlas != nullptr && ownView);
        CHECK(width == kAtlasWidth && height == kAtlasHeight && format == 56); // R16_UNORM, the engine's own sampling format
        ComPtr<ID3D11Resource> atlasResource;
        atlas->GetResource(&atlasResource);
        CHECK(atlasResource.Get() == fx.atlasTexture.Get()); // the same resource, the march's own view on it
        ID3D11ShaderResourceView* sky = views.SecureSky(fx.context.Get(), fx.skyView.Get(), ownView, width, height, format);
        CHECK(sky != nullptr && ownView && width == kSkyWidth && height == kSkyHeight);
        // A source the march cannot secure (a multisampled texture) answers null: the fail-open, and the march skips.
        D3D11_TEXTURE2D_DESC desc {};
        fx.atlasTexture->GetDesc(&desc);
        desc.SampleDesc.Count = 4;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> msaa;
        CHECK_SUCCEEDED(fx.device->CreateTexture2D(&desc, nullptr, &msaa));
        ComPtr<ID3D11ShaderResourceView> msaaSrv;
        D3D11_SHADER_RESOURCE_VIEW_DESC msaaSrvDesc {};
        msaaSrvDesc.Format = DXGI_FORMAT_R16_UNORM;
        msaaSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        CHECK_SUCCEEDED(fx.device->CreateShaderResourceView(msaa.Get(), &msaaSrvDesc, &msaaSrv));
        ID3D11ShaderResourceView* refused = views.SecureAtlas(fx.context.Get(), msaaSrv.Get(), ownView, width, height, format);
        CHECK(refused == nullptr);
    }
    return Finish("godrays_warp");
}
