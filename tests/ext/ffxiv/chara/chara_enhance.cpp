#include "check.h"
#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/chara/CharaEnhance.h"
#include "ext/ffxiv/chara/CharaStats.h"

#include <Chara_Fxaa_VS.h>

#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::chara;

// The dispatcher's raw full-screen draw (CharaDispatch.cpp is not part of this test).
namespace ext::ffxiv::chara
{
void DrawFullscreenTriangle(ID3D11DeviceContext* context) { context->Draw(3, 0); }
} // namespace ext::ffxiv::chara

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

Rgba Grey(uint8_t v) { return { v, v, v, 255 }; }

std::vector<Rgba> Staircase(UINT n)
{
    std::vector<Rgba> px(static_cast<size_t>(n) * n);
    for (UINT y = 0; y < n; ++y)
        for (UINT x = 0; x < n; ++x)
            px[static_cast<size_t>(y) * n + x] = x > y ? Grey(255) : Grey(0);
    return px;
}

ComPtr<ID3D11Texture2D> MakeTexture(Env& e, UINT w, UINT h, DXGI_FORMAT format, const std::vector<Rgba>* pixels, UINT samples = 1,
                                    UINT bind = D3D11_BIND_SHADER_RESOURCE)
{
    D3D11_TEXTURE2D_DESC d {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.SampleDesc.Count = samples;
    d.Format = format;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA data { pixels ? pixels->data() : nullptr, w * 4, 0 };
    ComPtr<ID3D11Texture2D> t;
    e.device->CreateTexture2D(&d, pixels ? &data : nullptr, &t);
    return t;
}

ComPtr<ID3D11ShaderResourceView> MakeSrv(Env& e, ID3D11Texture2D* t, DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC d {};
    d.Format = format;
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipLevels = 1;
    D3D11_TEXTURE2D_DESC td {};
    t->GetDesc(&td);
    if (format == DXGI_FORMAT_UNKNOWN)
        d.Format = td.Format;
    ComPtr<ID3D11ShaderResourceView> v;
    e.device->CreateShaderResourceView(t, &d, &v);
    return v;
}

std::vector<Rgba> Read(Env& e, ID3D11Texture2D* tex, UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC d {};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D11Texture2D> staging;
    e.device->CreateTexture2D(&d, nullptr, &staging);
    e.context->CopyResource(staging.Get(), tex);
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

// The game side: a 32x32 target, a pixel shader that copies t0 texel by texel, bound with a recognisable state.
struct Game
{
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    D3D11_VIEWPORT viewport { 0, 0, 32, 32, 0, 1 };
};

Game SetUpGame(Env& e, ID3D11ShaderResourceView* t0)
{
    Game g;
    g.target = MakeTexture(e, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 1, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    e.device->CreateRenderTargetView(g.target.Get(), nullptr, &g.rtv);
    e.device->CreateVertexShader(Chara_Fxaa_VS_cso, sizeof(Chara_Fxaa_VS_cso), nullptr, &g.vs);
    const char* source = "Texture2D<float4> t0 : register(t0);\n"
                         "float4 main(float4 p : SV_Position) : SV_Target { return t0.Load(int3(p.xy, 0)); }\n";
    ComPtr<ID3DBlob> blob, errors;
    D3DCompile(source, std::strlen(source), "game", nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors);
    e.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g.ps);
    D3D11_SAMPLER_DESC sd {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    e.device->CreateSamplerState(&sd, &g.sampler);
    D3D11_RASTERIZER_DESC rd {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    e.device->CreateRasterizerState(&rd, &g.raster);

    ID3D11DeviceContext* c = e.context.Get();
    ID3D11RenderTargetView* rtv = g.rtv.Get();
    ID3D11SamplerState* sampler = g.sampler.Get();
    ID3D11ShaderResourceView* srv = t0;
    c->OMSetRenderTargets(1, &rtv, nullptr);
    c->RSSetViewports(1, &g.viewport);
    c->RSSetState(g.raster.Get());
    c->VSSetShader(g.vs.Get(), nullptr, 0);
    c->PSSetShader(g.ps.Get(), nullptr, 0);
    c->PSSetSamplers(0, 1, &sampler);
    c->PSSetShaderResources(0, 1, &srv);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    return g;
}

struct Run
{
    bool handled = false;
    int drawCalls = 0;
    ID3D11ShaderResourceView* seenAtDraw = nullptr;
    UINT seenWidth = 0;
};

Run RunAround(Env& e, const table::Snapshot& snap, const DrawView* viewOverride = nullptr)
{
    Run r;
    DrawView view;
    view.context = e.context.Get();
    view.api = DrawApi::Draw;
    view.table = &snap;
    if (viewOverride != nullptr)
        view = *viewOverride;

    auto native = [&]
    {
        ++r.drawCalls;
        ComPtr<ID3D11ShaderResourceView> bound;
        e.context->PSGetShaderResources(0, 1, &bound);
        r.seenAtDraw = bound.Get();
        if (bound)
        {
            ComPtr<ID3D11Resource> res;
            bound->GetResource(&res);
            ComPtr<ID3D11Texture2D> tex;
            res.As(&tex);
            D3D11_TEXTURE2D_DESC d {};
            tex->GetDesc(&d);
            r.seenWidth = d.Width;
        }
        e.context->Draw(3, 0);
    };
    r.handled = enhance::Around(view, DrawCall { [](void* p) { (*static_cast<decltype(native)*>(p))(); }, &native });
    return r;
}

table::Snapshot TableWith(size_t entry, ID3D11Texture2D* tex, UINT w, UINT h)
{
    table::Snapshot s;
    s.ready = true;
    s.entries[entry] = { reinterpret_cast<uintptr_t>(tex), w, h };
    return s;
}

ID3D11ShaderResourceView* BoundT0(Env& e, ComPtr<ID3D11ShaderResourceView>& holder)
{
    e.context->PSGetShaderResources(0, 1, &holder);
    return holder.Get();
}

void Main2(Env& e)
{
    auto& cfg = ext::Cfg().ffxiv.chara;
    const auto stairs = Staircase(32);
    auto sourceTex = MakeTexture(e, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, &stairs);
    auto sourceSrv = MakeSrv(e, sourceTex.Get());
    Game game = SetUpGame(e, sourceSrv.Get());
    const auto snap = TableWith(0, sourceTex.Get(), 32, 32);

    // ---- 1x: the equipment output is filtered, drawn once with the filtered image, the game's t0 comes back ----
    cfg.previewUpscale = false;
    auto r = RunAround(e, snap);
    CHECK(r.handled && r.drawCalls == 1);
    CHECK(r.seenAtDraw != nullptr && r.seenAtDraw != sourceSrv.Get() && r.seenWidth == 32);
    ComPtr<ID3D11ShaderResourceView> holder;
    CHECK(BoundT0(e, holder) == sourceSrv.Get());
    holder.Reset();
    const auto drawn = Read(e, game.target.Get(), 32, 32);
    CHECK(drawn != stairs); // the draw saw the filtered image: the staircase edge got intermediate tones
    int intermediate = 0;
    for (const auto& p : drawn)
        intermediate += (p.r > 12 && p.r < 243) ? 1 : 0;
    CHECK(intermediate >= 8);
    for (UINT y = 0; y < 32; ++y)
        for (UINT x = 0; x < 32; ++x)
            if (std::abs(static_cast<int>(x) - static_cast<int>(y)) >= 3)
                CHECK(drawn[y * 32 + x] == stairs[y * 32 + x]); // away from the edge nothing changed
    CHECK(g_stats.equipShaderSeen == 1 && g_stats.equipMatched == 1 && g_stats.aaProcessed == 1 && g_stats.aaSkipped == 0);

    // the game's own pipeline state is intact
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11PixelShader> ps;
    e.context->OMGetRenderTargets(1, &rtv, nullptr);
    e.context->PSGetShader(&ps, nullptr, nullptr);
    CHECK(rtv.Get() == game.rtv.Get() && ps.Get() == game.ps.Get());
    rtv.Reset();
    ps.Reset();

    // ---- 2x (the default): the draw sees a 64 x 64 image ----
    cfg.previewUpscale = true;
    r = RunAround(e, snap);
    CHECK(r.handled && r.drawCalls == 1 && r.seenWidth == 64);
    CHECK(BoundT0(e, holder) == sourceSrv.Get());
    holder.Reset();
    CHECK(g_stats.aaProcessed == 2);

    // ---- everything that is not the equipment output stays native: Around returns false, the draw is not run ----
    const auto before = g_stats.aaProcessed.load();
    {
        // another entry holds the texture: not the equipment output (entry 0 only)
        auto other = TableWith(3, sourceTex.Get(), 32, 32);
        auto none = RunAround(e, other);
        CHECK(!none.handled && none.drawCalls == 0);
        // the table knows a different size
        auto wrongSize = TableWith(0, sourceTex.Get(), 32, 33);
        CHECK(!RunAround(e, wrongSize).handled);
        // an empty table (no preview renderer yet)
        table::Snapshot empty;
        empty.ready = true;
        CHECK(!RunAround(e, empty).handled);
        // another texture
        auto otherTex = MakeTexture(e, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, &stairs);
        auto notEntry = TableWith(0, otherTex.Get(), 32, 32);
        CHECK(!RunAround(e, notEntry).handled);
        // no table at all
        DrawView noTable;
        noTable.context = e.context.Get();
        CHECK(!RunAround(e, snap, &noTable).handled);
    }
    CHECK(g_stats.aaProcessed == before);
    CHECK(BoundT0(e, holder) == sourceSrv.Get());
    holder.Reset();

    // nothing bound at t0
    {
        ID3D11ShaderResourceView* none = nullptr;
        e.context->PSSetShaderResources(0, 1, &none);
        CHECK(!RunAround(e, snap).handled);
        ID3D11ShaderResourceView* back = sourceSrv.Get();
        e.context->PSSetShaderResources(0, 1, &back);
    }

    // unsupported format: native, counted as skipped
    {
        auto half = MakeTexture(e, 32, 32, DXGI_FORMAT_R16G16B16A16_FLOAT, nullptr);
        auto halfSrv = MakeSrv(e, half.Get());
        ID3D11ShaderResourceView* srv = halfSrv.Get();
        e.context->PSSetShaderResources(0, 1, &srv);
        const auto skippedBefore = g_stats.aaSkipped.load();
        CHECK(!RunAround(e, TableWith(0, half.Get(), 32, 32)).handled);
        CHECK(g_stats.aaSkipped == skippedBefore + 1);
        ID3D11ShaderResourceView* back = sourceSrv.Get();
        e.context->PSSetShaderResources(0, 1, &back);
    }

    // multisampled textures are not understood
    {
        auto ms = MakeTexture(e, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 4, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        if (ms)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC d {};
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
            ComPtr<ID3D11ShaderResourceView> msSrv;
            if (SUCCEEDED(e.device->CreateShaderResourceView(ms.Get(), &d, &msSrv)))
            {
                ID3D11ShaderResourceView* srv = msSrv.Get();
                e.context->PSSetShaderResources(0, 1, &srv);
                CHECK(!RunAround(e, TableWith(0, ms.Get(), 32, 32)).handled);
                ID3D11ShaderResourceView* back = sourceSrv.Get();
                e.context->PSSetShaderResources(0, 1, &back);
            }
        }
    }

    // a size the output limit refuses (the 2x output would exceed 4096): native
    {
        auto big = MakeTexture(e, 2100, 8, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr);
        auto bigSrv = MakeSrv(e, big.Get());
        ID3D11ShaderResourceView* srv = bigSrv.Get();
        e.context->PSSetShaderResources(0, 1, &srv);
        cfg.previewUpscale = true;
        CHECK(!RunAround(e, TableWith(0, big.Get(), 2100, 8)).handled); // 4200 > 4096
        cfg.previewUpscale = false;
        CHECK(RunAround(e, TableWith(0, big.Get(), 2100, 8)).handled);  // 2100 is fine at 1x
        ID3D11ShaderResourceView* back = sourceSrv.Get();
        e.context->PSSetShaderResources(0, 1, &back);
    }

    // after the module released its objects it works again
    enhance::Detach();
    cfg.previewUpscale = false;
    CHECK(RunAround(e, snap).handled);
}
} // namespace

int main()
{
    Env e;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &e.device, nullptr, &e.context)))
    {
        std::printf("FAIL: no WARP device\n");
        return 1;
    }
    Main2(e);
    return TEST_RESULT("chara_enhance");
}
