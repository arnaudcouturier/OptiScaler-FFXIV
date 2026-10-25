// S10 D0: the D3D11 helpers of the ReShade probe on the WARP software adapter: the SEH-guarded call (normal return, access
// violation, C++ exception), format families, the pipeline state block round trip, and the sampled before/after
// comparison of a render target (8-bit and FP16 targets, unchanged target, unsupported format).
#include "ext/reshade/ReShadeD3D11.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace ext::reshade::d3d11;

namespace
{
void Ok(void* p) { *static_cast<int*>(p) = 42; }
void Crash(void*)
{
    volatile int* null = nullptr;
    *null = 1;
}
void Throw(void*) { throw std::runtime_error("boom"); }

void TestGuardedCall()
{
    int v = 0;
    assert(GuardedCall(&Ok, &v) == 0 && v == 42);
    assert(GuardedCall(&Crash, nullptr) == 0xC0000005u);
    assert(GuardedCall(&Throw, nullptr) != 0); // a C++ exception crossing the frame is reported, not propagated
}

// The process-wide context-state mutex: one object, recursive on a thread, exclusive between threads
void TestContextStateMutex()
{
    std::recursive_mutex& m = ContextStateMutex();
    assert(&m == &ContextStateMutex());
    {
        std::lock_guard<std::recursive_mutex> outer(m);
        std::lock_guard<std::recursive_mutex> inner(ContextStateMutex()); // recursive: a Release under a capture takes it again
        std::atomic<int> other { -1 };
        std::thread t([&] { other = m.try_lock() ? 1 : 0; if (other == 1) m.unlock(); });
        t.join();
        assert(other == 0); // held by this thread: another thread does not get in
    }
    std::atomic<int> after { -1 };
    std::thread t([&] { after = m.try_lock() ? 1 : 0; if (after == 1) m.unlock(); });
    t.join();
    assert(after == 1); // and free again once released
    std::recursive_mutex* seenByThread = nullptr;
    std::thread t2([&] { seenByThread = &ContextStateMutex(); });
    t2.join();
    assert(seenByThread == &m); // one object for the whole process
}

void TestFamilies()
{
    const FormatFamily f = FamilyOf(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    assert(f.known && f.linear == DXGI_FORMAT_R8G8B8A8_UNORM && f.srgb == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    assert(SameFamily(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
    assert(SameFamily(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB));
    assert(!SameFamily(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM));
    assert(SameFamily(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT)); // identical formats always match
    assert(!SameFamily(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM));
    assert(!FamilyOf(DXGI_FORMAT_R16G16B16A16_FLOAT).known);
}

struct Device
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    bool Create()
    {
        D3D_FEATURE_LEVEL level {};
        return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level,
                                           &context));
    }
};

ComPtr<ID3D11Texture2D> MakeTarget(Device& d, DXGI_FORMAT format, UINT w, UINT h, ComPtr<ID3D11RenderTargetView>& rtv)
{
    D3D11_TEXTURE2D_DESC td {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> tex;
    assert(SUCCEEDED(d.device->CreateTexture2D(&td, nullptr, &tex)));
    assert(SUCCEEDED(d.device->CreateRenderTargetView(tex.Get(), nullptr, &rtv)));
    return tex;
}

void TestStateBlock(Device& d)
{
    ID3D11DeviceContext* c = d.context.Get();
    ComPtr<ID3D11RenderTargetView> rtv;
    auto tex = MakeTarget(d, DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, rtv);

    D3D11_BLEND_DESC bd {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;
    assert(SUCCEEDED(d.device->CreateBlendState(&bd, &blend)));
    D3D11_DEPTH_STENCIL_DESC dd {};
    dd.DepthEnable = FALSE;
    ComPtr<ID3D11DepthStencilState> depth;
    assert(SUCCEEDED(d.device->CreateDepthStencilState(&dd, &depth)));
    D3D11_RASTERIZER_DESC rd {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    assert(SUCCEEDED(d.device->CreateRasterizerState(&rd, &raster)));
    D3D11_BUFFER_DESC bufd {};
    bufd.ByteWidth = 256;
    bufd.Usage = D3D11_USAGE_DEFAULT;
    bufd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb;
    assert(SUCCEEDED(d.device->CreateBuffer(&bufd, nullptr, &cb)));
    D3D11_SAMPLER_DESC sd {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ComPtr<ID3D11SamplerState> sampler;
    assert(SUCCEEDED(d.device->CreateSamplerState(&sd, &sampler)));

    const float factor[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
    const D3D11_VIEWPORT vp { 1.f, 2.f, 30.f, 40.f, 0.f, 1.f };
    const D3D11_RECT scissor { 1, 2, 3, 4 };
    ID3D11RenderTargetView* rtvs[1] = { rtv.Get() };
    ID3D11Buffer* cbs[1] = { cb.Get() };
    ID3D11SamplerState* samplers[1] = { sampler.Get() };
    c->OMSetRenderTargets(1, rtvs, nullptr);
    c->RSSetViewports(1, &vp);
    c->RSSetScissorRects(1, &scissor);
    c->RSSetState(raster.Get());
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    c->OMSetBlendState(blend.Get(), factor, 0xFF0F);
    c->OMSetDepthStencilState(depth.Get(), 7);
    c->PSSetConstantBuffers(3, 1, cbs);
    c->VSSetConstantBuffers(1, 1, cbs);
    c->PSSetSamplers(2, 1, samplers);

    StateBlock block;
    block.Capture(c);

    // clobber everything the way an effect pass would
    ID3D11RenderTargetView* none[1] = { nullptr };
    ID3D11Buffer* nocb[1] = { nullptr };
    ID3D11SamplerState* nosampler[1] = { nullptr };
    const D3D11_VIEWPORT other { 0.f, 0.f, 5.f, 5.f, 0.f, 1.f };
    const D3D11_RECT otherRect { 9, 9, 10, 10 };
    const float otherFactor[4] = { 1.f, 1.f, 1.f, 1.f };
    c->OMSetRenderTargets(1, none, nullptr);
    c->RSSetViewports(1, &other);
    c->RSSetScissorRects(1, &otherRect);
    c->RSSetState(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    c->OMSetBlendState(nullptr, otherFactor, 0xFFFFFFFF);
    c->OMSetDepthStencilState(nullptr, 0);
    c->PSSetConstantBuffers(3, 1, nocb);
    c->VSSetConstantBuffers(1, 1, nocb);
    c->PSSetSamplers(2, 1, nosampler);

    block.Apply(c);

    ComPtr<ID3D11RenderTargetView> gotRtv;
    c->OMGetRenderTargets(1, &gotRtv, nullptr);
    assert(gotRtv.Get() == rtv.Get());
    UINT n = 1;
    D3D11_VIEWPORT gotVp {};
    c->RSGetViewports(&n, &gotVp);
    assert(gotVp.TopLeftX == 1.f && gotVp.TopLeftY == 2.f && gotVp.Width == 30.f && gotVp.Height == 40.f);
    D3D11_RECT gotRect {};
    n = 1;
    c->RSGetScissorRects(&n, &gotRect);
    assert(gotRect.left == 1 && gotRect.top == 2 && gotRect.right == 3 && gotRect.bottom == 4);
    ComPtr<ID3D11RasterizerState> gotRaster;
    c->RSGetState(&gotRaster);
    assert(gotRaster.Get() == raster.Get());
    D3D11_PRIMITIVE_TOPOLOGY topology {};
    c->IAGetPrimitiveTopology(&topology);
    assert(topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ComPtr<ID3D11BlendState> gotBlend;
    float gotFactor[4] {};
    UINT mask = 0;
    c->OMGetBlendState(&gotBlend, gotFactor, &mask);
    assert(gotBlend.Get() == blend.Get() && mask == 0xFF0F && std::fabs(gotFactor[2] - 0.3f) < 1e-6f);
    ComPtr<ID3D11DepthStencilState> gotDepth;
    UINT ref = 0;
    c->OMGetDepthStencilState(&gotDepth, &ref);
    assert(gotDepth.Get() == depth.Get() && ref == 7);
    ID3D11Buffer* gotCb[1] = {};
    c->PSGetConstantBuffers(3, 1, gotCb);
    assert(gotCb[0] == cb.Get());
    if (gotCb[0])
        gotCb[0]->Release();
    c->VSGetConstantBuffers(1, 1, gotCb);
    assert(gotCb[0] == cb.Get());
    if (gotCb[0])
        gotCb[0]->Release();
    ID3D11SamplerState* gotSampler[1] = {};
    c->PSGetSamplers(2, 1, gotSampler);
    assert(gotSampler[0] == sampler.Get());
    if (gotSampler[0])
        gotSampler[0]->Release();

    // The block owns references only until Release: dropping it must not leak or crash.
    block.Release();
    block.Apply(c); // not captured: no-op
}

void Clear(Device& d, ID3D11RenderTargetView* rtv, float r, float g, float b)
{
    const float color[4] = { r, g, b, 1.f };
    d.context->ClearRenderTargetView(rtv, color);
}

void TestSnapshot(Device& d)
{
    ComPtr<ID3D11RenderTargetView> rtv;
    auto tex = MakeTarget(d, DXGI_FORMAT_R8G8B8A8_UNORM, 128, 96, rtv);
    TargetSnapshot snap;
    std::string note;

    Clear(d, rtv.Get(), 1.f, 0.f, 0.f);
    assert(snap.Capture(d.context.Get(), tex.Get(), note));
    Clear(d, rtv.Get(), 0.f, 1.f, 0.f);
    auto diff = snap.CaptureAndDiff(d.context.Get(), tex.Get());
    assert(diff.valid);
    assert(diff.sampled == (128 / 16) * (96 / 16));
    assert(diff.changedFraction == 1.0);
    assert(std::fabs(diff.meanAbs - 170.0) < 0.01); // |255-0| + |0-255| + 0 over three channels
    assert(diff.maxDelta == 255);
    assert(diff.readbackUs > 0);

    // identical content: nothing changed
    Clear(d, rtv.Get(), 0.25f, 0.5f, 0.75f);
    assert(snap.Capture(d.context.Get(), tex.Get(), note));
    diff = snap.CaptureAndDiff(d.context.Get(), tex.Get());
    assert(diff.valid && diff.changedFraction == 0.0 && diff.maxDelta == 0 && diff.meanAbs == 0.0);

    // CaptureAndDiff without a before-snapshot says so
    diff = snap.CaptureAndDiff(d.context.Get(), tex.Get());
    assert(!diff.valid && diff.note.find("before-snapshot") != std::string::npos);

    // FP16 target (the HDR-style format): 0.5 -> 1.0 is a delta of 127.5 per red sample
    ComPtr<ID3D11RenderTargetView> rtv16;
    auto tex16 = MakeTarget(d, DXGI_FORMAT_R16G16B16A16_FLOAT, 64, 64, rtv16);
    Clear(d, rtv16.Get(), 0.5f, 0.f, 0.f);
    assert(snap.Capture(d.context.Get(), tex16.Get(), note));
    Clear(d, rtv16.Get(), 1.0f, 0.f, 0.f);
    diff = snap.CaptureAndDiff(d.context.Get(), tex16.Get());
    assert(diff.valid && diff.changedFraction == 1.0 && std::fabs(diff.meanAbs - 42.5) < 0.01 && diff.maxDelta == 127);

    // unsupported format: reported, never a crash
    ComPtr<ID3D11RenderTargetView> rtv32;
    auto tex32 = MakeTarget(d, DXGI_FORMAT_R32_FLOAT, 32, 32, rtv32);
    assert(!snap.Capture(d.context.Get(), tex32.Get(), note));
    assert(note.find("not supported") != std::string::npos);
    diff = snap.CaptureAndDiff(d.context.Get(), tex32.Get());
    assert(!diff.valid);
}
} // namespace

int main()
{
    std::fputs("guarded call...\n", stderr);
    TestGuardedCall();
    std::fputs("context state mutex...\n", stderr);
    TestContextStateMutex();
    std::fputs("families...\n", stderr);
    TestFamilies();

    Device d;
    if (!d.Create())
    {
        std::puts("SKIP: no WARP D3D11 device");
        return 0;
    }
    std::fputs("state block...\n", stderr);
    TestStateBlock(d);
    std::fputs("snapshot...\n", stderr);
    TestSnapshot(d);
    std::puts("PASS: guarded call (AV and C++ exception), the context-state mutex (one object, recursive, exclusive), format families, D3D11 state block round trip, target snapshot diff (8-bit, FP16, unchanged, unsupported)");
    return 0;
}
