// Engine light controls, the exposure hold's PASS on D3D11 WARP with the real shader and the real Pass class on a synthetic 160x96 FP16 frame.
//
// What a player would feel if it broke, and what is checked:
//   the auto hold multiplies the stored scene by s_hold / s_now with s_now read from the GPU copy of g_CommonParameter (b1): a 1.3x take-back comes back to the held picture
//   within the half-float rounding, the alpha channel is passed through bit for bit, the gain is bounded to 0.5..2, and a copy that fails the fingerprint y = max(1, 2.04 s^2),
//   a NaN exposure or an out-of-band one is gain 1 and the OUTPUT BITS ARE THE INPUT'S (the multiply by 1 is exact);
//   the manual factor scales exactly (0.5 of a half float is exact);
//   no pass at all (null, no failure, no draw) for every identity: mode off, no copy, no hold, a manual factor of 1;
//   the pass covers the render window and a two-pixel margin and writes nothing beyond;
//   the game's render target, t0, constant buffers, rasterizer / blend / depth state and viewport come back after a run;
//   the timing pairs balance; every binding the pass cannot read returns null with a reason.
#include "check.h"

#include "ext/ffxiv/enginelight/EngineLightPass.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
namespace el = ext::ffxiv::enginelight;
namespace sh = ext::ffxiv::shafts;

#define CHECK_SUCCEEDED(call) CHECK(SUCCEEDED(call))

namespace
{
constexpr uint32_t kWidth = 160;
constexpr uint32_t kHeight = 96;

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
    return sign * std::ldexp(float(1024 + mantissa), int(exponent) - 25);
}

bool Near(float a, float b, float relative) { return std::fabs(a - b) <= relative * std::fabs(b) + 1e-6f; }

struct Fixture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> sceneTexture, gameTarget;
    ComPtr<ID3D11ShaderResourceView> sceneView;
    ComPtr<ID3D11RenderTargetView> gameRtv;
    ComPtr<ID3D11Buffer> common, gameBuffer0, gameBuffer1;
    ComPtr<ID3D11RasterizerState> gameRaster;
    ComPtr<ID3D11BlendState> gameBlend;
    ComPtr<ID3D11DepthStencilState> gameDepth;
    std::vector<uint16_t> input;

    bool Create()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
            return false;
        // left half a moonlit stone (0.10 / 0.11 / 0.13, alpha 0.5), right half a lamp (2.0 / 1.8 / 1.2, alpha 1.0)
        input.assign(size_t(kWidth) * kHeight * 4, 0);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const bool lamp = x >= kWidth / 2;
                const size_t at = (size_t(y) * kWidth + x) * 4;
                input[at + 0] = HalfOf(lamp ? 2.0f : 0.10f);
                input[at + 1] = HalfOf(lamp ? 1.8f : 0.11f);
                input[at + 2] = HalfOf(lamp ? 1.2f : 0.13f);
                input[at + 3] = HalfOf(lamp ? 1.0f : 0.5f);
            }
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA initial { input.data(), kWidth * 8, 0 };
        if (FAILED(device->CreateTexture2D(&desc, &initial, &sceneTexture)) || FAILED(device->CreateShaderResourceView(sceneTexture.Get(), nullptr, &sceneView)))
            return false;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &gameTarget)) || FAILED(device->CreateRenderTargetView(gameTarget.Get(), nullptr, &gameRtv)))
            return false;
        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth = 64;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &common)) || FAILED(device->CreateBuffer(&bd, nullptr, &gameBuffer0)) || FAILED(device->CreateBuffer(&bd, nullptr, &gameBuffer1)))
            return false;
        D3D11_RASTERIZER_DESC raster {};
        raster.FillMode = D3D11_FILL_WIREFRAME;
        raster.CullMode = D3D11_CULL_FRONT;
        raster.DepthClipEnable = TRUE;
        D3D11_BLEND_DESC blend {};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = 0xF;
        D3D11_DEPTH_STENCIL_DESC depth {};
        depth.DepthEnable = TRUE;
        depth.DepthFunc = D3D11_COMPARISON_GREATER;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        return SUCCEEDED(device->CreateRasterizerState(&raster, &gameRaster)) && SUCCEEDED(device->CreateBlendState(&blend, &gameBlend)) &&
               SUCCEEDED(device->CreateDepthStencilState(&depth, &gameDepth));
    }

    void WriteExposure(float s, float y)
    {
        float bytes[16] {};
        bytes[12] = s;
        bytes[13] = y;
        bytes[14] = std::numeric_limits<float>::max();
        context->UpdateSubresource(common.Get(), 0, nullptr, bytes, 0, 0);
    }

    // The game's own state around a draw: the target, t0, two constant buffers, rasterizer, blend with a factor and a mask, depth and a viewport.
    void BindGameState()
    {
        ID3D11RenderTargetView* targets[1] { gameRtv.Get() };
        context->OMSetRenderTargets(1, targets, nullptr);
        ID3D11ShaderResourceView* views[1] { sceneView.Get() };
        context->PSSetShaderResources(0, 1, views);
        ID3D11Buffer* buffers[2] { gameBuffer0.Get(), gameBuffer1.Get() };
        context->PSSetConstantBuffers(0, 2, buffers);
        context->RSSetState(gameRaster.Get());
        const float factor[4] { 0.1f, 0.2f, 0.3f, 0.4f };
        context->OMSetBlendState(gameBlend.Get(), factor, 0xABCD1237u);
        context->OMSetDepthStencilState(gameDepth.Get(), 7);
        const D3D11_VIEWPORT viewport { 3.0f, 5.0f, 100.0f, 60.0f, 0.1f, 0.9f };
        context->RSSetViewports(1, &viewport);
    }

    struct StateSnapshot
    {
        ID3D11RenderTargetView* target = nullptr;
        ID3D11ShaderResourceView* t0 = nullptr;
        ID3D11Buffer* cb0 = nullptr;
        ID3D11Buffer* cb1 = nullptr;
        ID3D11RasterizerState* raster = nullptr;
        ID3D11BlendState* blend = nullptr;
        float factor[4] {};
        UINT mask = 0;
        ID3D11DepthStencilState* depth = nullptr;
        UINT stencilRef = 0;
        D3D11_VIEWPORT viewport {};
    };
    StateSnapshot Snapshot()
    {
        StateSnapshot s;
        context->OMGetRenderTargets(1, &s.target, nullptr);
        context->PSGetShaderResources(0, 1, &s.t0);
        ID3D11Buffer* cbs[2] {};
        context->PSGetConstantBuffers(0, 2, cbs);
        s.cb0 = cbs[0];
        s.cb1 = cbs[1];
        context->RSGetState(&s.raster);
        context->OMGetBlendState(&s.blend, s.factor, &s.mask);
        context->OMGetDepthStencilState(&s.depth, &s.stencilRef);
        UINT count = 1;
        context->RSGetViewports(&count, &s.viewport);
        return s;
    }
    static void Release(StateSnapshot& s)
    {
        for (IUnknown* object : { (IUnknown*)s.target, (IUnknown*)s.t0, (IUnknown*)s.cb0, (IUnknown*)s.cb1, (IUnknown*)s.raster, (IUnknown*)s.blend, (IUnknown*)s.depth })
            if (object != nullptr)
                object->Release();
    }

    std::vector<uint16_t> ReadRgba(ID3D11Texture2D* texture)
    {
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        ComPtr<ID3D11Texture2D> staging;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CHECK_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging));
        context->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        CHECK_SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<uint16_t> out(size_t(desc.Width) * desc.Height * 4);
        for (uint32_t y = 0; y < desc.Height; ++y)
            std::memcpy(out.data() + size_t(y) * desc.Width * 4, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, desc.Width * 8);
        context->Unmap(staging.Get(), 0);
        return out;
    }

    el::Inputs AutoInputs(float sHold) const
    {
        el::Inputs in;
        in.scene = sceneView.Get();
        in.sceneFormat = 10;
        in.sceneWidth = kWidth;
        in.sceneHeight = kHeight;
        in.window = sh::Window { 0.0f, 0.0f, float(kWidth), float(kHeight) };
        in.mode = el::kExposureAuto;
        in.sHold = sHold;
        in.exposureCopy = common.Get();
        return in;
    }
};

float PixelOf(const std::vector<uint16_t>& image, uint32_t x, uint32_t y, int channel) { return FromHalf(image[(size_t(y) * kWidth + x) * 4 + channel]); }
} // namespace

int main()
{
    Fixture fx;
    if (!fx.Create())
    {
        std::printf("engine_light_pass_warp: no WARP device\n");
        return 1;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    fx.BindGameState(); // the pass always runs mid-frame: a context with the game's state bound
    el::Pass pass;

    std::printf("-- the auto hold: stored x s_hold / s_now, alpha untouched --\n");
    {
        fx.WriteExposure(1.4f * 1.3f, el::YOf(1.4f * 1.3f));
        ID3D11ShaderResourceView* out = pass.Run(fx.context.Get(), fx.AutoInputs(1.4f));
        CHECK(out != nullptr && pass.Failure() == nullptr);
        CHECK_EQ(pass.DrawsLastRun(), 1u);
        const std::vector<uint16_t> image = fx.ReadRgba(pass.SceneTexture());
        const float gain = 1.0f / 1.3f;
        CHECK(Near(PixelOf(image, 20, 40, 0), 0.10f * gain, 0.002f) && Near(PixelOf(image, 20, 40, 1), 0.11f * gain, 0.002f) && Near(PixelOf(image, 20, 40, 2), 0.13f * gain, 0.002f));
        CHECK(Near(PixelOf(image, 120, 40, 0), 2.0f * gain, 0.002f) && Near(PixelOf(image, 120, 40, 1), 1.8f * gain, 0.002f) && Near(PixelOf(image, 120, 40, 2), 1.2f * gain, 0.002f));
        // alpha: the stone's 0.5 and the lamp's 1.0 come back as the same half bits
        CHECK_EQ(image[(size_t(40) * kWidth + 20) * 4 + 3], fx.input[(size_t(40) * kWidth + 20) * 4 + 3]);
        CHECK_EQ(image[(size_t(40) * kWidth + 120) * 4 + 3], fx.input[(size_t(40) * kWidth + 120) * 4 + 3]);
    }

    std::printf("-- bounded gain (0.5 .. 2) --\n");
    {
        fx.WriteExposure(5.0f, el::YOf(5.0f)); // hold 1.4 / 5.0 = 0.28 -> clamped to 0.5
        pass.Run(fx.context.Get(), fx.AutoInputs(1.4f));
        std::vector<uint16_t> image = fx.ReadRgba(pass.SceneTexture());
        CHECK(Near(PixelOf(image, 20, 40, 1), 0.11f * el::kGainMin, 0.002f));
        fx.WriteExposure(0.3f, el::YOf(0.3f)); // hold 1.4 / 0.3 = 4.67 -> clamped to 2
        pass.Run(fx.context.Get(), fx.AutoInputs(1.4f));
        image = fx.ReadRgba(pass.SceneTexture());
        CHECK(Near(PixelOf(image, 20, 40, 1), 0.11f * el::kGainMax, 0.002f));
    }

    std::printf("-- a copy that fails the fingerprint, a NaN and an out-of-band exposure are gain 1, the input's own bits --\n");
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const struct
        {
            float s, y;
        } bad[] { { 1.9f, 123.0f }, { nan, 1.0f }, { 1.9f, nan }, { 9.0f, el::YOf(9.0f) }, { 0.1f, 1.0f } };
        for (const auto& b : bad)
        {
            fx.WriteExposure(b.s, b.y);
            ID3D11ShaderResourceView* out = pass.Run(fx.context.Get(), fx.AutoInputs(1.4f));
            CHECK(out != nullptr);
            const std::vector<uint16_t> image = fx.ReadRgba(pass.SceneTexture());
            bool same = true;
            for (size_t i = 0; i < image.size(); ++i)
                same = same && image[i] == fx.input[i];
            CHECK(same);
        }
    }

    std::printf("-- the manual factor scales exactly; a factor of 1 is no pass --\n");
    {
        el::Inputs in = fx.AutoInputs(0.0f);
        in.mode = el::kExposureManual;
        in.manualGain = 0.5f;
        in.exposureCopy = nullptr;
        ID3D11ShaderResourceView* out = pass.Run(fx.context.Get(), in);
        CHECK(out != nullptr);
        const std::vector<uint16_t> image = fx.ReadRgba(pass.SceneTexture());
        bool exact = true;
        for (size_t i = 0; i < image.size(); i += 4)
            for (int c = 0; c < 3; ++c)
                exact = exact && FromHalf(image[i + c]) == FromHalf(fx.input[i + c]) * 0.5f;
        CHECK(exact);
        in.manualGain = 1.0f;
        CHECK(pass.Run(fx.context.Get(), in) == nullptr && pass.Failure() == nullptr && pass.DrawsLastRun() == 0u);
    }

    std::printf("-- every identity is no pass: null, no failure, no draw --\n");
    {
        fx.WriteExposure(1.9f, el::YOf(1.9f));
        el::Inputs noCopy = fx.AutoInputs(1.4f);
        noCopy.exposureCopy = nullptr;
        CHECK(pass.Run(fx.context.Get(), noCopy) == nullptr && pass.Failure() == nullptr && pass.DrawsLastRun() == 0u);
        el::Inputs noHold = fx.AutoInputs(0.0f);
        CHECK(pass.Run(fx.context.Get(), noHold) == nullptr && pass.Failure() == nullptr && pass.DrawsLastRun() == 0u);
        el::Inputs off = fx.AutoInputs(1.4f);
        off.mode = el::kExposureOff;
        CHECK(pass.Run(fx.context.Get(), off) == nullptr && pass.Failure() == nullptr && pass.DrawsLastRun() == 0u);
    }

    std::printf("-- the pass covers the window and a two-pixel margin and writes nothing beyond --\n");
    {
        el::Pass windowed;
        fx.WriteExposure(1.4f * 1.3f, el::YOf(1.4f * 1.3f));
        el::Inputs in = fx.AutoInputs(1.4f);
        in.window = sh::Window { 40.0f, 12.0f, 80.0f, 64.0f };
        CHECK(windowed.Run(fx.context.Get(), in) != nullptr);
        const std::vector<uint16_t> image = fx.ReadRgba(windowed.SceneTexture());
        CHECK(Near(PixelOf(image, 41, 13, 1), 0.11f / 1.3f, 0.002f));        // inside the window
        CHECK(Near(PixelOf(image, 38, 10, 1), 0.11f / 1.3f, 0.002f));        // inside the two-pixel margin
        CHECK(PixelOf(image, 36, 8, 1) == 0.0f && PixelOf(image, 150, 90, 1) == 0.0f);  // beyond it: untouched (the pass's own texture starts empty)
    }

    std::printf("-- the game's state comes back after a run, the timing pairs balance --\n");
    {
        fx.WriteExposure(1.4f * 1.3f, el::YOf(1.4f * 1.3f));
        fx.BindGameState();
        Fixture::StateSnapshot before = fx.Snapshot();
        for (int i = 0; i < 3; ++i)
            CHECK(pass.Run(fx.context.Get(), fx.AutoInputs(1.4f)) != nullptr);
        Fixture::StateSnapshot after = fx.Snapshot();
        CHECK(before.target == after.target && before.t0 == after.t0 && before.cb0 == after.cb0 && before.cb1 == after.cb1);
        CHECK(before.raster == after.raster && before.blend == after.blend && before.depth == after.depth);
        CHECK(std::memcmp(before.factor, after.factor, sizeof before.factor) == 0 && before.mask == after.mask && before.stencilRef == after.stencilRef);
        CHECK(std::memcmp(&before.viewport, &after.viewport, sizeof before.viewport) == 0);
        Fixture::Release(before);
        Fixture::Release(after);
        CHECK_EQ(pass.GpuTimingBegins(), pass.GpuTimingEnds());
        CHECK(pass.GpuTimingBegins() >= 1u);
    }

    std::printf("-- a binding the pass cannot read returns null with a reason --\n");
    {
        el::Inputs wrongFormat = fx.AutoInputs(1.4f);
        wrongFormat.sceneFormat = 28;
        CHECK(pass.Run(fx.context.Get(), wrongFormat) == nullptr && pass.Failure() != nullptr);
        el::Inputs badWindow = fx.AutoInputs(1.4f);
        badWindow.window = sh::Window { 0.0f, 0.0f, 0.0f, 0.0f };
        CHECK(pass.Run(fx.context.Get(), badWindow) == nullptr && pass.Failure() != nullptr);
        el::Inputs wrongSize = fx.AutoInputs(1.4f);
        wrongSize.sceneWidth = kWidth / 2;
        CHECK(pass.Run(fx.context.Get(), wrongSize) == nullptr && pass.Failure() != nullptr);
        CHECK(pass.Run(fx.context.Get(), el::Inputs {}) == nullptr && pass.Failure() != nullptr);
    }

    return Finish("engine_light_pass_warp");
}
