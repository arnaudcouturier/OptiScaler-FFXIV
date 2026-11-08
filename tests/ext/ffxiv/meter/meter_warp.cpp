// Effect meter (METER): the pass (ext/ffxiv/meter/MeterPass.cpp and shaders/Meter.hlsl) on the D3D11 WARP software adapter, on synthetic pictures this test
// builds itself. The CPU references are the rules' own (MeterRules.h): ReferenceTotals visits every texel, TapTotals visits exactly the taps the shader does.
//
// What a wrong shader or pass would break, and what is checked:
//   a known change gives the known numbers: the chain stage (display terms against the typed white) changes a rectangle by 10.9 levels (visible) and another by
//   5.5 (not visible); the occlusion is measured on its RED channel only (a BGRA texture: byte 2) and over its occluded texels only; the sun mask over its lit
//   texels; the planar map over its reflecting texels; the share, the mean and the coverage are the expected ones;
//   the GPU totals equal the CPU tap emulation (taps and region counts exactly, the visible count exactly on pictures whose changes are far from the threshold,
//   the sum within float rounding) and land within 1 % of the CPU full diff;
//   key off: an output equal to the input reads exactly 0 visible and 0 sum;
//   the window crop is the dump's box (floored origin): changes outside it are not counted, the taps are the box's;
//   a NaN texel counts as a tap and nothing else;
//   the game's state comes back after a start (t0/t1, b0, render target, viewport, VS/PS, topology) and the stage's own textures are only read;
//   one draw per measurement; a start while one is in flight fails; every view the pass cannot read (SRGB, multisampled, unequal sizes, a window outside the
//   texture, a stage that makes no picture) fails with a reason and queues nothing; a second collect answers false.
#include "check.h"

#include <windows.h>

#include "ext/ffxiv/meter/MeterPass.h"
#include "ext/ffxiv/meter/MeterRules.h"

#include <d3d11.h>

#include <Meter_Ps.h>
#include <Meter_Vs.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
namespace mt = ext::ffxiv::meter;
using mt::StageId;

#define CHECK_SUCCEEDED(call) CHECK(SUCCEEDED(call))

namespace
{
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

// A picture as the shader's Load returns it (logical r, g, b, a, after the format's quantisation), plus the bytes that make the texture.
struct Image
{
    DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uint32_t w = 0, h = 0;
    std::vector<float> rgba; // w * h * 4, what Load returns
    std::vector<uint8_t> bytes;
    uint32_t pitch = 0;

    void Init(DXGI_FORMAT f, uint32_t width, uint32_t height)
    {
        format = f;
        w = width;
        h = height;
        rgba.assign(size_t(w) * h * 4, 0.0f);
    }
    float* At(uint32_t x, uint32_t y) { return &rgba[(size_t(y) * w + x) * 4]; }
    const float* At(uint32_t x, uint32_t y) const { return &rgba[(size_t(y) * w + x) * 4]; }

    // Quantises rgba to the format (so the CPU reference sees what the GPU loads) and fills the upload bytes.
    void Bake()
    {
        if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        {
            pitch = w * 8;
            bytes.assign(size_t(pitch) * h, 0);
            for (size_t i = 0; i < rgba.size(); ++i)
            {
                const uint16_t half = HalfOf(rgba[i]);
                rgba[i] = FromHalf(half);
                std::memcpy(&bytes[i * 2], &half, 2);
            }
        }
        else if (format == DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            pitch = w * 4;
            bytes.assign(size_t(pitch) * h, 0);
            for (size_t px = 0; px < size_t(w) * h; ++px)
            {
                const uint32_t order[4] { 2, 1, 0, 3 }; // byte 0 = blue, 1 = green, 2 = red, 3 = alpha
                for (int c = 0; c < 4; ++c)
                {
                    float& v = rgba[px * 4 + c];
                    const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                    const uint8_t byte = uint8_t(std::lround(clamped * 255.0f));
                    bytes[px * 4 + order[c]] = byte;
                    v = float(byte) / 255.0f;
                }
            }
        }
        else if (format == DXGI_FORMAT_R8_UNORM)
        {
            pitch = w;
            bytes.assign(size_t(pitch) * h, 0);
            for (size_t px = 0; px < size_t(w) * h; ++px)
            {
                float& v = rgba[px * 4];
                const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                const uint8_t byte = uint8_t(std::lround(clamped * 255.0f));
                bytes[px] = byte;
                v = float(byte) / 255.0f;
                rgba[px * 4 + 1] = rgba[px * 4 + 2] = 0.0f; // Load of an R8 texture returns (r, 0, 0, 1)
                rgba[px * 4 + 3] = 1.0f;
            }
        }
    }
};

struct Fixture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    bool Create() { return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)); }

    ComPtr<ID3D11ShaderResourceView> Upload(const Image& image, DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN, UINT sampleCount = 1, UINT arraySize = 1)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = image.w;
        desc.Height = image.h;
        desc.MipLevels = 1;
        desc.ArraySize = arraySize;
        desc.Format = image.format;
        desc.SampleDesc.Count = sampleCount;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        if (sampleCount == 1 && arraySize == 1)
        {
            D3D11_SUBRESOURCE_DATA initial { image.bytes.data(), image.pitch, 0 };
            CHECK_SUCCEEDED(device->CreateTexture2D(&desc, &initial, &texture));
        }
        else
        {
            if (sampleCount > 1)
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; // MSAA textures cannot be filled by data
            CHECK_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture));
        }
        if (texture == nullptr)
            return view;
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
        viewDesc.Format = viewFormat == DXGI_FORMAT_UNKNOWN ? image.format : viewFormat;
        if (sampleCount > 1)
        {
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        }
        else if (arraySize > 1)
        {
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            viewDesc.Texture2DArray.MipLevels = 1;
            viewDesc.Texture2DArray.ArraySize = arraySize;
        }
        else
        {
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MipLevels = 1;
        }
        CHECK_SUCCEEDED(device->CreateShaderResourceView(texture.Get(), &viewDesc, &view));
        return view;
    }

    // Starts one measurement and waits (politely, bounded) for it.
    bool Measure(mt::Pass& pass, const mt::Request& request, mt::Result& result)
    {
        if (!pass.Start(context.Get(), request))
            return false;
        for (int i = 0; i < 4000; ++i)
        {
            if (pass.Collect(context.Get(), result))
                return true;
            context->Flush();
            Sleep(1);
        }
        return false;
    }
};

mt::Request RequestFor(StageId id, ID3D11ShaderResourceView* in, ID3D11ShaderResourceView* out, float white = 0.7f, uint32_t seed = 3)
{
    mt::Request r;
    r.id = id;
    r.in = in;
    r.out = out;
    r.white = white;
    r.seed = seed;
    return r;
}

// The CPU references over an image pair, with the box origin and the stage's params.
auto Fetcher(const Image& a, const Image& b, uint32_t x0, uint32_t y0)
{
    return [&a, &b, x0, y0](uint32_t x, uint32_t y, float* in, float* out) {
        std::memcpy(in, a.At(x0 + x, y0 + y), 16);
        std::memcpy(out, b.At(x0 + x, y0 + y), 16);
    };
}

bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

// The deterministic base picture of the chain tests: a gradient with texture, 0.05 .. 0.30 (plus a change of 0.03 it stays below white 0.35).
float Base(uint32_t x, uint32_t y) { return 0.05f + 0.25f * float((x * 7 + y * 13) % 256) / 255.0f; }
} // namespace

int main()
{
    Fixture fx;
    if (!fx.Create())
    {
        std::printf("meter_warp: no WARP device\n");
        return 1;
    }
    constexpr uint32_t W = 1920, H = 1080;

    // ---- the chain stage: a known change ------------------------------------------------------------------------------------------------------------
    {
        Image in, out;
        in.Init(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H);
        out.Init(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H);
        // A visible change (10.9 levels at white 0.7: 0.03 stored) on [192, 1408) x [96, 704) and a change below the threshold (5.5 levels: 0.015 stored) on
        // [1408, 1792) x [96, 704). Every edge is a multiple of the block (16), so a block is wholly inside or outside.
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                float* a = in.At(x, y);
                float* b = out.At(x, y);
                const float v = Base(x, y);
                a[0] = v;
                a[1] = v * 0.9f;
                a[2] = v * 0.8f;
                a[3] = 1.0f;
                float d = 0.0f;
                if (y >= 96 && y < 704 && x >= 192 && x < 1408)
                    d = 0.03f;
                else if (y >= 96 && y < 704 && x >= 1408 && x < 1792)
                    d = 0.015f;
                b[0] = v + d;
                b[1] = v * 0.9f;
                b[2] = v * 0.8f;
                b[3] = 1.0f;
            }
        in.Bake();
        out.Bake();
        auto inView = fx.Upload(in);
        auto outView = fx.Upload(out);
        mt::Pass pass;
        mt::Result result;
        CHECK(fx.Measure(pass, RequestFor(StageId::Gi, inView.Get(), outView.Get(), 0.7f, 3), result));
        CHECK_EQ(pass.DrawsLastStart(), 1u);
        CHECK(pass.Failure() == nullptr);
        CHECK(!pass.InFlight());
        CHECK_EQ(result.geometry.block, 16u);
        CHECK_EQ(result.geometry.binsX, 120u);
        CHECK_EQ(result.geometry.binsY, 68u);
        CHECK_EQ(result.boxWidth, W);
        CHECK_EQ(result.boxHeight, H);
        CHECK(result.id == StageId::Gi);

        const mt::Params params = mt::ParamsOf(StageId::Gi, 0.7f);
        const auto fetch = Fetcher(in, out, 0, 0);
        const mt::Totals cpuTaps = mt::TapTotals(W, H, result.geometry, 3, params, fetch);
        const mt::Totals cpuFull = mt::ReferenceTotals(W, H, params, fetch);
        std::printf("  chain: gpu taps %.0f region %.0f visible %.0f sum %.1f | cpu taps %.0f region %.0f visible %.0f sum %.1f\n", result.totals.taps,
                    result.totals.region, result.totals.visible, result.totals.sum, cpuTaps.taps, cpuTaps.region, cpuTaps.visible, cpuTaps.sum);
        // the GPU equals the CPU tap emulation
        CHECK(result.totals.taps == cpuTaps.taps);
        CHECK(result.totals.region == cpuTaps.region);
        CHECK(result.totals.visible == cpuTaps.visible);
        CHECK(Near(result.totals.sum, cpuTaps.sum, cpuTaps.sum * 1e-3 + 1.0));
        // the known numbers: the visible share is the first rectangle's area over the window (the second change is below the threshold)
        const double expectedShare = double(1216) * 608 / (double(W) * H);
        CHECK(Near(result.totals.Share(), expectedShare, 1e-9));
        CHECK(Near(result.totals.Coverage(), 1.0, 1e-9)); // the chain's region is the window
        // the mean: 10.9 levels over the first rectangle, 5.5 over the second (the sampled mean equals the full one within 1 %)
        const double expectedMean = (1216.0 * 608 * 0.03 / 0.7 * 255.0 + 384.0 * 608 * 0.015 / 0.7 * 255.0) / (double(W) * H);
        CHECK(Near(result.totals.Mean(), expectedMean, expectedMean * 0.01));
        // the sampled numbers land within 1 % (absolute share) of the full diff
        CHECK(Near(result.totals.Share(), cpuFull.Share(), 0.01));
        CHECK(Near(result.totals.Mean(), cpuFull.Mean(), 0.1));
        // the white scales the levels: at white 0.35 the 5.5-level change becomes 11 levels and turns visible
        CHECK(fx.Measure(pass, RequestFor(StageId::Gi, inView.Get(), outView.Get(), 0.35f, 3), result));
        CHECK(Near(result.totals.Share(), double(1216 + 384) * 608 / (double(W) * H), 1e-9));
        // an unusable white falls back to the captured measurement instead of dividing by it
        CHECK(fx.Measure(pass, RequestFor(StageId::Gi, inView.Get(), outView.Get(), 0.0f, 3), result));
        CHECK(result.totals.region > 0.0 && std::isfinite(result.totals.sum));
        // two seeds on this block-aligned picture give the same totals (the taps move inside their blocks)
        mt::Result other;
        CHECK(fx.Measure(pass, RequestFor(StageId::Gi, inView.Get(), outView.Get(), 0.7f, 99), other));
        CHECK(fx.Measure(pass, RequestFor(StageId::Gi, inView.Get(), outView.Get(), 0.7f, 3), result));
        CHECK(Near(other.totals.Share(), result.totals.Share(), 0.01));
        CHECK_EQ(pass.Started(), pass.Collected());
    }

    // ---- key off: output equals input ----------------------------------------------------------------------------------------------------------------
    {
        Image in;
        in.Init(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                float* a = in.At(x, y);
                const float v = Base(x, y) * 2.0f; // some values above the white too
                a[0] = v;
                a[1] = v * 0.9f;
                a[2] = v * 0.8f;
                a[3] = 1.0f;
            }
        in.Bake();
        auto a = fx.Upload(in);
        auto b = fx.Upload(in); // a second texture with the same bits
        mt::Pass pass;
        mt::Result result;
        for (StageId id : { StageId::Shafts, StageId::Godrays, StageId::Gi, StageId::Dof, StageId::Atmosphere, StageId::NightGrade, StageId::PuddleTone })
        {
            CHECK(fx.Measure(pass, RequestFor(id, a.Get(), b.Get(), 0.7f, 5), result));
            CHECK(result.totals.region == result.totals.taps && result.totals.taps > 0.0);
            CHECK(result.totals.visible == 0.0 && result.totals.sum == 0.0);
        }
    }

    // ---- key off for the other stage kinds: the pre-light stages' textures (R8 masks, BGRA occlusion / planar map / G-buffer 1) ---------------------------------------
    {
        const struct
        {
            StageId id;
            DXGI_FORMAT format;
        } kinds[] {
            { StageId::ContactShadows, DXGI_FORMAT_R8_UNORM }, { StageId::ShadowLift, DXGI_FORMAT_R8_UNORM },  { StageId::Ao, DXGI_FORMAT_B8G8R8A8_UNORM },
            { StageId::Gloss, DXGI_FORMAT_B8G8R8A8_UNORM },    { StageId::Ssr, DXGI_FORMAT_B8G8R8A8_UNORM },
        };
        mt::Pass pass;
        for (const auto& kind : kinds)
        {
            Image in;
            in.Init(kind.format, 640, 360);
            for (uint32_t y = 0; y < 360; ++y)
                for (uint32_t x = 0; x < 640; ++x)
                {
                    float* a = in.At(x, y);
                    const float v = float((x * 5 + y * 3) % 200) / 255.0f + 0.1f;
                    a[0] = a[1] = a[2] = v;
                    a[3] = 0.5f;
                }
            in.Bake();
            auto a = fx.Upload(in);
            auto b = fx.Upload(in);
            mt::Result result;
            CHECK(fx.Measure(pass, RequestFor(kind.id, a.Get(), b.Get(), 1.0f, 9), result));
            CHECK(result.totals.taps > 0.0);
            CHECK(result.totals.visible == 0.0 && result.totals.sum == 0.0);
        }
    }

    // ---- the occlusion: red channel only, occluded texels only -------------------------------------------------------------------------------------------
    {
        Image in, out;
        in.Init(DXGI_FORMAT_B8G8R8A8_UNORM, W, H);
        out.Init(DXGI_FORMAT_B8G8R8A8_UNORM, W, H);
        // the engine's occlusion is white (1.0) outside a 400 x 304 patch at (480, 240); inside it is 128/255 in red. This module's texture: the left 208 columns of the
        // patch at 100/255 (28 levels, visible), the right 192 at 130/255 (2 levels, not visible). Green is changed everywhere in the output (must not count).
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                float* a = in.At(x, y);
                float* b = out.At(x, y);
                const bool patch = x >= 480 && x < 880 && y >= 240 && y < 544;
                a[0] = patch ? 128.0f / 255.0f : 1.0f;
                a[1] = 0.5f;
                a[2] = 0.5f;
                a[3] = 1.0f;
                b[0] = patch ? (x < 688 ? 100.0f / 255.0f : 130.0f / 255.0f) : 1.0f;
                b[1] = 0.9f; // green moved by 100 levels everywhere
                b[2] = 0.5f;
                b[3] = 1.0f;
            }
        in.Bake();
        out.Bake();
        auto inView = fx.Upload(in);
        auto outView = fx.Upload(out);
        mt::Pass pass;
        mt::Result result;
        CHECK(fx.Measure(pass, RequestFor(StageId::Ao, inView.Get(), outView.Get(), 1.0f, 1), result));
        const double patchTexels = 400.0 * 304.0;
        CHECK(result.totals.region > 0.0);
        CHECK(Near(result.totals.Coverage(), patchTexels / (double(W) * H), 1e-6)); // the patch is the region, not the window
        CHECK(Near(result.totals.Share(), 208.0 / 400.0, 1e-9));                    // the left 208 of the patch's 400 columns changed by 28 levels
        CHECK(Near(result.totals.Mean(), (28.0 * 208.0 + 2.0 * 192.0) / 400.0, 0.01)); // the red channel's change only (the green moved by 100)
        const mt::Params params = mt::ParamsOf(StageId::Ao, 1.0f);
        const mt::Totals cpu = mt::TapTotals(W, H, result.geometry, 1, params, Fetcher(in, out, 0, 0));
        CHECK(result.totals.region == cpu.region && result.totals.visible == cpu.visible && result.totals.taps == cpu.taps);
        // an AO texture where this module did nothing: no visible change, the region is still the occluded texels
        auto sameView = fx.Upload(in);
        CHECK(fx.Measure(pass, RequestFor(StageId::Ao, inView.Get(), sameView.Get(), 1.0f, 1), result));
        CHECK(result.totals.visible == 0.0 && result.totals.sum == 0.0 && result.totals.region > 0.0);
    }

    // ---- the sun mask: lit texels only (R8) ---------------------------------------------------------------------------------------------------------------
    {
        constexpr uint32_t MW = 1000, MH = 700; // block 8
        Image in, out;
        in.Init(DXGI_FORMAT_R8_UNORM, MW, MH);
        out.Init(DXGI_FORMAT_R8_UNORM, MW, MH);
        // lit (200/255) on the left 600 columns, black on the rest; the combined mask darkens the lit part's top half to 180/255 (20 levels)
        for (uint32_t y = 0; y < MH; ++y)
            for (uint32_t x = 0; x < MW; ++x)
            {
                const bool lit = x < 600;
                in.At(x, y)[0] = lit ? 200.0f / 255.0f : 0.0f;
                out.At(x, y)[0] = lit ? (y < 350 ? 180.0f / 255.0f : 200.0f / 255.0f) : 0.0f;
            }
        in.Bake();
        out.Bake();
        auto inView = fx.Upload(in);
        auto outView = fx.Upload(out);
        mt::Pass pass;
        mt::Result result;
        CHECK(fx.Measure(pass, RequestFor(StageId::ContactShadows, inView.Get(), outView.Get(), 1.0f, 2), result));
        CHECK(result.geometry.block == 16u);
        const mt::Totals full = mt::ReferenceTotals(MW, MH, mt::ParamsOf(StageId::ContactShadows, 1.0f), Fetcher(in, out, 0, 0));
        CHECK(Near(full.Coverage(), 0.6, 1e-9) && Near(full.Share(), 0.5, 1e-9) && Near(full.Mean(), 10.0, 1e-3));
        CHECK(Near(result.totals.Coverage(), full.Coverage(), 0.01));
        CHECK(Near(result.totals.Share(), full.Share(), 0.01));
        CHECK(Near(result.totals.Mean(), full.Mean(), 0.2));
        // the shadow lift's region is the texels that are not fully lit: the same picture read as a lift (shaded = below 247.5 levels = all of the lit part here)
        CHECK(fx.Measure(pass, RequestFor(StageId::ShadowLift, inView.Get(), outView.Get(), 1.0f, 2), result));
        CHECK(result.totals.region == result.totals.taps); // every texel is below 247.5 levels in the input or the output
    }

    // ---- the planar map: reflecting texels only ------------------------------------------------------------------------------------------------------------
    {
        Image in, out;
        in.Init(DXGI_FORMAT_B8G8R8A8_UNORM, W, H);
        out.Init(DXGI_FORMAT_B8G8R8A8_UNORM, W, H);
        // the engine's map is black; this module writes a reflection (60 levels) in a 640 x 288 patch and a faint one (4 levels) in another of 320 x 96
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                float* b = out.At(x, y);
                const bool bright = x >= 800 && x < 1440 && y >= 400 && y < 688;
                const bool faint = x >= 96 && x < 416 && y >= 800 && y < 896;
                const float v = bright ? 60.0f / 255.0f : (faint ? 4.0f / 255.0f : 0.0f);
                b[0] = b[1] = b[2] = v;
                b[3] = 1.0f;
                in.At(x, y)[3] = 1.0f;
            }
        in.Bake();
        out.Bake();
        auto inView = fx.Upload(in);
        auto outView = fx.Upload(out);
        mt::Pass pass;
        mt::Result result;
        CHECK(fx.Measure(pass, RequestFor(StageId::Ssr, inView.Get(), outView.Get(), 1.0f, 4), result));
        const double bright = 640.0 * 288.0, faint = 320.0 * 96.0;
        CHECK(Near(result.totals.Coverage(), (bright + faint) / (double(W) * H), 1e-6));
        CHECK(Near(result.totals.Share(), bright / (bright + faint), 1e-9));
        CHECK(Near(result.totals.Mean(), (bright * 60.0 + faint * 4.0) / (bright + faint), 0.01));
    }

    // ---- the puddle gloss: the alpha channel of G-buffer 1 over the window --------------------------------------------------------------------------------------
    {
        Image in, out;
        in.Init(DXGI_FORMAT_B8G8R8A8_UNORM, W, H);
        out.Init(DXGI_FORMAT_B8G8R8A8_UNORM, W, H);
        // the gloss weight (alpha) goes from 0.2 to 0.5 (51 -> 128 of 255: 77 levels) on the top-left 960 x 544 quarter and stays; the colour channels move by 100 levels everywhere and must not count
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                float* a = in.At(x, y);
                float* b = out.At(x, y);
                a[0] = a[1] = a[2] = 0.3f;
                a[3] = 0.2f;
                b[0] = b[1] = b[2] = 0.3f + 100.0f / 255.0f;
                b[3] = (x < 960 && y < 544) ? 0.5f : 0.2f;
            }
        in.Bake();
        out.Bake();
        auto inView = fx.Upload(in);
        auto outView = fx.Upload(out);
        mt::Pass pass;
        mt::Result result;
        CHECK(fx.Measure(pass, RequestFor(StageId::Gloss, inView.Get(), outView.Get(), 1.0f, 8), result));
        const double share = 960.0 * 544.0 / (double(W) * H);
        CHECK(Near(result.totals.Coverage(), 1.0, 1e-9)); // the gloss is measured over the window
        CHECK(Near(result.totals.Share(), share, 0.002)); // 960 is a multiple of 16; 544 = 34 x 16 (the last block row is partial: the tap density is uniform)
        CHECK(Near(result.totals.Mean(), share * 77.0, 0.1)); // the alpha's change only (0.2 -> 51/255, 0.5 -> 128/255: 77 levels)
    }

    // ---- the window crop, a NaN texel -----------------------------------------------------------------------------------------------------------------
    {
        Image in, out;
        in.Init(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H);
        out.Init(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                float* a = in.At(x, y);
                float* b = out.At(x, y);
                a[0] = a[1] = a[2] = 0.3f;
                a[3] = 1.0f;
                // a big change everywhere OUTSIDE the box and none inside it
                const bool inside = x < 320 && y < 180;
                b[0] = b[1] = b[2] = inside ? 0.3f : 0.6f;
                b[3] = 1.0f;
            }
        in.Bake();
        out.Bake();
        auto inView = fx.Upload(in);
        auto outView = fx.Upload(out);
        mt::Pass pass;
        mt::Result result;
        mt::Request request = RequestFor(StageId::Dof, inView.Get(), outView.Get(), 0.7f, 6);
        request.x = 0.5f; // the engine's half-pixel origin: floored, like the dump's crop
        request.y = 0.5f;
        request.width = 320.0f;
        request.height = 180.0f;
        CHECK(fx.Measure(pass, request, result));
        CHECK_EQ(result.boxWidth, 320u);
        CHECK_EQ(result.boxHeight, 180u);
        CHECK(result.geometry.Exact()); // block 4: every texel of the box is a tap
        CHECK(result.totals.taps == 320.0 * 180.0);
        CHECK(result.totals.visible == 0.0 && result.totals.sum == 0.0); // the change lies outside the box
        // the other side of the same picture: a box that covers only the changed part counts all of it
        request.x = 320.0f;
        request.y = 180.0f;
        request.width = 320.0f;
        request.height = 180.0f;
        CHECK(fx.Measure(pass, request, result));
        CHECK(result.totals.taps == 320.0 * 180.0 && result.totals.visible == result.totals.taps);
        CHECK(Near(result.totals.Mean(), 0.3 / 0.7 * 255.0, 0.3)); // (the half rounding of 0.3 and 0.6)

        // a NaN texel in the output (a half NaN, 0x7E00) never counts as a visible change: its input is black, so a driver that loads the NaN as 0 sees no change
        // and one that keeps the NaN excludes the texel from the region - either way it is one tap and not one visible texel
        out.Init(DXGI_FORMAT_R16G16B16A16_FLOAT, 64, 64);
        in.Init(DXGI_FORMAT_R16G16B16A16_FLOAT, 64, 64);
        for (uint32_t y = 0; y < 64; ++y)
            for (uint32_t x = 0; x < 64; ++x)
            {
                float* a = in.At(x, y);
                float* b = out.At(x, y);
                a[0] = a[1] = a[2] = (x == 10 && y == 20) ? 0.0f : 0.3f;
                a[3] = 1.0f;
                b[0] = b[1] = b[2] = (x == 10 && y == 20) ? std::numeric_limits<float>::quiet_NaN() : 0.6f;
                b[3] = 1.0f;
            }
        in.Bake();
        out.Bake();
        // Bake() turns the NaN into a half NaN through HalfOf (exponent 31): write the bits explicitly to be sure
        {
            const uint16_t nan = 0x7E00;
            std::memcpy(&out.bytes[(size_t(20) * 64 + 10) * 8], &nan, 2);
            std::memcpy(&out.bytes[(size_t(20) * 64 + 10) * 8 + 2], &nan, 2);
            std::memcpy(&out.bytes[(size_t(20) * 64 + 10) * 8 + 4], &nan, 2);
        }
        auto nanIn = fx.Upload(in);
        auto nanOut = fx.Upload(out);
        CHECK(fx.Measure(pass, RequestFor(StageId::Gi, nanIn.Get(), nanOut.Get(), 0.7f, 1), result));
        std::printf("  nan: taps %.0f region %.0f visible %.0f sum %.1f\n", result.totals.taps, result.totals.region, result.totals.visible, result.totals.sum);
        CHECK(result.totals.taps == 64.0 * 64.0);
        CHECK(result.totals.visible == 64.0 * 64.0 - 1.0);
        CHECK(result.totals.region == 64.0 * 64.0 - 1.0 || result.totals.region == 64.0 * 64.0);
        CHECK(std::isfinite(result.totals.sum));
    }

    // ---- the game's state comes back ---------------------------------------------------------------------------------------------------------------------
    {
        Image in, out;
        in.Init(DXGI_FORMAT_R16G16B16A16_FLOAT, 256, 128);
        out.Init(DXGI_FORMAT_R16G16B16A16_FLOAT, 256, 128);
        for (uint32_t y = 0; y < 128; ++y)
            for (uint32_t x = 0; x < 256; ++x)
            {
                in.At(x, y)[0] = 0.2f;
                out.At(x, y)[0] = 0.4f;
            }
        in.Bake();
        out.Bake();
        auto inView = fx.Upload(in);
        auto outView = fx.Upload(out);
        // the game's own bindings: two other views at t0/t1, a constant buffer at b0, a render target, a viewport, a vertex and a pixel shader, a topology
        Image other;
        other.Init(DXGI_FORMAT_R16G16B16A16_FLOAT, 32, 32);
        other.Bake();
        auto gameT0 = fx.Upload(other);
        auto gameT1 = fx.Upload(other);
        D3D11_BUFFER_DESC bufferDesc {};
        bufferDesc.ByteWidth = 64;
        bufferDesc.Usage = D3D11_USAGE_DEFAULT;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> gameCb;
        CHECK_SUCCEEDED(fx.device->CreateBuffer(&bufferDesc, nullptr, &gameCb));
        D3D11_TEXTURE2D_DESC targetDesc {};
        targetDesc.Width = 64;
        targetDesc.Height = 64;
        targetDesc.MipLevels = 1;
        targetDesc.ArraySize = 1;
        targetDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        targetDesc.SampleDesc.Count = 1;
        targetDesc.Usage = D3D11_USAGE_DEFAULT;
        targetDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> gameTarget;
        ComPtr<ID3D11RenderTargetView> gameRtv;
        CHECK_SUCCEEDED(fx.device->CreateTexture2D(&targetDesc, nullptr, &gameTarget));
        CHECK_SUCCEEDED(fx.device->CreateRenderTargetView(gameTarget.Get(), nullptr, &gameRtv));
        ComPtr<ID3D11VertexShader> gameVs;
        ComPtr<ID3D11PixelShader> gamePs;
        CHECK_SUCCEEDED(fx.device->CreateVertexShader(Meter_Vs_cso, sizeof Meter_Vs_cso, nullptr, &gameVs));
        CHECK_SUCCEEDED(fx.device->CreatePixelShader(Meter_Ps_cso, sizeof Meter_Ps_cso, nullptr, &gamePs));
        ID3D11ShaderResourceView* views[2] { gameT0.Get(), gameT1.Get() };
        ID3D11Buffer* buffers[1] { gameCb.Get() };
        ID3D11RenderTargetView* rtv = gameRtv.Get();
        const D3D11_VIEWPORT viewport { 7.0f, 9.0f, 33.0f, 44.0f, 0.0f, 1.0f };
        ID3D11DeviceContext* c = fx.context.Get();
        c->PSSetShaderResources(0, 2, views);
        c->PSSetConstantBuffers(0, 1, buffers);
        c->OMSetRenderTargets(1, &rtv, nullptr);
        c->RSSetViewports(1, &viewport);
        c->VSSetShader(gameVs.Get(), nullptr, 0);
        c->PSSetShader(gamePs.Get(), nullptr, 0);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

        mt::Pass pass;
        mt::Result result;
        CHECK(fx.Measure(pass, RequestFor(StageId::Gi, inView.Get(), outView.Get(), 0.7f, 1), result));

        ID3D11ShaderResourceView* gotViews[2] {};
        c->PSGetShaderResources(0, 2, gotViews);
        CHECK(gotViews[0] == gameT0.Get() && gotViews[1] == gameT1.Get());
        for (auto* v : gotViews)
            if (v != nullptr)
                v->Release();
        ID3D11Buffer* gotBuffer = nullptr;
        c->PSGetConstantBuffers(0, 1, &gotBuffer);
        CHECK(gotBuffer == gameCb.Get());
        if (gotBuffer != nullptr)
            gotBuffer->Release();
        ID3D11RenderTargetView* gotRtv = nullptr;
        c->OMGetRenderTargets(1, &gotRtv, nullptr);
        CHECK(gotRtv == gameRtv.Get());
        if (gotRtv != nullptr)
            gotRtv->Release();
        D3D11_VIEWPORT gotViewport {};
        UINT viewportCount = 1;
        c->RSGetViewports(&viewportCount, &gotViewport);
        CHECK(viewportCount == 1 && gotViewport.TopLeftX == 7.0f && gotViewport.TopLeftY == 9.0f && gotViewport.Width == 33.0f && gotViewport.Height == 44.0f);
        ID3D11VertexShader* gotVs = nullptr;
        c->VSGetShader(&gotVs, nullptr, nullptr);
        CHECK(gotVs == gameVs.Get());
        if (gotVs != nullptr)
            gotVs->Release();
        ID3D11PixelShader* gotPs = nullptr;
        c->PSGetShader(&gotPs, nullptr, nullptr);
        CHECK(gotPs == gamePs.Get());
        if (gotPs != nullptr)
            gotPs->Release();
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        c->IAGetPrimitiveTopology(&topology);
        CHECK(topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

        // the stage's textures are only read: the output still holds its bytes
        CHECK(Near(result.totals.Mean(), 0.2 / 0.7 * 255.0, 0.05));
    }

    // ---- one start at a time; a second collect answers false ----------------------------------------------------------------------------------------------
    {
        Image in;
        in.Init(DXGI_FORMAT_R16G16B16A16_FLOAT, 256, 128);
        in.Bake();
        auto a = fx.Upload(in);
        auto b = fx.Upload(in);
        mt::Pass pass;
        CHECK(pass.Start(fx.context.Get(), RequestFor(StageId::Gi, a.Get(), b.Get())));
        CHECK(pass.InFlight());
        CHECK(!pass.Start(fx.context.Get(), RequestFor(StageId::Gi, a.Get(), b.Get())));
        CHECK(pass.Failure() != nullptr);
        CHECK_EQ(pass.DrawsLastStart(), 0u);
        mt::Result result;
        bool got = false;
        for (int i = 0; i < 4000 && !got; ++i)
        {
            got = pass.Collect(fx.context.Get(), result);
            if (!got)
            {
                fx.context->Flush();
                Sleep(1);
            }
        }
        CHECK(got);
        CHECK(!pass.InFlight());
        CHECK(!pass.Collect(fx.context.Get(), result));
        CHECK_EQ(pass.Started(), 1u);
        CHECK_EQ(pass.Collected(), 1u);
        if (result.haveGpuMs)
            CHECK(result.gpuMs >= 0.0 && result.gpuMs < 1000.0);
    }

    // ---- what the pass cannot read -------------------------------------------------------------------------------------------------------------------------
    {
        Image in, tiny;
        in.Init(DXGI_FORMAT_R16G16B16A16_FLOAT, 256, 128);
        in.Bake();
        tiny.Init(DXGI_FORMAT_R16G16B16A16_FLOAT, 128, 128);
        tiny.Bake();
        auto a = fx.Upload(in);
        auto b = fx.Upload(in);
        auto s = fx.Upload(tiny);
        mt::Pass pass;
        const auto refused = [&](const mt::Request& r, const char* what) {
            const bool started = pass.Start(fx.context.Get(), r);
            CHECK(!started);
            CHECK(pass.Failure() != nullptr);
            CHECK(!pass.InFlight());
            CHECK_EQ(pass.DrawsLastStart(), 0u);
            if (started)
                std::printf("  not refused: %s\n", what);
        };
        refused(RequestFor(StageId::Gi, a.Get(), s.Get()), "unequal sizes");
        mt::Request flare = RequestFor(StageId::Flare, a.Get(), b.Get());
        refused(flare, "a stage that makes no picture");
        mt::Request outside = RequestFor(StageId::Gi, a.Get(), b.Get());
        outside.x = 5000.0f;
        outside.y = 5000.0f;
        outside.width = 10.0f;
        outside.height = 10.0f;
        refused(outside, "a window outside the texture");
        mt::Request none = RequestFor(StageId::Gi, nullptr, b.Get());
        refused(none, "no view");
        // an SRGB view of the same bytes: a Load would linearise what the stage stored, so it is not read
        Image srgb;
        srgb.Init(DXGI_FORMAT_B8G8R8A8_UNORM, 64, 64);
        srgb.Bake();
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = 64;
        desc.Height = 64;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> typeless;
        CHECK_SUCCEEDED(fx.device->CreateTexture2D(&desc, nullptr, &typeless));
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
        viewDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> srgbView;
        CHECK_SUCCEEDED(fx.device->CreateShaderResourceView(typeless.Get(), &viewDesc, &srgbView));
        refused(RequestFor(StageId::Ao, srgbView.Get(), srgbView.Get()), "an sRGB view");
        // an array view
        Image arrayImage;
        arrayImage.Init(DXGI_FORMAT_R16G16B16A16_FLOAT, 64, 64);
        arrayImage.Bake();
        auto arrayView = fx.Upload(arrayImage, DXGI_FORMAT_UNKNOWN, 1, 2);
        if (arrayView)
            refused(RequestFor(StageId::Gi, arrayView.Get(), arrayView.Get()), "an array view");
        // after all those refusals a good request still runs
        mt::Result result;
        CHECK(fx.Measure(pass, RequestFor(StageId::Gi, a.Get(), b.Get()), result));
        CHECK(pass.Failure() == nullptr);
        // the formats the pass reads
        CHECK(mt::Pass::FormatReadable(DXGI_FORMAT_R16G16B16A16_FLOAT) && mt::Pass::FormatReadable(DXGI_FORMAT_B8G8R8A8_UNORM) && mt::Pass::FormatReadable(DXGI_FORMAT_R8_UNORM));
        CHECK(!mt::Pass::FormatReadable(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) && !mt::Pass::FormatReadable(DXGI_FORMAT_R32_UINT) &&
              !mt::Pass::FormatReadable(DXGI_FORMAT_R16G16B16A16_TYPELESS) && !mt::Pass::FormatReadable(DXGI_FORMAT_D32_FLOAT) &&
              !mt::Pass::FormatReadable(DXGI_FORMAT_BC1_UNORM));
    }

    return Finish("meter_warp");
}
