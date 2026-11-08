// Engine light controls, the lamp arm (ENG2) on the D3D11 WARP software adapter with the real EngineLightLamps.cpp.
//
// The contract a player would feel if it broke:
//   a point-light volume draw (additive ONE/ONE/ADD on two R11G11B10 targets, a g_LightParam shader) contributes `factor` times its light to BOTH light buffers, and
//   nothing else does: the directional light (not blended), a one-target reader, an additive pair in another format and a draw without the light bit come out unscaled;
//   the game's blend state, blend factor and sample mask come back bit for bit after the draw, also when the draw throws (mutant: the state not restored);
//   a factor of 1 builds no wrapper and touches no state (a day frame is the engine's own picture);
//   the wrapper composes with what the seam does around the draw: an ambient-occlusion-style reader swap that calls the wrapped draw still gets the factor;
//   the capture frame's row carries the whole bound g_LightParam of each volume draw (and no other frame builds one).
#include "check.h"

#include "context_set_counter.h"

#include "ext/ffxiv/diag/StageDump.h"
#include "ext/ffxiv/enginelight/EngineLightLamps.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
namespace el = ext::ffxiv::enginelight;
namespace eng = ext::ffxiv::engine;

namespace
{
constexpr UINT kSize = 8;

const char* kHlsl = R"hlsl(
cbuffer Light : register(b2) { float4 Tint; };
Texture2D<float4> Reader : register(t1);
struct VSOut { float4 pos : SV_Position; };
VSOut VS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
struct Out2 { float4 a : SV_Target0; float4 b : SV_Target1; };
// a point-light volume / the directional light: two light buffers (diffuse, specular)
Out2 PS_Pair(VSOut i)
{
    Out2 o;
    o.a = float4(0.25, 0.5, 1.0, 1.0);
    o.b = float4(0.125, 0.25, 0.5, 1.0);
    return o;
}
// the same with a reader (the ambient occlusion swap's texture) multiplied in
Out2 PS_PairReader(VSOut i)
{
    const float k = Reader.Load(int3(0, 0, 0)).x;
    Out2 o;
    o.a = float4(0.25, 0.5, 1.0, 1.0) * k;
    o.b = float4(0.125, 0.25, 0.5, 1.0) * k;
    return o;
}
float4 PS_One(VSOut i) : SV_Target { return float4(0.25, 0.5, 1.0, 1.0); }
)hlsl";

float F11(uint32_t v)
{
    const uint32_t e = (v >> 6) & 31u, m = v & 63u;
    return e == 0 ? float(m) / 64.0f * std::ldexp(1.0f, -14) : (1.0f + float(m) / 64.0f) * std::ldexp(1.0f, int(e) - 15);
}
float F10(uint32_t v)
{
    const uint32_t e = (v >> 5) & 31u, m = v & 31u;
    return e == 0 ? float(m) / 32.0f * std::ldexp(1.0f, -14) : (1.0f + float(m) / 32.0f) * std::ldexp(1.0f, int(e) - 15);
}

struct Rgb
{
    float r = 0, g = 0, b = 0;
};

struct Fixture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> psPair, psReader, psOne;
    ComPtr<ID3D11Texture2D> tex[2];
    ComPtr<ID3D11RenderTargetView> rtv[2];
    ComPtr<ID3D11Texture2D> texFloat[2];
    ComPtr<ID3D11RenderTargetView> rtvFloat[2];
    ComPtr<ID3D11Texture2D> texR8;
    ComPtr<ID3D11RenderTargetView> rtvR8;
    ComPtr<ID3D11Buffer> lightBuffer;
    ComPtr<ID3D11ShaderResourceView> readerSrv;
    ComPtr<ID3D11BlendState> additive, additiveIndependentBad, none, maxBlend;
    ComPtr<ID3D11RasterizerState> noCull;

    bool Init()
    {
        const D3D_FEATURE_LEVEL levels[] { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL got {};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, &got, &context)))
            return false;
        auto compile = [&](const char* entry, const char* profile, ComPtr<ID3DBlob>& blob) {
            ComPtr<ID3DBlob> err;
            return SUCCEEDED(D3DCompile(kHlsl, std::strlen(kHlsl), "lamps", nullptr, nullptr, entry, profile, 0, 0, &blob, &err));
        };
        ComPtr<ID3DBlob> vsb, p1, p2, p3;
        if (!compile("VS", "vs_5_0", vsb) || !compile("PS_Pair", "ps_5_0", p1) || !compile("PS_PairReader", "ps_5_0", p2) || !compile("PS_One", "ps_5_0", p3))
            return false;
        if (FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs)) ||
            FAILED(device->CreatePixelShader(p1->GetBufferPointer(), p1->GetBufferSize(), nullptr, &psPair)) ||
            FAILED(device->CreatePixelShader(p2->GetBufferPointer(), p2->GetBufferSize(), nullptr, &psReader)) ||
            FAILED(device->CreatePixelShader(p3->GetBufferPointer(), p3->GetBufferSize(), nullptr, &psOne)))
            return false;
        auto makeTarget = [&](DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& t, ComPtr<ID3D11RenderTargetView>& v) {
            D3D11_TEXTURE2D_DESC d {};
            d.Width = d.Height = kSize;
            d.MipLevels = d.ArraySize = 1;
            d.Format = format;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET;
            return SUCCEEDED(device->CreateTexture2D(&d, nullptr, &t)) && SUCCEEDED(device->CreateRenderTargetView(t.Get(), nullptr, &v));
        };
        for (int i = 0; i < 2; ++i)
            if (!makeTarget(DXGI_FORMAT_R11G11B10_FLOAT, tex[i], rtv[i]) || !makeTarget(DXGI_FORMAT_R16G16B16A16_FLOAT, texFloat[i], rtvFloat[i]))
                return false;
        if (!makeTarget(DXGI_FORMAT_R8_UNORM, texR8, rtvR8))
            return false;

        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth = 512;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        std::vector<unsigned char> bytes(512);
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<unsigned char>((i * 7 + 3) & 0xFF);
        D3D11_SUBRESOURCE_DATA init { bytes.data(), 0, 0 };
        if (FAILED(device->CreateBuffer(&bd, &init, &lightBuffer)))
            return false;

        // the AO-like reader: a 1x1 float texture holding 0.5
        D3D11_TEXTURE2D_DESC readerDesc {};
        readerDesc.Width = readerDesc.Height = 1;
        readerDesc.MipLevels = readerDesc.ArraySize = 1;
        readerDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        readerDesc.SampleDesc.Count = 1;
        readerDesc.Usage = D3D11_USAGE_DEFAULT;
        readerDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const float half[4] { 0.5f, 0.5f, 0.5f, 0.5f };
        D3D11_SUBRESOURCE_DATA readerData { half, 16, 16 };
        ComPtr<ID3D11Texture2D> readerTex;
        if (FAILED(device->CreateTexture2D(&readerDesc, &readerData, &readerTex)) || FAILED(device->CreateShaderResourceView(readerTex.Get(), nullptr, &readerSrv)))
            return false;

        D3D11_RASTERIZER_DESC raster {};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE; // the fullscreen triangle is back-facing in screen space
        raster.DepthClipEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&raster, &noCull)))
            return false;

        auto blend = [&](bool enable, D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND_OP op, bool independent, bool enable1, ComPtr<ID3D11BlendState>& out) {
            D3D11_BLEND_DESC b {};
            b.IndependentBlendEnable = independent;
            for (int i = 0; i < 2; ++i)
            {
                D3D11_RENDER_TARGET_BLEND_DESC& rt = b.RenderTarget[i];
                rt.BlendEnable = (i == 0 ? enable : enable1);
                rt.SrcBlend = src;
                rt.DestBlend = dst;
                rt.BlendOp = op;
                rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
                rt.DestBlendAlpha = D3D11_BLEND_ONE;
                rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                rt.RenderTargetWriteMask = 0xF;
            }
            return SUCCEEDED(device->CreateBlendState(&b, &out));
        };
        return blend(true, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, false, true, additive) &&
               blend(true, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, true, false, additiveIndependentBad) &&
               blend(false, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, false, false, none) &&
               blend(true, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX, false, true, maxBlend);
    }

    void ClearPair(ID3D11RenderTargetView* a, ID3D11RenderTargetView* b, float v)
    {
        const float c[4] { v, v, v, v };
        context->ClearRenderTargetView(a, c);
        context->ClearRenderTargetView(b, c);
    }

    // Pixel (0,0) of a texture as RGB.
    Rgb Read(ID3D11Texture2D* source, DXGI_FORMAT format)
    {
        D3D11_TEXTURE2D_DESC d {};
        source->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        device->CreateTexture2D(&d, nullptr, &staging);
        context->CopyResource(staging.Get(), source);
        D3D11_MAPPED_SUBRESOURCE m {};
        context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
        Rgb out;
        if (format == DXGI_FORMAT_R11G11B10_FLOAT)
        {
            const uint32_t v = *static_cast<const uint32_t*>(m.pData);
            out = { F11(v & 0x7FFu), F11((v >> 11) & 0x7FFu), F10((v >> 22) & 0x3FFu) };
        }
        else if (format == DXGI_FORMAT_R8_UNORM)
            out = { float(*static_cast<const unsigned char*>(m.pData)) / 255.0f, 0, 0 };
        context->Unmap(staging.Get(), 0);
        return out;
    }

    void Setup(ID3D11PixelShader* ps, ID3D11BlendState* blend, ID3D11RenderTargetView* a, ID3D11RenderTargetView* b, const float factor[4], UINT mask)
    {
        D3D11_VIEWPORT vp { 0, 0, float(kSize), float(kSize), 0, 1 };
        context->RSSetViewports(1, &vp);
        context->RSSetState(noCull.Get());
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        ID3D11RenderTargetView* rtvs[2] { a, b };
        context->OMSetRenderTargets(b != nullptr ? 2 : 1, rtvs, nullptr);
        context->OMSetBlendState(blend, factor, mask);
        ID3D11Buffer* buffers[1] { lightBuffer.Get() };
        context->PSSetConstantBuffers(2, 1, buffers);
    }
};

// The game's real draw call: a bare Draw(3).
struct GameCall
{
    ID3D11DeviceContext* context = nullptr;
    int calls = 0;
    bool throwAfter = false;
};
void GameDraw(void* opaque)
{
    GameCall& call = *static_cast<GameCall*>(opaque);
    call.context->Draw(3, 0);
    ++call.calls;
    if (call.throwAfter)
        throw 42;
}

// The seam's ambient-occlusion-style swap around the (wrapped) draw: it binds a reader texture at t1, calls the draw it was given, puts the game's binding back.
struct SwapCall
{
    ID3D11DeviceContext* context = nullptr;
    ID3D11ShaderResourceView* reader = nullptr;
    el::DrawFn inner = nullptr;
    void* innerUser = nullptr;
};
void SwapAround(void* opaque)
{
    SwapCall& call = *static_cast<SwapCall*>(opaque);
    ID3D11ShaderResourceView* views[1] { call.reader };
    call.context->PSSetShaderResources(1, 1, views);
    call.inner(call.innerUser);
    ID3D11ShaderResourceView* none[1] {};
    call.context->PSSetShaderResources(1, 1, none);
}

// One game draw as the seam sees it: anchors with the light bit (or not), the wrapper substituted when the factor asks for it.
struct Seen
{
    bool wrapped = false;
};
Seen DrawThroughSeam(Fixture& f, eng::Anchors anchors, GameCall& game, bool throwsOut = false)
{
    el::DrawFn draw = &GameDraw;
    void* user = &game;
    el::LampDraw lamp;
    Seen seen;
    setcount::Scope counting; // every pipeline-state setter the arm calls from here to the end of the draw is counted
    seen.wrapped = el::PrepareLampDraw(f.context.Get(), anchors, draw, user, lamp);
    if (throwsOut)
    {
        try
        {
            draw(user);
        }
        catch (int)
        {
        }
    }
    else
        draw(user);
    return seen;
}

bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

std::vector<std::string> g_rows;
void RowSink(const std::string& row) { g_rows.push_back(row); }
} // namespace

int main()
{
    Fixture f;
    if (!f.Init())
    {
        std::printf("engine_light_lamps_warp: the WARP fixture could not be built\n");
        return 2;
    }
    if (!setcount::Install(f.context.Get()))
        return 2;
    const eng::Anchors lightAnchors = eng::kShaftsLightBit | (eng::Anchors(2) << 44);
    const eng::Anchors otherAnchors = eng::kShaftsCompositeBit;
    const float gameFactor[4] { 0.1f, 0.2f, 0.3f, 0.4f };
    const UINT gameMask = 0xABCD1237u; // bit 0 set: the single sample must stay covered
    GameCall game;
    game.context = f.context.Get();

    // ---- a factor of 1: no wrapper, no state touched; the draw adds the light once ------------------------------------------------------
    {
        el::SetLampFactor(1.0f);
        f.ClearPair(f.rtv[0].Get(), f.rtv[1].Get(), 0.0f);
        f.Setup(f.psPair.Get(), f.additive.Get(), f.rtv[0].Get(), f.rtv[1].Get(), gameFactor, gameMask);
        setcount::g_counts = 0;
        const Seen seen = DrawThroughSeam(f, lightAnchors, game);
        CHECK(!seen.wrapped);
        CHECK_EQ(setcount::g_counts, 0L); // at an identity factor nothing of ours rebinds or changes any game state: ZERO setter calls
        const Rgb a = f.Read(f.tex[0].Get(), DXGI_FORMAT_R11G11B10_FLOAT);
        const Rgb b = f.Read(f.tex[1].Get(), DXGI_FORMAT_R11G11B10_FLOAT);
        CHECK(Near(a.r, 0.25f, 0.0f) && Near(a.g, 0.5f, 0.0f) && Near(a.b, 1.0f, 0.0f));
        CHECK(Near(b.r, 0.125f, 0.0f) && Near(b.g, 0.25f, 0.0f) && Near(b.b, 0.5f, 0.0f));
    }

    // ---- a volume draw: BOTH light buffers carry factor x the light; the game's state comes back bit for bit (M1) ----------------------------
    {
        el::SetLampFactor(3.0f);
        f.ClearPair(f.rtv[0].Get(), f.rtv[1].Get(), 0.0f);
        f.Setup(f.psPair.Get(), f.additive.Get(), f.rtv[0].Get(), f.rtv[1].Get(), gameFactor, gameMask);
        const el::LampStatus before = el::CurrentLampStatus();
        setcount::g_counts = 0;
        const Seen seen = DrawThroughSeam(f, lightAnchors, game);
        CHECK(seen.wrapped);
        CHECK_EQ(setcount::g_counts, 2L); // the positive control: exactly the derived state set and the game's state set back (so the counter really sees our calls)
        const Rgb a = f.Read(f.tex[0].Get(), DXGI_FORMAT_R11G11B10_FLOAT);
        const Rgb b = f.Read(f.tex[1].Get(), DXGI_FORMAT_R11G11B10_FLOAT);
        CHECK(Near(a.r, 0.75f, 0.0f) && Near(a.g, 1.5f, 0.0f) && Near(a.b, 3.0f, 0.0f));      // 3 x (0.25, 0.5, 1.0)
        CHECK(Near(b.r, 0.375f, 0.0f) && Near(b.g, 0.75f, 0.0f) && Near(b.b, 1.5f, 0.0f));    // 3 x (0.125, 0.25, 0.5)
        // the game's blend state, factor and sample mask are exactly what they were
        ComPtr<ID3D11BlendState> state;
        float factor[4] {};
        UINT mask = 0;
        f.context->OMGetBlendState(&state, factor, &mask);
        CHECK(state.Get() == f.additive.Get());
        CHECK(std::memcmp(factor, gameFactor, sizeof factor) == 0);
        CHECK_EQ(mask, gameMask);
        const el::LampStatus after = el::CurrentLampStatus();
        CHECK_EQ(after.scaled, before.scaled + 1);
        CHECK_EQ(after.statesCreated, before.statesCreated == 0 ? 1u : before.statesCreated);   // one derived state, built once
        // a second volume draw reuses it
        f.Setup(f.psPair.Get(), f.additive.Get(), f.rtv[0].Get(), f.rtv[1].Get(), gameFactor, gameMask);
        DrawThroughSeam(f, lightAnchors, game);
        CHECK_EQ(el::CurrentLampStatus().statesCreated, after.statesCreated);
        // another factor (the weight moved): the same derived state, a different factor
        el::SetLampFactor(2.0f);
        f.ClearPair(f.rtv[0].Get(), f.rtv[1].Get(), 0.0f);
        f.Setup(f.psPair.Get(), f.additive.Get(), f.rtv[0].Get(), f.rtv[1].Get(), gameFactor, gameMask);
        DrawThroughSeam(f, lightAnchors, game);
        const Rgb c = f.Read(f.tex[0].Get(), DXGI_FORMAT_R11G11B10_FLOAT);
        CHECK(Near(c.r, 0.5f, 0.0f) && Near(c.g, 1.0f, 0.0f) && Near(c.b, 2.0f, 0.0f));
        CHECK_EQ(el::CurrentLampStatus().statesCreated, after.statesCreated);
    }

    // ---- a draw that throws: the state is restored all the same -------------------------------------------------------------------------------------
    {
        el::SetLampFactor(3.0f);
        f.Setup(f.psPair.Get(), f.additive.Get(), f.rtv[0].Get(), f.rtv[1].Get(), gameFactor, gameMask);
        GameCall throwing = game;
        throwing.calls = 0;
        throwing.throwAfter = true;
        DrawThroughSeam(f, lightAnchors, throwing, true);
        CHECK_EQ(throwing.calls, 1);
        ComPtr<ID3D11BlendState> state;
        float factor[4] {};
        UINT mask = 0;
        f.context->OMGetBlendState(&state, factor, &mask);
        CHECK(state.Get() == f.additive.Get());
        CHECK(std::memcmp(factor, gameFactor, sizeof factor) == 0);
        CHECK_EQ(mask, gameMask);
    }

    // ---- what is NOT a volume is left alone (M2) ------------------------------------------------------------------------------------------------------
    {
        el::SetLampFactor(3.0f);
        // the directional light: not blended, two R11G11B10 targets, a light-bit shader
        f.ClearPair(f.rtv[0].Get(), f.rtv[1].Get(), 0.0f);
        f.Setup(f.psPair.Get(), f.none.Get(), f.rtv[0].Get(), f.rtv[1].Get(), gameFactor, gameMask);
        const uint64_t scaledBefore = el::CurrentLampStatus().scaled;
        DrawThroughSeam(f, lightAnchors, game);
        Rgb a = f.Read(f.tex[0].Get(), DXGI_FORMAT_R11G11B10_FLOAT);
        CHECK(Near(a.r, 0.25f, 0.0f) && Near(a.g, 0.5f, 0.0f) && Near(a.b, 1.0f, 0.0f));
        // a blended pair in another format (not the light buffers)
        f.Setup(f.psPair.Get(), f.additive.Get(), f.rtvFloat[0].Get(), f.rtvFloat[1].Get(), gameFactor, gameMask);
        const float zero[4] {};
        f.context->ClearRenderTargetView(f.rtvFloat[0].Get(), zero);
        DrawThroughSeam(f, lightAnchors, game);
        // a one-target additive draw in R11G11B10 (a reader of one light buffer): one render target is not the pair
        f.ClearPair(f.rtv[0].Get(), f.rtv[1].Get(), 0.0f);
        f.Setup(f.psOne.Get(), f.additive.Get(), f.rtv[0].Get(), nullptr, gameFactor, gameMask);
        DrawThroughSeam(f, lightAnchors, game);
        a = f.Read(f.tex[0].Get(), DXGI_FORMAT_R11G11B10_FLOAT);
        CHECK(Near(a.r, 0.25f, 0.0f) && Near(a.g, 0.5f, 0.0f) && Near(a.b, 1.0f, 0.0f));
        // the shadow-mask reader: one R8 target, MAX blend
        f.Setup(f.psOne.Get(), f.maxBlend.Get(), f.rtvR8.Get(), nullptr, gameFactor, gameMask);
        DrawThroughSeam(f, lightAnchors, game);
        // an additive state whose second target is not additive (independent blend): not the volume shape
        f.ClearPair(f.rtv[0].Get(), f.rtv[1].Get(), 0.0f);
        f.Setup(f.psPair.Get(), f.additiveIndependentBad.Get(), f.rtv[0].Get(), f.rtv[1].Get(), gameFactor, gameMask);
        DrawThroughSeam(f, lightAnchors, game);
        a = f.Read(f.tex[0].Get(), DXGI_FORMAT_R11G11B10_FLOAT);
        CHECK(Near(a.r, 0.25f, 0.0f));                  // RT1 is not blending, so only RT0 added: the shape test refused it (a factor of 3 would read 0.75)
        CHECK_EQ(el::CurrentLampStatus().scaled, scaledBefore);
        CHECK(el::CurrentLampStatus().notVolume >= 4);
        // a draw without the light bit never gets a wrapper, whatever the factor
        f.ClearPair(f.rtv[0].Get(), f.rtv[1].Get(), 0.0f);
        f.Setup(f.psPair.Get(), f.additive.Get(), f.rtv[0].Get(), f.rtv[1].Get(), gameFactor, gameMask);
        const Seen seen = DrawThroughSeam(f, otherAnchors, game);
        CHECK(!seen.wrapped);
        a = f.Read(f.tex[0].Get(), DXGI_FORMAT_R11G11B10_FLOAT);
        CHECK(Near(a.r, 0.25f, 0.0f));
    }

    // ---- composes with a reader swap around the draw (the seam's ambient occlusion / puddles path) ---------------------------------------------------------
    {
        el::SetLampFactor(3.0f);
        f.ClearPair(f.rtv[0].Get(), f.rtv[1].Get(), 0.0f);
        f.Setup(f.psReader.Get(), f.additive.Get(), f.rtv[0].Get(), f.rtv[1].Get(), gameFactor, gameMask);
        el::DrawFn draw = &GameDraw;
        void* user = &game;
        el::LampDraw lamp;
        CHECK(el::PrepareLampDraw(f.context.Get(), lightAnchors, draw, user, lamp));
        SwapCall swap { f.context.Get(), f.readerSrv.Get(), draw, user };   // the swap calls the WRAPPED draw, as OnGameDraw's branches do
        SwapAround(&swap);
        const Rgb a = f.Read(f.tex[0].Get(), DXGI_FORMAT_R11G11B10_FLOAT);
        CHECK(Near(a.r, 0.375f, 0.0f) && Near(a.g, 0.75f, 0.0f) && Near(a.b, 1.5f, 0.0f));   // 3 x 0.5 (the reader) x (0.25, 0.5, 1.0): both applied
    }

    // ---- the capture frame's row: the whole bound g_LightParam of each volume draw, and no row on any other frame -----------------------------------------
    {
        el::SetLampFactor(3.0f);
        el::SetLampRowSinkForTest(&RowSink);
        g_rows.clear();
        f.Setup(f.psPair.Get(), f.additive.Get(), f.rtv[0].Get(), f.rtv[1].Get(), gameFactor, gameMask);
        DrawThroughSeam(f, lightAnchors, game);
        CHECK(g_rows.empty());                          // the capture frame is closed: no string is built
        ext::ffxiv::diag::detail::g_stageOpen.store(ext::ffxiv::diag::StageBitOf(ext::ffxiv::diag::StageId::NightGrade), std::memory_order_relaxed);
        DrawThroughSeam(f, lightAnchors, game);
        f.Setup(f.psPair.Get(), f.none.Get(), f.rtv[0].Get(), f.rtv[1].Get(), gameFactor, gameMask);
        DrawThroughSeam(f, lightAnchors, game);          // the directional light: not a volume, no row
        ext::ffxiv::diag::detail::g_stageOpen.store(0, std::memory_order_relaxed);
        CHECK_EQ(g_rows.size(), size_t(1));
        if (!g_rows.empty())
        {
            const std::string& row = g_rows[0];
            CHECK(row.find(",volume,3.0000,512,") != std::string::npos);
            std::string expected;
            for (size_t i = 0; i < 512; ++i)
            {
                char hex[4];
                std::snprintf(hex, sizeof hex, "%02x", unsigned((i * 7 + 3) & 0xFF));
                expected += hex;
            }
            CHECK(row.find(expected) != std::string::npos);   // all 512 bytes, in order
        }
        el::SetLampRowSinkForTest(nullptr);
    }

    // ---- at an identity factor the arm does not even look at the context: a poison pointer is never dereferenced ------------------------------------------------
    {
        el::SetLampFactor(1.0f);
        ID3D11DeviceContext* poison = reinterpret_cast<ID3D11DeviceContext*>(static_cast<uintptr_t>(0x10));
        el::DrawFn draw = &GameDraw;
        void* user = &game;
        el::LampDraw lamp;
        CHECK(!el::PrepareLampDraw(poison, lightAnchors, draw, user, lamp));
        CHECK(draw == &GameDraw && user == &game);        // the caller's draw function and user are untouched
        el::SetLampFactor(3.0f);
        CHECK(!el::PrepareLampDraw(poison, otherAnchors, draw, user, lamp)); // not a light draw: still nothing, whatever the factor
        CHECK(draw == &GameDraw && user == &game);
        el::SetLampFactor(1.0f);
    }

    el::ReleaseLamps();
    return Finish("engine_light_lamps_warp");
}
