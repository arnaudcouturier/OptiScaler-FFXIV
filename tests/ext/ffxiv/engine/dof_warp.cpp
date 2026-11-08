// Depth of field, Route B: the pass (ext/ffxiv/engine/DofPass.cpp and its shaders) on the D3D11 WARP software adapter,
// on synthetic buffers this test builds itself.
//
// The frame is 160x120 with the camera at its origin looking down -z. The window's centre column carries the focus
// plane's depth (20 m); the left half of the depth is a near surface (8 m) and the right half a far one (60 m), and one
// column of the near surface carries a highlight above the pack's cap. The CPU reference in this file computes the
// pack's alpha (the rules' own CoC through the fixture camera) and the disc blur's exact tap loop (the golden-angle
// pattern, its own-coverage check, the energy normalization), and the pass's own targets are compared against it per
// pixel.
//
// What a wrong pack, blur or merge would break, and what is checked:
//   the focus is the view depth under the screen centre (auto), and a manual key overrides it with the same metres;
//   the near and far blur targets match the CPU reference blur per pixel (24 steps, aperture 0.05) in auto focus and in
//   manual focus at a different plane; the steps and aperture keys are checked only to change what the taps reach (a
//   wider aperture gives a different near target), not against the reference;
//   a perfectly focused pixel's merge is the scene's own value byte for byte, at every setting;
//   the focus band and the near cap (band section, on a noise scene): a plane whose raw CoC is inside +-0.12 is returned byte for byte,
//   the near plane's CoC in the pack is held at 0.25 and the near amount never passes 0.25 x strength, the far plane beyond the band is
//   still blurred and not capped, and the merge equals scene + (target - scene) x amount pixel by pixel;
//   Strength 0 (the pass's own rules gate) is the CopyResource byte-exact answer with no draws;
//   the pack's colour cap keeps a highlight above 10 from smearing through the taps (the blurred content is at the
//   cap, the in-focus highlight keeps its own value byte for byte);
//   the game's bindings come back after a run, and a binding the pass cannot read returns null with a reason.
#include "check.h"

#include "ext/ffxiv/engine/DofPass.h"
#include "ext/ffxiv/engine/DofRules.h"

#include <d3d11.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::dof;
using ext::ffxiv::shafts::Window;

#define CHECK_SUCCEEDED(call) CHECK(SUCCEEDED(call))

namespace
{
constexpr uint32_t kWidth = 160;
constexpr uint32_t kHeight = 120;
constexpr float kFocus = 20.0f;  // the fixture's focus plane: the centre column's own depth
constexpr float kNearA = 8.0f;   // the near half's first band
constexpr float kNearB = 14.0f;  // the near half's second band (a different CoC, so the near blur mixes)
constexpr float kFarA = 60.0f;   // the far half's first band
constexpr float kFarB = 30.0f;   // the far half's second band (a different CoC, so the far blur mixes)
constexpr float kWall = 0.50f;  // the scene's surface colour, exactly representable as a half
constexpr float kSky = 0.25f;   // the scene's sky colour (the right half's colour, a far scene)
constexpr float kHot = 18.0f;   // a highlight above the pack's cap, on the focus plane's own column
constexpr float kWhite = 1.0f;  // the FP16 alpha's own 1.0 (the merged output's untouched alpha)

constexpr uint32_t kQuarterWidth = QuarterExtent(float(kWidth));  // 40
constexpr uint32_t kQuarterHeight = QuarterExtent(float(kHeight)); // 30

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

// ---- the fixture's camera, the march's own fixture matrix -------------------------------------------------------------------------

constexpr float kNear = 0.1f;
constexpr float kFar = 100.0f;
constexpr float kTanFov = 1.0f;

constexpr float ProjectionA() { return kNear / (kFar - kNear); }
constexpr float ProjectionB() { return kNear * kFar / (kFar - kNear); }

void FillInverseProjection(float out[16])
{
    const float a = ProjectionA(), b = ProjectionB();
    out[0] = b / kTanFov; out[1] = 0.0f; out[2] = 0.0f; out[3] = 0.0f;
    out[4] = 0.0f; out[5] = b / kTanFov; out[6] = 0.0f; out[7] = 0.0f;
    out[8] = 0.0f; out[9] = 0.0f; out[10] = 0.0f; out[11] = -b;
    out[12] = 0.0f; out[13] = 0.0f; out[14] = 1.0f; out[15] = a;
}

// The depth texel of a view distance in this camera (forward projection's own inverse): d = b/|z| - a.
float DepthOfDistance(float viewZ)
{
    return (ProjectionB() / viewZ) - ProjectionA();
}

// ---- the reference pack and blur: the rules' own formulas, the shader's own shapes ------------------------------------------------

struct ReferencePack
{
    float colourCap = kPackColourCap;
    std::vector<float> coc;                       // the signed CoC per window pixel (the pack's alpha)
    std::vector<std::array<float, 3>> colour;     // the capped colour per window pixel (the pack's rgb)
    float focus = kFocus;
};

// The pack's own two channels per window pixel, through the fixture camera (the pack's shader's reconstruction: the
// depth texel is the pack's at, the mapping row's own scale and offset).
void FillReferencePack(ReferencePack& pack, const float inverse[16], float focus, const std::vector<float>& depth)
{
    pack.focus = focus;
    pack.coc.assign(size_t(kWidth) * kHeight, 0.0f);
    pack.colour.assign(size_t(kWidth) * kHeight, { 0.0f, 0.0f, 0.0f });
    const float ndcScaleX = 2.0f / float(kWidth);
    const float ndcScaleY = -2.0f / float(kHeight);
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const float ndcX = (float(x) + 0.5f) * ndcScaleX - 1.0f;
            const float ndcY = 1.0f - (float(y) + 0.5f) * (-ndcScaleY) * -1.0f - ((float(y) + 0.5f) * 2.0f / float(kHeight) - 1.0f) -
                               ((float(y) + 0.5f) * (-2.0f / float(kHeight)) + 1.0f) + ((float(y) + 0.5f) * (-2.0f / float(kHeight)) + 1.0f);
            const float ndcYFixed = (float(y) + 0.5f) * (-2.0f / float(kHeight)) + 1.0f;
            const float ndcXFixed = (float(x) + 0.5f) * (2.0f / float(kWidth)) - 1.0f;
            const float viewZ = ViewDistanceAtDepth(inverse, ndcXFixed, ndcYFixed, depth[size_t(y) * kWidth + x]);
            const size_t at = size_t(y) * kWidth + x;
            pack.coc[at] = CoCSigned(viewZ, focus);
            // The pack's rgb: min(col, 10), the game's own saturation habit (the pack's own shader cap).
            const float raw = (x == kWidth / 2) ? kHot : (x < kWidth / 2 ? kWall : kSky);
            for (uint32_t c = 0; c < 3; ++c)
                pack.colour[at][c] = raw < pack.colourCap ? raw : pack.colourCap;
        }
}

// One quarter pixel's pair of answers, the shader's own loop mirrored exactly (the march's own WARP discipline: the same
// float ops, the same clamps, the same order). The window is the fixture's own (the shader reads the pass's real one;
// this one is its stand-in, and the default Window{} would clamp the taps against an empty window).
void ReferenceBlurQuarter(const ReferencePack& pack, const Window& window, uint32_t qx, uint32_t qy, float reach,
                          uint32_t steps, float& nearR, float& nearG, float& nearB, float& farR, float& farG, float& farB)
{
    const Window& win = window;
    // The quarter pixel's window block at its centre, clamped inside the window (the march's own rule).
    float texelX = win.x + (float(qx) + 0.5f) * 4.0f;
    float texelY = win.y + (float(qy) + 0.5f) * 4.0f;
    texelX = texelX > win.x ? texelX : win.x; // max(texel, Window.xy), the shader's own clamp
    texelY = texelY > win.y ? texelY : win.y;
    texelX = texelX < win.x + win.width - 1.0f ? texelX : win.x + win.width - 1.0f;
    texelY = texelY < win.y + win.height - 1.0f ? texelY : win.y + win.height - 1.0f;
    const int tx = int(texelX), ty = int(texelY);
    const size_t ownAt = size_t(ty) * kWidth + tx;
    const float ownR = pack.colour[ownAt][0], ownG = pack.colour[ownAt][1], ownB = pack.colour[ownAt][2];
    const float ownC = pack.coc[ownAt];
    if (!(reach > 0.0f) || !(steps >= 2))
    {
        nearR = ownR; nearG = ownG; nearB = ownB;
        farR = ownR; farG = ownG; farB = ownB;
        return;
    }
    float nearSumR = 0.0f, nearSumG = 0.0f, nearSumB = 0.0f, nearWeight = 0.0f;
    float farSumR = 0.0f, farSumG = 0.0f, farSumB = 0.0f, farWeight = 0.0f;
    for (uint32_t i = 0; i < steps; ++i)
    {
        const float angle = float(i) * kGoldenAngle;
        const float radius = std::sqrt((float(i) + 0.5f) / float(steps)) * reach;
        float tapX = texelX + radius * std::cos(angle);
        float tapY = texelY + radius * std::sin(angle);
        tapX = tapX > win.x ? tapX : win.x;             // max(Window.xy, tap) - the shader's own clamp
        tapX = tapX < win.x + win.width - 1.0f ? tapX : win.x + win.width - 1.0f;
        tapY = tapY > win.y ? tapY : win.y;
        tapY = tapY < win.y + win.height - 1.0f ? tapY : win.y + win.height - 1.0f;
        const size_t at = size_t(int(tapY)) * kWidth + int(tapX);
        const float tapCoc = pack.coc[at];
        const float weight = TapWeight(tapCoc);
        if (!(weight > 0.0f))
            continue;
        if (!(radius <= std::fabs(tapCoc) * reach)) // TapReaches: the tap's own coverage, at the pre-snap radius
            continue;
        const float tapR = pack.colour[at][0], tapG = pack.colour[at][1], tapB = pack.colour[at][2];
        if (tapCoc > 0.0f)
        {
            nearSumR += weight * tapR;
            nearSumG += weight * tapG;
            nearSumB += weight * tapB;
            nearWeight += weight;
        }
        else
        {
            farSumR += weight * tapR;
            farSumG += weight * tapG;
            farSumB += weight * tapB;
            farWeight += weight;
        }
    }
    nearR = nearWeight > 0.0f ? nearSumR / nearWeight : ownR;
    nearG = nearWeight > 0.0f ? nearSumG / nearWeight : ownG;
    nearB = nearWeight > 0.0f ? nearSumB / nearWeight : ownB;
    farR = farWeight > 0.0f ? farSumR / farWeight : ownR;
    farG = farWeight > 0.0f ? farSumG / farWeight : ownG;
    farB = farWeight > 0.0f ? farSumB / farWeight : ownB;
    (void) ownC;
}

// ---- the device and the frame's buffers ------------------------------------------------------------------------------------------

struct Fixture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> sceneTexture, toneTexture, depthTexture;
    ComPtr<ID3D11ShaderResourceView> sceneView, toneView, depthView;
    ComPtr<ID3D11RenderTargetView> toneRtv;
    Window window { 0.0f, 0.0f, float(kWidth), float(kHeight) };
    float inverse[16] {};
    std::vector<float> depthValues;
    float ndcScale[2] { 2.0f / float(kWidth), -2.0f / float(kHeight) };
    float ndcOffset[2] { -1.0f, 1.0f };

    bool Create()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                     nullptr, &context)))
            return false;
        if (device == nullptr || context == nullptr)
            return false;
        FillInverseProjection(inverse);

        // The scene: the near half's wall colour (with the focus plane's own hot column beside the centre), the far
        // half's sky colour. Alpha 1.0 everywhere.
        std::vector<uint16_t> scene(size_t(kWidth) * kHeight * 4);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const float raw = x == kWidth / 2 ? kHot : (x < kWidth / 2 ? kWall : kSky);
                const size_t at = (size_t(y) * kWidth + x) * 4;
                scene[at + 0] = HalfOf(raw);
                scene[at + 1] = HalfOf(raw);
                scene[at + 2] = HalfOf(raw);
                scene[at + 3] = HalfOf(1.0f);
            }
        // The depth: the centre column is the focus plane, the near half two bands, the far half two bands - the blur
        // has something to mix inside each half, not only at the halves' own boundary.
        depthValues.assign(size_t(kWidth) * kHeight, 0.0f);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                depthValues[size_t(y) * kWidth + x] =
                    DepthOfDistance(x == kWidth / 2 ? kFocus
                                    : (x < kWidth / 2 ? (x < kWidth / 4 ? kNearA : kNearB)
                                                      : (x < 3 * kWidth / 4 ? kFarA : kFarB)));
        if (!CreateTexture(kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, scene, kWidth * 8, sceneTexture, sceneView,
                           nullptr) ||
            !CreateTexture(kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
                           std::vector<uint16_t>(scene.size(), HalfOf(0.0f)), kWidth * 8, toneTexture, toneView, &toneRtv))
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
        return true;
    }

    template <class Element>
    bool CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, const std::vector<Element>& pixels,
                       uint32_t rowPitch, ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11ShaderResourceView>& view,
                       ComPtr<ID3D11RenderTargetView>* rtv)
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

    Inputs MakeInputs(const Params& params)
    {
        Inputs inputs;
        inputs.scene = sceneView.Get();
        inputs.depth = depthView.Get();
        inputs.sceneFormat = 10; // DXGI_FORMAT_R16G16B16A16_FLOAT
        inputs.sceneWidth = kWidth;
        inputs.sceneHeight = kHeight;
        inputs.depthFormat = 41; // DXGI_FORMAT_R32_FLOAT
        inputs.depthWidth = kWidth;
        inputs.depthHeight = kHeight;
        inputs.window = window;
        std::memcpy(inputs.inverseProjection, inverse, sizeof inputs.inverseProjection);
        inputs.ndcScale[0] = ndcScale[0];
        inputs.ndcScale[1] = ndcScale[1];
        inputs.ndcOffset[0] = ndcOffset[0];
        inputs.ndcOffset[1] = ndcOffset[1];
        inputs.params = params;
        return inputs;
    }

    // Read an RGBA FP16 texture back as referenced bytes (row-major, fours).
    std::vector<uint16_t> ReadRgba(ID3D11ShaderResourceView* view, uint32_t width, uint32_t height)
    {
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        resource.As(&texture);
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        ComPtr<ID3D11Texture2D> staging;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CHECK_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging));
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        CHECK_SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<uint16_t> out(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(out.data() + size_t(y) * width * 4,
                        static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, width * 4 * sizeof(uint16_t));
        context->Unmap(staging.Get(), 0);
        return out;
    }
};
} // namespace

// The first release's look: the old sections pin it (the pack, the blur and the merge of the cinematic look are the shipped code), the new
// sections below pin the distance look.
Params Cine(float strength, float aperture, int32_t steps, const char* focusMode, float focusDistance)
{
    return BuildParams(strength, aperture, steps, focusMode, focusDistance, "cinematic");
}

// ---- the band scene: per-pixel noise over five depth zones (the focus band and the near cap, on the real shaders) -----------------
//
// The existing fixture's scene is flat colour per half, so a blur of it is the same flat colour and cannot tell a pass that blurred from
// one that did not. This scene is hash noise at every pixel (levels 0.125..1.0 in eighths, exact in FP16) over these depth zones, the
// centre column being the focus plane (the auto focus reads it):
//   x   0..39  NearIn   17 m   raw CoC +0.081  inside the band  -> no blur at all, the merge returns the scene byte for byte
//   x  40..79  NearOut   8 m   raw CoC +0.429  beyond the band  -> the CoC the pack carries is held at the near cap (0.25)
//   x      80  Focus    20 m   raw CoC  0      the focus plane
//   x  81..119 FarIn    24 m   raw CoC -0.091  inside the band  -> no blur at all
//   x 120..159 FarOut   60 m   raw CoC -0.500  beyond the band  -> -(0.5 - 0.12) / 0.88 = -0.4318, not capped
// The numbers are typed here, not read from the rules.
namespace band
{
constexpr float kNearIn = 17.0f;
constexpr float kNearOut = 8.0f;
constexpr float kFarIn = 24.0f;
constexpr float kFarOut = 60.0f;
constexpr float kFarOutCoc = -0.38f / 0.88f; // the far side beyond the band: the remapped magnitude, negative, uncapped

enum class Zone
{
    NearIn,
    NearOut,
    Focus,
    FarIn,
    FarOut,
};

Zone ZoneOf(uint32_t x)
{
    if (x == kWidth / 2)
        return Zone::Focus;
    if (x < kWidth / 4)
        return Zone::NearIn;
    if (x < kWidth / 2)
        return Zone::NearOut;
    return x < 3 * kWidth / 4 ? Zone::FarIn : Zone::FarOut;
}

float DepthMetres(Zone zone)
{
    switch (zone)
    {
    case Zone::NearIn: return kNearIn;
    case Zone::NearOut: return kNearOut;
    case Zone::Focus: return kFocus;
    case Zone::FarIn: return kFarIn;
    case Zone::FarOut: return kFarOut;
    }
    return kFocus;
}

// The expected pack alpha of a zone, from the typed numbers above.
float ExpectedAlpha(Zone zone)
{
    switch (zone)
    {
    case Zone::NearOut: return 0.25f;
    case Zone::FarOut: return kFarOutCoc;
    default: return 0.0f;
    }
}

float Level(uint32_t x, uint32_t y, uint32_t channel)
{
    uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ ((channel + 1u) * 83492791u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return 0.125f * float((h & 7u) + 1u);
}

struct Scene
{
    ComPtr<ID3D11Texture2D> sceneTexture, depthTexture;
    ComPtr<ID3D11ShaderResourceView> sceneView, depthView;
    std::vector<uint16_t> sceneBits; // the scene as the pass reads it (FP16 RGBA, row-major)

    bool Create(Fixture& fx)
    {
        sceneBits.assign(size_t(kWidth) * kHeight * 4, 0);
        std::vector<float> depth(size_t(kWidth) * kHeight, 0.0f);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const size_t at = size_t(y) * kWidth + x;
                for (uint32_t c = 0; c < 3; ++c)
                    sceneBits[at * 4 + c] = HalfOf(Level(x, y, c));
                sceneBits[at * 4 + 3] = HalfOf(1.0f);
                depth[at] = DepthOfDistance(DepthMetres(ZoneOf(x)));
            }
        if (!fx.CreateTexture(kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, sceneBits, kWidth * 8, sceneTexture, sceneView, nullptr))
            return false;
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial { depth.data(), kWidth * sizeof(float), 0 };
        return SUCCEEDED(fx.device->CreateTexture2D(&desc, &initial, &depthTexture)) &&
               SUCCEEDED(fx.device->CreateShaderResourceView(depthTexture.Get(), nullptr, &depthView));
    }
};

// The merge's four-texel bilinear read of a quarter target at a window pixel's own coordinate, mirrored (the window is 0,0,160,120).
float QuarterRead(const std::vector<uint16_t>& target, float windowX, float windowY, uint32_t channel)
{
    const float cx = windowX / float(kWidth) * float(kQuarterWidth) - 0.5f;
    const float cy = windowY / float(kHeight) * float(kQuarterHeight) - 0.5f;
    const int bx = int(std::floor(cx)), by = int(std::floor(cy));
    const float fx = std::clamp(cx - float(bx), 0.0f, 1.0f), fy = std::clamp(cy - float(by), 0.0f, 1.0f);
    const auto at = [&](int x, int y) {
        x = std::clamp(x, 0, int(kQuarterWidth) - 1);
        y = std::clamp(y, 0, int(kQuarterHeight) - 1);
        return FromHalf(target[(size_t(y) * kQuarterWidth + size_t(x)) * 4 + channel]);
    };
    const float top = at(bx, by) + (at(bx + 1, by) - at(bx, by)) * fx;
    const float bottom = at(bx, by + 1) + (at(bx + 1, by + 1) - at(bx, by + 1)) * fx;
    return top + (bottom - top) * fy;
}
} // namespace band


// ---- the distance look: a depth ramp on a camera with an infinite far plane (the game's own), over the same noise ---------------------
//
// Columns (the centre cross reads the subject, 9 columns wide so every tap of the focus cross lands in it):
//   x   0..39   6 m    nearer than the focus       -> never blurred
//   x  40..75  15 m    before the start (20 m)     -> not blurred
//   x  76..84  subject (8.4 m, or 40 m, or no depth at all = the sky)
//   x  85..109 40 m    CoC (1 - 20/40) / (1 - 1/8) = 0.5714
//   x 110..134 160 m   the far limit               -> 1
//   x 135..159 1700 m  beyond it                   -> 1
namespace ramp
{
constexpr float kB = 0.1f; // the camera's near plane; with an infinite far plane viewZ = b / depth
constexpr float kNearM = 6.0f, kBeforeStartM = 15.0f, kMidM = 40.0f, kFarLimitM = 160.0f, kBeyondM = 1700.0f;

void FillInverseInfinite(float out[16])
{
    for (uint32_t i = 0; i < 16; ++i)
        out[i] = 0.0f;
    out[0] = kB;
    out[5] = kB;
    out[11] = -kB;
    out[14] = 1.0f;
    out[15] = 0.0f; // an infinite far plane: depth 0 is the sky, and the view distance of depth d is b / d
}

float DepthOf(float metres) { return metres > 0.0f ? kB / metres : 0.0f; }

float ColumnMetres(uint32_t x, float subject)
{
    if (x >= 76 && x <= 84)
        return subject;
    if (x < 40)
        return kNearM;
    if (x < 76)
        return kBeforeStartM;
    if (x < 110)
        return kMidM;
    return x < 135 ? kFarLimitM : kBeyondM;
}

// The expected pack alpha of a column, typed (focus 8.4 m gives the 20 m start and the 160 m far limit).
float ExpectedAlpha(uint32_t x, float start)
{
    if (x >= 76 && x <= 84)
        return 0.0f;
    const float metres = ColumnMetres(x, 8.4f);
    return metres <= start ? 0.0f : -std::fmin(1.0f, (1.0f - start / metres) / (1.0f - 1.0f / 8.0f));
}

struct Scene
{
    ComPtr<ID3D11Texture2D> sceneTexture, depthTexture;
    ComPtr<ID3D11ShaderResourceView> sceneView, depthView;
    std::vector<uint16_t> sceneBits;
    std::vector<float> depth;

    bool Create(Fixture& fx, float subjectMetres)
    {
        sceneBits.assign(size_t(kWidth) * kHeight * 4, 0);
        depth.assign(size_t(kWidth) * kHeight, 0.0f);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const size_t at = size_t(y) * kWidth + x;
                for (uint32_t c = 0; c < 3; ++c)
                    sceneBits[at * 4 + c] = HalfOf(band::Level(x, y, c));
                sceneBits[at * 4 + 3] = HalfOf(1.0f);
                depth[at] = DepthOf(ColumnMetres(x, subjectMetres));
            }
        if (!fx.CreateTexture(kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, sceneBits, kWidth * 8, sceneTexture, sceneView, nullptr))
            return false;
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial { depth.data(), kWidth * sizeof(float), 0 };
        return SUCCEEDED(fx.device->CreateTexture2D(&desc, &initial, &depthTexture)) &&
               SUCCEEDED(fx.device->CreateShaderResourceView(depthTexture.Get(), nullptr, &depthView));
    }

    Inputs MakeInputs(Fixture& fx, const Params& params, float deltaSeconds) const
    {
        Inputs inputs = fx.MakeInputs(params);
        inputs.scene = sceneView.Get();
        inputs.depth = depthView.Get();
        FillInverseInfinite(inputs.inverseProjection);
        inputs.deltaSeconds = deltaSeconds;
        return inputs;
    }
};

// The reference pack of the distance look for a focus: the capped colour and the signed CoC per window pixel, through this camera.
void FillReferencePack(ReferencePack& pack, const Scene& scene, float focus)
{
    float inverse[16];
    FillInverseInfinite(inverse);
    pack.focus = focus;
    pack.coc.assign(size_t(kWidth) * kHeight, 0.0f);
    pack.colour.assign(size_t(kWidth) * kHeight, { 0.0f, 0.0f, 0.0f });
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const size_t at = size_t(y) * kWidth + x;
            const float ndcX = (float(x) + 0.5f) * (2.0f / float(kWidth)) - 1.0f;
            const float ndcY = (float(y) + 0.5f) * (-2.0f / float(kHeight)) + 1.0f;
            const float viewZ = ViewDistanceAtDepth(inverse, ndcX, ndcY, scene.depth[at]);
            pack.coc[at] = DistanceCoCSigned(viewZ, focus);
            for (uint32_t c = 0; c < 3; ++c)
                pack.colour[at][c] = std::fmin(FromHalf(scene.sceneBits[at * 4 + c]), kPackColourCap);
        }
}
} // namespace ramp

// The focus state the distance look keeps (one texel, R = the smoothed focus in metres, G = this run's raw centre sample), read back.
static bool ReadFocusState(Fixture& fx, ID3D11ShaderResourceView* view, float& focus, float& sample)
{
    if (view == nullptr)
        return false;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    resource.As(&texture);
    if (!texture)
        return false;
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    if (desc.Width != 1 || desc.Height != 1 || desc.Format != DXGI_FORMAT_R32G32_FLOAT)
        return false;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(fx.device->CreateTexture2D(&desc, nullptr, &staging)))
        return false;
    fx.context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(fx.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    const float* values = static_cast<const float*>(mapped.pData);
    focus = values[0];
    sample = values[1];
    fx.context->Unmap(staging.Get(), 0);
    return true;
}

int main()
{
    Fixture fx;
    if (!fx.Create())
    {
        std::fflush(stdout); std::fflush(stdout); std::printf("dof_warp: the fixture could not be created\n");
        return 1;
    }
    CHECK_EQ(kQuarterWidth, 40u);
    CHECK_EQ(kQuarterHeight, 30u);
    Pass pass;

    // ---- the auto focus: the pass answers, and the blur targets match the CPU reference --------------------------------------------
    {
        std::fflush(stdout); std::printf("stage: fixtures up (WARP device %p)\n", (void*)fx.device.Get());
        const Params params = Cine(0.8f, 0.05f, 24, "auto", 3.0f);
        const Inputs inputs = fx.MakeInputs(params);
        ID3D11ShaderResourceView* ran = pass.Run(fx.context.Get(), inputs);
        std::fflush(stdout); std::printf("stage: first run done\n");
        CHECK(ran != nullptr);
        if (ran == nullptr)
        {
            std::fflush(stdout); std::printf("dof_warp: the pass did not run (%s)\n", pass.Failure());
            return Finish("dof_warp");
        }
        // The pass made its own copy of the scene and its own pack and quarter targets, one of each size.
        CHECK_EQ(pass.ScenesCreated(), 1u);
        CHECK_EQ(pass.PacksCreated(), 1u);
        CHECK_EQ(pass.QuartersCreated(), 1u);

        ReferencePack pack;
        FillReferencePack(pack, fx.inverse, kFocus, fx.depthValues);
        const float reach = ReachPixels(float(kHeight), params.aperture);
        // A quick look at the pack itself, before the blur's own answer: the CoC at the centre, the near and the far.
        {
            const std::vector<uint16_t> packRead = fx.ReadRgba(pass.PackView(), kWidth, kHeight);
            const auto packAt = [&](uint32_t x, uint32_t y) {
                const size_t at = (size_t(y) * kWidth + x) * 4;
                return std::make_tuple(FromHalf(packRead[at + 0]), FromHalf(packRead[at + 3]));
            };
            auto [c19, a19] = packAt(78, 60);
            auto [c21, a21] = packAt(84, 60);
            std::fflush(stdout); std::printf("  pack(78) rgb=%.3f coc=%.4f  pack(84) rgb=%.3f coc=%.4f (reference %.4f / %.4f)\n", c19, a19, c21,
                        a21, pack.coc[size_t(60) * kWidth + 78], pack.coc[size_t(60) * kWidth + 84]);
        }
        const std::vector<uint16_t> nearTarget = fx.ReadRgba(pass.NearView(), kQuarterWidth, kQuarterHeight);
        const std::vector<uint16_t> farTarget = fx.ReadRgba(pass.FarView(), kQuarterWidth, kQuarterHeight);
        {
            const size_t at = (size_t(15) * kQuarterWidth + 19) * 4; // the quarter pixel whose block centre is x=78, y=62
            const float nR21 = FromHalf(nearTarget[at + 0]), nG21 = FromHalf(nearTarget[at + 1]),
                                     nB21 = FromHalf(nearTarget[at + 2]), nA21 = FromHalf(nearTarget[at + 3]);
            const float fR21 = FromHalf(farTarget[at + 0]), fG21 = FromHalf(farTarget[at + 1]),
                                     fB21 = FromHalf(farTarget[at + 2]), fA21 = FromHalf(farTarget[at + 3]);
            std::fflush(stdout); std::printf("  near(19,15) rgb=(%.3f,%.3f,%.3f) a=%.4f | far(19,15) rgb=(%.3f,%.3f,%.3f) a=%.4f\n", nR21, nG21, nB21,
                        nA21, fR21, fG21, fB21, fA21);
        }
        // The near and far blur against the CPU reference, per pixel, both halves.
        float worstNear = 0.0f, worstFar = 0.0f;
        for (uint32_t qy = 0; qy < kQuarterHeight; ++qy)
            for (uint32_t qx = 0; qx < kQuarterWidth; ++qx)
            {
                float nR = 0.0f, nG = 0.0f, nB = 0.0f, fR = 0.0f, fG = 0.0f, fB = 0.0f;
                ReferenceBlurQuarter(pack, fx.window, qx, qy, reach, params.steps, nR, nG, nB, fR, fG, fB);
                const size_t at = (size_t(qy) * kQuarterWidth + qx) * 4;
                worstNear = std::max(worstNear, std::fabs(FromHalf(nearTarget[at + 0]) - nR));
                worstNear = std::max(worstNear, std::fabs(FromHalf(nearTarget[at + 1]) - nG));
                worstNear = std::max(worstNear, std::fabs(FromHalf(nearTarget[at + 2]) - nB));
                worstFar = std::max(worstFar, std::fabs(FromHalf(farTarget[at + 0]) - fR));
                worstFar = std::max(worstFar, std::fabs(FromHalf(farTarget[at + 1]) - fG));
                worstFar = std::max(worstFar, std::fabs(FromHalf(farTarget[at + 2]) - fB));
            }
        std::fflush(stdout); std::printf("  worstNear=%.6f worstFar=%.6f\n", worstNear, worstFar);
        // Where do the zeros sit? The alpha per row at two quarter columns.
        for (const uint32_t row : { 0u, 7u, 15u, 29u })
            std::fflush(stdout), std::printf("  near a row %2u: q19=%.4f q20=%.4f | far q19=%.4f q20=%.4f\n", row,
                                             FromHalf(nearTarget[(size_t(row) * kQuarterWidth + 19) * 4 + 3]),
                                             FromHalf(nearTarget[(size_t(row) * kQuarterWidth + 20) * 4 + 3]),
                                             FromHalf(farTarget[(size_t(row) * kQuarterWidth + 19) * 4 + 3]),
                                             FromHalf(farTarget[(size_t(row) * kQuarterWidth + 20) * 4 + 3]));
        CHECK(worstNear < 2.0e-3f); // WARP is deterministic; float rounding is the only difference (the march's own bound)
        CHECK(worstFar < 2.0e-3f);
        // Both classes exist in the fixture and are apart: the near half's near target mixed, the far half's far target
        // mixed, and the centre column's own nothing.
        float nearMixMost = 0.0f, farMixLeast = 2.0f;
        for (uint32_t qy = 0; qy < kQuarterHeight; ++qy)
            for (uint32_t qx = 0; qx < kQuarterWidth; ++qx)
            {
                const float x = (float(qx) + 0.5f) * 4.0f;
                if (x == float(kWidth / 2))
                    continue;
                const size_t at = (size_t(qy) * kQuarterWidth + qx) * 4;
                if (x < float(kWidth / 2))
                    nearMixMost = std::max(nearMixMost, FromHalf(nearTarget[at + 0]));
                else
                    farMixLeast = std::min(farMixLeast, FromHalf(farTarget[at + 0]));
            }
        CHECK(nearMixMost > 0.0f);   // the near side has real near blur
        CHECK(farMixLeast < kWall);  // the far half's blur softened the sky's own colour toward the wall's
    }

    std::printf("-- stage: merged section --\n");
    // ---- the merged output: the in-focus column byte-exact, the halves mixed by the pack's own alpha -------------------------------
    {
        const Params params = Cine(0.8f, 0.05f, 24, "auto", 3.0f);
        const Inputs inputs = fx.MakeInputs(params);
        ID3D11ShaderResourceView* ran = pass.Run(fx.context.Get(), inputs);
        CHECK(ran != nullptr);
        const std::vector<uint16_t> merged = fx.ReadRgba(ran, kWidth, kHeight);
        // The centre column: the pack's own zero CoC, so the merge passes the scene's own value byte for byte -
        // including the hot highlight's own above-cap value (the merge reads the scene, never the pack's capped copy).
        for (uint32_t y = 0; y < kHeight; ++y)
        {
            const size_t at = (size_t(y) * kWidth + kWidth / 2) * 4;
            CHECK_EQ(merged[at + 0], HalfOf(kHot));
            CHECK_EQ(merged[at + 3], HalfOf(1.0f));
        }
        // Both halves' pixels mixed by the pack's own alpha, toward the reference halves at the strength the key set.
        const std::vector<uint16_t> nearTarget = fx.ReadRgba(pass.NearView(), kQuarterWidth, kQuarterHeight);
        const std::vector<uint16_t> farTarget = fx.ReadRgba(pass.FarView(), kQuarterWidth, kQuarterHeight);
        (void) nearTarget;
        (void) farTarget;
        // The halves: every pixel's merge stays inside the sources' own range (the blur only mixes the scene's own
        // texels - rule 4), the near half mixes its own two bands, and the far half mixes its own two.
        size_t changed = 0;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth / 2; ++x)
            {
                const size_t at = (size_t(y) * kWidth + x) * 4;
                const float r = FromHalf(merged[at + 0]);
                CHECK(r >= kSky - 2.0e-3f && r <= kWall + 2.0e-3f); // inside the sources' own range
                if (std::fabs(r - kWall) > 1.0e-6f)
                    ++changed;
            }
        CHECK(changed > 0); // the near half really blurred
        changed = 0;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = kWidth / 2 + 1; x < kWidth; ++x)
            {
                const size_t at = (size_t(y) * kWidth + x) * 4;
                const float r = FromHalf(merged[at + 0]);
                CHECK(r >= kSky - 2.0e-3f && r <= kWall + 2.0e-3f); // inside the sources' own range
                if (std::fabs(r - kSky) > 1.0e-6f)
                    ++changed;
            }
        CHECK(changed > 0); // the far half really blurred
    }

    std::printf("-- stage: manual section --\n");
    // ---- the manual focus lands on the key's own metres (the same blur, computed against a reference with that focus) ---------------
    {
        const Params params = Cine(0.8f, 0.05f, 24, "manual", kFocus - 4.0f); // focus 16 m: a real plane change
        const Inputs inputs = fx.MakeInputs(params);
        CHECK(pass.Run(fx.context.Get(), inputs) != nullptr);
        ReferencePack pack;
        FillReferencePack(pack, fx.inverse, kFocus - 4.0f, fx.depthValues);
        const float reach = ReachPixels(float(kHeight), params.aperture);
        const std::vector<uint16_t> nearTarget = fx.ReadRgba(pass.NearView(), kQuarterWidth, kQuarterHeight);
        float worst = 0.0f;
        for (uint32_t qy = 0; qy < kQuarterHeight; ++qy)
            for (uint32_t qx = 0; qx < kQuarterWidth; ++qx)
            {
                float nR = 0.0f, nG = 0.0f, nB = 0.0f, fR = 0.0f, fG = 0.0f, fB = 0.0f;
                ReferenceBlurQuarter(pack, fx.window, qx, qy, reach, params.steps, nR, nG, nB, fR, fG, fB);
                const size_t at = (size_t(qy) * kQuarterWidth + qx) * 4;
                worst = std::max(worst, std::fabs(FromHalf(nearTarget[at + 0]) - nR));
                worst = std::max(worst, std::fabs(FromHalf(nearTarget[at + 1]) - nG));
                worst = std::max(worst, std::fabs(FromHalf(nearTarget[at + 2]) - nB));
            }
        CHECK(worst < 2.0e-3f);
    }

    std::printf("-- stage: knobs section --\n");
    // ---- the steps' and the aperture's own knobs reach the taps (a bigger reach really reaches farther) -----------------------------
    {
        const Params coarse = Cine(0.8f, 0.05f, 16, "auto", kFocus);
        const Inputs coarseInputs = fx.MakeInputs(coarse);
        CHECK(pass.Run(fx.context.Get(), coarseInputs) != nullptr);
        const std::vector<uint16_t> coarseNear = fx.ReadRgba(pass.NearView(), kQuarterWidth, kQuarterHeight);
        const Params wide = Cine(0.8f, kMaxAperture, 24, "auto", kFocus);
        const Inputs wideInputs = fx.MakeInputs(wide);
        CHECK(pass.Run(fx.context.Get(), wideInputs) != nullptr);
        const std::vector<uint16_t> wideNear = fx.ReadRgba(pass.NearView(), kQuarterWidth, kQuarterHeight);
        // The wide aperture's near blur reaches farther than the coarse one at some pixel, so the two runs differ.
        CHECK(coarseNear != wideNear);
        // The pack and the quarter targets were once for this scene size: neither moving keys nor re-runs re-created them.
        CHECK_EQ(pass.ScenesCreated(), 1u);
        CHECK_EQ(pass.PacksCreated(), 1u);
        CHECK_EQ(pass.QuartersCreated(), 1u);
    }

    std::printf("-- stage: band section --\n");
    // ---- the focus band and the near cap, on the real shaders over a textured scene ------------------------------------------------
    {
        band::Scene scene;
        CHECK(scene.Create(fx));
        const float strength = 0.8f;
        const Params params = Cine(strength, 0.05f, 24, "auto", 3.0f);
        Inputs inputs = fx.MakeInputs(params);
        inputs.scene = scene.sceneView.Get();
        inputs.depth = scene.depthView.Get();
        Pass bandPass;
        ID3D11ShaderResourceView* ran = bandPass.Run(fx.context.Get(), inputs);
        CHECK(ran != nullptr);
        if (ran == nullptr)
            return Finish("dof_warp");
        const std::vector<uint16_t> pack = fx.ReadRgba(bandPass.PackView(), kWidth, kHeight);
        const std::vector<uint16_t> merged = fx.ReadRgba(ran, kWidth, kHeight);
        const std::vector<uint16_t> nearTarget = fx.ReadRgba(bandPass.NearView(), kQuarterWidth, kQuarterHeight);
        const std::vector<uint16_t> farTarget = fx.ReadRgba(bandPass.FarView(), kQuarterWidth, kQuarterHeight);

        // 1. The pack's alpha, pixel by pixel, against the typed per-zone numbers: 0 inside the band (and on the focus plane), the
        //    near side held at the cap (0.25, exact in FP16), the far side the remapped magnitude; never past the cap on the near side.
        float alphaMax = -2.0f, alphaMin = 2.0f, worstAlpha = 0.0f;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const float a = FromHalf(pack[(size_t(y) * kWidth + x) * 4 + 3]);
                alphaMax = std::max(alphaMax, a);
                alphaMin = std::min(alphaMin, a);
                worstAlpha = std::max(worstAlpha, std::fabs(a - band::ExpectedAlpha(band::ZoneOf(x))));
            }
        std::printf("  band pack alpha: max %.6f min %.6f worst distance from the typed zone value %.6f\n", alphaMax, alphaMin, worstAlpha);
        CHECK(worstAlpha < 1.0e-3f);
        CHECK(alphaMax <= kNearCap); // the near side never passes the cap...
        CHECK(alphaMax == 0.25f);    // ...and the 8 m plane sits exactly on it
        CHECK(alphaMin >= -1.0f);
        CHECK(alphaMin < -0.4f);     // the far plane beyond the band is still a real CoC
        for (const uint32_t x : { 5u, 30u, 80u, 90u, 110u })
            CHECK_EQ(FromHalf(pack[(size_t(60) * kWidth + x) * 4 + 3]), 0.0f); // inside the band: no CoC (the sign of the zero is not asserted)

        // 2. The merge, pixel by pixel: scene + (the class's quarter target - scene) x (alpha's half x strength). This ties the pack's
        //    shaped alpha to PS_Merge's amounts and to PS_Blur's near and far targets through the real shaders.
        float worstMerge = 0.0f, nearAmountMost = 0.0f, farAmountMost = 0.0f;
        size_t changedNearOut = 0, changedFarOut = 0, changedBand = 0, pixelsBand = 0;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const size_t at = (size_t(y) * kWidth + x) * 4;
                const float alpha = FromHalf(pack[at + 3]);
                const float nearAmount = std::max(alpha, 0.0f) * strength;
                const float farAmount = std::max(-alpha, 0.0f) * strength;
                nearAmountMost = std::max(nearAmountMost, nearAmount);
                farAmountMost = std::max(farAmountMost, farAmount);
                const band::Zone zone = band::ZoneOf(x);
                bool differs = false;
                for (uint32_t c = 0; c < 3; ++c)
                {
                    const float sceneValue = FromHalf(scene.sceneBits[at + c]);
                    float expected = sceneValue;
                    if (nearAmount > 0.0f)
                        expected = sceneValue + (band::QuarterRead(nearTarget, float(x) + 0.5f, float(y) + 0.5f, c) - sceneValue) * nearAmount;
                    else if (farAmount > 0.0f)
                        expected = sceneValue + (band::QuarterRead(farTarget, float(x) + 0.5f, float(y) + 0.5f, c) - sceneValue) * farAmount;
                    worstMerge = std::max(worstMerge, std::fabs(FromHalf(merged[at + c]) - expected));
                    differs = differs || merged[at + c] != scene.sceneBits[at + c];
                }
                CHECK_EQ(merged[at + 3], scene.sceneBits[at + 3]); // the alpha rides through untouched
                if (zone == band::Zone::NearOut && differs)
                    ++changedNearOut;
                if (zone == band::Zone::FarOut && differs)
                    ++changedFarOut;
                if (zone == band::Zone::NearIn || zone == band::Zone::FarIn || zone == band::Zone::Focus)
                {
                    ++pixelsBand;
                    if (differs)
                        ++changedBand;
                }
            }
        std::printf("  band merge: worst distance from scene + (target - scene) x amount %.6f; near amount most %.6f (cap x strength %.6f), far amount most %.6f\n",
                    worstMerge, nearAmountMost, kNearCap * strength, farAmountMost);
        std::printf("  band merge: changed pixels near-out %zu, far-out %zu; inside the band/focus %zu of %zu\n", changedNearOut, changedFarOut, changedBand, pixelsBand);
        CHECK(worstMerge < 2.5e-3f);
        CHECK(nearAmountMost <= kNearCap * strength + 1.0e-6f); // the near blur amount never exceeds the cap times the strength...
        CHECK(nearAmountMost > kNearCap * strength - 1.0e-6f);  // ...and reaches it on the 8 m plane
        CHECK(farAmountMost > 0.3f);                             // the far side is not capped to it

        // 3. Inside the band and on the focus plane the output is the scene byte for byte (every channel of every pixel), on a scene
        //    whose pixels all differ from their neighbours, so a blur of any amount would show.
        CHECK_EQ(changedBand, size_t(0));
        CHECK_EQ(pixelsBand, size_t(kHeight) * (kWidth / 4 + 1 + (kWidth / 4 - 1)));
        // 4. Beyond the band both sides are still blurred: the near plane (capped) and the far plane (uncapped).
        CHECK(changedNearOut > size_t(kHeight) * 5);
        CHECK(changedFarOut > size_t(kHeight) * kWidth / 4 / 2);
    }

    std::printf("-- stage: strength0 section --\n");
    // ---- Strength 0: no draws, and the copy's byte-exact answer --------------------------------------------------------------------
    {
        const Params off = Cine(0.0f, 0.05f, 24, "auto", kFocus);
        const Inputs inputs = fx.MakeInputs(off);
        ID3D11ShaderResourceView* ran = pass.Run(fx.context.Get(), inputs);
        CHECK(ran != nullptr);
        CHECK_EQ(pass.DrawsLastRun(), 0u); // the merge at that key would mix nothing: no draw at all is the answer
        const std::vector<uint16_t> copy = fx.ReadRgba(ran, kWidth, kHeight);
        std::vector<uint16_t> scene(size_t(kWidth) * kHeight * 4);
        size_t differing = 0;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const size_t at = (size_t(y) * kWidth + x) * 4;
                const float raw = x == kWidth / 2 ? kHot : (x < kWidth / 2 ? kWall : kSky);
                scene[at + 0] = HalfOf(raw);
                scene[at + 1] = HalfOf(raw);
                scene[at + 2] = HalfOf(raw);
                scene[at + 3] = HalfOf(1.0f);
                if (copy[at] != scene[at])
                {
                    if (differing < 4)
                        std::printf("  copy(%u,%u) got %.4f expected %.4f\n", x, y, FromHalf(copy[at]), raw);
                    ++differing;
                }
            }
        CHECK(differing == 0); // the identity at Strength 0 is one CopyResource, byte for byte
    }

    std::printf("-- stage: cap section --\n");
    // ---- the pack's cap: a highlight above 10 cannot smears through the taps --------------------------------------------------------
    {
        const Params params = Cine(0.8f, kMaxAperture, 48, "auto", kFocus); // the widest, deepest blur of them all
        const Inputs inputs = fx.MakeInputs(params);
        CHECK(pass.Run(fx.context.Get(), inputs) != nullptr);
        const std::vector<uint16_t> nearTarget = fx.ReadRgba(pass.NearView(), kQuarterWidth, kQuarterHeight);
        for (uint32_t qy = 0; qy < kQuarterHeight; ++qy)
            for (uint32_t qx = 0; qx < kQuarterWidth; ++qx)
            {
                const size_t at = (size_t(qy) * kQuarterWidth + qx) * 4;
                // No blurred answer carries the hot column's own value through: the pack's cap holds at every tap.
                CHECK(FromHalf(nearTarget[at + 0]) <= kPackColourCap + 2.0e-3f);
                CHECK(FromHalf(nearTarget[at + 1]) <= kPackColourCap + 2.0e-3f);
                CHECK(FromHalf(nearTarget[at + 2]) <= kPackColourCap + 2.0e-3f);
            }
    }

    std::printf("-- stage: bindings section --\n");
    // ---- the game's bindings come back after a run ----------------------------------------------------------------------------------
    {
        ComPtr<ID3D11RenderTargetView> gameTarget = fx.toneRtv;
        ComPtr<ID3D11ShaderResourceView> gameView = fx.sceneView;
        ID3D11RenderTargetView* targets[1] { gameTarget.Get() };
        fx.context->OMSetRenderTargets(1, targets, nullptr);
        ID3D11ShaderResourceView* views[1] { gameView.Get() };
        fx.context->PSSetShaderResources(0, 1, views);
        CHECK(pass.Run(fx.context.Get(), fx.MakeInputs(Cine(0.8f, 0.05f, 24, "auto", kFocus))) != nullptr);
        ID3D11RenderTargetView* back[2] { nullptr, nullptr };
        fx.context->OMGetRenderTargets(2, back, nullptr);
        CHECK(back[0] == gameTarget.Get() && back[1] == nullptr);
        if (back[0] != nullptr)
            back[0]->Release();
        ID3D11ShaderResourceView* backView = nullptr;
        fx.context->PSGetShaderResources(0, 1, &backView);
        CHECK(backView == gameView.Get());
        if (backView != nullptr)
            backView->Release();
    }

    std::printf("-- stage: refusal section --\n");
    // ---- a binding the pass cannot read returns null with a reason ------------------------------------------------------------------
    {
        Pass refused;
        Inputs noDepth = fx.MakeInputs(Cine(0.8f, 0.05f, 24, "auto", kFocus));
        noDepth.depth = nullptr;
        CHECK(refused.Run(fx.context.Get(), noDepth) == nullptr);
        CHECK(refused.Failure() != nullptr);
        Inputs badScene = fx.MakeInputs(Cine(0.8f, 0.05f, 24, "auto", kFocus));
        badScene.sceneFormat = 28; // R8G8B8A8_UNORM: not the FP16 the tone map reads
        CHECK(refused.Run(fx.context.Get(), badScene) == nullptr);
        CHECK(refused.Failure() != nullptr);
        Inputs badDepth = fx.MakeInputs(Cine(0.8f, 0.05f, 24, "auto", kFocus));
        badDepth.depthFormat = 87; // DXGI_FORMAT_D24_UNORM_S8_UINT: the typeless resource's own format, never a view's
        CHECK(refused.Run(fx.context.Get(), badDepth) == nullptr);
        Inputs emptyWindow = fx.MakeInputs(Cine(0.8f, 0.05f, 24, "auto", kFocus));
        emptyWindow.window = Window { 0.0f, 0.0f, 0.0f, 0.0f };
        CHECK(refused.Run(fx.context.Get(), emptyWindow) == nullptr);
        // A scene of another size than the draw measured (the pack's own boundary): the refusal of another pass's binding.
        Inputs otherScene = fx.MakeInputs(Cine(0.8f, 0.05f, 24, "auto", kFocus));
        otherScene.sceneWidth = kWidth + 1;
        CHECK(refused.Run(fx.context.Get(), otherScene) == nullptr);
        CHECK(refused.Failure() != nullptr);
    }

    std::printf("-- stage: distance look section --\n");
    // ---- the distance look through the real shaders: nothing before the start, the capped radius after it, the near side never ------------
    {
        ramp::Scene scene;
        CHECK(scene.Create(fx, 8.4f));
        const float strength = 0.8f;
        const Params params = BuildParams(strength, 0.05f, 24, "auto", 3.0f, "distance");
        CHECK(params.mode == Mode::Distance);
        Pass distancePass;
        const Inputs inputs = scene.MakeInputs(fx, params, 0.0f);
        ID3D11ShaderResourceView* ran = distancePass.Run(fx.context.Get(), inputs);
        CHECK(ran != nullptr);
        if (ran == nullptr)
        {
            std::printf("dof_warp: the distance look did not run (%s)\n", distancePass.Failure());
            return Finish("dof_warp");
        }
        CHECK_EQ(distancePass.DrawsLastRun(), 4u); // the focus update, the pack, the blur and the merge

        // The focus: the first sample is adopted whole (the cross of five taps all read the 8.4 m subject).
        float focus = 0.0f, sample = 0.0f;
        CHECK(ReadFocusState(fx, distancePass.FocusView(), focus, sample));
        std::printf("  focus state after the first run: focus %.4f sample %.4f\n", focus, sample);
        CHECK(std::fabs(focus - 8.4f) < 2.0e-3f);
        CHECK(std::fabs(sample - 8.4f) < 2.0e-3f);

        const std::vector<uint16_t> pack = fx.ReadRgba(distancePass.PackView(), kWidth, kHeight);
        const std::vector<uint16_t> merged = fx.ReadRgba(ran, kWidth, kHeight);
        const std::vector<uint16_t> nearTarget = fx.ReadRgba(distancePass.NearView(), kQuarterWidth, kQuarterHeight);
        const std::vector<uint16_t> farTarget = fx.ReadRgba(distancePass.FarView(), kQuarterWidth, kQuarterHeight);

        // 1. The pack's alpha, column by column, against the typed numbers: exactly no CoC up to the start (20 m) and never a positive one anywhere.
        float alphaMax = -2.0f, alphaMin = 2.0f, worstAlpha = 0.0f;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const float a = FromHalf(pack[(size_t(y) * kWidth + x) * 4 + 3]);
                alphaMax = std::max(alphaMax, a);
                alphaMin = std::min(alphaMin, a);
                worstAlpha = std::max(worstAlpha, std::fabs(a - ramp::ExpectedAlpha(x, 20.0f)));
                if (x < 85)
                    CHECK(a == 0.0f); // 6 m, 15 m and the subject: before the start, no CoC at all (-0 is a zero)
            }
        std::printf("  distance pack alpha: max %.6f min %.6f worst distance from the typed column value %.6f\n", alphaMax, alphaMin, worstAlpha);
        CHECK(worstAlpha < 1.0e-3f);
        CHECK(alphaMax <= 0.0f);       // the near side never blurs: no positive CoC
        CHECK(alphaMin >= -1.0f);      // capped at the full CoC
        CHECK(alphaMin < -0.99f);      // and the far limit really reaches it
        CHECK(std::fabs(FromHalf(pack[(size_t(30) * kWidth + 90) * 4 + 3]) - (-0.5714286f)) < 1.0e-3f); // 40 m, typed

        // 2. The blur targets against the CPU reference at the capped radius (0.75 % of the 120-pixel window = 0.9 px): a radius taken from
        //    the reach key (6 px) would sample other texels of this noise scene and miss by a lot.
        ReferencePack reference;
        ramp::FillReferencePack(reference, scene, 8.4f);
        const float reach = DistanceReachPixels(float(kHeight));
        float worstNear = 0.0f, worstFar = 0.0f;
        for (uint32_t qy = 0; qy < kQuarterHeight; ++qy)
            for (uint32_t qx = 0; qx < kQuarterWidth; ++qx)
            {
                float nR = 0.0f, nG = 0.0f, nB = 0.0f, fR = 0.0f, fG = 0.0f, fB = 0.0f;
                ReferenceBlurQuarter(reference, fx.window, qx, qy, reach, params.steps, nR, nG, nB, fR, fG, fB);
                const size_t at = (size_t(qy) * kQuarterWidth + qx) * 4;
                worstNear = std::max({ worstNear, std::fabs(FromHalf(nearTarget[at + 0]) - nR), std::fabs(FromHalf(nearTarget[at + 1]) - nG),
                                       std::fabs(FromHalf(nearTarget[at + 2]) - nB) });
                worstFar = std::max({ worstFar, std::fabs(FromHalf(farTarget[at + 0]) - fR), std::fabs(FromHalf(farTarget[at + 1]) - fG),
                                      std::fabs(FromHalf(farTarget[at + 2]) - fB) });
            }
        std::printf("  distance blur targets against the reference: near %.6f far %.6f (reach %.3f px)\n", worstNear, worstFar, reach);
        CHECK(worstNear < 2.0e-3f);
        CHECK(worstFar < 2.0e-3f);

        // 3. The merge, pixel by pixel: scene + (far target - scene) x (|alpha| x the effective strength). Before the start the output is the scene
        //    byte for byte; after it, the far-limit columns are mixed by at most the effective strength (0.25 x 0.8) and not more.
        const float effective = EffectiveStrength(Mode::Distance, strength);
        float worstMerge = 0.0f, amountMost = 0.0f;
        size_t changedBefore = 0, pixelsBefore = 0, changedAfter = 0, pixelsAfter = 0;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const size_t at = (size_t(y) * kWidth + x) * 4;
                const float amount = std::max(-FromHalf(pack[at + 3]), 0.0f) * effective;
                amountMost = std::max(amountMost, amount);
                bool differs = false;
                for (uint32_t c = 0; c < 3; ++c)
                {
                    const float sceneValue = FromHalf(scene.sceneBits[at + c]);
                    const float expected = amount > 0.0f ? sceneValue + (band::QuarterRead(farTarget, float(x) + 0.5f, float(y) + 0.5f, c) - sceneValue) * amount
                                                         : sceneValue;
                    worstMerge = std::max(worstMerge, std::fabs(FromHalf(merged[at + c]) - expected));
                    differs = differs || merged[at + c] != scene.sceneBits[at + c];
                }
                CHECK_EQ(merged[at + 3], scene.sceneBits[at + 3]);
                if (x < 85)
                {
                    ++pixelsBefore;
                    changedBefore += differs ? 1 : 0;
                }
                else
                {
                    ++pixelsAfter;
                    changedAfter += differs ? 1 : 0;
                }
            }
        std::printf("  distance merge: worst %.6f, amount most %.4f (0.25 x 0.8 = %.4f), changed before the start %zu of %zu, after %zu of %zu\n", worstMerge,
                    amountMost, 0.25f * 0.8f, changedBefore, pixelsBefore, changedAfter, pixelsAfter);
        CHECK(worstMerge < 2.5e-3f);
        CHECK_EQ(changedBefore, size_t(0));                       // nothing nearer than the start is touched, on a scene a blur of any size would show in
        CHECK(changedAfter > pixelsAfter / 2);                    // and the far columns are blurred
        CHECK(amountMost <= effective + 1.0e-6f && amountMost > effective - 1.0e-4f); // the mix at the limit is the effective strength, no more
        CHECK(distancePass.FocusView() != nullptr);
        CHECK_EQ(distancePass.ScenesCreated(), 1u);
        CHECK_EQ(distancePass.PacksCreated(), 1u);
        CHECK_EQ(distancePass.QuartersCreated(), 1u);
    }

    std::printf("-- stage: distance focus section --\n");
    // ---- the auto focus in time: the first sample snaps, the dt moves it exponentially, the sky holds it, a stale gap adopts ------------------
    {
        ramp::Scene subject, distant, sky;
        CHECK(subject.Create(fx, 8.4f));
        CHECK(distant.Create(fx, 40.0f));
        CHECK(sky.Create(fx, 0.0f)); // depth 0 under the cross: the sky of a reversed-Z camera with an infinite far plane
        const Params params = BuildParams(0.8f, 0.05f, 24, "auto", 3.0f, "distance");
        Pass focusPass;
        float focus = 0.0f, sample = 0.0f;
        const auto run = [&](const ramp::Scene& scene, float dt) {
            ID3D11ShaderResourceView* result = focusPass.Run(fx.context.Get(), scene.MakeInputs(fx, params, dt));
            CHECK(result != nullptr);
            CHECK(ReadFocusState(fx, focusPass.FocusView(), focus, sample));
        };
        run(subject, 0.0f);
        CHECK(std::fabs(focus - 8.4f) < 2.0e-3f);                                    // adopted whole (nothing before)
        run(distant, 0.0f);
        CHECK(std::fabs(focus - 8.4f) < 2.0e-3f && std::fabs(sample - 40.0f) < 2.0e-2f); // no time passed: the focus holds, the sample is the new subject
        run(distant, 0.4f);                                                              // 40 m is a far outlier (> 3 x 8.4): the slow constant, 0.2212 of the way
        const float afterSlow = 8.4f + (40.0f - 8.4f) * (1.0f - std::exp(-0.4f / 1.6f));
        std::printf("  focus after 0.4 s toward a far outlier: %.4f (expected %.4f)\n", focus, afterSlow);
        CHECK(std::fabs(focus - afterSlow) < 5.0e-2f);
        const float held = focus;
        run(subject, 0.4f);                                                          // back to the 8.4 m subject: closer, the fast constant, 63.2 % of the way
        const float afterFast = held + (8.4f - held) * (1.0f - std::exp(-1.0f));
        std::printf("  focus after 0.4 s back to 8.4 m: %.4f (expected %.4f)\n", focus, afterFast);
        CHECK(std::fabs(focus - afterFast) < 5.0e-2f);
        const float beforeSky = focus;
        run(sky, 0.3f);                                                              // no depth under the cross: the sky never moves the focus
        CHECK(sample == 0.0f);
        CHECK(std::fabs(focus - beforeSky) < 1.0e-4f);
        run(distant, 1.5f);                                                              // a gap past the stale limit: the sample is adopted whole
        CHECK(std::fabs(focus - 40.0f) < 5.0e-2f);
        CHECK_EQ(focusPass.DrawsLastRun(), 4u);

        // A manual focus needs no state: three draws, the key's metres decide the start (30 m focus -> 60 m start: the 40 m column is not blurred).
        Pass manualPass;
        const Params manual = BuildParams(0.8f, 0.05f, 24, "manual", 30.0f, "distance");
        ID3D11ShaderResourceView* result = manualPass.Run(fx.context.Get(), subject.MakeInputs(fx, manual, 0.0f));
        CHECK(result != nullptr);
        CHECK_EQ(manualPass.DrawsLastRun(), 3u);
        const std::vector<uint16_t> pack = fx.ReadRgba(manualPass.PackView(), kWidth, kHeight);
        CHECK_EQ(FromHalf(pack[(size_t(30) * kWidth + 90) * 4 + 3]) == 0.0f, true);                      // 40 m < 60 m
        CHECK(std::fabs(FromHalf(pack[(size_t(30) * kWidth + 120) * 4 + 3]) - (-(1.0f - 60.0f / 160.0f) / 0.875f)) < 1.0e-3f); // 160 m: -0.7143
        // The cinematic look is three draws too, and keeps its own shaped CoC.
        Pass cinematicPass;
        result = cinematicPass.Run(fx.context.Get(), subject.MakeInputs(fx, Cine(0.8f, 0.05f, 24, "auto", 3.0f), 0.0f));
        CHECK(result != nullptr);
        CHECK_EQ(cinematicPass.DrawsLastRun(), 3u);
    }

    return Finish("dof_warp");
}
