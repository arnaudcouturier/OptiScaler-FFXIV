// Headless shader test for the NR "Linear light + composed" proxy (DlssNr ReversibleMode 5).
// Windows D3D11 WARP executes the shared HLSL; no game or NR DLL is involved.
//
// Reconstructed behaviour (see docs/RECONSTRUCTION-REFERENCE.md section F2): the encode writes the normalised
// frame, clipped to the unit cube, with opaque alpha and no sRGB curve; the resolve then reads the proxy and
// the model back without an sRGB decode, so a model that returns its input leaves the frame unchanged.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "ext/nr/pass/DlssNr_Common.h"
using Microsoft::WRL::ComPtr;

struct Pixel { float r, g, b, a; };
static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D call failed"); }
static void expect(bool ok, const char* label) { if (!ok) throw std::runtime_error(label); }
static bool near3(const Pixel& a, const Pixel& b, float tol)
{
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

int wmain(int argc, wchar_t** argv) try
{
    if (argc != 2)
        throw std::runtime_error("Pass the dlssnr.hlsl path");

    ComPtr<ID3DBlob> code, errors;
    const HRESULT compiled = D3DCompileFromFile(argv[1], nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain",
                                                "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors)
        std::fprintf(stderr, "%s", (char*) errors->GetBufferPointer());
    check(compiled);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                            nullptr, &ctx));
    ComPtr<ID3D11ComputeShader> shader;
    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));

    constexpr UINT kCount = 3;
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = kCount;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    // Three inputs: inside the white point, over it (clips in the proxy), and a dark pixel.
    const std::array<Pixel, kCount> frame { { { 0.50f, 1.00f, 1.50f, 0.30f },
                                              { 4.00f, 2.00f, 1.00f, 0.60f },
                                              { 0.02f, 0.04f, 0.06f, 0.90f } } };
    constexpr float kWhite = 2.0f;

    ComPtr<ID3D11Texture2D> source, model, original, output, keep, readback;
    ComPtr<ID3D11ShaderResourceView> sourceSrv, modelSrv, originalSrv;
    D3D11_SUBRESOURCE_DATA data { frame.data(), sizeof(frame), 0 };
    check(device->CreateTexture2D(&desc, &data, &source));
    check(device->CreateTexture2D(&desc, &data, &model));
    check(device->CreateTexture2D(&desc, &data, &original));
    check(device->CreateShaderResourceView(source.Get(), nullptr, &sourceSrv));
    check(device->CreateShaderResourceView(model.Get(), nullptr, &modelSrv));
    check(device->CreateShaderResourceView(original.Get(), nullptr, &originalSrv));
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    check(device->CreateTexture2D(&desc, nullptr, &output));
    check(device->CreateTexture2D(&desc, nullptr, &keep));
    ComPtr<ID3D11UnorderedAccessView> outputUav, keepUav;
    check(device->CreateUnorderedAccessView(output.Get(), nullptr, &outputUav));
    check(device->CreateUnorderedAccessView(keep.Get(), nullptr, &keepUav));
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&desc, nullptr, &readback));

    D3D11_BUFFER_DESC buffer {};
    buffer.ByteWidth = sizeof(DlssNrConstants);
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants;
    check(device->CreateBuffer(&buffer, nullptr, &constants));
    D3D11_SAMPLER_DESC sampling {};
    sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampling.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    check(device->CreateSamplerState(&sampling, &sampler));

    // t0 source/proxy, t1 model, t2 original, t3 motion and t4 exposure (unused here, bound to something valid).
    ID3D11ShaderResourceView* srvs[] = { sourceSrv.Get(), modelSrv.Get(), originalSrv.Get(), originalSrv.Get(),
                                         originalSrv.Get() };
    ID3D11UnorderedAccessView* uavs[] = { outputUav.Get(), keepUav.Get() };
    ctx->CSSetShader(shader.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 5, srvs);
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
    ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());

    DlssNrConstants settings {};
    settings.Width = kCount;
    settings.Height = 1;
    settings.WhitePoint = kWhite;
    settings.Passthrough = 0;
    settings.ApplyModel = 1;
    settings.TransferStrength = settings.ColourStrength = 1.0f;
    settings.MaxRatio = 8.0f;
    settings.Transfer = 0;
    settings.SkinDetail = settings.SkinColour = settings.EnvironmentDetail = settings.EnvironmentColour = 1.0f;

    auto run = [&]() {
        ctx->UpdateSubresource(constants.Get(), 0, nullptr, &settings, 0, 0);
        ctx->Dispatch(1, 1, 1);
        ctx->CopyResource(readback.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        check(ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::array<Pixel, kCount> result;
        std::memcpy(result.data(), mapped.pData, sizeof(result));
        ctx->Unmap(readback.Get(), 0);
        return result;
    };

    // --- Encode ------------------------------------------------------------------------------------------
    settings.Mode = DlssNrMode_Encode;
    settings.ReversibleMode = 5;
    const auto linear = run();
    for (UINT i = 0; i < kCount; ++i)
    {
        const Pixel want { std::fmin(frame[i].r / kWhite, 1.0f), std::fmin(frame[i].g / kWhite, 1.0f),
                           std::fmin(frame[i].b / kWhite, 1.0f), 1.0f };
        expect(near3(linear[i], want, 0.0001f), "Linear light encode is not saturate(frame / white)");
        expect(linear[i].a == 1.0f, "Linear light encode must write opaque alpha");
    }
    expect(linear[1].r == 1.0f, "Linear light encode must clip above the white point");
    std::puts("PASS: linear light encode = saturate(frame / white point), opaque alpha, clipped above white");

    // The encode must still hand the untouched frame to the keep surface, exactly as the other modes do.
    {
        ctx->CopyResource(readback.Get(), keep.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        check(ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::array<Pixel, kCount> kept;
        std::memcpy(kept.data(), mapped.pData, sizeof(kept));
        ctx->Unmap(readback.Get(), 0);
        for (UINT i = 0; i < kCount; ++i)
            expect(near3(kept[i], frame[i], 0.0001f) && std::abs(kept[i].a - frame[i].a) < 0.0001f,
                   "Linear light encode lost the untouched copy");
    }
    std::puts("PASS: linear light encode keeps the untouched frame");

    // The soft knee (mode 0) is a different curve with the frame's own alpha: the mode switch is live.
    settings.ReversibleMode = 0;
    const auto knee = run();
    expect(!near3(knee[0], linear[0], 0.01f) && std::abs(knee[0].a - frame[0].a) < 0.0001f,
           "Mode 0 must keep its sRGB-encoded soft-knee proxy and the frame's alpha");
    std::puts("PASS: mode 0 still writes the sRGB soft-knee proxy with the frame's alpha");

    // --- Resolve -----------------------------------------------------------------------------------------
    // The proxy and a model that returns it unchanged are the linear-light encode above. Because nothing is
    // decoded, the composition sees proxy == model and gives the frame back (within the OkLab round trip).
    ctx->UpdateSubresource(source.Get(), 0, nullptr, linear.data(), sizeof(linear), 0);
    ctx->UpdateSubresource(model.Get(), 0, nullptr, linear.data(), sizeof(linear), 0);
    settings.Mode = DlssNrMode_Resolve;
    settings.ReversibleMode = 5;
    const auto resolved = run();
    const auto luma = [](const Pixel& p) { return 0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b; };
    for (UINT i = 0; i < kCount; ++i)
    {
        if (i == 1)
        {
            // Over the white point the proxy was clipped, so the model never saw the real colour ratio and
            // saturated highlights may change hue (the documented cost of this mode). The frame's own
            // luminance is still what the composition lands on.
            expect(std::abs(luma(resolved[i]) - luma(frame[i])) < 0.02f * luma(frame[i]),
                   "Linear light resolve lost the luminance of a clipped highlight");
            continue;
        }
        const float tol = 0.002f * std::fmax(1.0f, std::fmax(frame[i].r, std::fmax(frame[i].g, frame[i].b)));
        if (!near3(resolved[i], frame[i], tol))
            std::fprintf(stderr, "pixel %u: got %.5f %.5f %.5f, want %.5f %.5f %.5f\n", i, resolved[i].r,
                         resolved[i].g, resolved[i].b, frame[i].r, frame[i].g, frame[i].b);
        expect(near3(resolved[i], frame[i], tol), "Linear light resolve changed a frame the model left alone");
    }
    std::puts("PASS: linear light resolve returns the frame when the model returns its input (clipped highlights keep luminance)");

    // A darker model answer must darken the frame (the edit has to reach the output).
    std::array<Pixel, kCount> darker = linear;
    for (auto& p : darker) { p.r *= 0.5f; p.g *= 0.5f; p.b *= 0.5f; }
    ctx->UpdateSubresource(model.Get(), 0, nullptr, darker.data(), sizeof(darker), 0);
    const auto edited = run();
    expect(edited[0].r < frame[0].r * 0.9f && edited[0].g < frame[0].g * 0.9f, "Linear light resolve ignored the model");
    std::puts("PASS: linear light resolve applies the model's edit");

    // Negative control: the same linear surfaces read as sRGB (any other mode) do NOT give the frame back.
    ctx->UpdateSubresource(model.Get(), 0, nullptr, linear.data(), sizeof(linear), 0);
    settings.ReversibleMode = 0;
    const auto decoded = run();
    expect(!near3(decoded[0], frame[0], 0.05f), "Mode 0 must still sRGB-decode the proxy");
    std::puts("PASS: other modes still decode the proxy as sRGB (negative control)");

    // Matched residual (Transfer 1 with a residual path): the proxy is rebuilt at full resolution from the frame. For
    // linear light that rebuild is saturate(frame / white) - the same surface the encode wrote - so a model that
    // returns its input still gives the frame back, and an edit still reaches the output.
    settings.Transfer = 1;
    settings.SpatialResidual = 1;
    settings.ReversibleMode = 5;
    const auto matched = run();
    for (const UINT i : { 0u, 2u })
    {
        const float tol = 0.002f * std::fmax(1.0f, std::fmax(frame[i].r, std::fmax(frame[i].g, frame[i].b)));
        if (!near3(matched[i], frame[i], tol))
            std::fprintf(stderr, "matched pixel %u: got %.5f %.5f %.5f, want %.5f %.5f %.5f\n", i, matched[i].r,
                         matched[i].g, matched[i].b, frame[i].r, frame[i].g, frame[i].b);
        expect(near3(matched[i], frame[i], tol), "Linear light matched residual changed a frame the model left alone");
    }
    ctx->UpdateSubresource(model.Get(), 0, nullptr, darker.data(), sizeof(darker), 0);
    const auto matchedEdit = run();
    expect(matchedEdit[0].r < frame[0].r * 0.9f && matchedEdit[0].g < frame[0].g * 0.9f,
           "Linear light matched residual ignored the model");
    // proxy + (model - proxy) is the model when the rebuilt proxy is the encoded one, so the matched-residual result
    // has to equal the classic one (computed above as `edited`). A differently rebuilt proxy would make them differ.
    for (UINT i = 0; i < kCount; ++i)
    {
        const float tol = 0.002f * std::fmax(1.0f, std::fmax(edited[i].r, std::fmax(edited[i].g, edited[i].b)));
        if (!near3(matchedEdit[i], edited[i], tol))
            std::fprintf(stderr, "matched edit pixel %u: got %.5f %.5f %.5f, classic %.5f %.5f %.5f\n", i,
                         matchedEdit[i].r, matchedEdit[i].g, matchedEdit[i].b, edited[i].r, edited[i].g, edited[i].b);
        expect(near3(matchedEdit[i], edited[i], tol), "Linear light matched residual disagrees with the classic transfer");
    }
    std::puts("PASS: linear light matched residual rebuilds the linear proxy (identity, edit equals the classic transfer)");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
