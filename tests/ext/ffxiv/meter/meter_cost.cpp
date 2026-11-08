// Effect meter (METER): the GPU cost of one sample, measured on this machine's GPU (local class, Gpu). The pass times itself with a disjoint timestamp set that covers its draw and
// its draw plus the corner copy to the staging texture (the whole GPU cost of one sample).
//
// The GPU is kept BUSY the way a running game keeps it: before every sample two heavy 3840x2160 draws are queued ahead of it, so the clocks are up and the meter's own work is
// timed in the command stream between them (timestamps are taken in order, so the heavy draws before it do not count). An idle-GPU measurement (sleep between samples) reads the
// clock ramp, not the pass: the first version of this test did that and read 0.2-0.4 ms.
//
// The test fails when the median total is above the brief's budget (0.05 ms per sampled frame; one effect is sampled per frame, so one sample is a frame's whole cost).
// Nothing here touches the game; the game must be absent (the runner takes the build mutex and refuses to run beside ffxiv_dx11).
#include "check.h"

#include <windows.h>

#include "ext/ffxiv/meter/MeterPass.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
namespace mt = ext::ffxiv::meter;
using mt::StageId;

namespace
{
ComPtr<ID3D11ShaderResourceView> Picture(ID3D11Device* device, uint32_t w, uint32_t h, DXGI_FORMAT format, uint32_t bytesPerTexel, uint32_t salt)
{
    std::vector<uint8_t> bytes(size_t(w) * h * bytesPerTexel);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = uint8_t((i * 2654435761u + salt) >> 13);
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) // keep the halves finite and below 1: the exponent bits of every second byte stay small
        for (size_t i = 1; i < bytes.size(); i += 2)
            bytes[i] = uint8_t(bytes[i] & 0x37);
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial { bytes.data(), w * bytesPerTexel, 0 };
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    CHECK(SUCCEEDED(device->CreateTexture2D(&desc, &initial, &texture)));
    if (texture)
        CHECK(SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view)));
    return view;
}

const char* kHeavy = R"(
float4 VS(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); }
float4 PS(float4 p : SV_Position) : SV_Target
{
    float x = p.x * 0.001 + p.y * 0.0007;
    [loop] for (int i = 0; i < 1500; ++i) x = sin(x) * 1.0003 + cos(x * 1.3) * 0.5;
    return float4(x, 0, 0, 1);
}
)";
} // namespace

int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context)))
    {
        std::printf("meter_cost: no hardware adapter\n");
        return 1;
    }
    // the load: a heavy full-screen draw into a 3840x2160 target
    ComPtr<ID3D11VertexShader> heavyVs;
    ComPtr<ID3D11PixelShader> heavyPs;
    ComPtr<ID3D11Texture2D> loadTexture;
    ComPtr<ID3D11RenderTargetView> loadRtv;
    {
        ComPtr<ID3DBlob> vs, ps, errors;
        CHECK(SUCCEEDED(D3DCompile(kHeavy, std::strlen(kHeavy), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &errors)));
        CHECK(SUCCEEDED(D3DCompile(kHeavy, std::strlen(kHeavy), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &errors)));
        if (!vs || !ps)
            return Finish("meter_cost");
        CHECK(SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &heavyVs)));
        CHECK(SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &heavyPs)));
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = 3840;
        desc.Height = 2160;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        CHECK(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &loadTexture)));
        CHECK(SUCCEEDED(device->CreateRenderTargetView(loadTexture.Get(), nullptr, &loadRtv)));
    }
    const auto queueLoad = [&]() {
        const D3D11_VIEWPORT viewport { 0.0f, 0.0f, 3840.0f, 2160.0f, 0.0f, 1.0f };
        ID3D11RenderTargetView* rtv = loadRtv.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(heavyVs.Get(), nullptr, 0);
        context->PSSetShader(heavyPs.Get(), nullptr, 0);
        context->Draw(3, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    };

    struct Case
    {
        const char* name;
        StageId id;
        uint32_t w, h;
        DXGI_FORMAT format;
        uint32_t bytes;
    };
    const Case cases[] {
        { "chain stage, FP16 3840x2160 (the window the live sessions had)", StageId::Gi, 3840, 2160, DXGI_FORMAT_R16G16B16A16_FLOAT, 8 },
        { "chain stage, FP16 1920x1080", StageId::Atmosphere, 1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT, 8 },
        { "occlusion, B8G8R8A8 1920x1080", StageId::Ao, 1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM, 4 },
        { "sun mask, R8 3840x2160", StageId::ContactShadows, 3840, 2160, DXGI_FORMAT_R8_UNORM, 1 },
    };
    for (const Case& c : cases)
    {
        auto in = Picture(device.Get(), c.w, c.h, c.format, c.bytes, 1);
        auto out = Picture(device.Get(), c.w, c.h, c.format, c.bytes, 2);
        if (!in || !out)
            continue;
        mt::Pass pass;
        std::vector<double> total, draw;
        for (int i = 0; i < 160; ++i)
        {
            queueLoad();
            queueLoad();
            mt::Request request;
            request.id = c.id;
            request.in = in.Get();
            request.out = out.Get();
            request.seed = uint32_t(i);
            if (!pass.Start(context.Get(), request))
            {
                std::printf("  start failed: %s\n", pass.Failure() != nullptr ? pass.Failure() : "?");
                CHECK(false);
                break;
            }
            queueLoad(); // the GPU never runs dry behind the sample either
            mt::Result result;
            bool got = false;
            for (int k = 0; k < 20000 && !got; ++k)
            {
                got = pass.Collect(context.Get(), result);
                if (!got)
                    context->Flush();
            }
            CHECK(got);
            if (got && result.haveGpuMs && i >= 20) // the first runs warm the clocks
            {
                total.push_back(result.gpuMs);
                draw.push_back(result.gpuDrawMs);
            }
        }
        if (total.empty())
        {
            std::printf("  %s: no timing came back\n", c.name);
            CHECK(false);
            continue;
        }
        std::sort(total.begin(), total.end());
        std::sort(draw.begin(), draw.end());
        const double median = total[total.size() / 2], p95 = total[total.size() * 95 / 100], worst = total.back();
        std::printf("  %-62s draw+copy median %.4f ms  p95 %.4f  max %.4f | draw alone median %.4f ms  (%zu samples)\n", c.name, median, p95, worst, draw[draw.size() / 2],
                    total.size());
        CHECK(median <= 0.05);
    }
    return Finish("meter_cost");
}
