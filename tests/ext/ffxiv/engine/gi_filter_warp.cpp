// Screen-space light bounce: the outlier control (PS_Clamp) and the edge-aware filter (PS_Atrous0..3) in ext/ffxiv/engine/shaders/Gi.hlsl on the D3D11
// WARP software adapter, the REAL shaders (compiled from Gi.hlsl by prepare_gi.ps1, the pass's own fxc recipe) drawn over irradiance and guide textures
// this test builds itself. Every expectation is decided on values the shaders write, against an independent CPU reference with its numbers typed in
// THIS file (the rules header's constants are pinned against the shader by gi_pins, not read here).
//
// The first version of this file tested the old 5x5 binomial smooth. Its blocks are still here, as the filter's step-1 behaviour on a flat guide (the
// filter on a flat guide at step 1 IS that stencil), because they pin what made the first game run print a diamond lattice:
//   a checker input (even x+y = 1, odd = 0, data everywhere) comes out as one flat 0.5 (the parity classes mix);
//   a seeded random field comes out with correlated neighbours, no checkerboard component, at least the old ring's noise reduction and the
//   independent reference at every texel; a flat field stays flat at every texel, borders and corners included; a tap outside the extent is
//   SKIPPED, not clamped (hand-computed impulse values); a TRACED texel with no hit (alpha above 0, rgb 0) is a valid zero and is averaged in, an
//   UNTRACEABLE one (alpha 0) is skipped as a source and has no answer of its own.
// What the filter adds, and what a wrong filter would break:
//   the later iterations (taps 2, 4 and 8 texels apart) equal the reference at every texel, so the doubling steps are the pass's;
//   a depth edge is kept: a bright near surface beside a dark far one stays bright and dark, where a flat guide mixes them (mutant: the depth test gone);
//   a SLANTED plane is one surface to its far end: a field on a plane whose inverse depth is linear in the texel position is smoothed by the same
//   weights as a flat one (mutant: a plain relative depth test, which kills the wide steps on a floor seen at a grazing angle);
//   a thin object does not leak into the plane beside it: the slope next to a silhouette is the SMALLER one-sided step, which sits on the plane
//   (mutant: always the plus side, which predicts the object's depth for the neighbour and mixes it in);
//   the clamp holds a texel to 16 times the mean luminance of its OTHER traced neighbours plus 0.1 (a 50x outlier is capped to that, the texel itself
//   is not in its own mean, a lone hit under the floor survives, the traced zeros count as zeros, the untraceable neighbours' colours are never
//   read, the data marker passes through, the colour scales as a whole).
#include "check.h"

#include <Gi_Atrous0.h>
#include <Gi_Atrous1.h>
#include <Gi_Atrous2.h>
#include <Gi_Atrous3.h>
#include <Gi_Clamp.h>
#include <Gi_Vs.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;

#define CHECK_SUCCEEDED(call) CHECK(SUCCEEDED(call))

namespace
{
constexpr uint32_t kWidth = 63; // odd on purpose: the pass keeps an odd window's last half pixel
constexpr uint32_t kHeight = 47;
constexpr uint32_t kSlotIrradiance = 6; // the shaders' register t6
constexpr uint32_t kSlotGuide = 7;      // and t7
constexpr float kData = 4.0f;           // the pass's alpha: the rays that produced the estimate (anything above 0 marks data)

// The numbers the shaders carry, typed here so this test does not read the rules header.
constexpr double kRow[5] { 1.0, 4.0, 6.0, 4.0, 1.0 };
constexpr double kSigma = 0.03;
constexpr double kClampRatio = 16.0;
constexpr double kClampFloor = 0.1;
constexpr int kSteps[4] { 1, 2, 4, 8 };

struct Texel
{
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

// ---- half floats (round to nearest even; the targets and the inputs are R16G16B16A16_FLOAT) ----------------------------------------

uint16_t HalfOf(float value)
{
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    if (magnitude > 0x7F800000u)
        return uint16_t(sign | 0x7E00u); // NaN
    if (magnitude >= 0x477FF000u)
        return uint16_t(sign | 0x7C00u); // infinity, or too large for a half
    if (magnitude < 0x38800000u)
    {
        // below the smallest normal half: a denormal, in units of 2^-24, rounded to nearest even
        const uint32_t units = uint32_t(std::lrintf(std::fabs(value) * 16777216.0f));
        return uint16_t(sign | units);
    }
    const uint32_t rounded = (magnitude - 0x38000000u + 0x0FFFu + ((magnitude >> 13) & 1u)) >> 13;
    return uint16_t(sign | rounded);
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

float Rounded(float value) { return FromHalf(HalfOf(value)); }

// ---- the device and the one draw --------------------------------------------------------------------------------------------------

struct Filter
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> clamp;
    ComPtr<ID3D11PixelShader> atrous[4];
    ComPtr<ID3D11RasterizerState> rasterizer;

    bool Init()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)) ||
            device == nullptr)
            return false;
        if (FAILED(device->CreateVertexShader(Gi_Vs_cso, sizeof Gi_Vs_cso, nullptr, &vs)) ||
            FAILED(device->CreatePixelShader(Gi_Clamp_cso, sizeof Gi_Clamp_cso, nullptr, &clamp)) ||
            FAILED(device->CreatePixelShader(Gi_Atrous0_cso, sizeof Gi_Atrous0_cso, nullptr, &atrous[0])) ||
            FAILED(device->CreatePixelShader(Gi_Atrous1_cso, sizeof Gi_Atrous1_cso, nullptr, &atrous[1])) ||
            FAILED(device->CreatePixelShader(Gi_Atrous2_cso, sizeof Gi_Atrous2_cso, nullptr, &atrous[2])) ||
            FAILED(device->CreatePixelShader(Gi_Atrous3_cso, sizeof Gi_Atrous3_cso, nullptr, &atrous[3])))
            return false;
        D3D11_RASTERIZER_DESC raster {};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        return SUCCEEDED(device->CreateRasterizerState(&raster, &rasterizer));
    }

    ComPtr<ID3D11ShaderResourceView> Input(const std::vector<uint16_t>& halves)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init {};
        init.pSysMem = halves.data();
        init.SysMemPitch = kWidth * 4 * sizeof(uint16_t);
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        CHECK_SUCCEEDED(device->CreateTexture2D(&desc, &init, &texture));
        CHECK_SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv));
        return srv;
    }

    // Draws `ps` over the irradiance (t6) and the guide (t7) and returns the target read back.
    std::vector<Texel> Run(ID3D11PixelShader* ps, const std::vector<uint16_t>& irradianceHalves, const std::vector<uint16_t>& guideHalves)
    {
        const ComPtr<ID3D11ShaderResourceView> irradianceSrv = Input(irradianceHalves);
        const ComPtr<ID3D11ShaderResourceView> guideSrv = Input(guideHalves);
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target;
        ComPtr<ID3D11RenderTargetView> targetRtv;
        CHECK_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &target));
        CHECK_SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &targetRtv));

        const float blendFactor[4] {};
        const D3D11_VIEWPORT viewport { 0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f };
        ID3D11ShaderResourceView* views[kSlotGuide + 1] {};
        views[kSlotIrradiance] = irradianceSrv.Get();
        views[kSlotGuide] = guideSrv.Get();
        ID3D11RenderTargetView* rtv = targetRtv.Get();
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        context->RSSetState(rasterizer.Get());
        context->RSSetViewports(1, &viewport);
        context->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->PSSetShaderResources(0, kSlotGuide + 1, views);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* none[kSlotGuide + 1] {};
        context->PSSetShaderResources(0, kSlotGuide + 1, none);
        context->OMSetRenderTargets(0, nullptr, nullptr);

        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        CHECK_SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging));
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        std::vector<Texel> out(size_t(kWidth) * kHeight);
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        {
            for (uint32_t y = 0; y < kHeight; ++y)
            {
                const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<const unsigned char*>(mapped.pData) + size_t(mapped.RowPitch) * y);
                for (uint32_t x = 0; x < kWidth; ++x)
                    out[size_t(y) * kWidth + x] = Texel { FromHalf(row[x * 4]), FromHalf(row[x * 4 + 1]), FromHalf(row[x * 4 + 2]), FromHalf(row[x * 4 + 3]) };
            }
            context->Unmap(staging.Get(), 0);
        }
        else
        {
            CHECK(false);
        }
        return out;
    }
};

std::vector<uint16_t> HalvesOf(const std::vector<Texel>& texels)
{
    std::vector<uint16_t> out(texels.size() * 4);
    for (size_t i = 0; i < texels.size(); ++i)
    {
        out[i * 4] = HalfOf(texels[i].r);
        out[i * 4 + 1] = HalfOf(texels[i].g);
        out[i * 4 + 2] = HalfOf(texels[i].b);
        out[i * 4 + 3] = HalfOf(texels[i].a);
    }
    return out;
}

// The guide: z in the alpha channel (0 = no guide), the other channels 0.
std::vector<uint16_t> GuideHalves(const std::vector<float>& z)
{
    std::vector<uint16_t> out(z.size() * 4, 0);
    for (size_t i = 0; i < z.size(); ++i)
        out[i * 4 + 3] = HalfOf(z[i]);
    return out;
}

size_t At(uint32_t x, uint32_t y) { return size_t(y) * kWidth + x; }

// The interior: the texels whose whole 5x5 window lies inside the extent (no skipped tap), where the stencil's algebra is exact.
bool Interior(uint32_t x, uint32_t y) { return x >= 2 && y >= 2 && x + 2 < kWidth && y + 2 < kHeight; }

// The input as the GPU reads it (every channel through the half float), so the reference and the shader see the same numbers.
std::vector<Texel> AsStored(std::vector<Texel> in)
{
    for (Texel& t : in)
        t = Texel { Rounded(t.r), Rounded(t.g), Rounded(t.b), Rounded(t.a) };
    return in;
}

std::vector<float> ZAsStored(std::vector<float> z)
{
    for (float& v : z)
        v = Rounded(v);
    return z;
}

// ---- the independent references ---------------------------------------------------------------------------------------------------

struct Grid
{
    std::vector<Texel> irradiance;
    std::vector<float> z; // 0 = no guide
};

bool HasData(const Texel& t) { return t.a > 0.0f && t.a == t.a; }

double InverseDepthAt(const Grid& grid, int x, int y)
{
    if (x < 0 || y < 0 || x >= int(kWidth) || y >= int(kHeight))
        return 0.0;
    const float z = grid.z[At(uint32_t(x), uint32_t(y))];
    return z > 0.0f ? 1.0 / double(z) : 0.0;
}

// The slope of the inverse depth along an axis: the smaller one-sided step where both sides have a guide, the one that exists, or 0.
double SlopeOf(double centre, double plus, double minus)
{
    const bool havePlus = plus > 0.0, haveMinus = minus > 0.0;
    const double stepPlus = plus - centre, stepMinus = centre - minus;
    if (havePlus && haveMinus)
        return std::fabs(stepPlus) <= std::fabs(stepMinus) ? stepPlus : stepMinus;
    if (havePlus)
        return stepPlus;
    if (haveMinus)
        return stepMinus;
    return 0.0;
}

// One a-trous iteration's answer at a texel: the typed binomial weights at `step` texels apart, taps outside the extent skipped, taps without data or
// without a guide skipped, each weighed by exp(-|inverse depth - the centre plane's| / (0.03 x the centre's inverse depth)); a centre without data or
// guide has no answer.
Texel ReferenceAtrous(const Grid& grid, uint32_t x, uint32_t y, int step)
{
    const Texel centre = grid.irradiance[At(x, y)];
    const double u0 = InverseDepthAt(grid, int(x), int(y));
    if (!HasData(centre) || !(u0 > 0.0))
        return Texel {};
    const double sx = SlopeOf(u0, InverseDepthAt(grid, int(x) + 1, int(y)), InverseDepthAt(grid, int(x) - 1, int(y)));
    const double sy = SlopeOf(u0, InverseDepthAt(grid, int(x), int(y) + 1), InverseDepthAt(grid, int(x), int(y) - 1));
    double sum[3] {};
    double weight = 0.0;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
        {
            const int ox = dx * step, oy = dy * step;
            const int tx = int(x) + ox, ty = int(y) + oy;
            if (tx < 0 || ty < 0 || tx >= int(kWidth) || ty >= int(kHeight))
                continue;
            const Texel& t = grid.irradiance[At(uint32_t(tx), uint32_t(ty))];
            if (!HasData(t))
                continue;
            const double uq = InverseDepthAt(grid, tx, ty);
            if (!(uq > 0.0))
                continue;
            const double predicted = u0 + sx * double(ox) + sy * double(oy);
            const double w = kRow[dx + 2] * kRow[dy + 2] * std::exp(-std::fabs(uq - predicted) / (kSigma * u0));
            sum[0] += double(t.r) * w;
            sum[1] += double(t.g) * w;
            sum[2] += double(t.b) * w;
            weight += w;
        }
    if (!(weight > 0.0))
        return Texel {};
    return Texel { float(sum[0] / weight), float(sum[1] / weight), float(sum[2] / weight), 1.0f };
}

double Luminance(const Texel& t) { return 0.2126 * double(t.r) + 0.7152 * double(t.g) + 0.0722 * double(t.b); }

// The clamp's answer at a texel: the luminance held to 16 x the mean luminance of the other traced texels of the 5x5 neighbourhood (taps outside the
// extent and taps without data skipped; none left = kept as it is) + 0.1, the colour scaled as a whole, the marker passed through.
Texel ReferenceClamp(const Grid& grid, uint32_t x, uint32_t y)
{
    const Texel centre = grid.irradiance[At(x, y)];
    if (!HasData(centre))
        return centre;
    double sum = 0.0, count = 0.0;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
        {
            if (dx == 0 && dy == 0)
                continue;
            const int tx = int(x) + dx, ty = int(y) + dy;
            if (tx < 0 || ty < 0 || tx >= int(kWidth) || ty >= int(kHeight))
                continue;
            const Texel& t = grid.irradiance[At(uint32_t(tx), uint32_t(ty))];
            if (!HasData(t))
                continue;
            sum += Luminance(t);
            count += 1.0;
        }
    if (!(count > 0.0))
        return centre;
    const double cap = kClampRatio * (sum / count) + kClampFloor;
    const double l = Luminance(centre);
    if (!(l > cap))
        return centre;
    const float scale = float(cap / l);
    return Texel { centre.r * scale, centre.g * scale, centre.b * scale, centre.a };
}

double WorstAgainst(const std::vector<Texel>& got, const std::vector<Texel>& expected, bool interiorOnly = false)
{
    double worst = 0.0;
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            if (interiorOnly && !Interior(x, y))
                continue;
            const Texel& g = got[At(x, y)];
            const Texel& e = expected[At(x, y)];
            worst = std::fmax(worst, std::fmax(std::fabs(double(g.r) - double(e.r)), std::fmax(std::fabs(double(g.g) - double(e.g)), std::fabs(double(g.b) - double(e.b)))));
            worst = std::fmax(worst, std::fabs(double(g.a) - double(e.a)));
        }
    return worst;
}

// A seeded LCG, 24 bits of it as a fraction: the same field on every run.
struct Seeded
{
    uint32_t state;
    explicit Seeded(uint32_t seed) : state(seed) {}
    float Next()
    {
        state = state * 1664525u + 1013904223u;
        return float((state >> 8) & 0xFFFFFFu) / 16777216.0f;
    }
};

// ---- the statistics of one channel over the interior -----------------------------------------------------------------------------

double Mean(const std::vector<double>& v)
{
    double s = 0.0;
    for (double x : v)
        s += x;
    return v.empty() ? 0.0 : s / double(v.size());
}

// Pearson correlation between the channel at a texel and at the texel (dx,dy) away, over interior pairs.
double Correlation(const std::vector<Texel>& image, int dx, int dy)
{
    std::vector<double> a, b;
    for (uint32_t y = 2; y + 2 < kHeight; ++y)
        for (uint32_t x = 2; x + 2 < kWidth; ++x)
        {
            const int nx = int(x) + dx, ny = int(y) + dy;
            if (nx < 2 || ny < 2 || nx + 2 >= int(kWidth) || ny + 2 >= int(kHeight))
                continue;
            a.push_back(double(image[At(x, y)].r));
            b.push_back(double(image[At(uint32_t(nx), uint32_t(ny))].r));
        }
    const double ma = Mean(a), mb = Mean(b);
    double sab = 0.0, saa = 0.0, sbb = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        sab += (a[i] - ma) * (b[i] - mb);
        saa += (a[i] - ma) * (a[i] - ma);
        sbb += (b[i] - mb) * (b[i] - mb);
    }
    return (saa > 0.0 && sbb > 0.0) ? sab / std::sqrt(saa * sbb) : 0.0;
}

double StdDev(const std::vector<Texel>& image)
{
    std::vector<double> v;
    for (uint32_t y = 2; y + 2 < kHeight; ++y)
        for (uint32_t x = 2; x + 2 < kWidth; ++x)
            v.push_back(double(image[At(x, y)].r));
    const double m = Mean(v);
    double s = 0.0;
    for (double x : v)
        s += (x - m) * (x - m);
    return std::sqrt(s / double(v.size()));
}

std::vector<Texel> RandomField(uint32_t seedValue)
{
    Seeded seed(seedValue);
    std::vector<Texel> in(size_t(kWidth) * kHeight);
    for (Texel& t : in)
        t = Texel { 0.05f + 2.0f * seed.Next(), 0.05f + 2.0f * seed.Next(), 0.05f + 2.0f * seed.Next(), kData };
    return AsStored(in);
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Filter filter;
    if (!filter.Init())
    {
        std::printf("gi_filter_warp: no D3D11 WARP device, or the shaders could not be created\n");
        return 1;
    }
    const std::vector<float> flatZ(size_t(kWidth) * kHeight, 5.0f);
    const std::vector<uint16_t> flatGuide = GuideHalves(flatZ);
    ID3D11PixelShader* const step1 = filter.atrous[0].Get();

    // ---- 1. the checker: the parity classes must mix (step 1, flat guide) -----------------------------------------------------------
    // Red is the checker (even x+y = 1, odd = 0), green its complement, blue a flat 0.25; every texel carries data, the odd ones included.
    {
        std::vector<Texel> in(size_t(kWidth) * kHeight);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const bool even = ((x + y) & 1u) == 0u;
                in[At(x, y)] = Texel { even ? 1.0f : 0.0f, even ? 0.0f : 1.0f, 0.25f, kData };
            }
        const std::vector<Texel> out = filter.Run(step1, HalvesOf(in), flatGuide);
        double evenSum = 0.0, oddSum = 0.0, worstFromHalf = 0.0;
        uint32_t evenCount = 0, oddCount = 0;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                if (!Interior(x, y))
                    continue;
                const Texel& t = out[At(x, y)];
                if (((x + y) & 1u) == 0u)
                {
                    evenSum += double(t.r);
                    ++evenCount;
                }
                else
                {
                    oddSum += double(t.r);
                    ++oddCount;
                }
                // Each parity class carries half of the stencil's weight (128 of 256), so the interior is a flat 0.5 in red and green.
                worstFromHalf = std::fmax(worstFromHalf, std::fmax(std::fabs(double(t.r) - 0.5), std::fabs(double(t.g) - 0.5)));
                worstFromHalf = std::fmax(worstFromHalf, std::fabs(double(t.b) - 0.25));
            }
        const double contrast = std::fabs(evenSum / double(evenCount) - oddSum / double(oddCount)); // the input's own contrast is 1.0
        std::printf("gi_filter_warp: checker input, interior mean even %.5f odd %.5f, contrast %.5f of the input's 1.0, worst distance from 0.5 %.5f\n",
                    evenSum / double(evenCount), oddSum / double(oddCount), contrast, worstFromHalf);
        // The dense binomial stencil measures 0.0000 (the half float's rounding of 0.5 is exact); a stencil whose response at the checkerboard
        // frequency is not exactly zero (a 1-2-3-2-1 tent leaves 1.2%) fails here.
        CHECK(contrast < 0.002);
        CHECK(worstFromHalf < 0.002);
    }

    // ---- 2. a seeded random field: correlated neighbours, no checkerboard component, noise reduced, equal to the reference ---------------
    {
        const std::vector<Texel> in = RandomField(0x1234ABCDu);
        const std::vector<Texel> out = filter.Run(step1, HalvesOf(in), flatGuide);
        const Grid grid { in, ZAsStored(flatZ) };
        std::vector<Texel> expected(in.size());
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                expected[At(x, y)] = ReferenceAtrous(grid, x, y, 1);
        const double outX = Correlation(out, 1, 0), outY = Correlation(out, 0, 1);
        const double outD = Correlation(out, 1, 1), outA = Correlation(out, 1, -1);
        const double ratio = StdDev(out) / StdDev(in);
        const double worst = WorstAgainst(out, expected);
        std::printf("gi_filter_warp: random field, output correlation x %.3f y %.3f diagonal %.3f anti-diagonal %.3f, std ratio %.3f, worst distance from the reference %.5f\n",
                    outX, outY, outD, outA, ratio, worst);
        CHECK(outX > 0.5 && outY > 0.5);
        CHECK(outD > 0.4 && outA > 0.4);
        CHECK(ratio < 0.32); // the ring the first stencil used cut the noise to 0.348 of its deviation; the dense stencil is not allowed to smooth less
        CHECK(worst < 0.003);
    }

    // ---- 3. a flat field stays flat at every texel, borders and corners included, at every step -------------------------------------
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        std::vector<Texel> in(size_t(kWidth) * kHeight, Texel { 0.7f, 0.2f, 1.5f, kData });
        const std::vector<Texel> out = filter.Run(filter.atrous[iteration].Get(), HalvesOf(in), flatGuide);
        double worst = 0.0;
        for (const Texel& t : out)
        {
            worst = std::fmax(worst, std::fabs(double(t.r) - double(Rounded(0.7f))));
            worst = std::fmax(worst, std::fabs(double(t.g) - double(Rounded(0.2f))));
            worst = std::fmax(worst, std::fabs(double(t.b) - double(Rounded(1.5f))));
            CHECK_EQ(t.a, 1.0f);
        }
        std::printf("gi_filter_warp: flat field at step %d, worst distance from the input over all %zu texels %.5f\n", kSteps[iteration], out.size(), worst);
        CHECK(worst < 0.002);
    }

    // ---- 4. the later iterations: taps 2, 4 and 8 texels apart equal the reference at every texel ----------------------------------
    for (int iteration = 1; iteration < 4; ++iteration)
    {
        const std::vector<Texel> in = RandomField(0x0BADF00Du + uint32_t(iteration));
        const std::vector<Texel> out = filter.Run(filter.atrous[iteration].Get(), HalvesOf(in), flatGuide);
        const Grid grid { in, ZAsStored(flatZ) };
        std::vector<Texel> expected(in.size());
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                expected[At(x, y)] = ReferenceAtrous(grid, x, y, kSteps[iteration]);
        const double worst = WorstAgainst(out, expected);
        std::printf("gi_filter_warp: random field at step %d, worst distance from the reference %.5f\n", kSteps[iteration], worst);
        CHECK(worst < 0.003);
    }

    // ---- 5. texels without data contribute nothing and have no answer ---------------------------------------------------------------
    {
        // A flat 0.5 field. Scattered holes: alpha 0 with a huge colour, and alpha 0 with a NaN colour (the marker is the pass's own count, never a NaN:
        // fxc without -Gis may fold a NaN compare, so the marker is the only thing the shaders are asked to read as 'no data'); one 7x7 block of
        // alpha-0 texels with a huge colour in it. A texel with data comes out as the field's own 0.5 with alpha 1 (a hole's colour is never read, whatever it is); a hole
        // comes out (0,0,0,0): it has no answer of its own, and the apply never reads it as a source.
        std::vector<Texel> in(size_t(kWidth) * kHeight, Texel { 0.5f, 0.5f, 0.5f, kData });
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const uint32_t n = x + y * kWidth;
                if (n % 7 == 3)
                    in[At(x, y)] = Texel { 60000.0f, 60000.0f, 60000.0f, 0.0f };
                else if (n % 11 == 5)
                    in[At(x, y)] = Texel { NAN, NAN, NAN, 0.0f };
            }
        for (uint32_t y = 20; y <= 26; ++y)
            for (uint32_t x = 20; x <= 26; ++x)
                in[At(x, y)] = Texel { 60000.0f, 60000.0f, 60000.0f, 0.0f };
        const std::vector<Texel> halves = in;
        for (int iteration : { 0, 3 })
        {
            const std::vector<Texel> out = filter.Run(filter.atrous[iteration].Get(), HalvesOf(halves), flatGuide);
            double worst = 0.0;
            uint32_t holes = 0, withData = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    const Texel& src = halves[At(x, y)];
                    const Texel& t = out[At(x, y)];
                    if (!(src.a > 0.0f))
                    {
                        if ((t.r != 0.0f || t.a != 0.0f) && holes < 3)
                            std::printf("gi_filter_warp: hole at %u,%u (source alpha %g, colour %g) reads (%g, %g, %g, %g)\n", x, y, double(src.a), double(src.r),
                                        double(t.r), double(t.g), double(t.b), double(t.a));
                        CHECK_EQ(t.r, 0.0f);
                        CHECK_EQ(t.g, 0.0f);
                        CHECK_EQ(t.b, 0.0f);
                        CHECK_EQ(t.a, 0.0f);
                        ++holes;
                        continue;
                    }
                    CHECK_EQ(t.a, 1.0f);
                    worst = std::fmax(worst, std::fmax(std::fabs(double(t.r) - 0.5), std::fmax(std::fabs(double(t.g) - 0.5), std::fabs(double(t.b) - 0.5))));
                    ++withData;
                }
            std::printf("gi_filter_warp: holes at step %d: worst distance from 0.5 over %u texels with data %.5f, %u holes read back (0,0,0,0)\n",
                        kSteps[iteration], withData, worst, holes);
            CHECK(holes > 100 && withData > 1000);
            CHECK(worst < 0.002);
        }
    }

    // ---- 6. a tap outside the extent is skipped, not clamped ------------------------------------------------------------------------
    // An impulse of 1 on one border line of an otherwise 0 field (data everywhere). Hand-computed from the row 1-4-6-4-1 (sum 16):
    //   on the border line itself only dx = 0, +1, +2 are inside: weights 6 + 4 + 1 = 11 on that axis, the impulse sits at dx = 0 (weight 6);
    //   along the line every row is inside (16 on the other axis, the impulse is the same in every row), so
    //   skipped:  (6 * 16) / (11 * 16) = 0.545454...   clamped (the missing taps read the border texel): (11 * 16) / (16 * 16) = 0.6875
    //   one texel in from the border: dx = -1..+2 are inside (4 + 6 + 4 + 1 = 15), the impulse at dx = -1 (weight 4): (4 * 16) / (15 * 16) = 0.2666...
    {
        const double onLine = 6.0 / 11.0;
        const double oneIn = 4.0 / 15.0;
        struct Case
        {
            const char* name;
            bool vertical; // the impulse is a column (true) or a row (false)
            uint32_t line; // its index
        };
        const Case cases[] { { "left column", true, 0 }, { "right column", true, kWidth - 1 }, { "top row", false, 0 }, { "bottom row", false, kHeight - 1 } };
        for (const Case& c : cases)
        {
            std::vector<Texel> in(size_t(kWidth) * kHeight, Texel { 0.0f, 0.0f, 0.0f, kData });
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                    if ((c.vertical ? x : y) == c.line)
                        in[At(x, y)] = Texel { 1.0f, 1.0f, 1.0f, kData };
            const std::vector<Texel> out = filter.Run(step1, HalvesOf(in), flatGuide);
            double worstOnLine = 0.0, worstOneIn = 0.0;
            const uint32_t length = c.vertical ? kHeight : kWidth;
            for (uint32_t i = 2; i + 2 < length; ++i) // along the line, away from the corners (the other axis is fully inside there)
            {
                const uint32_t x0 = c.vertical ? c.line : i, y0 = c.vertical ? i : c.line;
                const uint32_t lineBefore = c.line == 0 ? 1 : c.line - 1;
                const uint32_t x1 = c.vertical ? lineBefore : i, y1 = c.vertical ? i : lineBefore;
                worstOnLine = std::fmax(worstOnLine, std::fabs(double(out[At(x0, y0)].r) - onLine));
                worstOneIn = std::fmax(worstOneIn, std::fabs(double(out[At(x1, y1)].r) - oneIn));
            }
            std::printf("gi_filter_warp: border impulse, %s: worst distance from the skipped answer %.5f on the line (%.5f), %.5f one texel in (%.5f)\n", c.name,
                        worstOnLine, onLine, worstOneIn, oneIn);
            CHECK(worstOnLine < 0.003);
            CHECK(worstOneIn < 0.003);
        }
    }

    // ---- 7. a traced texel with no hit is a valid zero; an untraceable one is skipped as a source and has no answer -------------------
    {
        // A row-constant field (every interior row the same, so the 5x5 result equals the 1-4-6-4-1 row alone, hand-computed from sum 16):
        //   x <  20  a hit            (rgb 1, alpha kData)
        //   x 20..23 traced, no hit   (rgb 0, alpha kData: a valid zero)
        //   x >= 24  untraceable      (alpha 0, with a huge colour behind it that must never be read)
        std::vector<Texel> in(size_t(kWidth) * kHeight);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                in[At(x, y)] = x < 20 ? Texel { 1.0f, 1.0f, 1.0f, kData }
                                      : (x < 24 ? Texel { 0.0f, 0.0f, 0.0f, kData } : Texel { 60000.0f, 60000.0f, 60000.0f, 0.0f });
        const std::vector<Texel> out = filter.Run(step1, HalvesOf(in), flatGuide);
        struct Row
        {
            uint32_t x;
            double expected;
            float alpha;
            const char* why;
        };
        const Row rows[] {
            { 17, 1.0, 1.0f, "taps 15..19 all hits" },
            { 18, 15.0 / 16.0, 1.0f, "taps 16..20: 1,1,1,1,0 -> (1+4+6+4)/16" },
            { 19, 11.0 / 16.0, 1.0f, "taps 17..21: 1,1,1,0,0 -> (1+4+6)/16: the traced zeros weigh in" },
            { 20, 5.0 / 16.0, 1.0f, "taps 18..22: 1,1,0,0,0 -> (1+4)/16" },
            { 21, 1.0 / 16.0, 1.0f, "taps 19..23: 1,0,0,0,0 -> 1/16" },
            { 22, 0.0, 1.0f, "taps 20..24: zeros, and the untraceable tap at 24 skipped" },
            { 23, 0.0, 1.0f, "taps 21..25: three traced zeros, two untraceable skipped (weights present 1+4+6)" },
            { 24, 0.0, 0.0f, "an untraceable texel has no answer of its own, however many traced texels are in its reach" },
            { 25, 0.0, 0.0f, "same: no answer" },
            { 26, 0.0, 0.0f, "same: no answer" },
        };
        double worst = 0.0;
        for (const Row& r : rows)
            for (uint32_t y = 2; y + 2 < kHeight; ++y)
            {
                const Texel& t = out[At(r.x, y)];
                worst = std::fmax(worst, std::fabs(double(t.r) - r.expected));
                if (t.a != r.alpha || std::fabs(double(t.r) - r.expected) > 0.003)
                {
                    std::printf("gi_filter_warp: traced-zero row x=%u y=%u: got %.5f alpha %.1f, expected %.5f alpha %.1f (%s)\n", r.x, y, double(t.r),
                                double(t.a), r.expected, double(r.alpha), r.why);
                    CHECK(false);
                    break;
                }
            }
        std::printf("gi_filter_warp: traced zeros next to hits and untraceable texels, worst distance from the hand-computed rows %.5f\n", worst);
    }
    {
        // A lone hit among traced misses (alpha kData everywhere, rgb 0 except one texel of 1): the hit is shared over its 25 taps with its binomial
        // weight (36/256 at its own texel, 24/256 at a direct neighbour, 1/256 at the outer corner), nothing outside its reach, and every texel keeps its
        // data (alpha 1). A filter that skipped the zeros would hand the whole 1.0 to all 25 taps.
        std::vector<Texel> in(size_t(kWidth) * kHeight, Texel { 0.0f, 0.0f, 0.0f, kData });
        in[At(30, 22)] = Texel { 1.0f, 1.0f, 1.0f, kData };
        const std::vector<Texel> out = filter.Run(step1, HalvesOf(in), flatGuide);
        struct Probe
        {
            int dx, dy;
            double expected;
        };
        const Probe probes[] { { 0, 0, 36.0 / 256.0 }, { 1, 0, 24.0 / 256.0 }, { 0, -1, 24.0 / 256.0 }, { 1, 1, 16.0 / 256.0 }, { 2, 0, 6.0 / 256.0 },
                               { 2, 2, 1.0 / 256.0 }, { 3, 0, 0.0 }, { -3, -3, 0.0 } };
        double worst = 0.0;
        for (const Probe& p : probes)
        {
            const Texel& t = out[At(uint32_t(30 + p.dx), uint32_t(22 + p.dy))];
            worst = std::fmax(worst, std::fabs(double(t.r) - p.expected));
            CHECK(std::fabs(double(t.r) - p.expected) < 0.0005);
            CHECK_EQ(t.a, 1.0f);
        }
        std::printf("gi_filter_warp: a lone hit among traced misses, worst distance from the weights %.5f (own texel %.5f, bound 36/256)\n", worst,
                    double(out[At(30, 22)].r));
        CHECK(out[At(30, 22)].r < 0.15f);
    }

    // ---- 8. a depth edge is kept -----------------------------------------------------------------------------------------------------
    {
        // Left of x = 31 a near surface (z 5, inverse depth 0.2) carrying 1, right of it a far one (z 10, inverse depth 0.1) carrying 0, all traced.
        // The edge texel's neighbours across the step are 0.1 off its plane: 3.3 sigma x 5, so their weight is ~1e-7 and nothing crosses at any step. With
        // a flat guide the same field is mixed at the edge (0.6875 at x = 30 after one iteration): the test cannot pass for lack of an edge.
        std::vector<Texel> in(size_t(kWidth) * kHeight);
        std::vector<float> z(size_t(kWidth) * kHeight);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const bool isNear = x < 31;
                in[At(x, y)] = isNear ? Texel { 1.0f, 1.0f, 1.0f, kData } : Texel { 0.0f, 0.0f, 0.0f, kData };
                z[At(x, y)] = isNear ? 5.0f : 10.0f;
            }
        const std::vector<uint16_t> guide = GuideHalves(z);
        const std::vector<Texel> flat = filter.Run(step1, HalvesOf(in), flatGuide);
        CHECK(flat[At(30, 20)].r < 0.9f && flat[At(31, 20)].r > 0.1f);
        for (int iteration : { 0, 1, 3 })
        {
            const std::vector<Texel> out = filter.Run(filter.atrous[iteration].Get(), HalvesOf(in), guide);
            double worstNear = 0.0, worstFar = 0.0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    const double expected = x < 31 ? 1.0 : 0.0;
                    (x < 31 ? worstNear : worstFar) = std::fmax(x < 31 ? worstNear : worstFar, std::fabs(double(out[At(x, y)].r) - expected));
                }
            std::printf("gi_filter_warp: depth edge at step %d: near side off by %.6f, far side off by %.6f (a flat guide leaves %.4f at the edge texel)\n",
                        kSteps[iteration], worstNear, worstFar, double(flat[At(30, 20)].r));
            CHECK(worstNear < 0.002 && worstFar < 0.002);
        }
    }

    // ---- 9. a slanted plane is one surface to its far end ---------------------------------------------------------------------------
    {
        // The inverse depth of a plane is linear in the texel position: u = 0.2 + 0.0015 x + 0.0008 y (z about 5 down to 4). The slope measured at a
        // texel predicts the plane out to 16 texels, so the field is smoothed by the same weights as on a flat guide. A plain relative depth test sees a
        // 0.024 step across 16 texels (4 sigma) and starves the wide iterations.
        std::vector<float> z(size_t(kWidth) * kHeight);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                z[At(x, y)] = 1.0f / (0.2f + 0.0015f * float(x) + 0.0008f * float(y));
        const std::vector<float> stored = ZAsStored(z);
        const std::vector<uint16_t> guide = GuideHalves(z);
        for (int iteration = 0; iteration < 4; ++iteration)
        {
            const std::vector<Texel> in = RandomField(0xC0FFEE00u + uint32_t(iteration));
            const std::vector<Texel> out = filter.Run(filter.atrous[iteration].Get(), HalvesOf(in), guide);
            const Grid grid { in, stored };
            std::vector<Texel> expected(in.size());
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                    expected[At(x, y)] = ReferenceAtrous(grid, x, y, kSteps[iteration]);
            const double worst = WorstAgainst(out, expected);
            // The same field through a flat guide, for the weights' own size: the slanted answer must be as smooth.
            const std::vector<Texel> flatOut = filter.Run(filter.atrous[iteration].Get(), HalvesOf(in), flatGuide);
            const double ratioSlanted = StdDev(out) / StdDev(in), ratioFlat = StdDev(flatOut) / StdDev(in);
            std::printf("gi_filter_warp: slanted plane at step %d: worst distance from the reference %.5f, noise kept %.3f (flat guide %.3f)\n", kSteps[iteration],
                        worst, ratioSlanted, ratioFlat);
            CHECK(worst < 0.004);
            CHECK(ratioSlanted < ratioFlat * 1.15 + 0.01);
        }
    }

    // ---- 10. a thin object does not leak into the plane beside it ----------------------------------------------------------------
    {
        // A plane at z 5 with a two-texel wide strip (x 30..31) at z 2.5 carrying 1, everything else carrying 0. The plane texel just left of the strip
        // (x = 29) has the strip on its plus side and the plane on its minus side: the slope is the smaller step (0, the plane's), so the strip's inverse
        // depth (0.4) is 0.2 off the predicted one and the strip weighs nothing. A slope taken always from the plus side predicts 0.4 there and mixes it in.
        std::vector<Texel> in(size_t(kWidth) * kHeight, Texel { 0.0f, 0.0f, 0.0f, kData });
        std::vector<float> z(size_t(kWidth) * kHeight, 5.0f);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 30; x <= 31; ++x)
            {
                in[At(x, y)] = Texel { 1.0f, 1.0f, 1.0f, kData };
                z[At(x, y)] = 2.5f;
            }
        const std::vector<Texel> out = filter.Run(step1, HalvesOf(in), GuideHalves(z));
        const Grid grid { AsStored(in), ZAsStored(z) };
        std::vector<Texel> expected(in.size());
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                expected[At(x, y)] = ReferenceAtrous(grid, x, y, 1);
        const double worst = WorstAgainst(out, expected);
        std::printf("gi_filter_warp: thin object: the plane texels beside it read %.6f / %.6f / %.6f / %.6f, the strip %.5f, worst distance from the reference %.5f\n",
                    double(out[At(28, 20)].r), double(out[At(29, 20)].r), double(out[At(32, 20)].r), double(out[At(33, 20)].r), double(out[At(30, 20)].r), worst);
        CHECK(worst < 0.003);
        for (uint32_t x : { 28u, 29u, 32u, 33u })
            CHECK(out[At(x, 20)].r < 0.005f);
        CHECK(out[At(30, 20)].r > 0.995f && out[At(31, 20)].r > 0.995f);
    }

    // ---- 11. the clamp ----------------------------------------------------------------------------------------------------------------
    {
        ID3D11PixelShader* const clampPs = filter.clamp.Get();
        // 11a. A flat 0.02 field with one 50x outlier (luminance about 1.0, a warm colour): held to 16 x 0.02 + 0.1 = 0.42, hue kept, neighbours
        // untouched, the marker passed through. The texel is not in its own mean: with it in, the cap would be 16 x (24 x 0.02 + 1.0) / 25 + 0.1 = 1.0 and
        // nothing would be clamped.
        {
            std::vector<Texel> in(size_t(kWidth) * kHeight, Texel { 0.02f, 0.02f, 0.02f, kData });
            const Texel outlier { 1.7f, 0.85f, 0.425f, kData }; // luminance 1.0
            in[At(30, 22)] = outlier;
            const std::vector<Texel> halves = AsStored(in);
            const std::vector<Texel> out = filter.Run(clampPs, HalvesOf(in), flatGuide);
            const Grid grid { halves, ZAsStored(flatZ) };
            std::vector<Texel> expected(in.size());
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                    expected[At(x, y)] = ReferenceClamp(grid, x, y);
            const double worst = WorstAgainst(out, expected);
            const Texel& capped = out[At(30, 22)];
            const double lum = Luminance(capped);
            std::printf("gi_filter_warp: clamp, a 50x outlier: luminance %.4f (cap 0.42), hue r/g %.3f g/b %.3f, neighbour %.4f, worst distance from the reference %.5f\n",
                        lum, double(capped.r / capped.g), double(capped.g / capped.b), double(out[At(31, 22)].r), worst);
            CHECK(worst < 0.002);
            CHECK(std::fabs(lum - 0.42) < 0.004);
            CHECK(std::fabs(double(capped.r / capped.g) - 2.0) < 0.01 && std::fabs(double(capped.g / capped.b) - 2.0) < 0.01);
            CHECK_EQ(capped.a, kData);
            CHECK(std::fabs(double(out[At(31, 22)].r) - 0.02) < 0.0005 && std::fabs(double(out[At(30, 23)].r) - 0.02) < 0.0005);
            CHECK(std::fabs(double(out[At(5, 5)].r) - 0.02) < 0.0005);
        }
        // 11b. Traced zeros with lone hits: one under the floor (luminance 0.08) survives, one over it (0.5) is held to the floor, 0 x 16 + 0.1.
        {
            std::vector<Texel> in(size_t(kWidth) * kHeight, Texel { 0.0f, 0.0f, 0.0f, kData });
            in[At(15, 15)] = Texel { 0.08f, 0.08f, 0.08f, kData };
            in[At(45, 30)] = Texel { 0.5f, 0.5f, 0.5f, kData };
            const std::vector<Texel> out = filter.Run(clampPs, HalvesOf(in), flatGuide);
            std::printf("gi_filter_warp: clamp, lone hits among traced zeros: 0.08 reads %.4f, 0.5 reads %.4f (the floor 0.1)\n", double(out[At(15, 15)].r),
                        double(out[At(45, 30)].r));
            CHECK(std::fabs(double(out[At(15, 15)].r) - 0.08) < 0.0005);
            CHECK(std::fabs(double(out[At(45, 30)].r) - 0.1) < 0.0007);
        }
        // 11c. Untraceable neighbours are skipped, their colours never read: the same 50x outlier among 0.02 texels, half of the neighbours replaced by
        // untraceable texels with a huge colour. The mean of the traced ones is still 0.02 and the outlier is still held to 0.42.
        {
            std::vector<Texel> in(size_t(kWidth) * kHeight, Texel { 0.02f, 0.02f, 0.02f, kData });
            for (uint32_t y = 20; y <= 24; ++y)
                for (uint32_t x = 28; x <= 32; ++x)
                    if (((x + y) & 1u) != 0u)
                        in[At(x, y)] = Texel { 60000.0f, 60000.0f, 60000.0f, 0.0f };
            in[At(30, 22)] = Texel { 1.7f, 0.85f, 0.425f, kData };
            const std::vector<Texel> out = filter.Run(clampPs, HalvesOf(in), flatGuide);
            const Grid grid { AsStored(in), ZAsStored(flatZ) };
            const double worst = WorstAgainst(out, [&] {
                std::vector<Texel> e(in.size());
                for (uint32_t y = 0; y < kHeight; ++y)
                    for (uint32_t x = 0; x < kWidth; ++x)
                        e[At(x, y)] = ReferenceClamp(grid, x, y);
                return e;
            }());
            std::printf("gi_filter_warp: clamp, untraceable neighbours: the outlier reads luminance %.4f (cap 0.42), worst distance from the reference %.5f\n",
                        Luminance(out[At(30, 22)]), worst);
            CHECK(worst < 0.002);
            CHECK(std::fabs(Luminance(out[At(30, 22)]) - 0.42) < 0.004);
        }
        // 11d. A traced texel with no traced neighbour has nothing to be compared with and is kept; a texel without data is returned as it is.
        {
            std::vector<Texel> in(size_t(kWidth) * kHeight, Texel { 0.0f, 0.0f, 0.0f, 0.0f });
            in[At(30, 22)] = Texel { 5.0f, 5.0f, 5.0f, kData };
            const std::vector<Texel> out = filter.Run(clampPs, HalvesOf(in), flatGuide);
            std::printf("gi_filter_warp: clamp, an isolated traced texel keeps %.4f; a texel without data reads (%.1f, %.1f, %.1f, %.1f)\n", double(out[At(30, 22)].r),
                        double(out[At(5, 5)].r), double(out[At(5, 5)].g), double(out[At(5, 5)].b), double(out[At(5, 5)].a));
            CHECK(out[At(30, 22)].r == 5.0f && out[At(30, 22)].a == kData);
            CHECK(out[At(5, 5)].r == 0.0f && out[At(5, 5)].a == 0.0f);
        }
        // 11e. The reference on a random field with a few outliers, corners and borders included (a tap outside the extent is skipped).
        {
            std::vector<Texel> in = RandomField(0x5EED5EEDu);
            for (Texel& t : in)
                t = Texel { t.r * 0.02f, t.g * 0.02f, t.b * 0.02f, kData };
            in[At(0, 0)] = Texel { 3.0f, 3.0f, 3.0f, kData };
            in[At(kWidth - 1, kHeight - 1)] = Texel { 3.0f, 2.0f, 1.0f, kData };
            in[At(31, 0)] = Texel { 2.0f, 2.0f, 2.0f, kData };
            in[At(10, 30)] = Texel { 4.0f, 4.0f, 4.0f, kData };
            const std::vector<Texel> out = filter.Run(clampPs, HalvesOf(in), flatGuide);
            const Grid grid { AsStored(in), ZAsStored(flatZ) };
            std::vector<Texel> expected(in.size());
            uint32_t clamped = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    expected[At(x, y)] = ReferenceClamp(grid, x, y);
                    clamped += expected[At(x, y)].r != grid.irradiance[At(x, y)].r ? 1u : 0u;
                }
            const double worst = WorstAgainst(out, expected);
            std::printf("gi_filter_warp: clamp on a random field with four outliers, %u texels clamped, worst distance from the reference %.5f\n", clamped, worst);
            CHECK(clamped >= 4);
            CHECK(worst < 0.002);
        }
    }

    return Finish("gi_filter_warp");
}
