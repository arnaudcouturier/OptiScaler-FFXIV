// Headless shader test: Windows D3D11 WARP executes the NR compose shader with the constants the preview NR recipe builds
// (CharaNrRecipe.h): Encode (pass-through), Resolve and ZeroMotion on a small image. No game, no NR model.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "ext/ffxiv/chara/CharaNrRecipe.h"

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::chara::nr;

namespace
{
struct Pixel
{
    float r, g, b, a;
};
constexpr UINT kW = 4, kH = 1;

void Check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("D3D call failed");
}
void Expect(bool ok, const char* label)
{
    if (!ok)
        throw std::runtime_error(label);
}
bool Near(Pixel a, Pixel b, float tol = 0.0005f)
{
    return std::abs(a.r - b.r) < tol && std::abs(a.g - b.g) < tol && std::abs(a.b - b.b) < tol && std::abs(a.a - b.a) < tol;
}

struct Rig
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Texture2D> readback;
    D3D11_TEXTURE2D_DESC desc {};

    struct Image
    {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
    };

    Image spare;

    void Init(const wchar_t* shaderPath)
    {
        ComPtr<ID3DBlob> code, errors;
        const HRESULT compiled = D3DCompileFromFile(shaderPath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0",
                                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors)
            std::fprintf(stderr, "%s", static_cast<char*>(errors->GetBufferPointer()));
        Check(compiled);
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx));
        Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth = sizeof(DlssNrConstants);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&bd, nullptr, &constants));
        D3D11_SAMPLER_DESC sd {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        Check(device->CreateSamplerState(&sd, &sampler));
        desc.Width = kW;
        desc.Height = kH;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        D3D11_TEXTURE2D_DESC rd = desc;
        rd.Usage = D3D11_USAGE_STAGING;
        rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&rd, nullptr, &readback));
    }

    Image Make(const std::array<Pixel, kW>* pixels, bool uav)
    {
        Image image;
        D3D11_TEXTURE2D_DESC d = desc;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
        D3D11_SUBRESOURCE_DATA data { pixels != nullptr ? pixels->data() : nullptr, sizeof(Pixel) * kW, 0 };
        Check(device->CreateTexture2D(&d, pixels != nullptr ? &data : nullptr, &image.texture));
        Check(device->CreateShaderResourceView(image.texture.Get(), nullptr, &image.srv));
        if (uav)
            Check(device->CreateUnorderedAccessView(image.texture.Get(), nullptr, &image.uav));
        return image;
    }

    std::array<Pixel, kW> Read(const Image& image)
    {
        ctx->CopyResource(readback.Get(), image.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        Check(ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::array<Pixel, kW> out;
        std::memcpy(out.data(), mapped.pData, sizeof(out));
        ctx->Unmap(readback.Get(), 0);
        return out;
    }

    // The binding of DlssNr_Dx12::DispatchPass: t0 source, t1 model, t2 original, t3 motion, t4 previous edit (a missing one is the
    // source); u0 target, u1 keep (a missing one is the target).
    void Dispatch(const DlssNrConstants& c, const Image& source, const Image* model, const Image* original, const Image& target, const Image* keep)
    {
        ID3D11ShaderResourceView* srvs[5] = { source.srv.Get(), model ? model->srv.Get() : source.srv.Get(),
                                              original ? original->srv.Get() : source.srv.Get(), source.srv.Get(), source.srv.Get() };
        // D3D11 does not take one image in two UAV slots (D3D12 does): a missing "keep" image is a spare one.
        if (keep == nullptr)
        {
            if (spare.texture == nullptr)
                spare = Make(nullptr, true);
            keep = &spare;
        }
        ID3D11UnorderedAccessView* uavs[2] = { target.uav.Get(), keep->uav.Get() };
        ID3D11ShaderResourceView* none[5] = {};
        ID3D11UnorderedAccessView* noUav[2] = {};
        ctx->CSSetShader(shader.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 5, srvs);
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->UpdateSubresource(constants.Get(), 0, nullptr, &c, 0, 0);
        ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->Dispatch(1, 1, 1);
        ctx->CSSetShaderResources(0, 5, none);
        ctx->CSSetUnorderedAccessViews(0, 2, noUav, nullptr);
    }
};
} // namespace

int wmain(int argc, wchar_t** argv) try
{
    if (argc != 2)
        throw std::runtime_error("Pass the dlssnr.hlsl path");
    Rig rig;
    rig.Init(argv[1]);

    // A preview image: opaque character pixels and a transparent background with its own (meaningful) alpha.
    const std::array<Pixel, kW> frame { { { 0.75f, 0.50f, 0.40f, 1.0f }, { 0.20f, 0.40f, 0.80f, 1.0f }, { 0.05f, 0.05f, 0.05f, 0.0f }, { 0.60f, 0.30f, 0.10f, 0.5f } } };
    Settings settings; // the defaults of [DlssNr]
    const DlssNrConstants encode = MakeEncode(kW, kH, settings);
    const DlssNrConstants resolve = MakeResolve(kW, kH, settings);

    auto source = rig.Make(&frame, false);
    auto proxy = rig.Make(nullptr, true);
    auto original = rig.Make(nullptr, true);
    auto answer = rig.Make(nullptr, true);
    auto result = rig.Make(nullptr, true);

    // Encode of an already tone-mapped preview is a pass-through: the proxy and the kept original are the frame, alpha included.
    rig.Dispatch(encode, source, nullptr, nullptr, proxy, &original);
    const auto proxyPixels = rig.Read(proxy);
    const auto originalPixels = rig.Read(original);
    for (size_t i = 0; i < kW; ++i)
        Expect(Near(proxyPixels[i], frame[i]) && Near(originalPixels[i], frame[i]), "Encode is not a pass-through of the preview image");
    std::puts("PASS: encode of the preview image is a pass-through that keeps alpha");

    // A model that returns exactly what it was shown leaves the image unchanged, with the original alpha.
    Rig::Image modelOut = rig.Make(&proxyPixels, true);
    rig.Dispatch(resolve, proxy, &modelOut, &original, result, nullptr);
    const auto unchanged = rig.Read(result);
    for (size_t i = 0; i < kW; ++i)
        Expect(Near(unchanged[i], frame[i]), "Resolve of an unchanged model answer altered the image or its alpha");
    std::puts("PASS: resolve of an unchanged answer returns the original image and alpha");

    // A model that brightens pixel 0: the result moves up, never beyond the guard, and keeps alpha; transparent pixels keep alpha 0.
    std::array<Pixel, kW> edited = proxyPixels;
    edited[0] = { 0.80f, 0.55f, 0.45f, 1.0f };
    edited[2] = { 0.10f, 0.10f, 0.10f, 0.0f };
    auto editedImage = rig.Make(&edited, true);
    rig.Dispatch(resolve, proxy, &editedImage, &original, result, nullptr);
    const auto changed = rig.Read(result);
    Expect(changed[0].r > frame[0].r + 0.01f && changed[0].r <= edited[0].r + 0.001f, "The model's edit did not reach the result");
    Expect(changed[1].r == frame[1].r || Near(changed[1], frame[1]), "An untouched pixel changed");
    Expect(changed[0].a == 1.0f && changed[2].a == 0.0f && std::abs(changed[3].a - 0.5f) < 0.0005f, "Alpha is not the original's");

    // ApplyModel off: the clean image, bit for bit (the pass still runs).
    Settings off = settings;
    off.applyModel = false;
    rig.Dispatch(MakeResolve(kW, kH, off), proxy, &editedImage, &original, result, nullptr);
    const auto bypass = rig.Read(result);
    for (size_t i = 0; i < kW; ++i)
        Expect(Near(bypass[i], frame[i]), "ApplyModel=off did not return the clean image");

    // Transfer strength 0: no edit lands.
    Settings none = settings;
    none.transferStrength = 0.0f;
    none.colourStrength = 0.0f;
    rig.Dispatch(MakeResolve(kW, kH, none), proxy, &editedImage, &original, result, nullptr);
    const auto zeroStrength = rig.Read(result);
    for (size_t i = 0; i < kW; ++i)
        Expect(Near(zeroStrength[i], frame[i]), "Strength 0 changed the image");
    std::puts("PASS: resolve applies the edit, keeps alpha, honours ApplyModel and the strengths");

    // ZeroMotion writes zero whatever the target held.
    const std::array<Pixel, kW> junk { { { 1, 2, 3, 4 }, { 5, 6, 7, 8 }, { 9, 10, 11, 12 }, { 13, 14, 15, 16 } } };
    auto motion = rig.Make(&junk, true);
    rig.Dispatch(MakeZeroMotion(kW, kH), source, nullptr, nullptr, motion, nullptr);
    for (const Pixel& p : rig.Read(motion))
        Expect(p.r == 0 && p.g == 0 && p.b == 0 && p.a == 0, "ZeroMotion left a non-zero value");
    std::puts("PASS: zero-motion pass writes zeros");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
