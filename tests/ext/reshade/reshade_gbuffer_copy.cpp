// Automatic ReShade G-buffers, the D3D11 half (ReShadeGBufferCopy.cpp, shaders/gbuffer_remap.hlsl) on the WARP software adapter.
// Pure: the view format table (every DXGI_FORMAT value), the request checks, the constants and the motion multiplier. WARP: the
// render-window remap of every supported format family against two independent CPU references (the integer formula, and a scan for the
// texel whose span holds the pixel centre) at several ratios, the 8888 sRGB twins, scaled motion vectors, the cache, the target and view
// lifetime, the game's context state (also with the add-on's view bound in it), the bypass scope, refusals that draw nothing, faults
// that put the game's state back, a swap back that keeps failing, DescribeView on good and garbage handles, the engine routing, and
// the product shader driven directly (clamp, largest edge).
#include "pch.h"

#include "ext/core/HooksBypass.h"
#include "ext/reshade/ReShadeGBufferCopy.h"

#include <GBufferRemap_Ps_Shader.h>
#include <GBufferRemap_Vs_Shader.h>

#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace ext::reshade::depth;

namespace
{
int g_fail = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);                                           \
            ++g_fail;                                                                                                  \
        }                                                                                                              \
    } while (0)

const char* StatusName(RemapStatus s)
{
    switch (s)
    {
    case RemapStatus::Ok:
        return "Ok";
    case RemapStatus::NoContext:
        return "NoContext";
    case RemapStatus::NotPlain2D:
        return "NotPlain2D";
    case RemapStatus::UnsupportedFormat:
        return "UnsupportedFormat";
    case RemapStatus::Refused:
        return "Refused";
    case RemapStatus::Failed:
        return "Failed";
    }
    return "?";
}

// ---- pure: the format table --------------------------------------------------------------------------------------------------
struct FormatRow
{
    DXGI_FORMAT view;
    bool scalable;
    DXGI_FORMAT resource; // UNKNOWN = the format itself
    DXGI_FORMAT twin;
};
const FormatRow kFormatRows[] = {
    { DXGI_FORMAT_R8G8B8A8_UNORM, false, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB },
    { DXGI_FORMAT_B8G8R8A8_UNORM, false, DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB },
    { DXGI_FORMAT_R8G8_UNORM, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R8_UNORM, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R10G10B10A2_UNORM, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R11G11B10_FLOAT, true, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R16G16B16A16_FLOAT, true, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R16G16B16A16_UNORM, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R16G16B16A16_SNORM, true, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R16G16_FLOAT, true, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R16G16_UNORM, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R16G16_SNORM, true, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R16_FLOAT, true, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R16_UNORM, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R32_FLOAT, true, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R32G32_FLOAT, true, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R32G32B32A32_FLOAT, true, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R8G8B8A8_SNORM, true, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
    { DXGI_FORMAT_R8G8_SNORM, true, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
};

bool Listed(int f)
{
    for (const FormatRow& r : kFormatRows)
        if (int(r.view) == f)
            return true;
    return false;
}

void FormatTable()
{
    for (const FormatRow& r : kFormatRows)
    {
        const RemapFormat c = ClassifyRemapFormat(r.view);
        if (!c.supported || c.valueScalable != r.scalable)
            std::printf("  format %d: supported=%d scalable=%d, expected scalable=%d\n", int(r.view), c.supported ? 1 : 0,
                        c.valueScalable ? 1 : 0, r.scalable ? 1 : 0);
        CHECK(c.supported);
        CHECK(c.valueScalable == r.scalable);
        CHECK(c.resourceFormat == (r.resource != DXGI_FORMAT_UNKNOWN ? r.resource : r.view));
        CHECK(c.rtvFormat == r.view); // the render target view and the srv are always the UNORM / float view itself
        CHECK(c.srvFormat == r.view);
        CHECK(c.srgbTwin == r.twin);
    }
    // Nothing else is supported: every other value of the enumeration (and a few beyond it) is refused, with nothing filled in.
    for (int f = 0; f < 256; ++f)
    {
        if (Listed(f))
            continue;
        const RemapFormat c = ClassifyRemapFormat(DXGI_FORMAT(f));
        if (c.supported)
            std::printf("  format %d should not be supported\n", f);
        CHECK(!c.supported);
        CHECK(!c.valueScalable);
        CHECK(c.resourceFormat == DXGI_FORMAT_UNKNOWN && c.rtvFormat == DXGI_FORMAT_UNKNOWN);
        CHECK(c.srvFormat == DXGI_FORMAT_UNKNOWN && c.srgbTwin == DXGI_FORMAT_UNKNOWN);
    }
    // the ones the decision is about, by name
    for (DXGI_FORMAT f : { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_R32_TYPELESS,
                           DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS,
                           DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_BC1_UNORM_SRGB,
                           DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SINT, DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_SINT,
                           DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R8_UINT, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D16_UNORM,
                           DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
                           DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC7_UNORM,
                           DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_A8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM,
                           DXGI_FORMAT_R9G9B9E5_SHAREDEXP, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_B5G6R5_UNORM })
        CHECK(!ClassifyRemapFormat(f).supported);
    CHECK(!ClassifyRemapFormat(DXGI_FORMAT(0xFFFF)).supported);
    CHECK(!ClassifyRemapFormat(DXGI_FORMAT(-1)).supported);
}

// ---- pure: the request check -------------------------------------------------------------------------------------------------
ViewDesc GoodView()
{
    ViewDesc v;
    v.ok = true;
    v.plain2D = true;
    v.width = 64;
    v.height = 48;
    v.format = uint32_t(DXGI_FORMAT_R16G16_FLOAT);
    return v;
}
RemapRequest GoodRequest(GBuffer buffer = GBuffer::MotionVectors)
{
    RemapRequest r;
    r.context = 1;
    r.buffer = buffer;
    r.srv = 2;
    r.renderW = 32;
    r.renderH = 24;
    r.outputW = 64;
    r.outputH = 48;
    r.stamp = 5;
    r.scaleMotion = true;
    return r;
}

void Checks()
{
    static_assert(kMaxEdge == 0x4000); // the D3D11 texture limit; the packing and the shader's 32-bit maths depend on it
    ViewDesc v = GoodView();
    RemapRequest r = GoodRequest();
    CHECK(CheckRemap(v, r) == RemapStatus::Ok);

    // the view: readable, plain 2D
    v = GoodView();
    v.plain2D = false;
    CHECK(CheckRemap(v, r) == RemapStatus::NotPlain2D);
    v = GoodView();
    v.ok = false;
    CHECK(CheckRemap(v, r) == RemapStatus::NotPlain2D);

    // the format: supported, and for scaled motion vectors one that can hold scaled values
    v = GoodView();
    for (DXGI_FORMAT f : { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R32_UINT,
                           DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_BC1_UNORM })
    {
        v.format = uint32_t(f);
        CHECK(CheckRemap(v, r) == RemapStatus::UnsupportedFormat);
        CHECK(CheckRemap(v, GoodRequest(GBuffer::Normals)) == RemapStatus::UnsupportedFormat);
    }
    for (const FormatRow& row : kFormatRows)
    {
        v.format = uint32_t(row.view);
        r = GoodRequest(GBuffer::MotionVectors);
        r.scaleMotion = true;
        CHECK(CheckRemap(v, r) == (row.scalable ? RemapStatus::Ok : RemapStatus::UnsupportedFormat));
        r.scaleMotion = false; // not scaled: any supported format is fine
        CHECK(CheckRemap(v, r) == RemapStatus::Ok);
        for (GBuffer other : { GBuffer::Normals, GBuffer::Decals, GBuffer::DecalsWater, GBuffer::Material })
        {
            r = GoodRequest(other);
            r.scaleMotion = true; // other buffers never scale: the flag does not matter
            CHECK(CheckRemap(v, r) == RemapStatus::Ok);
        }
    }

    // sizes: 1 .. kMaxEdge on the source, the render window and the output
    const auto size = [](uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, uint32_t sw, uint32_t sh)
    {
        ViewDesc view = GoodView();
        view.width = sw;
        view.height = sh;
        RemapRequest req = GoodRequest(GBuffer::Normals);
        req.renderW = rw;
        req.renderH = rh;
        req.outputW = ow;
        req.outputH = oh;
        return CheckRemap(view, req);
    };
    CHECK(size(32, 24, 64, 48, 64, 48) == RemapStatus::Ok);
    CHECK(size(0, 24, 64, 48, 64, 48) == RemapStatus::Refused);
    CHECK(size(32, 0, 64, 48, 64, 48) == RemapStatus::Refused);
    CHECK(size(32, 24, 0, 48, 64, 48) == RemapStatus::Refused);
    CHECK(size(32, 24, 64, 0, 64, 48) == RemapStatus::Refused);
    CHECK(size(32, 24, 64, 48, 0, 48) == RemapStatus::Refused);
    CHECK(size(32, 24, 64, 48, 64, 0) == RemapStatus::Refused);
    CHECK(size(1, 1, 1, 1, 1, 1) == RemapStatus::Ok);
    CHECK(size(kMaxEdge, kMaxEdge, kMaxEdge, kMaxEdge, kMaxEdge, kMaxEdge) == RemapStatus::Ok);
    for (uint32_t bad : { kMaxEdge + 1, 0xFFFFFFFFu })
    {
        CHECK(size(bad, 24, kMaxEdge, 48, kMaxEdge, 48) == RemapStatus::Refused);
        CHECK(size(32, bad, 64, kMaxEdge, 64, kMaxEdge) == RemapStatus::Refused);
        CHECK(size(32, 24, bad, 48, 64, 48) == RemapStatus::Refused);
        CHECK(size(32, 24, 64, bad, 64, 48) == RemapStatus::Refused);
        CHECK(size(32, 24, 64, 48, bad, 48) == RemapStatus::Refused);
        CHECK(size(32, 24, 64, 48, 64, bad) == RemapStatus::Refused);
    }
    // the render window lies inside the source (origin 0,0), on each axis separately
    CHECK(size(64, 48, 64, 48, 64, 48) == RemapStatus::Ok);
    CHECK(size(65, 24, 80, 48, 64, 48) == RemapStatus::Refused);
    CHECK(size(32, 49, 64, 64, 64, 48) == RemapStatus::Refused);
    CHECK(size(32, 24, 64, 48, 32, 24) == RemapStatus::Ok);   // a render-sized source
    CHECK(size(32, 24, 64, 48, 31, 24) == RemapStatus::Refused);
    CHECK(size(32, 24, 64, 48, 32, 23) == RemapStatus::Refused);
    CHECK(size(32, 24, 40, 30, 100, 80) == RemapStatus::Ok);  // a source larger than the output
    // the render window is not larger than the output, on each axis separately
    CHECK(size(64, 24, 64, 48, 64, 48) == RemapStatus::Ok);
    CHECK(size(48, 24, 40, 48, 64, 48) == RemapStatus::Refused);
    CHECK(size(32, 40, 64, 30, 64, 48) == RemapStatus::Refused);
    CHECK(size(33, 24, 32, 48, 64, 48) == RemapStatus::Refused);

    // the order of the checks is part of the contract: the view first, then the format, then the sizes
    v = GoodView();
    v.plain2D = false;
    v.format = uint32_t(DXGI_FORMAT_R32_UINT);
    r = GoodRequest();
    r.renderW = 0;
    CHECK(CheckRemap(v, r) == RemapStatus::NotPlain2D);
    v.plain2D = true;
    CHECK(CheckRemap(v, r) == RemapStatus::UnsupportedFormat);
    v.format = uint32_t(DXGI_FORMAT_R16G16_FLOAT);
    CHECK(CheckRemap(v, r) == RemapStatus::Refused);
    v.ok = false;
    v.format = uint32_t(DXGI_FORMAT_R32_UINT);
    CHECK(CheckRemap(v, r) == RemapStatus::NotPlain2D);
    // scaled motion vectors on UNORM with bad sizes: the format wins
    v = GoodView();
    v.format = uint32_t(DXGI_FORMAT_R16G16_UNORM);
    r = GoodRequest();
    r.outputW = 0;
    CHECK(CheckRemap(v, r) == RemapStatus::UnsupportedFormat);
}

void Constants()
{
    // the multiplier: only scaled motion vectors, per axis
    ValueMultiplier m = MotionMultiplier(GBuffer::MotionVectors, true, 3840, 2160, 1920, 1080);
    CHECK(m.x == 2.0f && m.y == 2.0f);
    m = MotionMultiplier(GBuffer::MotionVectors, true, 3840, 2160, 2560, 1440);
    CHECK(m.x == 1.5f && m.y == 1.5f);
    m = MotionMultiplier(GBuffer::MotionVectors, true, 16, 12, 8, 8); // anisotropic: x and y differ, so a swapped axis is visible
    CHECK(m.x == 2.0f && m.y == 1.5f);
    m = MotionMultiplier(GBuffer::MotionVectors, true, 8, 12, 8, 6);
    CHECK(m.x == 1.0f && m.y == 2.0f);
    m = MotionMultiplier(GBuffer::MotionVectors, true, 3840, 2160, 3840, 2160);
    CHECK(m.x == 1.0f && m.y == 1.0f);
    m = MotionMultiplier(GBuffer::MotionVectors, true, 3840, 2160, 2258, 1270);
    CHECK(m.x == float(3840) / float(2258) && m.y == float(2160) / float(1270));
    m = MotionMultiplier(GBuffer::MotionVectors, false, 3840, 2160, 1920, 1080);
    CHECK(m.x == 1.0f && m.y == 1.0f);
    for (GBuffer b : { GBuffer::Normals, GBuffer::Decals, GBuffer::DecalsWater, GBuffer::Material })
        for (bool scale : { false, true })
        {
            m = MotionMultiplier(b, scale, 3840, 2160, 1920, 1080);
            CHECK(m.x == 1.0f && m.y == 1.0f); // never, whatever the flag says
        }
    m = MotionMultiplier(GBuffer::MotionVectors, true, 3840, 2160, 0, 0); // a render size of 0 is not divided by
    CHECK(m.x == 1.0f && m.y == 1.0f);
    m = MotionMultiplier(GBuffer::MotionVectors, true, 3840, 2160, 0, 1080);
    CHECK(m.x == 1.0f && m.y == 2.0f);

    // the constant buffer: the render pair, the output pair (x in the low 16 bits), the multiplier
    static_assert(offsetof(RemapConstants, render) == 0 && offsetof(RemapConstants, output) == 4);
    static_assert(offsetof(RemapConstants, mulX) == 8 && offsetof(RemapConstants, mulY) == 12);
    RemapRequest r = GoodRequest(GBuffer::MotionVectors);
    r.renderW = 32;
    r.renderH = 24;
    r.outputW = 64;
    r.outputH = 48;
    RemapConstants k = MakeRemapConstants(r);
    CHECK(k.render == (32u | (24u << 16)));
    CHECK(k.output == (64u | (48u << 16)));
    CHECK(k.mulX == 2.0f && k.mulY == 2.0f);
    r.scaleMotion = false;
    k = MakeRemapConstants(r);
    CHECK(k.mulX == 1.0f && k.mulY == 1.0f);
    r = GoodRequest(GBuffer::Normals);
    k = MakeRemapConstants(r);
    CHECK(k.mulX == 1.0f && k.mulY == 1.0f);
    r = GoodRequest(GBuffer::MotionVectors);
    r.renderW = 0x4000;
    r.renderH = 1;
    r.outputW = 0x4000;
    r.outputH = 0x4000;
    k = MakeRemapConstants(r);
    CHECK(k.render == 0x00014000u);
    CHECK(k.output == 0x40004000u);
    CHECK(k.mulX == 1.0f && k.mulY == 16384.0f);
}

void Replaced()
{
    int a = 0, b = 0;
    CHECK(!RemapDeviceReplaced(&a, &a, S_OK));
    CHECK(RemapDeviceReplaced(&a, &b, S_OK));
    CHECK(RemapDeviceReplaced(nullptr, &a, S_OK)); // nothing set up yet
    CHECK(RemapDeviceReplaced(nullptr, nullptr, S_OK));
    CHECK(!RemapDeviceReplaced(&a, &a, S_FALSE)); // a success code that is not S_OK
    CHECK(RemapDeviceReplaced(&a, &a, DXGI_ERROR_DEVICE_REMOVED));
    CHECK(RemapDeviceReplaced(&a, &a, DXGI_ERROR_DEVICE_HUNG));
    CHECK(RemapDeviceReplaced(&a, &a, E_FAIL));
    CHECK(RemapDeviceReplaced(&a, nullptr, S_OK));
}

// ---- WARP helpers --------------------------------------------------------------------------------------------------------
struct Rig
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    bool Create(UINT flags = 0, D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0)
    {
        D3D_FEATURE_LEVEL got {};
        return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, &level, 1, D3D11_SDK_VERSION, &device, &got,
                                           &context));
    }
};

ULONG RefCount(IUnknown* p)
{
    p->AddRef();
    return p->Release();
}

struct DrawProbe
{
    int draws = 0;
    int mode = 0; // 0 normal, 1 C++ exception, 2 access violation, 3 normal + the unbind step will crash
    bool sawBypass = false;
    IUnknown* watch = nullptr; // its reference count is read inside the draw
    ULONG watchRefs = 0;
};
DrawProbe g_draw;
void InstallUnbindCrashes(ID3D11DeviceContext* c); // below, with the other hooks
void TestDraw(ID3D11DeviceContext* c)
{
    ++g_draw.draws;
    g_draw.sawBypass = ext::core::HooksBypassed();
    if (g_draw.watch != nullptr)
        g_draw.watchRefs = RefCount(g_draw.watch);
    if (g_draw.mode == 1)
        throw std::runtime_error("draw");
    if (g_draw.mode == 2)
    {
        volatile int* null = nullptr;
        *null = 1;
    }
    c->Draw(3, 0);
    if (g_draw.mode == 3)
        InstallUnbindCrashes(c); // from here on, unbinding in the private state crashes
}

// ---- the game's context state: a dump of everything a draw could change, and a game-like pipeline state ----------------------
using Dump = std::vector<std::pair<std::string, uint64_t>>;
template <class T> uint64_t Take(T* p)
{
    const uint64_t v = reinterpret_cast<uintptr_t>(p);
    if (p != nullptr)
        p->Release();
    return v;
}
uint64_t Bits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
Dump DumpState(ID3D11DeviceContext* c)
{
    Dump d;
    auto add = [&](const std::string& n, uint64_t v) { d.emplace_back(n, v); };
    D3D11_PRIMITIVE_TOPOLOGY topology {};
    c->IAGetPrimitiveTopology(&topology);
    add("topology", uint64_t(topology));
    ID3D11InputLayout* layout = nullptr;
    c->IAGetInputLayout(&layout);
    add("layout", Take(layout));
    ID3D11Buffer* vb[2] {};
    UINT strides[2] {}, offsets[2] {};
    c->IAGetVertexBuffers(0, 2, vb, strides, offsets);
    for (int i = 0; i < 2; ++i)
    {
        add("vb" + std::to_string(i), Take(vb[i]));
        add("vbStride" + std::to_string(i), strides[i]);
        add("vbOffset" + std::to_string(i), offsets[i]);
    }
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ibFormat {};
    UINT ibOffset = 0;
    c->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
    add("ib", Take(ib));
    add("ibFormat", uint64_t(ibFormat));
    add("ibOffset", ibOffset);

    ID3D11VertexShader* vs = nullptr;
    c->VSGetShader(&vs, nullptr, nullptr);
    add("vs", Take(vs));
    ID3D11PixelShader* ps = nullptr;
    c->PSGetShader(&ps, nullptr, nullptr);
    add("ps", Take(ps));
    ID3D11GeometryShader* gs = nullptr;
    c->GSGetShader(&gs, nullptr, nullptr);
    add("gs", Take(gs));
    ID3D11ComputeShader* cs = nullptr;
    c->CSGetShader(&cs, nullptr, nullptr);
    add("cs", Take(cs));

    ID3D11Buffer* cb[4] {};
    c->VSGetConstantBuffers(0, 4, cb);
    for (int i = 0; i < 4; ++i)
        add("vsCb" + std::to_string(i), Take(cb[i]));
    c->PSGetConstantBuffers(0, 4, cb);
    for (int i = 0; i < 4; ++i)
        add("psCb" + std::to_string(i), Take(cb[i]));
    ID3D11ShaderResourceView* srv[4] {};
    c->VSGetShaderResources(0, 4, srv);
    for (int i = 0; i < 4; ++i)
        add("vsSrv" + std::to_string(i), Take(srv[i]));
    c->PSGetShaderResources(0, 4, srv);
    for (int i = 0; i < 4; ++i)
        add("psSrv" + std::to_string(i), Take(srv[i]));
    ID3D11SamplerState* smp[3] {};
    c->PSGetSamplers(0, 3, smp);
    for (int i = 0; i < 3; ++i)
        add("psSampler" + std::to_string(i), Take(smp[i]));

    UINT n = 2;
    D3D11_VIEWPORT vp[2] {};
    c->RSGetViewports(&n, vp);
    add("viewportCount", n);
    for (UINT i = 0; i < n && i < 2; ++i)
    {
        add("vpX" + std::to_string(i), Bits(vp[i].TopLeftX));
        add("vpY" + std::to_string(i), Bits(vp[i].TopLeftY));
        add("vpW" + std::to_string(i), Bits(vp[i].Width));
        add("vpH" + std::to_string(i), Bits(vp[i].Height));
        add("vpMin" + std::to_string(i), Bits(vp[i].MinDepth));
        add("vpMax" + std::to_string(i), Bits(vp[i].MaxDepth));
    }
    n = 2;
    D3D11_RECT sc[2] {};
    c->RSGetScissorRects(&n, sc);
    add("scissorCount", n);
    for (UINT i = 0; i < n && i < 2; ++i)
    {
        add("scL" + std::to_string(i), uint64_t(sc[i].left));
        add("scT" + std::to_string(i), uint64_t(sc[i].top));
        add("scR" + std::to_string(i), uint64_t(sc[i].right));
        add("scB" + std::to_string(i), uint64_t(sc[i].bottom));
    }
    ID3D11RasterizerState* rs = nullptr;
    c->RSGetState(&rs);
    add("rasterizer", Take(rs));
    ID3D11BlendState* bs = nullptr;
    float factor[4] {};
    UINT mask = 0;
    c->OMGetBlendState(&bs, factor, &mask);
    add("blend", Take(bs));
    add("blendMask", mask);
    for (int i = 0; i < 4; ++i)
        add("blendFactor" + std::to_string(i), Bits(factor[i]));
    ID3D11DepthStencilState* ds = nullptr;
    UINT ref = 0;
    c->OMGetDepthStencilState(&ds, &ref);
    add("depthStencil", Take(ds));
    add("stencilRef", ref);
    ID3D11RenderTargetView* rtv[3] {};
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(3, rtv, &dsv);
    for (int i = 0; i < 3; ++i)
        add("rtv" + std::to_string(i), Take(rtv[i]));
    add("dsv", Take(dsv));
    // predication is part of the state; open queries are not (nothing to read back: the header documents what an open occlusion query counts)
    ID3D11Predicate* predicate = nullptr;
    BOOL predicateValue = FALSE;
    c->GetPredication(&predicate, &predicateValue);
    add("predicate", Take(predicate));
    add("predicateValue", uint64_t(predicateValue));
    return d;
}
bool SameState(const char* what, const Dump& a, const Dump& b)
{
    bool same = a.size() == b.size();
    for (size_t i = 0; same && i < a.size(); ++i)
        if (a[i] != b[i])
        {
            std::printf("  %s: %s changed (%llx -> %llx)\n", what, a[i].first.c_str(), (unsigned long long) a[i].second,
                        (unsigned long long) b[i].second);
            same = false;
        }
    return same;
}

// A game-like pipeline state: shaders, buffers, views, states, viewport, scissor, targets and a non-default topology
struct GameState
{
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb, vb, ib;
    ComPtr<ID3D11Texture2D> srvTex, rtTex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;

    bool Set(Rig& rig)
    {
        ID3D11Device* d = rig.device.Get();
        ID3D11DeviceContext* c = rig.context.Get();
        bool ok = SUCCEEDED(d->CreateVertexShader(GBufferRemap_Vs_cso, sizeof(GBufferRemap_Vs_cso), nullptr, &vs)) &&
                  SUCCEEDED(d->CreatePixelShader(GBufferRemap_Ps_cso, sizeof(GBufferRemap_Ps_cso), nullptr, &ps));
        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth = 64;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ok = ok && SUCCEEDED(d->CreateBuffer(&bd, nullptr, &cb));
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        ok = ok && SUCCEEDED(d->CreateBuffer(&bd, nullptr, &vb));
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        ok = ok && SUCCEEDED(d->CreateBuffer(&bd, nullptr, &ib));
        D3D11_TEXTURE2D_DESC td {};
        td.Width = td.Height = 16;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ok = ok && SUCCEEDED(d->CreateTexture2D(&td, nullptr, &srvTex)) && SUCCEEDED(d->CreateShaderResourceView(srvTex.Get(), nullptr, &srv));
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ok = ok && SUCCEEDED(d->CreateTexture2D(&td, nullptr, &rtTex)) && SUCCEEDED(d->CreateRenderTargetView(rtTex.Get(), nullptr, &rtv));
        D3D11_SAMPLER_DESC sd {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        ok = ok && SUCCEEDED(d->CreateSamplerState(&sd, &sampler));
        D3D11_RASTERIZER_DESC rd {};
        rd.FillMode = D3D11_FILL_WIREFRAME;
        rd.CullMode = D3D11_CULL_FRONT;
        rd.ScissorEnable = TRUE;
        ok = ok && SUCCEEDED(d->CreateRasterizerState(&rd, &raster));
        D3D11_BLEND_DESC bld {};
        bld.RenderTarget[0].BlendEnable = TRUE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
        ok = ok && SUCCEEDED(d->CreateBlendState(&bld, &blend));
        D3D11_DEPTH_STENCIL_DESC dd {};
        dd.DepthEnable = TRUE;
        dd.DepthFunc = D3D11_COMPARISON_GREATER;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ok = ok && SUCCEEDED(d->CreateDepthStencilState(&dd, &depth));
        if (!ok)
            return false;

        ID3D11Buffer* cbs[1] = { cb.Get() };
        ID3D11Buffer* vbs[1] = { vb.Get() };
        const UINT stride = 16, offset = 8;
        ID3D11ShaderResourceView* srvs[1] = { srv.Get() };
        ID3D11SamplerState* samplers[1] = { sampler.Get() };
        ID3D11RenderTargetView* rtvs[1] = { rtv.Get() };
        const D3D11_VIEWPORT vp { 1.0f, 2.0f, 11.0f, 12.0f, 0.1f, 0.9f };
        const D3D11_RECT scissor { 1, 2, 9, 10 };
        const float factor[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        c->IASetVertexBuffers(1, 1, vbs, &stride, &offset);
        c->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 4);
        c->VSSetShader(vs.Get(), nullptr, 0);
        c->PSSetShader(ps.Get(), nullptr, 0);
        c->VSSetConstantBuffers(1, 1, cbs);
        c->PSSetConstantBuffers(2, 1, cbs);
        c->VSSetShaderResources(2, 1, srvs);
        c->PSSetShaderResources(1, 1, srvs);
        c->PSSetSamplers(1, 1, samplers);
        c->RSSetViewports(1, &vp);
        c->RSSetScissorRects(1, &scissor);
        c->RSSetState(raster.Get());
        c->OMSetBlendState(blend.Get(), factor, 0xFF0F);
        c->OMSetDepthStencilState(depth.Get(), 5);
        c->OMSetRenderTargets(1, rtvs, nullptr);
        return true;
    }
};

// ---- a canary on a COM object's destruction -------------------------------------------------------------------------------
struct Canary final : IUnknown
{
    std::atomic<ULONG> refs { 1 };
    bool* destroyed;
    explicit Canary(bool* flag) : destroyed(flag) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = --refs;
        if (n == 0)
        {
            *destroyed = true;
            delete this;
        }
        return n;
    }
};

// ---- vtable hooks --------------------------------------------------------------------------------------------------------
// The D3D11 runtime's objects share one method table per class, so a patch reaches every device and context of the process. Each
// patch is removed again, and only if the slot still holds the hook (a context-state swap rewrites the context's table).
// Slot numbers: ID3D11Device (CreateShaderResourceView 7, CreateVertexShader 12, GetDeviceRemovedReason 39), ID3D11DeviceContext
// (Draw 13, OMSetRenderTargets 33), ID3D11DeviceContext1 (SwapDeviceContextState 131). The tests check that their hook was reached.
constexpr int kDevCreateSrv = 7, kDevCreateVs = 12, kDevRemovedReason = 39;
constexpr int kCtxOmSetRenderTargets = 33, kCtx1Swap = 131;

struct SlotPatch
{
    void** slot = nullptr;
    void* original = nullptr;
    void* hook = nullptr;
    void Install(void* object, int index, void* fn)
    {
        Remove();
        void** table = *reinterpret_cast<void***>(object);
        slot = &table[index];
        DWORD old = 0;
        VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
        original = *slot;
        hook = fn;
        *slot = fn;
        VirtualProtect(slot, sizeof(void*), old, &old);
    }
    void Remove()
    {
        if (slot != nullptr && *slot == hook)
        {
            DWORD old = 0;
            VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
            *slot = original;
            VirtualProtect(slot, sizeof(void*), old, &old);
        }
        slot = nullptr;
    }
    ~SlotPatch() { Remove(); }
    template <class T> T Original() const { return reinterpret_cast<T>(original); }
};

[[noreturn]] void CrashNow()
{
    volatile int* null = nullptr;
    *null = 1;
    std::abort();
}

// OMSetRenderTargets that crashes (installed from inside the draw, i.e. inside the private state): the unbind step has no ClearState
SlotPatch g_patchOm;
void STDMETHODCALLTYPE HkOmCrash(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*) { CrashNow(); }
void InstallUnbindCrashes(ID3D11DeviceContext* c)
{
    g_patchOm.Install(c, kCtxOmSetRenderTargets, reinterpret_cast<void*>(&HkOmCrash));
}

// SwapDeviceContextState: counts the calls and misbehaves on one of them
enum class SwapFault { None, CrashBefore, CrashAfter, NullPreviousNoSwap, NullPreviousSwapped };
struct SwapHook
{
    SlotPatch patch;
    int calls = 0;
    int faultCall = 0; // 1-based; 0 = never
    bool persistent = false; // every call from faultCall on misbehaves, not just that one
    std::vector<int> alsoFaultAt; // more single calls (1-based) that misbehave
    ID3D11DeviceContext1* onlyContext = nullptr; // misbehave for this context only (null = every context)
    SwapFault fault = SwapFault::None;
    std::vector<ID3DDeviceContextState*> targets;  // the state asked for, per call
    std::vector<ID3DDeviceContextState*> previous; // the previous state returned, per call
    std::vector<bool> bypassed;                    // the bypass scope was open, per call
    ID3DDeviceContextState* lost = nullptr;        // NullPreviousSwapped: the game's state whose reference was dropped
};
SwapHook g_swap;
using SwapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3DDeviceContextState*, ID3DDeviceContextState**);
void STDMETHODCALLTYPE HkSwap(ID3D11DeviceContext1* c, ID3DDeviceContextState* s, ID3DDeviceContextState** p)
{
    const int call = ++g_swap.calls;
    g_swap.targets.push_back(s);
    g_swap.bypassed.push_back(ext::core::HooksBypassed());
    const bool mine = g_swap.onlyContext == nullptr || c == g_swap.onlyContext;
    bool faulty = mine && g_swap.faultCall != 0 && (g_swap.persistent ? call >= g_swap.faultCall : call == g_swap.faultCall);
    for (int extra : g_swap.alsoFaultAt)
        faulty = faulty || (mine && call == extra);
    if (faulty && g_swap.fault == SwapFault::CrashBefore)
        CrashNow();
    if (faulty && g_swap.fault == SwapFault::NullPreviousNoSwap)
    {
        if (p != nullptr)
            *p = nullptr;
        g_swap.previous.push_back(nullptr);
        return;
    }
    g_swap.patch.Original<SwapFn>()(c, s, p);
    if (faulty && g_swap.fault == SwapFault::NullPreviousSwapped && p != nullptr)
    {
        g_swap.lost = *p; // the reference stays with the test: the swap happened, the caller is told nothing
        *p = nullptr;
    }
    g_swap.previous.push_back(p != nullptr ? *p : nullptr);
    if (faulty && g_swap.fault == SwapFault::CrashAfter)
        CrashNow();
}
void InstallSwapHook(Rig& rig, int faultCall, SwapFault fault, bool persistent = false, bool onlyThisContext = false)
{
    ComPtr<ID3D11DeviceContext1> c1;
    rig.context.As(&c1);
    g_swap.calls = 0;
    g_swap.faultCall = faultCall;
    g_swap.persistent = persistent;
    g_swap.alsoFaultAt.clear();
    g_swap.onlyContext = onlyThisContext ? c1.Get() : nullptr;
    g_swap.fault = fault;
    g_swap.targets.clear();
    g_swap.previous.clear();
    g_swap.bypassed.clear();
    g_swap.lost = nullptr;
    g_swap.patch.Install(c1.Get(), kCtx1Swap, reinterpret_cast<void*>(&HkSwap));
}

// Device calls: what the hooks see while a Remap runs
enum class DeviceFault { None, SrvCrash, VertexShaderFails, DeviceRemoved };
struct DeviceHook
{
    SlotPatch srv, vs, removed;
    bool active = false; // a Remap is running
    DeviceFault fault = DeviceFault::None;
    int srvCalls = 0, srvBypassed = 0, removedCalls = 0, vsCalls = 0;
};
DeviceHook g_dev;
using SrvFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
using VsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using RemovedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*);
HRESULT STDMETHODCALLTYPE HkCreateSrv(ID3D11Device* d, ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
                                      ID3D11ShaderResourceView** out)
{
    if (g_dev.active)
    {
        ++g_dev.srvCalls;
        g_dev.srvBypassed += ext::core::HooksBypassed() ? 1 : 0;
        if (g_dev.fault == DeviceFault::SrvCrash)
            CrashNow();
    }
    return g_dev.srv.Original<SrvFn>()(d, r, desc, out);
}
HRESULT STDMETHODCALLTYPE HkCreateVs(ID3D11Device* d, const void* code, SIZE_T size, ID3D11ClassLinkage* link, ID3D11VertexShader** out)
{
    if (g_dev.active)
    {
        ++g_dev.vsCalls;
        if (g_dev.fault == DeviceFault::VertexShaderFails)
            return E_FAIL;
    }
    return g_dev.vs.Original<VsFn>()(d, code, size, link, out);
}
HRESULT STDMETHODCALLTYPE HkRemoved(ID3D11Device* d)
{
    if (g_dev.active)
    {
        ++g_dev.removedCalls;
        if (g_dev.fault == DeviceFault::DeviceRemoved)
            return DXGI_ERROR_DEVICE_REMOVED;
    }
    return g_dev.removed.Original<RemovedFn>()(d);
}
void InstallDeviceHooks(Rig& rig)
{
    g_dev = DeviceHook {};
    g_dev.srv.Install(rig.device.Get(), kDevCreateSrv, reinterpret_cast<void*>(&HkCreateSrv));
    g_dev.vs.Install(rig.device.Get(), kDevCreateVs, reinterpret_cast<void*>(&HkCreateVs));
    g_dev.removed.Install(rig.device.Get(), kDevRemovedReason, reinterpret_cast<void*>(&HkRemoved));
}
void RemoveDeviceHooks()
{
    g_dev.srv.Remove();
    g_dev.vs.Remove();
    g_dev.removed.Remove();
    g_dev.active = false;
    g_dev.fault = DeviceFault::None;
}

// ---- sources, formats and their texel patterns ------------------------------------------------------------------------------
enum class Cls { U8, S8, U16, S16, F16, F32, U10, F11 };
struct Fmt
{
    const char* name;
    DXGI_FORMAT view;
    UINT channels;
    UINT bytes;
    Cls cls;
};
const Fmt kFmts[] = {
    { "R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, Cls::U8 },
    { "B8G8R8A8_UNORM", DXGI_FORMAT_B8G8R8A8_UNORM, 4, 4, Cls::U8 },
    { "R8G8_UNORM", DXGI_FORMAT_R8G8_UNORM, 2, 2, Cls::U8 },
    { "R8_UNORM", DXGI_FORMAT_R8_UNORM, 1, 1, Cls::U8 },
    { "R10G10B10A2_UNORM", DXGI_FORMAT_R10G10B10A2_UNORM, 4, 4, Cls::U10 },
    { "R11G11B10_FLOAT", DXGI_FORMAT_R11G11B10_FLOAT, 3, 4, Cls::F11 },
    { "R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT, 4, 8, Cls::F16 },
    { "R16G16B16A16_UNORM", DXGI_FORMAT_R16G16B16A16_UNORM, 4, 8, Cls::U16 },
    { "R16G16B16A16_SNORM", DXGI_FORMAT_R16G16B16A16_SNORM, 4, 8, Cls::S16 },
    { "R16G16_FLOAT", DXGI_FORMAT_R16G16_FLOAT, 2, 4, Cls::F16 },
    { "R16G16_UNORM", DXGI_FORMAT_R16G16_UNORM, 2, 4, Cls::U16 },
    { "R16G16_SNORM", DXGI_FORMAT_R16G16_SNORM, 2, 4, Cls::S16 },
    { "R16_FLOAT", DXGI_FORMAT_R16_FLOAT, 1, 2, Cls::F16 },
    { "R16_UNORM", DXGI_FORMAT_R16_UNORM, 1, 2, Cls::U16 },
    { "R32_FLOAT", DXGI_FORMAT_R32_FLOAT, 1, 4, Cls::F32 },
    { "R32G32_FLOAT", DXGI_FORMAT_R32G32_FLOAT, 2, 8, Cls::F32 },
    { "R32G32B32A32_FLOAT", DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 16, Cls::F32 },
    { "R8G8B8A8_SNORM", DXGI_FORMAT_R8G8B8A8_SNORM, 4, 4, Cls::S8 },
    { "R8G8_SNORM", DXGI_FORMAT_R8G8_SNORM, 2, 2, Cls::S8 },
};
const Fmt* FindFmt(DXGI_FORMAT view)
{
    for (const Fmt& f : kFmts)
        if (f.view == view)
            return &f;
    return nullptr;
}
bool HasTwin(const Fmt& f) { return f.view == DXGI_FORMAT_R8G8B8A8_UNORM || f.view == DXGI_FORMAT_B8G8R8A8_UNORM; }
// The texture an add-on makes for a view format: the 8888 UNORM views sit on a typeless parent (so the sRGB twin can exist)
DXGI_FORMAT ResourceOf(const Fmt& f)
{
    if (f.view == DXGI_FORMAT_R8G8B8A8_UNORM)
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if (f.view == DXGI_FORMAT_B8G8R8A8_UNORM)
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    return f.view;
}
DXGI_FORMAT SrgbOf(const Fmt& f)
{
    if (f.view == DXGI_FORMAT_R8G8B8A8_UNORM)
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (f.view == DXGI_FORMAT_B8G8R8A8_UNORM)
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    return DXGI_FORMAT_UNKNOWN;
}
bool IsMotionClass(const Fmt& f) { return f.cls == Cls::F16 || f.cls == Cls::F32 || f.cls == Cls::S16 || f.cls == Cls::S8; }

// Float values that are exact in half precision: small multiples of 1/32 (and a sign)
uint16_t ToHalf(float f)
{
    if (f == 0.0f)
        return 0;
    const uint16_t sign = f < 0.0f ? uint16_t(0x8000) : uint16_t(0);
    const float a = std::fabs(f);
    uint32_t u = 0;
    std::memcpy(&u, &a, 4);
    const int e = int((u >> 23) & 0xFF) - 127 + 15;
    return uint16_t(sign | (e << 10) | ((u >> 13) & 0x3FF));
}

// Every texel of the render window has its own value, and so has every channel: idx = y * renderW + x + 1 (at most 200 here, so even an
// 8-bit channel 0 is unique). SNORM never uses the most negative code, which reads back as the one above it.
void PatternTexel(const Fmt& f, uint32_t idx, uint8_t* out)
{
    std::memset(out, 0, f.bytes);
    switch (f.cls)
    {
    case Cls::U8:
    case Cls::S8:
        for (UINT c = 0; c < f.channels; ++c)
        {
            uint32_t v = c == 0 ? idx : (idx * (c * 6 + 1) + c * 41) & 0xFF;
            if (f.cls == Cls::S8)
            {
                if (c == 0)
                    v += v >= 128 ? 1 : 0;
                else if (v == 0x80)
                    v = 0x81;
            }
            out[c] = uint8_t(v);
        }
        break;
    case Cls::U16:
    case Cls::S16:
        for (UINT c = 0; c < f.channels; ++c)
        {
            uint32_t v = c == 0 ? idx * 37 + 1 : (idx * (c * 11 + 3) + c * 1009) & 0xFFFF;
            if (f.cls == Cls::S16 && v == 0x8000)
                v = 0x8001;
            const uint16_t h = uint16_t(v);
            std::memcpy(out + 2 * c, &h, 2);
        }
        break;
    case Cls::F16:
        for (UINT c = 0; c < f.channels; ++c)
        {
            const uint16_t h = ToHalf(float((idx * (c + 1) + c * 7) % 1500 + 1) / 32.0f);
            std::memcpy(out + 2 * c, &h, 2);
        }
        break;
    case Cls::F32:
        for (UINT c = 0; c < f.channels; ++c)
        {
            const float v = float((idx * (c + 1) + c * 7) % 5000 + 1) / 64.0f;
            std::memcpy(out + 4 * c, &v, 4);
        }
        break;
    case Cls::U10:
    {
        const uint32_t u = (idx & 0x3FF) | (((idx * 5 + 7) & 0x3FF) << 10) | (((idx * 11 + 3) & 0x3FF) << 20) | ((idx & 3) << 30);
        std::memcpy(out, &u, 4);
        break;
    }
    case Cls::F11: // finite, normal, non-negative 11 / 11 / 10 bit floats
    {
        const uint32_t u = (64 + (idx * 7) % 1800) | ((64 + (idx * 13 + 5) % 1800) << 11) | ((32 + (idx * 3) % 900) << 22);
        std::memcpy(out, &u, 4);
        break;
    }
    }
}

// Motion vectors: small EVEN integers per channel (a multiplier of 1, 1.5 or 2 keeps them integral), stored as n / 32 in the float
// formats and as the integer code itself in the SNORM ones, so the scaled value is exact in every format.
void MotionInts(uint32_t idx, int n[4])
{
    n[0] = 2 * int(1 + idx % 29) * (idx % 2 != 0 ? -1 : 1);
    n[1] = 2 * int(1 + (idx * 3) % 31) * (idx % 3 == 0 ? -1 : 1);
    n[2] = 2 * int(1 + idx % 7);
    n[3] = 2 * int(1 + idx % 5);
}
void MotionTexel(const Fmt& f, uint32_t idx, float mulX, float mulY, uint8_t* out)
{
    std::memset(out, 0, f.bytes);
    int n[4] {};
    MotionInts(idx, n);
    for (UINT c = 0; c < f.channels; ++c)
    {
        const float m = c == 0 ? mulX : (c == 1 ? mulY : 1.0f);
        const int v = int(std::lround(float(n[c]) * m)); // exact: n is even, m is 1, 1.5 or 2
        switch (f.cls)
        {
        case Cls::F16:
        {
            const uint16_t h = ToHalf(float(v) / 32.0f);
            std::memcpy(out + 2 * c, &h, 2);
            break;
        }
        case Cls::F32:
        {
            const float x = float(v) / 32.0f;
            std::memcpy(out + 4 * c, &x, 4);
            break;
        }
        case Cls::S16:
        {
            const int16_t s = int16_t(v);
            std::memcpy(out + 2 * c, &s, 2);
            break;
        }
        case Cls::S8:
            out[c] = uint8_t(int8_t(v));
            break;
        default:
            break;
        }
    }
}

struct Source
{
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv, srvSrgb;
    UINT w = 0, h = 0;
};
using TexelFn = std::function<void(uint32_t x, uint32_t y, uint8_t* out)>;

// An add-on's G-buffer: w x h, the picture only in the top-left rw x rh window, zero elsewhere
Source MakeSource(Rig& rig, const Fmt& f, UINT w, UINT h, UINT rw, UINT rh, const TexelFn& fill)
{
    Source s;
    s.w = w;
    s.h = h;
    std::vector<uint8_t> data(size_t(w) * h * f.bytes, 0);
    for (UINT y = 0; y < rh && y < h; ++y)
        for (UINT x = 0; x < rw && x < w; ++x)
            fill(x, y, &data[(size_t(y) * w + x) * f.bytes]);
    D3D11_TEXTURE2D_DESC td {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = ResourceOf(f);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA init { data.data(), w * f.bytes, 0 };
    if (FAILED(rig.device->CreateTexture2D(&td, &init, &s.tex)))
    {
        std::printf("  could not create a %s source %ux%u\n", f.name, w, h);
        return s;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd {};
    vd.Format = f.view;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    rig.device->CreateShaderResourceView(s.tex.Get(), &vd, &s.srv);
    if (SrgbOf(f) != DXGI_FORMAT_UNKNOWN)
    {
        vd.Format = SrgbOf(f);
        rig.device->CreateShaderResourceView(s.tex.Get(), &vd, &s.srvSrgb);
    }
    return s;
}
TexelFn Pattern(const Fmt& f, UINT rw)
{
    return [&f, rw](uint32_t x, uint32_t y, uint8_t* out) { PatternTexel(f, y * rw + x + 1, out); };
}
TexelFn Motion(const Fmt& f, UINT rw, float mx = 1.0f, float my = 1.0f)
{
    return [&f, rw, mx, my](uint32_t x, uint32_t y, uint8_t* out) { MotionTexel(f, y * rw + x + 1, mx, my, out); };
}

struct Img
{
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bind = 0;
    UINT bytes = 0;
    std::vector<uint8_t> data; // tight rows
    const uint8_t* At(UINT x, UINT y) const { return &data[(size_t(y) * w + x) * bytes]; }
};
UINT BytesOfFormat(DXGI_FORMAT format)
{
    for (const Fmt& f : kFmts)
        if (ResourceOf(f) == format || f.view == format)
            return f.bytes;
    return 4;
}
Img ReadTexture(Rig& rig, ID3D11Texture2D* tex)
{
    Img img;
    D3D11_TEXTURE2D_DESC td {};
    tex->GetDesc(&td);
    img.w = td.Width;
    img.h = td.Height;
    img.format = td.Format;
    img.bind = td.BindFlags;
    img.bytes = BytesOfFormat(td.Format);
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(rig.device->CreateTexture2D(&sd, nullptr, &staging)))
        return img;
    rig.context->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m {};
    if (FAILED(rig.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return img;
    img.data.resize(size_t(img.w) * img.h * img.bytes);
    for (UINT y = 0; y < img.h; ++y)
        std::memcpy(&img.data[size_t(y) * img.w * img.bytes], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch,
                    size_t(img.w) * img.bytes);
    rig.context->Unmap(staging.Get(), 0);
    return img;
}
ComPtr<ID3D11Texture2D> TextureOf(ID3D11ShaderResourceView* view)
{
    if (view == nullptr)
        return nullptr;
    ComPtr<ID3D11Resource> res;
    view->GetResource(&res);
    ComPtr<ID3D11Texture2D> tex;
    res.As(&tex);
    return tex;
}
ID3D11ShaderResourceView* ViewPtr(uint64_t view) { return reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<uintptr_t>(view)); }
uint64_t U64(const void* p) { return reinterpret_cast<uintptr_t>(p); }
Img ReadView(Rig& rig, uint64_t view)
{
    ComPtr<ID3D11Texture2D> tex = TextureOf(ViewPtr(view));
    return tex != nullptr ? ReadTexture(rig, tex.Get()) : Img {};
}

// One pass on its own device, with the test draw
struct Fixture
{
    Rig rig;
    GBuffer kind = GBuffer::Normals;
    std::unique_ptr<GBufferRemap> pass;
    RemapOut out;
    RemapReport report;
    uint64_t stamp = 100;
    bool Create(GBuffer k = GBuffer::Normals, UINT flags = 0, D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0)
    {
        if (!rig.Create(flags, level))
            return false;
        kind = k;
        pass = std::make_unique<GBufferRemap>(k, &TestDraw);
        g_draw = DrawProbe {};
        return true;
    }
    // a request with a fresh stamp (so it is never a cache hit unless the caller repeats a request)
    RemapRequest Req(uint32_t ow, uint32_t oh, uint32_t rw, uint32_t rh, bool scaleMotion = true)
    {
        RemapRequest r;
        r.context = U64(rig.context.Get());
        r.buffer = kind;
        r.renderW = rw;
        r.renderH = rh;
        r.outputW = ow;
        r.outputH = oh;
        r.stamp = ++stamp;
        r.scaleMotion = scaleMotion;
        return r;
    }
    RemapStatus Go(const Source& s, RemapRequest r)
    {
        out = RemapOut {};
        r.srv = U64(s.srv.Get());
        r.srvSrgb = U64(s.srvSrgb.Get());
        return pass->Remap(rig.context.Get(), s.srv.Get(), s.srvSrgb.Get(), r, out, &report);
    }
    RemapStatus Run(const Source& s, uint32_t ow, uint32_t oh, uint32_t rw, uint32_t rh, bool scaleMotion = true)
    {
        return Go(s, Req(ow, oh, rw, rh, scaleMotion));
    }
};

// ---- the two CPU references ------------------------------------------------------------------------------------------------
// 1. the integer formula of the shader
uint32_t RefSrc(uint32_t i, uint32_t render, uint32_t output)
{
    return std::min((i * 2 + 1) * render / (output * 2), render - 1);
}
// 2. the texel whose span [s, s + 1) holds the pixel centre (i + 0.5) * render / output, found by scanning with exact products
uint32_t ScanSrc(uint32_t i, uint32_t render, uint32_t output)
{
    const uint64_t centre = uint64_t(2 * i + 1) * render; // compared with s * 2 * output
    for (uint32_t s = 0; s < render; ++s)
        if (uint64_t(s) * 2 * output <= centre && centre < uint64_t(s + 1) * 2 * output)
            return s;
    return render - 1;
}

struct Case
{
    const char* name;
    uint32_t sw, sh; // the add-on's texture
    uint32_t rw, rh; // the render window (top-left)
    uint32_t ow, oh; // the output
};
const Case kCases[] = {
    { "identity", 8, 6, 8, 6, 8, 6 },
    { "2:1 on both axes (1920 -> 3840)", 16, 12, 8, 6, 16, 12 },
    { "2:3, output 3 over render 2", 3, 3, 2, 2, 3, 3 },
    { "2:3, 6x4 -> 9x6", 9, 6, 6, 4, 9, 6 },
    { "2:3, bigger: 14x12 -> 21x18", 21, 18, 14, 12, 21, 18 },
    { "4:3 (1.33): 12x9 -> 16x12", 16, 12, 12, 9, 16, 12 },
    { "non-integer: 7x5 -> 11x9", 11, 9, 7, 5, 11, 9 },
    { "non-integer: 13x10 -> 17x11", 17, 11, 13, 10, 17, 11 },
    { "smaller in x only", 16, 12, 8, 12, 16, 12 },
    { "smaller in y only", 16, 12, 16, 6, 16, 12 },
    { "one texel", 5, 4, 1, 1, 5, 4 },
    { "source larger than the output", 20, 14, 8, 6, 12, 9 },
    { "source is render-sized", 8, 6, 8, 6, 16, 12 },
    { "tall", 9, 61, 6, 30, 9, 61 },
};

// Every output pixel holds the bytes of the texel the references name
void CheckImage(const char* what, const Fmt& f, const Case& c, const Img& img, const TexelFn& expect)
{
    CHECK(img.w == c.ow && img.h == c.oh);
    if (img.w != c.ow || img.h != c.oh || img.data.size() != size_t(c.ow) * c.oh * f.bytes)
    {
        std::printf("  %s / %s / %s: no usable image (%ux%u, %zu bytes)\n", what, f.name, c.name, img.w, img.h, img.data.size());
        return;
    }
    uint8_t want[16] {};
    for (uint32_t y = 0; y < c.oh; ++y)
        for (uint32_t x = 0; x < c.ow; ++x)
        {
            const uint32_t sxA = RefSrc(x, c.rw, c.ow), syA = RefSrc(y, c.rh, c.oh);
            const uint32_t sxB = ScanSrc(x, c.rw, c.ow), syB = ScanSrc(y, c.rh, c.oh);
            bool ok = sxA == sxB && syA == syB;
            expect(sxA, syA, want);
            ok = ok && std::memcmp(img.At(x, y), want, f.bytes) == 0;
            expect(sxB, syB, want);
            ok = ok && std::memcmp(img.At(x, y), want, f.bytes) == 0;
            if (!ok)
            {
                std::printf("CHECK FAILED %s:%d: %s / %s / %s: pixel (%u,%u) is wrong (source %u,%u by the formula, %u,%u by the scan)\n",
                            __FILE__, __LINE__, what, f.name, c.name, x, y, sxA, syA, sxB, syB);
                ++g_fail;
                return;
            }
        }
}

void Mapping()
{
    for (const Case& c : kCases)
    {
        // the two references agree (a self-check of the test)
        for (uint32_t i = 0; i < c.ow; ++i)
            CHECK(RefSrc(i, c.rw, c.ow) == ScanSrc(i, c.rw, c.ow));
        for (uint32_t i = 0; i < c.oh; ++i)
            CHECK(RefSrc(i, c.rh, c.oh) == ScanSrc(i, c.rh, c.oh));
    }
    for (const Fmt& f : kFmts)
    {
        Fixture fx;
        if (!fx.Create())
            return;
        for (const Case& c : kCases)
        {
            const Source src = MakeSource(fx.rig, f, c.sw, c.sh, c.rw, c.rh, Pattern(f, c.rw));
            CHECK(src.srv != nullptr);
            if (src.srv == nullptr)
                continue;
            const RemapStatus st = fx.Run(src, c.ow, c.oh, c.rw, c.rh);
            if (st != RemapStatus::Ok)
                std::printf("  %s / %s: refused (%s)\n", f.name, c.name, StatusName(st));
            CHECK(st == RemapStatus::Ok);
            if (st != RemapStatus::Ok)
                continue;
            CHECK(fx.out.srv != 0 && !fx.out.cached);
            CHECK(fx.out.format == uint32_t(f.view));
            CheckImage("remap", f, c, ReadView(fx.rig, fx.out.srv), [&](uint32_t sx, uint32_t sy, uint8_t* o) { PatternTexel(f, sy * c.rw + sx + 1, o); });
        }
    }
}

// The first and last row / column read the first / last render texel: stated outright for one ratio, on a format with a unique value
void EdgesExplicit()
{
    Fixture fx;
    if (!fx.Create())
        return;
    const Fmt& f = *FindFmt(DXGI_FORMAT_R32G32B32A32_FLOAT);
    const Source src = MakeSource(fx.rig, f, 8, 6, 5, 4, Pattern(f, 5));
    CHECK(fx.Run(src, 8, 6, 5, 4) == RemapStatus::Ok);
    const Img img = ReadView(fx.rig, fx.out.srv);
    CHECK(img.w == 8 && img.h == 6 && img.data.size() == 8 * 6 * 16);
    if (img.data.size() != 8 * 6 * 16)
        return;
    // 5 -> 8 on x: 0 0 1 2 2 3 4 4; 4 -> 6 on y: 0 1 1 2 3 3 (rows 1 and 4 sit exactly on a texel border: (2i + 1) * 4 / 12)
    const uint32_t xs[8] = { 0, 0, 1, 2, 2, 3, 4, 4 };
    const uint32_t ys[6] = { 0, 1, 1, 2, 3, 3 };
    uint8_t want[16] {};
    for (uint32_t y = 0; y < 6; ++y)
        for (uint32_t x = 0; x < 8; ++x)
        {
            PatternTexel(f, ys[y] * 5 + xs[x] + 1, want);
            CHECK(std::memcmp(img.At(x, y), want, 16) == 0);
        }
}

// The 8888 formats: the copy lives on the typeless parent, the srv is the UNORM view, the second srv the sRGB twin of the same bytes
void EightEightEightEight()
{
    for (const Fmt& f : kFmts)
    {
        Fixture fx;
        if (!fx.Create())
            return;
        const Case& c = kCases[5];
        const Source src = MakeSource(fx.rig, f, c.sw, c.sh, c.rw, c.rh, Pattern(f, c.rw));
        CHECK(fx.Run(src, c.ow, c.oh, c.rw, c.rh) == RemapStatus::Ok);
        ComPtr<ID3D11Texture2D> tex = TextureOf(ViewPtr(fx.out.srv));
        CHECK(tex != nullptr);
        if (tex == nullptr)
            continue;
        D3D11_TEXTURE2D_DESC td {};
        tex->GetDesc(&td);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd {};
        ViewPtr(fx.out.srv)->GetDesc(&vd);
        CHECK(vd.Format == f.view && fx.out.format == uint32_t(f.view));
        CHECK(td.Format == ResourceOf(f));
        CHECK((td.BindFlags & D3D11_BIND_RENDER_TARGET) != 0 && (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0);
        CHECK(td.Width == c.ow && td.Height == c.oh && td.MipLevels == 1 && td.ArraySize == 1 && td.SampleDesc.Count == 1);
        if (HasTwin(f))
        {
            CHECK(fx.out.srvSrgb != 0 && fx.out.srvSrgb != fx.out.srv);
            D3D11_SHADER_RESOURCE_VIEW_DESC sd {};
            ViewPtr(fx.out.srvSrgb)->GetDesc(&sd);
            CHECK(sd.Format == SrgbOf(f));
            CHECK(sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D);
            CHECK(TextureOf(ViewPtr(fx.out.srvSrgb)).Get() == tex.Get()); // the same bytes
            CheckImage("twin", f, c, ReadView(fx.rig, fx.out.srvSrgb), [&](uint32_t sx, uint32_t sy, uint8_t* o) { PatternTexel(f, sy * c.rw + sx + 1, o); });
        }
        else
        {
            CHECK(fx.out.srvSrgb == fx.out.srv); // no twin: the same view twice
        }
    }
}

// ---- motion vectors ------------------------------------------------------------------------------------------------------------
struct MotionCase
{
    const char* name;
    uint32_t rw, rh, ow, oh;
    float mx, my;
};
const MotionCase kMotionCases[] = {
    { "x2 (1920 -> 3840)", 8, 8, 16, 16, 2.0f, 2.0f },
    { "x1.5 (2560 -> 3840)", 8, 8, 12, 12, 1.5f, 1.5f },
    { "anisotropic: x2 on x, x1.5 on y", 8, 8, 16, 12, 2.0f, 1.5f },
    { "anisotropic: x1.5 on x, x2 on y", 8, 8, 12, 16, 1.5f, 2.0f },
    { "x2 on y only", 8, 6, 8, 12, 1.0f, 2.0f },
};

void MotionVectors()
{
    for (const Fmt& f : kFmts)
    {
        if (!IsMotionClass(f))
            continue;
        for (const MotionCase& m : kMotionCases)
            for (int scale = 0; scale < 2; ++scale)
            {
                Fixture fx;
                if (!fx.Create(GBuffer::MotionVectors))
                    return;
                const Case c { m.name, m.ow, m.oh, m.rw, m.rh, m.ow, m.oh };
                const Source src = MakeSource(fx.rig, f, c.sw, c.sh, c.rw, c.rh, Motion(f, c.rw));
                const RemapStatus st = fx.Run(src, c.ow, c.oh, c.rw, c.rh, scale != 0);
                if (st != RemapStatus::Ok)
                    std::printf("  %s / %s (scale %d): %s\n", f.name, m.name, scale, StatusName(st));
                CHECK(st == RemapStatus::Ok);
                if (st != RemapStatus::Ok)
                    continue;
                const float mx = scale != 0 ? m.mx : 1.0f, my = scale != 0 ? m.my : 1.0f;
                // the first two channels are scaled, the third and fourth are the source's
                CheckImage(scale != 0 ? "scaled motion" : "unscaled motion", f, c, ReadView(fx.rig, fx.out.srv),
                           [&](uint32_t sx, uint32_t sy, uint8_t* o) { MotionTexel(f, sy * c.rw + sx + 1, mx, my, o); });
            }
    }
}

// Every other buffer keeps its values, whatever scaleMotion says
void OtherBuffersNeverScale()
{
    const Fmt& f = *FindFmt(DXGI_FORMAT_R16G16_FLOAT);
    for (GBuffer kind : { GBuffer::Normals, GBuffer::Decals, GBuffer::DecalsWater, GBuffer::Material })
        for (int scale = 0; scale < 2; ++scale)
        {
            Fixture fx;
            if (!fx.Create(kind))
                return;
            const MotionCase& m = kMotionCases[2];
            const Case c { m.name, m.ow, m.oh, m.rw, m.rh, m.ow, m.oh };
            const Source src = MakeSource(fx.rig, f, c.sw, c.sh, c.rw, c.rh, Motion(f, c.rw));
            CHECK(fx.Run(src, c.ow, c.oh, c.rw, c.rh, scale != 0) == RemapStatus::Ok);
            CheckImage("buffer that never scales", f, c, ReadView(fx.rig, fx.out.srv),
                       [&](uint32_t sx, uint32_t sy, uint8_t* o) { MotionTexel(f, sy * c.rw + sx + 1, 1.0f, 1.0f, o); });
        }
}

// Vector values cannot be scaled in UNORM: scaled motion vectors on such a format are left to the add-on, and nothing is drawn
void UnormMotionVectors()
{
    for (const Fmt& f : kFmts)
    {
        if (IsMotionClass(f))
            continue;
        Fixture fx;
        if (!fx.Create(GBuffer::MotionVectors))
            return;
        const Case& c = kCases[1];
        const Source src = MakeSource(fx.rig, f, c.sw, c.sh, c.rw, c.rh, Pattern(f, c.rw));
        if (f.cls == Cls::F11) // a float format after all (it holds scaled values, if only positive ones): scaled is fine
        {
            CHECK(fx.Run(src, c.ow, c.oh, c.rw, c.rh, true) == RemapStatus::Ok);
            CHECK(fx.Run(src, c.ow, c.oh, c.rw, c.rh, false) == RemapStatus::Ok);
            continue;
        }
        InstallSwapHook(fx.rig, 0, SwapFault::None);
        RemapStatus st = fx.Run(src, c.ow, c.oh, c.rw, c.rh, true);
        if (st != RemapStatus::UnsupportedFormat)
            std::printf("  %s: scaled motion vectors gave %s\n", f.name, StatusName(st));
        CHECK(st == RemapStatus::UnsupportedFormat);
        CHECK(g_draw.draws == 0 && g_swap.calls == 0);
        CHECK(fx.out.srv == 0 && fx.out.srvSrgb == 0);
        CHECK(!fx.pass->Ready()); // not even set up: the request is looked at first
        // not scaled: any supported format is fine, and the values are the source's
        st = fx.Run(src, c.ow, c.oh, c.rw, c.rh, false);
        CHECK(st == RemapStatus::Ok);
        if (st == RemapStatus::Ok)
            CheckImage("unscaled UNORM motion", f, c, ReadView(fx.rig, fx.out.srv),
                       [&](uint32_t sx, uint32_t sy, uint8_t* o) { PatternTexel(f, sy * c.rw + sx + 1, o); });
        g_swap.patch.Remove();
    }
}


// ---- the cache -----------------------------------------------------------------------------------------------------------------
const Fmt& Rg16() { return *FindFmt(DXGI_FORMAT_R16G16_FLOAT); }

void CacheKey()
{
    Fixture fx;
    if (!fx.Create())
        return;
    const Fmt& f = Rg16();
    const Source a = MakeSource(fx.rig, f, 16, 12, 8, 6, Pattern(f, 8));
    const Source b = MakeSource(fx.rig, f, 16, 12, 8, 6, Motion(f, 8)); // other content
    InstallSwapHook(fx.rig, 0, SwapFault::None); // counts the swaps; none of them misbehaves
    const RemapRequest k0 = fx.Req(16, 12, 8, 6);

    CHECK(fx.Go(a, k0) == RemapStatus::Ok && !fx.out.cached);
    CHECK(g_draw.draws == 1 && g_swap.calls == 2);
    const RemapOut first = fx.out;
    // the same key: the same views, nothing drawn, not even a state swap
    for (int i = 0; i < 3; ++i)
    {
        CHECK(fx.Go(a, k0) == RemapStatus::Ok);
        CHECK(fx.out.cached && fx.report.cached);
        CHECK(fx.out.srv == first.srv && fx.out.srvSrgb == first.srvSrgb && fx.out.format == first.format);
        CHECK(g_draw.draws == 1 && g_swap.calls == 2);
    }

    // one field of the key at a time: a redraw, and a new key to hit afterwards
    int expectedDraws = 1;
    const auto redraws = [&](const char* what, const Source& src, const RemapRequest& r)
    {
        const RemapStatus st = fx.Go(src, r);
        ++expectedDraws;
        if (st != RemapStatus::Ok || fx.out.cached || g_draw.draws != expectedDraws)
            std::printf("  cache key '%s': status %s cached=%d draws=%d (want %d)\n", what, StatusName(st), fx.out.cached ? 1 : 0, g_draw.draws,
                        expectedDraws);
        CHECK(st == RemapStatus::Ok && !fx.out.cached && g_draw.draws == expectedDraws);
        CHECK(fx.Go(src, r) == RemapStatus::Ok && fx.out.cached && g_draw.draws == expectedDraws); // and now it hits
    };
    RemapRequest r = k0;
    r.stamp += 1;
    redraws("stamp", a, r);
    redraws("srv", b, r);
    {
        const Img img = ReadView(fx.rig, fx.out.srv); // the new content, not the old one
        uint8_t want[8] {};
        MotionTexel(f, 1, 1.0f, 1.0f, want);
        CHECK(img.data.size() == 16 * 12 * f.bytes && std::memcmp(img.At(0, 0), want, f.bytes) == 0);
    }
    r.renderW = 6;
    redraws("render width", b, r);
    r.renderH = 4;
    redraws("render height", b, r);
    r.outputW = 17;
    redraws("output width", b, r);
    r.outputH = 13;
    redraws("output height", b, r);
    r.scaleMotion = !r.scaleMotion;
    redraws("scaleMotion", b, r);
    r.scaleMotion = !r.scaleMotion;
    redraws("scaleMotion back", b, r);
    r.stamp = 0; // stamp 0 is a stamp like any other
    redraws("stamp 0", b, r);

    // Release() ends the cache
    fx.pass->Release();
    CHECK(fx.Go(b, r) == RemapStatus::Ok && !fx.out.cached);
    CHECK(g_draw.draws == expectedDraws + 1);

    // a refusal and a context that is not ours leave it alone
    const int draws = g_draw.draws;
    const int swaps = g_swap.calls;
    RemapRequest bad = r;
    bad.renderW = 0;
    CHECK(fx.Go(b, bad) == RemapStatus::Refused);
    CHECK(fx.out.srv == 0);
    CHECK(fx.pass->Remap(nullptr, b.srv.Get(), nullptr, r, fx.out) == RemapStatus::NoContext);
    ComPtr<ID3D11DeviceContext> deferred;
    CHECK(SUCCEEDED(fx.rig.device->CreateDeferredContext(0, &deferred)));
    if (deferred != nullptr)
        CHECK(fx.pass->Remap(deferred.Get(), b.srv.Get(), nullptr, r, fx.out) == RemapStatus::NoContext);
    CHECK(fx.out.srv == 0 && g_draw.draws == draws && g_swap.calls == swaps);
    CHECK(fx.Go(b, r) == RemapStatus::Ok && fx.out.cached && g_draw.draws == draws);
    g_swap.patch.Remove();
}

// ---- the target and the lifetime of the views ------------------------------------------------------------------------------
void TargetRecreation()
{
    Fixture fx;
    if (!fx.Create())
        return;
    const Fmt& f = Rg16();
    const Fmt& unorm = *FindFmt(DXGI_FORMAT_R16G16_UNORM);
    const Source src = MakeSource(fx.rig, f, 40, 30, 16, 12, Pattern(f, 16));
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    CHECK(fx.report.targetCreated && fx.report.status == RemapStatus::Ok);
    CHECK(fx.report.source.ok && fx.report.source.plain2D && fx.report.source.width == 40 && fx.report.source.height == 30);
    CHECK(fx.report.source.format == uint32_t(f.view));
    const ComPtr<ID3D11Texture2D> first = TextureOf(ViewPtr(fx.out.srv));
    CHECK(first != nullptr);
    if (first == nullptr)
        return;
    // another render size, another source of the same format: the same target, only the constants change
    const std::pair<uint32_t, uint32_t> windows[] = { { 10, 7 }, { 4, 3 }, { 16, 12 }, { 16, 5 }, { 1, 1 } };
    for (const auto& rs : windows)
    {
        CHECK(fx.Run(src, 16, 12, rs.first, rs.second) == RemapStatus::Ok);
        CHECK(!fx.report.targetCreated);
        CHECK(TextureOf(ViewPtr(fx.out.srv)).Get() == first.Get());
    }
    // and the contents follow the constants
    CHECK(fx.Run(src, 16, 12, 10, 7) == RemapStatus::Ok);
    {
        const Img mapped = ReadView(fx.rig, fx.out.srv);
        uint8_t want[4] {};
        PatternTexel(f, 1, want);
        CHECK(mapped.data.size() == 16 * 12 * 4 && std::memcmp(mapped.At(0, 0), want, 4) == 0);
        PatternTexel(f, 6 * 16 + 9 + 1, want); // the last pixel reads the last texel of the 10 x 7 window of this source: (9, 6)
        CHECK(std::memcmp(mapped.At(15, 11), want, 4) == 0);
    }
    // either dimension of the output recreates it
    CHECK(fx.Run(src, 17, 12, 16, 12) == RemapStatus::Ok);
    CHECK(fx.report.targetCreated);
    ComPtr<ID3D11Texture2D> now = TextureOf(ViewPtr(fx.out.srv));
    CHECK(now != nullptr && now.Get() != first.Get());
    D3D11_TEXTURE2D_DESC td {};
    if (now != nullptr)
    {
        now->GetDesc(&td);
        CHECK(td.Width == 17 && td.Height == 12 && td.Format == f.view);
    }
    CHECK(fx.Run(src, 17, 12, 16, 12) == RemapStatus::Ok && !fx.report.targetCreated);
    CHECK(fx.Run(src, 17, 13, 16, 12) == RemapStatus::Ok && fx.report.targetCreated);
    now = TextureOf(ViewPtr(fx.out.srv));
    CHECK(now != nullptr);
    if (now != nullptr)
    {
        now->GetDesc(&td);
        CHECK(td.Width == 17 && td.Height == 13);
    }
    // the format of the add-on's view recreates it, too
    const Source srcU = MakeSource(fx.rig, unorm, 40, 30, 16, 12, Pattern(unorm, 16));
    CHECK(fx.Run(srcU, 17, 13, 16, 12) == RemapStatus::Ok);
    CHECK(fx.report.targetCreated);
    now = TextureOf(ViewPtr(fx.out.srv));
    CHECK(now != nullptr);
    if (now != nullptr)
    {
        now->GetDesc(&td);
        CHECK(td.Format == DXGI_FORMAT_R16G16_UNORM && td.Width == 17 && td.Height == 13);
    }
    CHECK(fx.out.format == uint32_t(DXGI_FORMAT_R16G16_UNORM));
    CHECK(fx.Run(srcU, 17, 13, 16, 12) == RemapStatus::Ok && !fx.report.targetCreated);
    CHECK(fx.Run(src, 17, 13, 16, 12) == RemapStatus::Ok && fx.report.targetCreated); // and back
}

// The new target is made and drawn FIRST; the old views go after that. What the caller took a reference of stays valid.
void ViewLifetime()
{
    static const GUID kCanary = { 0x8a6e2f1c, 0x4d3b, 0x4f8a, { 0x9c, 0x11, 0x5e, 0x77, 0x10, 0x2a, 0x3b, 0x01 } };
    Fixture fx;
    if (!fx.Create())
        return;
    const Fmt& f = Rg16();
    const Source src = MakeSource(fx.rig, f, 40, 30, 16, 12, Pattern(f, 16));
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    // the views are not AddRef'd for the caller: the pass holds the only reference
    CHECK(RefCount(ViewPtr(fx.out.srv)) == 1); // RefCount() answers the number of holders: the pass
    ComPtr<ID3D11ShaderResourceView> held(ViewPtr(fx.out.srv)); // the caller's own reference, as the binder takes it
    const Img before = ReadView(fx.rig, fx.out.srv);
    CHECK(before.data.size() == 16 * 12 * 4);
    bool oldDestroyed = false, newDestroyed = false;
    {
        Canary* c = new Canary(&oldDestroyed);
        ComPtr<ID3D11Texture2D> t = TextureOf(held.Get());
        t->SetPrivateDataInterface(kCanary, c);
        c->Release();
    }

    // a new output size: the old target is still held by the pass while the new one is drawn, and gone from the pass afterwards
    g_draw.watch = held.Get();
    CHECK(fx.Run(src, 20, 12, 16, 12) == RemapStatus::Ok);
    g_draw.watch = nullptr;
    CHECK(fx.report.targetCreated);
    CHECK(g_draw.watchRefs == 2);          // the caller's and the pass's: the old views had not been released when the draw ran
    CHECK(RefCount(held.Get()) == 1);      // ... and now the pass has let go: only the caller's reference is left
    CHECK(fx.out.srv != U64(held.Get()));
    {
        Canary* c = new Canary(&newDestroyed);
        ComPtr<ID3D11Texture2D> t = TextureOf(ViewPtr(fx.out.srv));
        t->SetPrivateDataInterface(kCanary, c);
        c->Release();
    }
    fx.rig.context->Flush();
    CHECK(!oldDestroyed); // the caller's reference keeps the old copy alive ...
    const Img after = ReadView(fx.rig, U64(held.Get()));
    CHECK(after.w == 16 && after.h == 12 && after.data == before.data); // ... and intact
    held.Reset();
    fx.rig.context->Flush();
    CHECK(oldDestroyed); // the caller let go: it is freed

    // Release() while the caller holds the new copy: the copy survives, the pass's own references go
    ComPtr<ID3D11ShaderResourceView> heldNew(ViewPtr(fx.out.srv));
    const Img newBefore = ReadView(fx.rig, fx.out.srv);
    fx.pass->Release();
    fx.rig.context->Flush();
    CHECK(!newDestroyed);
    CHECK(ReadView(fx.rig, U64(heldNew.Get())).data == newBefore.data);
    CHECK(RefCount(heldNew.Get()) == 1);
    heldNew.Reset();
    fx.rig.context->Flush();
    CHECK(newDestroyed);
    CHECK(!fx.pass->Ready());
    fx.pass->Release(); // twice: nothing more to say
    CHECK(fx.Run(src, 20, 12, 16, 12) == RemapStatus::Ok); // and the pass comes back on the next request

    // a failed draw at a new size keeps the old target, returns Failed and clears the cache key
    const RemapRequest k = fx.Req(20, 12, 16, 12);
    CHECK(fx.Go(src, k) == RemapStatus::Ok);
    ID3D11Texture2D* keptRaw = nullptr; // (a pointer value only: a reference here would keep the target alive and hide a release)
    bool keptDestroyed = false;
    {
        ComPtr<ID3D11Texture2D> t = TextureOf(ViewPtr(fx.out.srv));
        keptRaw = t.Get();
        Canary* c = new Canary(&keptDestroyed);
        t->SetPrivateDataInterface(kCanary, c);
        c->Release();
    }
    CHECK(fx.Go(src, k) == RemapStatus::Ok && fx.out.cached);
    g_draw.mode = 1;
    CHECK(fx.Run(src, 24, 12, 16, 12) == RemapStatus::Failed);
    g_draw.mode = 0;
    CHECK(fx.out.srv == 0 && fx.out.srvSrgb == 0 && !fx.out.cached);
    CHECK(!fx.report.targetCreated);
    fx.rig.context->Flush();
    CHECK(!keptDestroyed);
    CHECK(fx.Go(src, k) == RemapStatus::Ok);
    CHECK(!fx.out.cached); // the key was cleared: it is drawn again ...
    CHECK(!fx.report.targetCreated && TextureOf(ViewPtr(fx.out.srv)).Get() == keptRaw); // ... into the very target that was kept
    // a failed draw into the target that is there (same size) clears the key just the same
    g_draw.mode = 2;
    CHECK(fx.Run(src, 20, 12, 8, 6) == RemapStatus::Failed);
    g_draw.mode = 0;
    CHECK(fx.Go(src, k) == RemapStatus::Ok && !fx.out.cached);
    CHECK(fx.Go(src, k) == RemapStatus::Ok && fx.out.cached);
}

// ---- the game's context state ---------------------------------------------------------------------------------------------
void ContextState()
{
    for (UINT flags : { 0u, UINT(D3D11_CREATE_DEVICE_SINGLETHREADED) })
    {
        Fixture fx;
        if (!fx.Create(GBuffer::Normals, flags))
            return;
        GameState game;
        CHECK(game.Set(fx.rig));
        const Fmt& f = Rg16();
        const Source src = MakeSource(fx.rig, f, 24, 18, 12, 9, Pattern(f, 12));
        const Dump before = DumpState(fx.rig.context.Get());
        CHECK(before.size() > 40);
        CHECK(fx.Run(src, 24, 18, 12, 9) == RemapStatus::Ok);
        CHECK(g_draw.draws == 1);
        CHECK(SameState(flags != 0 ? "single-threaded device, after a remap" : "after a remap", before, DumpState(fx.rig.context.Get())));
        // a second one (target already there), and a cache hit
        const RemapRequest k = fx.Req(24, 18, 9, 7);
        CHECK(fx.Go(src, k) == RemapStatus::Ok);
        CHECK(SameState("after the second remap", before, DumpState(fx.rig.context.Get())));
        CHECK(fx.Go(src, k) == RemapStatus::Ok && fx.out.cached);
        CHECK(SameState("after a cache hit", before, DumpState(fx.rig.context.Get())));
        // the game's view of the shader slots is the game's own, so its next draw still works
        fx.rig.context->Draw(3, 0);
        // refusals leave it alone, too
        CHECK(fx.Run(src, 0, 18, 12, 9) == RemapStatus::Refused);
        CHECK(SameState("after a refusal", before, DumpState(fx.rig.context.Get())));
        // a draw that throws, and one that crashes
        for (int mode : { 1, 2 })
        {
            g_draw.mode = mode;
            CHECK(fx.Run(src, 24, 18, 12, 9) == RemapStatus::Failed);
            CHECK(fx.out.srv == 0);
            CHECK(SameState(mode == 1 ? "after a draw that throws" : "after a draw that crashes", before, DumpState(fx.rig.context.Get())));
            g_draw.mode = 0;
        }
        // and the pass still works afterwards
        CHECK(fx.Run(src, 24, 18, 12, 9) == RemapStatus::Ok);
        CHECK(SameState("after recovery", before, DumpState(fx.rig.context.Get())));
    }
}

// At begin_effects the game's own state may still have the G-buffer bound: as a render target, or as a shader resource in a pixel shader
// slot. The pass reads it from its private state and must neither disturb those bindings nor read an unbound (all zero) view.
void SourceBoundInGameState()
{
    for (const DXGI_FORMAT view : { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM })
        for (int variant = 0; variant < 3; ++variant)
        {
            Fixture fx;
            if (!fx.Create())
                return;
            const Fmt& f = *FindFmt(view);
            const Case& c = kCases[5];
            const Source src = MakeSource(fx.rig, f, c.sw, c.sh, c.rw, c.rh, Pattern(f, c.rw));
            ComPtr<ID3D11RenderTargetView> rtv;
            D3D11_RENDER_TARGET_VIEW_DESC rd {};
            rd.Format = f.view;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            CHECK(SUCCEEDED(fx.rig.device->CreateRenderTargetView(src.tex.Get(), &rd, &rtv)));
            if (rtv == nullptr)
                continue;
            GameState game; // colour target, shaders, buffers and the other states as in the state test
            CHECK(game.Set(fx.rig));
            ID3D11RenderTargetView* rtvs[1] = { rtv.Get() };
            ID3D11ShaderResourceView* srvs[1] = { src.srv.Get() };
            const char* what = "";
            if (variant == 0)
            {
                fx.rig.context->OMSetRenderTargets(1, rtvs, nullptr);
                what = "the G-buffer bound as the game's render target";
            }
            else if (variant == 1)
            {
                fx.rig.context->PSSetShaderResources(3, 1, srvs);
                what = "the add-on's view bound in a pixel shader slot";
            }
            else
            {
                fx.rig.context->PSSetShaderResources(3, 1, srvs);
                fx.rig.context->VSSetShaderResources(3, 1, srvs);
                what = "the add-on's view bound in two shader stages";
            }
            const Dump before = DumpState(fx.rig.context.Get());
            uint64_t boundRtv = 0, boundSrv = 0;
            for (const auto& entry : before)
            {
                if (entry.first == "rtv0")
                    boundRtv = entry.second;
                if (entry.first == "psSrv3")
                    boundSrv = entry.second;
            }
            CHECK(variant != 0 || boundRtv == U64(rtv.Get()));
            CHECK(variant == 0 || boundSrv == U64(src.srv.Get()));

            const RemapStatus st = fx.Run(src, c.ow, c.oh, c.rw, c.rh);
            if (st != RemapStatus::Ok)
                std::printf("  %s (%s): %s\n", f.name, what, StatusName(st));
            CHECK(st == RemapStatus::Ok);
            CHECK(SameState(what, before, DumpState(fx.rig.context.Get())));
            if (st != RemapStatus::Ok)
                continue;
            CheckImage(what, f, c, ReadView(fx.rig, fx.out.srv), [&](uint32_t sx, uint32_t sy, uint8_t* o) { PatternTexel(f, sy * c.rw + sx + 1, o); });
        }
}

void BypassScopeIsOn()
{
    Fixture fx;
    if (!fx.Create())
        return;
    const Fmt& f = Rg16();
    const Source src = MakeSource(fx.rig, f, 16, 12, 8, 6, Pattern(f, 8));
    CHECK(!ext::core::HooksBypassed());
    CHECK(fx.Run(src, 16, 12, 8, 6) == RemapStatus::Ok);
    CHECK(g_draw.draws == 1 && g_draw.sawBypass);
    CHECK(!ext::core::HooksBypassed()); // the scope ended with the call
    for (int mode : { 1, 2 }) // ... also on the way out of a C++ exception and of an access violation
    {
        g_draw.mode = mode;
        g_draw.sawBypass = false;
        CHECK(fx.Run(src, 16, 12, 8, 6) == RemapStatus::Failed);
        CHECK(g_draw.sawBypass && !ext::core::HooksBypassed());
    }
    g_draw.mode = 0;
    // a cache hit and a refusal leave it as it was, too
    const RemapRequest k = fx.Req(16, 12, 8, 6);
    CHECK(fx.Go(src, k) == RemapStatus::Ok);
    CHECK(fx.Go(src, k) == RemapStatus::Ok && fx.out.cached && !ext::core::HooksBypassed());
    CHECK(fx.Run(src, 0, 12, 8, 6) == RemapStatus::Refused && !ext::core::HooksBypassed());
    {
        ext::core::BypassScope outer; // an outer scope is not switched off by the pass
        CHECK(fx.Run(src, 16, 12, 8, 6) == RemapStatus::Ok);
        CHECK(ext::core::HooksBypassed());
    }
    CHECK(!ext::core::HooksBypassed());
}

// UpdateSubresource is predicated by the game's SetPredication, a Draw in the private state is not. The constants are written in the
// private state: a fresh constant buffer is all zeros, which would divide by zero in the shader.
void PredicatedGameState()
{
    for (int predicateValue = 0; predicateValue < 2; ++predicateValue)
    {
        Fixture fx;
        if (!fx.Create())
            return;
        GameState game;
        CHECK(game.Set(fx.rig));
        ComPtr<ID3D11Predicate> predicate;
        D3D11_QUERY_DESC qd { D3D11_QUERY_OCCLUSION_PREDICATE, 0 };
        CHECK(SUCCEEDED(fx.rig.device->CreatePredicate(&qd, &predicate)));
        if (predicate == nullptr)
            return;
        fx.rig.context->Begin(predicate.Get());
        fx.rig.context->End(predicate.Get());
        fx.rig.context->SetPredication(predicate.Get(), predicateValue != 0);
        const Dump before = DumpState(fx.rig.context.Get());
        bool predicated = false;
        for (const auto& entry : before)
            predicated = predicated || (entry.first == "predicate" && entry.second != 0);
        CHECK(predicated);

        const Fmt& f = *FindFmt(DXGI_FORMAT_R32_FLOAT);
        const Case first { "predicated, first remap", 17, 11, 13, 10, 17, 11 };
        const Case second { "predicated, new constants", 17, 11, 7, 5, 17, 11 };
        for (const Case& c : { first, second })
        {
            const Source src = MakeSource(fx.rig, f, c.sw, c.sh, c.rw, c.rh, Pattern(f, c.rw));
            fx.rig.context->SetPredication(predicate.Get(), predicateValue != 0);
            const RemapStatus st = fx.Run(src, c.ow, c.oh, c.rw, c.rh);
            CHECK(SameState("after a remap under predication", before, DumpState(fx.rig.context.Get())));
            fx.rig.context->SetPredication(nullptr, FALSE); // the readback is a CopyResource: predicated, too
            CHECK(st == RemapStatus::Ok);
            if (st == RemapStatus::Ok)
                CheckImage(predicateValue != 0 ? "predicate value TRUE" : "predicate value FALSE", f, c, ReadView(fx.rig, fx.out.srv),
                           [&](uint32_t sx, uint32_t sy, uint8_t* o) { PatternTexel(f, sy * c.rw + sx + 1, o); });
        }
        fx.rig.context->SetPredication(nullptr, FALSE);
    }
}

// The bypass scope covers the prepare step (the views), not only the draw
void BypassCoversEveryCall()
{
    Fixture fx;
    if (!fx.Create())
        return;
    const Fmt& f = Rg16();
    const Source src = MakeSource(fx.rig, f, 40, 30, 16, 12, Pattern(f, 16));
    InstallDeviceHooks(fx.rig);
    g_dev.active = true;
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    g_dev.active = false;
    CHECK(g_dev.srvCalls >= 1); // the view of the new target
    CHECK(g_dev.srvBypassed == g_dev.srvCalls);
    CHECK(g_dev.removedCalls >= 1); // the device is asked on every remap
    CHECK(!ext::core::HooksBypassed());
    RemoveDeviceHooks();
}

// ---- refusals --------------------------------------------------------------------------------------------------------------
ComPtr<ID3D11Texture2D> MakeTexture(Rig& rig, DXGI_FORMAT format, UINT w, UINT h, UINT bind, UINT array = 1, UINT samples = 1, UINT mips = 1)
{
    D3D11_TEXTURE2D_DESC td {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = mips;
    td.ArraySize = array;
    td.Format = format;
    td.SampleDesc.Count = samples;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    ComPtr<ID3D11Texture2D> tex;
    rig.device->CreateTexture2D(&td, nullptr, &tex);
    return tex;
}
ComPtr<ID3D11ShaderResourceView> MakeView(Rig& rig, ID3D11Resource* res, DXGI_FORMAT format, D3D11_SRV_DIMENSION dim, UINT mostDetailedMip = 0, UINT mips = 1)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC vd {};
    vd.Format = format;
    vd.ViewDimension = dim;
    switch (dim)
    {
    case D3D11_SRV_DIMENSION_TEXTURE2D:
        vd.Texture2D.MostDetailedMip = mostDetailedMip;
        vd.Texture2D.MipLevels = mips;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
        vd.Texture2DArray.MostDetailedMip = mostDetailedMip;
        vd.Texture2DArray.MipLevels = mips;
        vd.Texture2DArray.FirstArraySlice = 0;
        vd.Texture2DArray.ArraySize = 1;
        break;
    default:
        break;
    }
    ComPtr<ID3D11ShaderResourceView> view;
    rig.device->CreateShaderResourceView(res, &vd, &view);
    return view;
}

void Refusals()
{
    Fixture fx;
    if (!fx.Create())
        return;
    const Fmt& f = Rg16();
    const Source good = MakeSource(fx.rig, f, 16, 12, 8, 6, Pattern(f, 8));
    InstallSwapHook(fx.rig, 0, SwapFault::None);
    const RemapRequest k = fx.Req(16, 12, 8, 6);
    // a cache entry exists before the refusals: they leave it alone
    CHECK(fx.Go(good, k) == RemapStatus::Ok);
    const RemapOut cachedOut = fx.out;

    const auto expectRefused = [&](const char* what, ID3D11ShaderResourceView* srv, RemapStatus want, const RemapRequest& r)
    {
        const int draws = g_draw.draws;
        const int swaps = g_swap.calls;
        fx.out = RemapOut {};
        RemapReport rep;
        RemapRequest req = r;
        req.srv = U64(srv);
        const RemapStatus st = fx.pass->Remap(fx.rig.context.Get(), srv, nullptr, req, fx.out, &rep);
        if (st != want || g_draw.draws != draws || g_swap.calls != swaps || fx.out.srv != 0 || rep.status != want)
            std::printf("  refusal '%s': %s (want %s), draws %d, swaps %d, out.srv %llx\n", what, StatusName(st), StatusName(want),
                        g_draw.draws - draws, g_swap.calls - swaps, (unsigned long long) fx.out.srv);
        CHECK(st == want && rep.status == want);
        CHECK(g_draw.draws == draws && g_swap.calls == swaps); // nothing was drawn, no state was touched
        CHECK(fx.out.srv == 0 && fx.out.srvSrgb == 0 && !fx.out.cached);
    };

    // sizes
    RemapRequest r = k;
    r.renderW = 0;
    expectRefused("zero render width", good.srv.Get(), RemapStatus::Refused, r);
    r = k;
    r.renderH = 0;
    expectRefused("zero render height", good.srv.Get(), RemapStatus::Refused, r);
    r = k;
    r.outputW = 0;
    expectRefused("zero output width", good.srv.Get(), RemapStatus::Refused, r);
    r = k;
    r.outputH = 0;
    expectRefused("zero output height", good.srv.Get(), RemapStatus::Refused, r);
    r = k;
    r.outputW = kMaxEdge + 1;
    expectRefused("output beyond kMaxEdge", good.srv.Get(), RemapStatus::Refused, r);
    r = k;
    r.renderH = kMaxEdge + 1;
    expectRefused("render beyond kMaxEdge", good.srv.Get(), RemapStatus::Refused, r);
    r = k;
    r.renderW = 17; // wider than the source (16)
    r.outputW = 32;
    expectRefused("render window outside the source (x)", good.srv.Get(), RemapStatus::Refused, r);
    r = k;
    r.renderH = 13;
    r.outputH = 32;
    expectRefused("render window outside the source (y)", good.srv.Get(), RemapStatus::Refused, r);
    r = k;
    r.renderW = 16;
    r.outputW = 12; // larger than the output
    expectRefused("render window larger than the output", good.srv.Get(), RemapStatus::Refused, r);
    // the buffer is the pass's own
    r = k;
    r.buffer = GBuffer::MotionVectors;
    expectRefused("a request for another buffer", good.srv.Get(), RemapStatus::Refused, r);
    r.buffer = GBuffer::Count;
    expectRefused("a request for no buffer", good.srv.Get(), RemapStatus::Refused, r);

    // the view
    expectRefused("no view", nullptr, RemapStatus::NotPlain2D, k);
    const ComPtr<ID3D11Texture2D> array = MakeTexture(fx.rig, DXGI_FORMAT_R16G16_FLOAT, 16, 12, D3D11_BIND_SHADER_RESOURCE, 2);
    CHECK(array != nullptr);
    if (array != nullptr)
    {
        const auto v2d = MakeView(fx.rig, array.Get(), DXGI_FORMAT_R16G16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D);
        const auto varr = MakeView(fx.rig, array.Get(), DXGI_FORMAT_R16G16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2DARRAY);
        CHECK(v2d != nullptr && varr != nullptr);
        expectRefused("a 2D view of a two-slice texture", v2d.Get(), RemapStatus::NotPlain2D, k);
        expectRefused("an array view", varr.Get(), RemapStatus::NotPlain2D, k);
    }
    const ComPtr<ID3D11Texture2D> single = MakeTexture(fx.rig, DXGI_FORMAT_R16G16_FLOAT, 16, 12, D3D11_BIND_SHADER_RESOURCE, 1);
    if (single != nullptr)
    {
        const auto varr = MakeView(fx.rig, single.Get(), DXGI_FORMAT_R16G16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2DARRAY);
        CHECK(varr != nullptr);
        expectRefused("an array view of a one-slice texture", varr.Get(), RemapStatus::NotPlain2D, k);
    }
    const ComPtr<ID3D11Texture2D> mips = MakeTexture(fx.rig, DXGI_FORMAT_R16G16_FLOAT, 16, 12, D3D11_BIND_SHADER_RESOURCE, 1, 1, 3);
    CHECK(mips != nullptr);
    if (mips != nullptr)
    {
        const auto vmip1 = MakeView(fx.rig, mips.Get(), DXGI_FORMAT_R16G16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D, 1, 2);
        CHECK(vmip1 != nullptr);
        expectRefused("a view of mip 1", vmip1.Get(), RemapStatus::NotPlain2D, k);
        const auto vmip0 = MakeView(fx.rig, mips.Get(), DXGI_FORMAT_R16G16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 3); // mip 0 of a mipped texture is fine
        CHECK(vmip0 != nullptr);
    }
    const ComPtr<ID3D11Texture2D> msaa = MakeTexture(fx.rig, DXGI_FORMAT_R16G16B16A16_FLOAT, 16, 12, D3D11_BIND_SHADER_RESOURCE, 1, 4);
    if (msaa != nullptr)
    {
        const auto vms = MakeView(fx.rig, msaa.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2DMS);
        CHECK(vms != nullptr);
        expectRefused("a multisampled source", vms.Get(), RemapStatus::NotPlain2D, k);
    }
    else
        std::puts("  note: no 4x MSAA R16G16B16A16_FLOAT on this adapter, the multisample refusal is pinned by the plain2D rule of the pure test only");

    // the format of the view (a typeless view format cannot be created, so that one is in the pure table only)
    const ComPtr<ID3D11Texture2D> typeless = MakeTexture(fx.rig, DXGI_FORMAT_R8G8B8A8_TYPELESS, 16, 12, D3D11_BIND_SHADER_RESOURCE);
    CHECK(typeless != nullptr);
    if (typeless != nullptr)
    {
        const auto vsrgb = MakeView(fx.rig, typeless.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_SRV_DIMENSION_TEXTURE2D);
        const auto vuint = MakeView(fx.rig, typeless.Get(), DXGI_FORMAT_R8G8B8A8_UINT, D3D11_SRV_DIMENSION_TEXTURE2D);
        const auto vsint = MakeView(fx.rig, typeless.Get(), DXGI_FORMAT_R8G8B8A8_SINT, D3D11_SRV_DIMENSION_TEXTURE2D);
        CHECK(vsrgb != nullptr && vuint != nullptr && vsint != nullptr);
        expectRefused("an sRGB view", vsrgb.Get(), RemapStatus::UnsupportedFormat, k);
        expectRefused("a UINT view", vuint.Get(), RemapStatus::UnsupportedFormat, k);
        expectRefused("a SINT view", vsint.Get(), RemapStatus::UnsupportedFormat, k);
    }
    const ComPtr<ID3D11Texture2D> bc = MakeTexture(fx.rig, DXGI_FORMAT_BC1_UNORM, 16, 12, D3D11_BIND_SHADER_RESOURCE);
    if (bc != nullptr)
    {
        const auto vbc = MakeView(fx.rig, bc.Get(), DXGI_FORMAT_BC1_UNORM, D3D11_SRV_DIMENSION_TEXTURE2D);
        CHECK(vbc != nullptr);
        expectRefused("a BC1 view", vbc.Get(), RemapStatus::UnsupportedFormat, k);
    }
    const ComPtr<ID3D11Texture2D> depth = MakeTexture(fx.rig, DXGI_FORMAT_R24G8_TYPELESS, 16, 12, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL);
    if (depth != nullptr)
    {
        const auto vd = MakeView(fx.rig, depth.Get(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS, D3D11_SRV_DIMENSION_TEXTURE2D);
        CHECK(vd != nullptr);
        expectRefused("a depth view", vd.Get(), RemapStatus::UnsupportedFormat, k);
    }
    // scaled motion vectors on UNORM (the pure table has every format; one real one here)
    {
        Fixture mv;
        CHECK(mv.Create(GBuffer::MotionVectors));
        const Fmt& u = *FindFmt(DXGI_FORMAT_R16G16_UNORM);
        const Source usrc = MakeSource(mv.rig, u, 16, 12, 8, 6, Pattern(u, 8));
        CHECK(mv.Run(usrc, 16, 12, 8, 6, true) == RemapStatus::UnsupportedFormat && g_draw.draws == 0);
    }

    // a source that cannot be read: an exception under the guard, not a crash
    expectRefused("a pointer that is no object", reinterpret_cast<ID3D11ShaderResourceView*>(uintptr_t(0x10)), RemapStatus::Failed, k);

    // context
    {
        const int draws = g_draw.draws;
        const int swaps = g_swap.calls;
        RemapOut o;
        CHECK(fx.pass->Remap(nullptr, good.srv.Get(), nullptr, k, o) == RemapStatus::NoContext);
        CHECK(o.srv == 0);
        // a pointer that is no object: guarded (the context is untrusted too)
        CHECK(fx.pass->Remap(reinterpret_cast<ID3D11DeviceContext*>(uintptr_t(0x10)), good.srv.Get(), nullptr, k, o) == RemapStatus::Failed);
        CHECK(o.srv == 0);
        CHECK(g_draw.draws == draws && g_swap.calls == swaps);
    }
    // the cache survived all of it
    CHECK(fx.Go(good, k) == RemapStatus::Ok && fx.out.cached);
    CHECK(fx.out.srv == cachedOut.srv && fx.out.srvSrgb == cachedOut.srvSrgb);
    g_swap.patch.Remove();
}

void DeferredContext()
{
    Fixture fx;
    if (!fx.Create())
        return;
    ComPtr<ID3D11DeviceContext> deferred;
    CHECK(SUCCEEDED(fx.rig.device->CreateDeferredContext(0, &deferred)));
    if (deferred == nullptr)
        return;
    const Fmt& f = Rg16();
    const Source src = MakeSource(fx.rig, f, 16, 12, 8, 6, Pattern(f, 8));
    RemapRequest k = fx.Req(16, 12, 8, 6);
    RemapOut out;
    RemapReport rep;
    CHECK(fx.pass->Remap(deferred.Get(), src.srv.Get(), nullptr, k, out, &rep) == RemapStatus::NoContext);
    CHECK(rep.status == RemapStatus::NoContext);
    CHECK(g_draw.draws == 0);
    CHECK(!fx.pass->Ready()); // not even set up: the type is the first thing looked at
    // the type is looked at first: a deferred context with a view that is no object is still just a deferred context
    CHECK(fx.pass->Remap(deferred.Get(), reinterpret_cast<ID3D11ShaderResourceView*>(uintptr_t(0x10)), nullptr, k, out) == RemapStatus::NoContext);
    // the same request on the immediate context works
    CHECK(fx.Go(src, k) == RemapStatus::Ok);
    // and a deferred one, with a pass that is up and a warm cache, still does not draw
    CHECK(fx.pass->Remap(deferred.Get(), src.srv.Get(), nullptr, k, out) == RemapStatus::NoContext);
    CHECK(out.srv == 0 && g_draw.draws == 1);
}

// A device the set-up failed on (marked by hand: WARP has no device the set-up fails on): no Remap tries again, and nothing else is
// disturbed. The failing set-up itself is exercised in FailedSetUp.
void GivenUpDevice()
{
    Fixture fx;
    Rig bad;
    if (!fx.Create() || !bad.Create())
        return;
    CHECK(!RemapDeviceMarkedFailed(bad.device.Get()) && !RemapDeviceMarkedFailed(fx.rig.device.Get()));
    MarkRemapDeviceFailed(bad.device.Get());
    CHECK(RemapDeviceMarkedFailed(bad.device.Get()) && !RemapDeviceMarkedFailed(fx.rig.device.Get()));
    const Fmt& f = Rg16();
    const Source srcBad = MakeSource(bad, f, 16, 12, 8, 6, Pattern(f, 8));
    RemapOut out;
    RemapRequest r = fx.Req(16, 12, 8, 6);
    r.context = U64(bad.context.Get());
    CHECK(fx.pass->Remap(bad.context.Get(), srcBad.srv.Get(), nullptr, r, out) == RemapStatus::Failed);
    CHECK(!fx.pass->Ready() && fx.pass->InitFailures() == 0); // no set-up was attempted
    CHECK(g_draw.draws == 0 && out.srv == 0);
    // a good device is fine, and a call for the marked one then leaves the running pass alone
    const Source src = MakeSource(fx.rig, f, 16, 12, 8, 6, Pattern(f, 8));
    const RemapRequest k = fx.Req(16, 12, 8, 6);
    CHECK(fx.Go(src, k) == RemapStatus::Ok);
    CHECK(fx.pass->Ready());
    CHECK(fx.pass->Remap(bad.context.Get(), srcBad.srv.Get(), nullptr, r, out) == RemapStatus::Failed);
    CHECK(fx.pass->Ready());
    CHECK(fx.Go(src, k) == RemapStatus::Ok && fx.out.cached); // the cache of the running pass is untouched
    // Init on its own
    CHECK(!fx.pass->Init(nullptr));
    CHECK(!fx.pass->Ready());
    CHECK(fx.pass->Init(fx.rig.device.Get()));
    CHECK(fx.pass->Ready());
    CHECK(fx.Go(src, k) == RemapStatus::Ok && !fx.out.cached); // Init released the target: nothing is cached
    // Init() first, then a remap on the same device does not set up again
    CHECK(fx.pass->Init(fx.rig.device.Get()));
    const uint32_t failures = fx.pass->InitFailures();
    CHECK(fx.Go(src, fx.Req(16, 12, 8, 6)) == RemapStatus::Ok && fx.pass->InitFailures() == failures);
}

// A source that has no objects of the pass's device: not tested (undefined behaviour of D3D11, not a refusal path).

// The pass does not keep the add-on's texture alive and gives its own target back
void NoLeftovers()
{
    static const GUID kCanary = { 0x8a6e2f1c, 0x4d3b, 0x4f8a, { 0x9c, 0x11, 0x5e, 0x77, 0x10, 0x2a, 0x3b, 0x02 } };
    Fixture fx;
    if (!fx.Create())
        return;
    bool sourceDestroyed = false, targetDestroyed = false;
    {
        const Fmt& f = Rg16();
        Source src = MakeSource(fx.rig, f, 16, 12, 8, 6, Pattern(f, 8));
        Canary* c = new Canary(&sourceDestroyed);
        src.tex->SetPrivateDataInterface(kCanary, c);
        c->Release();
        CHECK(fx.Run(src, 16, 12, 8, 6) == RemapStatus::Ok);
        Canary* t = new Canary(&targetDestroyed);
        const ComPtr<ID3D11Texture2D> target = TextureOf(ViewPtr(fx.out.srv));
        if (target != nullptr)
            target->SetPrivateDataInterface(kCanary, t);
        t->Release();
    }
    fx.rig.context->Flush();
    CHECK(sourceDestroyed);  // nothing of the pass (its views, the private state) still refers to the add-on's texture
    CHECK(!targetDestroyed); // the pass owns the target
    fx.pass->Release();
    fx.rig.context->Flush();
    CHECK(targetDestroyed);  // ... and gives it up on Release
}

// ---- faults in the restore -------------------------------------------------------------------------------------------------
// Whatever fails on the way back, the game's context must end up in the game's state: the unbind step and the swap back are separate
// guarded steps, and a swap back that did not happen is retried by the next call, before anything else.
void FaultsInTheRestore()
{
    Fixture fx;
    if (!fx.Create())
        return;
    GameState game;
    CHECK(game.Set(fx.rig));
    const Fmt& f = Rg16();
    const Source src = MakeSource(fx.rig, f, 40, 30, 16, 12, Pattern(f, 16));
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    const Dump before = DumpState(fx.rig.context.Get());
    const ComPtr<ID3D11Texture2D> target = TextureOf(ViewPtr(fx.out.srv));

    // 1. the unbind step crashes (OMSetRenderTargets, from inside the private state)
    const RemapRequest k = fx.Req(16, 12, 16, 12);
    CHECK(fx.Go(src, k) == RemapStatus::Ok);
    g_draw.mode = 3;
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed);
    g_draw.mode = 0;
    g_patchOm.Remove();
    CHECK(fx.out.srv == 0);
    CHECK(SameState("after a crash while unbinding", before, DumpState(fx.rig.context.Get())));
    CHECK(fx.Go(src, k) == RemapStatus::Ok && !fx.out.cached); // a failure clears the key
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    CHECK(SameState("after recovery from the unbind crash", before, DumpState(fx.rig.context.Get())));

    // 2. the swap back crashes before it swaps: the context is left in the private state, and the NEXT call puts the game's state back
    // first, with the very state object the first swap returned. A context that really was in the private state has that state cleared
    // (swap back, enter, clear, swap back again).
    InstallSwapHook(fx.rig, 2, SwapFault::CrashBefore);
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed);
    CHECK(g_swap.calls >= 2);
    ID3DDeviceContextState* gameState = g_swap.previous.empty() ? nullptr : g_swap.previous[0];
    CHECK(gameState != nullptr);
    g_swap.faultCall = 0;
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok); // calls 3 to 5 are the retry (swap back, enter the private state to clear it, swap back), 6 and 7 the remap
    CHECK(g_swap.calls == 7);
    if (g_swap.calls >= 5)
    {
        CHECK(g_swap.targets[2] == gameState);
        CHECK(g_swap.targets[3] != gameState); // clearing: into the private state
        CHECK(g_swap.targets[4] == gameState);
        CHECK(g_swap.targets[0] != gameState);
    }
    CHECK(SameState("after the retried swap back", before, DumpState(fx.rig.context.Get())));
    for (bool open : g_swap.bypassed) // the retry, too, is our own call
        CHECK(open);
    // a Release while a swap back is outstanding retries it, too
    InstallSwapHook(fx.rig, 2, SwapFault::CrashBefore);
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed);
    g_swap.faultCall = 0;
    fx.pass->Release();
    CHECK(SameState("after Release with a swap back outstanding", before, DumpState(fx.rig.context.Get())));
    CHECK(g_swap.calls == 5);
    for (bool open : g_swap.bypassed)
        CHECK(open);
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);

    // 3. the swap back is done and then crashes
    InstallSwapHook(fx.rig, 2, SwapFault::CrashAfter);
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed);
    CHECK(SameState("after a crash behind the swap back", before, DumpState(fx.rig.context.Get())));
    g_swap.faultCall = 0;
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    CHECK(g_swap.calls == 5); // the retry is one swap: the context was in the game's state already, so there is nothing to clear
    CHECK(SameState("after the retry behind a finished swap back", before, DumpState(fx.rig.context.Get())));
    g_swap.patch.Remove();
    (void)target;
}

// A crash while the target is made: Failed, nothing drawn, the game's state as it was, the old target (if any) kept
void CrashInThePrepareStep()
{
    Fixture fx;
    if (!fx.Create())
        return;
    GameState game;
    CHECK(game.Set(fx.rig));
    const Fmt& f = Rg16();
    const Source src = MakeSource(fx.rig, f, 40, 30, 16, 12, Pattern(f, 16));
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    const ComPtr<ID3D11Texture2D> kept = TextureOf(ViewPtr(fx.out.srv));
    const Dump before = DumpState(fx.rig.context.Get());
    const int draws = g_draw.draws;
    InstallDeviceHooks(fx.rig);
    g_dev.fault = DeviceFault::SrvCrash;
    g_dev.active = true;
    CHECK(fx.Run(src, 20, 12, 16, 12) == RemapStatus::Failed); // a new size: the view of the new target is made, and crashes
    g_dev.active = false;
    CHECK(g_dev.srvCalls >= 1);
    RemoveDeviceHooks();
    CHECK(fx.out.srv == 0);
    CHECK(g_draw.draws == draws);
    CHECK(SameState("after a crash in the prepare step", before, DumpState(fx.rig.context.Get())));
    CHECK(!ext::core::HooksBypassed());
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok && TextureOf(ViewPtr(fx.out.srv)).Get() == kept.Get()); // the old target was kept
    CHECK(fx.Run(src, 20, 12, 16, 12) == RemapStatus::Ok);
}

// ---- a swap back that keeps failing ------------------------------------------------------------------------------------------
// The pass leaves the context in its private state when the swap back fails, and keeps the game's state object to try again. The
// attempts are capped, so a context that can never be put back does not block every later remap (also on another device), the recovery
// touches the immediate context only when the call is for that context, and it clears the private state. Giving up keeps the game's
// state object alive on purpose: the context may still be in the private state, and the object must not be destroyed under it.
int SwapsTo(ID3DDeviceContextState* state)
{
    int n = 0;
    for (ID3DDeviceContextState* t : g_swap.targets)
        n += t == state ? 1 : 0;
    return n;
}
size_t LogLines(const char* needle)
{
    size_t n = 0;
    for (const std::string& line : depthtest_log::Lines())
        n += line.find(needle) != std::string::npos ? 1 : 0;
    return n;
}

// Leaves the context of `fx` in the private state: the swap back (call 2 of the hook) crashes, and with `persistent` so does every later
// swap. Returns the game's state object with a reference of the test's own (the pass holds, or leaks, its own).
ID3DDeviceContextState* MakeStuck(Fixture& fx, const Source& src, bool persistent, bool onlyThisContext = false)
{
    InstallSwapHook(fx.rig, 2, SwapFault::CrashBefore, persistent, onlyThisContext);
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed);
    ID3DDeviceContextState* game = g_swap.previous.empty() ? nullptr : g_swap.previous[0];
    CHECK(game != nullptr);
    if (game != nullptr)
        game->AddRef();
    return game;
}

void StuckRecoveryIsCapped()
{
    Fixture fx;
    if (!fx.Create())
        return;
    GameState game;
    CHECK(game.Set(fx.rig));
    const Fmt& f = Rg16();
    const Source src = MakeSource(fx.rig, f, 40, 30, 16, 12, Pattern(f, 16));
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    const Dump before = DumpState(fx.rig.context.Get());
    ComPtr<ID3D11DeviceContext1> c1;
    fx.rig.context.As(&c1);
    const size_t lines = LogLines("giving up");
    ID3DDeviceContextState* gs = MakeStuck(fx, src, true);
    if (gs == nullptr)
        return;
    CHECK(SwapsTo(gs) == 1); // the swap back of the remap itself
    const ULONG refs = RefCount(gs);
    const int draws = g_draw.draws;
    for (int i = 1; i <= 8; ++i)
    {
        const int callsBefore = g_swap.calls;
        CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed);
        if (i == 1)
            CHECK(g_swap.calls == callsBefore + 1); // a failed recovery ends the call: the remap does not go on to swap into its state
    }
    CHECK(g_draw.draws == draws);
    CHECK(SwapsTo(gs) == 1 + 3); // three attempts to put the state back, then the pass gave up: the later calls do not try again
    CHECK(LogLines("giving up") == lines + 1); // said once
    CHECK(RefCount(gs) == refs); // the pass leaked its reference on purpose (a Reset would have dropped it)

    // the context is put back by hand; the pass is not blocked any more
    g_swap.faultCall = 0;
    g_swap.patch.Remove();
    c1->SwapDeviceContextState(gs, nullptr);
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    CHECK(SameState("after giving up and putting the state back by hand", before, DumpState(fx.rig.context.Get())));

    // a second episode starts with a fresh count: three attempts again, and a second line
    ID3DDeviceContextState* gs2 = MakeStuck(fx, src, true);
    if (gs2 == nullptr)
        return;
    CHECK(SwapsTo(gs2) == 1);
    for (int i = 1; i <= 5; ++i)
        CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed);
    CHECK(SwapsTo(gs2) == 1 + 3);
    CHECK(LogLines("giving up") == lines + 2);
    g_swap.faultCall = 0;
    g_swap.patch.Remove();
    c1->SwapDeviceContextState(gs2, nullptr);
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
}

// A recovery that worked ends the episode: the count of failed attempts starts over with the next one
void AttemptsStartOverAfterARecovery()
{
    Fixture fx;
    if (!fx.Create())
        return;
    GameState game;
    CHECK(game.Set(fx.rig));
    const Fmt& f = Rg16();
    const Source src = MakeSource(fx.rig, f, 40, 30, 16, 12, Pattern(f, 16));
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    const size_t lines = LogLines("giving up");
    ID3DDeviceContextState* gs = MakeStuck(fx, src, true);
    if (gs == nullptr)
        return;
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed); // failed attempt 1
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed); // failed attempt 2
    g_swap.faultCall = 0;
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok); // the third works: recovered, and the remap goes on
    CHECK(LogLines("giving up") == lines);
    ID3DDeviceContextState* gs2 = MakeStuck(fx, src, true);
    if (gs2 == nullptr)
        return;
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed); // 1
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed); // 2
    CHECK(LogLines("giving up") == lines); // two failures of the new episode are not a give-up, whatever the old one counted
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed); // 3: gives up
    CHECK(LogLines("giving up") == lines + 1);
    CHECK(SwapsTo(gs2) == 1 + 3);
    g_swap.faultCall = 0;
    g_swap.patch.Remove();
    ComPtr<ID3D11DeviceContext1> c1;
    fx.rig.context.As(&c1);
    c1->SwapDeviceContextState(gs2, nullptr);
}

// A removed device is no reason to keep trying to put a state back on its context
void StuckOnARemovedDevice()
{
    Fixture fx;
    if (!fx.Create())
        return;
    GameState game;
    CHECK(game.Set(fx.rig));
    const Fmt& f = Rg16();
    const Source src = MakeSource(fx.rig, f, 40, 30, 16, 12, Pattern(f, 16));
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    ID3DDeviceContextState* gs = MakeStuck(fx, src, true);
    if (gs == nullptr)
        return;
    const size_t lines = LogLines("giving up");
    InstallDeviceHooks(fx.rig);
    g_dev.fault = DeviceFault::DeviceRemoved;
    g_dev.active = true;
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed);
    g_dev.active = false;
    RemoveDeviceHooks();
    CHECK(SwapsTo(gs) == 1); // no attempt on the removed device's context
    CHECK(LogLines("giving up") == lines + 1);
    g_swap.faultCall = 0;
    g_swap.patch.Remove();
    ComPtr<ID3D11DeviceContext1> c1;
    fx.rig.context.As(&c1);
    c1->SwapDeviceContextState(gs, nullptr);
}

// A stuck context of one device does not block a remap on another one: the state is given up at once (the call is not for that
// context), the other device's remap goes through, and the stuck context is not touched again.
void StuckDoesNotBlockAnotherDevice()
{
    Fixture fx;
    Rig other;
    if (!fx.Create() || !other.Create())
        return;
    GameState game;
    CHECK(game.Set(fx.rig));
    const Fmt& f = Rg16();
    const Source src = MakeSource(fx.rig, f, 40, 30, 16, 12, Pattern(f, 16));
    const Source srcOther = MakeSource(other, f, 40, 30, 16, 12, Pattern(f, 16));
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    ID3DDeviceContextState* gs = MakeStuck(fx, src, true, true);
    if (gs == nullptr)
        return;
    const size_t lines = LogLines("giving up");
    const ULONG refs = RefCount(gs);
    RemapRequest r = fx.Req(16, 12, 16, 12);
    r.context = U64(other.context.Get());
    RemapOut out;
    CHECK(fx.pass->Remap(other.context.Get(), srcOther.srv.Get(), nullptr, r, out) == RemapStatus::Ok);
    CHECK(out.srv != 0);
    CHECK(SwapsTo(gs) == 1); // the stuck context was not touched again
    CHECK(LogLines("giving up") == lines + 1);
    CHECK(RefCount(gs) == refs);
    // and the first device, still stuck, is just another device now
    g_swap.faultCall = 0;
    g_swap.patch.Remove();
    ComPtr<ID3D11DeviceContext1> c1;
    fx.rig.context.As(&c1);
    c1->SwapDeviceContextState(gs, nullptr);
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
}

// A deferred context is refused before anything else: while a swap back is outstanding it must not touch the immediate context
void DeferredContextWhileStuck()
{
    Fixture fx;
    if (!fx.Create())
        return;
    GameState game;
    CHECK(game.Set(fx.rig));
    ComPtr<ID3D11DeviceContext> deferred;
    CHECK(SUCCEEDED(fx.rig.device->CreateDeferredContext(0, &deferred)));
    if (deferred == nullptr)
        return;
    const Fmt& f = Rg16();
    const Source src = MakeSource(fx.rig, f, 40, 30, 16, 12, Pattern(f, 16));
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    const Dump before = DumpState(fx.rig.context.Get());
    ID3DDeviceContextState* gs = MakeStuck(fx, src, false);
    if (gs == nullptr)
        return;
    g_swap.faultCall = 0;
    const int calls = g_swap.calls;
    RemapOut out;
    CHECK(fx.pass->Remap(deferred.Get(), src.srv.Get(), nullptr, fx.Req(16, 12, 16, 12), out) == RemapStatus::NoContext);
    CHECK(g_swap.calls == calls); // no swap, no state change on the immediate context
    // the next immediate call recovers first, as before
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    CHECK(SwapsTo(gs) >= 2);
    CHECK(SameState("after the recovery that followed a deferred call", before, DumpState(fx.rig.context.Get())));
    g_swap.patch.Remove();
    gs->Release();
}

// The destructor does not drop the only reference to the game's state object while the context may still be in the private state
void DestructorKeepsTheGameStateAlive()
{
    Fixture fx;
    if (!fx.Create())
        return;
    GameState game;
    CHECK(game.Set(fx.rig));
    const Fmt& f = Rg16();
    const Source src = MakeSource(fx.rig, f, 40, 30, 16, 12, Pattern(f, 16));
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    ID3DDeviceContextState* gs = MakeStuck(fx, src, true);
    if (gs == nullptr)
        return;
    const size_t lines = LogLines("giving up");
    const ULONG refs = RefCount(gs);
    fx.pass.reset(); // Release() tries once more, fails, and the destructor must leak the reference
    CHECK(RefCount(gs) == refs);
    CHECK(LogLines("giving up") == lines + 1);
    // put the game's state back by hand
    g_swap.faultCall = 0;
    g_swap.patch.Remove();
    ComPtr<ID3D11DeviceContext1> c1;
    fx.rig.context.As(&c1);
    c1->SwapDeviceContextState(gs, nullptr);
}

// A removed device: the pass lets go of everything, is not set up again on the removed device, and works again once it reports healthy
void RemovedDevice()
{
    Fixture fx;
    if (!fx.Create())
        return;
    const Fmt& f = Rg16();
    const Source src = MakeSource(fx.rig, f, 40, 30, 16, 12, Pattern(f, 16));
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok);
    InstallDeviceHooks(fx.rig);
    g_dev.fault = DeviceFault::DeviceRemoved;
    g_dev.active = true;
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed);
    CHECK(g_dev.removedCalls >= 1);
    CHECK(!fx.pass->Ready());
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed); // again: not set up again on the removed device
    CHECK(!fx.pass->Ready() && fx.pass->InitFailures() == 0);
    CHECK(!RemapDeviceMarkedFailed(fx.rig.device.Get())); // removed is not "set-up failed"
    g_dev.active = false;
    RemoveDeviceHooks();
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Ok); // healthy again
    CHECK(fx.pass->Ready());
}

// A set-up that fails (the vertex shader is refused): reported, the device is marked and not tried again, nothing half built stays
void FailedSetUp()
{
    Fixture fx;
    if (!fx.Create())
        return;
    const Fmt& f = Rg16();
    const Source src = MakeSource(fx.rig, f, 40, 30, 16, 12, Pattern(f, 16));
    InstallDeviceHooks(fx.rig);
    g_dev.fault = DeviceFault::VertexShaderFails;
    g_dev.active = true;
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed);
    CHECK(g_dev.vsCalls == 1);
    CHECK(fx.pass->InitFailures() == 1);
    CHECK(RemapDeviceMarkedFailed(fx.rig.device.Get()));
    CHECK(!fx.pass->Ready()); // the private state made before the shader failed is gone
    CHECK(g_draw.draws == 0 && fx.out.srv == 0);
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed);
    CHECK(g_dev.vsCalls == 1 && fx.pass->InitFailures() == 1); // not tried again, even though the cause is gone below
    g_dev.active = false;
    RemoveDeviceHooks();
    CHECK(fx.Run(src, 16, 12, 16, 12) == RemapStatus::Failed);
    CHECK(fx.pass->InitFailures() == 1 && !fx.pass->Ready());
    // another device is fine
    Rig other;
    if (!other.Create())
        return;
    const Source srcOther = MakeSource(other, f, 40, 30, 16, 12, Pattern(f, 16));
    RemapRequest r = fx.Req(16, 12, 16, 12);
    r.context = U64(other.context.Get());
    RemapOut out;
    CHECK(fx.pass->Remap(other.context.Get(), srcOther.srv.Get(), nullptr, r, out) == RemapStatus::Ok);
    CHECK(fx.pass->Ready());
}

// ---- DescribeView and the engine -------------------------------------------------------------------------------------------
void DescribeViews()
{
    Rig rig;
    if (!rig.Create())
        return;
    LiveRemapEngine engine;
    ViewDesc d;
    const Fmt& f = Rg16();
    const Source good = MakeSource(rig, f, 16, 12, 8, 6, Pattern(f, 8));
    CHECK(engine.DescribeView(U64(good.srv.Get()), d));
    CHECK(d.ok && d.plain2D && d.width == 16 && d.height == 12 && d.format == uint32_t(f.view));
    CHECK(d.device == U64(rig.device.Get()));

    // the format of the VIEW, not of the resource
    const Fmt& rgba = *FindFmt(DXGI_FORMAT_R8G8B8A8_UNORM);
    const Source twin = MakeSource(rig, rgba, 10, 8, 5, 4, Pattern(rgba, 5));
    CHECK(engine.DescribeView(U64(twin.srv.Get()), d) && d.plain2D && d.format == uint32_t(DXGI_FORMAT_R8G8B8A8_UNORM) && d.width == 10);
    CHECK(engine.DescribeView(U64(twin.srvSrgb.Get()), d) && d.plain2D && d.format == uint32_t(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));

    // not plain 2D: described (ok), but not plain
    const ComPtr<ID3D11Texture2D> array = MakeTexture(rig, DXGI_FORMAT_R16G16_FLOAT, 16, 12, D3D11_BIND_SHADER_RESOURCE, 2);
    const ComPtr<ID3D11Texture2D> single = MakeTexture(rig, DXGI_FORMAT_R16G16_FLOAT, 16, 12, D3D11_BIND_SHADER_RESOURCE, 1);
    const ComPtr<ID3D11Texture2D> mips = MakeTexture(rig, DXGI_FORMAT_R16G16_FLOAT, 16, 16, D3D11_BIND_SHADER_RESOURCE, 1, 1, 4);
    CHECK(array != nullptr && single != nullptr && mips != nullptr);
    if (array != nullptr && single != nullptr && mips != nullptr)
    {
        const auto a2d = MakeView(rig, array.Get(), DXGI_FORMAT_R16G16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D);
        CHECK(engine.DescribeView(U64(a2d.Get()), d) && d.ok && !d.plain2D && d.width == 16 && d.height == 12);
        const auto aarr = MakeView(rig, array.Get(), DXGI_FORMAT_R16G16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2DARRAY);
        CHECK(engine.DescribeView(U64(aarr.Get()), d) && d.ok && !d.plain2D);
        const auto sarr = MakeView(rig, single.Get(), DXGI_FORMAT_R16G16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2DARRAY);
        CHECK(engine.DescribeView(U64(sarr.Get()), d) && d.ok && !d.plain2D);
        const auto m1 = MakeView(rig, mips.Get(), DXGI_FORMAT_R16G16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D, 1, 3);
        CHECK(engine.DescribeView(U64(m1.Get()), d) && d.ok && !d.plain2D && d.width == 16 && d.height == 16); // mip 0 of the resource
        const auto m0 = MakeView(rig, mips.Get(), DXGI_FORMAT_R16G16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 4);
        CHECK(engine.DescribeView(U64(m0.Get()), d) && d.ok && d.plain2D);
    }
    const ComPtr<ID3D11Texture2D> msaa = MakeTexture(rig, DXGI_FORMAT_R16G16B16A16_FLOAT, 16, 12, D3D11_BIND_SHADER_RESOURCE, 1, 4);
    if (msaa != nullptr)
    {
        const auto vms = MakeView(rig, msaa.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2DMS);
        CHECK(engine.DescribeView(U64(vms.Get()), d) && d.ok && !d.plain2D);
    }

    // garbage: an untrusted handle never crashes, and the answer is empty
    const auto garbage = [&](const char* what, uint64_t handle)
    {
        d = ViewDesc {};
        d.ok = true; // (a leftover from an earlier answer must not survive)
        d.plain2D = true;
        d.width = 99;
        d.format = 99;
        d.device = 99;
        const bool answered = engine.DescribeView(handle, d);
        if (answered || d.ok || d.plain2D || d.width != 0 || d.height != 0 || d.format != 0 || d.device != 0)
            std::printf("  DescribeView(%s): answered=%d ok=%d plain2D=%d width=%u format=%u\n", what, answered ? 1 : 0, d.ok ? 1 : 0, d.plain2D ? 1 : 0, d.width, d.format);
        CHECK(!answered && !d.ok && !d.plain2D && d.width == 0 && d.height == 0 && d.format == 0 && d.device == 0);
    };
    garbage("null", 0);
    garbage("0x10", 0x10);
    garbage("-1", ~0ull);
    void* reserved = VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS); // an address range with no memory behind it
    CHECK(reserved != nullptr);
    if (reserved != nullptr)
    {
        garbage("a decommitted page", U64(reserved));
        VirtualFree(reserved, 0, MEM_RELEASE);
    }
    void* zeros = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); // an "object" whose method table is null
    CHECK(zeros != nullptr);
    if (zeros != nullptr)
    {
        garbage("a zeroed page", U64(zeros));
        VirtualFree(zeros, 0, MEM_RELEASE);
    }
    // the free function the pass itself uses
    CHECK(!ReadViewDesc(nullptr, d));
    CHECK(ReadViewDesc(good.srv.Get(), d) && d.plain2D);
}

// The engine: one pass per kind, the routing, Release, and when the raw draw is used
ID3D11DeviceContext* g_hookedContext = nullptr;
ID3D11DeviceContext* HookedContext() { return g_hookedContext; }

void EngineRouting()
{
    Rig rig;
    if (!rig.Create())
        return;
    g_draw = DrawProbe {};
    g_hookedContext = rig.context.Get();
    LiveRemapEngine engine(&TestDraw, &HookedContext);
    const Fmt& f = Rg16();
    const Source src = MakeSource(rig, f, 16, 12, 8, 6, Motion(f, 8));
    const auto request = [&](GBuffer b, uint64_t stamp)
    {
        RemapRequest r;
        r.context = U64(rig.context.Get());
        r.buffer = b;
        r.srv = U64(src.srv.Get());
        r.renderW = 8;
        r.renderH = 6;
        r.outputW = 16;
        r.outputH = 12;
        r.stamp = stamp;
        r.scaleMotion = true;
        return r;
    };

    // one pass per kind: the same view and stamp are a miss for each kind and a hit for each kind afterwards
    RemapOut out[kGBuffers];
    for (size_t i = 0; i < kGBuffers; ++i)
    {
        CHECK(engine.Remap(request(GBuffer(i), 7), out[i]) == RemapStatus::Ok);
        CHECK(!out[i].cached && out[i].srv != 0);
        for (size_t j = 0; j < i; ++j)
            CHECK(out[i].srv != out[j].srv && TextureOf(ViewPtr(out[i].srv)).Get() != TextureOf(ViewPtr(out[j].srv)).Get());
    }
    CHECK(g_draw.draws == int(kGBuffers));
    for (size_t i = 0; i < kGBuffers; ++i)
    {
        RemapOut again;
        CHECK(engine.Remap(request(GBuffer(i), 7), again) == RemapStatus::Ok);
        CHECK(again.cached && again.srv == out[i].srv);
    }
    CHECK(g_draw.draws == int(kGBuffers)); // all hits
    // the motion vectors are scaled, the others are not (same source, same size)
    {
        const Img mv = ReadView(rig, out[size_t(GBuffer::MotionVectors)].srv);
        const Img nm = ReadView(rig, out[size_t(GBuffer::Normals)].srv);
        uint8_t wantMv[4] {}, wantNm[4] {};
        MotionTexel(f, 1, 2.0f, 2.0f, wantMv);
        MotionTexel(f, 1, 1.0f, 1.0f, wantNm);
        CHECK(mv.data.size() == 16 * 12 * 4 && nm.data.size() == 16 * 12 * 4);
        CHECK(std::memcmp(mv.At(0, 0), wantMv, 4) == 0 && std::memcmp(nm.At(0, 0), wantNm, 4) == 0);
        CHECK(std::memcmp(wantMv, wantNm, 4) != 0);
    }
    // a new stamp for one kind leaves the others' caches alone
    {
        RemapOut o;
        CHECK(engine.Remap(request(GBuffer::Decals, 8), o) == RemapStatus::Ok && !o.cached);
        CHECK(engine.Remap(request(GBuffer::Normals, 7), o) == RemapStatus::Ok && o.cached);
        CHECK(engine.Remap(request(GBuffer::Decals, 8), o) == RemapStatus::Ok && o.cached);
    }

    // Release drops all of them, and a later Remap works again (as a miss)
    const int draws = g_draw.draws;
    ComPtr<ID3D11ShaderResourceView> heldNormals(ViewPtr(out[size_t(GBuffer::Normals)].srv));
    engine.Release();
    CHECK(RefCount(heldNormals.Get()) == 1); // the engine's passes let go: the caller's reference keeps the copy
    for (size_t i = 0; i < kGBuffers; ++i)
    {
        RemapOut o;
        CHECK(engine.Remap(request(GBuffer(i), 7), o) == RemapStatus::Ok);
        CHECK(!o.cached);
    }
    CHECK(g_draw.draws == draws + int(kGBuffers));
    engine.Release();
    engine.Release(); // twice

    // refusals: no context, a deferred context, a buffer that does not exist, a view that is no object
    {
        const int before = g_draw.draws;
        RemapOut o;
        RemapRequest r = request(GBuffer::Normals, 9);
        r.context = 0;
        CHECK(engine.Remap(r, o) == RemapStatus::NoContext && o.srv == 0);
        ComPtr<ID3D11DeviceContext> deferred;
        CHECK(SUCCEEDED(rig.device->CreateDeferredContext(0, &deferred)));
        if (deferred != nullptr)
        {
            r.context = U64(deferred.Get());
            CHECK(engine.Remap(r, o) == RemapStatus::NoContext && o.srv == 0);
        }
        r = request(GBuffer::Normals, 9);
        r.context = 0x10;
        CHECK(engine.Remap(r, o) == RemapStatus::Failed && o.srv == 0);
        r = request(GBuffer::Normals, 9);
        r.buffer = GBuffer::Count;
        CHECK(engine.Remap(r, o) == RemapStatus::Refused && o.srv == 0);
        r.buffer = GBuffer(200);
        CHECK(engine.Remap(r, o) == RemapStatus::Refused && o.srv == 0);
        r = request(GBuffer::Normals, 9);
        r.srv = 0x10;
        CHECK(engine.Remap(r, o) == RemapStatus::Failed && o.srv == 0);
        r.srv = 0;
        CHECK(engine.Remap(r, o) == RemapStatus::NotPlain2D && o.srv == 0);
        CHECK(g_draw.draws == before);
        CHECK(engine.Remap(request(GBuffer::Normals, 9), o) == RemapStatus::Ok); // and it works afterwards
    }

    // the raw draw: only for the context the lighting hooks hooked (hooked == the request's context)
    {
        Rig other;
        CHECK(other.Create());
        LiveRemapEngine raw(&TestDraw, &HookedContext);
        g_hookedContext = rig.context.Get();
        g_draw = DrawProbe {};
        RemapOut o;
        CHECK(raw.Remap(request(GBuffer::Normals, 20), o) == RemapStatus::Ok);
        CHECK(g_draw.draws == 1 && g_draw.sawBypass); // the raw draw, inside the bypass scope
        g_hookedContext = other.context.Get(); // hooks on another context: not this one's raw draw
        CHECK(raw.Remap(request(GBuffer::Normals, 21), o) == RemapStatus::Ok);
        CHECK(g_draw.draws == 1);
        {
            const Img img = ReadView(rig, o.srv); // the context's own Draw did the work
            uint8_t want[4] {};
            MotionTexel(f, 1, 1.0f, 1.0f, want);
            CHECK(img.data.size() == 16 * 12 * 4 && std::memcmp(img.At(0, 0), want, 4) == 0);
        }
        g_hookedContext = nullptr; // no hooks at all
        CHECK(raw.Remap(request(GBuffer::Normals, 22), o) == RemapStatus::Ok && g_draw.draws == 1);
        g_hookedContext = rig.context.Get(); // and back
        CHECK(raw.Remap(request(GBuffer::Normals, 23), o) == RemapStatus::Ok && g_draw.draws == 2);
        // another kind is decided on its own call
        CHECK(raw.Remap(request(GBuffer::Decals, 24), o) == RemapStatus::Ok && g_draw.draws == 3);
        // an engine with no raw draw never uses one
        LiveRemapEngine plain(nullptr, &HookedContext);
        CHECK(plain.Remap(request(GBuffer::Normals, 25), o) == RemapStatus::Ok && g_draw.draws == 3);
        LiveRemapEngine none;
        CHECK(none.Remap(request(GBuffer::Normals, 26), o) == RemapStatus::Ok && g_draw.draws == 3);
        // a raw draw but no hooked-context function: no raw draw either
        LiveRemapEngine noHook(&TestDraw, nullptr);
        CHECK(noHook.Remap(request(GBuffer::Normals, 27), o) == RemapStatus::Ok && g_draw.draws == 3);
    }

    // A request the pass will refuse does not cost the live pass: with a raw draw configured and hooked == the immediate context, a
    // deferred or garbage context makes the draw decision differ, and must not replace the pass (its target, its cache) on the way to
    // the refusal.
    {
        LiveRemapEngine raw(&TestDraw, &HookedContext);
        g_hookedContext = rig.context.Get();
        g_draw = DrawProbe {};
        RemapOut o;
        CHECK(raw.Remap(request(GBuffer::Normals, 40), o) == RemapStatus::Ok && !o.cached && g_draw.draws == 1);
        const uint64_t srv = o.srv;
        ComPtr<ID3D11DeviceContext> deferred;
        CHECK(SUCCEEDED(rig.device->CreateDeferredContext(0, &deferred)));
        RemapRequest r = request(GBuffer::Normals, 40);
        if (deferred != nullptr)
        {
            r.context = U64(deferred.Get());
            CHECK(raw.Remap(r, o) == RemapStatus::NoContext && o.srv == 0);
        }
        r.context = 0x10;
        CHECK(raw.Remap(r, o) == RemapStatus::Failed && o.srv == 0);
        r.context = 0;
        CHECK(raw.Remap(r, o) == RemapStatus::NoContext && o.srv == 0);
        CHECK(raw.Remap(request(GBuffer::Normals, 40), o) == RemapStatus::Ok);
        CHECK(o.cached && o.srv == srv && g_draw.draws == 1); // the same pass, the same cache: nothing was drawn again
    }
    g_hookedContext = nullptr;
}

// ---- the product shader, driven directly ---------------------------------------------------------------------------------
// The pass never draws a pixel beyond the output size, so the clamp and the largest sizes are looked at here.
Img RunShader(Rig& rig, ID3D11Texture2D* source, const RemapConstants& k, UINT tw, UINT th)
{
    Img none;
    ID3D11Device* d = rig.device.Get();
    ID3D11DeviceContext* c = rig.context.Get();
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    D3D11_BUFFER_DESC bd {};
    bd.ByteWidth = sizeof(RemapConstants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init { &k, 0, 0 };
    D3D11_TEXTURE2D_DESC td {};
    td.Width = tw;
    td.Height = th;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(d->CreateVertexShader(GBufferRemap_Vs_cso, sizeof(GBufferRemap_Vs_cso), nullptr, &vs)) ||
        FAILED(d->CreatePixelShader(GBufferRemap_Ps_cso, sizeof(GBufferRemap_Ps_cso), nullptr, &ps)) ||
        FAILED(d->CreateBuffer(&bd, &init, &cb)) || FAILED(d->CreateTexture2D(&td, nullptr, &target)) ||
        FAILED(d->CreateRenderTargetView(target.Get(), nullptr, &rtv)) || FAILED(d->CreateShaderResourceView(source, nullptr, &srv)))
        return none;
    const D3D11_VIEWPORT vp { 0.0f, 0.0f, float(tw), float(th), 0.0f, 1.0f };
    ID3D11RenderTargetView* rtvs[1] = { rtv.Get() };
    ID3D11Buffer* cbs[1] = { cb.Get() };
    ID3D11ShaderResourceView* srvs[1] = { srv.Get() };
    c->RSSetViewports(1, &vp);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs.Get(), nullptr, 0);
    c->PSSetShader(ps.Get(), nullptr, 0);
    c->PSSetConstantBuffers(0, 1, cbs);
    c->PSSetShaderResources(0, 1, srvs);
    c->OMSetRenderTargets(1, rtvs, nullptr);
    c->Draw(3, 0);
    ID3D11ShaderResourceView* noSrv[1] = {};
    c->PSSetShaderResources(0, 1, noSrv);
    c->OMSetRenderTargets(0, nullptr, nullptr);
    return ReadTexture(rig, target.Get());
}
float AsFloat(const Img& img, UINT x, UINT y)
{
    float v = 0;
    std::memcpy(&v, img.At(x, y), 4);
    return v;
}
RemapRequest ShaderRequest(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh)
{
    RemapRequest r;
    r.buffer = GBuffer::Normals;
    r.renderW = rw;
    r.renderH = rh;
    r.outputW = ow;
    r.outputH = oh;
    return r;
}

void ShaderClamp()
{
    Rig rig;
    if (!rig.Create())
        return;
    const Fmt& f = *FindFmt(DXGI_FORMAT_R32_FLOAT);
    const Source src = MakeSource(rig, f, 10, 8, 4, 3, Pattern(f, 4));
    // render 4x3 mapped for an output of 3x2, drawn into a 7x5 target: the pixels beyond the output read the last render texel (x 3,
    // y 2), never past the render window (the source is larger, with zeros beyond it)
    const Img img = RunShader(rig, src.tex.Get(), MakeRemapConstants(ShaderRequest(4, 3, 3, 2)), 7, 5);
    CHECK(img.w == 7 && img.h == 5 && img.data.size() == 35 * 4);
    if (img.data.size() != 35 * 4)
        return;
    uint8_t want[4] {};
    for (uint32_t y = 0; y < 5; ++y)
        for (uint32_t x = 0; x < 7; ++x)
        {
            const uint32_t sx = std::min((x * 2 + 1) * 4 / (3 * 2), 4u - 1);
            const uint32_t sy = std::min((y * 2 + 1) * 3 / (2 * 2), 3u - 1);
            PatternTexel(f, sy * 4 + sx + 1, want);
            if (std::memcmp(img.At(x, y), want, 4) != 0)
                std::printf("  clamp: pixel (%u,%u) read the wrong texel (want %u,%u)\n", x, y, sx, sy);
            CHECK(std::memcmp(img.At(x, y), want, 4) == 0);
        }
    PatternTexel(f, 2 * 4 + 3 + 1, want);
    CHECK(std::memcmp(img.At(6, 4), want, 4) == 0); // the corner: the last render texel
}

// The shader's own arithmetic on 1-pixel strips: a source of `render` texels whose value is its index, mapped to `output` pixels. The
// strips cover the largest edge (the products reach (2 * 0x3FFF + 1) * 0x4000, beyond what float32 holds exactly) and the real ratios
// of the game (Quality is exactly 2:3).
struct Strip
{
    uint32_t render, output;
};
const Strip kStrips[] = {
    { 16384, 16384 }, { 16384, 16383 }, { 16384, 5000 },  { 16384, 1 },    { 16383, 16384 }, { 16383, 16383 }, { 16383, 10000 },
    { 9973, 16384 },  { 9973, 16383 },  { 8191, 16384 },  { 12345, 16381 }, { 16381, 12345 }, { 2560, 3840 },   { 2258, 3840 },
    { 1706, 3840 },   { 1920, 3840 },   { 1440, 2160 },   { 1270, 2160 },  { 1080, 2160 },   { 720, 2160 },    { 3840, 3840 },
};

// What the shader would do with float32 maths instead of integers, for the self-check below
uint32_t FloatMul(uint32_t i, uint32_t r, uint32_t o) // (i + 0.5) * render / output
{
    return std::min(uint32_t(std::floor((float(i) + 0.5f) * float(r) / float(o))), r - 1);
}
uint32_t FloatRatio(uint32_t i, uint32_t r, uint32_t o) // (i + 0.5) * (render / output)
{
    return std::min(uint32_t(std::floor((float(i) + 0.5f) * (float(r) / float(o)))), r - 1);
}
uint32_t FloatRcp(uint32_t i, uint32_t r, uint32_t o) // (i + 0.5) * render * (1 / output)
{
    return std::min(uint32_t(std::floor((float(i) + 0.5f) * float(r) * (1.0f / float(o)))), r - 1);
}

void ShaderStrips()
{
    Rig rig;
    if (!rig.Create())
        return;
    uint32_t floatMulWrong = 0, floatRatioWrong = 0, floatRcpWrong = 0;
    for (int vertical = 0; vertical < 2; ++vertical)
        for (const Strip& strip : kStrips)
        {
            const UINT w = vertical ? 1 : strip.render, h = vertical ? strip.render : 1;
            std::vector<float> values(strip.render);
            for (uint32_t i = 0; i < strip.render; ++i)
                values[i] = float(i);
            D3D11_TEXTURE2D_DESC td {};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R32_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init { values.data(), w * 4, 0 };
            ComPtr<ID3D11Texture2D> src;
            CHECK(SUCCEEDED(rig.device->CreateTexture2D(&td, &init, &src)));
            if (src == nullptr)
                return;
            const UINT ow = vertical ? 1 : strip.output, oh = vertical ? strip.output : 1;
            const Img img = RunShader(rig, src.Get(), MakeRemapConstants(ShaderRequest(w, h, ow, oh)), ow, oh);
            CHECK(img.data.size() == size_t(strip.output) * 4);
            if (img.data.size() != size_t(strip.output) * 4)
                continue;
            uint32_t wrong = 0, firstWrong = 0;
            for (uint32_t i = 0; i < strip.output; ++i)
            {
                // 64-bit maths here, 32-bit in the shader
                const uint64_t s = std::min<uint64_t>((uint64_t(i) * 2 + 1) * strip.render / (uint64_t(strip.output) * 2), strip.render - 1);
                CHECK(strip.render > 5000 || s == ScanSrc(i, strip.render, strip.output)); // the scan is slow on the big ones
                const float got = vertical ? AsFloat(img, 0, i) : AsFloat(img, i, 0);
                if (got != float(s) && wrong++ == 0)
                    firstWrong = i;
                if (!vertical)
                {
                    floatMulWrong += FloatMul(i, strip.render, strip.output) != s ? 1 : 0;
                    floatRatioWrong += FloatRatio(i, strip.render, strip.output) != s ? 1 : 0;
                    floatRcpWrong += FloatRcp(i, strip.render, strip.output) != s ? 1 : 0;
                }
            }
            if (wrong != 0)
                std::printf("  strip %u -> %u (%s): %u pixels wrong, first at %u\n", strip.render, strip.output, vertical ? "vertical" : "horizontal", wrong,
                            firstWrong);
            CHECK(wrong == 0);
        }
    // The strips must be able to tell a float version from the integer one: each float form of the mapping is wrong somewhere
    std::printf("  float32 forms of the mapping disagree with the integer one at: (i+.5)*r/o %u, (i+.5)*(r/o) %u, (i+.5)*r*rcp(o) %u pixels\n",
                floatMulWrong, floatRatioWrong, floatRcpWrong);
    CHECK(floatMulWrong > 0);
    CHECK(floatRatioWrong > 0);
    CHECK(floatRcpWrong > 0);
}
} // namespace

int main()
{
    FormatTable();
    Checks();
    Constants();
    Replaced();

    Rig probe;
    if (!probe.Create())
    {
        std::puts("SKIP: no WARP D3D11 device (the pure checks ran)");
        return g_fail == 0 ? 0 : 1;
    }
    std::fputs("mapping...\n", stderr);
    Mapping();
    EdgesExplicit();
    EightEightEightEight();
    MotionVectors();
    OtherBuffersNeverScale();
    UnormMotionVectors();
    std::fputs("cache and lifetime...\n", stderr);
    CacheKey();
    TargetRecreation();
    ViewLifetime();
    NoLeftovers();
    std::fputs("context state...\n", stderr);
    ContextState();
    SourceBoundInGameState();
    BypassScopeIsOn();
    PredicatedGameState();
    BypassCoversEveryCall();
    std::fputs("refusals...\n", stderr);
    Refusals();
    DeferredContext();
    GivenUpDevice();
    std::fputs("faults...\n", stderr);
    FaultsInTheRestore();
    CrashInThePrepareStep();
    StuckRecoveryIsCapped();
    AttemptsStartOverAfterARecovery();
    StuckOnARemovedDevice();
    StuckDoesNotBlockAnotherDevice();
    DeferredContextWhileStuck();
    DestructorKeepsTheGameStateAlive();
    RemovedDevice();
    FailedSetUp();
    std::fputs("engine...\n", stderr);
    DescribeViews();
    EngineRouting();
    std::fputs("shader...\n", stderr);
    ShaderClamp();
    ShaderStrips();

    if (g_fail != 0)
    {
        std::printf("FAIL: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::puts("PASS: G-buffer copy (format table sweep, request checks, constants and motion multiplier, remap of 19 view formats x 14 cases against two references, 8888 sRGB twins, scaled motion vectors, cache key, target recreation, view lifetime and order, context state (also with the view bound in it), bypass scope, refusals, faults, a stuck swap back (capped attempts, another device, removed device, deferred context, destructor leak), DescribeView on good and garbage handles, engine routing, product shader clamp and largest edge)");
    return 0;
}
