// The reflections' frame-exact A/B (ext/ffxiv/engine/SsrAb.cpp) on the D3D11 WARP software adapter.
//
// The fixture is the water draw's shape, simplified: a TESSELLATED draw (VS, HS, DS, PS) over a full-window triangle, a pixel shader that samples
// the reflection map at t8 the way the game's water shaders do - it ADDS the map's rgb and scales a probe colour by 1/(1+alpha)^2 - and blends the
// result ADDITIVELY onto a float target that already holds a picture, with a D24S8 depth buffer in a state that WOULD write both depth and stencil
// (always-pass depth, write all, stencil REPLACE). That makes every claim of the diagnostic measurable:
//   the extra draw sees the same destination (additive blend: a copy that was skipped changes the sum),
//   it samples the engine's map and the real draw samples this module's (the two pictures differ exactly by the formula, texel by texel),
//   it writes neither depth nor stencil (the D24S8 bytes are identical before the real draw; the real draw does change them: the state would write),
//   the explicit-state guard puts everything back (render target, depth view, depth-stencil state and reference, the reflection slot, HS/DS/PS),
//   the tessellation stages stay bound for the extra draw (the guard is never asked to neutralise them),
//   the extra draw is another module's replay and this module's own work, the real draw is neither,
//   a draw with a second target, an output-merger UAV, no target, a multisampled target, the SSR diagnostic on, a closed capture or a served-less
//   reader is left alone (one draw, one note, no extra draw), and the capture's one draw is taken once.
#include "pch.h" // the tests' stand-in for the product header: the counting log macros
#include "check.h"

#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/diag/StageDump.h"
#include "ext/ffxiv/engine/EnginePass.h"
#include "ext/ffxiv/engine/SsrAb.h"
#include "ext/ffxiv/engine/SsrApply.h"
#include "ext/present/PresentSeams.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <process.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
namespace ab = ext::ffxiv::engine::ssr::ab;
namespace diag = ext::ffxiv::diag;
using ext::ffxiv::engine::ssr::BoundReader;

// ---- the module's seams (the same three the reflections' other device test defines) ------------------------------------------------------
namespace ext::ffxiv::engine
{
int g_ssrTestPublishCalls = 0;
bool g_ssrTestOwnWork = false;
void Publish(bool) { ++g_ssrTestPublishCalls; }
void SetOwnWork(bool on) { g_ssrTestOwnWork = on; }
} // namespace ext::ffxiv::engine

namespace ext::present
{
uint64_t FrameCount() noexcept { return 1; }
} // namespace ext::present

namespace ext::ffxiv::chara::probe::dump
{
std::filesystem::path OutputRoot() { return std::filesystem::temp_directory_path() / (L"ssr-ab-test-" + std::to_wstring(_getpid())); }
} // namespace ext::ffxiv::chara::probe::dump

namespace
{
constexpr uint32_t kWidth = 16;
constexpr uint32_t kHeight = 12;
constexpr uint32_t kSlot = 8;
constexpr float kProbe[3] { 0.8f, 0.6f, 0.4f };
constexpr UINT kStencilRef = 0x55;
constexpr D3D11_VIEWPORT kViewport { 2.0f, 2.0f, 12.0f, 8.0f, 0.0f, 1.0f }; // x 2..13, y 2..9

const char* kSource = R"hlsl(
Texture2D<float4> g_SamplerReflectionMap : register(t8);
cbuffer Probe : register(b0) { float4 g_Probe; };
struct VsOut { float4 pos : POSITION0; };
VsOut VS(uint id : SV_VertexID)
{
    float2 p = float2((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0);
    VsOut o;
    o.pos = float4(p, 0.25, 1.0);
    return o;
}
struct HsConst { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
HsConst HsC(InputPatch<VsOut, 3> patch)
{
    HsConst c;
    c.edges[0] = 1; c.edges[1] = 1; c.edges[2] = 1; c.inside = 1;
    return c;
}
[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(3)] [patchconstantfunc("HsC")]
VsOut HS(InputPatch<VsOut, 3> patch, uint i : SV_OutputControlPointID) { return patch[i]; }
[domain("tri")]
float4 DS(HsConst c, const OutputPatch<VsOut, 3> patch, float3 bary : SV_DomainLocation) : SV_Position
{
    return patch[0].pos * bary.x + patch[1].pos * bary.y + patch[2].pos * bary.z;
}
// The water shaders' use of the reflection map, simplified: the map's rgb is ADDED (ungated by its alpha) and its alpha dims a probe colour.
float4 PS(float4 position : SV_Position) : SV_Target0
{
    int2 texel = int2(position.xy);
    float4 m = g_SamplerReflectionMap.Load(int3(texel, 0));
    float k = 1.0 / (1.0 + m.a);
    return float4(g_Probe.rgb * (k * k) + m.rgb, 1.0);
}
)hlsl";

// ---- the recorded hooks ---------------------------------------------------------------------------------------------------------------------
struct Picture
{
    std::string name;
    uint32_t width = 0, height = 0;
    std::vector<float> rgba;
    diag::StageWindow window;
    uint32_t crc = 0;
};
std::vector<Picture> g_pictures;
std::vector<std::string> g_notes;
int g_takeAsks = 0;      // every ask
int g_offers = 0;        // the asks answered yes (the tries of the capture)
bool g_accepted = false; // the A/B kept a draw
int g_accepts = 0;

// The runtime's rule: an offer while no draw was kept and fewer than kMaxTries were offered; each offer is one try.
bool FakeTake(uint32_t* position, uint32_t* tryIndex)
{
    ++g_takeAsks;
    if (g_accepted || g_offers >= int(ab::kMaxTries))
        return false;
    ++g_offers;
    if (position != nullptr)
        *position = 4242;
    if (tryIndex != nullptr)
        *tryIndex = uint32_t(g_offers);
    return true;
}

void FakeAccept()
{
    g_accepted = true;
    ++g_accepts;
}

void FakeNote(const char* text) { g_notes.push_back(text); }

std::vector<float> ReadFloats(ID3D11DeviceContext* context, ID3D11Texture2D* texture, uint32_t* width, uint32_t* height)
{
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    std::vector<float> out;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)))
        return out;
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE map {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return out;
    *width = desc.Width;
    *height = desc.Height;
    out.resize(size_t(desc.Width) * desc.Height * 4);
    for (uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(out.data() + size_t(y) * desc.Width * 4, static_cast<const unsigned char*>(map.pData) + size_t(y) * map.RowPitch, desc.Width * 16);
    context->Unmap(staging.Get(), 0);
    return out;
}

std::vector<uint32_t> ReadWords(ID3D11DeviceContext* context, ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    std::vector<uint32_t> out;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)))
        return out;
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE map {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return out;
    out.resize(size_t(desc.Width) * desc.Height);
    for (uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(out.data() + size_t(y) * desc.Width, static_cast<const unsigned char*>(map.pData) + size_t(y) * map.RowPitch, desc.Width * 4);
    context->Unmap(staging.Get(), 0);
    return out;
}

void FakeImage(const char* name, ID3D11DeviceContext* context, ID3D11Texture2D* texture, const diag::StageWindow& window, uint32_t crc)
{
    Picture p;
    p.name = name;
    p.window = window;
    p.crc = crc;
    p.rgba = ReadFloats(context, texture, &p.width, &p.height);
    g_pictures.push_back(std::move(p));
}

const Picture* Find(const char* name)
{
    for (const Picture& p : g_pictures)
        if (p.name == name)
            return &p;
    return nullptr;
}

bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

bool AnyNoteHas(const char* needle)
{
    for (const std::string& n : g_notes)
        if (Has(n, needle))
            return true;
    return false;
}

// ---- the maps: B8G8R8A8_UNORM like the engine's, a different content for the engine's and this module's ----------------------------------
struct Texel
{
    uint8_t b, g, r, a;
};

Texel EngineTexel(uint32_t x, uint32_t y)
{
    return Texel { uint8_t((x * 13 + y * 7 + 5) & 0xFF), uint8_t((x * 5 + y * 11 + 40) & 0xFF), uint8_t((x * 3 + y * 17 + 90) & 0xFF),
                   uint8_t(((x + y) % 3 == 0) ? ((x * 20 + y * 9) & 0xFF) : 0) };
}

Texel OursTexel(uint32_t x, uint32_t y)
{
    return Texel { uint8_t((x * 7 + y * 3 + 60) & 0xFF), uint8_t((x * 9 + y * 5 + 10) & 0xFF), uint8_t((x * 11 + y * 2 + 200) & 0xFF),
                   uint8_t((x % 2) ? (96 + ((x * 6 + y * 13) & 0x7F)) : 0) };
}

// The simplified water shader on the CPU: the probe dimmed by the map's alpha plus the map's rgb, in the order the shader does it.
void Water(const Texel& t, float out[3])
{
    const float a = float(t.a) / 255.0f;
    const float k = 1.0f / (1.0f + a);
    const float rgb[3] { float(t.r) / 255.0f, float(t.g) / 255.0f, float(t.b) / 255.0f };
    for (int c = 0; c < 3; ++c)
        out[c] = kProbe[c] * (k * k) + rgb[c];
}

void Before(uint32_t x, uint32_t y, float out[4])
{
    out[0] = 0.01f * float(x) + 0.02f * float(y);
    out[1] = 0.03f * float(x) + 0.005f * float(y);
    out[2] = 0.5f - 0.01f * float(x);
    out[3] = 0.25f;
}

bool InWindow(uint32_t x, uint32_t y) { return x >= 2 && x < 14 && y >= 2 && y < 10; }

bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

struct Observation
{
    bool seen = false;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_DEPTH_STENCIL_DESC dss {};
    UINT ref = 0;
    ID3D11ShaderResourceView* slot = nullptr;
    bool hs = false, ds = false, ps = false;
    bool replay = false, ownWork = false;
    std::vector<uint32_t> depth;
};

// The pipeline pointers a draw's state is compared by (a getter hands back a reference, released right away: the pointer value is what is compared).
struct Snapshot
{
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11DepthStencilState* dss = nullptr;
    UINT ref = 0;
    ID3D11ShaderResourceView* slot = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    D3D11_VIEWPORT viewport {};
    bool Equal(const Snapshot& o) const
    {
        return rtv == o.rtv && dsv == o.dsv && dss == o.dss && ref == o.ref && slot == o.slot && vs == o.vs && hs == o.hs && ds == o.ds && ps == o.ps &&
               blend == o.blend && rs == o.rs && topology == o.topology && viewport.TopLeftX == o.viewport.TopLeftX &&
               viewport.TopLeftY == o.viewport.TopLeftY && viewport.Width == o.viewport.Width && viewport.Height == o.viewport.Height;
    }
};

template <class T> T* Raw(ComPtr<T>& p) { return p.Get(); }

struct Fixture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11Texture2D> target, depth;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11Texture2D> engineMap, oursMap;
    ComPtr<ID3D11ShaderResourceView> engineView, oursView;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> writes;
    int draws = 0;
    Observation observed[4];

    ComPtr<ID3D11Texture2D> MakeMap(Texel (*texel)(uint32_t, uint32_t))
    {
        std::vector<uint8_t> bytes(size_t(kWidth) * kHeight * 4);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const Texel t = texel(x, y);
                uint8_t* p = &bytes[(size_t(y) * kWidth + x) * 4];
                p[0] = t.b;
                p[1] = t.g;
                p[2] = t.r;
                p[3] = t.a;
            }
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA initial { bytes.data(), kWidth * 4, 0 };
        ComPtr<ID3D11Texture2D> texture;
        device->CreateTexture2D(&desc, &initial, &texture);
        return texture;
    }

    ComPtr<ID3D11Texture2D> MakeTarget(DXGI_FORMAT format, uint32_t samples, const float* initial)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = samples;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        std::vector<float> data;
        D3D11_SUBRESOURCE_DATA sub {};
        const D3D11_SUBRESOURCE_DATA* sp = nullptr;
        if (initial != nullptr)
        {
            data.assign(initial, initial + size_t(kWidth) * kHeight * 4);
            sub = { data.data(), kWidth * 16, 0 };
            sp = &sub;
        }
        ComPtr<ID3D11Texture2D> texture;
        device->CreateTexture2D(&desc, sp, &texture);
        return texture;
    }

    bool Create()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)) || !device)
            return false;
        const auto compile = [](const char* entry, const char* profile, ComPtr<ID3DBlob>& blob) {
            ComPtr<ID3DBlob> errors;
            const HRESULT hr = D3DCompile(kSource, std::strlen(kSource), "ssr_ab_fixture", nullptr, nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0,
                                          &blob, &errors);
            if (FAILED(hr) && errors)
                std::printf("shader %s: %s\n", entry, static_cast<const char*>(errors->GetBufferPointer()));
            return SUCCEEDED(hr);
        };
        ComPtr<ID3DBlob> vsb, hsb, dsb, psb;
        if (!compile("VS", "vs_5_0", vsb) || !compile("HS", "hs_5_0", hsb) || !compile("DS", "ds_5_0", dsb) || !compile("PS", "ps_5_0", psb))
            return false;
        device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
        device->CreateHullShader(hsb->GetBufferPointer(), hsb->GetBufferSize(), nullptr, &hs);
        device->CreateDomainShader(dsb->GetBufferPointer(), dsb->GetBufferSize(), nullptr, &ds);
        device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);

        const float probe[4] { kProbe[0], kProbe[1], kProbe[2], 0.0f };
        D3D11_BUFFER_DESC cb {};
        cb.ByteWidth = 16;
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA cbData { probe, 0, 0 };
        device->CreateBuffer(&cb, &cbData, &constants);

        engineMap = MakeMap(&EngineTexel);
        oursMap = MakeMap(&OursTexel);
        device->CreateShaderResourceView(engineMap.Get(), nullptr, &engineView);
        device->CreateShaderResourceView(oursMap.Get(), nullptr, &oursView);

        D3D11_TEXTURE2D_DESC dd {};
        dd.Width = kWidth;
        dd.Height = kHeight;
        dd.MipLevels = 1;
        dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
        dd.SampleDesc.Count = 1;
        dd.Usage = D3D11_USAGE_DEFAULT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        device->CreateTexture2D(&dd, nullptr, &depth);
        D3D11_DEPTH_STENCIL_VIEW_DESC dv {};
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(depth.Get(), &dv, &dsv);

        D3D11_RASTERIZER_DESC rs {};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        device->CreateRasterizerState(&rs, &rasterizer);

        D3D11_BLEND_DESC bd {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&bd, &blend);

        // A state that WOULD write both: the depth test always passes and writes, the stencil replaces with the reference under a full write mask.
        D3D11_DEPTH_STENCIL_DESC writeDesc {};
        writeDesc.DepthEnable = TRUE;
        writeDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        writeDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        writeDesc.StencilEnable = TRUE;
        writeDesc.StencilReadMask = 0xFF;
        writeDesc.StencilWriteMask = 0xFF;
        writeDesc.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS };
        writeDesc.BackFace = writeDesc.FrontFace;
        device->CreateDepthStencilState(&writeDesc, &writes);
        return vs && hs && ds && ps && constants && engineView && oursView && depth && dsv && rasterizer && blend && writes;
    }

    // A new target holding the picture, the depth buffer cleared, and the pipeline bound as the game binds a water draw: the engine's map at t8.
    void Reset(bool withDepth = true)
    {
        float initial[kWidth * kHeight * 4];
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                Before(x, y, &initial[(size_t(y) * kWidth + x) * 4]);
        target = MakeTarget(DXGI_FORMAT_R32G32B32A32_FLOAT, 1, initial);
        device->CreateRenderTargetView(target.Get(), nullptr, &rtv);
        context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.5f, 7);
        BindPipeline();
        ID3D11RenderTargetView* targets[1] { rtv.Get() };
        context->OMSetRenderTargets(1, targets, withDepth ? dsv.Get() : nullptr);
        ID3D11ShaderResourceView* engine = engineView.Get();
        context->PSSetShaderResources(kSlot, 1, &engine);
        draws = 0;
        for (Observation& o : observed)
            o = Observation {};
        g_pictures.clear();
        g_notes.clear();
        g_accepted = false;
        g_accepts = 0;
        g_offers = 0;
        g_takeAsks = 0;
    }

    void BindPipeline()
    {
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->HSSetShader(hs.Get(), nullptr, 0);
        context->DSSetShader(ds.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11Buffer* cb = constants.Get();
        context->PSSetConstantBuffers(0, 1, &cb);
        context->RSSetState(rasterizer.Get());
        context->RSSetViewports(1, &kViewport);
        const float factor[4] {};
        context->OMSetBlendState(blend.Get(), factor, 0xFFFFFFFFu);
        context->OMSetDepthStencilState(writes.Get(), kStencilRef);
    }

    Snapshot Take()
    {
        Snapshot s;
        ID3D11RenderTargetView* r = nullptr;
        ID3D11DepthStencilView* d = nullptr;
        context->OMGetRenderTargets(1, &r, &d);
        s.rtv = r;
        s.dsv = d;
        if (r) r->Release();
        if (d) d->Release();
        ID3D11DepthStencilState* dss = nullptr;
        context->OMGetDepthStencilState(&dss, &s.ref);
        s.dss = dss;
        if (dss) dss->Release();
        ID3D11ShaderResourceView* v = nullptr;
        context->PSGetShaderResources(kSlot, 1, &v);
        s.slot = v;
        if (v) v->Release();
        ID3D11VertexShader* vsr = nullptr; context->VSGetShader(&vsr, nullptr, nullptr); s.vs = vsr; if (vsr) vsr->Release();
        ID3D11HullShader* hsr = nullptr; context->HSGetShader(&hsr, nullptr, nullptr); s.hs = hsr; if (hsr) hsr->Release();
        ID3D11DomainShader* dsr = nullptr; context->DSGetShader(&dsr, nullptr, nullptr); s.ds = dsr; if (dsr) dsr->Release();
        ID3D11PixelShader* psr = nullptr; context->PSGetShader(&psr, nullptr, nullptr); s.ps = psr; if (psr) psr->Release();
        ID3D11BlendState* bl = nullptr; float f[4]; UINT m = 0; context->OMGetBlendState(&bl, f, &m); s.blend = bl; if (bl) bl->Release();
        ID3D11RasterizerState* rsr = nullptr; context->RSGetState(&rsr); s.rs = rsr; if (rsr) rsr->Release();
        context->IAGetPrimitiveTopology(&s.topology);
        UINT n = 1;
        context->RSGetViewports(&n, &s.viewport);
        return s;
    }

    // The game's draw callback: records what the state looks like at that call, then draws.
    static void GameDraw(void* opaque)
    {
        Fixture& f = *static_cast<Fixture*>(opaque);
        Observation& o = f.observed[std::min(f.draws, 3)];
        o.seen = true;
        const Snapshot s = f.Take();
        o.rtv = s.rtv;
        o.dsv = s.dsv;
        o.ref = s.ref;
        o.slot = s.slot;
        o.hs = s.hs != nullptr;
        o.ds = s.ds != nullptr;
        o.ps = s.ps != nullptr;
        ID3D11DepthStencilState* dss = nullptr;
        UINT ref = 0;
        f.context->OMGetDepthStencilState(&dss, &ref);
        if (dss)
        {
            dss->GetDesc(&o.dss);
            dss->Release();
        }
        o.replay = ext::ffxiv::chara::t_replay;
        o.ownWork = ext::ffxiv::engine::g_ssrTestOwnWork;
        o.depth = ReadWords(f.context.Get(), f.depth.Get());
        ++f.draws;
        f.context->Draw(3, 0);
    }

    // What the draw path does for a served reader: the swap guard holds the engine's view while this module's is bound for the draw.
    struct Served
    {
        BoundReader reader;
    };
    BoundReader Serve()
    {
        ID3D11ShaderResourceView* previous = ext::ffxiv::engine::ssr::detail::SwapSlot(context.Get(), kSlot, oursView.Get());
        return BoundReader(context.Get(), kSlot, previous);
    }
};

void OpenCapture()
{
    diag::detail::g_stageOpen.store(diag::kStagePreBit, std::memory_order_relaxed);
    diag::detail::g_abTake = &FakeTake;
    diag::detail::g_abAccept = &FakeAccept;
    diag::detail::g_abImage = &FakeImage;
    diag::detail::g_abNote = &FakeNote;
}

void CloseCapture() { diag::detail::g_stageOpen.store(0, std::memory_order_relaxed); }
} // namespace

int main()
{
    Fixture f;
    CHECK(f.Create());
    if (!f.device)
        return Finish("ssr_ab_warp");
    D3D11_FEATURE_DATA_D3D11_OPTIONS options {};
    (void) options;

    // ======================================================================================================================================
    // the capture frame's A/B draw: the extra draw with the engine's map, then the real one with this module's
    // ======================================================================================================================================
    {
        OpenCapture();
        f.Reset();
        ab::ResetForTest();
        const std::vector<uint32_t> depthAtStart = ReadWords(f.context.Get(), f.depth.Get());
        CHECK(!depthAtStart.empty());
        BoundReader reader = f.Serve();
        CHECK(reader.Bound());
        const Snapshot game = f.Take();
        CHECK(game.slot == f.oursView.Get()); // this module's map is bound for the draw
        CHECK(game.hs != nullptr && game.ds != nullptr);

        ab::DrawFn draw = &Fixture::GameDraw;
        void* user = &f;
        ab::Call call;
        const bool begun = ab::Begin(call, f.context.Get(), reader, draw, user);
        CHECK(begun);
        CHECK(call.active);
        CHECK(draw != &Fixture::GameDraw && user == &call); // the draw path now runs the thunk
        CHECK_EQ(f.draws, 0);                               // nothing was drawn by Begin itself
        draw(user);                                         // the draw path's one call
        ab::End(call);

        CHECK_EQ(f.draws, 2); // the extra draw and the real one
        CHECK_EQ(ab::Runs(), 1u);
        CHECK_EQ(ab::Kept(), 1u);
        CHECK_EQ(ab::SkippedEmpty() + ab::SkippedSmall() + ab::KeptUnknown(), 0u);
        CHECK_EQ(g_accepts, 1); // the capture is told this draw was kept
        const Observation& shadow = f.observed[0];
        const Observation& real = f.observed[1];
        CHECK(shadow.seen && real.seen);

        // The extra draw: a private target (not the game's), the game's depth view, the engine's map, tessellation still bound, depth writes OFF and
        // stencil writes OFF with the game's tests and reference otherwise, marked as a replay and as this module's own work.
        CHECK(shadow.rtv != nullptr && shadow.rtv != game.rtv);
        CHECK(shadow.dsv == game.dsv);
        CHECK(shadow.slot == f.engineView.Get());
        CHECK(shadow.hs && shadow.ds && shadow.ps);
        CHECK(shadow.dss.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO);
        CHECK_EQ(uint32_t(shadow.dss.StencilWriteMask), 0u);
        CHECK(shadow.dss.DepthEnable && shadow.dss.DepthFunc == D3D11_COMPARISON_ALWAYS); // the game's tests are kept
        CHECK(shadow.dss.StencilEnable && shadow.dss.StencilReadMask == 0xFF);
        CHECK(shadow.dss.FrontFace.StencilPassOp == D3D11_STENCIL_OP_REPLACE && shadow.dss.BackFace.StencilPassOp == D3D11_STENCIL_OP_REPLACE);
        CHECK_EQ(shadow.ref, kStencilRef);
        CHECK(shadow.replay && shadow.ownWork);

        // The real draw: the game's own target, state and depth view; this module's map; neither marker.
        CHECK(real.rtv == game.rtv && real.dsv == game.dsv);
        CHECK(real.slot == f.oursView.Get());
        CHECK(real.hs && real.ds && real.ps);
        CHECK(real.dss.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL && real.dss.StencilWriteMask == 0xFF);
        CHECK_EQ(real.ref, kStencilRef);
        CHECK(!real.replay && !real.ownWork);

        // The depth-stencil buffer: unchanged until the real draw (the extra draw wrote neither depth nor stencil), changed by the real one (the
        // state would have written both: the check is not vacuous).
        CHECK(shadow.depth == depthAtStart);
        CHECK(real.depth == depthAtStart);
        const std::vector<uint32_t> depthAfter = ReadWords(f.context.Get(), f.depth.Get());
        CHECK(depthAfter != depthAtStart);
        CHECK_EQ(depthAfter[3 * kWidth + 3] >> 24, uint32_t(kStencilRef)); // the real draw's stencil REPLACE landed inside the window
        CHECK(depthAfter[0] == depthAtStart[0]);                            // and outside it nothing did

        // Everything is back as the game left it, the reflection slot with this module's view.
        const Snapshot after = f.Take();
        CHECK(after.Equal(game));

        // Three pictures, in the order they were made, named, with the shader and the window.
        CHECK_EQ(g_pictures.size(), size_t(3));
        const Picture* before = Find("ssr-ab-before");
        const Picture* engine = Find("ssr-ab-engine");
        const Picture* ours = Find("ssr-ab-ours");
        CHECK(before != nullptr && engine != nullptr && ours != nullptr);
        if (before && engine && ours)
        {
            CHECK(g_pictures[0].name == "ssr-ab-before" && g_pictures[1].name == "ssr-ab-engine" && g_pictures[2].name == "ssr-ab-ours");
            CHECK_EQ(before->width, kWidth);
            CHECK_EQ(before->height, kHeight);
            CHECK(before->window.x == 2.0f && before->window.y == 2.0f && before->window.width == 12.0f && before->window.height == 8.0f);
            uint32_t exactBefore = 0, formulaEngine = 0, formulaOurs = 0, formulaDiff = 0, outsideKept = 0, differing = 0, windowTexels = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    const size_t i = (size_t(y) * kWidth + x) * 4;
                    float b[4];
                    Before(x, y, b);
                    bool sameBefore = true;
                    for (int c = 0; c < 4; ++c)
                        sameBefore = sameBefore && before->rgba[i + c] == b[c];
                    exactBefore += sameBefore ? 1 : 0;
                    if (!InWindow(x, y))
                    {
                        bool kept = true;
                        for (int c = 0; c < 4; ++c)
                            kept = kept && engine->rgba[i + c] == b[c] && ours->rgba[i + c] == b[c];
                        outsideKept += kept ? 1 : 0;
                        continue;
                    }
                    ++windowTexels;
                    float we[3], wo[3];
                    Water(EngineTexel(x, y), we);
                    Water(OursTexel(x, y), wo);
                    bool okEngine = Near(engine->rgba[i + 3], b[3] + 1.0f, 1e-6f), okOurs = Near(ours->rgba[i + 3], b[3] + 1.0f, 1e-6f), okDiff = true;
                    for (int c = 0; c < 3; ++c)
                    {
                        okEngine = okEngine && Near(engine->rgba[i + c], b[c] + we[c], 1e-4f);
                        okOurs = okOurs && Near(ours->rgba[i + c], b[c] + wo[c], 1e-4f);
                        okDiff = okDiff && Near(ours->rgba[i + c] - engine->rgba[i + c], wo[c] - we[c], 2e-4f);
                    }
                    formulaEngine += okEngine ? 1 : 0;
                    formulaOurs += okOurs ? 1 : 0;
                    formulaDiff += okDiff ? 1 : 0;
                    differing += (std::fabs(ours->rgba[i] - engine->rgba[i]) > 1e-3f) ? 1 : 0;
                }
            CHECK_EQ(exactBefore, kWidth * kHeight);          // the copy is the draw's own destination, bit for bit
            CHECK_EQ(windowTexels, 96u);
            CHECK_EQ(formulaEngine, windowTexels);            // the extra draw = destination + the water formula on the ENGINE's map
            CHECK_EQ(formulaOurs, windowTexels);              // the real draw = destination + the water formula on THIS MODULE's map
            CHECK_EQ(formulaDiff, windowTexels);              // so the pictures differ exactly by the formula
            CHECK_EQ(outsideKept, kWidth * kHeight - windowTexels); // outside the viewport nothing was drawn by either draw
            CHECK(differing > windowTexels / 2);              // and the maps are different enough for the difference to be seen
            CHECK_EQ(before->crc, engine->crc);
            CHECK_EQ(engine->crc, ours->crc);
        }
        // The game's target after everything is the real draw's picture.
        uint32_t w = 0, h = 0;
        const std::vector<float> finalTarget = ReadFloats(f.context.Get(), f.target.Get(), &w, &h);
        CHECK(ours != nullptr && finalTarget == ours->rgba);

        // Notes: what ran.
        CHECK(AnyNoteHas("# ssr-ab ran"));
        CHECK(AnyNoteHas("# ssr-ab try 1 | frame position 4242"));
        CHECK(AnyNoteHas("extra draw wrote 96 pixels (floor 1 of 96) | kept")); // one pixel per texel of the 12 x 8 window
        CHECK(AnyNoteHas("extra draw wrote 96 pixels, real draw wrote 96 pixels (equal)")); // the real draw wrote exactly what the extra draw did
        CHECK(AnyNoteHas("frame position 4242"));
        CHECK(AnyNoteHas("reflection slot t8"));
        CHECK(AnyNoteHas("depth view bound"));
        CHECK(AnyNoteHas("depth state copied from the game's (depth writes on, stencil write mask 0xFF)"));

        // ---- the capture's one draw is taken once: the next reader draw of the frame is left alone ---------------------------------------------
        {
            const size_t pictures = g_pictures.size();
            const int drawsBefore = f.draws;
            BoundReader again = f.Serve();
            ab::DrawFn d2 = &Fixture::GameDraw;
            void* u2 = &f;
            ab::Call second;
            CHECK(!ab::Begin(second, f.context.Get(), again, d2, u2));
            CHECK(d2 == &Fixture::GameDraw && u2 == &f && !second.active);
            d2(u2);
            ab::End(second);
            CHECK_EQ(f.draws, drawsBefore + 1);        // one draw: the game's own
            CHECK_EQ(g_pictures.size(), pictures);     // no picture
            CHECK_EQ(ab::Runs(), 1u);
        }
    }

    // ======================================================================================================================================
    // a frame that is not the capture frame: the open mask is closed, the take hook is never asked, the draw is the game's alone
    // ======================================================================================================================================
    {
        f.Reset();
        CloseCapture();
        BoundReader reader = f.Serve();
        const Snapshot game = f.Take();
        ab::DrawFn draw = &Fixture::GameDraw;
        void* user = &f;
        ab::Call call;
        CHECK(!ab::Begin(call, f.context.Get(), reader, draw, user));
        CHECK(draw == &Fixture::GameDraw && user == &f && !call.active);
        CHECK_EQ(g_takeAsks, 0); // not even asked
        draw(user);
        ab::End(call);
        CHECK_EQ(f.draws, 1);
        CHECK(g_pictures.empty() && g_notes.empty());
        CHECK(f.Take().Equal(game));
        // A reader draw that was not served (the guard holds nothing) never asks either, with the capture open.
        OpenCapture();
        f.Reset();
        BoundReader unserved;
        ab::DrawFn d2 = &Fixture::GameDraw;
        void* u2 = &f;
        ab::Call c2;
        CHECK(!ab::Begin(c2, f.context.Get(), unserved, d2, u2));
        CHECK_EQ(g_takeAsks, 0);
    }

    // ======================================================================================================================================
    // declines: each leaves the draw as the game made it (one draw, no picture), counted, noted in the facts, logged once a session
    // ======================================================================================================================================
    const auto declined = [&f](ab::Decline reason, const char* noteText, const char* what) {
        BoundReader reader = f.Serve();
        const Snapshot game = f.Take();
        ab::DrawFn draw = &Fixture::GameDraw;
        void* user = &f;
        ab::Call call;
        const uint32_t countBefore = ab::Declines(reason);
        const int logBefore = g_testLogLines;
        const bool begun = ab::Begin(call, f.context.Get(), reader, draw, user);
        if (begun)
            std::printf("  [%s] Begin accepted a draw it should decline\n", what);
        CHECK(!begun);
        CHECK(draw == &Fixture::GameDraw && user == &f && !call.active);
        CHECK_EQ(ab::Declines(reason), countBefore + 1);
        CHECK(AnyNoteHas("# ssr-ab declined"));
        CHECK(AnyNoteHas(noteText));
        CHECK(AnyNoteHas("frame position 4242"));
        draw(user);
        ab::End(call);
        CHECK_EQ(f.draws, 1);
        CHECK(g_pictures.empty());
        CHECK_EQ(ab::Runs(), 0u);
        CHECK(f.Take().Equal(game));
        return g_testLogLines - logBefore; // the log lines this decline wrote
    };

    {
        // a second render target bound (RT1)
        OpenCapture();
        ab::ResetForTest();
        f.Reset();
        ComPtr<ID3D11Texture2D> second = f.MakeTarget(DXGI_FORMAT_R32G32B32A32_FLOAT, 1, nullptr);
        ComPtr<ID3D11RenderTargetView> secondRtv;
        f.device->CreateRenderTargetView(second.Get(), nullptr, &secondRtv);
        ID3D11RenderTargetView* two[2] { f.rtv.Get(), secondRtv.Get() };
        f.context->OMSetRenderTargets(2, two, f.dsv.Get());
        CHECK_EQ(declined(ab::Decline::ExtraTargets, "the draw binds more than RT0", "RT1"), 1); // the first decline of a reason logs
        // a second decline of the same reason in the next capture: counted and noted, not logged again
        f.Reset();
        f.context->OMSetRenderTargets(2, two, f.dsv.Get());
        CHECK_EQ(declined(ab::Decline::ExtraTargets, "the draw binds more than RT0", "RT1 again"), 0);
        CHECK_EQ(ab::Declines(ab::Decline::ExtraTargets), 2u);
    }
    {
        // an output-merger UAV bound
        f.Reset();
        D3D11_TEXTURE2D_DESC ud {};
        ud.Width = 4;
        ud.Height = 4;
        ud.MipLevels = 1;
        ud.ArraySize = 1;
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        ud.SampleDesc.Count = 1;
        ud.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> uavTexture;
        ComPtr<ID3D11UnorderedAccessView> uav;
        CHECK(SUCCEEDED(f.device->CreateTexture2D(&ud, nullptr, &uavTexture)));
        CHECK(SUCCEEDED(f.device->CreateUnorderedAccessView(uavTexture.Get(), nullptr, &uav)));
        ID3D11RenderTargetView* one[1] { f.rtv.Get() };
        ID3D11UnorderedAccessView* uavs[1] { uav.Get() };
        f.context->OMSetRenderTargetsAndUnorderedAccessViews(1, one, f.dsv.Get(), 1, 1, uavs, nullptr);
        declined(ab::Decline::OutputUav, "the draw binds an output-merger UAV", "UAV");
    }
    {
        // no render target
        f.Reset();
        f.context->OMSetRenderTargets(0, nullptr, f.dsv.Get());
        declined(ab::Decline::NoTarget, "the draw writes no colour target", "no target");
    }
    {
        // a multisampled target
        f.Reset();
        ComPtr<ID3D11Texture2D> msaa = f.MakeTarget(DXGI_FORMAT_R8G8B8A8_UNORM, 4, nullptr);
        ComPtr<ID3D11RenderTargetView> msaaRtv;
        if (msaa && SUCCEEDED(f.device->CreateRenderTargetView(msaa.Get(), nullptr, &msaaRtv)))
        {
            ID3D11RenderTargetView* one[1] { msaaRtv.Get() };
            f.context->OMSetRenderTargets(1, one, nullptr);
            declined(ab::Decline::Multisampled, "RT0 is multisampled", "msaa");
        }
        else
            std::printf("  (no 4x multisampled R8G8B8A8 target on this adapter: the multisample case is not exercised)\n");
    }
    {
        // the SSR diagnostic is on: its F7 queries and copies would count the extra draw
        f.Reset();
        ext::ffxiv::engine::ssr::PublishDiagnostic(true);
        CHECK(ext::ffxiv::engine::ssr::DiagnosticOn());
        declined(ab::Decline::SsrDiagnostic, "the SSR diagnostic is on", "diagnostic");
        ext::ffxiv::engine::ssr::PublishDiagnostic(false);
        CHECK(!ext::ffxiv::engine::ssr::DiagnosticOn());
    }
    {
        // a draw with no depth view and no depth-stencil state bound still gets its extra draw (the default state, depth writes off)
        f.Reset(false);
        f.context->OMSetDepthStencilState(nullptr, 0);
        ab::ResetForTest();
        BoundReader reader = f.Serve();
        ab::DrawFn draw = &Fixture::GameDraw;
        void* user = &f;
        ab::Call call;
        CHECK(ab::Begin(call, f.context.Get(), reader, draw, user));
        draw(user);
        ab::End(call);
        CHECK_EQ(f.draws, 2);
        CHECK(f.observed[0].dsv == nullptr && f.observed[0].dss.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO);
        CHECK(AnyNoteHas("depth view none") && AnyNoteHas("depth state default"));
        CHECK_EQ(g_pictures.size(), size_t(3));
    }

    // ======================================================================================================================================
    // which draw is kept: a draw that writes no pixel is skipped (its pictures would show nothing), the next reader draw is offered, the first
    // with coverage is kept; the real draw runs once whatever the extra draw found
    // ======================================================================================================================================
    {
        OpenCapture();
        ab::ResetForTest();
        f.Reset();
        // the game's depth test rejects everything (DepthFunc NEVER): the extra draw and the real draw both write no pixel
        D3D11_DEPTH_STENCIL_DESC never {};
        never.DepthEnable = TRUE;
        never.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        never.DepthFunc = D3D11_COMPARISON_NEVER;
        never.StencilEnable = FALSE;
        ComPtr<ID3D11DepthStencilState> rejects;
        CHECK(SUCCEEDED(f.device->CreateDepthStencilState(&never, &rejects)));
        f.context->OMSetDepthStencilState(rejects.Get(), 0);
        {
            BoundReader reader = f.Serve();
            ab::DrawFn draw = &Fixture::GameDraw;
            void* user = &f;
            ab::Call call;
            CHECK(ab::Begin(call, f.context.Get(), reader, draw, user));
            draw(user);
            ab::End(call);
            CHECK_EQ(f.draws, 2);      // the extra draw and the real draw were issued
            CHECK(g_pictures.empty()); // ... but nothing is kept
            CHECK_EQ(g_accepts, 0);    // and the capture is not told so: it offers the next reader draw
            CHECK(!g_accepted);
            CHECK_EQ(ab::Kept(), 0u);
            CHECK_EQ(ab::SkippedEmpty(), 1u);
            CHECK(AnyNoteHas("# ssr-ab try 1 | frame position 4242 |"));
            CHECK(AnyNoteHas("extra draw wrote 0 pixels (floor 1 of 96) | skipped: wrote no pixel"));
            CHECK(!AnyNoteHas("# ssr-ab ran"));
            CHECK(f.Take().dss == rejects.Get()); // the game's state is back
        }
        // the next reader draw of the frame (the game's own state again, which covers the window) is offered and kept
        f.context->OMSetDepthStencilState(f.writes.Get(), kStencilRef);
        {
            const int drawsBefore = f.draws;
            BoundReader reader = f.Serve();
            ab::DrawFn draw = &Fixture::GameDraw;
            void* user = &f;
            ab::Call call;
            CHECK(ab::Begin(call, f.context.Get(), reader, draw, user));
            draw(user);
            ab::End(call);
            CHECK_EQ(f.draws, drawsBefore + 2);
            CHECK_EQ(g_pictures.size(), size_t(3));
            CHECK_EQ(g_accepts, 1);
            CHECK_EQ(ab::Kept(), 1u);
            CHECK(AnyNoteHas("# ssr-ab try 2 | frame position 4242 |"));
            CHECK(AnyNoteHas("# ssr-ab ran | try 2 |"));
        }
        // and after the kept one no draw is offered: a third reader draw is left alone
        {
            const int drawsBefore = f.draws;
            BoundReader reader = f.Serve();
            ab::DrawFn draw = &Fixture::GameDraw;
            void* user = &f;
            ab::Call call;
            CHECK(!ab::Begin(call, f.context.Get(), reader, draw, user));
            draw(user);
            ab::End(call);
            CHECK_EQ(f.draws, drawsBefore + 1);
        }
    }
    {
        // the tries are capped: every reader draw writes nothing, so after kMaxTries offers none is made (and the real draw always runs)
        OpenCapture();
        ab::ResetForTest();
        f.Reset();
        D3D11_DEPTH_STENCIL_DESC never {};
        never.DepthEnable = TRUE;
        never.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        never.DepthFunc = D3D11_COMPARISON_NEVER;
        ComPtr<ID3D11DepthStencilState> rejects;
        f.device->CreateDepthStencilState(&never, &rejects);
        f.context->OMSetDepthStencilState(rejects.Get(), 0);
        int begun = 0;
        for (uint32_t i = 0; i < ab::kMaxTries + 3; ++i)
        {
            BoundReader reader = f.Serve();
            ab::DrawFn draw = &Fixture::GameDraw;
            void* user = &f;
            ab::Call call;
            begun += ab::Begin(call, f.context.Get(), reader, draw, user) ? 1 : 0;
            draw(user);
            ab::End(call);
        }
        CHECK_EQ(begun, int(ab::kMaxTries));
        CHECK_EQ(ab::SkippedEmpty(), ab::kMaxTries);
        CHECK(g_pictures.empty() && g_accepts == 0);
        CHECK_EQ(f.draws, int(ab::kMaxTries) * 2 + 3); // two draws per try, one per draw after the last try
    }
    {
        // the coverage cannot be counted (no query): the draw is kept and the line says so
        OpenCapture();
        ab::ResetForTest();
        f.Reset();
        ab::SetCoverageQueryForTest(false);
        BoundReader reader = f.Serve();
        ab::DrawFn draw = &Fixture::GameDraw;
        void* user = &f;
        ab::Call call;
        CHECK(ab::Begin(call, f.context.Get(), reader, draw, user));
        draw(user);
        ab::End(call);
        ab::SetCoverageQueryForTest(true);
        CHECK_EQ(f.draws, 2);
        CHECK_EQ(g_pictures.size(), size_t(3));
        CHECK_EQ(ab::KeptUnknown(), 1u);
        CHECK(AnyNoteHas("coverage unknown | kept"));
        CHECK(AnyNoteHas("pixels not counted"));
    }

    // ======================================================================================================================================
    // the depth-stencil copy: pure
    // ======================================================================================================================================
    {
        D3D11_DEPTH_STENCIL_DESC in {};
        in.DepthEnable = TRUE;
        in.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        in.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        in.StencilEnable = TRUE;
        in.StencilReadMask = 0x3C;
        in.StencilWriteMask = 0xAA;
        in.FrontFace = { D3D11_STENCIL_OP_INCR, D3D11_STENCIL_OP_DECR, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_EQUAL };
        in.BackFace = { D3D11_STENCIL_OP_ZERO, D3D11_STENCIL_OP_INVERT, D3D11_STENCIL_OP_INCR_SAT, D3D11_COMPARISON_NOT_EQUAL };
        const D3D11_DEPTH_STENCIL_DESC out = ab::ShadowDepthStencilDesc(in);
        CHECK(out.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO);
        CHECK_EQ(uint32_t(out.StencilWriteMask), 0u);
        // everything else is the game's
        CHECK(out.DepthEnable == in.DepthEnable && out.DepthFunc == in.DepthFunc && out.StencilEnable == in.StencilEnable);
        CHECK(out.StencilReadMask == in.StencilReadMask);
        CHECK(std::memcmp(&out.FrontFace, &in.FrontFace, sizeof in.FrontFace) == 0 && std::memcmp(&out.BackFace, &in.BackFace, sizeof in.BackFace) == 0);
    }

    CloseCapture();
    diag::detail::g_abTake = nullptr;
    diag::detail::g_abAccept = nullptr;
    diag::detail::g_abImage = nullptr;
    diag::detail::g_abNote = nullptr;
    return Finish("ssr_ab_warp");
}
