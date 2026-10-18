// The debug tint of preview NR on D3D11 WARP: the real CharaNrTint.cpp and CharaGuides.cpp with chara_guides.hlsl compiled from the
// repository (argument 1 = repository root).
//   * the bring-up result (an RGBA16F image) is tinted cyan by one extra pass into a private target; the result itself stays as it was
//     (it is shared with the D3D12 queue and must not be written by the D3D11 side), the alpha is kept
//   * one pass per result (serial), not one per UI draw; a new result draws again; another image has its own target; a size change
//     rebuilds the target; unused targets are released
//   * without the cyan shader no tint is claimed (the caller then counts nothing and gives the untinted result)
//   * the game's context state (render target, texture, viewport, shaders, constant buffers, fixed-function state) is the same after a
//     tint as before it, for a drawn, a cached, a resized, a failed and a refused call (the tint runs inside the game's UI draw)
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

// ---- The game's context state around a tint ------------------------------------------------------------------------------------------
// The tint runs inside the game's own UI draw: the game has bound its render target, its texture, its viewport, its shaders and its constant
// buffers, and expects every one of them to be there after the call. The pass works in a private context state that is swapped in and out;
// this checks the swap really gives the game's state back, after a drawn tint, a cached one, a new result, a resized one, the failure without
// the cyan shader and a refused call.
struct BoundState
{
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv0, srv1, srv3;
    ComPtr<ID3D11Buffer> vsCb0, vertexBuffer, indexBuffer;
    UINT vertexStride = 0, vertexOffset = 0, indexOffset = 0;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11Buffer> cb0, cb1;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depthState;
    ComPtr<ID3D11RasterizerState> raster;
    D3D11_VIEWPORT viewport {};
    D3D11_RECT scissor {};
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    FLOAT blendFactor[4] {};
    UINT sampleMask = 0;
    UINT stencilRef = 0;
};

BoundState Capture()
{
    BoundState s;
    g_context->OMGetRenderTargets(1, &s.rtv, &s.dsv);
    g_context->PSGetShaderResources(0, 1, &s.srv0);
    g_context->PSGetShaderResources(1, 1, &s.srv1);
    g_context->PSGetShaderResources(3, 1, &s.srv3);
    g_context->VSGetConstantBuffers(0, 1, &s.vsCb0);
    g_context->IAGetVertexBuffers(0, 1, &s.vertexBuffer, &s.vertexStride, &s.vertexOffset);
    g_context->IAGetIndexBuffer(&s.indexBuffer, &s.indexFormat, &s.indexOffset);
    g_context->PSGetShader(&s.ps, nullptr, nullptr);
    g_context->VSGetShader(&s.vs, nullptr, nullptr);
    g_context->PSGetConstantBuffers(0, 1, &s.cb0);
    g_context->PSGetConstantBuffers(1, 1, &s.cb1);
    g_context->PSGetSamplers(0, 1, &s.sampler);
    g_context->OMGetBlendState(&s.blend, s.blendFactor, &s.sampleMask);
    g_context->OMGetDepthStencilState(&s.depthState, &s.stencilRef);
    g_context->RSGetState(&s.raster);
    UINT views = 1, rects = 1;
    g_context->RSGetViewports(&views, &s.viewport);
    g_context->RSGetScissorRects(&rects, &s.scissor);
    g_context->IAGetPrimitiveTopology(&s.topology);
    return s;
}

// Every field the game bound, compared by object identity (not by content): a restored state shows the game's own objects, not equal copies.
bool SameState(const BoundState& a, const BoundState& b)
{
    return a.rtv.Get() == b.rtv.Get() && a.dsv.Get() == b.dsv.Get() && a.srv0.Get() == b.srv0.Get() && a.ps.Get() == b.ps.Get() &&
           a.vs.Get() == b.vs.Get() && a.cb0.Get() == b.cb0.Get() && a.cb1.Get() == b.cb1.Get() && a.sampler.Get() == b.sampler.Get() &&
           a.blend.Get() == b.blend.Get() && a.depthState.Get() == b.depthState.Get() && a.raster.Get() == b.raster.Get() &&
           std::memcmp(&a.viewport, &b.viewport, sizeof(a.viewport)) == 0 && std::memcmp(&a.scissor, &b.scissor, sizeof(a.scissor)) == 0 &&
           a.topology == b.topology && std::memcmp(a.blendFactor, b.blendFactor, sizeof(a.blendFactor)) == 0 && a.sampleMask == b.sampleMask &&
           a.stencilRef == b.stencilRef && a.srv1.Get() == b.srv1.Get() && a.srv3.Get() == b.srv3.Get() && a.vsCb0.Get() == b.vsCb0.Get() &&
           a.vertexBuffer.Get() == b.vertexBuffer.Get() && a.vertexStride == b.vertexStride && a.vertexOffset == b.vertexOffset &&
           a.indexBuffer.Get() == b.indexBuffer.Get() && a.indexFormat == b.indexFormat && a.indexOffset == b.indexOffset;
}

std::vector<uint8_t> ReadBuffer(ID3D11Buffer* buffer, UINT size)
{
    D3D11_BUFFER_DESC bd {};
    buffer->GetDesc(&bd);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    std::vector<uint8_t> out(size);
    if (!Ok(g_device->CreateBuffer(&bd, nullptr, &staging), "staging buffer"))
        return out;
    g_context->CopyResource(staging.Get(), buffer);
    D3D11_MAPPED_SUBRESOURCE m {};
    if (!Ok(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "Map buffer"))
        return out;
    std::memcpy(out.data(), m.pData, size);
    g_context->Unmap(staging.Get(), 0);
    return out;
}

void TestGameStateKept(guides::Passes& passes, guides::Passes& noCyan, const ComPtr<ID3DBlob>& vsCode, const ComPtr<ID3DBlob>& psCode)
{
    // The game's objects. Distinct textures for the render target and the sampled texture, a viewport and a scissor that differ from the target's
    // size, two constant buffers of its own (not the pass's settings buffer), non-default fixed-function state.
    constexpr UINT tw = 32, th = 24;
    Tex target = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, tw, th);
    Tex sampled = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16);
    Tex sampled2 = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8);
    Tex sampled3 = MakeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4);
    ComPtr<ID3D11Buffer> vb, ib;
    D3D11_BUFFER_DESC bufferDesc {};
    bufferDesc.ByteWidth = 64;
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    Ok(g_device->CreateBuffer(&bufferDesc, nullptr, &vb), "game vertex buffer");
    bufferDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    Ok(g_device->CreateBuffer(&bufferDesc, nullptr, &ib), "game index buffer");
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> dsv;
    D3D11_TEXTURE2D_DESC dd {};
    dd.Width = tw;
    dd.Height = th;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.SampleDesc.Count = 1;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    Ok(g_device->CreateTexture2D(&dd, nullptr, &depthTexture), "depth texture");
    Ok(g_device->CreateDepthStencilView(depthTexture.Get(), nullptr, &dsv), "DSV");

    const float cb0Data[4] = { 1.0f, 2.0f, 3.0f, 4.0f }, cb1Data[4] = { 5.0f, 6.0f, 7.0f, 8.0f };
    ComPtr<ID3D11Buffer> cb0, cb1;
    D3D11_BUFFER_DESC bd {};
    bd.ByteWidth = sizeof(cb0Data);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init0 { cb0Data, 0, 0 }, init1 { cb1Data, 0, 0 };
    Ok(g_device->CreateBuffer(&bd, &init0, &cb0), "game cb0");
    Ok(g_device->CreateBuffer(&bd, &init1, &cb1), "game cb1");

    ComPtr<ID3D11VertexShader> gameVs;
    ComPtr<ID3D11PixelShader> gamePs;
    Ok(g_device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &gameVs), "game VS");
    Ok(g_device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &gamePs), "game PS");

    D3D11_SAMPLER_DESC sd {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    Ok(g_device->CreateSamplerState(&sd, &sampler), "game sampler");

    D3D11_BLEND_DESC blendDesc {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;
    Ok(g_device->CreateBlendState(&blendDesc, &blend), "game blend state");
    D3D11_DEPTH_STENCIL_DESC depthDesc {};
    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    ComPtr<ID3D11DepthStencilState> depthState;
    Ok(g_device->CreateDepthStencilState(&depthDesc, &depthState), "game depth state");
    D3D11_RASTERIZER_DESC rasterDesc {};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_BACK;
    rasterDesc.ScissorEnable = TRUE;
    rasterDesc.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    Ok(g_device->CreateRasterizerState(&rasterDesc, &raster), "game rasterizer state");

    // The game's draw state.
    const float clear[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    g_context->ClearRenderTargetView(target.rtv.Get(), clear);
    ID3D11RenderTargetView* rtv = target.rtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, dsv.Get());
    ID3D11ShaderResourceView* srv = sampled.srv.Get();
    g_context->PSSetShaderResources(0, 1, &srv);
    g_context->VSSetShader(gameVs.Get(), nullptr, 0);
    g_context->PSSetShader(gamePs.Get(), nullptr, 0);
    ID3D11Buffer* buffers[2] = { cb0.Get(), cb1.Get() };
    g_context->PSSetConstantBuffers(0, 2, buffers);
    ID3D11SamplerState* samplerState = sampler.Get();
    g_context->PSSetSamplers(0, 1, &samplerState);
    const FLOAT factor[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
    g_context->OMSetBlendState(blend.Get(), factor, 0xFFFFFFF7u);
    g_context->OMSetDepthStencilState(depthState.Get(), 7);
    g_context->RSSetState(raster.Get());
    const D3D11_VIEWPORT viewport { 3.0f, 5.0f, 17.0f, 11.0f, 0.1f, 0.9f };
    g_context->RSSetViewports(1, &viewport);
    const D3D11_RECT scissor { 2, 3, 20, 15 };
    g_context->RSSetScissorRects(1, &scissor);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    // more of what a UI draw has bound: the other texture slots, a vertex-stage constant buffer, a vertex buffer and an index buffer
    ID3D11ShaderResourceView* moreSrvs[3] = { sampled2.srv.Get(), nullptr, sampled3.srv.Get() };
    g_context->PSSetShaderResources(1, 3, moreSrvs);
    ID3D11Buffer* vsBuffer = cb1.Get();
    g_context->VSSetConstantBuffers(0, 1, &vsBuffer);
    ID3D11Buffer* vertexBuffer = vb.Get();
    const UINT stride = 16, offset = 16;
    g_context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    g_context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 4);

    const BoundState before = Capture();
    // the capture sees what the game bound (or the whole test would compare nothing with nothing)
    CHECK(before.rtv.Get() == target.rtv.Get() && before.dsv.Get() == dsv.Get() && before.srv0.Get() == sampled.srv.Get());
    CHECK(before.ps.Get() == gamePs.Get() && before.vs.Get() == gameVs.Get() && before.cb0.Get() == cb0.Get() && before.cb1.Get() == cb1.Get());
    CHECK(before.srv1.Get() == sampled2.srv.Get() && before.srv3.Get() == sampled3.srv.Get() && before.vsCb0.Get() == cb1.Get() &&
          before.vertexBuffer.Get() == vb.Get() && before.vertexStride == 16 && before.vertexOffset == 16 && before.indexBuffer.Get() == ib.Get() &&
          before.indexFormat == DXGI_FORMAT_R16_UINT && before.indexOffset == 4);
    CHECK(before.sampler.Get() == sampler.Get() && before.blend.Get() == blend.Get() && before.depthState.Get() == depthState.Get() &&
          before.raster.Get() == raster.Get());
    CHECK(before.viewport.TopLeftX == 3.0f && before.viewport.Width == 17.0f && before.scissor.right == 20 &&
          before.topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP && before.stencilRef == 7 && before.sampleMask == 0xFFFFFFF7u);

    nr::TintTargets targets;
    nr::TintTargets noShaderTargets;
    const auto data = HalfImage(kW, kH, kR, kG, kB, kA);
    Tex result = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, kW, kH, data.data(), kW * 8);
    const auto big = HalfImage(kW * 2, kH * 2, kR, kG, kB, kA);
    Tex bigResult = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, kW * 2, kH * 2, big.data(), kW * 2 * 8);
    // creating textures binds nothing: the game's state is as it was
    CHECK(SameState(before, Capture()));

    bool drawn = false;
    // a drawn tint
    ComPtr<ID3D11ShaderResourceView> tinted = targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x1000, result.srv.Get(), 1, 100, &drawn);
    CHECK(tinted != nullptr && drawn); // the pass really ran inside the game's draw
    CHECK(SameState(before, Capture()));
    // the same result again: cached, nothing drawn
    tinted = targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x1000, result.srv.Get(), 1, 110, &drawn);
    CHECK(tinted != nullptr && !drawn);
    CHECK(SameState(before, Capture()));
    // a new result
    tinted = targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x1000, result.srv.Get(), 2, 120, &drawn);
    CHECK(tinted != nullptr && drawn);
    CHECK(SameState(before, Capture()));
    // a result of another size: the target is rebuilt, then drawn
    tinted = targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x1000, bigResult.srv.Get(), 3, 130, &drawn);
    CHECK(tinted != nullptr && drawn);
    CHECK(SameState(before, Capture()));
    // another preview image
    tinted = targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x2000, result.srv.Get(), 4, 140, &drawn);
    CHECK(tinted != nullptr && drawn);
    CHECK(SameState(before, Capture()));
    // the pass is entered but fails (no cyan shader): the private state is left again
    CHECK(noShaderTargets.Tinted(noCyan, g_device.Get(), g_context.Get(), 0x1000, result.srv.Get(), 1, 150, &drawn) == nullptr && !drawn);
    CHECK(noShaderTargets.Attempts() == 1); // it did get as far as entering the pass
    CHECK(SameState(before, Capture()));
    // refused before anything ran
    CHECK(targets.Tinted(passes, g_device.Get(), g_context.Get(), 0x1000, nullptr, 5, 160, &drawn) == nullptr && !drawn);
    CHECK(SameState(before, Capture()));

    // nothing the game owns was written: its render target is still the clear colour, its constant buffers keep their data
    const auto pixels = Read(target.texture.Get(), 4);
    bool targetIntact = true;
    for (size_t i = 0; i < pixels.size(); i += 4)
        targetIntact = targetIntact && Near(pixels[i], 64.0f, 1.5f) && Near(pixels[i + 1], 128.0f, 1.5f) && Near(pixels[i + 2], 191.0f, 1.5f) && pixels[i + 3] == 255;
    CHECK(targetIntact);
    const auto c0 = ReadBuffer(cb0.Get(), sizeof(cb0Data)), c1 = ReadBuffer(cb1.Get(), sizeof(cb1Data));
    CHECK(std::memcmp(c0.data(), cb0Data, sizeof(cb0Data)) == 0 && std::memcmp(c1.data(), cb1Data, sizeof(cb1Data)) == 0);

    // leave the immediate context clean for the next test
    g_context->ClearState();
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
    guides::Passes noCyan; // the cyan shader was not supplied: the pass is entered and then fails
    CHECK(noCyan.Init(g_device.Get(), guides::Shaders { Of(vs), Of(copy), Of(guidesPs), Of(alpha), Of(alphaTint) }, DrawTriangle));
    TestGameStateKept(passes, noCyan, vs, copy);
    return TEST_RESULT("chara_nr_tint_warp");
}
