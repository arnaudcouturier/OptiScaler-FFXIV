#include "check.h"
#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/chara/CharaEnhance.h"
#include "ext/ffxiv/chara/CharaFlood.h"
#include "ext/ffxiv/chara/CharaProbeHooks.h"
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
    ComPtr<ID3D11PixelShader> psSample; // samples t0 over the whole target (the flood image is only 8x8)
    ComPtr<ID3D11PixelShader> psSlot3;  // samples t3
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
    for (int slot : { 0, 3 })
    {
        const std::string sampled = "Texture2D<float4> tex : register(t" + std::to_string(slot) + ");\n"
                                    "SamplerState smp : register(s0);\n"
                                    "float4 main(float4 p : SV_Position) : SV_Target { return tex.SampleLevel(smp, p.xy / 32.0, 0); }\n";
        ComPtr<ID3DBlob> sblob;
        D3DCompile(sampled.c_str(), sampled.size(), "game-sample", nullptr, nullptr, "main", "ps_5_0", 0, 0, &sblob, &errors);
        e.device->CreatePixelShader(sblob->GetBufferPointer(), sblob->GetBufferSize(), nullptr, slot == 0 ? &g.psSample : &g.psSlot3);
    }
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

Run RunAround(Env& e, const table::Snapshot& snap, const DrawView* viewOverride = nullptr, DrawApi api = DrawApi::Draw)
{
    Run r;
    DrawView view;
    view.context = e.context.Get();
    view.api = api;
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
        if (api == DrawApi::DrawIndexed)
            e.context->DrawIndexed(3, 0, 0);
        else
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

// A swap-trace sink of the test: remembers what the trace reported.
struct SinkLog
{
    int arms = 0;
    bool answer = true;
    std::vector<probe::SwapRecord> records;
} g_sink;

bool TestArm(probe::SwapKind)
{
    ++g_sink.arms;
    return g_sink.answer;
}

void TestReport(const probe::SwapRecord& record) { g_sink.records.push_back(record); }

const probe::SwapSink kTestSink { &TestArm, &TestReport };

ULONG RefCount(IUnknown* object)
{
    object->AddRef();
    return object->Release();
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
    ComPtr<ID3D11PixelShader> psSample; // samples t0 over the whole target (the flood image is only 8x8)
    ComPtr<ID3D11PixelShader> psSlot3;  // samples t3
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

    // ---- the game issues this draw as an indexed draw: same filtering, and the input-assembler state is untouched ----
    {
        const uint16_t indices[3] = { 0, 1, 2 };
        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth = sizeof(indices);
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init { indices, 0, 0 };
        ComPtr<ID3D11Buffer> indexBuffer;
        CHECK(SUCCEEDED(e.device->CreateBuffer(&bd, &init, &indexBuffer)));
        e.context->IASetIndexBuffer(indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
        e.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        cfg.previewUpscale = false;
        e.context->ClearRenderTargetView(game.rtv.Get(), std::array<float, 4> { 0, 0, 0, 0 }.data());
        const auto processedBefore = g_stats.aaProcessed.load();
        r = RunAround(e, snap, nullptr, DrawApi::DrawIndexed);
        CHECK(r.handled && r.drawCalls == 1 && r.seenWidth == 32);
        CHECK(g_stats.aaProcessed == processedBefore + 1);
        const auto indexed = Read(e, game.target.Get(), 32, 32);
        int indexedIntermediate = 0;
        for (const auto& p : indexed)
            indexedIntermediate += (p.r > 12 && p.r < 243) ? 1 : 0;
        CHECK(indexedIntermediate >= 8); // the indexed draw saw the filtered image too

        ComPtr<ID3D11Buffer> boundIndex;
        DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
        UINT indexOffset = 1;
        e.context->IAGetIndexBuffer(&boundIndex, &indexFormat, &indexOffset);
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        e.context->IAGetPrimitiveTopology(&topology);
        ComPtr<ID3D11InputLayout> layout;
        e.context->IAGetInputLayout(&layout);
        CHECK(boundIndex.Get() == indexBuffer.Get() && indexFormat == DXGI_FORMAT_R16_UINT && indexOffset == 0);
        CHECK(topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && layout == nullptr);
        CHECK(BoundT0(e, holder) == sourceSrv.Get());
        holder.Reset();
        cfg.previewUpscale = true;
    }

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

    // ---- the probe's swap trace: reports what t0 held before the swap, during the draw and after it, and changes nothing ----
    {
        cfg.previewUpscale = false;
        const float clear[4] {};
        auto render = [&]
        {
            e.context->ClearRenderTargetView(game.rtv.Get(), clear);
            return RunAround(e, snap);
        };

        probe::g_swapSink.store(nullptr);
        cfg.previewProbe = false;
        const Run plainRun = render();
        const auto plain = Read(e, game.target.Get(), 32, 32);
        CHECK(plainRun.handled);

        // sink installed and probe on: same draw result, one record with the three pointers, game view bound again, no leaked reference
        g_sink = SinkLog {};
        probe::g_swapSink.store(&kTestSink);
        cfg.previewProbe = true;
        const ULONG refsBefore = RefCount(sourceSrv.Get());
        const Run tracedRun = render();
        const auto traced = Read(e, game.target.Get(), 32, 32);
        CHECK(tracedRun.handled && tracedRun.drawCalls == 1);
        CHECK(traced == plain);
        CHECK(g_sink.arms == 1 && g_sink.records.size() == 1);
        if (g_sink.records.size() == 1)
        {
            const probe::SwapRecord& rec = g_sink.records[0];
            CHECK(rec.kind == probe::SwapKind::Aa && rec.slot == 0);
            CHECK(rec.before == reinterpret_cast<uintptr_t>(sourceSrv.Get()));
            CHECK(rec.afterSwap == reinterpret_cast<uintptr_t>(tracedRun.seenAtDraw) && rec.afterSwap != rec.before);
            CHECK(rec.replacement == rec.afterSwap);
            CHECK(rec.afterDraw == rec.before);
        }
        CHECK(BoundT0(e, holder) == sourceSrv.Get());
        holder.Reset();
        CHECK(RefCount(sourceSrv.Get()) == refsBefore);

        // the sink declines (the probe traces once per burst): the draw is still the same and nothing is reported
        g_sink = SinkLog {};
        g_sink.answer = false;
        const Run declined = render();
        CHECK(declined.handled && Read(e, game.target.Get(), 32, 32) == plain);
        CHECK(g_sink.arms == 1 && g_sink.records.empty());

        // probe key off: the sink is not even asked
        g_sink = SinkLog {};
        cfg.previewProbe = false;
        CHECK(render().handled && Read(e, game.target.Get(), 32, 32) == plain);
        CHECK(g_sink.arms == 0 && g_sink.records.empty());

        // no sink: nothing happens either
        cfg.previewProbe = true;
        probe::g_swapSink.store(nullptr);
        CHECK(render().handled && Read(e, game.target.Get(), 32, 32) == plain);
        CHECK(g_sink.arms == 0 && g_sink.records.empty());
        cfg.previewProbe = false;

        // a trace around a draw that leaves a different view in the slot reports the difference (the draw is not ours to fix)
        g_sink = SinkLog {};
        probe::g_swapSink.store(&kTestSink);
        cfg.previewProbe = true;
        {
            ID3D11ShaderResourceView* replacement = sourceSrv.Get();
            probe::SwapTrace trace(e.context.Get(), probe::SwapKind::Flood, 0, replacement);
            trace.Applied();
            trace.Restored();
            trace.Restored(); // reports once
        }
        CHECK(g_sink.records.size() == 1);
        if (g_sink.records.size() == 1)
            CHECK(g_sink.records[0].kind == probe::SwapKind::Flood && g_sink.records[0].before == g_sink.records[0].afterDraw);
        probe::g_swapSink.store(nullptr);
        cfg.previewProbe = false;
    }

    // ---- the debug tint: counted exactly when the tinted pass ran ----
    {
        cfg.previewUpscale = false;
        cfg.previewDebugFlood = false;
        const auto base = g_stats.tintApplied.load();
        cfg.previewDebugTint = false;
        CHECK(RunAround(e, snap).handled);
        CHECK(g_stats.tintApplied == base);
        cfg.previewDebugTint = true;
        CHECK(RunAround(e, snap).handled);
        CHECK(g_stats.tintApplied == base + 1);
        CHECK(RunAround(e, snap).handled);
        CHECK(g_stats.tintApplied == base + 2);
        cfg.previewDebugTint = false;
        CHECK(RunAround(e, snap).handled);
        CHECK(g_stats.tintApplied == base + 2);
    }

    // ---- the flood test: the equipment draw samples solid magenta, whatever the AA could produce ----
    {
        const float clear[4] {};
        const Rgba magenta { 255, 0, 255, 255 };
        auto allMagenta = [&](const std::vector<Rgba>& px)
        {
            for (const auto& p : px)
                if (!(p == magenta))
                    return false;
            return !px.empty();
        };
        e.context->PSSetShader(game.psSample.Get(), nullptr, 0);

        // flood on, AA otherwise fine: the draw runs once, sees the 8x8 flood image, samples magenta everywhere, t0 is bound again
        cfg.previewUpscale = false;
        cfg.previewDebugTint = true; // the tint must not be applied under flood (the pass does not run)
        cfg.previewDebugFlood = true;
        e.context->ClearRenderTargetView(game.rtv.Get(), clear);
        const auto aaBefore = g_stats.aaProcessed.load();
        const auto matchedBefore = g_stats.equipMatched.load();
        const auto skippedBefore = g_stats.aaSkipped.load();
        const auto tintBefore = g_stats.tintApplied.load();
        const auto floodBefore = g_stats.floodSwaps.load();
        const ULONG sourceRefs = RefCount(sourceSrv.Get());
        Run fr = RunAround(e, snap);
        CHECK(fr.handled && fr.drawCalls == 1);
        CHECK(fr.seenAtDraw != nullptr && fr.seenAtDraw != sourceSrv.Get() && fr.seenWidth == 8);
        CHECK(allMagenta(Read(e, game.target.Get(), 32, 32)));
        CHECK(BoundT0(e, holder) == sourceSrv.Get());
        holder.Reset();
        CHECK(RefCount(sourceSrv.Get()) == sourceRefs);
        CHECK(g_stats.floodSwaps == floodBefore + 1 && g_stats.equipMatched == matchedBefore + 1);
        CHECK(g_stats.aaProcessed == aaBefore && g_stats.aaSkipped == skippedBefore && g_stats.tintApplied == tintBefore);
        CHECK(flood::Created());

        // no leak: a second draw leaves the reference counts of the game's view and of the flood view where they were
        ID3D11ShaderResourceView* floodView = fr.seenAtDraw;
        const ULONG floodRefs = RefCount(floodView);
        fr = RunAround(e, snap);
        CHECK(fr.handled && fr.seenAtDraw == floodView);
        CHECK(RefCount(floodView) == floodRefs && RefCount(sourceSrv.Get()) == sourceRefs);
        CHECK(g_stats.floodSwaps == floodBefore + 2);
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC vd {};
            floodView->GetDesc(&vd);
            CHECK(vd.Format == DXGI_FORMAT_R8G8B8A8_UNORM && vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D);
        }

        // 2x setting and an indexed draw do not matter either
        cfg.previewUpscale = true;
        e.context->ClearRenderTargetView(game.rtv.Get(), clear);
        fr = RunAround(e, snap, nullptr, DrawApi::DrawIndexed);
        CHECK(fr.handled && fr.drawCalls == 1 && fr.seenWidth == 8);
        cfg.previewUpscale = false;

        // an equipment image the AA cannot handle (16-bit float): the flood still replaces it
        {
            auto half = MakeTexture(e, 32, 32, DXGI_FORMAT_R16G16B16A16_FLOAT, nullptr);
            auto halfSrv = MakeSrv(e, half.Get());
            ID3D11ShaderResourceView* srv = halfSrv.Get();
            e.context->PSSetShaderResources(0, 1, &srv);
            e.context->ClearRenderTargetView(game.rtv.Get(), clear);
            const auto skipped = g_stats.aaSkipped.load();
            const Run hr = RunAround(e, TableWith(0, half.Get(), 32, 32));
            CHECK(hr.handled && hr.drawCalls == 1 && hr.seenAtDraw != halfSrv.Get() && g_stats.aaSkipped == skipped);
            CHECK(allMagenta(Read(e, game.target.Get(), 32, 32)));
            CHECK(BoundT0(e, holder) == halfSrv.Get());
            holder.Reset();
            ID3D11ShaderResourceView* back = sourceSrv.Get();
            e.context->PSSetShaderResources(0, 1, &back);
        }

        // an sRGB view gets the sRGB view of the flood image
        {
            auto srgbTex = MakeTexture(e, 32, 32, DXGI_FORMAT_B8G8R8A8_TYPELESS, &stairs);
            auto srgbSrv = MakeSrv(e, srgbTex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
            ID3D11ShaderResourceView* srv = srgbSrv.Get();
            e.context->PSSetShaderResources(0, 1, &srv);
            e.context->ClearRenderTargetView(game.rtv.Get(), clear);
            const Run sr = RunAround(e, TableWith(0, srgbTex.Get(), 32, 32));
            CHECK(sr.handled && sr.seenAtDraw != nullptr);
            if (sr.seenAtDraw != nullptr)
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC vd {};
                sr.seenAtDraw->GetDesc(&vd);
                CHECK(vd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
            }
            CHECK(allMagenta(Read(e, game.target.Get(), 32, 32)));
            ID3D11ShaderResourceView* back = sourceSrv.Get();
            e.context->PSSetShaderResources(0, 1, &back);
        }

        // a texture that is not the equipment output stays untouched under flood: not handled, no draw, t0 as it was
        {
            const auto floodNow = g_stats.floodSwaps.load();
            auto otherTex = MakeTexture(e, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, &stairs);
            const Run none = RunAround(e, TableWith(0, otherTex.Get(), 32, 32));
            CHECK(!none.handled && none.drawCalls == 0);
            const Run wrongEntry = RunAround(e, TableWith(3, sourceTex.Get(), 32, 32));
            CHECK(!wrongEntry.handled && wrongEntry.drawCalls == 0);
            CHECK(g_stats.floodSwaps == floodNow);
            CHECK(BoundT0(e, holder) == sourceSrv.Get());
            holder.Reset();
        }

        // flood off again: the AA path (with the tint) is back, the flood image is not used
        cfg.previewDebugFlood = false;
        e.context->ClearRenderTargetView(game.rtv.Get(), clear);
        const auto tintNow = g_stats.tintApplied.load();
        const Run back = RunAround(e, snap);
        CHECK(back.handled && back.seenWidth == 32 && back.seenAtDraw != floodView);
        CHECK(g_stats.tintApplied == tintNow + 1);
        cfg.previewDebugTint = false;
        e.context->PSSetShader(game.ps.Get(), nullptr, 0);
    }

    // ---- the generic flood swap (a draw no module took): only the named slots that hold a plain 2D colour view are replaced ----
    {
        const float clear[4] {};
        const Rgba magenta { 255, 0, 255, 255 };
        const auto grey = std::vector<Rgba>(32 * 32, Grey(90));
        auto previewTex = MakeTexture(e, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, &grey);
        auto previewSrv = MakeSrv(e, previewTex.Get());
        auto uintTex = MakeTexture(e, 32, 32, DXGI_FORMAT_R32_UINT, nullptr);
        auto uintSrv = MakeSrv(e, uintTex.Get());
        auto bystander = MakeTexture(e, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, &stairs);
        auto bystanderSrv = MakeSrv(e, bystander.Get());

        // t0 = the unrelated staircase, t3 = the preview copy, t4 = an integer view, t5 = a bystander
        ID3D11ShaderResourceView* bound[8] { sourceSrv.Get(), nullptr, nullptr, previewSrv.Get(), uintSrv.Get(), bystanderSrv.Get() };
        e.context->PSSetShaderResources(0, 8, bound);
        e.context->PSSetShader(game.psSlot3.Get(), nullptr, 0);

        struct Seen
        {
            int calls = 0;
            ID3D11ShaderResourceView* v[8] {};
        } seen;
        auto native = [&]
        {
            ++seen.calls;
            ID3D11ShaderResourceView* now[8] {};
            e.context->PSGetShaderResources(0, 8, now);
            for (int i = 0; i < 8; ++i)
            {
                seen.v[i] = now[i];
                if (now[i] != nullptr)
                    now[i]->Release();
            }
            e.context->Draw(3, 0);
        };
        const DrawCall call { [](void* p) { (*static_cast<decltype(native)*>(p))(); }, &native };
        auto boundAgain = [&]
        {
            ID3D11ShaderResourceView* now[8] {};
            e.context->PSGetShaderResources(0, 8, now);
            bool same = true;
            for (int i = 0; i < 8; ++i)
            {
                same = same && now[i] == bound[i];
                if (now[i] != nullptr)
                    now[i]->Release();
            }
            return same;
        };

        // the slot named by the mask is replaced, nothing else; the draw runs once and samples magenta; everything is bound again
        ID3D11ShaderResourceView* mine[4] { sourceSrv.Get(), previewSrv.Get(), uintSrv.Get(), bystanderSrv.Get() };
        std::array<ULONG, 4> refsBefore {};
        for (int i = 0; i < 4; ++i)
            refsBefore[i] = RefCount(mine[i]);
        e.context->ClearRenderTargetView(game.rtv.Get(), clear);
        const auto floodBefore = g_stats.floodSwaps.load();
        g_sink = SinkLog {};
        probe::g_swapSink.store(&kTestSink);
        cfg.previewProbe = true;
        CHECK(flood::SwapAndDraw(e.context.Get(), 1u << 3, call));
        probe::g_swapSink.store(nullptr);
        cfg.previewProbe = false;
        CHECK(seen.calls == 1);
        CHECK(seen.v[0] == sourceSrv.Get() && seen.v[4] == uintSrv.Get() && seen.v[5] == bystanderSrv.Get());
        CHECK(seen.v[3] != nullptr && seen.v[3] != previewSrv.Get());
        const auto px = Read(e, game.target.Get(), 32, 32);
        CHECK(!px.empty() && px[0] == magenta && px[32 * 31 + 31] == magenta);
        CHECK(boundAgain());
        for (int i = 0; i < 4; ++i)
            CHECK(RefCount(mine[i]) == refsBefore[i]);
        CHECK(g_stats.floodSwaps == floodBefore + 1);
        CHECK(g_sink.records.size() == 1);
        if (g_sink.records.size() == 1)
        {
            const probe::SwapRecord& rec = g_sink.records[0];
            CHECK(rec.kind == probe::SwapKind::Flood && rec.slot == 3);
            CHECK(rec.before == reinterpret_cast<uintptr_t>(previewSrv.Get()));
            CHECK(rec.afterSwap == reinterpret_cast<uintptr_t>(seen.v[3]) && rec.replacement == rec.afterSwap);
            CHECK(rec.afterDraw == rec.before);
        }

        // two slots in the mask: both replaced
        seen = {};
        CHECK(flood::SwapAndDraw(e.context.Get(), (1u << 0) | (1u << 3), call));
        CHECK(seen.v[0] != sourceSrv.Get() && seen.v[3] != previewSrv.Get() && seen.v[5] == bystanderSrv.Get() && boundAgain());

        // a mask that names only an empty slot, an integer view or a bystander that is not named: nothing replaced, the draw is NOT run
        seen = {};
        const auto before = g_stats.floodSwaps.load();
        CHECK(!flood::SwapAndDraw(e.context.Get(), 1u << 1, call));
        CHECK(!flood::SwapAndDraw(e.context.Get(), 1u << 4, call));
        CHECK(!flood::SwapAndDraw(e.context.Get(), 0, call));
        CHECK(seen.calls == 0 && g_stats.floodSwaps == before && boundAgain());

        // bit 5 only: the bystander is the only replaced view
        seen = {};
        CHECK(flood::SwapAndDraw(e.context.Get(), 1u << 5, call));
        CHECK(seen.v[5] != bystanderSrv.Get() && seen.v[3] == previewSrv.Get() && seen.v[0] == sourceSrv.Get() && boundAgain());

        // an array view and a multisampled view are left alone (CanReplace only takes a plain 2D view): naming them in the mask replaces
        // nothing, the draw is NOT run, everything stays bound (REVIEW-CHARA-2 weak test 5)
        {
            D3D11_TEXTURE2D_DESC td {};
            td.Width = td.Height = 32;
            td.MipLevels = 1;
            td.ArraySize = 2;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> arrayTex;
            CHECK(SUCCEEDED(e.device->CreateTexture2D(&td, nullptr, &arrayTex)));
            ComPtr<ID3D11ShaderResourceView> arraySrv;
            D3D11_SHADER_RESOURCE_VIEW_DESC vd {};
            vd.Format = td.Format;
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray.MipLevels = 1;
            vd.Texture2DArray.ArraySize = 2;
            CHECK(arrayTex != nullptr && SUCCEEDED(e.device->CreateShaderResourceView(arrayTex.Get(), &vd, &arraySrv)));

            td.ArraySize = 1;
            td.SampleDesc.Count = 4;
            ComPtr<ID3D11Texture2D> msTex;
            ComPtr<ID3D11ShaderResourceView> msSrv;
            const HRESULT msHr = e.device->CreateTexture2D(&td, nullptr, &msTex);
            CHECK(SUCCEEDED(msHr)); // the case must exist: a missing multisampled texture is a failure, not a skip
            if (msTex != nullptr)
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC md {};
                md.Format = td.Format;
                md.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
                CHECK(SUCCEEDED(e.device->CreateShaderResourceView(msTex.Get(), &md, &msSrv)));
            }

            ID3D11ShaderResourceView* withViews[8] { sourceSrv.Get(), nullptr, nullptr, previewSrv.Get(), uintSrv.Get(), bystanderSrv.Get(),
                                                     arraySrv.Get(), msSrv.Get() };
            e.context->PSSetShaderResources(0, 8, withViews);
            seen = {};
            const auto floodCount = g_stats.floodSwaps.load();
            CHECK(!flood::SwapAndDraw(e.context.Get(), 1u << 6, call));              // the array view alone
            CHECK(!flood::SwapAndDraw(e.context.Get(), 1u << 7, call));              // the multisampled view alone
            CHECK(!flood::SwapAndDraw(e.context.Get(), (1u << 6) | (1u << 7), call)); // both
            CHECK(seen.calls == 0 && g_stats.floodSwaps == floodCount);
            ID3D11ShaderResourceView* now[8] {};
            e.context->PSGetShaderResources(0, 8, now);
            CHECK(now[6] == arraySrv.Get() && now[7] == msSrv.Get() && now[3] == previewSrv.Get());
            for (auto* v : now)
                if (v != nullptr)
                    v->Release();
            // a plain view named together with them is still replaced; the array and multisampled views are not
            seen = {};
            CHECK(flood::SwapAndDraw(e.context.Get(), (1u << 3) | (1u << 6) | (1u << 7), call));
            CHECK(seen.calls == 1 && seen.v[3] != previewSrv.Get() && seen.v[6] == arraySrv.Get() && seen.v[7] == msSrv.Get());
            e.context->PSSetShaderResources(0, 8, bound);
        }

        // the flood image is created once and handed out again (sRGB family picks the sRGB view)
        ID3D11ShaderResourceView* plain = flood::View(e.context.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        ID3D11ShaderResourceView* srgb = flood::View(e.context.Get(), DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        CHECK(plain != nullptr && srgb != nullptr && plain != srgb && plain == flood::View(e.context.Get(), DXGI_FORMAT_R8G8B8A8_UNORM));
        flood::Release();
        CHECK(!flood::Created());

        ID3D11ShaderResourceView* none[8] {};
        e.context->PSSetShaderResources(0, 8, none);
        ID3D11ShaderResourceView* back = sourceSrv.Get();
        e.context->PSSetShaderResources(0, 1, &back);
        e.context->PSSetShader(game.ps.Get(), nullptr, 0);
    }

    // ---- the flood's own log lines: switch on, swaps per second, switch off (releases the texture) ----
    {
        cfg.previewDebugFlood = true;
        g_testLogLines = 0;
        flood::Update(1000);
        CHECK(g_testLogLines == 1 && g_testLastLog.find("flood on") != std::string::npos);
        CHECK(flood::View(e.context.Get(), DXGI_FORMAT_R8G8B8A8_UNORM) != nullptr && flood::Created());
        g_stats.floodSwaps += 5;
        flood::Update(1100);
        CHECK(g_testLogLines == 1);
        flood::Update(2200);
        CHECK(g_testLogLines == 2 && g_testLastLog == "CharaView probe: flood swaps 5 in the last second (total 5)");
        flood::Update(2300);
        CHECK(g_testLogLines == 2); // nothing new: no line for a second without swaps
        cfg.previewDebugFlood = false;
        flood::Update(2400);
        CHECK(g_testLogLines == 3 && g_testLastLog.find("flood off") != std::string::npos && !flood::Created());
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
