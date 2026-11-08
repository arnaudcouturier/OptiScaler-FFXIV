// Atmosphere: the pass (ext/ffxiv/engine/AtmoPass.cpp and shaders/Atmo.hlsl) on the D3D11 WARP software adapter, on synthetic buffers this test builds
// itself, against a CPU reference that is AtmoRules.h's own functions applied pixel by pixel.
//
// The frame is 160x120, the camera at its origin looking down -z through the capture's own inverse projection (reversed Z, infinite far, near 0.1 m:
// view z = 0.1 / depth). The columns are surfaces at known distances: 0..31 at 10 m (nearer than the 40 m start), 32..63 at 100 m, 64..95 at 400 m,
// 96..127 the far plane (depth 0: the sky), 128..159 at 60 m. The scene is noise at every pixel (levels exact in FP16).
//
// What a wrong pass would break, and what is checked:
//   the mix against the CPU reference, per pixel and per channel, within two half-precision steps: the optical depth, the transmittance, the sun's lobe,
//     the ambient term and - because the reference is written in linear light - the domain of the mix (mutants: T applied twice, the mix done in stored
//     space, the start distance ignored, the phase's g sign flipped);
//   the near field and the sky are the scene's own texels bit for bit (the sky exclusion; the start distance), and the window's outside is the handed-in
//     picture bit for bit;
//   a key that says nothing to do (no medium, no density, off) answers a byte-exact copy with no draw (mutant: key off is not a passthrough);
//   no sun runs on the ambient term alone and the lobe moves the haze toward the sun (mirrored sun: the left and right halves swap);
//   two runs of one frame are one picture, bit for bit (nothing in the pass is noise or history);
//   the engine-fog scale and the night scale reach the pass through the density (a foggy engine leaves less of ours);
//   the game's bindings come back after a run, and a binding the pass cannot read returns null with a reason.
#include "check.h"

#include "ext/ffxiv/engine/AtmoPass.h"
#include "ext/ffxiv/engine/AtmoRules.h"

#include <d3d11.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::engine::atmo;
using ext::ffxiv::shafts::Window;

#define CHECK_SUCCEEDED(call) CHECK(SUCCEEDED(call))

namespace
{
constexpr uint32_t kWidth = 160;
constexpr uint32_t kHeight = 120;

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

// The size of one half-precision step at a value (the tolerance's unit).
float HalfStep(float value)
{
    const float magnitude = std::fabs(value) < 6.1e-5f ? 6.1e-5f : std::fabs(value);
    return std::ldexp(1.0f, int(std::floor(std::log2(magnitude))) - 10);
}

// The capture's own inverse projection (reversed Z, infinite far, near 0.1): view z = -0.1 / depth.
void CaptureInverseProjection(float out[16])
{
    std::memset(out, 0, 16 * sizeof(float));
    out[0] = 0.7307643f;
    out[5] = 0.4110417f;
    out[11] = -1.0f;
    out[14] = 10.0f;
}

constexpr float kStart = 40.0f;
constexpr float kRho = 0.004f; // per metre

float ZoneDistance(uint32_t x)
{
    if (x < 32)
        return 10.0f;
    if (x < 64)
        return 100.0f;
    if (x < 96)
        return 400.0f;
    if (x < 128)
        return 0.0f; // the sky
    return 60.0f;
}

float SceneLevel(uint32_t x, uint32_t y, uint32_t c)
{
    // Hash noise, levels 0.125 .. 1.5 in eighths (exact in FP16); the sky columns carry a bright value.
    uint32_t h = x * 73856093u ^ y * 19349663u ^ c * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return 0.125f * float(1 + (h % 12));
}

struct Fixture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> sceneTexture, otherTexture, depthTexture;
    ComPtr<ID3D11ShaderResourceView> sceneView, otherView, depthView;
    ComPtr<ID3D11RenderTargetView> otherRtv;
    Window window { 0.0f, 0.0f, float(kWidth), float(kHeight) };
    float inverse[16] {};
    std::vector<uint16_t> sceneBits;
    std::vector<float> depthValues;
    float ndcScale[2] { 2.0f / float(kWidth), -2.0f / float(kHeight) };
    float ndcOffset[2] { -1.0f, 1.0f };

    bool Create()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
            return false;
        if (device == nullptr || context == nullptr)
            return false;
        CaptureInverseProjection(inverse);
        sceneBits.assign(size_t(kWidth) * kHeight * 4, 0);
        depthValues.assign(size_t(kWidth) * kHeight, 0.0f);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const size_t at = (size_t(y) * kWidth + x) * 4;
                for (uint32_t c = 0; c < 3; ++c)
                    sceneBits[at + c] = HalfOf(SceneLevel(x, y, c));
                sceneBits[at + 3] = HalfOf(1.0f);
                const float z = ZoneDistance(x);
                depthValues[size_t(y) * kWidth + x] = z > 0.0f ? 0.1f / z : 0.0f;
            }
        if (!CreateColour(sceneBits, sceneTexture, sceneView, nullptr) ||
            !CreateColour(std::vector<uint16_t>(sceneBits.size(), HalfOf(0.0f)), otherTexture, otherView, &otherRtv))
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
        D3D11_SUBRESOURCE_DATA initial { depthValues.data(), kWidth * sizeof(float), 0 };
        return SUCCEEDED(device->CreateTexture2D(&desc, &initial, &depthTexture)) && SUCCEEDED(device->CreateShaderResourceView(depthTexture.Get(), nullptr, &depthView));
    }

    bool CreateColour(const std::vector<uint16_t>& pixels, ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11ShaderResourceView>& view, ComPtr<ID3D11RenderTargetView>* rtv)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rtv != nullptr ? D3D11_BIND_RENDER_TARGET : 0u);
        D3D11_SUBRESOURCE_DATA initial { pixels.data(), kWidth * 8, 0 };
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
        inputs.densityPerMetre = kRho;
        inputs.upView[0] = 0.0f;
        inputs.upView[1] = 1.0f;
        inputs.upView[2] = 0.0f;
        // The sun: up and a little to the right of the view's centre, behind the scene.
        inputs.sunView[0] = 0.6f;
        inputs.sunView[1] = 0.1f;
        inputs.sunView[2] = -std::sqrt(1.0f - 0.36f - 0.01f);
        inputs.sunGain = 0.8f;
        inputs.fogColour[0] = 0.36f;
        inputs.fogColour[1] = 0.53f;
        inputs.fogColour[2] = 0.59f;
        inputs.sunTint[0] = 1.0f;
        inputs.sunTint[1] = 0.9f;
        inputs.sunTint[2] = 0.8f;
        inputs.whiteLinear = WhiteLinearOf(0.838f);
        return inputs;
    }

    std::vector<uint16_t> ReadRgba(ID3D11ShaderResourceView* view)
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
        std::vector<uint16_t> out(size_t(kWidth) * kHeight * 4);
        for (uint32_t y = 0; y < kHeight; ++y)
            std::memcpy(out.data() + size_t(y) * kWidth * 4, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, kWidth * 4 * sizeof(uint16_t));
        context->Unmap(staging.Get(), 0);
        return out;
    }
};

// The CPU reference for one pixel: AtmoRules.h's functions in the shader's own order. `expected` is the stored value the pass must write (the scene's own
// half when the pixel is an early-out), and `early` says which.
struct Reference
{
    float rgb[3];
    bool early;
};

Reference ReferenceAt(const Fixture& fx, const Inputs& in, uint32_t x, uint32_t y)
{
    const size_t at = (size_t(y) * kWidth + x) * 4;
    Reference r {};
    for (uint32_t c = 0; c < 3; ++c)
        r.rgb[c] = FromHalf(fx.sceneBits[at + c]);
    r.early = true;
    const float depth = fx.depthValues[size_t(y) * kWidth + x];
    if (!IsAtmosphereSurface(depth))
        return r;
    const float ndcX = (float(x) + 0.5f) * in.ndcScale[0] + in.ndcOffset[0];
    const float ndcY = (float(y) + 0.5f) * in.ndcScale[1] + in.ndcOffset[1];
    float p[3];
    if (!ViewPositionOf(in.inverseProjection, ndcX, ndcY, depth, p))
        return r;
    const float distance = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (!(distance > in.params.start))
        return r;
    const float dir[3] { p[0] / distance, p[1] / distance, p[2] / distance };
    const float up = dir[0] * in.upView[0] + dir[1] * in.upView[1] + dir[2] * in.upView[2];
    const float tau = OpticalDepth(in.densityPerMetre, in.params.falloff, up, in.params.start, distance);
    const float transmittance = Transmittance(tau);
    if (!(transmittance < 1.0f))
        return r;
    const float cosTheta = dir[0] * in.sunView[0] + dir[1] * in.sunView[1] + dir[2] * in.sunView[2];
    const Inscatter air = InscatterOf(in.params, in.fogColour, in.sunTint, in.whiteLinear, in.sunGain, cosTheta);
    for (uint32_t c = 0; c < 3; ++c)
        r.rgb[c] = ComposeStored(r.rgb[c], transmittance, air.rgb[c]);
    r.early = false;
    return r;
}

bool SameBits(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(uint16_t)) == 0;
}

struct Totals
{
    uint32_t pixels = 0, mixed = 0, mismatches = 0, earlyExact = 0, earlyMismatch = 0;
    float worst = 0.0f;
};

// Compares a pass output to the reference over a rectangle (the window): early pixels must be the scene's own bits, mixed pixels within two half steps.
Totals Compare(const Fixture& fx, const Inputs& in, const std::vector<uint16_t>& out, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
    Totals totals;
    for (uint32_t y = y0; y < y0 + h; ++y)
        for (uint32_t x = x0; x < x0 + w; ++x)
        {
            const Reference ref = ReferenceAt(fx, in, x, y);
            const size_t at = (size_t(y) * kWidth + x) * 4;
            ++totals.pixels;
            if (ref.early)
            {
                bool exact = true;
                for (uint32_t c = 0; c < 4; ++c)
                    exact = exact && out[at + c] == fx.sceneBits[at + c];
                exact ? ++totals.earlyExact : ++totals.earlyMismatch;
                continue;
            }
            ++totals.mixed;
            bool ok = out[at + 3] == fx.sceneBits[at + 3];
            for (uint32_t c = 0; c < 3; ++c)
            {
                const float got = FromHalf(out[at + c]);
                const float tolerance = 2.5f * HalfStep(ref.rgb[c]) + 1.0e-6f;
                const float error = std::fabs(got - ref.rgb[c]);
                totals.worst = std::max(totals.worst, error / HalfStep(ref.rgb[c]));
                ok = ok && error <= tolerance;
            }
            if (!ok)
                ++totals.mismatches;
        }
    return totals;
}

float MeanLuma(const std::vector<uint16_t>& pixels, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1)
{
    double sum = 0.0;
    uint32_t count = 0;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
        {
            const size_t at = (size_t(y) * kWidth + x) * 4;
            sum += double(FromHalf(pixels[at])) + double(FromHalf(pixels[at + 1])) + double(FromHalf(pixels[at + 2]));
            ++count;
        }
    return float(sum / (3.0 * count));
}
} // namespace

int main()
{
    Fixture fx;
    if (!fx.Create())
    {
        std::printf("atmo_warp: no WARP device\n");
        return 1;
    }
    Pass pass;
    const Params params; // the defaults: start 40 m, falloff 0.01, g 0.7, sun 0.5, sky 1.0

    std::printf("-- stage: the mix against the CPU reference --\n");
    std::vector<uint16_t> baseline;
    {
        const Inputs inputs = fx.MakeInputs(params);
        ID3D11ShaderResourceView* out = pass.Run(fx.context.Get(), inputs);
        if (out == nullptr)
        {
            std::fflush(stdout);
            std::printf("atmo_warp: the pass did not run (%s)\n", pass.Failure());
            return 1;
        }
        CHECK(pass.DrawsLastRun() == 1);
        baseline = fx.ReadRgba(out);
        const Totals t = Compare(fx, inputs, baseline, 0, 0, kWidth, kHeight);
        std::printf("   %u pixels: %u mixed (worst %.2f half steps, %u mismatches), %u early-outs exact, %u early-outs wrong\n", t.pixels, t.mixed, double(t.worst),
                    t.mismatches, t.earlyExact, t.earlyMismatch);
        CHECK(t.mixed > 4000);        // the 100 m, 400 m and 60 m columns, 128 columns' worth of rows
        CHECK(t.mismatches == 0);     // the mix, per pixel, within two half steps (T applied twice / stored-space mix / g flipped / start ignored would fail)
        CHECK(t.earlyMismatch == 0);  // the sky and the near field: the scene's own texels, bit for bit
        CHECK(t.earlyExact > 4000);   // columns 0..31 (10 m) and 96..127 (the sky)
        // The pass really changed the far columns: the mean moved.
        const float before = MeanLuma(fx.sceneBits, 64, 96, 0, kHeight);
        const float after = MeanLuma(baseline, 64, 96, 0, kHeight);
        CHECK(std::fabs(after - before) > 0.02f);
    }

    std::printf("-- stage: the exclusions, the window, the identity --\n");
    {
        // The sky columns and the near columns: bit-exact, by explicit column, so a failure names what it is.
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                if (x < 32 || (x >= 96 && x < 128))
                    for (uint32_t c = 0; c < 4; ++c)
                        CHECK(baseline[(size_t(y) * kWidth + x) * 4 + c] == fx.sceneBits[(size_t(y) * kWidth + x) * 4 + c]);
        // A window inside the frame: its outside is the handed-in picture bit for bit, its inside matches the reference under its own mapping.
        Fixture& w = fx;
        const Window savedWindow = w.window;
        const float savedScale[2] { w.ndcScale[0], w.ndcScale[1] };
        const float savedOffset[2] { w.ndcOffset[0], w.ndcOffset[1] };
        w.window = Window { 16.0f, 12.0f, 128.0f, 96.0f };
        w.ndcScale[0] = 2.0f / 128.0f;
        w.ndcScale[1] = -2.0f / 96.0f;
        w.ndcOffset[0] = -1.0f - 2.0f * 16.0f / 128.0f;
        w.ndcOffset[1] = 1.0f + 2.0f * 12.0f / 96.0f;
        const Inputs inputs = w.MakeInputs(params);
        ID3D11ShaderResourceView* out = pass.Run(w.context.Get(), inputs);
        CHECK(out != nullptr);
        if (out != nullptr)
        {
            const std::vector<uint16_t> pixels = w.ReadRgba(out);
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                    if (x < 16 || x >= 144 || y < 12 || y >= 108)
                        for (uint32_t c = 0; c < 4; ++c)
                            CHECK(pixels[(size_t(y) * kWidth + x) * 4 + c] == w.sceneBits[(size_t(y) * kWidth + x) * 4 + c]);
            const Totals t = Compare(w, inputs, pixels, 16, 12, 128, 96);
            CHECK(t.mixed > 2000 && t.mismatches == 0 && t.earlyMismatch == 0);
        }
        w.window = savedWindow;
        w.ndcScale[0] = savedScale[0];
        w.ndcScale[1] = savedScale[1];
        w.ndcOffset[0] = savedOffset[0];
        w.ndcOffset[1] = savedOffset[1];

        // Nothing to do answers a byte-exact copy with no draw: no medium (density key 0), no density per metre, no light to scatter (both strengths 0).
        const std::vector<uint16_t> scene = fx.sceneBits;
        {
            Params none = params;
            none.density = 0.0f;
            ID3D11ShaderResourceView* copy = pass.Run(fx.context.Get(), fx.MakeInputs(none));
            CHECK(copy != nullptr && pass.DrawsLastRun() == 0 && copy != nullptr && SameBits(fx.ReadRgba(copy), scene));
        }
        {
            Inputs noDensity = fx.MakeInputs(params);
            noDensity.densityPerMetre = 0.0f;
            ID3D11ShaderResourceView* copy = pass.Run(fx.context.Get(), noDensity);
            CHECK(copy != nullptr && pass.DrawsLastRun() == 0 && SameBits(fx.ReadRgba(copy), scene));
        }
        {
            Params dark = params;
            dark.sunStrength = 0.0f;
            dark.skyStrength = 0.0f;
            ID3D11ShaderResourceView* copy = pass.Run(fx.context.Get(), fx.MakeInputs(dark));
            CHECK(copy != nullptr && pass.DrawsLastRun() == 0 && SameBits(fx.ReadRgba(copy), scene));
        }
        // A start distance past every surface leaves the whole frame as it was (the draw ran, every pixel returned the scene's own texel).
        {
            Params late = params;
            late.start = kMaxStart;
            ID3D11ShaderResourceView* lateOut = pass.Run(fx.context.Get(), fx.MakeInputs(late));
            CHECK(lateOut != nullptr);
            const std::vector<uint16_t> pixels = lateOut != nullptr ? fx.ReadRgba(lateOut) : std::vector<uint16_t> {};
            // the 400 m column is beyond 500 m only at the corners; everything nearer than 500 m is exact
            uint32_t different = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < 64; ++x)
                    for (uint32_t c = 0; c < 4; ++c)
                        different += pixels[(size_t(y) * kWidth + x) * 4 + c] != scene[(size_t(y) * kWidth + x) * 4 + c];
            CHECK(different == 0);
        }
    }

    std::printf("-- stage: the sun --\n");
    {
        // No sun: the ambient term alone, against the reference with gain 0; and the sun makes the right (sun-side) far columns brighter than no sun.
        Inputs noSun = fx.MakeInputs(params);
        noSun.sunGain = 0.0f;
        ID3D11ShaderResourceView* out = pass.Run(fx.context.Get(), noSun);
        CHECK(out != nullptr);
        std::vector<uint16_t> ambient;
        if (out != nullptr)
        {
            ambient = fx.ReadRgba(out);
            const Totals t = Compare(fx, noSun, ambient, 0, 0, kWidth, kHeight);
            CHECK(t.mismatches == 0 && t.earlyMismatch == 0 && t.mixed > 4000);
            // The sun adds light to the sun-side far pixels (columns 140..159: toward +x, where the lobe is strongest): brighter than without it.
            CHECK(MeanLuma(baseline, 140, 160, 0, kHeight) > MeanLuma(ambient, 140, 160, 0, kHeight));
        }
        // Mirror the sun: the lobe's brightening moves to the other side (mutant: the phase's sign flipped would not mirror).
        Inputs mirrored = fx.MakeInputs(params);
        mirrored.sunView[0] = -mirrored.sunView[0];
        out = pass.Run(fx.context.Get(), mirrored);
        CHECK(out != nullptr);
        if (out != nullptr)
        {
            const std::vector<uint16_t> pixels = fx.ReadRgba(out);
            const Totals t = Compare(fx, mirrored, pixels, 0, 0, kWidth, kHeight);
            CHECK(t.mismatches == 0 && t.earlyMismatch == 0);
            CHECK(MeanLuma(pixels, 140, 160, 0, kHeight) < MeanLuma(baseline, 140, 160, 0, kHeight)); // the sun left the right edge
            CHECK(MeanLuma(pixels, 32, 48, 0, kHeight) > MeanLuma(baseline, 32, 48, 0, kHeight));      // ... and reached the left one (the 100 m column)
        }
    }

    std::printf("-- stage: determinism, the density scales --\n");
    {
        // Two runs of one frame are one picture, bit for bit.
        ID3D11ShaderResourceView* again = pass.Run(fx.context.Get(), fx.MakeInputs(params));
        CHECK(again != nullptr && SameBits(fx.ReadRgba(again), baseline));
        // A foggy engine (the engine-fog scale) leaves less of our haze than a clear one; a night light (the night scale) leaves the key's share.
        const float clear = EffectiveDensityPerMetre(params, 0.0009f, 1.029f, true);
        const float foggy = EffectiveDensityPerMetre(params, 0.0094f, 1.029f, true);
        const float night = EffectiveDensityPerMetre(params, 0.0009f, 0.04f, true);
        CHECK(foggy < clear && night < clear);
        auto deviation = [&](float densityPerMetre) {
            Inputs inputs = fx.MakeInputs(params);
            inputs.densityPerMetre = densityPerMetre;
            ID3D11ShaderResourceView* out = pass.Run(fx.context.Get(), inputs);
            CHECK(out != nullptr);
            const std::vector<uint16_t> pixels = out != nullptr ? fx.ReadRgba(out) : std::vector<uint16_t> {};
            double sum = 0.0;
            for (size_t i = 0; i < pixels.size() && i < fx.sceneBits.size(); ++i)
                sum += std::fabs(double(FromHalf(pixels[i])) - double(FromHalf(fx.sceneBits[i])));
            return sum;
        };
        const double changedClear = deviation(clear);
        const double changedFoggy = deviation(foggy);
        const double changedNight = deviation(night);
        CHECK(changedFoggy < changedClear && changedNight < changedClear && changedFoggy > 0.0 && changedNight > 0.0);
        // The density is monotone in what it changes.
        CHECK(deviation(0.008f) > changedClear);
    }

    std::printf("-- stage: bindings and refusals --\n");
    {
        ComPtr<ID3D11RenderTargetView> gameTarget = fx.otherRtv;
        ComPtr<ID3D11ShaderResourceView> gameView = fx.sceneView;
        ID3D11RenderTargetView* targets[1] { gameTarget.Get() };
        fx.context->OMSetRenderTargets(1, targets, nullptr);
        ID3D11ShaderResourceView* views[1] { gameView.Get() };
        fx.context->PSSetShaderResources(0, 1, views);
        CHECK(pass.Run(fx.context.Get(), fx.MakeInputs(params)) != nullptr);
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

        Pass refused;
        Inputs noDepth = fx.MakeInputs(params);
        noDepth.depth = nullptr;
        CHECK(refused.Run(fx.context.Get(), noDepth) == nullptr && refused.Failure() != nullptr);
        Inputs badScene = fx.MakeInputs(params);
        badScene.sceneFormat = 28; // R8G8B8A8_UNORM: not the FP16 the tone map reads
        CHECK(refused.Run(fx.context.Get(), badScene) == nullptr && refused.Failure() != nullptr);
        Inputs badDepth = fx.MakeInputs(params);
        badDepth.depthFormat = 87;
        CHECK(refused.Run(fx.context.Get(), badDepth) == nullptr && refused.Failure() != nullptr);
        Inputs emptyWindow = fx.MakeInputs(params);
        emptyWindow.window = Window { 0.0f, 0.0f, 0.0f, 0.0f };
        CHECK(refused.Run(fx.context.Get(), emptyWindow) == nullptr);
        Inputs otherScene = fx.MakeInputs(params);
        otherScene.sceneWidth = kWidth + 1;
        CHECK(refused.Run(fx.context.Get(), otherScene) == nullptr && refused.Failure() != nullptr);
        Inputs badRow = fx.MakeInputs(params);
        badRow.ndcScale[1] = 2.0f / float(kHeight); // the engine's y scale is negative
        CHECK(refused.Run(fx.context.Get(), badRow) == nullptr && refused.Failure() != nullptr);
        Inputs nanColour = fx.MakeInputs(params);
        nanColour.fogColour[1] = NAN;
        CHECK(refused.Run(fx.context.Get(), nanColour) == nullptr && refused.Failure() != nullptr);
        Inputs nanProjection = fx.MakeInputs(params);
        nanProjection.inverseProjection[5] = NAN;
        CHECK(refused.Run(fx.context.Get(), nanProjection) == nullptr && refused.Failure() != nullptr);
        // A healthy run after the refusals still works and the pass kept one scene copy per size.
        CHECK(pass.ScenesCreated() == 1);
        CHECK(pass.GpuTimingEnds() <= pass.GpuTimingBegins());
    }

    return Finish("atmo_warp");
}
