// S10-E: WARP D3D11 proof of the ORDER of the live boundary capture (docs/HUDLESS.md section 16):
//   effects (a fake that rewrites the render target, standing in for ReShade's render_effects) run BEFORE the snapshot copy, so the
//   snapshot holds the modified pixels, and the later UI draw is not in it;
// plus the side-effect rules: a rejected target runs neither the effects nor the copy, a failed effects call delivers no frame, the
// engine must have asked in the previous Present, one capture per frame, the snapshot texture follows size / format changes.
#define NOMINMAX
#include "check.h"

#include "ext/ffxiv/ui/UiLive.h"

#include <d3dcompiler.h>

#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::ui;
using namespace ext::ffxiv::ui::live;
using ext::hudless::FrameImage;
using ext::hudless::TakeResult;
using ext::reshade::ApplyResult;

namespace
{
struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> psUi;

    bool Init()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx)))
            return false;
        const char* v = "float4 VS(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}";
        const char* p = "float4 PS():SV_Target{return float4(0,0,1,1);}"; // the UI: blue
        ComPtr<ID3DBlob> code, err;
        if (FAILED(D3DCompile(v, strlen(v), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &code, &err)) ||
            FAILED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs)))
            return false;
        code.Reset();
        if (FAILED(D3DCompile(p, strlen(p), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &code, &err)) ||
            FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &psUi)))
            return false;
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(psUi.Get(), nullptr, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        return true;
    }

    ComPtr<ID3D11Texture2D> Target(UINT w, UINT h, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM, UINT samples = 1)
    {
        D3D11_TEXTURE2D_DESC d {};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.Format = format;
        d.SampleDesc.Count = samples;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> t;
        device->CreateTexture2D(&d, nullptr, &t);
        return t;
    }

    ComPtr<ID3D11RenderTargetView> Rtv(ID3D11Texture2D* t)
    {
        ComPtr<ID3D11RenderTargetView> v;
        device->CreateRenderTargetView(t, nullptr, &v);
        return v;
    }

    void Bind(ID3D11RenderTargetView* rtv, UINT w, UINT h)
    {
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp { 0, 0, float(w), float(h), 0, 1 };
        ctx->RSSetViewports(1, &vp);
    }

    // Reads a texture back through a staging copy.
    std::vector<uint8_t> Read(ID3D11Texture2D* t, UINT& pitch)
    {
        D3D11_TEXTURE2D_DESC d {};
        t->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        device->CreateTexture2D(&d, nullptr, &staging);
        ctx->CopyResource(staging.Get(), t);
        D3D11_MAPPED_SUBRESOURCE m {};
        std::vector<uint8_t> out;
        if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
        {
            out.assign(static_cast<uint8_t*>(m.pData), static_cast<uint8_t*>(m.pData) + size_t(m.RowPitch) * d.Height);
            pitch = m.RowPitch;
            ctx->Unmap(staging.Get(), 0);
        }
        return out;
    }
};

struct Px
{
    uint8_t r, g, b, a;
};
Px At(const std::vector<uint8_t>& img, UINT pitch, UINT x, UINT y)
{
    const uint8_t* p = img.data() + size_t(y) * pitch + size_t(x) * 4;
    return { p[0], p[1], p[2], p[3] };
}
bool Is(Px p, int r, int g, int b) { return p.r == r && p.g == g && p.b == b; }

// ---- the fakes ---------------------------------------------------------------------------------------------------------------
struct FakeEffects
{
    ApplyResult result = ApplyResult::Applied;
    int calls = 0;
    ID3D11Resource* lastTarget = nullptr;
    float tint[4] { 1, 0, 1, 1 }; // magenta: "the presets"
    int copies = 0;
    bool targetWasModifiedAtCopy = false;
    ID3D11RenderTargetView* targetRtv = nullptr; // what to check at copy time
};
FakeEffects g_fx;
Gpu* g_gpu = nullptr;

ApplyResult FakeApply(ID3D11DeviceContext* c, ID3D11Resource* target)
{
    ++g_fx.calls;
    g_fx.lastTarget = target;
    if (g_fx.result == ApplyResult::Applied)
    {
        // what render_effects does: rewrites the bound render target (here: every pixel becomes the tint)
        ComPtr<ID3D11RenderTargetView> rtv;
        c->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv)
            c->ClearRenderTargetView(rtv.Get(), g_fx.tint);
    }
    return g_fx.result;
}

void CountingCopy(ID3D11DeviceContext* c, ID3D11Resource* dest, ID3D11Resource* source)
{
    ++g_fx.copies;
    c->CopyResource(dest, source);
}

Env MakeEnv()
{
    Env e;
    e.effects = &FakeApply;
    e.copy = &CountingCopy;
    return e;
}

DrawEvent R1(uint64_t frame, UINT bbW = 128, UINT bbH = 72, bool scene = true)
{
    DrawEvent e;
    e.r1Set = true;
    e.sawSceneStage = scene;
    e.bbWidth = bbW;
    e.bbHeight = bbH;
    e.frame = frame;
    return e;
}

// One bridge Present as the engine does it: Take(), then the Present exit.
TakeResult Present(LiveSource& s, FrameImage& img)
{
    const TakeResult r = s.Take(img);
    s.EndFrame();
    return r;
}

void Run()
{
    Gpu gpu;
    CHECK(gpu.Init());
    if (!gpu.device)
        return;
    g_gpu = &gpu;
    auto* ctx = gpu.ctx.Get();

    auto bb = gpu.Target(128, 72);
    auto bbRtv = gpu.Rtv(bb.Get());
    const float green[4] { 0, 1, 0, 1 };

    LiveSource source(MakeEnv());
    FrameImage img;

    // ---- the engine has not asked yet: nothing happens, the draws are untouched -----------------------------------------------
    gpu.Bind(bbRtv.Get(), 128, 72);
    ctx->ClearRenderTargetView(bbRtv.Get(), green);
    source.OnDraw(ctx, R1(1));
    CHECK_EQ(g_fx.calls, 0);
    CHECK_EQ(g_fx.copies, 0);
    CHECK(Present(source, img) == TakeResult::NotThisFrame); // Take() arms the next frame
    CHECK(source.Armed());

    // ---- frame 2: ORDER. scene (green) -> R1 boundary: effects (magenta) -> snapshot -> UI draw (blue, top-left corner) ----------
    ctx->ClearRenderTargetView(bbRtv.Get(), green);
    D3D11_VIEWPORT corner { 0, 0, 32, 18, 0, 1 };
    source.OnDraw(ctx, R1(2)); // before the boundary draw runs
    CHECK_EQ(g_fx.calls, 1);
    CHECK_EQ(g_fx.copies, 1);
    CHECK(g_fx.lastTarget == bb.Get());          // the effects got the very resource bound as render target 0
    ctx->RSSetViewports(1, &corner);
    ctx->Draw(3, 0);                             // the UI draw (the boundary draw itself)
    source.OnDraw(ctx, R1(2));                   // a second R1 event in the same frame (e.g. after a supersession): nothing happens
    CHECK_EQ(g_fx.calls, 1);                     // once per frame
    CHECK_EQ(g_fx.copies, 1);

    UINT pitch = 0, snapPitch = 0;
    const auto backbuffer = gpu.Read(bb.Get(), pitch); // after the UI draw
    CHECK(source.Take(img) == TakeResult::Ok);
    CHECK(img.texture != nullptr);
    CHECK_EQ(img.width, 128u);
    CHECK_EQ(img.height, 72u);
    CHECK(img.format == DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(img.sequence, 2u);
    const auto snap = gpu.Read(img.texture.Get(), snapPitch);
    CHECK(Is(At(snap, snapPitch, 100, 60), 255, 0, 255)); // the presets (magenta), not the scene's green: effects ran BEFORE the copy
    CHECK(Is(At(snap, snapPitch, 5, 5), 255, 0, 255));    // the UI draw (blue, top-left) is NOT in the snapshot
    CHECK(Is(At(backbuffer, pitch, 5, 5), 0, 0, 255));    // ... but it is in the backbuffer after the UI draw
    CHECK(Is(At(backbuffer, pitch, 100, 60), 255, 0, 255));
    CHECK(source.Take(img) == TakeResult::NotThisFrame);   // consumed by the first call
    source.EndFrame();
    {
        const auto st = source.GetStats();
        CHECK_EQ(st.captured, 1u);
        CHECK_EQ(st.delivered, 1u);
        CHECK_EQ(st.rejected, 0u);
        CHECK_EQ(st.effects[size_t(ApplyResult::Applied)], 1u);
    }

    // ---- frame 3: a render target of another size: no effects, no copy, rejected with the guard text ------------------------------
    {
        auto tiny = gpu.Target(64, 64, DXGI_FORMAT_B8G8R8A8_UNORM);
        auto tinyRtv = gpu.Rtv(tiny.Get());
        gpu.Bind(tinyRtv.Get(), 64, 64);
        const int calls = g_fx.calls, copies = g_fx.copies;
        source.OnDraw(ctx, R1(3));
        CHECK_EQ(g_fx.calls, calls);
        CHECK_EQ(g_fx.copies, copies);
        CHECK(source.Take(img) == TakeResult::Rejected);
        CHECK(img.texture == nullptr);
        CHECK(std::string(source.LastReason()) == "render target is not the full-size backbuffer");
        source.EndFrame();
        CHECK_EQ(source.GetStats().rejectedBy[size_t(Reason::SizeMismatch)], 1u);
    }

    // ---- frame 4: multisampled target, typeless sRGB-less formats, no render target ----------------------------------------------
    {
        auto msaa = gpu.Target(128, 72, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
        if (msaa)
        {
            auto msaaRtv = gpu.Rtv(msaa.Get());
            gpu.Bind(msaaRtv.Get(), 128, 72);
            const int calls = g_fx.calls;
            source.OnDraw(ctx, R1(4));
            CHECK_EQ(g_fx.calls, calls);
            CHECK(source.Take(img) == TakeResult::Rejected);
            source.EndFrame();
            CHECK_EQ(source.GetStats().rejectedBy[size_t(Reason::Multisampled)], 1u);
        }
        else
            Present(source, img);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        const int calls = g_fx.calls;
        source.OnDraw(ctx, R1(5));
        CHECK_EQ(g_fx.calls, calls);
        CHECK(source.Take(img) == TakeResult::Rejected);
        source.EndFrame();
        CHECK_EQ(source.GetStats().rejectedBy[size_t(Reason::NoRenderTarget)], 1u);
    }

    // ---- menu-only frame: the R1 event without a scene stage is ignored (no verdict, nothing applied) -------------------------------
    gpu.Bind(bbRtv.Get(), 128, 72);
    {
        const int calls = g_fx.calls, copies = g_fx.copies;
        source.OnDraw(ctx, R1(6, 128, 72, /*scene=*/false));
        CHECK_EQ(g_fx.calls, calls);
        CHECK_EQ(g_fx.copies, copies);
        CHECK(source.Take(img) == TakeResult::NotThisFrame);
        CHECK(std::string(source.LastReason()) == "no boundary in this frame");
        source.EndFrame();
    }

    // ---- effects failed: the frame is rejected, no copy, the live backbuffer was left as the effects left it ---------------------
    {
        g_fx.result = ApplyResult::Failed;
        const int calls = g_fx.calls, copies = g_fx.copies;
        ctx->ClearRenderTargetView(bbRtv.Get(), green);
        source.OnDraw(ctx, R1(7));
        CHECK_EQ(g_fx.calls, calls + 1);
        CHECK_EQ(g_fx.copies, copies); // no snapshot of a frame whose effects failed
        CHECK(source.Take(img) == TakeResult::Rejected);
        CHECK(img.texture == nullptr);
        CHECK(std::string(source.LastReason()) == "ReShade effects failed");
        source.EndFrame();
        CHECK_EQ(source.GetStats().rejectedBy[size_t(Reason::EffectsFailed)], 1u);
    }

    // ---- no presets to apply (no ReShade, nothing enabled, effects disabled): the plain UI-free snapshot is delivered --------------
    for (ApplyResult r : { ApplyResult::NoReShade, ApplyResult::NoRuntime, ApplyResult::NothingToApply, ApplyResult::NotEnabled })
    {
        g_fx.result = r;
        ctx->ClearRenderTargetView(bbRtv.Get(), green);
        source.OnDraw(ctx, R1(8));
        CHECK(source.Take(img) == TakeResult::Ok);
        UINT p = 0;
        const auto s = gpu.Read(img.texture.Get(), p);
        CHECK(Is(At(s, p, 64, 36), 0, 255, 0)); // the scene as it was: nothing was applied
        source.EndFrame();
    }
    g_fx.result = ApplyResult::Applied;

    // ---- supersession after the capture: the frame is rejected (the effects already ran), no second effects call ------------------
    {
        const int calls = g_fx.calls;
        ctx->ClearRenderTargetView(bbRtv.Get(), green);
        source.OnDraw(ctx, R1(9));
        CHECK_EQ(g_fx.calls, calls + 1);
        DrawEvent cleared;
        cleared.r1Cleared = true;
        source.OnDraw(ctx, cleared);   // a later scene/post stage dropped the R1 candidate
        source.OnDraw(ctx, R1(9));     // ... and a new R1 appears
        CHECK_EQ(g_fx.calls, calls + 1); // never a second render_effects in a frame
        CHECK(source.Take(img) == TakeResult::Rejected);
        CHECK(std::string(source.LastReason()) == "boundary moved after the capture");
        source.EndFrame();
        CHECK_EQ(source.GetStats().rejectedBy[size_t(Reason::Superseded)], 1u);
    }

    // ---- the engine stops asking: the next frame does nothing ---------------------------------------------------------------------
    {
        source.EndFrame(); // a Present in which the engine returned before Take()
        CHECK(!source.Armed());
        const int calls = g_fx.calls, copies = g_fx.copies;
        ctx->ClearRenderTargetView(bbRtv.Get(), green);
        source.OnDraw(ctx, R1(10));
        CHECK_EQ(g_fx.calls, calls);
        CHECK_EQ(g_fx.copies, copies);
        CHECK(Present(source, img) == TakeResult::NotThisFrame); // asking again re-arms, one frame later
        source.OnDraw(ctx, R1(11));
        CHECK_EQ(g_fx.calls, calls + 1);
        source.EndFrame();
    }

    // ---- the snapshot texture follows a size change (ResizeBuffers) ---------------------------------------------------------------
    {
        auto large = gpu.Target(256, 144);
        auto largeRtv = gpu.Rtv(large.Get());
        gpu.Bind(largeRtv.Get(), 256, 144);
        source.Take(img);
        source.EndFrame(); // armed
        source.OnDraw(ctx, R1(12, 256, 144));
        CHECK(source.Take(img) == TakeResult::Ok);
        CHECK_EQ(img.width, 256u);
        CHECK_EQ(img.height, 144u);
        D3D11_TEXTURE2D_DESC d {};
        img.texture->GetDesc(&d);
        CHECK_EQ(d.Width, 256u);
        CHECK_EQ(d.Height, 144u);
        CHECK_EQ(d.SampleDesc.Count, 1u);
        CHECK_EQ(d.ArraySize, 1u);
        source.EndFrame();
    }

    // ---- release drops the texture and the armed state -----------------------------------------------------------------------------
    source.Release();
    CHECK(!source.Armed());
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}
} // namespace

int main()
{
    Run();
    return Finish("ui_live_warp");
}
