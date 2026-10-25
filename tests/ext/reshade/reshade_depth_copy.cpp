// Automatic ReShade scene depth, the D3D11 half (ReShadeDepthCopy.cpp, shaders/depth_crop.hlsl) on the WARP software adapter.
// Pure: the view format table, the request checks, the constant packing, the device-replaced rule. WARP: the crop and nearest
// remap against two independent CPU references (the integer formula, and a scan for the texel whose span holds the pixel centre),
// the source families, the game's context state, the bypass scope, the publish order of the three paths (new capture, Release,
// device change) with a recording sink, refusals that draw nothing, failures that put the game's state back, and the product
// shader driven directly (clamp, largest edge).
#include "ext/core/HooksBypass.h"
#include "ext/reshade/ReShadeDepthCopy.h"

#include <DepthCrop_Ps_Shader.h>
#include <DepthCrop_Vs_Shader.h>

#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// ---- pure ------------------------------------------------------------------------------------------------------------------
void FormatTable()
{
    struct Row
    {
        DXGI_FORMAT in, out;
    };
    const Row rows[] = {
        { DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS },
        { DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS },
        { DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS },
        { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT },
        { DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT },
        { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT },
        { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS },
        { DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS },
        { DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS },
        { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM },
        { DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM },
        { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM },
    };
    for (const Row& r : rows)
    {
        if (SrvFormatFor(r.in) != r.out)
            std::printf("  format %d -> %d, expected %d\n", int(r.in), int(SrvFormatFor(r.in)), int(r.out));
        CHECK(SrvFormatFor(r.in) == r.out);
    }
    // Nothing else is read: every other value of the enumeration (and a few beyond it) falls through.
    for (int f = 0; f < 256; ++f)
    {
        bool listed = false;
        for (const Row& r : rows)
            listed = listed || int(r.in) == f;
        if (!listed && SrvFormatFor(DXGI_FORMAT(f)) != DXGI_FORMAT_UNKNOWN)
            std::printf("  format %d should not be supported\n", f);
        CHECK(listed || SrvFormatFor(DXGI_FORMAT(f)) == DXGI_FORMAT_UNKNOWN);
    }
    CHECK(SrvFormatFor(DXGI_FORMAT_UNKNOWN) == DXGI_FORMAT_UNKNOWN);
    CHECK(SrvFormatFor(DXGI_FORMAT_R8_UNORM) == DXGI_FORMAT_UNKNOWN);
    CHECK(SrvFormatFor(DXGI_FORMAT_R16_FLOAT) == DXGI_FORMAT_UNKNOWN);
    CHECK(SrvFormatFor(DXGI_FORMAT_X32_TYPELESS_G8X24_UINT) == DXGI_FORMAT_UNKNOWN); // the stencil plane
    CHECK(SrvFormatFor(DXGI_FORMAT_X24_TYPELESS_G8_UINT) == DXGI_FORMAT_UNKNOWN);
    CHECK(SrvFormatFor(DXGI_FORMAT_R8G8B8A8_UNORM) == DXGI_FORMAT_UNKNOWN);
}

SourceDesc GoodSource()
{
    SourceDesc s;
    s.width = 64;
    s.height = 48;
    s.arraySize = 1;
    s.sampleCount = 1;
    s.bindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
    s.format = DXGI_FORMAT_R32_TYPELESS;
    return s;
}
CaptureArgs GoodArgs()
{
    CaptureArgs a;
    a.outputW = 64;
    a.outputH = 48;
    a.baseX = 3;
    a.baseY = 2;
    a.renderW = 32;
    a.renderH = 24;
    return a;
}

void Checks()
{
    static_assert(kMaxEdge == 0x4000); // the D3D11 texture limit; the packing and the shader's 32-bit maths depend on it
    SourceDesc s = GoodSource();
    CaptureArgs a = GoodArgs();
    CHECK(CheckSource(s, a) == Result::Ok);

    // output size: nonzero, at most kMaxEdge
    a = GoodArgs();
    a.outputW = 0;
    CHECK(CheckSource(s, a) == Result::BadOutputSize);
    a = GoodArgs();
    a.outputH = 0;
    CHECK(CheckSource(s, a) == Result::BadOutputSize);
    a = GoodArgs();
    a.outputW = kMaxEdge + 1;
    CHECK(CheckSource(s, a) == Result::BadOutputSize);
    a = GoodArgs();
    a.outputH = kMaxEdge + 1;
    CHECK(CheckSource(s, a) == Result::BadOutputSize);
    a = GoodArgs();
    a.outputW = a.outputH = 1;
    CHECK(CheckSource(s, a) == Result::Ok);
    a.outputW = a.outputH = kMaxEdge;
    CHECK(CheckSource(s, a) == Result::Ok);

    // render size: 1 .. kMaxEdge
    a = GoodArgs();
    a.renderW = 0;
    CHECK(CheckSource(s, a) == Result::BadRenderSize);
    a = GoodArgs();
    a.renderH = 0;
    CHECK(CheckSource(s, a) == Result::BadRenderSize);
    a = GoodArgs();
    a.renderW = kMaxEdge + 1;
    CHECK(CheckSource(s, a) == Result::BadRenderSize);
    a = GoodArgs();
    a.renderH = kMaxEdge + 1;
    CHECK(CheckSource(s, a) == Result::BadRenderSize);
    a = GoodArgs();
    a.renderW = a.renderH = 1;
    CHECK(CheckSource(s, a) == Result::Ok);

    // source size: 1 .. kMaxEdge
    for (uint32_t bad : { 0u, kMaxEdge + 1, 0xFFFFFFFFu })
    {
        s = GoodSource();
        s.width = bad;
        CHECK(CheckSource(s, GoodArgs()) == Result::BadSourceSize);
        s = GoodSource();
        s.height = bad;
        CHECK(CheckSource(s, GoodArgs()) == Result::BadSourceSize);
    }
    s = GoodSource();
    s.width = s.height = kMaxEdge;
    CHECK(CheckSource(s, GoodArgs()) == Result::Ok);

    // array, samples, bind flags, format
    for (uint32_t n : { 0u, 2u, 6u })
    {
        s = GoodSource();
        s.arraySize = n;
        CHECK(CheckSource(s, GoodArgs()) == Result::ArrayTexture);
    }
    for (uint32_t n : { 0u, 2u, 4u, 8u })
    {
        s = GoodSource();
        s.sampleCount = n;
        CHECK(CheckSource(s, GoodArgs()) == Result::Multisampled);
    }
    s = GoodSource();
    s.bindFlags = D3D11_BIND_DEPTH_STENCIL;
    CHECK(CheckSource(s, GoodArgs()) == Result::NoShaderResource);
    s.bindFlags = 0;
    CHECK(CheckSource(s, GoodArgs()) == Result::NoShaderResource);
    s.bindFlags = D3D11_BIND_SHADER_RESOURCE;
    CHECK(CheckSource(s, GoodArgs()) == Result::Ok);
    s = GoodSource();
    s.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    CHECK(CheckSource(s, GoodArgs()) == Result::UnsupportedFormat);
    s.format = DXGI_FORMAT_UNKNOWN;
    CHECK(CheckSource(s, GoodArgs()) == Result::UnsupportedFormat);
    for (DXGI_FORMAT f : { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R16_TYPELESS,
                           DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D16_UNORM })
    {
        s = GoodSource();
        s.format = f;
        CHECK(CheckSource(s, GoodArgs()) == Result::Ok);
    }

    // the render region lies inside the source (unsigned maths: a base outside must not wrap)
    s = GoodSource(); // 64 x 48
    a = GoodArgs();
    a.baseX = 32;
    a.baseY = 24; // 32 + 32 == 64, 24 + 24 == 48: exactly fits
    CHECK(CheckSource(s, a) == Result::Ok);
    a.baseX = 33;
    CHECK(CheckSource(s, a) == Result::NotContained);
    a.baseX = 32;
    a.baseY = 25;
    CHECK(CheckSource(s, a) == Result::NotContained);
    // base beyond the source: 64 - 80 wraps to a huge value that is >= renderW
    a = GoodArgs();
    a.baseX = 80;
    CHECK(CheckSource(s, a) == Result::NotContained);
    a = GoodArgs();
    a.baseY = 60;
    CHECK(CheckSource(s, a) == Result::NotContained);
    a = GoodArgs();
    a.baseX = 65;
    a.renderW = 1;
    CHECK(CheckSource(s, a) == Result::NotContained);
    a = GoodArgs();
    a.baseX = 64; // base on the edge: nothing left
    a.renderW = 1;
    CHECK(CheckSource(s, a) == Result::NotContained);
    a = GoodArgs();
    a.baseY = 48;
    a.renderH = 1;
    CHECK(CheckSource(s, a) == Result::NotContained);
    a = GoodArgs();
    a.baseX = 0xFFFFFFFFu;
    CHECK(CheckSource(s, a) == Result::NotContained);
    a = GoodArgs();
    a.baseY = 0xFFFFFFFFu;
    CHECK(CheckSource(s, a) == Result::NotContained);
    a = GoodArgs();
    a.renderW = 65; // wider than the source
    CHECK(CheckSource(s, a) == Result::NotContained);
    a = GoodArgs();
    a.baseX = a.baseY = 0;
    a.renderH = 49;
    CHECK(CheckSource(s, a) == Result::NotContained);

    // everything at the limit
    s = GoodSource();
    s.width = s.height = kMaxEdge;
    a = GoodArgs();
    a.baseX = a.baseY = 0;
    a.renderW = a.renderH = a.outputW = a.outputH = kMaxEdge;
    CHECK(CheckSource(s, a) == Result::Ok);
    a.baseX = 1;
    CHECK(CheckSource(s, a) == Result::NotContained);
}

void Names()
{
    std::vector<std::string> seen;
    for (int i = 0; i <= int(Result::Fault); ++i)
    {
        const char* n = ResultName(Result(i));
        CHECK(n != nullptr && n[0] != '\0');
        if (n != nullptr)
        {
            CHECK(std::find(seen.begin(), seen.end(), n) == seen.end());
            seen.push_back(n);
        }
    }
}

void Constants()
{
    CaptureArgs a = GoodArgs(); // origin 3,2  render 32,24  output 64,48
    CropConstants k = MakeConstants(a);
    CHECK(k.origin == (3u | (2u << 16)));
    CHECK(k.render == (32u | (24u << 16)));
    CHECK(k.output == (64u | (48u << 16)));
    CHECK(k.reserved == 0);
    a.baseX = 0x3FFF;
    a.baseY = 0x4000;
    a.renderW = 0x4000;
    a.renderH = 1;
    a.outputW = 1;
    a.outputH = 0x4000;
    k = MakeConstants(a);
    CHECK(k.origin == 0x40003FFFu);
    CHECK(k.render == 0x00014000u);
    CHECK(k.output == 0x40000001u);
    CHECK(PackPair(1, 2) == 0x00020001u);
    CHECK(PackPair(0x20005, 1) == 0x00010005u); // the low half keeps 16 bits only: bits above them do not reach the high half
}

void Replaced()
{
    int a = 0, b = 0;
    CHECK(!DeviceReplaced(&a, &a, S_OK));
    CHECK(DeviceReplaced(&a, &b, S_OK));
    CHECK(DeviceReplaced(nullptr, &a, S_OK)); // nothing set up yet
    CHECK(DeviceReplaced(nullptr, nullptr, S_OK));
    CHECK(!DeviceReplaced(&a, &a, S_FALSE)); // a success code that is not S_OK
    CHECK(DeviceReplaced(&a, &a, DXGI_ERROR_DEVICE_REMOVED));
    CHECK(DeviceReplaced(&a, &a, DXGI_ERROR_DEVICE_HUNG));
    CHECK(DeviceReplaced(&a, &a, E_FAIL));
    CHECK(DeviceReplaced(&a, nullptr, S_OK));
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

// The binder's side of the sink, as far as it matters here: it keeps the view of the capture it holds (the real one AddRefs it
// per pass). `heldRefs` is the reference count of the view it held BEFORE the call, read inside the call: 2 = the pass still holds
// its own reference, 1 = the pass has let go already.
struct Recorder final : ICaptureSink
{
    struct Event
    {
        bool valid = false;
        DepthCapture capture;
        ULONG heldRefs = 0;
        uint64_t heldView = 0;
    };
    std::vector<Event> events;
    ComPtr<ID3D11ShaderResourceView> held;

    void Publish(const DepthCapture& c) noexcept override
    {
        Event e;
        e.valid = c.valid;
        e.capture = c;
        e.heldRefs = held ? RefCount(held.Get()) : 0;
        e.heldView = reinterpret_cast<uintptr_t>(held.Get());
        events.push_back(e);
        ComPtr<ID3D11ShaderResourceView> next;
        if (c.valid && c.copyView != 0)
            next = reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<uintptr_t>(c.copyView));
        held = next;
    }
    size_t Valid() const
    {
        size_t n = 0;
        for (const Event& e : events)
            n += e.valid ? 1 : 0;
        return n;
    }
};

struct DrawProbe
{
    int draws = 0;
    int mode = 0; // 0 normal, 1 C++ exception, 2 access violation
    bool sawBypass = false;
};
DrawProbe g_draw;
void InstallUnbindCrashes(ID3D11DeviceContext* c); // below, with the other hooks
void TestDraw(ID3D11DeviceContext* c)
{
    ++g_draw.draws;
    g_draw.sawBypass = ext::core::HooksBypassed();
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

enum class Kind { F32, Unorm24, Unorm16, F32S8 };
struct Family
{
    const char* name;
    DXGI_FORMAT format;
    UINT bytes;
    Kind kind;
    float tolerance;
};
const Family kFamilies[] = {
    { "R32_TYPELESS", DXGI_FORMAT_R32_TYPELESS, 4, Kind::F32, 0.0f },
    { "R32_FLOAT", DXGI_FORMAT_R32_FLOAT, 4, Kind::F32, 0.0f },
    { "R24G8_TYPELESS", DXGI_FORMAT_R24G8_TYPELESS, 4, Kind::Unorm24, 1e-6f },
    { "R16_TYPELESS", DXGI_FORMAT_R16_TYPELESS, 2, Kind::Unorm16, 1e-6f },
    { "R16_UNORM", DXGI_FORMAT_R16_UNORM, 2, Kind::Unorm16, 1e-6f },
    { "R32G8X24_TYPELESS", DXGI_FORMAT_R32G8X24_TYPELESS, 8, Kind::F32S8, 0.0f },
};

// Every source texel has its own value: index = y * 128 + x + 1 (sources are at most 128 x 80).
uint32_t Index(uint32_t x, uint32_t y) { return y * 128 + x + 1; }
float Expected(Kind kind, uint32_t index)
{
    switch (kind)
    {
    case Kind::Unorm24:
        return float(index * 1600u + 13u) / 16777215.0f; // UNORM24 -> float, the conversion the sampler applies
    case Kind::Unorm16:
        return float(index * 6u + 1u) / 65535.0f;
    default:
        return float(index) / 1024.0f;
    }
}
void Encode(const Family& f, uint32_t index, uint8_t* out)
{
    switch (f.kind)
    {
    case Kind::F32:
    {
        const float v = float(index) / 1024.0f;
        std::memcpy(out, &v, 4);
        break;
    }
    case Kind::Unorm24:
    {
        const uint32_t v = (index * 1600u + 13u) | ((index & 0xFFu) << 24); // depth in the low 24 bits, stencil above
        std::memcpy(out, &v, 4);
        break;
    }
    case Kind::Unorm16:
    {
        const uint16_t v = uint16_t(index * 6u + 1u);
        std::memcpy(out, &v, 2);
        break;
    }
    case Kind::F32S8:
    {
        const float v = float(index) / 1024.0f;
        const uint32_t stencil = index & 0xFFu;
        std::memcpy(out, &v, 4);
        std::memcpy(out + 4, &stencil, 4);
        break;
    }
    }
}

ComPtr<ID3D11Texture2D> MakeSource(Rig& rig, const Family& f, UINT w, UINT h, UINT bind = D3D11_BIND_SHADER_RESOURCE)
{
    std::vector<uint8_t> data(size_t(w) * h * f.bytes);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
            Encode(f, Index(x, y), &data[(size_t(y) * w + x) * f.bytes]);
    D3D11_TEXTURE2D_DESC td {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = f.format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init { data.data(), w * f.bytes, 0 };
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(rig.device->CreateTexture2D(&td, &init, &tex)))
        std::printf("  could not create a %s source %ux%u\n", f.name, w, h);
    return tex;
}

struct Image
{
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bind = 0;
    std::vector<float> px;
};
Image ReadTexture(Rig& rig, ID3D11Texture2D* tex)
{
    Image img;
    D3D11_TEXTURE2D_DESC td {};
    tex->GetDesc(&td);
    img.w = td.Width;
    img.h = td.Height;
    img.format = td.Format;
    img.bind = td.BindFlags;
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
    img.px.resize(size_t(img.w) * img.h);
    for (UINT y = 0; y < img.h; ++y)
        std::memcpy(&img.px[size_t(y) * img.w], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(img.w) * 4);
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
Image ReadView(Rig& rig, uint64_t view)
{
    ComPtr<ID3D11Texture2D> tex = TextureOf(ViewPtr(view));
    return tex != nullptr ? ReadTexture(rig, tex.Get()) : Image {};
}

struct Fixture
{
    Rig rig;
    Recorder sink;
    std::unique_ptr<DepthCopy> pass;
    bool Create(UINT flags = 0, D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0)
    {
        if (!rig.Create(flags, level))
            return false;
        pass = std::make_unique<DepthCopy>(sink, &TestDraw);
        g_draw = DrawProbe {};
        return true;
    }
    bool Run(ID3D11Resource* source, const CaptureArgs& args, CaptureReport* report = nullptr)
    {
        return pass->Capture(rig.context.Get(), source, args, report);
    }
    uint64_t LastView() const { return sink.events.empty() ? 0 : sink.events.back().capture.copyView; }
};

CaptureArgs Args(uint32_t ow, uint32_t oh, uint32_t bx, uint32_t by, uint32_t rw, uint32_t rh, uint64_t frame = 7, int64_t tick = 1000)
{
    CaptureArgs a;
    a.outputW = ow;
    a.outputH = oh;
    a.baseX = bx;
    a.baseY = by;
    a.renderW = rw;
    a.renderH = rh;
    a.frame = frame;
    a.tickMs = tick;
    return a;
}

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
    uint32_t srcW, srcH, bx, by, rw, rh, ow, oh;
};
const Case kCases[] = {
    { "identity", 8, 6, 0, 0, 8, 6, 8, 6 },
    { "identity with origin", 10, 8, 3, 2, 5, 4, 5, 4 },
    { "5x4 -> 8x6 with origin (2:3 on y)", 12, 9, 3, 2, 5, 4, 8, 6 },
    { "6x4 -> 9x6 (2:3 on both)", 10, 8, 2, 3, 6, 4, 9, 6 },
    { "2258x1270 -> 3840x2160 scaled down", 24, 16, 4, 3, 17, 10, 29, 17 },
    { "2x", 8, 6, 2, 1, 4, 3, 8, 6 },
    { "downscale", 12, 10, 1, 1, 9, 8, 4, 3 },
    { "one texel", 6, 5, 4, 3, 1, 1, 5, 4 },
    { "one pixel", 9, 7, 1, 1, 7, 5, 1, 1 },
    { "wide", 128, 20, 5, 3, 120, 11, 133, 17 },
    { "tall", 20, 80, 3, 7, 13, 70, 17, 79 },
    { "3:2 ratio rows", 8, 12, 0, 0, 3, 9, 2, 6 },
};

void CheckImage(const char* what, const Family& fam, const Case& c, const Image& img, bool identity)
{
    CHECK(img.w == c.ow && img.h == c.oh);
    if (img.w != c.ow || img.h != c.oh || img.px.size() != size_t(c.ow) * c.oh)
        return;
    for (uint32_t y = 0; y < c.oh; ++y)
        for (uint32_t x = 0; x < c.ow; ++x)
        {
            const uint32_t sx = c.bx + RefSrc(x, c.rw, c.ow);
            const uint32_t sy = c.by + RefSrc(y, c.rh, c.oh);
            float want = Expected(fam.kind, Index(sx, sy));
            if (identity) // independent brute force: pixel i reads source pixel base + i
                want = Expected(fam.kind, Index(c.bx + x, c.by + y));
            const float got = img.px[size_t(y) * c.ow + x];
            if (std::fabs(got - want) > fam.tolerance)
            {
                std::printf("CHECK FAILED %s:%d: %s / %s / %s: pixel (%u,%u) = %.9g, want %.9g (source %u,%u)\n", __FILE__, __LINE__, what, fam.name,
                            c.name, x, y, got, want, sx, sy);
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
    for (const Family& fam : kFamilies)
    {
        Fixture f;
        if (!f.Create())
            return;
        for (const Case& c : kCases)
        {
            auto src = MakeSource(f.rig, fam, c.srcW, c.srcH);
            CHECK(src != nullptr);
            if (src == nullptr)
                continue;
            CaptureReport report;
            const bool ok = f.Run(src.Get(), Args(c.ow, c.oh, c.bx, c.by, c.rw, c.rh), &report);
            if (!ok)
                std::printf("  %s / %s: Capture refused (%s)\n", fam.name, c.name, ResultName(report.result));
            CHECK(ok);
            if (!ok)
                continue;
            const bool identity = c.rw == c.ow && c.rh == c.oh;
            CheckImage("capture", fam, c, ReadView(f.rig, f.LastView()), identity);
        }
    }
}

// The last row and column read the last render texel, the first ones the first: stated outright for one ratio
void EdgesExplicit()
{
    Fixture f;
    if (!f.Create())
        return;
    const Family& fam = kFamilies[0];
    auto src = MakeSource(f.rig, fam, 20, 14);
    CHECK(f.Run(src.Get(), Args(8, 6, 3, 2, 5, 4)));
    const Image img = ReadView(f.rig, f.LastView());
    CHECK(img.w == 8 && img.h == 6 && img.px.size() == 48);
    if (img.px.size() != 48)
        return;
    CHECK(img.px[0] == Expected(Kind::F32, Index(3, 2)));
    CHECK(img.px[7] == Expected(Kind::F32, Index(3 + 4, 2)));                  // last column -> render column 4
    CHECK(img.px[5 * 8] == Expected(Kind::F32, Index(3, 2 + 3)));              // last row -> render row 3
    CHECK(img.px[5 * 8 + 7] == Expected(Kind::F32, Index(3 + 4, 2 + 3)));
    // 5 -> 8 on x: 0 0 1 2 2 3 4 4; 4 -> 6 on y: 0 1 1 2 3 3 (rows 1 and 4 sit exactly on a texel border: (2i + 1) * 4 / 12)
    const uint32_t xs[8] = { 0, 0, 1, 2, 2, 3, 4, 4 };
    const uint32_t ys[6] = { 0, 1, 1, 2, 3, 3 };
    for (uint32_t y = 0; y < 6; ++y)
        for (uint32_t x = 0; x < 8; ++x)
            CHECK(img.px[y * 8 + x] == Expected(Kind::F32, Index(3 + xs[x], 2 + ys[y])));
}

// The game's depth buffers: bound as depth-stencil and as a shader resource, cleared to one value
void DepthStencilSources()
{
    struct Row
    {
        const char* name;
        DXGI_FORMAT tex, dsv;
        float tolerance;
    };
    const Row rows[] = {
        { "R32_TYPELESS/D32_FLOAT", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, 0.0f },
        { "R24G8_TYPELESS/D24_UNORM_S8_UINT", DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, 1e-6f },
        { "R16_TYPELESS/D16_UNORM", DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, 3e-5f },
        { "R32G8X24_TYPELESS/D32_FLOAT_S8X24_UINT", DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 0.0f },
    };
    for (const Row& row : rows)
    {
        Fixture f;
        if (!f.Create())
            return;
        D3D11_TEXTURE2D_DESC td {};
        td.Width = 16;
        td.Height = 12;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = row.tex;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11DepthStencilView> dsv;
        D3D11_DEPTH_STENCIL_VIEW_DESC dd {};
        dd.Format = row.dsv;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        CHECK(SUCCEEDED(f.rig.device->CreateTexture2D(&td, nullptr, &tex)));
        CHECK(SUCCEEDED(f.rig.device->CreateDepthStencilView(tex.Get(), &dd, &dsv)));
        if (tex == nullptr || dsv == nullptr)
            continue;
        f.rig.context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.25f, 3);
        CaptureReport report;
        const bool ok = f.Run(tex.Get(), Args(10, 8, 2, 1, 8, 6), &report);
        if (!ok)
            std::printf("  %s refused: %s\n", row.name, ResultName(report.result));
        CHECK(ok);
        if (!ok)
            continue;
        CHECK(report.source.bindFlags == (D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE));
        const Image img = ReadView(f.rig, f.LastView());
        CHECK(img.px.size() == 80);
        bool allSame = img.px.size() == 80;
        for (float v : img.px)
            allSame = allSame && std::fabs(v - 0.25f) <= row.tolerance + 1e-6f;
        if (!allSame)
            std::printf("  %s: first pixel %.9g, want 0.25\n", row.name, img.px.empty() ? -1.0f : img.px[0]);
        CHECK(allSame);
    }
}

// ---- the game's context state --------------------------------------------------------------------------------------------
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
        bool ok = SUCCEEDED(d->CreateVertexShader(DepthCrop_Vs_cso, sizeof(DepthCrop_Vs_cso), nullptr, &vs)) &&
                  SUCCEEDED(d->CreatePixelShader(DepthCrop_Ps_cso, sizeof(DepthCrop_Ps_cso), nullptr, &ps));
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

void ContextState()
{
    for (UINT flags : { 0u, UINT(D3D11_CREATE_DEVICE_SINGLETHREADED) })
    {
        Fixture f;
        if (!f.Create(flags))
            return;
        GameState game;
        CHECK(game.Set(f.rig));
        auto src = MakeSource(f.rig, kFamilies[0], 16, 12);
        const Dump before = DumpState(f.rig.context.Get());
        CHECK(before.size() > 40);
        CaptureReport report;
        CHECK(f.Run(src.Get(), Args(10, 8, 2, 1, 8, 6), &report));
        CHECK(report.result == Result::Ok);
        CHECK(g_draw.draws == 1);
        CHECK(SameState(flags != 0 ? "single-threaded device, after a capture" : "after a capture", before, DumpState(f.rig.context.Get())));
        // a second capture (target already there)
        CHECK(f.Run(src.Get(), Args(10, 8, 3, 2, 7, 5)));
        CHECK(SameState("after the second capture", before, DumpState(f.rig.context.Get())));
        // the game's view of the shader slots is the game's own, so its next draw still works
        f.rig.context->Draw(3, 0);
        // refusals leave it alone, too
        CHECK(!f.Run(src.Get(), Args(0, 8, 2, 1, 8, 6)));
        CHECK(SameState("after a refusal", before, DumpState(f.rig.context.Get())));
        // a draw that throws, and one that crashes
        for (int mode : { 1, 2 })
        {
            g_draw.mode = mode;
            CaptureReport r;
            CHECK(!f.Run(src.Get(), Args(10, 8, 2, 1, 8, 6), &r));
            CHECK(r.result == Result::Fault);
            CHECK(SameState(mode == 1 ? "after a draw that throws" : "after a draw that crashes", before, DumpState(f.rig.context.Get())));
            g_draw.mode = 0;
        }
        // and the pass still works afterwards
        CHECK(f.Run(src.Get(), Args(10, 8, 2, 1, 8, 6)));
        CHECK(SameState("after recovery", before, DumpState(f.rig.context.Get())));
    }
}

// At NGX evaluate time the game's own state may still have the depth bound (as the depth-stencil target, or read-only together with a
// shader resource view in a pixel shader slot). The pass reads it from its private state and must neither disturb those bindings nor
// read an unbound (all zero) view.
void SourceBoundInGameState()
{
    struct Row
    {
        const char* name;
        DXGI_FORMAT tex, dsv;
        UINT readOnly;
        float tolerance;
    };
    const Row rows[] = {
        { "R32_TYPELESS/D32_FLOAT", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, D3D11_DSV_READ_ONLY_DEPTH, 0.0f },
        { "R24G8_TYPELESS/D24_UNORM_S8_UINT", DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT,
          D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL, 1e-6f },
    };
    for (const Row& row : rows)
        for (int readOnly = 0; readOnly < 2; ++readOnly)
        {
            Fixture f;
            if (!f.Create())
                return;
            D3D11_TEXTURE2D_DESC td {};
            td.Width = 16;
            td.Height = 12;
            td.MipLevels = td.ArraySize = 1;
            td.Format = row.tex;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> tex;
            ComPtr<ID3D11DepthStencilView> writable, readOnlyView;
            ComPtr<ID3D11ShaderResourceView> srv;
            D3D11_DEPTH_STENCIL_VIEW_DESC dd {};
            dd.Format = row.dsv;
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            D3D11_DEPTH_STENCIL_VIEW_DESC ro = dd;
            ro.Flags = row.readOnly;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd {};
            sd.Format = SrvFormatFor(row.tex);
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            CHECK(SUCCEEDED(f.rig.device->CreateTexture2D(&td, nullptr, &tex)));
            if (tex == nullptr)
                continue;
            CHECK(SUCCEEDED(f.rig.device->CreateDepthStencilView(tex.Get(), &dd, &writable)));
            CHECK(SUCCEEDED(f.rig.device->CreateDepthStencilView(tex.Get(), &ro, &readOnlyView)));
            CHECK(SUCCEEDED(f.rig.device->CreateShaderResourceView(tex.Get(), &sd, &srv)));
            if (writable == nullptr || readOnlyView == nullptr || srv == nullptr)
                continue;
            f.rig.context->ClearDepthStencilView(writable.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.25f, 3);

            GameState game; // colour target, shaders, buffers and the other states as in the state test
            CHECK(game.Set(f.rig));
            // a colour target of the depth buffer's size (the two must agree for the output merger)
            D3D11_TEXTURE2D_DESC cd = td;
            cd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            cd.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> colour;
            ComPtr<ID3D11RenderTargetView> colourView;
            CHECK(SUCCEEDED(f.rig.device->CreateTexture2D(&cd, nullptr, &colour)));
            if (colour == nullptr || FAILED(f.rig.device->CreateRenderTargetView(colour.Get(), nullptr, &colourView)))
                continue;
            ID3D11RenderTargetView* rtvs[1] = { colourView.Get() };
            ID3D11ShaderResourceView* srvs[1] = { srv.Get() };
            if (readOnly != 0)
            {
                f.rig.context->OMSetRenderTargets(1, rtvs, readOnlyView.Get());
                f.rig.context->PSSetShaderResources(3, 1, srvs);
            }
            else
                f.rig.context->OMSetRenderTargets(1, rtvs, writable.Get());
            const Dump before = DumpState(f.rig.context.Get());
            uint64_t boundDsv = 0, boundSrv = 0;
            for (const auto& entry : before)
            {
                if (entry.first == "dsv")
                    boundDsv = entry.second;
                if (entry.first == "psSrv3")
                    boundSrv = entry.second;
            }
            CHECK(boundDsv != 0);
            CHECK(readOnly == 0 || boundSrv != 0);

            CaptureReport r;
            const bool ok = f.Run(tex.Get(), Args(10, 8, 2, 1, 8, 6), &r);
            if (!ok)
                std::printf("  %s (%s): refused: %s\n", row.name, readOnly != 0 ? "read-only view + shader resource view" : "writable view",
                            ResultName(r.result));
            CHECK(ok);
            const char* what = readOnly != 0 ? "depth bound read-only with a shader resource view" : "depth bound as the depth-stencil target";
            CHECK(SameState(what, before, DumpState(f.rig.context.Get())));
            if (!ok)
                continue;
            const Image img = ReadView(f.rig, f.LastView());
            bool allSame = img.px.size() == 80;
            for (float v : img.px)
                allSame = allSame && std::fabs(v - 0.25f) <= row.tolerance + 1e-6f;
            if (!allSame)
                std::printf("  %s (%s): first pixel %.9g, want 0.25\n", row.name, readOnly != 0 ? "read-only" : "writable",
                            img.px.empty() ? -1.0f : img.px[0]);
            CHECK(allSame);
        }
}

void BypassScopeIsOn()
{
    Fixture f;
    if (!f.Create())
        return;
    auto src = MakeSource(f.rig, kFamilies[0], 16, 12);
    CHECK(!ext::core::HooksBypassed());
    CHECK(f.Run(src.Get(), Args(10, 8, 2, 1, 8, 6)));
    CHECK(g_draw.draws == 1 && g_draw.sawBypass);
    CHECK(!ext::core::HooksBypassed()); // the scope ended with the call
    for (int mode : { 1, 2 }) // ... also on the way out of a C++ exception and of an access violation
    {
        g_draw.mode = mode;
        g_draw.sawBypass = false;
        CHECK(!f.Run(src.Get(), Args(10, 8, 2, 1, 8, 6)));
        CHECK(g_draw.sawBypass && !ext::core::HooksBypassed());
    }
    g_draw.mode = 0;
    {
        ext::core::BypassScope outer; // an outer scope is not switched off by the pass
        CHECK(f.Run(src.Get(), Args(10, 8, 2, 1, 8, 6)));
        CHECK(ext::core::HooksBypassed());
    }
}

// ---- the publish contract ------------------------------------------------------------------------------------------------
void PublishedFields()
{
    Fixture f;
    if (!f.Create())
        return;
    auto src = MakeSource(f.rig, kFamilies[0], 16, 12);
    CaptureReport report;
    CHECK(f.Run(src.Get(), Args(10, 8, 2, 1, 8, 6, 12345, 987654), &report));
    CHECK(f.sink.events.size() == 1);
    if (f.sink.events.size() != 1)
        return;
    const DepthCapture& c = f.sink.events[0].capture;
    CHECK(c.valid);
    CHECK(c.outputW == 10 && c.outputH == 8);
    CHECK(c.renderW == 8 && c.renderH == 6);
    CHECK(c.frame == 12345);
    CHECK(c.tickMs == 987654);
    CHECK(c.copyView != 0);
    CHECK(c.device == reinterpret_cast<uintptr_t>(f.rig.device.Get()));
    CHECK(f.sink.events[0].heldRefs == 0);

    // the copy: an R32_FLOAT texture of the output size, render target and shader resource
    const Image img = ReadView(f.rig, c.copyView);
    CHECK(img.w == 10 && img.h == 8);
    CHECK(img.format == DXGI_FORMAT_R32_FLOAT);
    CHECK((img.bind & D3D11_BIND_RENDER_TARGET) != 0 && (img.bind & D3D11_BIND_SHADER_RESOURCE) != 0);

    // the report: what the log line needs
    CHECK(report.result == Result::Ok);
    CHECK(report.source.width == 16 && report.source.height == 12);
    CHECK(report.source.format == DXGI_FORMAT_R32_TYPELESS);
    CHECK(report.source.bindFlags == D3D11_BIND_SHADER_RESOURCE);
    CHECK(report.source.arraySize == 1 && report.source.sampleCount == 1);
    CHECK(report.viewFormat == DXGI_FORMAT_R32_FLOAT);
    CHECK(report.args.baseX == 2 && report.args.baseY == 1 && report.args.renderW == 8 && report.args.renderH == 6);
    CHECK(report.args.outputW == 10 && report.args.outputH == 8);
    CHECK(report.targetCreated);
    CaptureReport second;
    CHECK(f.Run(src.Get(), Args(10, 8, 2, 1, 8, 6, 12346, 987700), &second));
    CHECK(!second.targetCreated);
    CHECK(f.sink.events.size() == 2 && f.sink.events[1].capture.frame == 12346 && f.sink.events[1].capture.tickMs == 987700);
}

void RecreateOnlyOnOutputSize()
{
    Fixture f;
    if (!f.Create())
        return;
    auto src = MakeSource(f.rig, kFamilies[0], 40, 30);
    CaptureReport r;
    CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12), &r));
    CHECK(r.targetCreated);
    const ComPtr<ID3D11Texture2D> first = TextureOf(ViewPtr(f.LastView()));
    CHECK(first != nullptr);
    if (first == nullptr)
        return;
    // another origin, another render size (the quality changed): the same target, only the constants change
    CHECK(f.Run(src.Get(), Args(16, 12, 4, 3, 16, 12), &r));
    CHECK(!r.targetCreated);
    CHECK(f.Run(src.Get(), Args(16, 12, 4, 3, 10, 7), &r));
    CHECK(!r.targetCreated);
    CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 33, 25), &r));
    CHECK(!r.targetCreated);
    ComPtr<ID3D11Texture2D> now = TextureOf(ViewPtr(f.LastView()));
    CHECK(now.Get() == first.Get());
    // and the contents follow the constants: the 10x7 request read other texels than the 16x12 one
    CHECK(f.Run(src.Get(), Args(16, 12, 4, 3, 10, 7), &r));
    const Image mapped = ReadView(f.rig, f.LastView());
    CHECK(mapped.px.size() == 192 && mapped.px[0] == Expected(Kind::F32, Index(4, 3)));
    // either dimension of the output recreates it
    CHECK(f.Run(src.Get(), Args(17, 12, 0, 0, 16, 12), &r));
    CHECK(r.targetCreated);
    now = TextureOf(ViewPtr(f.LastView()));
    CHECK(now != nullptr && now.Get() != first.Get());
    if (now == nullptr)
        return;
    D3D11_TEXTURE2D_DESC td {};
    now->GetDesc(&td);
    CHECK(td.Width == 17 && td.Height == 12);
    CHECK(f.Run(src.Get(), Args(17, 12, 0, 0, 16, 12), &r));
    CHECK(!r.targetCreated);
    CHECK(f.Run(src.Get(), Args(17, 13, 0, 0, 16, 12), &r));
    CHECK(r.targetCreated);
    now = TextureOf(ViewPtr(f.LastView()));
    if (now == nullptr)
        return;
    now->GetDesc(&td);
    CHECK(td.Width == 17 && td.Height == 13);
}

// Path 1: the output size changed. The new capture is published while the pass still holds the old view.
void OrderOnRecreate()
{
    Fixture f;
    if (!f.Create())
        return;
    auto src = MakeSource(f.rig, kFamilies[0], 40, 30);
    CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12)));
    auto* oldView = ViewPtr(f.LastView());
    CHECK(oldView != nullptr);
    if (oldView == nullptr)
        return;
    oldView->AddRef(); // the test's own reference, to look at the old view afterwards
    CHECK(f.Run(src.Get(), Args(20, 12, 0, 0, 16, 12)));
    CHECK(f.sink.events.size() == 2);
    if (f.sink.events.size() == 2)
    {
        const Recorder::Event& e = f.sink.events[1];
        CHECK(e.valid);
        CHECK(e.heldView == reinterpret_cast<uintptr_t>(oldView));
        CHECK(e.heldRefs == 3); // the pass, the sink, the test: the pass had not let go of the old view when the new one was published
        CHECK(e.capture.copyView != reinterpret_cast<uintptr_t>(oldView));
    }
    CHECK(RefCount(oldView) == 1); // ... and has by now (the sink moved on to the new view)
    oldView->Release();
}

// Path 2: Release()
void OrderOnRelease()
{
    Fixture f;
    if (!f.Create())
        return;
    auto src = MakeSource(f.rig, kFamilies[0], 40, 30);
    f.pass->Release(); // nothing published yet: nothing to withdraw
    CHECK(f.sink.events.empty());
    CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12)));
    auto* view = ViewPtr(f.LastView());
    CHECK(view != nullptr);
    if (view == nullptr)
        return;
    view->AddRef();
    f.pass->Release();
    CHECK(f.sink.events.size() == 2);
    if (f.sink.events.size() == 2)
    {
        const Recorder::Event& e = f.sink.events[1];
        CHECK(!e.valid);
        CHECK(e.heldView == reinterpret_cast<uintptr_t>(view));
        CHECK(e.heldRefs == 3); // withdrawn while the pass still held the view
    }
    CHECK(RefCount(view) == 1); // released afterwards: only the test's reference is left
    CHECK(!f.pass->Ready());
    view->Release();
    f.pass->Release(); // twice: nothing more to say
    CHECK(f.sink.events.size() == 2);
    // the pass comes back on the next capture
    CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12)));
    CHECK(f.sink.events.size() == 3 && f.sink.events[2].valid);
}

// The destructor is a Release()
void OrderOnDestroy()
{
    Fixture f;
    if (!f.Create())
        return;
    auto src = MakeSource(f.rig, kFamilies[0], 40, 30);
    CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12)));
    auto* view = ViewPtr(f.LastView());
    CHECK(view != nullptr);
    if (view == nullptr)
        return;
    view->AddRef();
    f.pass.reset();
    CHECK(f.sink.events.size() == 2);
    if (f.sink.events.size() == 2)
    {
        CHECK(!f.sink.events[1].valid);
        CHECK(f.sink.events[1].heldRefs == 3);
    }
    CHECK(RefCount(view) == 1);
    view->Release();
}

// Path 3: another device. The old capture is withdrawn before anything of the old device goes, then the new one is published.
void OrderOnDeviceChange()
{
    Fixture f;
    Rig second;
    if (!f.Create() || !second.Create())
        return;
    auto srcA = MakeSource(f.rig, kFamilies[0], 40, 30);
    auto srcB = MakeSource(second, kFamilies[0], 40, 30);
    CHECK(f.Run(srcA.Get(), Args(16, 12, 0, 0, 16, 12)));
    auto* viewA = ViewPtr(f.LastView());
    CHECK(viewA != nullptr);
    if (viewA == nullptr)
        return;
    viewA->AddRef();
    CaptureReport r;
    CHECK(f.pass->Capture(second.context.Get(), srcB.Get(), Args(16, 12, 0, 0, 16, 12), &r));
    CHECK(r.result == Result::Ok && r.targetCreated);
    CHECK(f.sink.events.size() == 3);
    if (f.sink.events.size() == 3)
    {
        CHECK(f.sink.events[0].valid && f.sink.events[0].capture.device == reinterpret_cast<uintptr_t>(f.rig.device.Get()));
        CHECK(!f.sink.events[1].valid);
        CHECK(f.sink.events[1].heldView == reinterpret_cast<uintptr_t>(viewA));
        CHECK(f.sink.events[1].heldRefs == 3); // withdrawn while the pass still held the view of the old device
        CHECK(f.sink.events[2].valid && f.sink.events[2].capture.device == reinterpret_cast<uintptr_t>(second.device.Get()));
        CHECK(f.sink.events[2].capture.copyView != reinterpret_cast<uintptr_t>(viewA));
        CHECK(f.sink.events[2].heldRefs == 0);
    }
    CHECK(RefCount(viewA) == 1);
    viewA->Release();
    // the new capture is a real one
    const Image img = ReadView(second, f.LastView());
    CHECK(img.w == 16 && img.h == 12 && img.px.size() == 192 && img.px[0] == Expected(Kind::F32, Index(0, 0)));
    // back to the first device: another switch
    CHECK(f.Run(srcA.Get(), Args(16, 12, 0, 0, 16, 12)));
    CHECK(f.sink.events.size() == 5 && !f.sink.events[3].valid && f.sink.events[4].valid);
}

// A consumer that goes away
void WithdrawKeepsResources()
{
    Fixture f;
    if (!f.Create())
        return;
    auto src = MakeSource(f.rig, kFamilies[0], 40, 30);
    f.pass->Withdraw(); // nothing published
    CHECK(f.sink.events.empty());
    CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12)));
    auto* view = ViewPtr(f.LastView());
    CHECK(view != nullptr);
    if (view == nullptr)
        return;
    f.pass->Withdraw();
    CHECK(f.sink.events.size() == 2 && !f.sink.events[1].valid && f.sink.events[1].heldRefs == 2);
    CHECK(f.pass->Ready());
    f.pass->Withdraw(); // already withdrawn
    CHECK(f.sink.events.size() == 2);
    CaptureReport r;
    CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12), &r));
    CHECK(!r.targetCreated); // the target is still there
    CHECK(f.sink.events.size() == 3 && f.sink.events[2].valid);
    CHECK(f.LastView() == reinterpret_cast<uintptr_t>(view));
}

// A failure after the target was touched withdraws the capture that is published; a refusal does not
void FailureWithdraws()
{
    Fixture f;
    if (!f.Create())
        return;
    auto src = MakeSource(f.rig, kFamilies[0], 40, 30);
    // nothing published yet: a failing draw publishes nothing at all
    g_draw.mode = 1;
    CaptureReport r;
    CHECK(!f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12), &r));
    CHECK(r.result == Result::Fault);
    CHECK(f.sink.events.empty());
    g_draw.mode = 0;
    CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12)));
    CHECK(f.sink.events.size() == 1);
    // a refusal leaves the published capture to age out
    CHECK(!f.Run(src.Get(), Args(16, 12, 0, 0, 0, 12), &r));
    CHECK(r.result == Result::BadRenderSize);
    CHECK(f.sink.events.size() == 1);
    // an exception in the draw withdraws it
    g_draw.mode = 1;
    CHECK(!f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12), &r));
    CHECK(r.result == Result::Fault);
    CHECK(f.sink.events.size() == 2 && !f.sink.events[1].valid && f.sink.events[1].heldRefs == 2);
    // so does an access violation, and only once
    g_draw.mode = 0;
    CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12)));
    CHECK(f.sink.events.size() == 3 && f.sink.events[2].valid);
    g_draw.mode = 2;
    CHECK(!f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12), &r));
    CHECK(f.sink.events.size() == 4 && !f.sink.events[3].valid);
    CHECK(!f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12), &r));
    CHECK(f.sink.events.size() == 4);
    // a fault at a new output size: the old capture is withdrawn while the pass still holds its view, and the pass recovers
    g_draw.mode = 0;
    CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12)));
    CHECK(f.sink.events.size() == 5 && f.sink.events[4].valid);
    g_draw.mode = 1;
    CHECK(!f.Run(src.Get(), Args(20, 12, 0, 0, 16, 12), &r));
    CHECK(r.result == Result::Fault);
    CHECK(!r.targetCreated); // the new target was discarded
    CHECK(f.sink.events.size() == 6 && !f.sink.events[5].valid && f.sink.events[5].heldRefs == 2);
    g_draw.mode = 0;
    CHECK(f.Run(src.Get(), Args(20, 12, 0, 0, 16, 12)));
    CHECK(f.sink.events.size() == 7 && f.sink.events[6].valid);
    const Image img = ReadView(f.rig, f.LastView());
    CHECK(img.w == 20 && img.h == 12);
}

// ---- refusals ------------------------------------------------------------------------------------------------------------
ComPtr<ID3D11Texture2D> MakeTexture(Rig& rig, DXGI_FORMAT format, UINT w, UINT h, UINT bind, UINT array = 1, UINT samples = 1)
{
    D3D11_TEXTURE2D_DESC td {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = array;
    td.Format = format;
    td.SampleDesc.Count = samples;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    ComPtr<ID3D11Texture2D> tex;
    rig.device->CreateTexture2D(&td, nullptr, &tex);
    return tex;
}

void Refusals()
{
    Fixture f;
    if (!f.Create())
        return;
    const CaptureArgs args = Args(16, 12, 2, 1, 12, 9);
    auto good = MakeSource(f.rig, kFamilies[0], 16, 12);
    CaptureReport r;
    auto expectRefused = [&](const char* what, ID3D11Resource* src, Result want, const CaptureArgs& a)
    {
        const int draws = g_draw.draws;
        const size_t events = f.sink.events.size();
        const bool ok = f.Run(src, a, &r);
        if (ok || r.result != want || g_draw.draws != draws || f.sink.events.size() != events)
            std::printf("  refusal '%s': ok=%d result=%s want=%s draws=%d events=%zu\n", what, ok ? 1 : 0, ResultName(r.result),
                        ResultName(want), g_draw.draws - draws, f.sink.events.size() - events);
        CHECK(!ok);
        CHECK(r.result == want);
        CHECK(g_draw.draws == draws);            // nothing was drawn
        CHECK(f.sink.events.size() == events);  // nothing was published
    };
    // a published capture exists before the refusals: they leave it alone (it ages out)
    CHECK(f.Run(good.Get(), args));
    CHECK(f.sink.events.size() == 1);

    auto msaa = MakeTexture(f.rig, DXGI_FORMAT_R32_TYPELESS, 16, 12, D3D11_BIND_SHADER_RESOURCE, 1, 4);
    if (msaa)
    {
        expectRefused("multisampled", msaa.Get(), Result::Multisampled, args);
        CHECK(r.source.sampleCount == 4);
    }
    else
        std::puts("  note: no 4x MSAA R32_TYPELESS on this adapter, the multisample refusal is pinned by the pure test only");
    auto array = MakeTexture(f.rig, DXGI_FORMAT_R32_TYPELESS, 16, 12, D3D11_BIND_SHADER_RESOURCE, 2);
    CHECK(array != nullptr);
    expectRefused("array", array.Get(), Result::ArrayTexture, args);
    CHECK(r.source.arraySize == 2);
    auto noSrv = MakeTexture(f.rig, DXGI_FORMAT_R32_TYPELESS, 16, 12, D3D11_BIND_DEPTH_STENCIL);
    CHECK(noSrv != nullptr);
    expectRefused("no shader resource", noSrv.Get(), Result::NoShaderResource, args);
    auto colour = MakeTexture(f.rig, DXGI_FORMAT_R8G8B8A8_UNORM, 16, 12, D3D11_BIND_SHADER_RESOURCE);
    CHECK(colour != nullptr);
    expectRefused("unsupported format", colour.Get(), Result::UnsupportedFormat, args);
    CHECK(r.source.format == DXGI_FORMAT_R8G8B8A8_UNORM && r.viewFormat == DXGI_FORMAT_UNKNOWN);
    expectRefused("subrect not contained", good.Get(), Result::NotContained, Args(16, 12, 5, 1, 12, 9));
    expectRefused("subrect not contained (y)", good.Get(), Result::NotContained, Args(16, 12, 2, 4, 12, 9));
    expectRefused("base x beyond the source", good.Get(), Result::NotContained, Args(16, 12, 20, 1, 4, 4));
    expectRefused("base y beyond the source", good.Get(), Result::NotContained, Args(16, 12, 2, 40, 4, 4));
    expectRefused("base x on the edge", good.Get(), Result::NotContained, Args(16, 12, 16, 0, 1, 1));
    expectRefused("zero output", good.Get(), Result::BadOutputSize, Args(0, 12, 2, 1, 12, 9));
    expectRefused("zero render", good.Get(), Result::BadRenderSize, Args(16, 12, 2, 1, 0, 9));
    expectRefused("huge output", good.Get(), Result::BadOutputSize, Args(kMaxEdge + 1, 12, 2, 1, 12, 9));

    // not a 2D texture
    D3D11_BUFFER_DESC bd {};
    bd.ByteWidth = 256;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Buffer> buffer;
    CHECK(SUCCEEDED(f.rig.device->CreateBuffer(&bd, nullptr, &buffer)));
    expectRefused("buffer", buffer.Get(), Result::NotTexture2D, args);
    D3D11_TEXTURE3D_DESC t3 {};
    t3.Width = t3.Height = t3.Depth = 4;
    t3.MipLevels = 1;
    t3.Format = DXGI_FORMAT_R32_FLOAT;
    t3.Usage = D3D11_USAGE_DEFAULT;
    t3.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture3D> volume;
    CHECK(SUCCEEDED(f.rig.device->CreateTexture3D(&t3, nullptr, &volume)));
    expectRefused("3D texture", volume.Get(), Result::NotTexture2D, Args(4, 4, 0, 0, 4, 4));

    // no arguments
    expectRefused("no source", nullptr, Result::NullArgument, args);
    {
        const int draws = g_draw.draws;
        CHECK(!f.pass->Capture(nullptr, good.Get(), args, &r));
        CHECK(r.result == Result::NullArgument && g_draw.draws == draws);
    }
    // a report is optional
    CHECK(!f.pass->Capture(f.rig.context.Get(), nullptr, args));

    // pointers that are not objects: guarded, nothing escapes
    expectRefused("garbage source", reinterpret_cast<ID3D11Resource*>(uintptr_t(0x10)), Result::Fault, args);
    {
        const size_t events = f.sink.events.size();
        CHECK(!f.pass->Capture(reinterpret_cast<ID3D11DeviceContext*>(uintptr_t(0x10)), good.Get(), args, &r));
        CHECK(r.result == Result::Fault && f.sink.events.size() == events);
    }

    // still fine afterwards
    CHECK(f.Run(good.Get(), args));
}

// A source that belongs to another device: its view cannot be created. Nothing is drawn or published, the game's state is as it was,
// and the pass goes on afterwards.
void ForeignSource()
{
    Fixture f;
    Rig other;
    if (!f.Create() || !other.Create())
        return;
    auto good = MakeSource(f.rig, kFamilies[0], 16, 12);
    auto foreign = MakeSource(other, kFamilies[0], 16, 12);
    CHECK(f.Run(good.Get(), Args(16, 12, 0, 0, 16, 12)));
    const size_t events = f.sink.events.size();
    const int draws = g_draw.draws;
    GameState game;
    CHECK(game.Set(f.rig));
    const Dump before = DumpState(f.rig.context.Get());
    CaptureReport r;
    CHECK(!f.Run(foreign.Get(), Args(16, 12, 0, 0, 16, 12), &r));
    CHECK(r.result == Result::ResourceFailure);
    CHECK(g_draw.draws == draws && f.sink.events.size() == events);
    CHECK(SameState("after a source of another device", before, DumpState(f.rig.context.Get())));
    CHECK(f.Run(good.Get(), Args(16, 12, 0, 0, 16, 12)));
    CHECK(f.sink.events.size() == events + 1);
}

void DeferredContext()
{
    Fixture f;
    if (!f.Create())
        return;
    ComPtr<ID3D11DeviceContext> deferred;
    CHECK(SUCCEEDED(f.rig.device->CreateDeferredContext(0, &deferred)));
    if (deferred == nullptr)
        return;
    auto src = MakeSource(f.rig, kFamilies[0], 16, 12);
    CaptureReport r;
    CHECK(!f.pass->Capture(deferred.Get(), src.Get(), Args(16, 12, 0, 0, 16, 12), &r));
    CHECK(r.result == Result::DeferredContext);
    CHECK(g_draw.draws == 0);
    CHECK(f.sink.events.empty());
    CHECK(!f.pass->Ready()); // not even set up: the type is the first thing looked at
    // the type is looked at first: a deferred context with a source that is no object is still just a deferred context
    CHECK(!f.pass->Capture(deferred.Get(), reinterpret_cast<ID3D11Resource*>(uintptr_t(0x10)), Args(16, 12, 0, 0, 16, 12), &r));
    CHECK(r.result == Result::DeferredContext);
    // the same request on the immediate context works
    CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12)));
    // and a deferred one, with a pass that is up, still does not draw or publish
    CHECK(!f.pass->Capture(deferred.Get(), src.Get(), Args(16, 12, 0, 0, 16, 12), &r));
    CHECK(r.result == Result::DeferredContext && g_draw.draws == 1 && f.sink.events.size() == 1);
}

// A device the pass was given up on (the set-up failed there once): no capture tries again, and nothing else is disturbed. WARP
// has no device the set-up fails on, so the marking is set by hand here; the failing set-up itself is not exercised.
void GivenUpDevice()
{
    Fixture f;
    Rig bad;
    if (!f.Create() || !bad.Create())
        return;
    CHECK(!DeviceMarkedFailed(bad.device.Get()) && !DeviceMarkedFailed(f.rig.device.Get()));
    MarkDeviceFailed(bad.device.Get());
    CHECK(DeviceMarkedFailed(bad.device.Get()) && !DeviceMarkedFailed(f.rig.device.Get()));
    auto srcBad = MakeSource(bad, kFamilies[0], 16, 12);
    CaptureReport r;
    CHECK(!f.pass->Capture(bad.context.Get(), srcBad.Get(), Args(16, 12, 0, 0, 16, 12), &r));
    CHECK(r.result == Result::DeviceUnavailable);
    CHECK(!f.pass->Ready() && f.pass->InitFailures() == 0); // no set-up was attempted
    CHECK(f.sink.events.empty() && g_draw.draws == 0);
    // a good device is fine, and a call for the marked one then leaves the running pass alone
    auto src = MakeSource(f.rig, kFamilies[0], 16, 12);
    CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12)));
    CHECK(f.pass->Ready() && f.sink.events.size() == 1);
    CHECK(!f.pass->Capture(bad.context.Get(), srcBad.Get(), Args(16, 12, 0, 0, 16, 12), &r));
    CHECK(r.result == Result::DeviceUnavailable);
    CHECK(f.pass->Ready() && f.sink.events.size() == 1);
    CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12)));
    CHECK(f.sink.events.size() == 2 && f.sink.events[1].valid);
    // Init on its own: what is published is withdrawn before the old set-up goes
    const size_t before = f.sink.events.size();
    CHECK(!f.pass->Init(nullptr));
    CHECK(f.sink.events.size() == before + 1 && !f.sink.events.back().valid && f.sink.events.back().heldRefs == 2);
    CHECK(!f.pass->Ready());
    CHECK(f.pass->Init(f.rig.device.Get()));
    CHECK(f.pass->Ready());
}
// Init() on its own, then a capture on the same device does not set up again
void InitFirst()
{
    Fixture f;
    if (!f.Create())
        return;
    CHECK(!f.pass->Ready());
    CHECK(f.pass->Init(f.rig.device.Get()));
    CHECK(f.pass->Ready());
    auto src = MakeSource(f.rig, kFamilies[0], 16, 12);
    CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12)));
    CHECK(f.sink.events.size() == 1); // no withdrawal on the way: same device
}

// The pass does not keep the game's depth alive and gives its own target back
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
void NoLeftovers()
{
    static const GUID kCanary = { 0x8a6e2f1c, 0x4d3b, 0x4f8a, { 0x9c, 0x11, 0x5e, 0x77, 0x10, 0x2a, 0x3b, 0x01 } };
    Fixture f;
    if (!f.Create())
        return;
    bool sourceDestroyed = false, targetDestroyed = false;
    {
        auto src = MakeSource(f.rig, kFamilies[0], 16, 12);
        Canary* c = new Canary(&sourceDestroyed);
        src->SetPrivateDataInterface(kCanary, c);
        c->Release();
        CHECK(f.Run(src.Get(), Args(16, 12, 0, 0, 16, 12)));
        Canary* t = new Canary(&targetDestroyed);
        ComPtr<ID3D11Texture2D> target = TextureOf(ViewPtr(f.LastView()));
        if (target != nullptr)
            target->SetPrivateDataInterface(kCanary, t);
        t->Release();
    }
    f.rig.context->Flush();
    CHECK(sourceDestroyed); // nothing of the pass (its view, the private state) still refers to the game's depth
    CHECK(!targetDestroyed); // the pass owns the target
    f.pass->Release();
    f.sink.held.Reset();
    f.rig.context->Flush();
    CHECK(targetDestroyed); // ... and gives it up on Release
}

// ---- vtable hooks --------------------------------------------------------------------------------------------------------
// The D3D11 runtime's objects share one method table per class, so a patch reaches every device and context of the process. Each
// patch is removed again, and only if the slot still holds the hook (a context-state swap rewrites the context's table).
// Slot numbers: ID3D11Device (CreateShaderResourceView 7, CreateVertexShader 12, GetDeviceRemovedReason 39), ID3D11DeviceContext
// (Draw 13, OMSetRenderTargets 33, UpdateSubresource 48, ClearState 110), ID3D11DeviceContext1 (SwapDeviceContextState 131). The tests
// check that their hook was reached.
constexpr int kDevCreateSrv = 7, kDevCreateVs = 12, kDevRemovedReason = 39;
constexpr int kCtxOmSetRenderTargets = 33, kCtxUpdateSubresource = 48, kCtxClearState = 110, kCtx1Swap = 131;

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

// ClearState and OMSetRenderTargets that crash (installed from inside the draw, i.e. inside the private state)
SlotPatch g_patchClear, g_patchOm;
void STDMETHODCALLTYPE HkClearCrash(ID3D11DeviceContext*) { CrashNow(); }
void STDMETHODCALLTYPE HkOmCrash(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*) { CrashNow(); }
void InstallUnbindCrashes(ID3D11DeviceContext* c)
{
    g_patchClear.Install(c, kCtxClearState, reinterpret_cast<void*>(&HkClearCrash));
    g_patchOm.Install(c, kCtxOmSetRenderTargets, reinterpret_cast<void*>(&HkOmCrash));
}

// SwapDeviceContextState: counts the calls and misbehaves on one of them
enum class SwapFault { None, CrashBefore, CrashAfter, NullPreviousNoSwap, NullPreviousSwapped };
struct SwapHook
{
    SlotPatch patch;
    int calls = 0;
    int faultCall = 0; // 1-based; 0 = never
    SwapFault fault = SwapFault::None;
    std::vector<ID3DDeviceContextState*> targets;  // the state asked for, per call
    std::vector<ID3DDeviceContextState*> previous; // the previous state returned, per call
    ID3DDeviceContextState* lost = nullptr;        // NullPreviousSwapped: the game's state whose reference was dropped
};
SwapHook g_swap;
using SwapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3DDeviceContextState*, ID3DDeviceContextState**);
void STDMETHODCALLTYPE HkSwap(ID3D11DeviceContext1* c, ID3DDeviceContextState* s, ID3DDeviceContextState** p)
{
    const int call = ++g_swap.calls;
    g_swap.targets.push_back(s);
    const bool faulty = call == g_swap.faultCall;
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
void InstallSwapHook(Rig& rig, int faultCall, SwapFault fault)
{
    ComPtr<ID3D11DeviceContext1> c1;
    rig.context.As(&c1);
    g_swap.calls = 0;
    g_swap.faultCall = faultCall;
    g_swap.fault = fault;
    g_swap.targets.clear();
    g_swap.previous.clear();
    g_swap.lost = nullptr;
    g_swap.patch.Install(c1.Get(), kCtx1Swap, reinterpret_cast<void*>(&HkSwap));
}

// Device calls: what the hooks see while a Capture runs
enum class DeviceFault { None, SrvCrash, VertexShaderFails, DeviceRemoved };
struct DeviceHook
{
    SlotPatch srv, vs, removed, update;
    bool active = false; // a Capture is running
    DeviceFault fault = DeviceFault::None;
    int srvCalls = 0, srvBypassed = 0, updateCalls = 0, updateBypassed = 0, removedCalls = 0, vsCalls = 0;
};
DeviceHook g_dev;
using SrvFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
using VsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using RemovedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*);
using UpdateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
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
void STDMETHODCALLTYPE HkUpdate(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT p1,
                                UINT p2)
{
    if (g_dev.active)
    {
        ++g_dev.updateCalls;
        g_dev.updateBypassed += ext::core::HooksBypassed() ? 1 : 0;
    }
    g_dev.update.Original<UpdateFn>()(c, r, sub, box, data, p1, p2);
}
void InstallDeviceHooks(Rig& rig)
{
    g_dev = DeviceHook {};
    g_dev.srv.Install(rig.device.Get(), kDevCreateSrv, reinterpret_cast<void*>(&HkCreateSrv));
    g_dev.vs.Install(rig.device.Get(), kDevCreateVs, reinterpret_cast<void*>(&HkCreateVs));
    g_dev.removed.Install(rig.device.Get(), kDevRemovedReason, reinterpret_cast<void*>(&HkRemoved));
    g_dev.update.Install(rig.context.Get(), kCtxUpdateSubresource, reinterpret_cast<void*>(&HkUpdate));
}
void RemoveDeviceHooks()
{
    g_dev.srv.Remove();
    g_dev.vs.Remove();
    g_dev.removed.Remove();
    g_dev.update.Remove();
    g_dev.active = false;
    g_dev.fault = DeviceFault::None;
}
// Runs one Capture with the device hooks live (and the given fault)
bool RunHooked(Fixture& f, ID3D11Resource* source, const CaptureArgs& args, DeviceFault fault, CaptureReport* report = nullptr)
{
    g_dev.fault = fault;
    g_dev.active = true;
    const bool ok = f.Run(source, args, report);
    g_dev.active = false;
    g_dev.fault = DeviceFault::None;
    return ok;
}

// ---- faults in the restore ------------------------------------------------------------------------------------------------
// Whatever fails on the way back, the game's context must end up in the game's state: the unbind step and the swap back are separate
// guarded steps, and a swap back that did not happen is retried by the next call, before anything else.
void FaultsInTheRestore()
{
    Fixture f;
    if (!f.Create())
        return;
    GameState game;
    CHECK(game.Set(f.rig));
    auto src = MakeSource(f.rig, kFamilies[0], 40, 30);
    const CaptureArgs args = Args(16, 12, 0, 0, 16, 12);
    CHECK(f.Run(src.Get(), args));
    const Dump before = DumpState(f.rig.context.Get());
    CaptureReport r;

    // 1. the unbind step crashes (ClearState and OMSetRenderTargets, from inside the private state)
    size_t events = f.sink.events.size();
    g_draw.mode = 3;
    CHECK(!f.Run(src.Get(), args, &r));
    g_draw.mode = 0;
    g_patchClear.Remove();
    g_patchOm.Remove();
    CHECK(r.result == Result::Fault);
    CHECK(SameState("after a crash while unbinding", before, DumpState(f.rig.context.Get())));
    CHECK(f.sink.events.size() == events + 1 && !f.sink.events.back().valid);
    CHECK(f.Run(src.Get(), args));
    CHECK(SameState("after recovery from the unbind crash", before, DumpState(f.rig.context.Get())));

    // 2. the swap back crashes before it swaps: the context is left in the private state, and the NEXT call puts the game's state
    // back first, with the very state object the first swap returned
    InstallSwapHook(f.rig, 2, SwapFault::CrashBefore);
    events = f.sink.events.size();
    CHECK(!f.Run(src.Get(), args, &r));
    CHECK(r.result == Result::Fault);
    CHECK(f.sink.events.size() == events + 1 && !f.sink.events.back().valid);
    CHECK(g_swap.calls >= 2);
    ID3DDeviceContextState* gameState = g_swap.previous.empty() ? nullptr : g_swap.previous[0];
    CHECK(gameState != nullptr);
    g_swap.faultCall = 0;
    CHECK(f.Run(src.Get(), args)); // call 3 is the retry, 4 and 5 the capture
    CHECK(g_swap.calls == 5);
    if (g_swap.calls >= 3)
    {
        CHECK(g_swap.targets[2] == gameState);
        CHECK(g_swap.targets[0] != gameState);
    }
    CHECK(SameState("after the retried swap back", before, DumpState(f.rig.context.Get())));
    // a Release while a swap back is outstanding retries it, too
    InstallSwapHook(f.rig, 2, SwapFault::CrashBefore);
    CHECK(!f.Run(src.Get(), args));
    g_swap.faultCall = 0;
    f.pass->Release();
    CHECK(SameState("after Release with a swap back outstanding", before, DumpState(f.rig.context.Get())));
    CHECK(f.Run(src.Get(), args));

    // 3. the swap back is done and then crashes
    InstallSwapHook(f.rig, 2, SwapFault::CrashAfter);
    CHECK(!f.Run(src.Get(), args, &r));
    CHECK(r.result == Result::Fault);
    CHECK(SameState("after a crash behind the swap back", before, DumpState(f.rig.context.Get())));
    g_swap.faultCall = 0;
    CHECK(f.Run(src.Get(), args));
    CHECK(SameState("after the retry behind a finished swap back", before, DumpState(f.rig.context.Get())));
    g_swap.patch.Remove();
}

// A failed unbind leaves the game's depth bound in the private state; the next capture clears it first (ClearState at the start of
// the draw step)
void StaleBindingsAreCleared()
{
    static const GUID kCanary = { 0x1c9e7a42, 0x52d1, 0x4b6e, { 0x8a, 0x33, 0x0f, 0x9d, 0x6e, 0x21, 0xc4, 0x77 } };
    Fixture f;
    if (!f.Create())
        return;
    const CaptureArgs args = Args(16, 12, 0, 0, 16, 12);
    bool firstDestroyed = false;
    {
        auto first = MakeSource(f.rig, kFamilies[0], 40, 30);
        Canary* c = new Canary(&firstDestroyed);
        first->SetPrivateDataInterface(kCanary, c);
        c->Release();
        g_draw.mode = 3;
        CHECK(!f.Run(first.Get(), args));
        g_draw.mode = 0;
        g_patchClear.Remove();
        g_patchOm.Remove();
    }
    f.rig.context->Flush();
    CHECK(!firstDestroyed); // the private state still holds its view
    auto second = MakeSource(f.rig, kFamilies[0], 40, 30);
    CHECK(f.Run(second.Get(), args));
    f.rig.context->Flush();
    CHECK(firstDestroyed);
}

// A swap that returns no previous state: nothing is drawn, and a context that really is in the private state is never swapped into
// it again
void NoPreviousState()
{
    Fixture f;
    if (!f.Create())
        return;
    GameState game;
    CHECK(game.Set(f.rig));
    auto src = MakeSource(f.rig, kFamilies[0], 40, 30);
    const CaptureArgs args = Args(16, 12, 0, 0, 16, 12);
    CHECK(f.Run(src.Get(), args));
    const Dump before = DumpState(f.rig.context.Get());
    CaptureReport r;

    // the swap did not happen: a Fault, nothing drawn, and the next call is fine
    InstallSwapHook(f.rig, 1, SwapFault::NullPreviousNoSwap);
    int draws = g_draw.draws;
    size_t events = f.sink.events.size();
    CHECK(!f.Run(src.Get(), args, &r));
    CHECK(r.result == Result::Fault);
    CHECK(g_draw.draws == draws && f.sink.events.size() == events);
    CHECK(SameState("after a null previous state (no swap)", before, DumpState(f.rig.context.Get())));
    CHECK(f.Run(src.Get(), args));
    CHECK(g_swap.calls == 3);

    // the swap happened and the caller was told nothing: the context is in the private state, which is not swapped into itself again
    InstallSwapHook(f.rig, 1, SwapFault::NullPreviousSwapped);
    draws = g_draw.draws;
    events = f.sink.events.size();
    CHECK(!f.Run(src.Get(), args, &r));
    CHECK(r.result == Result::Fault);
    CHECK(g_draw.draws == draws);
    CHECK(!f.Run(src.Get(), args, &r)); // the second call sees the private state as the "previous" one
    CHECK(r.result == Result::Fault);
    CHECK(g_draw.draws == draws);
    // put the game's state back by hand so the rig stays usable
    ComPtr<ID3D11DeviceContext1> c1;
    f.rig.context.As(&c1);
    g_swap.patch.Remove();
    if (g_swap.lost != nullptr)
    {
        c1->SwapDeviceContextState(g_swap.lost, nullptr);
        g_swap.lost->Release();
    }
    CHECK(SameState("after putting the game's state back by hand", before, DumpState(f.rig.context.Get())));
}

// ---- the game's predication ---------------------------------------------------------------------------------------------
// UpdateSubresource is predicated by the game's SetPredication, a Draw in the private state is not. The constants are written in
// the private state: a fresh constant buffer is all zeros, which would divide by zero in the shader.
void PredicatedGameState()
{
    for (int predicateValue = 0; predicateValue < 2; ++predicateValue)
    {
        Fixture f;
        if (!f.Create())
            return;
        GameState game;
        CHECK(game.Set(f.rig));
        ComPtr<ID3D11Predicate> predicate;
        D3D11_QUERY_DESC qd { D3D11_QUERY_OCCLUSION_PREDICATE, 0 };
        CHECK(SUCCEEDED(f.rig.device->CreatePredicate(&qd, &predicate)));
        if (predicate == nullptr)
            return;
        f.rig.context->Begin(predicate.Get());
        f.rig.context->End(predicate.Get());
        f.rig.context->SetPredication(predicate.Get(), predicateValue != 0);
        const Dump before = DumpState(f.rig.context.Get());
        bool predicated = false;
        for (const auto& entry : before)
            predicated = predicated || (entry.first == "predicate" && entry.second != 0);
        CHECK(predicated);

        const Family& fam = kFamilies[0];
        const Case first { "predicated, first capture", 40, 30, 4, 3, 17, 10, 29, 17 };
        const Case second { "predicated, new constants", 40, 30, 9, 7, 12, 9, 20, 11 };
        auto src = MakeSource(f.rig, fam, first.srcW, first.srcH);
        for (const Case& c : { first, second })
        {
            CaptureReport r;
            f.rig.context->SetPredication(predicate.Get(), predicateValue != 0);
            const bool ok = f.Run(src.Get(), Args(c.ow, c.oh, c.bx, c.by, c.rw, c.rh), &r);
            CHECK(SameState("after a capture under predication", before, DumpState(f.rig.context.Get())));
            f.rig.context->SetPredication(nullptr, FALSE); // the readback is a CopyResource: predicated, too
            CHECK(ok);
            if (ok)
                CheckImage(predicateValue != 0 ? "predicate value TRUE" : "predicate value FALSE", fam, c, ReadView(f.rig, f.LastView()), false);
        }
        f.rig.context->SetPredication(nullptr, FALSE);
    }
}

// ---- calls the WARP device lets us observe or break ------------------------------------------------------------------------
// The bypass scope covers the prepare step (the views) and the constant upload, not only the draw.
void BypassCoversEveryCall()
{
    Fixture f;
    if (!f.Create())
        return;
    auto src = MakeSource(f.rig, kFamilies[0], 40, 30);
    const CaptureArgs args = Args(16, 12, 0, 0, 16, 12);
    InstallDeviceHooks(f.rig);
    CHECK(RunHooked(f, src.Get(), args, DeviceFault::None));
    CHECK(g_dev.srvCalls >= 2); // the source view and the view of the new target
    CHECK(g_dev.srvBypassed == g_dev.srvCalls);
    CHECK(g_dev.updateBypassed == g_dev.updateCalls); // (the context's table is the private state's while it is current: the count may be 0)
    CHECK(g_dev.removedCalls >= 1); // the device is asked on every capture
    CHECK(!ext::core::HooksBypassed());
    RemoveDeviceHooks();
}

// A crash while the view of the source is made: a Fault, nothing drawn or published, the game's state as it was
void CrashInThePrepareStep()
{
    Fixture f;
    if (!f.Create())
        return;
    GameState game;
    CHECK(game.Set(f.rig));
    auto src = MakeSource(f.rig, kFamilies[0], 40, 30);
    const CaptureArgs args = Args(16, 12, 0, 0, 16, 12);
    CHECK(f.Run(src.Get(), args));
    const Dump before = DumpState(f.rig.context.Get());
    const int draws = g_draw.draws;
    const size_t events = f.sink.events.size();
    InstallDeviceHooks(f.rig);
    CaptureReport r;
    CHECK(!RunHooked(f, src.Get(), args, DeviceFault::SrvCrash, &r));
    CHECK(g_dev.srvCalls >= 1);
    RemoveDeviceHooks();
    CHECK(r.result == Result::Fault);
    CHECK(g_draw.draws == draws && f.sink.events.size() == events);
    CHECK(SameState("after a crash in the prepare step", before, DumpState(f.rig.context.Get())));
    CHECK(!ext::core::HooksBypassed());
    CHECK(f.Run(src.Get(), args));
}

// A removed device: what is published is withdrawn (the pass still holds its view), nothing is set up again, and the next device
// (or the same one, once it reports healthy) works
void RemovedDevice()
{
    Fixture f;
    if (!f.Create())
        return;
    auto src = MakeSource(f.rig, kFamilies[0], 40, 30);
    const CaptureArgs args = Args(16, 12, 0, 0, 16, 12);
    CHECK(f.Run(src.Get(), args));
    InstallDeviceHooks(f.rig);
    CaptureReport r;
    CHECK(!RunHooked(f, src.Get(), args, DeviceFault::DeviceRemoved, &r));
    CHECK(r.result == Result::DeviceUnavailable);
    CHECK(g_dev.removedCalls >= 1);
    CHECK(f.sink.events.size() == 2 && !f.sink.events[1].valid && f.sink.events[1].heldRefs == 2);
    CHECK(!f.pass->Ready());
    CHECK(!RunHooked(f, src.Get(), args, DeviceFault::DeviceRemoved, &r)); // again: not set up again on the removed device
    CHECK(r.result == Result::DeviceUnavailable);
    CHECK(!f.pass->Ready() && f.pass->InitFailures() == 0);
    CHECK(!DeviceMarkedFailed(f.rig.device.Get())); // removed is not "set-up failed"
    CHECK(f.sink.events.size() == 2);
    RemoveDeviceHooks();
    CHECK(f.Run(src.Get(), args)); // healthy again
    CHECK(f.pass->Ready() && f.sink.events.size() == 3 && f.sink.events[2].valid);
}

// A set-up that fails (the vertex shader is refused): reported, the device is marked and not tried again, nothing half built stays
void FailedSetUp()
{
    Fixture f;
    if (!f.Create())
        return;
    auto src = MakeSource(f.rig, kFamilies[0], 40, 30);
    const CaptureArgs args = Args(16, 12, 0, 0, 16, 12);
    InstallDeviceHooks(f.rig);
    CaptureReport r;
    CHECK(!RunHooked(f, src.Get(), args, DeviceFault::VertexShaderFails, &r));
    CHECK(r.result == Result::DeviceUnavailable);
    CHECK(g_dev.vsCalls == 1);
    CHECK(f.pass->InitFailures() == 1);
    CHECK(DeviceMarkedFailed(f.rig.device.Get()));
    CHECK(!f.pass->Ready()); // the private state made before the shader failed is gone
    CHECK(f.sink.events.empty() && g_draw.draws == 0);
    CHECK(!RunHooked(f, src.Get(), args, DeviceFault::VertexShaderFails, &r));
    CHECK(g_dev.vsCalls == 1 && f.pass->InitFailures() == 1); // not tried again, even though the cause is gone below
    RemoveDeviceHooks();
    CHECK(!f.Run(src.Get(), args, &r));
    CHECK(r.result == Result::DeviceUnavailable && f.pass->InitFailures() == 1 && !f.pass->Ready());
    // another device is fine
    Rig other;
    if (!other.Create())
        return;
    auto srcOther = MakeSource(other, kFamilies[0], 40, 30);
    CHECK(f.pass->Capture(other.context.Get(), srcOther.Get(), args));
    CHECK(f.pass->Ready());
}

// ---- the product shader, driven directly ---------------------------------------------------------------------------------
// The pass never draws a pixel beyond the output size, so the clamp and the largest sizes are looked at here.
Image RunShader(Rig& rig, ID3D11Texture2D* source, const CropConstants& k, UINT tw, UINT th)
{
    Image none;
    ID3D11Device* d = rig.device.Get();
    ID3D11DeviceContext* c = rig.context.Get();
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    D3D11_BUFFER_DESC bd {};
    bd.ByteWidth = sizeof(CropConstants);
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
    if (FAILED(d->CreateVertexShader(DepthCrop_Vs_cso, sizeof(DepthCrop_Vs_cso), nullptr, &vs)) ||
        FAILED(d->CreatePixelShader(DepthCrop_Ps_cso, sizeof(DepthCrop_Ps_cso), nullptr, &ps)) ||
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

void ShaderClamp()
{
    Rig rig;
    if (!rig.Create())
        return;
    auto src = MakeSource(rig, kFamilies[1], 10, 8); // typed: a view without a description needs a typed format
    // render 4x3 at (1,1) mapped for an output of 3x2, drawn into a 7x5 target: the pixels beyond the output read the last
    // render texel (x 4, y 3), never past the render region
    CaptureArgs a = Args(3, 2, 1, 1, 4, 3);
    const Image img = RunShader(rig, src.Get(), MakeConstants(a), 7, 5);
    CHECK(img.w == 7 && img.h == 5 && img.px.size() == 35);
    if (img.px.size() != 35)
        return;
    for (uint32_t y = 0; y < 5; ++y)
        for (uint32_t x = 0; x < 7; ++x)
        {
            const uint32_t sx = 1 + std::min((x * 2 + 1) * 4 / (3 * 2), 4u - 1);
            const uint32_t sy = 1 + std::min((y * 2 + 1) * 3 / (2 * 2), 3u - 1);
            if (img.px[y * 7 + x] != Expected(Kind::F32, Index(sx, sy)))
                std::printf("  clamp: pixel (%u,%u) read the wrong texel (want %u,%u)\n", x, y, sx, sy);
            CHECK(img.px[y * 7 + x] == Expected(Kind::F32, Index(sx, sy)));
        }
    CHECK(img.px[4 * 7 + 6] == Expected(Kind::F32, Index(1 + 3, 1 + 2))); // the corner: the last render texel
}

// The shader's own arithmetic on 1-pixel strips: a source of `render` texels whose value is its index, mapped to `output`
// pixels. The strips cover the largest edge (the products reach (2 * 0x3FFF + 1) * 0x4000, beyond what float32 holds exactly)
// and the real ratios of the game (Quality is exactly 2:3).
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
            const Image img = RunShader(rig, src.Get(), MakeConstants(Args(ow, oh, 0, 0, w, h)), ow, oh);
            CHECK(img.px.size() == size_t(strip.output));
            if (img.px.size() != size_t(strip.output))
                continue;
            uint32_t wrong = 0, firstWrong = 0;
            for (uint32_t i = 0; i < strip.output; ++i)
            {
                // 64-bit maths here, 32-bit in the shader
                const uint64_t s = std::min<uint64_t>((uint64_t(i) * 2 + 1) * strip.render / (uint64_t(strip.output) * 2), strip.render - 1);
                CHECK(strip.render > 5000 || s == ScanSrc(i, strip.render, strip.output)); // the scan is slow on the big ones
                if (img.px[i] != float(s) && wrong++ == 0)
                    firstWrong = i;
                if (!vertical)
                {
                    floatMulWrong += FloatMul(i, strip.render, strip.output) != s ? 1 : 0;
                    floatRatioWrong += FloatRatio(i, strip.render, strip.output) != s ? 1 : 0;
                    floatRcpWrong += FloatRcp(i, strip.render, strip.output) != s ? 1 : 0;
                }
            }
            if (wrong != 0)
                std::printf("  strip %u -> %u (%s): %u pixels wrong, first at %u\n", strip.render, strip.output, vertical ? "vertical" : "horizontal",
                            wrong, firstWrong);
            CHECK(wrong == 0);
        }
    // The strips must be able to tell a float version from the integer one: each float form of the mapping is wrong somewhere
    std::printf("  float32 forms of the mapping disagree with the integer one at: (i+.5)*r/o %u, (i+.5)*(r/o) %u, (i+.5)*r*rcp(o) %u pixels\n",
                floatMulWrong, floatRatioWrong, floatRcpWrong);
    CHECK(floatMulWrong > 0);
    CHECK(floatRatioWrong > 0);
    CHECK(floatRcpWrong > 0);
}} // namespace

int main()
{
    FormatTable();
    Checks();
    Names();
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
    DepthStencilSources();
    std::fputs("context state...\n", stderr);
    ContextState();
    SourceBoundInGameState();
    BypassScopeIsOn();
    PredicatedGameState();
    FaultsInTheRestore();
    StaleBindingsAreCleared();
    NoPreviousState();
    BypassCoversEveryCall();
    CrashInThePrepareStep();
    RemovedDevice();
    FailedSetUp();
    std::fputs("publish...\n", stderr);
    PublishedFields();
    RecreateOnlyOnOutputSize();
    OrderOnRecreate();
    OrderOnRelease();
    OrderOnDestroy();
    OrderOnDeviceChange();
    WithdrawKeepsResources();
    FailureWithdraws();
    std::fputs("refusals...\n", stderr);
    Refusals();
    ForeignSource();
    DeferredContext();
    GivenUpDevice();
    InitFirst();
    NoLeftovers();
    std::fputs("shader...\n", stderr);
    ShaderClamp();
    ShaderStrips();

    if (g_fail != 0)
    {
        std::printf("FAIL: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::puts("PASS: depth copy (formats, request checks, constants, crop and nearest remap on 6 source families x 12 cases, context state (also with the depth bound in it), bypass scope, publish order on recreate / Release / destroy / device change, refusals, faults, product shader clamp and largest edge)");
    return 0;
}
