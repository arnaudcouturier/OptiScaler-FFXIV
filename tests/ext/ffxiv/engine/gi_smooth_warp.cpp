// Screen-space light bounce: the irradiance smooth (PS_Smooth in ext/ffxiv/engine/shaders/Gi.hlsl) on the D3D11 WARP software adapter, the REAL
// shader (compiled from Gi.hlsl by prepare_gi.ps1, the pass's own fxc recipe) drawn over irradiance textures this test builds itself.
//
// Why a test of its own: the first game run of the light bounce showed a fine diamond lattice (a checkerboard rotated 45 degrees at about one
// irradiance texel) on walls, floor and bed. The old stencil's tap offsets were written as (+-2,0), (0,+-2) and (+-1.5,+-1.5) and read through
// int(), so the diagonals landed on (+-1,+-1) and EVERY tap had an even x+y: a texel never mixed with a neighbour of the other checkerboard
// parity, the two parity classes were filtered as two independent noise fields and interleaved. The CPU reference that sat beside the shader
// truncated the offsets the same way, so no host test could see it. This test therefore decides on VALUES the shader writes, with no table of
// the rules header involved:
//   a checker input (even x+y = 1, odd = 0, data everywhere): the smoothed parity classes must come out equal (the old stencil keeps them at
//   1 and 0, a contrast of 100%; the dense binomial stencil gives 0.5 and 0.5);
//   a seeded random field: neighbouring output texels are correlated (the old stencil's horizontal and vertical neighbours are independent,
//   correlation about -0.16 on this field), the noise is reduced at least as much as the old ring did, and every output texel equals an
//   independent CPU reference built from the binomial weights typed in this file;
//   a flat field stays flat at every texel, borders and corners included (the weight sum divides out whatever taps were skipped);
//   texels without data (alpha 0 or NaN, with huge or NaN colour behind them) contribute nothing, a texel with no data around it at all comes
//   out (0,0,0,0), and a texel with data in reach comes out with alpha 1;
//   a one-texel-wide impulse on each border: a tap outside the extent is SKIPPED, not clamped (hand-computed values below);
//   a TRACED texel with no hit is a valid zero (alpha above 0, rgb 0) and is averaged in: a lone hit among traced misses is shared over its
//   25 taps, not carried whole; an UNTRACEABLE texel (alpha 0) is skipped and never lowers its neighbours (hand-computed rows below). The
//   smooth's code is the same for both; what changed is which texels the irradiance pass marks as data (gi_warp decides that through the
//   real PS_Gi), so this block guards the smooth's half of the contract.
#include "check.h"

#include <Gi_Smooth.h>
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
constexpr uint32_t kSlotIrradiance = 6; // the shader's register t6
constexpr float kData = 4.0f;           // the pass's alpha: the rays that produced the estimate (anything above 0 marks data)

// The binomial row the dense stencil is built from, typed here so this test does not read the rules header: the weight of a tap is the
// product of the row's entries at |dx| and |dy|.
constexpr double kRow[5] { 1.0, 4.0, 6.0, 4.0, 1.0 };

struct Texel
{
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

// ---- half floats (round to nearest even; the target and the input are R16G16B16A16_FLOAT) ----------------------------------------

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

struct Smoother
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11RasterizerState> rasterizer;

    bool Init()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)) ||
            device == nullptr)
            return false;
        if (FAILED(device->CreateVertexShader(Gi_Vs_cso, sizeof Gi_Vs_cso, nullptr, &vs)) ||
            FAILED(device->CreatePixelShader(Gi_Smooth_cso, sizeof Gi_Smooth_cso, nullptr, &ps)))
            return false;
        D3D11_RASTERIZER_DESC raster {};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        return SUCCEEDED(device->CreateRasterizerState(&raster, &rasterizer));
    }

    // Draws PS_Smooth over `input` (width x height texels, row-major) and returns the target read back.
    std::vector<Texel> Run(const std::vector<uint16_t>& inputHalves)
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
        init.pSysMem = inputHalves.data();
        init.SysMemPitch = kWidth * 4 * sizeof(uint16_t);
        ComPtr<ID3D11Texture2D> input;
        ComPtr<ID3D11ShaderResourceView> inputSrv;
        CHECK_SUCCEEDED(device->CreateTexture2D(&desc, &init, &input));
        CHECK_SUCCEEDED(device->CreateShaderResourceView(input.Get(), nullptr, &inputSrv));

        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target;
        ComPtr<ID3D11RenderTargetView> targetRtv;
        CHECK_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &target));
        CHECK_SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &targetRtv));

        const float blendFactor[4] {};
        const D3D11_VIEWPORT viewport { 0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f };
        ID3D11ShaderResourceView* views[kSlotIrradiance + 1] {};
        views[kSlotIrradiance] = inputSrv.Get();
        ID3D11RenderTargetView* rtv = targetRtv.Get();
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->RSSetState(rasterizer.Get());
        context->RSSetViewports(1, &viewport);
        context->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->PSSetShaderResources(0, kSlotIrradiance + 1, views);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* none[kSlotIrradiance + 1] {};
        context->PSSetShaderResources(0, kSlotIrradiance + 1, none);
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

size_t At(uint32_t x, uint32_t y) { return size_t(y) * kWidth + x; }

// The interior: the texels whose whole 5x5 window lies inside the extent (no skipped tap), where the stencil's algebra is exact.
bool Interior(uint32_t x, uint32_t y) { return x >= 2 && y >= 2 && x + 2 < kWidth && y + 2 < kHeight; }

// The independent reference: the typed binomial weights, taps outside the extent skipped, taps without data (alpha not above 0, or NaN)
// skipped, the sum divided by the weights that were present. Returns false (and a zero texel) when nothing was present.
Texel Reference(const std::vector<Texel>& in, uint32_t x, uint32_t y)
{
    double sum[3] {};
    double weight = 0.0;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
        {
            const int tx = int(x) + dx, ty = int(y) + dy;
            if (tx < 0 || ty < 0 || tx >= int(kWidth) || ty >= int(kHeight))
                continue;
            const Texel& t = in[At(uint32_t(tx), uint32_t(ty))];
            if (!(t.a > 0.0f) || t.a != t.a)
                continue;
            const double w = kRow[dx + 2] * kRow[dy + 2];
            sum[0] += double(t.r) * w;
            sum[1] += double(t.g) * w;
            sum[2] += double(t.b) * w;
            weight += w;
        }
    if (!(weight > 0.0))
        return Texel {};
    return Texel { float(sum[0] / weight), float(sum[1] / weight), float(sum[2] / weight), 1.0f };
}

// The input as the GPU reads it (every channel through the half float), so the reference and the shader see the same numbers.
std::vector<Texel> AsStored(std::vector<Texel> in)
{
    for (Texel& t : in)
        t = Texel { Rounded(t.r), Rounded(t.g), Rounded(t.b), Rounded(t.a) };
    return in;
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

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Smoother smoother;
    if (!smoother.Init())
    {
        std::printf("gi_smooth_warp: no D3D11 WARP device, or the shaders could not be created\n");
        return 1;
    }

    // ---- 1. the checker: the parity classes must mix ------------------------------------------------------------------------------
    // Red is the checker (even x+y = 1, odd = 0), green its complement, blue a flat 0.25; every texel carries data, the odd ones included
    // (an odd texel with alpha 0 would test the hole path, not the mixing).
    {
        std::vector<Texel> in(size_t(kWidth) * kHeight);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const bool even = ((x + y) & 1u) == 0u;
                in[At(x, y)] = Texel { even ? 1.0f : 0.0f, even ? 0.0f : 1.0f, 0.25f, kData };
            }
        const std::vector<Texel> out = smoother.Run(HalvesOf(in));
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
        std::printf("gi_smooth_warp: checker input, interior mean even %.5f odd %.5f, contrast %.5f of the input's 1.0, worst distance from 0.5 %.5f\n",
                    evenSum / double(evenCount), oddSum / double(oddCount), contrast, worstFromHalf);
        // The old stencil leaves 1.0 and 0.0 (a contrast of 1.0); the dense binomial stencil measures 0.0000 (the half float's rounding of 0.5 is
        // exact). The bound is 0.2%, far under the 5% of the input's contrast a visible lattice needs and far over the rounding: a stencil whose
        // response at the checkerboard frequency is not exactly zero (a 1-2-3-2-1 tent leaves 1.2%) fails here too.
        CHECK(contrast < 0.002);
        CHECK(worstFromHalf < 0.002);
    }

    // ---- 2. a seeded random field: correlated neighbours, no checkerboard component, noise reduced, equal to the reference --------------
    {
        Seeded seed(0x1234ABCDu);
        std::vector<Texel> in(size_t(kWidth) * kHeight);
        for (Texel& t : in)
            t = Texel { 0.05f + 2.0f * seed.Next(), 0.05f + 2.0f * seed.Next(), 0.05f + 2.0f * seed.Next(), kData };
        in = AsStored(in);
        const std::vector<Texel> out = smoother.Run(HalvesOf(in));
        const double inputCorrelationX = Correlation(in, 1, 0);
        const double outX = Correlation(out, 1, 0), outY = Correlation(out, 0, 1);
        const double outD = Correlation(out, 1, 1), outA = Correlation(out, 1, -1);
        const double ratio = StdDev(out) / StdDev(in);
        double worst = 0.0;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const Texel ref = Reference(in, x, y);
                const Texel& got = out[At(x, y)];
                worst = std::fmax(worst, std::fmax(std::fabs(double(got.r - ref.r)), std::fmax(std::fabs(double(got.g - ref.g)), std::fabs(double(got.b - ref.b)))));
                CHECK_EQ(got.a, 1.0f);
            }
        std::printf("gi_smooth_warp: random field, input lag-1 correlation %.3f; output correlation x %.3f y %.3f diagonal %.3f anti-diagonal %.3f, "
                    "std ratio %.3f, worst distance from the reference %.5f\n",
                    inputCorrelationX, outX, outY, outD, outA, ratio, worst);
        // The old stencil: x and y correlation about 0 (the parity classes are independent fields), diagonal about 0.66.
        CHECK(outX > 0.5 && outY > 0.5);
        CHECK(outD > 0.4 && outA > 0.4);
        // The ring the old stencil used cut the noise to 0.348 of its deviation; the dense stencil is not allowed to smooth less.
        CHECK(ratio < 0.32);
        // The shader against the independent reference, borders included (a skipped tap divides out of the weight sum).
        CHECK(worst < 0.003);
    }

    // ---- 3. a flat field stays flat at every texel, borders and corners included -----------------------------------------------------
    {
        std::vector<Texel> in(size_t(kWidth) * kHeight, Texel { 0.7f, 0.2f, 1.5f, kData });
        const std::vector<Texel> out = smoother.Run(HalvesOf(in));
        double worst = 0.0;
        for (const Texel& t : out)
        {
            worst = std::fmax(worst, std::fabs(double(t.r) - double(Rounded(0.7f))));
            worst = std::fmax(worst, std::fabs(double(t.g) - double(Rounded(0.2f))));
            worst = std::fmax(worst, std::fabs(double(t.b) - double(Rounded(1.5f))));
            CHECK_EQ(t.a, 1.0f);
        }
        std::printf("gi_smooth_warp: flat field, worst distance from the input over all %zu texels %.5f\n", out.size(), worst);
        CHECK(worst < 0.002);
    }

    // ---- 4. texels without data contribute nothing ----------------------------------------------------------------------------------
    {
        // A flat 0.5 field. Scattered holes: alpha 0 with a huge colour, and alpha NaN with a NaN colour. One 7x7 block of alpha-0 texels
        // with a huge colour in it: the 3x3 texels in its middle have no data in their whole 5x5 reach.
        std::vector<Texel> in(size_t(kWidth) * kHeight, Texel { 0.5f, 0.5f, 0.5f, kData });
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const uint32_t n = x + y * kWidth;
                if (n % 7 == 3)
                    in[At(x, y)] = Texel { 60000.0f, 60000.0f, 60000.0f, 0.0f };
                else if (n % 11 == 5)
                    in[At(x, y)] = Texel { NAN, NAN, NAN, NAN };
            }
        for (uint32_t y = 20; y <= 26; ++y)
            for (uint32_t x = 20; x <= 26; ++x)
                in[At(x, y)] = Texel { 60000.0f, 60000.0f, 60000.0f, 0.0f };
        const std::vector<Texel> out = smoother.Run(HalvesOf(in));
        double worst = 0.0;
        uint32_t empties = 0;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const Texel& t = out[At(x, y)];
                const bool deadMiddle = x >= 22 && x <= 24 && y >= 22 && y <= 24;
                if (deadMiddle)
                {
                    CHECK_EQ(t.r, 0.0f);
                    CHECK_EQ(t.g, 0.0f);
                    CHECK_EQ(t.b, 0.0f);
                    CHECK_EQ(t.a, 0.0f);
                    ++empties;
                    continue;
                }
                // Everywhere else some tap has data, whatever the texel's own state, and the answer is the field's own 0.5.
                CHECK_EQ(t.a, 1.0f);
                worst = std::fmax(worst, std::fmax(std::fabs(double(t.r) - 0.5), std::fmax(std::fabs(double(t.g) - 0.5), std::fabs(double(t.b) - 0.5))));
            }
        std::printf("gi_smooth_warp: holes, worst distance from 0.5 outside the dead middle %.5f, dead-middle texels %u of 9\n", worst, empties);
        CHECK_EQ(empties, 9u);
        CHECK(worst < 0.002);
    }

    // ---- 5. a tap outside the extent is skipped, not clamped ----------------------------------------------------------------------
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
            bool vertical;  // the impulse is a column (true) or a row (false)
            uint32_t line;  // its index
        };
        const Case cases[] { { "left column", true, 0 }, { "right column", true, kWidth - 1 }, { "top row", false, 0 }, { "bottom row", false, kHeight - 1 } };
        for (const Case& c : cases)
        {
            std::vector<Texel> in(size_t(kWidth) * kHeight, Texel { 0.0f, 0.0f, 0.0f, kData });
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                    if ((c.vertical ? x : y) == c.line)
                        in[At(x, y)] = Texel { 1.0f, 1.0f, 1.0f, kData };
            const std::vector<Texel> out = smoother.Run(HalvesOf(in));
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
            std::printf("gi_smooth_warp: border impulse, %s: worst distance from the skipped answer %.5f on the line (%.5f), %.5f one texel in (%.5f)\n", c.name,
                        worstOnLine, onLine, worstOneIn, oneIn);
            CHECK(worstOnLine < 0.003);
            CHECK(worstOneIn < 0.003);
        }
    }

    // ---- 6. a traced texel with no hit is a valid zero; an untraceable one is skipped ---------------------------------------------
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
        const std::vector<Texel> out = smoother.Run(HalvesOf(in));
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
            { 24, 0.0, 1.0f, "taps 22..26: two traced zeros, three untraceable skipped" },
            { 25, 0.0, 1.0f, "taps 23..27: one traced zero (weight 1), four untraceable skipped" },
            { 26, 0.0, 0.0f, "taps 24..28: every tap untraceable -> no data at all" },
        };
        double worst = 0.0;
        for (const Row& r : rows)
            for (uint32_t y = 2; y + 2 < kHeight; ++y)
            {
                const Texel& t = out[At(r.x, y)];
                worst = std::fmax(worst, std::fabs(double(t.r) - r.expected));
                if (t.a != r.alpha || std::fabs(double(t.r) - r.expected) > 0.003)
                {
                    std::printf("gi_smooth_warp: traced-zero row x=%u y=%u: got %.5f alpha %.1f, expected %.5f alpha %.1f (%s)\n", r.x, y, double(t.r),
                                double(t.a), r.expected, double(r.alpha), r.why);
                    CHECK(false);
                    break;
                }
            }
        std::printf("gi_smooth_warp: traced zeros next to hits and untraceable texels, worst distance from the hand-computed rows %.5f\n", worst);
    }
    {
        // A lone hit among traced misses (alpha kData everywhere, rgb 0 except one texel of 1): the hit is shared over its 25 taps with its
        // binomial weight (36/256 at its own texel, 24/256 at a direct neighbour, 1/256 at the outer corner), nothing outside its reach, and
        // every texel keeps its data (alpha 1). A smooth that skipped the zeros would hand the whole 1.0 to all 25 taps.
        std::vector<Texel> in(size_t(kWidth) * kHeight, Texel { 0.0f, 0.0f, 0.0f, kData });
        in[At(30, 22)] = Texel { 1.0f, 1.0f, 1.0f, kData };
        const std::vector<Texel> out = smoother.Run(HalvesOf(in));
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
        std::printf("gi_smooth_warp: a lone hit among traced misses, worst distance from the weights %.5f (own texel %.5f, bound 36/256)\n", worst,
                    double(out[At(30, 22)].r));
        CHECK(out[At(30, 22)].r < 0.15f);
    }

    return Finish("gi_smooth_warp");
}
