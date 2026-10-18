// The debug tint of preview NR on D3D11 WARP: the real CharaNrTint.cpp and CharaGuides.cpp with chara_guides.hlsl compiled from the
// repository (argument 1 = repository root).
//   * the bring-up result (an RGBA16F image) is tinted cyan by one extra pass into a private target; the result itself stays as it was
//     (it is shared with the D3D12 queue and must not be written by the D3D11 side), the alpha is kept
//   * one pass per result (serial), not one per UI draw; a new result draws again; another image has its own target; a size change
//     rebuilds the target; unused targets are released
//   * without the cyan shader no tint is claimed (the caller then counts nothing and gives the untinted result)
//   * the alpha pass of the super-resolution path makes the cyan image when its colour came from NR, magenta for plain SR, and the plain
//     image without the tint (the production result)
#include "check.h"

#include "ext/ffxiv/chara/CharaGuides.h"
#include "ext/ffxiv/chara/CharaNrTint.h"

#include <d3dcompiler.h>
#include <DirectXPackedVector.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::chara;

namespace
{
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;

bool Ok(HRESULT hr, const char* what)
{
    if (SUCCEEDED(hr))
        return true;
    std::printf("FAIL: %s (0x%08lX)\n", what, static_cast<unsigned long>(hr));
    ++g_failures;
    return false;
}

ComPtr<ID3DBlob> Compile(const std::wstring& path, const char* entry, const char* profile)
{
    ComPtr<ID3DBlob> code, error;
    const HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, nullptr, entry, profile, 0, 0, &code, &error);
    if (FAILED(hr))
    {
        std::printf("FAIL: compile %s: %s\n", entry, error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
        ++g_failures;
    }
    return code;
}

guides::Bytecode Of(const ComPtr<ID3DBlob>& b) { return { b->GetBufferPointer(), b->GetBufferSize() }; }
void DrawTriangle(ID3D11DeviceContext* c) { c->Draw(3, 0); }
bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

struct Tex
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};

using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;

// A float image whose every pixel is (r, g, b, a).
std::vector<uint16_t> HalfImage(UINT w, UINT h, float r, float g, float b, float a)
{
    std::vector<uint16_t> data(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < data.size(); i += 4)
    {
        data[i + 0] = XMConvertFloatToHalf(r);
        data[i + 1] = XMConvertFloatToHalf(g);
        data[i + 2] = XMConvertFloatToHalf(b);
        data[i + 3] = XMConvertFloatToHalf(a);
    }
    return data;
}

Tex MakeTexture(DXGI_FORMAT format, UINT w, UINT h, const void* data = nullptr, UINT pitch = 0)
{
    Tex t;
    D3D11_TEXTURE2D_DESC td {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA init { data, pitch, 0 };
    Ok(g_device->CreateTexture2D(&td, data ? &init : nullptr, &t.texture), "CreateTexture2D");
    Ok(g_device->CreateShaderResourceView(t.texture.Get(), nullptr, &t.srv), "CreateSRV");
    Ok(g_device->CreateRenderTargetView(t.texture.Get(), nullptr, &t.rtv), "CreateRTV");
    return t;
}

// Reads a texture back (packed rows).
std::vector<uint8_t> Read(ID3D11Texture2D* texture, UINT bytesPerPixel)
{
    D3D11_TEXTURE2D_DESC td {};
    texture->GetDesc(&td);
    const UINT w = td.Width, h = td.Height;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * bytesPerPixel);
    if (!Ok(g_device->CreateTexture2D(&td, nullptr, &staging), "staging"))
        return out;
    g_context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE m {};
    if (!Ok(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "Map"))
        return out;
    for (UINT y = 0; y < h; ++y)
        std::memcpy(out.data() + static_cast<size_t>(y) * w * bytesPerPixel, static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                    static_cast<size_t>(w) * bytesPerPixel);
    g_context->Unmap(staging.Get(), 0);
    return out;
}

struct Rgba
{
    float r, g, b, a;
};

// Pixel (x, y) of a float image.
Rgba HalfAt(const std::vector<uint8_t>& bytes, UINT w, UINT x, UINT y)
{
    const uint16_t* p = reinterpret_cast<const uint16_t*>(bytes.data()) + (static_cast<size_t>(y) * w + x) * 4;
    return { XMConvertHalfToFloat(p[0]), XMConvertHalfToFloat(p[1]), XMConvertHalfToFloat(p[2]), XMConvertHalfToFloat(p[3]) };
}

bool IsCyanMix(const Rgba& p, float r, float g, float b, float a)
{
    // lerp(colour, (0, 1, 1), 0.35); the alpha is the source's
    return Near(p.r, r * 0.65f, 0.003f) && Near(p.g, g * 0.65f + 0.35f, 0.003f) && Near(p.b, b * 0.65f + 0.35f, 0.003f) && Near(p.a, a, 0.002f);
}

bool IsUntouched(const Rgba& p, float r, float g, float b, float a)
{
    return Near(p.r, r, 0.001f) && Near(p.g, g, 0.001f) && Near(p.b, b, 0.001f) && Near(p.a, a, 0.001f);
}

constexpr UINT kW = 8, kH = 6;
constexpr float kR = 0.2f, kG = 0.4f, kB = 0.6f, kA = 0.5f;

void TestTintPass(guides::Passes& passes)
{
    nr::TintTargets targets;
    const auto data = HalfImage(kW, kH, kR, kG, kB, kA);
    Tex result = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, kW, kH, data.data(), kW * 8);

    bool drawn = false;
    ComPtr<ID3D11ShaderResourceView> first = targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x1000, result.srv.Get(), 1, 100, &drawn);
    CHECK(first != nullptr && drawn && targets.Count() == 1);
    if (first == nullptr)
        return;
    ComPtr<ID3D11Resource> firstResource;
    first->GetResource(&firstResource);
    ComPtr<ID3D11Texture2D> firstTexture;
    firstResource.As(&firstTexture);
    // the tinted copy is cyan-mixed and keeps the alpha ...
    auto bytes = Read(firstTexture.Get(), 8);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x)
            CHECK(IsCyanMix(HalfAt(bytes, kW, x, y), kR, kG, kB, kA));
    // ... and the result itself is not written (it is owned by the D3D12 queue's pass)
    bytes = Read(result.texture.Get(), 8);
    CHECK(IsUntouched(HalfAt(bytes, kW, 0, 0), kR, kG, kB, kA) && IsUntouched(HalfAt(bytes, kW, kW - 1, kH - 1), kR, kG, kB, kA));
    // the view is of the target, not of the result
    CHECK(firstTexture.Get() != result.texture.Get());

    // the same result again (the next UI draw that samples the image): the target is reused, no pass
    const auto other = HalfImage(kW, kH, 0.9f, 0.9f, 0.9f, 1.0f);
    g_context->UpdateSubresource(result.texture.Get(), 0, nullptr, other.data(), kW * 8, 0);
    ComPtr<ID3D11ShaderResourceView> second = targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x1000, result.srv.Get(), 1, 110, &drawn);
    CHECK(second.Get() == first.Get() && !drawn);
    bytes = Read(firstTexture.Get(), 8);
    CHECK(IsCyanMix(HalfAt(bytes, kW, 3, 2), kR, kG, kB, kA)); // still the first result's tint: the second call drew nothing

    // a new result (new serial) draws again, into the same target
    ComPtr<ID3D11ShaderResourceView> third = targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x1000, result.srv.Get(), 2, 120, &drawn);
    CHECK(third.Get() == first.Get() && drawn);
    bytes = Read(firstTexture.Get(), 8);
    CHECK(IsCyanMix(HalfAt(bytes, kW, 3, 2), 0.9f, 0.9f, 0.9f, 1.0f));

    // another preview image has its own target
    ComPtr<ID3D11ShaderResourceView> elsewhere = targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x2000, result.srv.Get(), 3, 130, &drawn);
    CHECK(elsewhere != nullptr && elsewhere.Get() != first.Get() && drawn && targets.Count() == 2);

    // a result of another size rebuilds the target of that image
    const auto big = HalfImage(kW * 2, kH * 2, kR, kG, kB, kA);
    Tex bigResult = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, kW * 2, kH * 2, big.data(), kW * 2 * 8);
    ComPtr<ID3D11ShaderResourceView> resized = targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x1000, bigResult.srv.Get(), 4, 140, &drawn);
    CHECK(resized != nullptr && drawn);
    ComPtr<ID3D11Resource> resizedResource;
    resized->GetResource(&resizedResource);
    ComPtr<ID3D11Texture2D> resizedTexture;
    resizedResource.As(&resizedTexture);
    D3D11_TEXTURE2D_DESC rd {};
    resizedTexture->GetDesc(&rd);
    CHECK(rd.Width == kW * 2 && rd.Height == kH * 2);
    bytes = Read(resizedTexture.Get(), 8);
    CHECK(IsCyanMix(HalfAt(bytes, kW * 2, kW * 2 - 1, kH * 2 - 1), kR, kG, kB, kA));

    // bad input never claims a tint
    CHECK(targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x1000, nullptr, 5, 150, &drawn) == nullptr && !drawn);
    CHECK(targets.Tinted(passes, g_device.Get(), g_context.Get(), 0, result.srv.Get(), 5, 150, &drawn) == nullptr && !drawn);
    CHECK(targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x1000, result.srv.Get(), 0, 150, &drawn) == nullptr && !drawn);

    // unused targets are released: 0x1000 was asked for at 140, 0x2000 at 130
    targets.Release(5000 + 135, 5000);
    CHECK(targets.Count() == 1); // 0x2000 (asked at 130) is older than 5 s, 0x1000 (140) is not
    targets.Release(5000 + 141, 5000);
    CHECK(targets.Count() == 0);
    targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x3000, result.srv.Get(), 6, 200, &drawn);
    targets.Clear();
    CHECK(targets.Count() == 0);
}

void TestNoShader(const ComPtr<ID3DBlob>& vs, const ComPtr<ID3DBlob>& copy, const ComPtr<ID3DBlob>& guidesPs, const ComPtr<ID3DBlob>& alpha,
                  const ComPtr<ID3DBlob>& alphaTint)
{
    // The cyan shader was not supplied: no tint is claimed, and the result is not touched.
    guides::Passes passes;
    CHECK(passes.Init(g_device.Get(), guides::Shaders { Of(vs), Of(copy), Of(guidesPs), Of(alpha), Of(alphaTint) }, DrawTriangle));
    nr::TintTargets targets;
    const auto data = HalfImage(kW, kH, kR, kG, kB, kA);
    Tex result = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, kW, kH, data.data(), kW * 8);
    bool drawn = true;
    CHECK(targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x1000, result.srv.Get(), 1, 100, &drawn) == nullptr && !drawn);

    // The same result is not tried again at every UI draw that samples it: one attempt, then nothing until a new result comes
    CHECK(targets.Attempts() == 1);
    for (int draw = 0; draw < 5; ++draw)
        CHECK(targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x1000, result.srv.Get(), 1, 101 + draw, &drawn) == nullptr && !drawn);
    CHECK(targets.Attempts() == 1);
    CHECK(targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x1000, result.srv.Get(), 2, 110, &drawn) == nullptr && !drawn); // a new result: one more try
    CHECK(targets.Attempts() == 2);
    CHECK(targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x1000, result.srv.Get(), 2, 111, &drawn) == nullptr);
    CHECK(targets.Attempts() == 2);
    // another image has its own memory
    CHECK(targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x2000, result.srv.Get(), 2, 112, &drawn) == nullptr && targets.Attempts() == 3);
    CHECK(targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x2000, result.srv.Get(), 2, 113, &drawn) == nullptr && targets.Attempts() == 3);
}

// The alpha pass of the super-resolution path with an NR colour (the production result): cyan; with the SR colour: magenta; plain without.
void TestAlphaInputs(guides::Passes& passes)
{
    constexpr UINT w = 4, h = 4;
    std::vector<uint8_t> original(w * h * 4, 0);
    for (UINT i = 0; i < w * h; ++i)
        original[i * 4 + 3] = 200;
    const auto nrColour = HalfImage(w * 2, h * 2, 0.3f, 0.5f, 0.7f, 1.0f); // the production result: RGBA16F at twice the size
    Tex orig = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, original.data(), w * 4);
    Tex nrImage = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, w * 2, h * 2, nrColour.data(), w * 2 * 8);
    Tex plain = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, w * 2, h * 2);
    Tex cyan = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, w * 2, h * 2);
    Tex magenta = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, w * 2, h * 2);
    guides::Settings settings;
    settings.invWidth = 1.0f / w;
    settings.invHeight = 1.0f / h;
    {
        guides::Passes::Scope scope(passes, g_context.Get(), settings, nullptr, 0, 0);
        CHECK(scope.Ok());
        CHECK(scope.Alpha(nrImage.srv.Get(), orig.srv.Get(), plain.rtv.Get(), w * 2, h * 2, guides::Tint::None));
        CHECK(scope.Alpha(nrImage.srv.Get(), orig.srv.Get(), cyan.rtv.Get(), w * 2, h * 2, guides::Tint::Nr));
        CHECK(scope.Alpha(nrImage.srv.Get(), orig.srv.Get(), magenta.rtv.Get(), w * 2, h * 2, guides::Tint::Sr));
    }
    const auto p = Read(plain.texture.Get(), 4), c = Read(cyan.texture.Get(), 4), m = Read(magenta.texture.Get(), 4);
    // colour 0.3, 0.5, 0.7 -> 77, 128, 179; alpha from the native image (200)
    CHECK(Near(p[0], 77.0f, 1.5f) && Near(p[1], 128.0f, 1.5f) && Near(p[2], 179.0f, 1.5f) && p[3] == 200);
    // cyan: lerp(c, (0,1,1), 0.35) = (0.195, 0.675, 0.805) -> (50, 172, 205)
    CHECK(Near(c[0], 50.0f, 1.5f) && Near(c[1], 172.0f, 1.5f) && Near(c[2], 205.0f, 1.5f) && c[3] == 200);
    // magenta: lerp(c, (1,0,1), 0.35) = (0.545, 0.325, 0.805) -> (139, 83, 205)
    CHECK(Near(m[0], 139.0f, 1.5f) && Near(m[1], 83.0f, 1.5f) && Near(m[2], 205.0f, 1.5f) && m[3] == 200);
    CHECK(!(Near(c[0], m[0], 3.0f) && Near(c[1], m[1], 3.0f))); // the screen can tell the two apart
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: chara_nr_tint_warp <repository root>\n");
        return 2;
    }
    const std::wstring shader = std::wstring(argv[1], argv[1] + std::strlen(argv[1])) + L"/OptiScaler/ext/ffxiv/chara/shaders/chara_guides.hlsl";
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (!Ok(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &g_device, nullptr, &g_context),
            "D3D11CreateDevice(WARP)"))
        return 1;

    auto vs = Compile(shader, "VS", "vs_5_0");
    auto copy = Compile(shader, "Copy", "ps_5_0");
    auto guidesPs = Compile(shader, "Guides", "ps_5_0");
    auto alpha = Compile(shader, "Alpha", "ps_5_0");
    auto alphaTint = Compile(shader, "AlphaTint", "ps_5_0");
    auto alphaTintNr = Compile(shader, "AlphaTintNr", "ps_5_0");
    if (!vs || !copy || !guidesPs || !alpha || !alphaTint || !alphaTintNr)
        return 1;

    guides::Passes passes;
    CHECK(passes.Init(g_device.Get(), guides::Shaders { Of(vs), Of(copy), Of(guidesPs), Of(alpha), Of(alphaTint), Of(alphaTintNr) }, DrawTriangle));
    if (!passes.Ready())
        return 1;

    TestTintPass(passes);
    TestAlphaInputs(passes);
    TestNoShader(vs, copy, guidesPs, alpha, alphaTint);
    return TEST_RESULT("chara_nr_tint_warp");
}
