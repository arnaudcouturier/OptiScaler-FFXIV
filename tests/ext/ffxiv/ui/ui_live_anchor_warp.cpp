// S10-E2: WARP D3D11 proof of the live ANCHOR (R1b) end to end: the draw-time core (real render targets, pointer-identity backbuffer),
// the shipped hook glue (live::EventOf) and the live source with a fake effects layer, in the order the hook runs them
// (Core::OnDraw -> EventOf -> LiveSource::OnDraw -> the draw), then the bridge Present (FreezeFrame / CompleteFrame / Take /
// EndFrame).
//   * a UI draw into a preview render target comes first, the backbuffer UI draw second: the backbuffer draw is the anchor, the
//     frame is captured (effects run once, the snapshot holds the modified scene and not the HUD), nothing was rejected;
//   * UI draws into other render targets only: no effects, no copy, NotThisFrame, no rejection counted;
//   * the first UI draw on the backbuffer (the 99.5% case) and a preview drawn after it: unchanged, captured at the first draw;
//   * a scene stage after the capture still supersedes it (Rejected); a backbuffer the guard cannot use is rejected with a reason
//     and has no side effects.
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
const void* g_bb = nullptr;
dfmt::RtShape g_bbShape {};
bool BackbufferOf(const void** t, dfmt::RtShape* s)
{
    *t = g_bb;
    *s = g_bbShape;
    return g_bb != nullptr;
}

struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vs;

    bool Init()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx)))
            return false;
        const char* v = "float4 VS(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}";
        ComPtr<ID3DBlob> code, err;
        if (FAILED(D3DCompile(v, strlen(v), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &code, &err)))
            return false;
        return SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs));
    }

    ComPtr<ID3D11PixelShader> Ps(float r, float g, float b)
    {
        char hlsl[160];
        snprintf(hlsl, sizeof hlsl, "float4 PS():SV_Target{return float4(%f,%f,%f,1);}", r, g, b);
        ComPtr<ID3DBlob> code, err;
        ComPtr<ID3D11PixelShader> ps;
        if (SUCCEEDED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &code, &err)))
            device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps);
        return ps;
    }

    ComPtr<ID3D11Texture2D> Target(UINT w, UINT h, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        D3D11_TEXTURE2D_DESC d {};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
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

    void Bind(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp)
    {
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
    }

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

// ---- the fake effects layer ------------------------------------------------------------------------------------------------------
struct Fx
{
    int calls = 0, copies = 0;
    ID3D11Resource* lastTarget = nullptr;
    ApplyResult result = ApplyResult::Applied;
};
Fx g_fx;

ApplyResult FakeApply(ID3D11DeviceContext* c, ID3D11Resource* target)
{
    ++g_fx.calls;
    g_fx.lastTarget = target;
    if (g_fx.result == ApplyResult::Applied)
    {
        ComPtr<ID3D11RenderTargetView> rtv;
        c->OMGetRenderTargets(1, &rtv, nullptr);
        const float magenta[4] { 1, 0, 1, 1 };
        if (rtv)
            c->ClearRenderTargetView(rtv.Get(), magenta);
    }
    return g_fx.result;
}

void CountingCopy(ID3D11DeviceContext* c, ID3D11Resource* dest, ID3D11Resource* source)
{
    ++g_fx.copies;
    c->CopyResource(dest, source);
}

// ---- the hook, as FfxivUiBoundary.cpp runs it ---------------------------------------------------------------------------------------
struct Harness
{
    Gpu& gpu;
    Core& core;
    LiveSource& source;
    ComPtr<ID3D11PixelShader> psScene, psUi;
    FrameImage img;

    // One draw: ask the core, hand the event to the live source (before the draw runs), then let the draw run.
    Core::DrawResult Draw(ID3D11PixelShader* ps)
    {
        gpu.ctx->PSSetShader(ps, nullptr, 0);
        const auto res = core.OnDraw(gpu.ctx.Get());
        const DrawEvent ev = EventOf(core, res);
        if (ev.r1bSet || ev.r1bCleared)
            source.OnDraw(gpu.ctx.Get(), ev);
        gpu.ctx->Draw(3, 0);
        return res;
    }

    // The bridge Present: the engine asks for the frame once (Take), then the exit accounts it.
    TakeResult Present(FrameOutcome* outcomeOut = nullptr)
    {
        core.FreezeFrame();
        const TakeResult r = source.Take(img);
        FrameOutcome outcome = core.CompleteFrame();
        source.EndFrame(NoteOf(outcome));
        if (outcomeOut)
            *outcomeOut = std::move(outcome);
        return r;
    }
};

void Run()
{
    Gpu gpu;
    CHECK(gpu.Init());
    if (!gpu.device)
        return;
    auto* ctx = gpu.ctx.Get();

    auto bb = gpu.Target(128, 72);
    auto bbRtv = gpu.Rtv(bb.Get());
    auto preview = gpu.Target(64, 64, DXGI_FORMAT_B8G8R8A8_UNORM); // the character preview of Session 1 (352x352 B8G8R8A8)
    auto previewRtv = gpu.Rtv(preview.Get());
    g_bb = bb.Get();
    g_bbShape = { 128, 72, DXGI_FORMAT_R8G8B8A8_UNORM };

    auto psScene = gpu.Ps(0, 1, 0); // stage 1 (tonemap): draws the scene colour (green), full screen
    auto psUi = gpu.Ps(0, 0, 1);    // UI: blue
    CHECK(psScene && psUi);

    auto* core = new Core;
    core->SetBackbufferProvider(&BackbufferOf);
    core->TagShader(psScene.Get(), 0x85E777EFu, 1512);
    core->TagShader(psUi.Get(), 3865947726u, 2048);

    Env env;
    env.effects = &FakeApply;
    env.copy = &CountingCopy;
    LiveSource source(env);
    Harness h { gpu, *core, source, psScene, psUi, {} };

    gpu.ctx->VSSetShader(gpu.vs.Get(), nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D11_VIEWPORT full { 0, 0, 128, 72, 0, 1 };
    const D3D11_VIEWPORT corner { 0, 0, 32, 18, 0, 1 };
    const D3D11_VIEWPORT previewVp { 0, 0, 64, 64, 0, 1 };
    const float black[4] {};

    auto sceneFrame = [&]
    {
        gpu.Bind(bbRtv.Get(), full);
        ctx->ClearRenderTargetView(bbRtv.Get(), black);
        h.Draw(psScene.Get()); // ordinal 1: the scene stage, green over the whole backbuffer
    };

    // ---- frame 1: the engine has not asked yet; the Present's Take() arms the next frame -------------------------------------------
    sceneFrame();
    gpu.Bind(bbRtv.Get(), corner);
    h.Draw(psUi.Get());
    CHECK_EQ(g_fx.calls, 0);
    CHECK(h.Present() == TakeResult::NotThisFrame);
    CHECK(source.Armed());

    // ---- frame 2: PREVIEW first, backbuffer second -> the backbuffer UI draw is the anchor ------------------------------------------
    sceneFrame();
    gpu.Bind(previewRtv.Get(), previewVp);
    const auto previewDraw = h.Draw(psUi.Get()); // ordinal 2: UI into the preview render target
    CHECK_EQ(previewDraw.cand.r1, 2u);           // R1 (the old live anchor) sits on the preview ...
    CHECK_EQ(previewDraw.r1b, 0u);               // ... R1b does not exist yet
    CHECK(!previewDraw.r1bChanged);
    CHECK_EQ(g_fx.calls, 0);                     // the preview draw is ignored: no effects, no copy, no rejection
    CHECK_EQ(g_fx.copies, 0);
    CHECK_EQ(source.GetStats().rejected, 0u);
    h.Draw(psUi.Get());                          // ordinal 3: a second preview draw, still ignored
    CHECK_EQ(g_fx.calls, 0);
    gpu.Bind(bbRtv.Get(), corner);
    const auto hud = h.Draw(psUi.Get());         // ordinal 4: the first UI draw into the backbuffer
    CHECK_EQ(hud.r1b, 4u);
    CHECK(hud.r1bChanged);
    CHECK_EQ(g_fx.calls, 1);                     // effects ran at the anchor, once ...
    CHECK_EQ(g_fx.copies, 1);                    // ... and the copy followed them
    CHECK(g_fx.lastTarget == bb.Get());
    h.Draw(psUi.Get());                          // ordinal 5: later HUD draws change nothing
    h.Draw(psUi.Get());
    CHECK_EQ(g_fx.calls, 1);
    CHECK_EQ(g_fx.copies, 1);

    UINT pitch = 0, snapPitch = 0;
    const auto backbuffer = gpu.Read(bb.Get(), pitch); // after the HUD draws
    FrameOutcome outcome;
    CHECK(h.Present(&outcome) == TakeResult::Ok);
    CHECK(h.img.texture != nullptr);
    if (!h.img.texture)
        return; // a failure above; do not dereference
    CHECK_EQ(h.img.width, 128u);
    CHECK_EQ(h.img.height, 72u);
    CHECK(h.img.format == DXGI_FORMAT_R8G8B8A8_UNORM);
    const auto snap = gpu.Read(h.img.texture.Get(), snapPitch);
    CHECK(Is(At(snap, snapPitch, 100, 60), 255, 0, 255)); // the effects (magenta) ran BEFORE the copy, over the scene
    CHECK(Is(At(snap, snapPitch, 5, 5), 255, 0, 255));    // the HUD (blue, top-left) is NOT in the snapshot
    CHECK(Is(At(backbuffer, pitch, 5, 5), 0, 0, 255));    // ... but it is in the backbuffer
    CHECK_EQ(outcome.record.candidates.r1, 2u);           // the frame's R1 was the preview draw
    CHECK_EQ(outcome.r1b, 4u);
    {
        const auto st = source.GetStats();
        CHECK_EQ(st.captured, 1u);
        CHECK_EQ(st.delivered, 1u);
        CHECK_EQ(st.rejected, 0u);                 // the preview frame is captured, not rejected
        CHECK_EQ(st.anchorLater, 1u);              // ... and counted as one the R1 rule would have rejected
        CHECK_EQ(st.offscreenOnly, 0u);
        CHECK_EQ(st.effects[size_t(ApplyResult::Applied)], 1u);
    }

    // ---- frame 3: ONLY preview draws -> no anchor: NotThisFrame, nothing applied, nothing rejected ------------------------------------
    {
        const int calls = g_fx.calls, copies = g_fx.copies;
        sceneFrame();
        gpu.Bind(previewRtv.Get(), previewVp);
        for (int i = 0; i < 6; ++i)
            h.Draw(psUi.Get());
        gpu.Bind(bbRtv.Get(), full);
        CHECK_EQ(g_fx.calls, calls);
        CHECK_EQ(g_fx.copies, copies);
        FrameOutcome o;
        const TakeResult r = h.Present(&o);
        CHECK(r == TakeResult::NotThisFrame);
        CHECK(h.img.texture == nullptr);
        CHECK(std::string(source.LastReason()) == "no boundary in this frame");
        CHECK(o.record.candidates.r1 != 0);
        CHECK_EQ(o.r1b, 0u);
        const auto st = source.GetStats();
        CHECK_EQ(st.rejected, 0u);
        CHECK_EQ(st.offscreenOnly, 1u);
        CHECK_EQ(st.captured, 1u);
        CHECK_EQ(g_fx.calls, calls);               // ReShade's own pass would run on this frame: the live path did nothing
    }

    // ---- frame 4: the HUD starts on the backbuffer (99.5% of Session 1) and a preview follows it: captured at the FIRST draw -----------
    {
        const int calls = g_fx.calls;
        sceneFrame();
        gpu.Bind(bbRtv.Get(), corner);
        const auto first = h.Draw(psUi.Get());      // ordinal 2
        CHECK_EQ(first.r1b, 2u);
        CHECK_EQ(first.cand.r1, 2u);
        CHECK_EQ(g_fx.calls, calls + 1);
        gpu.Bind(previewRtv.Get(), previewVp);
        h.Draw(psUi.Get());                         // a preview drawn after the HUD started
        gpu.Bind(bbRtv.Get(), corner);
        h.Draw(psUi.Get());
        CHECK_EQ(g_fx.calls, calls + 1);
        CHECK(h.Present() == TakeResult::Ok);
        const auto st = source.GetStats();
        CHECK_EQ(st.captured, 2u);
        CHECK_EQ(st.anchorLater, 1u);               // R1b == R1 here: not counted
    }

    // ---- frame 5: a scene stage after the capture supersedes it (the effects already ran): Rejected -------------------------------------
    {
        const int calls = g_fx.calls;
        sceneFrame();
        gpu.Bind(bbRtv.Get(), corner);
        h.Draw(psUi.Get());                         // anchor, captured
        CHECK_EQ(g_fx.calls, calls + 1);
        gpu.Bind(bbRtv.Get(), full);
        h.Draw(psScene.Get());                      // a later scene stage drops R1b
        gpu.Bind(bbRtv.Get(), corner);
        h.Draw(psUi.Get());                         // a new R1b appears; the frame has its verdict, nothing runs again
        CHECK_EQ(g_fx.calls, calls + 1);
        CHECK(h.Present() == TakeResult::Rejected);
        CHECK(std::string(source.LastReason()) == "boundary moved after the capture");
        CHECK_EQ(source.GetStats().rejectedBy[size_t(Reason::Superseded)], 1u);
    }

    // ---- frame 6: the backbuffer the guard cannot use (BGRA8): the anchor exists, the guard rejects it, NO side effects --------------------
    {
        auto bgra = gpu.Target(128, 72, DXGI_FORMAT_B8G8R8A8_UNORM);
        auto bgraRtv = gpu.Rtv(bgra.Get());
        g_bb = bgra.Get();
        g_bbShape = { 128, 72, DXGI_FORMAT_B8G8R8A8_UNORM };
        const int calls = g_fx.calls, copies = g_fx.copies;
        gpu.Bind(bgraRtv.Get(), full);
        ctx->ClearRenderTargetView(bgraRtv.Get(), black);
        h.Draw(psScene.Get());
        gpu.Bind(bgraRtv.Get(), corner);
        const auto res = h.Draw(psUi.Get());
        CHECK_EQ(res.r1b, 2u);
        CHECK_EQ(g_fx.calls, calls);
        CHECK_EQ(g_fx.copies, copies);
        CHECK(h.Present() == TakeResult::Rejected);
        CHECK(std::string(source.LastReason()) == "render target is not the full-size backbuffer");
        CHECK_EQ(source.GetStats().rejectedBy[size_t(Reason::FormatUnsupported)], 1u);
        g_bb = bb.Get();
        g_bbShape = { 128, 72, DXGI_FORMAT_R8G8B8A8_UNORM };
    }

    // ---- frame 7: no backbuffer known to the core (provider fails): no anchor, NotThisFrame -----------------------------------------
    {
        g_bb = nullptr;
        const int calls = g_fx.calls;
        sceneFrame();
        gpu.Bind(bbRtv.Get(), corner);
        const auto res = h.Draw(psUi.Get());
        CHECK_EQ(res.r1b, 0u);
        CHECK_EQ(g_fx.calls, calls);
        CHECK(h.Present() == TakeResult::NotThisFrame);
        g_bb = bb.Get();
    }

    // ---- the engine stops asking: the next frame does nothing -----------------------------------------------------------------------
    {
        source.EndFrame(); // a Present in which the engine returned before Take()
        CHECK(!source.Armed());
        const int calls = g_fx.calls;
        sceneFrame();
        gpu.Bind(bbRtv.Get(), corner);
        h.Draw(psUi.Get());
        CHECK_EQ(g_fx.calls, calls);
        CHECK(h.Present() == TakeResult::NotThisFrame);
    }

    // ---- S10 E3: LATE ARMING ---------------------------------------------------------------------------------------------------------
    // The Session 2 failure: [HudLess] Mode was off when the game created its shaders and was switched on later in the menu. Every
    // shader below is created while the detector is NOT armed, through the shipped creation decision (Core::ShaderCreated with
    // registry = false, which is what the hook passes for Armed() == false); core->TagShader (the armed registry) is NEVER called for
    // them. Scene stage AND UI shader are stamped this way, because the core takes both stages only from the stamp (a UI-only fix
    // would leave sawSceneStage false and the frame is never acted on).
    {
        auto* lateCore = new Core;
        lateCore->SetBackbufferProvider(&BackbufferOf);
        auto lateScene = gpu.Ps(0, 1, 0);  // tonemap stage, green
        auto lateUi = gpu.Ps(0, 0, 1);     // UI, blue
        auto latePlain = gpu.Ps(1, 1, 0);  // not in the catalog (an ordinary game shader), yellow
        auto unseenScene = gpu.Ps(0, 1, 0); // never went through the creation hook (what the old gate produced)
        auto unseenUi = gpu.Ps(0, 0, 1);
        CHECK(lateScene && lateUi && latePlain && unseenScene && unseenUi);
        lateCore->ShaderCreated(lateScene.Get(), 0x85E777EFu, 1512, false);
        lateCore->ShaderCreated(lateUi.Get(), 3865947726u, 2048, false);
        lateCore->ShaderCreated(latePlain.Get(), 0x12345678u, 300, false);
        CHECK_EQ(lateCore->ShaderRows().size(), 0u); // nothing registered: D1 bookkeeping stays behind Armed()
        CHECK_EQ(lateCore->Stats().shadersCreatedAfterHook, 0u);

        // Disarmed frames: the hook does not call the core at all. Draws run, nothing is tracked.
        for (int frame = 0; frame < 3; ++frame)
        {
            gpu.Bind(bbRtv.Get(), full);
            ctx->ClearRenderTargetView(bbRtv.Get(), black);
            ctx->PSSetShader(lateScene.Get(), nullptr, 0);
            ctx->Draw(3, 0);
            gpu.Bind(bbRtv.Get(), corner);
            ctx->PSSetShader(lateUi.Get(), nullptr, 0);
            ctx->Draw(3, 0);
        }
        CHECK_EQ(lateCore->Stats().gameDraws, 0u);

        LiveSource lateSource(env);
        Harness hl { gpu, *lateCore, lateSource, lateScene, lateUi, {} };

        // The arming Present (RefreshArmed ran, the engine calls Take for the first time): arms the live source for the next frame.
        CHECK(hl.Present() == TakeResult::NotThisFrame);
        CHECK(lateSource.Armed());

        // The old failure, kept as a control: shaders that never passed through the creation hook are unknown, so there is no stage,
        // no R1b, no scene stage and nothing to act on.
        {
            const int calls = g_fx.calls;
            gpu.Bind(bbRtv.Get(), full);
            ctx->ClearRenderTargetView(bbRtv.Get(), black);
            hl.Draw(unseenScene.Get());
            gpu.Bind(bbRtv.Get(), corner);
            const auto res = hl.Draw(unseenUi.Get());
            CHECK_EQ(res.stage, kStageUnknown);
            CHECK_EQ(res.r1b, 0u);
            FrameOutcome o;
            CHECK(hl.Present(&o) == TakeResult::NotThisFrame);
            CHECK(!o.record.summary.sawSceneStage);
            CHECK_EQ(g_fx.calls, calls);
        }

        // The stamped frame: the scene stage, an ordinary shader, then the first UI draw into the backbuffer.
        {
            const int calls = g_fx.calls, copies = g_fx.copies;
            gpu.Bind(bbRtv.Get(), full);
            ctx->ClearRenderTargetView(bbRtv.Get(), black);
            const auto scene = hl.Draw(lateScene.Get());      // ordinal 1
            CHECK_EQ(scene.stage, 1);
            const auto plain = hl.Draw(latePlain.Get());      // ordinal 2: outside the catalog, created disarmed, unknown at its first draw
            CHECK_EQ(plain.stage, kStageUnknown);
            gpu.Bind(bbRtv.Get(), corner);
            const auto hud = hl.Draw(lateUi.Get());           // ordinal 3
            CHECK_EQ(hud.stage, kUiStage);
            CHECK_EQ(hud.r1b, 3u);
            CHECK(hud.r1bChanged);
            CHECK_EQ(g_fx.calls, calls + 1);                  // effects ran at the anchor, once ...
            CHECK_EQ(g_fx.copies, copies + 1);                // ... and the copy followed
            hl.Draw(lateUi.Get());                            // later HUD draws change nothing
            CHECK_EQ(g_fx.calls, calls + 1);

            UINT pitch = 0, snapPitch = 0;
            const auto backbuffer = gpu.Read(bb.Get(), pitch);
            FrameOutcome o;
            CHECK(hl.Present(&o) == TakeResult::Ok);
            CHECK(o.record.summary.sawSceneStage);
            CHECK_EQ(o.r1b, 3u);
            CHECK_EQ(o.record.summary.unknownDraws, 1u);      // only the ordinary shader
            CHECK_EQ(lateCore->PreHookShaders(), 3u);         // the two control shaders and the ordinary one (known consequence)
            CHECK(hl.img.texture != nullptr);
            if (hl.img.texture)
            {
                const auto snap = gpu.Read(hl.img.texture.Get(), snapPitch);
                CHECK(Is(At(snap, snapPitch, 100, 60), 255, 0, 255)); // effects colour, over the scene
                CHECK(Is(At(snap, snapPitch, 5, 5), 255, 0, 255));    // no HUD in the snapshot
                CHECK(Is(At(backbuffer, pitch, 5, 5), 0, 0, 255));    // the HUD is in the backbuffer
            }
            const auto st = lateSource.GetStats();
            CHECK_EQ(st.captured, 1u);
            CHECK_EQ(st.delivered, 1u);
            CHECK_EQ(st.rejected, 0u);
        }
        // The D1 registry was never used on this path; the stage records are shared, not per shader.
        CHECK_EQ(lateCore->ShaderRows().size(), 3u); // only the three pre-hook placeholders created at the first draw
        lateSource.Release();
    }

    source.Release();
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}
} // namespace

int main()
{
    Run();
    return Finish("ui_live_anchor_warp");
}
