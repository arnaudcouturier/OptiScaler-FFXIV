// ext/core/D3D11ExplicitState.h on the D3D11 WARP adapter: the guard saves exactly the categories a pass declares, Neutralise() sets HS/DS/GS, stream-out
// and predication to none, Restore() (and the destructor, also while an exception unwinds) puts every declared category back, exactly: the constant-buffer
// range, several viewports and scissors, the blend factor and sample mask, the stencil reference, a bound UAV and its counter. A category that is not
// declared is neither read nor written. OmSnapshot on its own puts the render targets and the UAV slots back after OMSetRenderTargets dropped them.
// FullStateSnapshot (the character-preview NGX calls) puts back every category a stand-in trashed, outputs before inputs (aliasing cases).
// Its SRV restore is bounded (one call [0, hi] per stage, none when unchanged): a recording layer of real Detours hooks pins the ranges.
#include "ext/core/D3D11ExplicitState.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <detours/detours.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace ext::core::explicit_state;

static int g_failures = 0;
#define CHECK(cond)                                                                                                                                  \
    do                                                                                                                                               \
    {                                                                                                                                                \
        if (!(cond))                                                                                                                                 \
        {                                                                                                                                            \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                      \
            ++g_failures;                                                                                                                            \
        }                                                                                                                                            \
    } while (0)

namespace
{
using Snap = std::map<std::string, uint64_t>;
template <class T> uint64_t P(T* p) { return reinterpret_cast<uint64_t>(p); }
uint64_t Bits(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; }

// ---- a recording layer on the six SetShaderResources entries (real Detours hooks) -------------------------------------------------------------------
// What a restore sends to the hooked setters, as ResTrack (ResTrack_dx11.cpp NoteGraphicsShaderResources / NoteCSShaderResources) sees it: it remembers the
// WHOLE range [start, start + num) of every call, null views included, so a restore must not send more than the slots that matter.
namespace rec
{
enum { kVS, kHS, kDS, kGS, kPS, kCS, kStages };
constexpr int kVtable[kStages] { 25, 59, 63, 31, 8, 67 }; // VSSet.., HSSet.., DSSet.., GSSet.., PSSet.., CSSetShaderResources
struct Call
{
    int stage;
    UINT start, num;
};
using SetSrvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
inline SetSrvFn g_orig[kStages] {};
inline void* g_target[kStages] {};
inline bool g_installed = false;
inline bool g_on = false;
inline ID3D11DeviceContext* g_watched = nullptr;
inline std::vector<Call> g_calls;
template <int I> void STDMETHODCALLTYPE Hook(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v)
{
    g_orig[I](c, s, n, v);
    if (g_on && c == g_watched)
        g_calls.push_back({ I, s, n });
}
inline const SetSrvFn kHooks[kStages] { Hook<0>, Hook<1>, Hook<2>, Hook<3>, Hook<4>, Hook<5> };

// Attach to the entries of this world's context; Uninstall() before the next world is created (the hooks live only inside the block that records).
inline bool Install(ID3D11DeviceContext* ctx)
{
    void** vt = *reinterpret_cast<void***>(ctx);
    if (g_installed)
        return false;
    for (int i = 0; i < kStages; ++i)
    {
        g_target[i] = vt[kVtable[i]];
        g_orig[i] = reinterpret_cast<SetSrvFn>(vt[kVtable[i]]);
    }
    if (DetourTransactionBegin() != NO_ERROR)
        return false;
    DetourUpdateThread(GetCurrentThread());
    bool ok = true;
    for (int i = 0; i < kStages; ++i)
        ok &= DetourAttach(reinterpret_cast<PVOID*>(&g_orig[i]), reinterpret_cast<PVOID>(kHooks[i])) == NO_ERROR;
    ok &= DetourTransactionCommit() == NO_ERROR;
    g_installed = ok;
    return ok;
}
inline void Uninstall()
{
    if (!g_installed)
        return;
    g_on = false;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    for (int i = 0; i < kStages; ++i)
        DetourDetach(reinterpret_cast<PVOID*>(&g_orig[i]), reinterpret_cast<PVOID>(kHooks[i]));
    DetourTransactionCommit();
    g_installed = false;
}
inline void Begin(ID3D11DeviceContext* ctx)
{
    g_calls.clear();
    g_watched = ctx;
    g_on = true;
}
inline void End() { g_on = false; }
inline std::vector<Call> Of(int stage)
{
    std::vector<Call> out;
    for (const Call& c : g_calls)
        if (c.stage == stage)
            out.push_back(c);
    return out;
}

// One SRV array per stage (VS, HS, DS, GS, PS, CS), all 128 slots: bound with one bulk call each.
using Plan = std::array<std::array<ID3D11ShaderResourceView*, kMaxStageSrvSlots>, kStages>;
inline void Apply(ID3D11DeviceContext* c, const Plan& p)
{
    c->VSSetShaderResources(0, kMaxStageSrvSlots, p[kVS].data());
    c->HSSetShaderResources(0, kMaxStageSrvSlots, p[kHS].data());
    c->DSSetShaderResources(0, kMaxStageSrvSlots, p[kDS].data());
    c->GSSetShaderResources(0, kMaxStageSrvSlots, p[kGS].data());
    c->PSSetShaderResources(0, kMaxStageSrvSlots, p[kPS].data());
    c->CSSetShaderResources(0, kMaxStageSrvSlots, p[kCS].data());
}
} // namespace rec

struct World
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> c1;
    bool fl111 = false;
    bool only110 = false; // create the device at feature level 11_0 only
    ComPtr<ID3D11Texture2D> targetTexture, ownTexture;
    ComPtr<ID3D11RenderTargetView> gameRtv, ownRtv;
    ComPtr<ID3D11VertexShader> gameVs, ownVs;
    ComPtr<ID3D11HullShader> gameHs;
    ComPtr<ID3D11DomainShader> gameDs;
    ComPtr<ID3D11GeometryShader> gameGs;
    ComPtr<ID3D11PixelShader> gamePs, ownPs;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> cbA, cbB, uavBuffer, counterDst, counterStage;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11SamplerState> samplerA, samplerB;
    ComPtr<ID3D11Texture2D> dummyTexture;
    std::array<ComPtr<ID3D11ShaderResourceView>, 8> srv;
    ComPtr<ID3D11RasterizerState> gameRs, ownRs;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11Predicate> predicate;
    D3D11_VIEWPORT viewports[2] { { 0, 0, 64, 64, 0, 1 }, { 4, 4, 32, 32, 0.25f, 0.75f } };
    D3D11_RECT rects[2] { { 0, 0, 64, 64 }, { 2, 2, 30, 30 } };
    const FLOAT blendFactor[4] { 0.25f, 0.5f, 0.75f, 1.0f };
    UINT first[2] { 16, 32 }, num[2] { 16, 16 };

    bool Create()
    {
        const D3D_FEATURE_LEVEL levels[] { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL got {};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, only110 ? levels + 1 : levels, only110 ? 1 : 2, D3D11_SDK_VERSION, &device, &got, &context)) || !device || !context)
            return false;
        fl111 = got >= D3D_FEATURE_LEVEL_11_1;
        if (FAILED(context.As(&c1)))
            return false;
        const char* hlsl = R"hlsl(
struct V { float3 p : POSITION; };
struct CP { float3 p : POSITION; };
struct F { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
struct G { float4 p : SV_Position; };
CP VS(V i) { CP o; o.p = i.p; return o; }
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")][outputcontrolpoints(3)][patchconstantfunc("PC")]
CP HS(InputPatch<CP,3> p, uint i : SV_OutputControlPointID) { return p[i]; }
F PC(InputPatch<CP,3> p) { F f; f.edge[0] = f.edge[1] = f.edge[2] = f.inside = 2; return f; }
[domain("tri")] float4 DS(F f, float3 uv : SV_DomainLocation, const OutputPatch<CP,3> p) : SV_Position { return float4(p[0].p * uv.x + p[1].p * uv.y + p[2].p * uv.z, 1); }
[maxvertexcount(3)] void GS(triangle G v[3], inout TriangleStream<G> s) { s.Append(v[0]); s.Append(v[1]); s.Append(v[2]); }
float4 VSOwn(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * 2 - 1, 0, 1); }
float4 PS() : SV_Target { return float4(0, 1, 0, 1); }
)hlsl";
        auto compile = [&](const char* entry, const char* profile, ComPtr<ID3DBlob>& blob) {
            ComPtr<ID3DBlob> err;
            return SUCCEEDED(D3DCompile(hlsl, std::strlen(hlsl), "explicit-state", nullptr, nullptr, entry, profile, 0, 0, &blob, &err));
        };
        ComPtr<ID3DBlob> vs, hs, ds, gs, ps, own;
        if (!compile("VS", "vs_5_0", vs) || !compile("HS", "hs_5_0", hs) || !compile("DS", "ds_5_0", ds) || !compile("GS", "gs_5_0", gs) ||
            !compile("PS", "ps_5_0", ps) || !compile("VSOwn", "vs_5_0", own))
            return false;
        bool ok = SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &gameVs)) &&
                  SUCCEEDED(device->CreateVertexShader(own->GetBufferPointer(), own->GetBufferSize(), nullptr, &ownVs)) &&
                  SUCCEEDED(device->CreateHullShader(hs->GetBufferPointer(), hs->GetBufferSize(), nullptr, &gameHs)) &&
                  SUCCEEDED(device->CreateDomainShader(ds->GetBufferPointer(), ds->GetBufferSize(), nullptr, &gameDs)) &&
                  SUCCEEDED(device->CreateGeometryShader(gs->GetBufferPointer(), gs->GetBufferSize(), nullptr, &gameGs)) &&
                  SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &gamePs)) &&
                  SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ownPs));
        const D3D11_INPUT_ELEMENT_DESC element { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
        ok = ok && SUCCEEDED(device->CreateInputLayout(&element, 1, vs->GetBufferPointer(), vs->GetBufferSize(), &layout));
        D3D11_TEXTURE2D_DESC td {};
        td.Width = td.Height = 64; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ok = ok && SUCCEEDED(device->CreateTexture2D(&td, nullptr, &targetTexture)) && SUCCEEDED(device->CreateRenderTargetView(targetTexture.Get(), nullptr, &gameRtv)) &&
             SUCCEEDED(device->CreateTexture2D(&td, nullptr, &ownTexture)) && SUCCEEDED(device->CreateRenderTargetView(ownTexture.Get(), nullptr, &ownRtv));
        D3D11_BUFFER_DESC cb {}; cb.ByteWidth = 4096; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ok = ok && SUCCEEDED(device->CreateBuffer(&cb, nullptr, &cbA)) && SUCCEEDED(device->CreateBuffer(&cb, nullptr, &cbB));
        D3D11_BUFFER_DESC ub {}; ub.ByteWidth = 16; ub.BindFlags = D3D11_BIND_UNORDERED_ACCESS; ub.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; ub.StructureByteStride = 4;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud {}; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Format = DXGI_FORMAT_UNKNOWN; ud.Buffer.NumElements = 4; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_COUNTER;
        ok = ok && SUCCEEDED(device->CreateBuffer(&ub, nullptr, &uavBuffer)) && SUCCEEDED(device->CreateUnorderedAccessView(uavBuffer.Get(), &ud, &uav));
        D3D11_BUFFER_DESC dst {}; dst.ByteWidth = 16;
        ok = ok && SUCCEEDED(device->CreateBuffer(&dst, nullptr, &counterDst));
        dst.Usage = D3D11_USAGE_STAGING; dst.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ok = ok && SUCCEEDED(device->CreateBuffer(&dst, nullptr, &counterStage));
        D3D11_SAMPLER_DESC sd {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        ok = ok && SUCCEEDED(device->CreateSamplerState(&sd, &samplerA));
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        ok = ok && SUCCEEDED(device->CreateSamplerState(&sd, &samplerB));
        D3D11_TEXTURE2D_DESC dd {}; dd.Width = dd.Height = 4; dd.MipLevels = dd.ArraySize = 1; dd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dd.SampleDesc.Count = 1; dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ok = ok && SUCCEEDED(device->CreateTexture2D(&dd, nullptr, &dummyTexture));
        for (auto& v : srv) ok = ok && SUCCEEDED(device->CreateShaderResourceView(dummyTexture.Get(), nullptr, &v));
        D3D11_RASTERIZER_DESC rd {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.ScissorEnable = TRUE;
        ok = ok && SUCCEEDED(device->CreateRasterizerState(&rd, &gameRs));
        rd.ScissorEnable = FALSE;
        ok = ok && SUCCEEDED(device->CreateRasterizerState(&rd, &ownRs));
        D3D11_BLEND_DESC bd {}; bd.RenderTarget[0].BlendEnable = TRUE; bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = ok && SUCCEEDED(device->CreateBlendState(&bd, &blend));
        D3D11_DEPTH_STENCIL_DESC dsd {}; dsd.DepthFunc = D3D11_COMPARISON_ALWAYS; dsd.StencilEnable = TRUE; dsd.StencilReadMask = dsd.StencilWriteMask = 0xff;
        dsd.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS }; dsd.BackFace = dsd.FrontFace;
        ok = ok && SUCCEEDED(device->CreateDepthStencilState(&dsd, &depth));
        const D3D11_QUERY_DESC pq { D3D11_QUERY_OCCLUSION_PREDICATE, 0 };
        ok = ok && SUCCEEDED(device->CreatePredicate(&pq, &predicate));
        return ok;
    }

    // The game's pipeline: everything a pass declares, all non-default.
    void BindGame()
    {
        ID3D11RenderTargetView* rtv = gameRtv.Get();
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        context->VSSetShader(gameVs.Get(), nullptr, 0);
        context->HSSetShader(gameHs.Get(), nullptr, 0);
        context->DSSetShader(gameDs.Get(), nullptr, 0);
        context->GSSetShader(gameGs.Get(), nullptr, 0);
        context->PSSetShader(gamePs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* views[8];
        for (int i = 0; i < 8; ++i) views[i] = srv[i].Get();
        context->PSSetShaderResources(0, 8, views);
        ID3D11SamplerState* s = samplerA.Get();
        context->PSSetSamplers(0, 1, &s);
        ID3D11Buffer* b0 = cbA.Get(); ID3D11Buffer* b1 = cbB.Get();
        c1->PSSetConstantBuffers1(0, 1, &b0, &first[0], &num[0]);
        c1->PSSetConstantBuffers1(1, 1, &b1, &first[1], &num[1]);
        context->RSSetState(gameRs.Get());
        context->RSSetViewports(2, viewports);
        context->RSSetScissorRects(2, rects);
        context->OMSetBlendState(blend.Get(), blendFactor, 0xfffffff7u);
        context->OMSetDepthStencilState(depth.Get(), 7);
        ID3D11UnorderedAccessView* u = uav.Get(); UINT initialCount = 5;
        c1->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, nullptr, 1, 1, &u, &initialCount);
        context->SetPredication(predicate.Get(), FALSE);
    }
    // What a pass does with a context it does not own: its own shaders, targets, state, no HS/DS/GS.
    void BindPrivate()
    {
        ID3D11RenderTargetView* rtv = ownRtv.Get();
        const D3D11_VIEWPORT vp { 0, 0, 16, 16, 0, 1 };
        const float zero[4] {};
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(ownVs.Get(), nullptr, 0);
        context->PSSetShader(ownPs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* none[8] {};
        context->PSSetShaderResources(0, 8, none);
        ID3D11SamplerState* s = samplerB.Get();
        context->PSSetSamplers(0, 1, &s);
        ID3D11Buffer* b = cbB.Get();
        context->PSSetConstantBuffers(0, 1, &b);
        context->PSSetConstantBuffers(1, 1, &b);
        context->RSSetState(ownRs.Get());
        context->RSSetViewports(1, &vp);
        context->OMSetBlendState(nullptr, zero, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);
        context->OMSetRenderTargets(1, &rtv, nullptr);
    }
    int64_t Counter()
    {
        c1->CopyStructureCount(counterDst.Get(), 0, uav.Get());
        context->CopyResource(counterStage.Get(), counterDst.Get());
        D3D11_MAPPED_SUBRESOURCE m {};
        if (FAILED(context->Map(counterStage.Get(), 0, D3D11_MAP_READ, 0, &m)))
            return -2;
        const uint32_t v = *static_cast<const uint32_t*>(m.pData);
        context->Unmap(counterStage.Get(), 0);
        return v;
    }
    Snap Take()
    {
        Snap s;
        ID3D11DeviceContext* c = context.Get();
        { ID3D11InputLayout* l = nullptr; c->IAGetInputLayout(&l); s["ia.layout"] = P(l); if (l) l->Release(); }
        { D3D11_PRIMITIVE_TOPOLOGY t {}; c->IAGetPrimitiveTopology(&t); s["ia.topology"] = t; }
        { ComPtr<ID3D11VertexShader> x; c->VSGetShader(&x, nullptr, nullptr); s["vs"] = P(x.Get()); }
        { ComPtr<ID3D11HullShader> x; c->HSGetShader(&x, nullptr, nullptr); s["hs"] = P(x.Get()); }
        { ComPtr<ID3D11DomainShader> x; c->DSGetShader(&x, nullptr, nullptr); s["ds"] = P(x.Get()); }
        { ComPtr<ID3D11GeometryShader> x; c->GSGetShader(&x, nullptr, nullptr); s["gs"] = P(x.Get()); }
        { ComPtr<ID3D11PixelShader> x; c->PSGetShader(&x, nullptr, nullptr); s["ps"] = P(x.Get()); }
        { ID3D11ShaderResourceView* v[8] {}; c->PSGetShaderResources(0, 8, v); for (int i = 0; i < 8; ++i) { s["srv" + std::to_string(i)] = P(v[i]); if (v[i]) v[i]->Release(); } }
        { ID3D11SamplerState* v[4] {}; c->PSGetSamplers(0, 4, v); for (int i = 0; i < 4; ++i) { s["smp" + std::to_string(i)] = P(v[i]); if (v[i]) v[i]->Release(); } }
        { ID3D11Buffer* b[2] {}; UINT f[2] {}, n[2] {}; c1->PSGetConstantBuffers1(0, 2, b, f, n);
          for (int i = 0; i < 2; ++i) { s["cb" + std::to_string(i)] = P(b[i]); s["cb" + std::to_string(i) + ".first"] = f[i]; s["cb" + std::to_string(i) + ".num"] = n[i]; if (b[i]) b[i]->Release(); } }
        { ID3D11RasterizerState* r = nullptr; c->RSGetState(&r); s["rs"] = P(r); if (r) r->Release(); }
        { D3D11_VIEWPORT v[16] {}; UINT n = 16; c->RSGetViewports(&n, v); s["vp.count"] = n; for (UINT i = 0; i < n; ++i) { const float* f = &v[i].TopLeftX; for (int k = 0; k < 6; ++k) s["vp" + std::to_string(i) + "." + std::to_string(k)] = Bits(f[k]); } }
        { D3D11_RECT v[16] {}; UINT n = 16; c->RSGetScissorRects(&n, v); s["sc.count"] = n; for (UINT i = 0; i < n; ++i) { s["sc" + std::to_string(i) + ".l"] = uint32_t(v[i].left); s["sc" + std::to_string(i) + ".r"] = uint32_t(v[i].right); s["sc" + std::to_string(i) + ".t"] = uint32_t(v[i].top); s["sc" + std::to_string(i) + ".b"] = uint32_t(v[i].bottom); } }
        { ID3D11BlendState* b = nullptr; FLOAT f[4] {}; UINT m = 0; c->OMGetBlendState(&b, f, &m); s["blend"] = P(b); s["mask"] = m; for (int i = 0; i < 4; ++i) s["bf" + std::to_string(i)] = Bits(f[i]); if (b) b->Release(); }
        { ID3D11DepthStencilState* d = nullptr; UINT r = 0; c->OMGetDepthStencilState(&d, &r); s["depth"] = P(d); s["stencilRef"] = r; if (d) d->Release(); }
        { ID3D11RenderTargetView* r[8] {}; ID3D11DepthStencilView* d = nullptr; ID3D11UnorderedAccessView* u[64] {}; const UINT nu = fl111 ? 64 : 8;
          c1->OMGetRenderTargetsAndUnorderedAccessViews(8, r, &d, 0, nu, u);
          for (int i = 0; i < 8; ++i) { s["rtv" + std::to_string(i)] = P(r[i]); if (r[i]) r[i]->Release(); }
          s["dsv"] = P(d); if (d) d->Release();
          for (UINT i = 0; i < nu; ++i) { if (u[i]) { s["uav" + std::to_string(i)] = P(u[i]); u[i]->Release(); } } }
        { ID3D11Predicate* p = nullptr; BOOL v = FALSE; c->GetPredication(&p, &v); s["pred"] = P(p); s["pred.value"] = v; if (p) p->Release(); }
        return s;
    }
};

std::string Diff(const Snap& a, const Snap& b)
{
    std::string out;
    for (auto& kv : a) { auto it = b.find(kv.first); if (it == b.end() || it->second != kv.second) out += kv.first + ";"; }
    for (auto& kv : b) if (!a.count(kv.first)) out += "+" + kv.first + ";";
    return out;
}
Spec Everything()
{
    Spec s;
    s.psSrvCount = 8; s.psSamplerCount = 4; s.psConstantBuffers = 2; s.scissors = true;
    return s;
}

// ---- FullStateSnapshot: the hostile world -------------------------------------------------------------------------------------------------------
// A game state that touches every category the snapshot covers, and a stand-in (what an unknown library call might do) that trashes every one of them.
#define FS_STAGES(X) X(VS) X(HS) X(DS) X(GS) X(PS) X(CS)

struct Rich
{
    World& w;
    explicit Rich(World& world) : w(world) {}
    ComPtr<ID3D11Texture2D> depthTexture, aliasTexture;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11RenderTargetView> aliasRtv;
    ComPtr<ID3D11ShaderResourceView> aliasTexSrv, aliasBufSrv;
    ComPtr<ID3D11ComputeShader> gameCs, ownCs;
    ComPtr<ID3D11ClassLinkage> linkage;
    ComPtr<ID3D11ClassInstance> instance;
    ComPtr<ID3D11PixelShader> psInterface;
    ComPtr<ID3D11Buffer> vbA, vbB, ownVb, ib, ownIb, soBuffer, aliasVb, csBuffer, ownCsBuffer, uaBuffer, csCounterDst, csCounterStage;
    ComPtr<ID3D11UnorderedAccessView> csUav, ownCsUav, aliasUav;
    ComPtr<ID3D11Predicate> ownPredicate;
    ComPtr<ID3D11RasterizerState> ownRs2;
    bool instancesWork = false;
    D3D11_VIEWPORT viewports[3] { { 0, 0, 64, 64, 0, 1 }, { 4, 4, 32, 32, 0.25f, 0.75f }, { 8, 8, 16, 16, 0.5f, 1.0f } };
    D3D11_RECT rects[3] { { 0, 0, 64, 64 }, { 2, 2, 30, 30 }, { 5, 6, 7, 8 } };

    bool Create()
    {
        ID3D11Device* d = w.device.Get();
        const char* hlsl = R"hlsl(
RWStructuredBuffer<uint> G : register(u2);
RWStructuredBuffer<uint> O : register(u5);
[numthreads(1,1,1)] void CSGame() { G.IncrementCounter(); }
[numthreads(1,1,1)] void CSOwn() { O.IncrementCounter(); }
interface IColor { float4 Get(); };
class RedColor : IColor { float4 Get() { return float4(1, 0, 0, 1); } };
class BlueColor : IColor { float4 Get() { return float4(0, 0, 1, 1); } };
IColor g_color;
float4 PSI() : SV_Target { return g_color.Get(); }
)hlsl";
        auto compile = [&](const char* entry, const char* profile, ComPtr<ID3DBlob>& blob) {
            ComPtr<ID3DBlob> err;
            return SUCCEEDED(D3DCompile(hlsl, std::strlen(hlsl), "full-state", nullptr, nullptr, entry, profile, 0, 0, &blob, &err));
        };
        ComPtr<ID3DBlob> cs1, cs2, psi;
        if (!compile("CSGame", "cs_5_0", cs1) || !compile("CSOwn", "cs_5_0", cs2) || !compile("PSI", "ps_5_0", psi))
            return false;
        bool ok = SUCCEEDED(d->CreateComputeShader(cs1->GetBufferPointer(), cs1->GetBufferSize(), nullptr, &gameCs)) &&
                  SUCCEEDED(d->CreateComputeShader(cs2->GetBufferPointer(), cs2->GetBufferSize(), nullptr, &ownCs));
        // a pixel shader with an interface slot and a class instance for it (the "with class instances" part of the contract)
        instancesWork = SUCCEEDED(d->CreateClassLinkage(&linkage)) && SUCCEEDED(d->CreatePixelShader(psi->GetBufferPointer(), psi->GetBufferSize(), linkage.Get(), &psInterface)) &&
                        SUCCEEDED(linkage->CreateClassInstance("RedColor", 0, 0, 0, 0, &instance));

        D3D11_TEXTURE2D_DESC dd {};
        dd.Width = dd.Height = 64; dd.MipLevels = dd.ArraySize = 1; dd.Format = DXGI_FORMAT_D32_FLOAT; dd.SampleDesc.Count = 1; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ok = ok && SUCCEEDED(d->CreateTexture2D(&dd, nullptr, &depthTexture)) && SUCCEEDED(d->CreateDepthStencilView(depthTexture.Get(), nullptr, &dsv));
        D3D11_TEXTURE2D_DESC td {};
        td.Width = td.Height = 16; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ok = ok && SUCCEEDED(d->CreateTexture2D(&td, nullptr, &aliasTexture)) && SUCCEEDED(d->CreateRenderTargetView(aliasTexture.Get(), nullptr, &aliasRtv)) &&
             SUCCEEDED(d->CreateShaderResourceView(aliasTexture.Get(), nullptr, &aliasTexSrv));

        D3D11_BUFFER_DESC vb {}; vb.ByteWidth = 256; vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        ok = ok && SUCCEEDED(d->CreateBuffer(&vb, nullptr, &vbA)) && SUCCEEDED(d->CreateBuffer(&vb, nullptr, &vbB)) && SUCCEEDED(d->CreateBuffer(&vb, nullptr, &ownVb));
        vb.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_STREAM_OUTPUT;
        ok = ok && SUCCEEDED(d->CreateBuffer(&vb, nullptr, &aliasVb));
        vb.BindFlags = D3D11_BIND_STREAM_OUTPUT;
        ok = ok && SUCCEEDED(d->CreateBuffer(&vb, nullptr, &soBuffer));
        D3D11_BUFFER_DESC ibd {}; ibd.ByteWidth = 256; ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        ok = ok && SUCCEEDED(d->CreateBuffer(&ibd, nullptr, &ib)) && SUCCEEDED(d->CreateBuffer(&ibd, nullptr, &ownIb));

        auto makeUav = [&](ComPtr<ID3D11Buffer>& buffer, ComPtr<ID3D11UnorderedAccessView>& view, UINT bind, UINT flags) {
            D3D11_BUFFER_DESC ub {}; ub.ByteWidth = 16; ub.BindFlags = bind; ub.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; ub.StructureByteStride = 4;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud {}; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Format = DXGI_FORMAT_UNKNOWN; ud.Buffer.NumElements = 4; ud.Buffer.Flags = flags;
            return SUCCEEDED(d->CreateBuffer(&ub, nullptr, &buffer)) && SUCCEEDED(d->CreateUnorderedAccessView(buffer.Get(), &ud, &view));
        };
        ok = ok && makeUav(csBuffer, csUav, D3D11_BIND_UNORDERED_ACCESS, D3D11_BUFFER_UAV_FLAG_COUNTER) && makeUav(ownCsBuffer, ownCsUav, D3D11_BIND_UNORDERED_ACCESS, D3D11_BUFFER_UAV_FLAG_COUNTER) &&
             makeUav(uaBuffer, aliasUav, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv {}; sv.Format = DXGI_FORMAT_UNKNOWN; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sv.Buffer.NumElements = 4;
        ok = ok && SUCCEEDED(d->CreateShaderResourceView(uaBuffer.Get(), &sv, &aliasBufSrv));
        D3D11_BUFFER_DESC dst {}; dst.ByteWidth = 16;
        ok = ok && SUCCEEDED(d->CreateBuffer(&dst, nullptr, &csCounterDst));
        dst.Usage = D3D11_USAGE_STAGING; dst.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ok = ok && SUCCEEDED(d->CreateBuffer(&dst, nullptr, &csCounterStage));
        const D3D11_QUERY_DESC pq { D3D11_QUERY_OCCLUSION_PREDICATE, 0 };
        ok = ok && SUCCEEDED(d->CreatePredicate(&pq, &ownPredicate));
        return ok;
    }

    // What the game has bound, in every category (alias = also bind the resources that the aliasing cases later bind as outputs).
    void BindGame(bool alias)
    {
        ID3D11DeviceContext* c = w.context.Get();
        ID3D11Buffer* nb[32] {}; UINT z[32] {};
        ID3D11Buffer* vbs[32] {}; UINT strides[32] {}, offsets[32] {};
        vbs[0] = vbA.Get(); strides[0] = 12; offsets[0] = 4;
        vbs[3] = vbB.Get(); strides[3] = 24; offsets[3] = 8;
        if (alias) { vbs[1] = aliasVb.Get(); strides[1] = 16; offsets[1] = 32; }
        c->IASetInputLayout(w.layout.Get());
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        c->IASetVertexBuffers(0, 32, vbs, strides, offsets);
        c->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 12);
        (void)nb; (void)z;
        c->VSSetShader(w.gameVs.Get(), nullptr, 0);
        c->HSSetShader(w.gameHs.Get(), nullptr, 0);
        c->DSSetShader(w.gameDs.Get(), nullptr, 0);
        c->GSSetShader(w.gameGs.Get(), nullptr, 0);
        if (instancesWork)
        {
            ID3D11ClassInstance* inst = instance.Get();
            c->PSSetShader(psInterface.Get(), &inst, 1);
        }
        else
            c->PSSetShader(w.gamePs.Get(), nullptr, 0);
        c->CSSetShader(gameCs.Get(), nullptr, 0);
#define FS_BIND_GAME(S)                                                                                                                              \
    {                                                                                                                                                \
        ID3D11ShaderResourceView* v[128] {};                                                                                                         \
        v[0] = w.srv[0].Get(); v[5] = w.srv[1].Get(); v[127] = w.srv[2].Get();                                                                        \
        if (alias) { v[3] = aliasTexSrv.Get(); v[4] = aliasBufSrv.Get(); }                                                                           \
        c->S##SetShaderResources(0, 128, v);                                                                                                         \
        ID3D11SamplerState* sm[16] {}; sm[0] = w.samplerA.Get(); sm[15] = w.samplerB.Get();                                                          \
        c->S##SetSamplers(0, 16, sm);                                                                                                                \
        ID3D11Buffer* b0 = w.cbA.Get(); c->S##SetConstantBuffers(0, 1, &b0);                                                                         \
        ID3D11Buffer* b5 = w.cbB.Get(); UINT f5 = 32, n5 = 16; w.c1->S##SetConstantBuffers1(5, 1, &b5, &f5, &n5);                                  \
        ID3D11Buffer* b13 = w.cbA.Get(); UINT f13 = 16, n13 = 16; w.c1->S##SetConstantBuffers1(13, 1, &b13, &f13, &n13);                          \
    }
        FS_STAGES(FS_BIND_GAME)
#undef FS_BIND_GAME
        ID3D11Buffer* so = soBuffer.Get(); UINT soOffset = 16;
        c->SOSetTargets(1, &so, &soOffset);
        c->RSSetState(w.gameRs.Get());
        c->RSSetViewports(3, viewports);
        c->RSSetScissorRects(3, rects);
        c->OMSetBlendState(w.blend.Get(), w.blendFactor, 0xfffffff7u);
        c->OMSetDepthStencilState(w.depth.Get(), 7);
        ID3D11RenderTargetView* rtv = w.gameRtv.Get();
        ID3D11UnorderedAccessView* u = w.uav.Get(); UINT initialCount = 5;
        w.c1->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, dsv.Get(), 1, 1, &u, &initialCount);
        ID3D11UnorderedAccessView* cu = csUav.Get(); UINT csInitial = 7;
        c->CSSetUnorderedAccessViews(2, 1, &cu, &csInitial);
        c->Dispatch(1, 1, 1); // the counter of the CS UAV moves on, so a restore that resets it is visible on every adapter
        c->SetPredication(w.predicate.Get(), FALSE);
    }

    // The stand-in: every category set to something else, a dispatch on top. `aliasing` also binds the game's SRV resources as outputs.
    void BindHostile(bool aliasing)
    {
        ID3D11DeviceContext* c = w.context.Get();
        ID3D11Buffer* vbs[32] {}; UINT strides[32] {}, offsets[32] {};
        vbs[2] = ownVb.Get(); strides[2] = 20; offsets[2] = 40;
        c->IASetInputLayout(nullptr);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        c->IASetVertexBuffers(0, 32, vbs, strides, offsets);
        c->IASetIndexBuffer(ownIb.Get(), DXGI_FORMAT_R32_UINT, 8);
        c->VSSetShader(w.ownVs.Get(), nullptr, 0);
        c->HSSetShader(nullptr, nullptr, 0);
        c->DSSetShader(nullptr, nullptr, 0);
        c->GSSetShader(nullptr, nullptr, 0);
        c->PSSetShader(w.ownPs.Get(), nullptr, 0);
        c->CSSetShader(ownCs.Get(), nullptr, 0);
#define FS_BIND_HOSTILE(S)                                                                                                                           \
    {                                                                                                                                                \
        ID3D11ShaderResourceView* v[128] {};                                                                                                         \
        v[1] = w.srv[7].Get(); v[100] = w.srv[6].Get();                                                                                               \
        c->S##SetShaderResources(0, 128, v);                                                                                                         \
        ID3D11SamplerState* sm[16] {}; sm[1] = w.samplerB.Get();                                                                                     \
        c->S##SetSamplers(0, 16, sm);                                                                                                                \
        ID3D11Buffer* none[14] {};                                                                                                                   \
        c->S##SetConstantBuffers(0, 14, none);                                                                                                       \
        ID3D11Buffer* b0 = w.cbB.Get(); UINT f0 = 48, n0 = 16; w.c1->S##SetConstantBuffers1(0, 1, &b0, &f0, &n0);                                  \
        ID3D11Buffer* b7 = w.cbA.Get(); c->S##SetConstantBuffers(7, 1, &b7);                                                                         \
    }
        FS_STAGES(FS_BIND_HOSTILE)
#undef FS_BIND_HOSTILE
        ID3D11Buffer* noSo[4] {};
        c->SOSetTargets(4, noSo, nullptr);
        c->RSSetState(w.ownRs.Get());
        const D3D11_VIEWPORT vp { 0, 0, 16, 16, 0, 1 };
        c->RSSetViewports(1, &vp);
        const D3D11_RECT rc { 1, 1, 2, 2 };
        c->RSSetScissorRects(1, &rc);
        const float zero[4] {};
        c->OMSetBlendState(nullptr, zero, 0xffffffff);
        c->OMSetDepthStencilState(nullptr, 0);
        { // every CS UAV slot, then one of its own
            ID3D11UnorderedAccessView* none[kMaxUavSlots] {}; UINT keepAll[kMaxUavSlots];
            for (UINT& k : keepAll) k = static_cast<UINT>(-1);
            c->CSSetUnorderedAccessViews(0, w.fl111 ? kMaxUavSlots : 8u, none, keepAll);
        }
        if (aliasing)
        {
            ID3D11RenderTargetView* rtv = aliasRtv.Get(); // the game's PS and CS SRV 3 is this texture: it is an output now
            ID3D11UnorderedAccessView* cu[1] { aliasUav.Get() }; UINT keep = static_cast<UINT>(-1); // the game's CS SRV 4 is this buffer: it is a UAV now
            c->CSSetUnorderedAccessViews(5, 1, cu, &keep);
            c->OMSetRenderTargets(1, &rtv, nullptr);
            ID3D11Buffer* so = aliasVb.Get(); UINT soOffset = 0; // the game's vertex buffer 1 is this buffer: it is a stream-out target now
            c->SOSetTargets(1, &so, &soOffset);
        }
        else
        {
            ID3D11RenderTargetView* rtv = w.ownRtv.Get();
            c->OMSetRenderTargets(1, &rtv, nullptr);
            ID3D11UnorderedAccessView* cu = ownCsUav.Get(); UINT zeroCount = 0;
            c->CSSetUnorderedAccessViews(5, 1, &cu, &zeroCount);
            c->Dispatch(1, 1, 1);
        }
        c->SetPredication(ownPredicate.Get(), TRUE);
    }

    int64_t CsCounter()
    {
        w.c1->CopyStructureCount(csCounterDst.Get(), 0, csUav.Get());
        w.context->CopyResource(csCounterStage.Get(), csCounterDst.Get());
        D3D11_MAPPED_SUBRESOURCE m {};
        if (FAILED(w.context->Map(csCounterStage.Get(), 0, D3D11_MAP_READ, 0, &m)))
            return -2;
        const uint32_t v = *static_cast<const uint32_t*>(m.pData);
        w.context->Unmap(csCounterStage.Get(), 0);
        return v;
    }

    // Everything the snapshot covers, read back through the public getters (the fixtures' TakeSnapshot, in this test's own words).
    Snap TakeFull()
    {
        Snap s;
        ID3D11DeviceContext* c = w.context.Get();
        ID3D11DeviceContext1* c1 = w.c1.Get();
        { ID3D11InputLayout* l = nullptr; c->IAGetInputLayout(&l); s["IA.layout"] = P(l); if (l) l->Release(); }
        { D3D11_PRIMITIVE_TOPOLOGY t {}; c->IAGetPrimitiveTopology(&t); s["IA.topology"] = t; }
        { ID3D11Buffer* b[32] {}; UINT st[32] {}, of[32] {}; c->IAGetVertexBuffers(0, 32, b, st, of);
          for (UINT i = 0; i < 32; ++i) { s["IA.vb" + std::to_string(i)] = P(b[i]); s["IA.vb" + std::to_string(i) + ".stride"] = st[i]; s["IA.vb" + std::to_string(i) + ".offset"] = of[i]; if (b[i]) b[i]->Release(); } }
        { ID3D11Buffer* b = nullptr; DXGI_FORMAT f {}; UINT o = 0; c->IAGetIndexBuffer(&b, &f, &o); s["IA.ib"] = P(b); s["IA.ib.format"] = f; s["IA.ib.offset"] = o; if (b) b->Release(); }
#define FS_TAKE(S)                                                                                                                                   \
    {                                                                                                                                                \
        const std::string st = #S;                                                                                                                   \
        { ID3D11DeviceChild* sh = nullptr; ID3D11ClassInstance* ci[D3D11_SHADER_MAX_INTERFACES] {}; UINT cn = D3D11_SHADER_MAX_INTERFACES;             \
          GetShaderOf##S(c, &sh, ci, &cn); s[st + ".shader"] = P(sh); s[st + ".ninst"] = cn; if (sh) sh->Release();                                    \
          for (UINT i = 0; i < cn; ++i) { s[st + ".inst" + std::to_string(i)] = P(ci[i]); if (ci[i]) ci[i]->Release(); } }                          \
        { ID3D11Buffer* cb[14] {}; UINT fi[14] {}, nu[14] {}; c1->S##GetConstantBuffers1(0, 14, cb, fi, nu);                                        \
          for (UINT i = 0; i < 14; ++i) { s[st + ".cb" + std::to_string(i)] = P(cb[i]); s[st + ".cb" + std::to_string(i) + ".first"] = fi[i];        \
              s[st + ".cb" + std::to_string(i) + ".num"] = nu[i]; if (cb[i]) cb[i]->Release(); } }                                                  \
        { ID3D11ShaderResourceView* v[128] {}; c->S##GetShaderResources(0, 128, v);                                                                 \
          for (UINT i = 0; i < 128; ++i) { s[st + ".srv" + std::to_string(i)] = P(v[i]); if (v[i]) v[i]->Release(); } }                              \
        { ID3D11SamplerState* sm[16] {}; c->S##GetSamplers(0, 16, sm);                                                                              \
          for (UINT i = 0; i < 16; ++i) { s[st + ".smp" + std::to_string(i)] = P(sm[i]); if (sm[i]) sm[i]->Release(); } }                           \
    }
        FS_STAGES(FS_TAKE)
#undef FS_TAKE
        { ID3D11Buffer* so[4] {}; c->SOGetTargets(4, so); for (UINT i = 0; i < 4; ++i) { s["SO.target" + std::to_string(i)] = P(so[i]); if (so[i]) so[i]->Release(); } }
        { ID3D11RasterizerState* r = nullptr; c->RSGetState(&r); s["RS.state"] = P(r); if (r) r->Release(); }
        { D3D11_VIEWPORT v[16] {}; UINT n = 16; c->RSGetViewports(&n, v); s["RS.viewports"] = n; for (UINT i = 0; i < n; ++i) { const float* f = &v[i].TopLeftX; for (int k = 0; k < 6; ++k) s["RS.vp" + std::to_string(i) + "." + std::to_string(k)] = Bits(f[k]); } }
        { D3D11_RECT v[16] {}; UINT n = 16; c->RSGetScissorRects(&n, v); s["RS.scissors"] = n; for (UINT i = 0; i < n; ++i) { s["RS.sc" + std::to_string(i) + ".l"] = uint32_t(v[i].left); s["RS.sc" + std::to_string(i) + ".r"] = uint32_t(v[i].right); s["RS.sc" + std::to_string(i) + ".t"] = uint32_t(v[i].top); s["RS.sc" + std::to_string(i) + ".b"] = uint32_t(v[i].bottom); } }
        { ID3D11BlendState* b = nullptr; FLOAT f[4] {}; UINT m = 0; c->OMGetBlendState(&b, f, &m); s["OM.blend"] = P(b); s["OM.sampleMask"] = m; for (int i = 0; i < 4; ++i) s["OM.blendFactor" + std::to_string(i)] = Bits(f[i]); if (b) b->Release(); }
        { ID3D11DepthStencilState* d = nullptr; UINT r = 0; c->OMGetDepthStencilState(&d, &r); s["OM.depthStencil"] = P(d); s["OM.stencilRef"] = r; if (d) d->Release(); }
        { ID3D11RenderTargetView* r[8] {}; ID3D11DepthStencilView* d = nullptr; ID3D11UnorderedAccessView* u[64] {}; const UINT nu = w.fl111 ? 64 : 8;
          c1->OMGetRenderTargetsAndUnorderedAccessViews(8, r, &d, 0, nu, u);
          for (int i = 0; i < 8; ++i) { s["OM.rtv" + std::to_string(i)] = P(r[i]); if (r[i]) r[i]->Release(); }
          s["OM.dsv"] = P(d); if (d) d->Release();
          for (UINT i = 0; i < nu; ++i) { s["OM.uav" + std::to_string(i)] = P(u[i]); if (u[i]) u[i]->Release(); } }
        { ID3D11UnorderedAccessView* u[64] {}; c->CSGetUnorderedAccessViews(0, w.fl111 ? 64 : 8, u); for (UINT i = 0; i < 64; ++i) { s["CS.uav" + std::to_string(i)] = P(u[i]); if (u[i]) u[i]->Release(); } }
        { ID3D11Predicate* p = nullptr; BOOL v = FALSE; c->GetPredication(&p, &v); s["pred.object"] = P(p); s["pred.value"] = v; if (p) p->Release(); }
        return s;
    }

    static void GetShaderOfVS(ID3D11DeviceContext* c, ID3D11DeviceChild** o, ID3D11ClassInstance** i, UINT* n) { ID3D11VertexShader* s = nullptr; c->VSGetShader(&s, i, n); *o = s; }
    static void GetShaderOfHS(ID3D11DeviceContext* c, ID3D11DeviceChild** o, ID3D11ClassInstance** i, UINT* n) { ID3D11HullShader* s = nullptr; c->HSGetShader(&s, i, n); *o = s; }
    static void GetShaderOfDS(ID3D11DeviceContext* c, ID3D11DeviceChild** o, ID3D11ClassInstance** i, UINT* n) { ID3D11DomainShader* s = nullptr; c->DSGetShader(&s, i, n); *o = s; }
    static void GetShaderOfGS(ID3D11DeviceContext* c, ID3D11DeviceChild** o, ID3D11ClassInstance** i, UINT* n) { ID3D11GeometryShader* s = nullptr; c->GSGetShader(&s, i, n); *o = s; }
    static void GetShaderOfPS(ID3D11DeviceContext* c, ID3D11DeviceChild** o, ID3D11ClassInstance** i, UINT* n) { ID3D11PixelShader* s = nullptr; c->PSGetShader(&s, i, n); *o = s; }
    static void GetShaderOfCS(ID3D11DeviceContext* c, ID3D11DeviceChild** o, ID3D11ClassInstance** i, UINT* n) { ID3D11ComputeShader* s = nullptr; c->CSGetShader(&s, i, n); *o = s; }
};
} // namespace

int main()
{
    World w;
    if (!w.Create())
    {
        std::printf("explicit_state_warp: no D3D11 WARP device (or a resource failed to build)\n");
        return 1;
    }
    ID3D11DeviceContext* c = w.context.Get();

    // ---- everything declared: neutralise, a pass's own state on top, restore: the full getter snapshot is identical, the UAV counter included
    {
        w.BindGame();
        const Snap before = w.Take();
        const int64_t counterBefore = w.Counter();
        std::printf("UAV counter after BindGame = %lld\n", static_cast<long long>(counterBefore));
        CHECK(counterBefore >= 0); // the value itself is the adapter's (the initial count of 5 is applied when the UAV is first used on some adapters); it has to survive
        CHECK(before.at("hs") != 0 && before.at("ds") != 0 && before.at("gs") != 0 && before.at("pred") != 0 && before.count("uav1") == 1);
        {
            Guard guard(c, w.c1.Get(), w.fl111, Everything());
            guard.Neutralise();
            const Snap neutral = w.Take();
            CHECK(neutral.at("hs") == 0 && neutral.at("ds") == 0 && neutral.at("gs") == 0 && neutral.at("pred") == 0);
            w.BindPrivate();
            CHECK(w.Take().at("vs") == P(w.ownVs.Get()));
            guard.Restore();
            CHECK(guard.Restored());
        }
        const std::string diff = Diff(before, w.Take());
        if (!diff.empty()) std::printf("difference after restore: %s\n", diff.c_str());
        CHECK(diff.empty());
        CHECK(w.Counter() == counterBefore);
    }

    // ---- the destructor restores on every exit, also while an exception unwinds
    {
        w.BindGame();
        const Snap before = w.Take();
        try
        {
            Guard guard(c, w.c1.Get(), w.fl111, Everything());
            guard.Neutralise();
            w.BindPrivate();
            throw 7;
        }
        catch (int) {}
        CHECK(Diff(before, w.Take()).empty());
    }

    // ---- Restore() runs once: a later change is not undone by the destructor
    {
        w.BindGame();
        {
            Guard guard(c, w.c1.Get(), w.fl111, Everything());
            guard.Neutralise();
            w.BindPrivate();
            guard.Restore();
            c->VSSetShader(w.ownVs.Get(), nullptr, 0);
            guard.Restore();
        }
        CHECK(w.Take().at("vs") == P(w.ownVs.Get()));
    }

    // ---- only the declared categories are touched: a sampler the pass did not declare stays where the pass left it, a declared one comes back;
    //      HS, DS and GS stay bound when the pass does not ask for them to be neutralised
    {
        w.BindGame();
        Spec narrow = Everything();
        narrow.psSamplerCount = 0; narrow.hullDomainGeometry = false; narrow.predication = false; narrow.streamOut = false;
        {
            Guard guard(c, w.c1.Get(), w.fl111, narrow);
            guard.Neutralise();
            const Snap neutral = w.Take();
            CHECK(neutral.at("hs") != 0 && neutral.at("ds") != 0 && neutral.at("gs") != 0 && neutral.at("pred") != 0);
            w.BindPrivate(); // sets sampler 0 to B, which was not declared
        }
        const Snap after = w.Take();
        CHECK(after.at("smp0") == P(w.samplerB.Get()));
        CHECK(after.at("vs") == P(w.gameVs.Get()));
        w.BindGame();
        {
            Guard guard(c, w.c1.Get(), w.fl111, Everything());
            w.BindPrivate();
        }
        CHECK(w.Take().at("smp0") == P(w.samplerA.Get()));
    }

    // ---- OmSnapshot on its own: OMSetRenderTargets(0) drops the UAV, Restore() puts the render target, the UAV and its counter back
    {
        w.BindGame();
        const int64_t counterAtBind = w.Counter();
        const Snap before = w.Take();
        {
            OmSnapshot om(w.c1.Get(), w.fl111);
            c->OMSetRenderTargets(0, nullptr, nullptr);
            const Snap dropped = w.Take();
            CHECK(dropped.count("uav1") == 0 && dropped.at("rtv0") == 0);
            om.Restore();
        }
        CHECK(Diff(before, w.Take()).empty());
        CHECK(w.Counter() == counterAtBind);
    }

    // ---- a game state that has none of HS, DS, GS, stream-out, a predicate or a UAV: nothing is invented
    {
        c->ClearState();
        ID3D11RenderTargetView* rtv = w.gameRtv.Get();
        c->OMSetRenderTargets(1, &rtv, nullptr);
        c->VSSetShader(w.gameVs.Get(), nullptr, 0);
        c->PSSetShader(w.gamePs.Get(), nullptr, 0);
        const Snap before = w.Take();
        CHECK(before.at("hs") == 0 && before.at("pred") == 0 && before.count("uav1") == 0);
        {
            Guard guard(c, w.c1.Get(), w.fl111, Everything());
            guard.Neutralise();
            w.BindPrivate();
        }
        CHECK(Diff(before, w.Take()).empty());
    }

    // ================================================================================================================================================
    // FullStateSnapshot (the character-preview NGX calls): a game state in every category, a stand-in that trashes every one of them, and the getter
    // snapshot compared before and after. The stand-in proves the snapshot, not NGX: the real NGX footprint is only known in the game.
    // ================================================================================================================================================
    auto fullStateCases = [](World& wd, const char* tag) {
        Rich r(wd);
        if (!r.Create())
        {
            std::printf("FullStateSnapshot[%s]: a resource failed to build\n", tag);
            ++g_failures;
            return;
        }
        ID3D11DeviceContext* ctx = wd.context.Get();
        std::printf("FullStateSnapshot[%s]: feature level 11_%d, class instances %s\n", tag, wd.fl111 ? 1 : 0, r.instancesWork ? "available" : "NOT available (instance checks skipped)");

        // 1. exactness over every category, counters kept
        {
            ctx->ClearState();
            r.BindGame(false);
            const Snap before = r.TakeFull();
            const int64_t psCounter = wd.Counter();
            const int64_t csCounter = r.CsCounter();
            CHECK(before.at("HS.shader") != 0 && before.at("DS.shader") != 0 && before.at("GS.shader") != 0 && before.at("CS.shader") != 0 && before.at("PS.shader") != 0);
            CHECK(before.at("SO.target0") != 0 && before.at("pred.object") != 0 && before.at("OM.dsv") != 0 && before.at("IA.vb3") != 0 && before.at("IA.ib") != 0);
            CHECK(before.at("RS.viewports") == 3 && before.at("RS.scissors") == 3 && before.at("OM.stencilRef") == 7 && before.at("OM.sampleMask") == 0xfffffff7u);
            CHECK(before.at("PS.cb5.first") == 32 && before.at("CS.cb13.first") == 16 && before.at("PS.srv127") != 0 && before.at("GS.smp15") != 0 && before.at("CS.uav2") != 0 && before.at("OM.uav1") != 0);
            if (r.instancesWork)
                CHECK(before.at("PS.ninst") >= 1 && before.at("PS.inst0") == P(r.instance.Get()));
            {
                FullStateSnapshot snapshot(ctx, wd.c1.Get(), wd.fl111);
                r.BindHostile(false);
                const Snap hostile = r.TakeFull();
                CHECK(hostile.at("VS.shader") == P(wd.ownVs.Get()) && hostile.at("HS.shader") == 0 && hostile.at("GS.shader") == 0 && hostile.at("CS.shader") == P(r.ownCs.Get()));
                CHECK(hostile.at("PS.srv0") == 0 && hostile.at("PS.srv1") != 0 && hostile.at("CS.srv127") == 0 && hostile.at("SO.target0") == 0 && hostile.at("pred.object") == P(r.ownPredicate.Get()));
                CHECK(hostile.at("RS.viewports") == 1 && hostile.at("OM.dsv") == 0 && hostile.at("CS.uav2") == 0 && hostile.at("CS.uav5") != 0 && hostile.at("OM.uav1") == 0);
                snapshot.Restore();
                CHECK(snapshot.Restored());
            }
            const std::string diff = Diff(before, r.TakeFull());
            if (!diff.empty())
                std::printf("FullStateSnapshot[%s]: difference after restore: %s\n", tag, diff.c_str());
            CHECK(diff.empty());
            CHECK(wd.Counter() == psCounter);
            CHECK(r.CsCounter() == csCounter);
            std::printf("FullStateSnapshot[%s]: PS UAV counter %lld, CS UAV counter %lld, kept\n", tag, static_cast<long long>(psCounter), static_cast<long long>(csCounter));
            // WARP reports 0 for CopyStructureCount whatever was bound (the PS UAV counter above too), so a restore that resets a counter is only visible on a
            // hardware adapter: the chara fixtures read the real counter (5 and 7) there.
        }

        // 2. the destructor restores on every exit, also while an exception unwinds
        {
            ctx->ClearState();
            r.BindGame(false);
            const Snap before = r.TakeFull();
            try
            {
                FullStateSnapshot snapshot(ctx, wd.c1.Get(), wd.fl111);
                r.BindHostile(false);
                throw 7;
            }
            catch (int) {}
            CHECK(Diff(before, r.TakeFull()).empty());
        }

        // 3. Restore() runs once: a later change is not undone by the destructor
        {
            ctx->ClearState();
            r.BindGame(false);
            {
                FullStateSnapshot snapshot(ctx, wd.c1.Get(), wd.fl111);
                r.BindHostile(false);
                snapshot.Restore();
                ctx->VSSetShader(wd.ownVs.Get(), nullptr, 0);
                snapshot.Restore();
            }
            CHECK(r.TakeFull().at("VS.shader") == P(wd.ownVs.Get()));
        }

        // 4. aliasing: the stand-in binds resources that are SRVs of the game's state as outputs (a render target, a CS UAV, a stream-out target). The runtime
        //    unbinds the game's SRVs when it does, and drops an SRV bind of a resource that is still an output: the restore has to put the outputs back first.
        {
            ctx->ClearState();
            r.BindGame(true);
            const Snap before = r.TakeFull();
            CHECK(before.at("PS.srv3") == P(r.aliasTexSrv.Get()) && before.at("CS.srv3") == P(r.aliasTexSrv.Get()) && before.at("CS.srv4") == P(r.aliasBufSrv.Get()) && before.at("IA.vb1") == P(r.aliasVb.Get()));
            {
                FullStateSnapshot snapshot(ctx, wd.c1.Get(), wd.fl111);
                r.BindHostile(true);
                const Snap aliased = r.TakeFull();
                // the hazard is real on this adapter: the game's SRVs are gone while their resources are outputs (this is what makes the check below mean something)
                const bool hazardSeen = aliased.at("PS.srv3") == 0 && aliased.at("CS.srv3") == 0 && aliased.at("CS.srv4") == 0 && aliased.at("IA.vb1") == 0;
                CHECK(hazardSeen);
                // and a restore in the wrong order loses them: SRVs first, outputs after
                ID3D11ShaderResourceView* wrongOrder = r.aliasTexSrv.Get();
                ctx->PSSetShaderResources(3, 1, &wrongOrder);
                ID3D11ShaderResourceView* got = nullptr;
                ctx->PSGetShaderResources(3, 1, &got);
                CHECK(got == nullptr); // the bind was silently dropped
                if (got) got->Release();
                snapshot.Restore();
            }
            const std::string diff = Diff(before, r.TakeFull());
            if (!diff.empty())
                std::printf("FullStateSnapshot[%s]: aliasing, difference after restore: %s\n", tag, diff.c_str());
            CHECK(diff.empty());
        }

        // 5. a game state that has nothing bound: nothing is invented, whatever the stand-in leaves behind is removed
        {
            ctx->ClearState();
            const Snap before = r.TakeFull();
            CHECK(before.at("RS.viewports") == 0 && before.at("HS.shader") == 0 && before.at("pred.object") == 0 && before.at("OM.uav1") == 0 && before.at("CS.uav5") == 0);
            {
                FullStateSnapshot snapshot(ctx, wd.c1.Get(), wd.fl111);
                r.BindHostile(false);
            }
            const std::string diff = Diff(before, r.TakeFull());
            if (!diff.empty())
                std::printf("FullStateSnapshot[%s]: empty game state, difference after restore: %s\n", tag, diff.c_str());
            CHECK(diff.empty());
        }

        // 6. an inert snapshot reads and writes nothing
        {
            ctx->ClearState();
            r.BindGame(false);
            {
                FullStateSnapshot snapshot(ctx, wd.c1.Get(), wd.fl111, false);
                r.BindHostile(false);
            }
            CHECK(r.TakeFull().at("VS.shader") == P(wd.ownVs.Get()));
            CHECK(r.TakeFull().at("pred.object") == P(r.ownPredicate.Get()));
        }

        // 7. the restore of the SRVs is bounded: each stage gets one call [0, hi] with hi = the highest slot that is bound, either before the stand-in or after it,
        //    and nothing above it. ResTrack remembers the whole range of every such call, so a full 128-slot restore would put slots 0-127 on its scan list.
        {
            const bool canRecord = rec::Install(ctx);
            CHECK(canRecord); // the hooks attach to this world's context entries (and are removed at the end of this block)
            auto view = [&](int i) { return wd.srv[i].Get(); };
            auto expectCalls = [&](const char* what, const UINT (&hi)[rec::kStages]) {
                if (!canRecord)
                    return;
                for (int s = 0; s < rec::kStages; ++s)
                {
                    const auto calls = rec::Of(s);
                    CHECK(calls.size() == 1);
                    if (calls.size() == 1)
                    {
                        const bool ok = calls[0].start == 0 && calls[0].num == hi[s] + 1;
                        if (!ok)
                            std::printf("FullStateSnapshot[%s]: %s, stage %d restore call [%u, +%u), expected [0, +%u)\n", tag, what, s, calls[0].start, calls[0].num, hi[s] + 1);
                        CHECK(ok);
                    }
                }
            };
            auto expectNoCalls = [&](const char* what) {
                if (!canRecord)
                    return;
                if (!rec::g_calls.empty())
                    std::printf("FullStateSnapshot[%s]: %s, %zu SRV restore calls, expected none\n", tag, what, rec::g_calls.size());
                CHECK(rec::g_calls.empty());
            };
            if (canRecord)
            { // self-test: a direct call is recorded, so a restore that sends nothing is not just a blind recorder
                ctx->ClearState();
                ID3D11ShaderResourceView* sv = view(0);
                ID3D11ShaderResourceView* two[2] { view(0), nullptr };
                rec::Begin(ctx);
                ctx->PSSetShaderResources(3, 1, &sv);
                ctx->CSSetShaderResources(0, 2, two);
                rec::End();
                CHECK(rec::Of(rec::kPS).size() == 1 && rec::Of(rec::kPS)[0].start == 3 && rec::Of(rec::kPS)[0].num == 1 && rec::Of(rec::kCS).size() == 1 && rec::Of(rec::kCS)[0].num == 2);
            }

            // (a) the game has low slots bound; the stand-in adds one HIGH slot per stage (and changes nothing else): it has to come back null, whatever the stage
            {
                ctx->ClearState();
                rec::Plan game {};
                for (auto& st : game) { st[0] = view(0); st[5] = view(1); }
                rec::Apply(ctx, game);
                const Snap before = r.TakeFull();
                const UINT high[rec::kStages] { 64, 100, 127, 10, 90, 100 };
                {
                    FullStateSnapshot snapshot(ctx, wd.c1.Get(), wd.fl111);
                    rec::Plan stand = game;
                    for (int s = 0; s < rec::kStages; ++s) stand[s][high[s]] = view(6);
                    rec::Apply(ctx, stand);
                    const Snap during = r.TakeFull();
                    CHECK(during.at("VS.srv64") == P(view(6)) && during.at("HS.srv100") == P(view(6)) && during.at("DS.srv127") == P(view(6)) && during.at("GS.srv10") == P(view(6)) &&
                          during.at("PS.srv90") == P(view(6)) && during.at("CS.srv100") == P(view(6)));
                    rec::Begin(ctx);
                    snapshot.Restore();
                    rec::End();
                }
                const std::string diff = Diff(before, r.TakeFull());
                if (!diff.empty())
                    std::printf("FullStateSnapshot[%s]: high slots, difference after restore: %s\n", tag, diff.c_str());
                CHECK(diff.empty());
                expectCalls("high slots added by the stand-in", high);
            }

            // (b) the game has a HIGH slot bound; the stand-in clears everything and binds one low slot: the high slot has to come back, the low one go
            {
                ctx->ClearState();
                rec::Plan game {};
                for (auto& st : game) { st[0] = view(0); st[100] = view(1); }
                rec::Apply(ctx, game);
                const Snap before = r.TakeFull();
                {
                    FullStateSnapshot snapshot(ctx, wd.c1.Get(), wd.fl111);
                    rec::Plan stand {};
                    for (auto& st : stand) st[1] = view(7);
                    rec::Apply(ctx, stand);
                    const Snap during = r.TakeFull();
                    CHECK(during.at("PS.srv100") == 0 && during.at("CS.srv100") == 0 && during.at("VS.srv1") == P(view(7)));
                    rec::Begin(ctx);
                    snapshot.Restore();
                    rec::End();
                }
                const std::string diff = Diff(before, r.TakeFull());
                if (!diff.empty())
                    std::printf("FullStateSnapshot[%s]: high slot of the game, difference after restore: %s\n", tag, diff.c_str());
                CHECK(diff.empty());
                const UINT high[rec::kStages] { 100, 100, 100, 100, 100, 100 };
                expectCalls("high slot of the game cleared by the stand-in", high);
            }

            // (c) nothing in the SRVs changed (the stand-in only changes the topology): no SRV call at all, with SRVs bound and with none bound
            {
                for (int bound = 0; bound < 2; ++bound)
                {
                    ctx->ClearState();
                    if (bound)
                    {
                        rec::Plan game {};
                        for (auto& st : game) { st[0] = view(0); st[100] = view(1); }
                        rec::Apply(ctx, game);
                    }
                    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    const Snap before = r.TakeFull();
                    {
                        FullStateSnapshot snapshot(ctx, wd.c1.Get(), wd.fl111);
                        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                        rec::Begin(ctx);
                        snapshot.Restore();
                        rec::End();
                    }
                    CHECK(Diff(before, r.TakeFull()).empty());
                    expectNoCalls(bound ? "SRVs unchanged (bound)" : "all stages empty and unchanged");
                }
            }

            // (d) an empty game state and one stand-in SRV, in the pixel stage only: only the pixel stage is restored, to [0, 3]
            {
                ctx->ClearState();
                const Snap before = r.TakeFull();
                {
                    FullStateSnapshot snapshot(ctx, wd.c1.Get(), wd.fl111);
                    ID3D11ShaderResourceView* sv = view(2);
                    ctx->PSSetShaderResources(3, 1, &sv);
                    rec::Begin(ctx);
                    snapshot.Restore();
                    rec::End();
                }
                CHECK(Diff(before, r.TakeFull()).empty());
                if (canRecord)
                {
                    CHECK(rec::g_calls.size() == 1 && rec::g_calls[0].stage == rec::kPS && rec::g_calls[0].start == 0 && rec::g_calls[0].num == 4);
                }
            }
            rec::Uninstall();
        }
        ctx->ClearState();
    };
    fullStateCases(w, "default adapter level");
    {
        World w110;
        w110.only110 = true;
        if (w110.Create())
            fullStateCases(w110, "feature level 11_0 device");
        else
            std::printf("FullStateSnapshot: no 11_0-only WARP device, that variant is skipped\n");
    }

    if (g_failures == 0)
        std::printf("d3d11_explicit_state_warp: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
