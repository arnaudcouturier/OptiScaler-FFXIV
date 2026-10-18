// Helper passes of the character preview DLSS path on D3D11 WARP: the shader chara_guides.hlsl is compiled from the repository
// (argument 1 = repository root) and run through the real CharaGuides.cpp, including the private context-state swap.
#include "check.h"
#include "ext/ffxiv/chara/CharaGuides.h"

#include <d3dcompiler.h>
#include <DirectXPackedVector.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::chara::guides;

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

Bytecode Of(const ComPtr<ID3DBlob>& b) { return { b->GetBufferPointer(), b->GetBufferSize() }; }

struct Tex
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};

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

// Copies a texture to a staging texture and returns its bytes (row pitch packed).
std::vector<uint8_t> Read(ID3D11Texture2D* texture, UINT bytesPerPixel, UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC td {};
    texture->GetDesc(&td);
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

bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

void DrawTriangle(ID3D11DeviceContext* c) { c->Draw(3, 0); }

constexpr UINT kW = 4, kH = 4;

ComPtr<ID3D11Buffer> MakeCamera(float zx, float zy, int vectors = 59, int offset = 0)
{
    std::vector<float> data(static_cast<size_t>(vectors) * 4);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = 7.0f; // garbage in every element except the one the shader reads
    data[(offset + 58) * 4 + 2] = zx;
    data[(offset + 58) * 4 + 3] = zy;
    D3D11_BUFFER_DESC bd {};
    bd.ByteWidth = static_cast<UINT>(data.size() * sizeof(float));
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init { data.data(), 0, 0 };
    ComPtr<ID3D11Buffer> buffer;
    Ok(g_device->CreateBuffer(&bd, &init, &buffer), "camera buffer");
    return buffer;
}

void TestGuides(Passes& passes, ID3D11Buffer* cameraBuffer, UINT first, UINT count)
{
    // Depth: a 24-bit depth texture cleared to 0.25, read through the R24_UNORM_X8 view like the game's depth.
    D3D11_TEXTURE2D_DESC dd {};
    dd.Width = kW;
    dd.Height = kH;
    dd.MipLevels = dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
    dd.SampleDesc.Count = 1;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> depthTex;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> depthSrv;
    Ok(g_device->CreateTexture2D(&dd, nullptr, &depthTex), "depth texture");
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd {};
    dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    Ok(g_device->CreateDepthStencilView(depthTex.Get(), &dvd, &dsv), "DSV");
    D3D11_SHADER_RESOURCE_VIEW_DESC svd {};
    svd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    Ok(g_device->CreateShaderResourceView(depthTex.Get(), &svd, &depthSrv), "depth SRV");
    g_context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.25f, 0);

    // Motion (RGBA16F): pixel 0 plain, 1 NaN, 2 +inf, 3 large, 4 camera offset exactly, the rest zero.
    using DirectX::PackedVector::XMConvertFloatToHalf;
    const uint16_t kNan = 0x7E00, kInf = 0x7C00;
    std::vector<uint16_t> motion(kW * kH * 4, 0);
    auto put = [&](int px, uint16_t x, uint16_t y) {
        motion[static_cast<size_t>(px) * 4 + 0] = x;
        motion[static_cast<size_t>(px) * 4 + 1] = y;
    };
    put(0, XMConvertFloatToHalf(0.75f), XMConvertFloatToHalf(0.0f));
    put(1, kNan, XMConvertFloatToHalf(1.0f));
    put(2, kInf, XMConvertFloatToHalf(0.0f));
    put(3, XMConvertFloatToHalf(10.0f), XMConvertFloatToHalf(-10.0f));
    put(4, XMConvertFloatToHalf(0.5f), XMConvertFloatToHalf(-0.25f));
    put(5, XMConvertFloatToHalf(0.0f), kNan); // the second component alone makes the pair invalid
    Tex motionTex = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, kW, kH, motion.data(), kW * 8);

    Tex depthOut = MakeTexture(DXGI_FORMAT_R32_FLOAT, kW, kH);
    Tex motionOut = MakeTexture(DXGI_FORMAT_R16G16_FLOAT, kW, kH);


    {
        Settings settings;
        Passes::Scope scope(passes, g_context.Get(), settings, cameraBuffer, first, count);
        CHECK(scope.Ok());
        CHECK(scope.Guides(motionTex.srv.Get(), depthSrv.Get(), depthOut.rtv.Get(), motionOut.rtv.Get(), kW, kH));
    }

    const auto depth = Read(depthOut.texture.Get(), 4, kW, kH);
    const auto mv = Read(motionOut.texture.Get(), 4, kW, kH);
    for (UINT i = 0; i < kW * kH; ++i)
    {
        float d;
        std::memcpy(&d, depth.data() + i * 4, 4);
        CHECK(Near(d, 0.25f, 1e-5f)); // depth copy
    }
    auto mvAt = [&](int px, int comp) {
        uint16_t h;
        std::memcpy(&h, mv.data() + px * 4 + comp * 2, 2);
        return DirectX::PackedVector::XMConvertHalfToFloat(h);
    };
    CHECK(Near(mvAt(0, 0), 0.25f, 1e-3f) && Near(mvAt(0, 1), 0.25f, 1e-3f));   // motion minus the camera offset
    CHECK(mvAt(1, 0) == 0.0f && mvAt(1, 1) == 0.0f);                           // NaN -> (0, 0)
    CHECK(mvAt(2, 0) == 0.0f && mvAt(2, 1) == 0.0f);                           // inf -> (0, 0)
    CHECK(Near(mvAt(3, 0), 2.0f, 1e-3f) && Near(mvAt(3, 1), -2.0f, 1e-3f));   // clamp to +-2
    CHECK(Near(mvAt(4, 0), 0.0f, 1e-3f) && Near(mvAt(4, 1), 0.0f, 1e-3f));    // the offset itself cancels
    CHECK(mvAt(5, 0) == 0.0f && mvAt(5, 1) == 0.0f);                           // one bad component zeroes the pair
    CHECK(Near(mvAt(6, 0), -0.5f, 1e-3f) && Near(mvAt(6, 1), 0.25f, 1e-3f));  // zero motion -> minus the offset
}

void TestCopy(Passes& passes)
{
    std::vector<uint8_t> bgra(kW * kH * 4);
    for (UINT i = 0; i < kW * kH; ++i)
    {
        bgra[i * 4 + 0] = 30;  // B
        bgra[i * 4 + 1] = 120; // G
        bgra[i * 4 + 2] = 220; // R
        bgra[i * 4 + 3] = 90;  // A
    }
    Tex src = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM, kW, kH, bgra.data(), kW * 4);
    Tex dst = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, kW, kH);
    {
        Settings settings;
        Passes::Scope scope(passes, g_context.Get(), settings, nullptr, 0, 0);
        CHECK(scope.Ok());
        CHECK(scope.Copy(src.srv.Get(), dst.rtv.Get(), kW, kH));
    }
    const auto out = Read(dst.texture.Get(), 4, kW, kH);
    CHECK(out[0] == 220 && out[1] == 120 && out[2] == 30 && out[3] == 90); // channels are swapped into RGBA order
}

// Alpha pass: the colour comes from the upscaled image, the alpha from the native image read at uv + jitter / size.
void TestAlpha(Passes& passes, float jitterX, float jitterY)
{
    constexpr UINT w = 8, h = 8; // native size; the output is 2w x 2h
    std::vector<uint8_t> original(w * h * 4);
    for (UINT y = 0; y < h; ++y)
    {
        for (UINT x = 0; x < w; ++x)
        {
            uint8_t* p = &original[(y * w + x) * 4];
            p[0] = p[1] = p[2] = 0;
            p[3] = static_cast<uint8_t>(20 * x + 2 * y); // a ramp along x (and slightly along y)
        }
    }
    std::vector<uint8_t> upscaled(w * 2 * h * 2 * 4);
    for (size_t i = 0; i < upscaled.size(); i += 4)
    {
        upscaled[i + 0] = 50;
        upscaled[i + 1] = 100;
        upscaled[i + 2] = 150;
        upscaled[i + 3] = 255; // must not reach the output alpha
    }
    Tex orig = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, original.data(), w * 4);
    Tex up = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, w * 2, h * 2, upscaled.data(), w * 2 * 4);
    Tex dst = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, w * 2, h * 2);

    Settings settings;
    settings.jitterX = jitterX;
    settings.jitterY = jitterY;
    settings.invWidth = 1.0f / w;
    settings.invHeight = 1.0f / h;
    {
        Passes::Scope scope(passes, g_context.Get(), settings, nullptr, 0, 0);
        CHECK(scope.Ok());
        CHECK(scope.Alpha(up.srv.Get(), orig.srv.Get(), dst.rtv.Get(), w * 2, h * 2));
    }
    const auto out = Read(dst.texture.Get(), 4, w * 2, h * 2);

    // Expected alpha: bilinear (clamp) read of the ramp at texel position (X + 0.5) / 2 - 0.5 + jitter.
    auto native = [&](int x, int y) { return static_cast<float>(original[(y * w + x) * 4 + 3]) / 255.0f; };
    auto expected = [&](UINT X, UINT Y) {
        const float px = (X + 0.5f) / 2.0f - 0.5f + jitterX;
        const float py = (Y + 0.5f) / 2.0f - 0.5f + jitterY;
        auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; };
        const float fx = std::floor(px), fy = std::floor(py);
        const float tx = px - fx, ty = py - fy;
        const int x0 = clampi(static_cast<int>(fx), 0, w - 1), x1 = clampi(static_cast<int>(fx) + 1, 0, w - 1);
        const int y0 = clampi(static_cast<int>(fy), 0, h - 1), y1 = clampi(static_cast<int>(fy) + 1, 0, h - 1);
        const float a = native(x0, y0) * (1 - tx) + native(x1, y0) * tx;
        const float b = native(x0, y1) * (1 - tx) + native(x1, y1) * tx;
        return a * (1 - ty) + b * ty;
    };
    for (UINT Y = 0; Y < h * 2; Y += 3)
    {
        for (UINT X = 0; X < w * 2; ++X)
        {
            const uint8_t* p = &out[(Y * w * 2 + X) * 4];
            CHECK(p[0] == 50 && p[1] == 100 && p[2] == 150);                    // colour from the upscaled image
            CHECK(Near(static_cast<float>(p[3]) / 255.0f, expected(X, Y), 0.012f)); // alpha from the native image, jittered
        }
    }
}

// The debug variant of the alpha pass: the colour is mixed with magenta (35 %), the alpha is the same as in the plain pass.
void TestAlphaTint(Passes& passes)
{
    constexpr UINT w = 4, h = 4;
    std::vector<uint8_t> original(w * h * 4, 0);
    for (UINT i = 0; i < w * h; ++i)
        original[i * 4 + 3] = 200;
    std::vector<uint8_t> upscaled(w * 2 * h * 2 * 4);
    for (size_t i = 0; i < upscaled.size(); i += 4)
    {
        upscaled[i + 0] = 50;
        upscaled[i + 1] = 100;
        upscaled[i + 2] = 150;
        upscaled[i + 3] = 255;
    }
    Tex orig = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, original.data(), w * 4);
    Tex up = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, w * 2, h * 2, upscaled.data(), w * 2 * 4);
    Tex plain = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, w * 2, h * 2);
    Tex tinted = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, w * 2, h * 2);

    Settings settings;
    settings.invWidth = 1.0f / w;
    settings.invHeight = 1.0f / h;
    {
        Passes::Scope scope(passes, g_context.Get(), settings, nullptr, 0, 0);
        CHECK(scope.Ok());
        CHECK(scope.Alpha(up.srv.Get(), orig.srv.Get(), plain.rtv.Get(), w * 2, h * 2));
        CHECK(scope.Alpha(up.srv.Get(), orig.srv.Get(), tinted.rtv.Get(), w * 2, h * 2, true));
    }
    const auto a = Read(plain.texture.Get(), 4, w * 2, h * 2);
    const auto b = Read(tinted.texture.Get(), 4, w * 2, h * 2);
    for (size_t i = 0; i < a.size(); i += 4)
    {
        CHECK(a[i] == 50 && a[i + 1] == 100 && a[i + 2] == 150 && a[i + 3] == 200);
        // lerp((50,100,150)/255, (1,0,1), 0.35) = (0.4775, 0.2549, 0.7320) -> (122, 65, 187)
        CHECK(Near(b[i], 122.0f, 1.5f) && Near(b[i + 1], 65.0f, 1.5f) && Near(b[i + 2], 187.0f, 1.5f) && b[i + 3] == 200);
    }
}

// The game's pipeline state must be identical after a Scope, whatever the passes bound.
void TestStateRestored(Passes& passes)
{
    Tex target = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16);
    Tex srv = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16);
    ComPtr<ID3D11Buffer> camera = MakeCamera(0.0f, 0.0f);
    D3D11_BLEND_DESC bd {};
    bd.RenderTarget[0].RenderTargetWriteMask = 0x5;
    ComPtr<ID3D11BlendState> blend;
    Ok(g_device->CreateBlendState(&bd, &blend), "blend state");
    D3D11_RASTERIZER_DESC rd {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    ComPtr<ID3D11RasterizerState> raster;
    Ok(g_device->CreateRasterizerState(&rd, &raster), "raster state");

    ID3D11RenderTargetView* rtv = target.rtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    const FLOAT factor[4] { 0.1f, 0.2f, 0.3f, 0.4f };
    g_context->OMSetBlendState(blend.Get(), factor, 0xFFFF0000u);
    g_context->RSSetState(raster.Get());
    const D3D11_VIEWPORT vp { 3.0f, 5.0f, 9.0f, 7.0f, 0.1f, 0.9f };
    g_context->RSSetViewports(1, &vp);
    ID3D11ShaderResourceView* s = srv.srv.Get();
    g_context->PSSetShaderResources(2, 1, &s);
    ID3D11Buffer* cb = camera.Get();
    g_context->PSSetConstantBuffers(3, 1, &cb);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

    {
        Settings settings;
        Passes::Scope scope(passes, g_context.Get(), settings, camera.Get(), 0, 59);
        CHECK(scope.Ok());
        Tex a = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4);
        Tex b = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4);
        CHECK(scope.Copy(a.srv.Get(), b.rtv.Get(), 4, 4));
        // inside the scope the context shows the private state: the game's render target is not bound
        ID3D11RenderTargetView* inside = nullptr;
        g_context->OMGetRenderTargets(1, &inside, nullptr);
        CHECK(inside != rtv);
        if (inside)
            inside->Release();
    }

    ComPtr<ID3D11RenderTargetView> rtvAfter;
    g_context->OMGetRenderTargets(1, &rtvAfter, nullptr);
    CHECK(rtvAfter.Get() == rtv);
    ComPtr<ID3D11BlendState> blendAfter;
    FLOAT factorAfter[4] {};
    UINT maskAfter = 0;
    g_context->OMGetBlendState(&blendAfter, factorAfter, &maskAfter);
    CHECK(blendAfter.Get() == blend.Get() && maskAfter == 0xFFFF0000u && factorAfter[2] == 0.3f);
    ComPtr<ID3D11RasterizerState> rasterAfter;
    g_context->RSGetState(&rasterAfter);
    CHECK(rasterAfter.Get() == raster.Get());
    D3D11_VIEWPORT vpAfter {};
    UINT count = 1;
    g_context->RSGetViewports(&count, &vpAfter);
    CHECK(vpAfter.TopLeftX == 3.0f && vpAfter.TopLeftY == 5.0f && vpAfter.Width == 9.0f && vpAfter.Height == 7.0f);
    ComPtr<ID3D11ShaderResourceView> srvAfter;
    g_context->PSGetShaderResources(2, 1, &srvAfter);
    CHECK(srvAfter.Get() == s);
    ComPtr<ID3D11Buffer> cbAfter;
    g_context->PSGetConstantBuffers(3, 1, &cbAfter);
    CHECK(cbAfter.Get() == cb);
    ComPtr<ID3D11PixelShader> psAfter;
    g_context->PSGetShader(&psAfter, nullptr, nullptr);
    CHECK(psAfter == nullptr);
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    g_context->IAGetPrimitiveTopology(&topology);
    CHECK(topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

    // Two scopes in a row: the second works the same.
    Settings settings;
    Passes::Scope again(passes, g_context.Get(), settings, nullptr, 0, 0);
    CHECK(again.Ok());
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: chara_guides_warp <repository root>\n");
        return 2;
    }
    const std::wstring shader = std::wstring(argv[1], argv[1] + std::strlen(argv[1])) + L"/OptiScaler/ext/ffxiv/chara/shaders/chara_guides.hlsl";

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (!Ok(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &g_device, nullptr, &g_context),
            "D3D11CreateDevice(WARP)"))
        return 1;

    auto vs = Compile(shader, "VS", "vs_5_0");
    auto copy = Compile(shader, "Copy", "ps_5_0");
    auto guides = Compile(shader, "Guides", "ps_5_0");
    auto alpha = Compile(shader, "Alpha", "ps_5_0");
    auto alphaTint = Compile(shader, "AlphaTint", "ps_5_0");
    if (!vs || !copy || !guides || !alpha || !alphaTint)
        return 1;

    Passes passes;
    CHECK(!passes.Init(g_device.Get(), Shaders {}, DrawTriangle)); // empty bytecode must fail, not crash
    CHECK(!passes.Ready());
    CHECK(passes.Init(g_device.Get(), Shaders { Of(vs), Of(copy), Of(guides), Of(alpha), Of(alphaTint) }, DrawTriangle));
    if (!passes.Ready())
        return 1;

    {
        ComPtr<ID3D11Buffer> whole = MakeCamera(0.5f, -0.25f);
        TestGuides(passes, whole.Get(), 0, 59); // the plain 59-vector block
        ComPtr<ID3D11Buffer> ranged = MakeCamera(0.5f, -0.25f, 80, 16);
        TestGuides(passes, ranged.Get(), 16, 64); // a block 16 vectors into a larger buffer
    }
    TestCopy(passes);
    TestAlpha(passes, 0.0f, 0.0f);
    TestAlpha(passes, 0.5f, -0.25f);
    TestAlpha(passes, -0.4375f, 0.3125f);
    TestAlphaTint(passes);
    TestStateRestored(passes);

    passes.Release();
    CHECK(!passes.Ready());
    return TEST_RESULT("chara_guides_warp");
}
