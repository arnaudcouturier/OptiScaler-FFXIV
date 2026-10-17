#include "check.h"
#include "pch.h"

#include "ext/ffxiv/chara/CharaEnhancePass.h"

#include <Chara_Fxaa_PS.h>
#include <Chara_Fxaa_VS.h>

#include <d3dcompiler.h>

#include <cmath>
#include <tuple>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::chara;

namespace
{
struct Rgba
{
    uint8_t r, g, b, a;
    bool operator==(const Rgba& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

struct Env
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

bool Create(Env& e)
{
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &e.device, nullptr,
                                       &e.context));
}

ComPtr<ID3D11Texture2D> MakeTexture(Env& e, UINT w, UINT h, const std::vector<Rgba>& pixels, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM)
{
    D3D11_TEXTURE2D_DESC d {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = format;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data { pixels.data(), w * 4, 0 };
    ComPtr<ID3D11Texture2D> t;
    e.device->CreateTexture2D(&d, &data, &t);
    return t;
}

ComPtr<ID3D11ShaderResourceView> MakeSrv(Env& e, ID3D11Texture2D* t, DXGI_FORMAT format)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC d {};
    d.Format = format;
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> v;
    e.device->CreateShaderResourceView(t, &d, &v);
    return v;
}

// Reads back an RGBA8 texture that was written through its first view (any RGBA8 format).
std::vector<Rgba> Read(Env& e, ID3D11ShaderResourceView* srv, UINT w, UINT h)
{
    ComPtr<ID3D11Resource> res;
    srv->GetResource(&res);
    D3D11_TEXTURE2D_DESC d {};
    ComPtr<ID3D11Texture2D> tex;
    res.As(&tex);
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // same layout as the typeless / sRGB variants
    ComPtr<ID3D11Texture2D> staging;
    e.device->CreateTexture2D(&d, nullptr, &staging);
    e.context->CopyResource(staging.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE m {};
    std::vector<Rgba> out(static_cast<size_t>(w) * h);
    if (SUCCEEDED(e.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
    {
        for (UINT y = 0; y < h; ++y)
            std::memcpy(&out[static_cast<size_t>(y) * w], static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, w * 4);
        e.context->Unmap(staging.Get(), 0);
    }
    return out;
}

Rgba Grey(uint8_t v, uint8_t a = 255) { return { v, v, v, a }; }
float Luma(const Rgba& p) { return (0.299f * p.r + 0.587f * p.g + 0.114f * p.b) / 255.0f; }

// A source image with a diagonal staircase edge: white where x > y, black elsewhere, opaque.
std::vector<Rgba> Staircase(UINT n)
{
    std::vector<Rgba> px(static_cast<size_t>(n) * n);
    for (UINT y = 0; y < n; ++y)
        for (UINT x = 0; x < n; ++x)
            px[static_cast<size_t>(y) * n + x] = x > y ? Grey(255) : Grey(0);
    return px;
}

void TestFlatAndSize(Env& e)
{
    const UINT n = 16;
    std::vector<Rgba> flat(n * n, Rgba { 128, 100, 77, 255 });
    auto tex = MakeTexture(e, n, n, flat);
    auto srv = MakeSrv(e, tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);

    FxaaPass pass;
    ID3D11ShaderResourceView* out = nullptr;
    CHECK(pass.Process(e.context.Get(), srv.Get(), n, n, false, 1, &out));
    CHECK(out != nullptr && pass.OutputWidth() == n && pass.OutputHeight() == n && pass.Failure() == nullptr);
    auto px = Read(e, out, n, n);
    for (const auto& p : px)
        CHECK(p == (Rgba { 128, 100, 77, 255 })); // flat early-out: the centre sample, unchanged

    // 2x: the output is twice the size and a flat image stays flat
    CHECK(pass.Process(e.context.Get(), srv.Get(), n, n, false, 2, &out));
    CHECK(pass.OutputWidth() == 2 * n && pass.OutputHeight() == 2 * n);
    px = Read(e, out, 2 * n, 2 * n);
    bool allSame = true;
    for (const auto& p : px)
        allSame = allSame && p == (Rgba { 128, 100, 77, 255 });
    CHECK(allSame);

    // a different source size gets a different output size
    std::vector<Rgba> wide(24 * 8, Grey(200));
    auto wideTex = MakeTexture(e, 24, 8, wide);
    auto wideSrv = MakeSrv(e, wideTex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(pass.Process(e.context.Get(), wideSrv.Get(), 24, 8, false, 2, &out));
    CHECK(pass.OutputWidth() == 48 && pass.OutputHeight() == 16);
}

void TestAlphaRule(Env& e)
{
    const UINT n = 16;
    FxaaPass pass;
    ID3D11ShaderResourceView* out = nullptr;

    // a hard edge but alpha 0.5 everywhere: the filter never blends across cut-outs and never changes alpha
    auto stairs = Staircase(n);
    for (auto& p : stairs)
        p.a = 128;
    auto tex = MakeTexture(e, n, n, stairs);
    auto srv = MakeSrv(e, tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(pass.Process(e.context.Get(), srv.Get(), n, n, false, 1, &out));
    auto px = Read(e, out, n, n);
    CHECK(px == stairs);

    // opaque staircase with one transparent texel: pixels whose 2x2 diagonal neighbourhood reaches it are unchanged
    auto mixed = Staircase(n);
    const UINT hx = 8, hy = 8; // on the edge's neighbourhood
    mixed[hy * n + hx].a = 0;
    tex = MakeTexture(e, n, n, mixed);
    srv = MakeSrv(e, tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(pass.Process(e.context.Get(), srv.Get(), n, n, false, 1, &out));
    px = Read(e, out, n, n);
    // the centre sample and the four diagonal neighbours decide the alpha rule
    for (int dy = -1; dy <= 1; dy += 2)
        for (int dx = -1; dx <= 1; dx += 2)
            CHECK(px[(hy + dy) * n + (hx + dx)] == mixed[(hy + dy) * n + (hx + dx)]);
    CHECK(px[hy * n + hx] == mixed[hy * n + hx]);

    // alpha is never changed for an opaque image either
    auto opaque = Staircase(n);
    tex = MakeTexture(e, n, n, opaque);
    srv = MakeSrv(e, tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(pass.Process(e.context.Get(), srv.Get(), n, n, false, 1, &out));
    px = Read(e, out, n, n);
    for (const auto& p : px)
        CHECK(p.a == 255);
}

void TestEdgeSmoothing(Env& e)
{
    const UINT n = 32;
    auto stairs = Staircase(n);
    auto tex = MakeTexture(e, n, n, stairs);
    auto srv = MakeSrv(e, tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    FxaaPass pass;
    ID3D11ShaderResourceView* out = nullptr;
    CHECK(pass.Process(e.context.Get(), srv.Get(), n, n, false, 1, &out));
    const auto px = Read(e, out, n, n);

    int inputIntermediate = 0, outputIntermediate = 0, edgeCount = 0;
    for (UINT y = 0; y < n; ++y)
    {
        for (UINT x = 0; x < n; ++x)
        {
            const Rgba& in = stairs[y * n + x];
            const Rgba& o = px[y * n + x];
            const float li = Luma(in), lo = Luma(o);
            inputIntermediate += (li > 0.05f && li < 0.95f) ? 1 : 0;
            outputIntermediate += (lo > 0.05f && lo < 0.95f) ? 1 : 0;

            const int distance = std::abs(static_cast<int>(x) - static_cast<int>(y));
            if (distance >= 3)
                CHECK(o == in); // away from the edge nothing changes
            else
                ++edgeCount;
            CHECK(o.a == 255);
            // smoothing only mixes the two colours: no value outside the neighbourhood's range
            CHECK(lo >= -1e-4f && lo <= 1.0001f);
        }
    }
    CHECK(inputIntermediate == 0);
    CHECK(outputIntermediate >= 8); // the staircase got intermediate tones
    CHECK(edgeCount > 0);

    // Low contrast is below max(0.0312, 0.125 * brightest): the early-out returns the centre sample, so the staircase
    // stays exactly as it was. The same shape with enough contrast is smoothed.
    for (const auto& [dark, bright, smoothed] : { std::tuple<uint8_t, uint8_t, bool> { 100, 108, false },
                                                  std::tuple<uint8_t, uint8_t, bool> { 100, 140, true } })
    {
        std::vector<Rgba> weak(n * n);
        for (UINT y = 0; y < n; ++y)
            for (UINT x = 0; x < n; ++x)
                weak[y * n + x] = x > y ? Grey(bright) : Grey(dark);
        auto wtex = MakeTexture(e, n, n, weak);
        auto wsrv = MakeSrv(e, wtex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        CHECK(pass.Process(e.context.Get(), wsrv.Get(), n, n, false, 1, &out));
        const auto wp = Read(e, out, n, n);
        CHECK((wp != weak) == smoothed);
    }

    // a straight (axis-aligned) edge has no diagonal contrast to follow along the edge direction: it must not get brighter
    // or darker than its two colours anywhere
    std::vector<Rgba> vertical(n * n);
    for (UINT y = 0; y < n; ++y)
        for (UINT x = 0; x < n; ++x)
            vertical[y * n + x] = x >= n / 2 ? Grey(255) : Grey(0);
    auto vtex = MakeTexture(e, n, n, vertical);
    auto vsrv = MakeSrv(e, vtex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(pass.Process(e.context.Get(), vsrv.Get(), n, n, false, 1, &out));
    const auto vp = Read(e, out, n, n);
    for (UINT y = 0; y < n; ++y)
        for (UINT x = 0; x < n; ++x)
            CHECK(Luma(vp[y * n + x]) >= 0.0f && Luma(vp[y * n + x]) <= 1.0f && vp[y * n + x].a == 255);
}

void TestUpscaled(Env& e)
{
    const UINT n = 32;
    auto stairs = Staircase(n);
    auto tex = MakeTexture(e, n, n, stairs);
    auto srv = MakeSrv(e, tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    FxaaPass pass;
    ID3D11ShaderResourceView* out = nullptr;
    CHECK(pass.Process(e.context.Get(), srv.Get(), n, n, false, 2, &out));
    CHECK(pass.OutputWidth() == 64 && pass.OutputHeight() == 64);
    const auto px = Read(e, out, 64, 64);

    // the texel size comes from the SOURCE: the interior far from the edge is exactly the source colours
    CHECK(px[(2 * 25) * 64 + 2 * 3] == Grey(0));   // x=3,y=25: below the diagonal -> black
    CHECK(px[(2 * 3) * 64 + 2 * 25] == Grey(255)); // x=25,y=3: above -> white
    int intermediate = 0;
    for (const auto& p : px)
    {
        const float l = Luma(p);
        intermediate += (l > 0.05f && l < 0.95f) ? 1 : 0;
        CHECK(p.a == 255);
    }
    CHECK(intermediate >= 16);
}

void TestSrgb(Env& e)
{
    const UINT n = 16;
    std::vector<Rgba> flat(n * n, Rgba { 128, 100, 77, 255 });
    auto tex = MakeTexture(e, n, n, flat, DXGI_FORMAT_R8G8B8A8_TYPELESS);
    auto srv = MakeSrv(e, tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    FxaaPass pass;
    ID3D11ShaderResourceView* out = nullptr;
    CHECK(pass.Process(e.context.Get(), srv.Get(), n, n, true, 1, &out));
    D3D11_SHADER_RESOURCE_VIEW_DESC vd {};
    out->GetDesc(&vd);
    CHECK(vd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    // sRGB in, sRGB out: the stored bytes are the source bytes
    const auto px = Read(e, out, n, n);
    for (const auto& p : px)
        CHECK(p == (Rgba { 128, 100, 77, 255 }));

    // switching back to UNORM recreates the views
    auto utex = MakeTexture(e, n, n, flat);
    auto usrv = MakeSrv(e, utex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(pass.Process(e.context.Get(), usrv.Get(), n, n, false, 1, &out));
    out->GetDesc(&vd);
    CHECK(vd.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
}

void TestRejectedRequests(Env& e)
{
    const UINT n = 16;
    std::vector<Rgba> flat(n * n, Grey(10));
    auto tex = MakeTexture(e, n, n, flat);
    auto srv = MakeSrv(e, tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    FxaaPass pass;
    ID3D11ShaderResourceView* out = reinterpret_cast<ID3D11ShaderResourceView*>(0x1);
    CHECK(!pass.Process(e.context.Get(), srv.Get(), n, n, false, 3, &out) && out == nullptr);
    CHECK(!pass.Process(e.context.Get(), srv.Get(), n, n, false, 0, &out));
    CHECK(!pass.Process(e.context.Get(), nullptr, n, n, false, 1, &out));
    CHECK(!pass.Process(e.context.Get(), srv.Get(), 0, n, false, 1, &out));
    CHECK(!pass.Process(nullptr, srv.Get(), n, n, false, 1, &out));
    CHECK(!pass.Process(e.context.Get(), srv.Get(), n, n, false, 1, nullptr));
    CHECK(pass.Failure() == nullptr); // bad requests are not permanent failures
    CHECK(pass.Process(e.context.Get(), srv.Get(), n, n, false, 1, &out) && out != nullptr);
    pass.Reset();
    CHECK(pass.OutputWidth() == 0);
}

// ---- pipeline state restore ---------------------------------------------------------------------------------------

ComPtr<ID3D11PixelShader> CompilePs(Env& e, const char* source)
{
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3DCompile(source, std::strlen(source), "game", nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors)))
        return nullptr;
    ComPtr<ID3D11PixelShader> ps;
    e.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps);
    return ps;
}

struct GameState
{
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11ShaderResourceView> t0;
    D3D11_VIEWPORT viewport { 1.0f, 2.0f, 30.0f, 40.0f, 0.1f, 0.9f };
};

GameState SetUpGameState(Env& e, UINT w, UINT h, ID3D11ShaderResourceView* t0)
{
    GameState g;
    D3D11_TEXTURE2D_DESC d {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    e.device->CreateTexture2D(&d, nullptr, &g.target);
    e.device->CreateRenderTargetView(g.target.Get(), nullptr, &g.rtv);
    e.device->CreateVertexShader(Chara_Fxaa_VS_cso, sizeof(Chara_Fxaa_VS_cso), nullptr, &g.vs);
    // the "game" pixel shader: copies t0 texel by texel
    g.ps = CompilePs(e, "Texture2D<float4> t0 : register(t0);\n"
                        "float4 main(float4 p : SV_Position) : SV_Target { return t0.Load(int3(p.xy, 0)); }\n");
    D3D11_SAMPLER_DESC sd {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    e.device->CreateSamplerState(&sd, &g.sampler);
    D3D11_BLEND_DESC bd {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    e.device->CreateBlendState(&bd, &g.blend);
    D3D11_RASTERIZER_DESC rd {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE;
    rd.DepthClipEnable = TRUE;
    e.device->CreateRasterizerState(&rd, &g.raster);
    D3D11_DEPTH_STENCIL_DESC dd {};
    dd.DepthEnable = FALSE;
    e.device->CreateDepthStencilState(&dd, &g.depth);
    g.t0 = ComPtr<ID3D11ShaderResourceView>(t0);

    ID3D11DeviceContext* c = e.context.Get();
    ID3D11RenderTargetView* rtv = g.rtv.Get();
    ID3D11SamplerState* sampler = g.sampler.Get();
    ID3D11ShaderResourceView* srv = t0;
    const float factor[4] { 0.25f, 0.5f, 0.75f, 1.0f };
    const D3D11_RECT scissor { 3, 4, 20, 22 };
    c->OMSetRenderTargets(1, &rtv, nullptr);
    c->RSSetViewports(1, &g.viewport);
    c->RSSetScissorRects(1, &scissor);
    c->VSSetShader(g.vs.Get(), nullptr, 0);
    c->PSSetShader(g.ps.Get(), nullptr, 0);
    c->PSSetSamplers(0, 1, &sampler);
    c->PSSetShaderResources(0, 1, &srv);
    c->OMSetBlendState(g.blend.Get(), factor, 0xfffffffe);
    c->OMSetDepthStencilState(g.depth.Get(), 7);
    c->RSSetState(g.raster.Get());
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    return g;
}

void CheckStateUnchanged(Env& e, const GameState& g, ID3D11ShaderResourceView* t0)
{
    ID3D11DeviceContext* c = e.context.Get();

    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    c->OMGetRenderTargets(1, &rtv, &dsv);
    CHECK(rtv.Get() == g.rtv.Get() && dsv == nullptr);

    D3D11_VIEWPORT vp {};
    UINT count = 1;
    c->RSGetViewports(&count, &vp);
    CHECK(count == 1 && vp.TopLeftX == g.viewport.TopLeftX && vp.TopLeftY == g.viewport.TopLeftY &&
          vp.Width == g.viewport.Width && vp.Height == g.viewport.Height && vp.MinDepth == g.viewport.MinDepth &&
          vp.MaxDepth == g.viewport.MaxDepth);

    D3D11_RECT scissor {};
    count = 1;
    c->RSGetScissorRects(&count, &scissor);
    CHECK(scissor.left == 3 && scissor.top == 4 && scissor.right == 20 && scissor.bottom == 22);

    ComPtr<ID3D11VertexShader> vs;
    c->VSGetShader(&vs, nullptr, nullptr);
    CHECK(vs.Get() == g.vs.Get());
    ComPtr<ID3D11PixelShader> ps;
    c->PSGetShader(&ps, nullptr, nullptr);
    CHECK(ps.Get() == g.ps.Get());

    ComPtr<ID3D11SamplerState> sampler;
    c->PSGetSamplers(0, 1, &sampler);
    CHECK(sampler.Get() == g.sampler.Get());

    ComPtr<ID3D11ShaderResourceView> srv;
    c->PSGetShaderResources(0, 1, &srv);
    CHECK(srv.Get() == t0);

    ComPtr<ID3D11BlendState> blend;
    float factor[4] {};
    UINT mask = 0;
    c->OMGetBlendState(&blend, factor, &mask);
    CHECK(blend.Get() == g.blend.Get() && factor[0] == 0.25f && factor[1] == 0.5f && factor[2] == 0.75f && mask == 0xfffffffe);

    ComPtr<ID3D11DepthStencilState> depth;
    UINT ref = 0;
    c->OMGetDepthStencilState(&depth, &ref);
    CHECK(depth.Get() == g.depth.Get() && ref == 7);

    ComPtr<ID3D11RasterizerState> raster;
    c->RSGetState(&raster);
    CHECK(raster.Get() == g.raster.Get());

    D3D11_PRIMITIVE_TOPOLOGY topology {};
    c->IAGetPrimitiveTopology(&topology);
    CHECK(topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
}

void TestStateRestored(Env& e)
{
    const UINT n = 32;
    auto stairs = Staircase(n);
    auto tex = MakeTexture(e, n, n, stairs);
    auto srv = MakeSrv(e, tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    GameState g = SetUpGameState(e, 64, 64, srv.Get());
    CHECK(g.ps != nullptr && g.rtv != nullptr);

    FxaaPass pass;
    ID3D11ShaderResourceView* filtered = nullptr;
    CheckStateUnchanged(e, g, srv.Get());
    CHECK(pass.Process(e.context.Get(), srv.Get(), n, n, false, 1, &filtered));
    CheckStateUnchanged(e, g, srv.Get());
    CHECK(pass.Process(e.context.Get(), srv.Get(), n, n, false, 2, &filtered));
    CheckStateUnchanged(e, g, srv.Get());

    // a failed request changes nothing either
    CHECK(!pass.Process(e.context.Get(), srv.Get(), n, n, false, 5, &filtered));
    CheckStateUnchanged(e, g, srv.Get());

}

void TestSwappedSourceDraw(Env& e)
{
    // The equipment draw: the filtered image is t0 for the draw, then the game's own t0 is bound again.
    const UINT n = 32;
    auto stairs = Staircase(n);
    auto tex = MakeTexture(e, n, n, stairs);
    auto srv = MakeSrv(e, tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    GameState g = SetUpGameState(e, n, n, srv.Get());

    // the target is 32x32 with a 30x40 viewport at (1,2): use a full viewport and no scissor for this one
    const D3D11_VIEWPORT full { 0, 0, static_cast<float>(n), static_cast<float>(n), 0, 1 };
    e.context->RSSetViewports(1, &full);
    D3D11_RASTERIZER_DESC rd {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    ComPtr<ID3D11RasterizerState> rs;
    e.device->CreateRasterizerState(&rd, &rs);
    e.context->RSSetState(rs.Get());
    e.context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    e.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    FxaaPass pass;
    ID3D11ShaderResourceView* filtered = nullptr;
    CHECK(pass.Process(e.context.Get(), srv.Get(), n, n, false, 1, &filtered));
    const auto filteredPixels = Read(e, filtered, n, n);

    int calls = 0;
    ID3D11ShaderResourceView* seen = nullptr;
    WithSource0(e.context.Get(), filtered,
                [&]
                {
                    ++calls;
                    ComPtr<ID3D11ShaderResourceView> now;
                    e.context->PSGetShaderResources(0, 1, &now);
                    seen = now.Get();
                    e.context->Draw(3, 0);
                });
    CHECK(calls == 1);
    CHECK(seen == filtered);

    ComPtr<ID3D11ShaderResourceView> after;
    e.context->PSGetShaderResources(0, 1, &after);
    CHECK(after.Get() == srv.Get()); // the game's own image is bound again

    // the draw sampled the filtered image (the game's shader copies t0 texel by texel)
    ComPtr<ID3D11ShaderResourceView> targetSrv;
    e.device->CreateShaderResourceView(g.target.Get(), nullptr, &targetSrv);
    const auto drawn = Read(e, targetSrv.Get(), n, n);
    for (size_t i = 0; i < drawn.size(); ++i)
        if (!(drawn[i] == filteredPixels[i]))
        {
            std::printf("first difference at %zu: drawn %d %d %d %d, filtered %d %d %d %d\n", i,drawn[i].r, drawn[i].g, drawn[i].b,
                        drawn[i].a, filteredPixels[i].r, filteredPixels[i].g, filteredPixels[i].b, filteredPixels[i].a);
            break;
        }
    CHECK(drawn == filteredPixels);
    CHECK(drawn != stairs); // and that differs from the original on the edge

    // with nothing bound at t0 before, nothing is bound after
    ID3D11ShaderResourceView* none = nullptr;
    e.context->PSSetShaderResources(0, 1, &none);
    WithSource0(e.context.Get(), filtered, [&] { e.context->Draw(3, 0); });
    ComPtr<ID3D11ShaderResourceView> nothing;
    e.context->PSGetShaderResources(0, 1, &nothing);
    CHECK(nothing == nullptr);
}
} // namespace

int main()
{
    Env e;
    if (!Create(e))
    {
        std::printf("FAIL: no WARP device\n");
        return 1;
    }
    TestFlatAndSize(e);
    TestAlphaRule(e);
    TestEdgeSmoothing(e);
    TestUpscaled(e);
    TestSrgb(e);
    TestRejectedRequests(e);
    TestStateRestored(e);
    TestSwappedSourceDraw(e);
    return TEST_RESULT("chara_fxaa");
}
