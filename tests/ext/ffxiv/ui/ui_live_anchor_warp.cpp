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
// [HudLess] AnchorRule = early (Early(), Equality()):
//   * a frame whose first UI draw goes off-screen is captured at that draw from the BACKBUFFER: the snapshot holds the backbuffer as it
//     was there, not the off-screen target and not the untagged backbuffer draws between the off-screen block and the HUD; one copy, no
//     effects call, Ok, the same-frame fact unchanged;
//   * presets that would run (or fail) keep the frame on R1b; SkipNextCopy, supersession, a backbuffer of another identity, a guard
//     failure and a frame without a backbuffer UI draw each end as specified;
//   * frames whose first UI draw is on the backbuffer give the same results, images and counters with the key on and off.
#define NOMINMAX
#include "check.h"

#include "ext/ffxiv/ui/UiLive.h"

#include <d3dcompiler.h>

#include <cstring>
#include <string>
#include <utility>
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
    std::string lines; // the log lines the last Present wrote (EndFrame)

    // One draw: ask the core, hand the event to the live source (before the draw runs), then let the draw run.
    Core::DrawResult Draw(ID3D11PixelShader* ps)
    {
        gpu.ctx->PSSetShader(ps, nullptr, 0);
        const auto res = core.OnDraw(gpu.ctx.Get());
        const DrawEvent ev = EventOf(core, res);
        if (IsLiveEvent(ev))
            source.OnDraw(gpu.ctx.Get(), ev);
        gpu.ctx->Draw(3, 0);
        return res;
    }

    // The bridge Present: the engine asks for the frame once (Take), may say it will not use the next one (SkipNextCopy), then the exit
    // accounts it.
    TakeResult Present(FrameOutcome* outcomeOut = nullptr, bool skipNext = false)
    {
        core.FreezeFrame();
        const TakeResult r = source.Take(img);
        if (skipNext)
            source.SkipNextCopy();
        FrameOutcome outcome = core.CompleteFrame();
        lines = source.EndFrame(NoteOf(outcome));
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
    CHECK(h.img.anchoredAfterOffscreenUi); // the engine is told the frame was anchored after off-screen UI draws (the counter below counts it)
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
        CHECK(!h.img.anchoredAfterOffscreenUi); // off-screen UI draws only: there is no anchor to be late
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
        CHECK(!h.img.anchoredAfterOffscreenUi);     // HUD first: not anchor-later, as the counter says
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

// ---- [HudLess] AnchorRule = early ---------------------------------------------------------------------------------------------------
int g_predicts = 0, g_fetches = 0;
ApplyResult g_predicted = ApplyResult::NothingToApply;
ID3D11Texture2D* g_fetchTexture = nullptr; // what the backbuffer fetch hands out (with a reference); null = unknown
ApplyResult FakePredict(ID3D11DeviceContext*)
{
    ++g_predicts;
    return g_predicted;
}
bool FakeBackbuffer(ID3D11Texture2D** out)
{
    ++g_fetches;
    *out = g_fetchTexture;
    if (g_fetchTexture)
        g_fetchTexture->AddRef();
    return g_fetchTexture != nullptr;
}
ULONG Refs(IUnknown* o)
{
    o->AddRef();
    return o->Release();
}

struct Scene
{
    Gpu& gpu;
    ComPtr<ID3D11Texture2D> bb, offscreen;
    ComPtr<ID3D11RenderTargetView> bbRtv, offRtv;
    ComPtr<ID3D11PixelShader> psScene, psScene2, psUi, psYellow, psWhite;
    static constexpr D3D11_VIEWPORT kFull { 0, 0, 128, 72, 0, 1 };
    static constexpr D3D11_VIEWPORT kTopRight { 96, 0, 32, 18, 0, 1 };    // an untagged draw BEFORE the off-screen block
    static constexpr D3D11_VIEWPORT kBottomRight { 96, 54, 32, 18, 0, 1 }; // an untagged draw AFTER it, before the HUD
    static constexpr D3D11_VIEWPORT kHud { 0, 0, 32, 18, 0, 1 };
    static constexpr D3D11_VIEWPORT kOff { 0, 0, 64, 64, 0, 1 };

    explicit Scene(Gpu& g) : gpu(g)
    {
        bb = gpu.Target(128, 72);
        bbRtv = gpu.Rtv(bb.Get());
        offscreen = gpu.Target(64, 64, DXGI_FORMAT_B8G8R8A8_UNORM); // the off-screen 352x352 UI target of an in-world frame
        offRtv = gpu.Rtv(offscreen.Get());
        psScene = gpu.Ps(0, 1, 0);  // stage 1, green, full screen
        psScene2 = gpu.Ps(0, 1, 1); // stage 1 too, cyan: a later scene stage tells its pixels from the first one's
        psUi = gpu.Ps(0, 0, 1);     // UI set, blue
        psYellow = gpu.Ps(1, 1, 0); // untagged
        psWhite = gpu.Ps(1, 1, 1);  // untagged
    }
    void Tag(Core& core)
    {
        core.TagShader(psScene.Get(), 0x85E777EFu, 1512);
        core.TagShader(psScene2.Get(), 0x85E777EFu, 1512);
        core.TagShader(psUi.Get(), 3865947726u, 2048);
    }
    void SceneStage(Harness& h)
    {
        const float black[4] {};
        gpu.Bind(bbRtv.Get(), kFull);
        gpu.ctx->ClearRenderTargetView(bbRtv.Get(), black);
        h.Draw(psScene.Get());
    }
    // The anchor-later layout of the city frames (last scene stage, untagged, the off-screen block, untagged, HUD):
    // ordinals 1 scene, 2 yellow, 3..5 the block (3 = R1), 6 white, 7..8 HUD (7 = R1b).
    void AnchorLaterFrame(Harness& h, bool hud = true, Core::DrawResult* r1 = nullptr, Core::DrawResult* r1b = nullptr)
    {
        SceneStage(h);
        gpu.Bind(bbRtv.Get(), kTopRight);
        h.Draw(psYellow.Get());
        const float red[4] { 1, 0, 0, 1 };
        gpu.ctx->ClearRenderTargetView(offRtv.Get(), red);
        gpu.Bind(offRtv.Get(), kOff);
        const auto first = h.Draw(psUi.Get());
        if (r1)
            *r1 = first;
        h.Draw(psUi.Get());
        h.Draw(psUi.Get());
        gpu.Bind(bbRtv.Get(), kBottomRight);
        h.Draw(psWhite.Get());
        if (!hud)
            return;
        gpu.Bind(bbRtv.Get(), kHud);
        const auto anchor = h.Draw(psUi.Get());
        if (r1b)
            *r1b = anchor;
        h.Draw(psUi.Get());
    }
    // The normal layout: the first UI draw is on the backbuffer (R1 == R1b), the off-screen block and an untagged draw come after it.
    void NormalFrame(Harness& h)
    {
        SceneStage(h);
        gpu.Bind(bbRtv.Get(), kTopRight);
        h.Draw(psYellow.Get());
        gpu.Bind(bbRtv.Get(), kHud);
        h.Draw(psUi.Get());
        gpu.Bind(offRtv.Get(), kOff);
        h.Draw(psUi.Get());
        h.Draw(psUi.Get());
        gpu.Bind(bbRtv.Get(), kBottomRight);
        h.Draw(psWhite.Get());
        gpu.Bind(bbRtv.Get(), kHud);
        h.Draw(psUi.Get());
    }
};

void Early()
{
    Gpu gpu;
    CHECK(gpu.Init());
    if (!gpu.device)
        return;
    gpu.ctx->VSSetShader(gpu.vs.Get(), nullptr, 0);
    gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Scene sc(gpu);
    CHECK(sc.psScene && sc.psScene2 && sc.psUi && sc.psYellow && sc.psWhite);
    g_bb = sc.bb.Get();
    g_bbShape = { 128, 72, DXGI_FORMAT_R8G8B8A8_UNORM };
    g_fetchTexture = sc.bb.Get();

    auto* core = new Core;
    core->SetBackbufferProvider(&BackbufferOf);
    sc.Tag(*core);
    Env env;
    env.effects = &FakeApply;
    env.copy = &CountingCopy;
    env.predictEffects = &FakePredict;
    env.backbuffer = &FakeBackbuffer;
    LiveSource source(env);
    Harness h { gpu, *core, source, sc.psScene, sc.psUi, {} };
    g_fx.result = ApplyResult::NothingToApply; // no technique enabled: the boundary applies nothing (the saved play sessions)

    sc.NormalFrame(h);
    CHECK(h.Present() == TakeResult::NotThisFrame); // arms the source
    CHECK(source.Armed());
    source.SetAnchorRule(AnchorRule::Early);

    UINT pitch = 0;
    // ---- an anchor-later frame: captured at R1 from the backbuffer ------------------------------------------------------------------
    {
        const auto before = source.GetStats();
        const int calls = g_fx.calls, copies = g_fx.copies, predicts = g_predicts, fetches = g_fetches;
        const ULONG refs = Refs(sc.bb.Get());
        Core::DrawResult r1 {}, r1b {};
        sc.SceneStage(h);
        gpu.Bind(sc.bbRtv.Get(), Scene::kTopRight);
        h.Draw(sc.psYellow.Get());
        const float red[4] { 1, 0, 0, 1 };
        gpu.ctx->ClearRenderTargetView(sc.offRtv.Get(), red);
        gpu.Bind(sc.offRtv.Get(), Scene::kOff);
        r1 = h.Draw(sc.psUi.Get());                    // ordinal 3: R1, into the off-screen target
        CHECK_EQ(r1.ordinal, 3u);
        CHECK_EQ(r1.r1Offscreen, 3u);
        CHECK(r1.r1OffscreenChanged);
        CHECK_EQ(g_fx.copies, copies + 1);             // the copy happened here, before the off-screen draw ran
        CHECK_EQ(g_fx.calls, calls);                   // no effects call: nothing would be applied
        CHECK_EQ(g_predicts, predicts + 1);
        CHECK_EQ(g_fetches, fetches + 1);
        CHECK_EQ(Refs(sc.bb.Get()), refs);             // the fetched backbuffer reference was dropped after the copy
        h.Draw(sc.psUi.Get());
        h.Draw(sc.psUi.Get());
        gpu.Bind(sc.bbRtv.Get(), Scene::kBottomRight);
        h.Draw(sc.psWhite.Get());                      // ordinal 6: an untagged draw into the backbuffer after the block
        gpu.Bind(sc.bbRtv.Get(), Scene::kHud);
        r1b = h.Draw(sc.psUi.Get());                   // ordinal 7: R1b
        CHECK_EQ(r1b.r1b, 7u);
        CHECK(r1b.r1bChanged);
        h.Draw(sc.psUi.Get());
        CHECK_EQ(g_fx.copies, copies + 1);             // no second copy at R1b
        CHECK_EQ(g_fx.calls, calls);

        const auto backbuffer = gpu.Read(sc.bb.Get(), pitch);
        FrameOutcome o;
        CHECK(h.Present(&o) == TakeResult::Ok);
        CHECK(h.img.anchoredAfterOffscreenUi); // the same-frame fact is still r1 != r1b
        CHECK(h.img.texture != nullptr);
        if (h.img.texture)
        {
            CHECK_EQ(h.img.width, 128u);              // the backbuffer, not the 64x64 off-screen target
            CHECK_EQ(h.img.height, 72u);
            CHECK(h.img.format == DXGI_FORMAT_R8G8B8A8_UNORM);
            UINT sp = 0;
            const auto snap = gpu.Read(h.img.texture.Get(), sp);
            CHECK(Is(At(snap, sp, 64, 36), 0, 255, 0));    // the scene
            CHECK(Is(At(snap, sp, 110, 8), 255, 255, 0));  // the untagged draw before R1 is in it
            CHECK(Is(At(snap, sp, 110, 62), 0, 255, 0));   // the untagged draw after the block is NOT
            CHECK(Is(At(snap, sp, 5, 5), 0, 255, 0));      // no HUD
            CHECK(Is(At(backbuffer, pitch, 110, 62), 255, 255, 255)); // ... though the backbuffer has both
            CHECK(Is(At(backbuffer, pitch, 5, 5), 0, 0, 255));
        }
        CHECK_EQ(o.record.candidates.r1, 3u);
        CHECK_EQ(o.r1b, 7u);
        const auto st = source.GetStats();
        CHECK_EQ(st.captured, before.captured + 1);
        CHECK_EQ(st.delivered, before.delivered + 1);
        CHECK_EQ(st.earlyAnchors, before.earlyAnchors + 1);
        CHECK_EQ(st.anchorLater, before.anchorLater + 1); // counted as before (r1 != r1b)
        CHECK_EQ(st.rejected, before.rejected);
        CHECK_EQ(st.effects[size_t(ApplyResult::NothingToApply)], before.effects[size_t(ApplyResult::NothingToApply)] + 1);
        CHECK(source.Summary().find("early anchors 1") != std::string::npos);
    }

    // ---- the same frame with the rule back on r1b: captured at R1b, with the untagged draw after the block ------------------------------
    {
        source.SetAnchorRule(AnchorRule::R1b);
        const auto before = source.GetStats();
        const int calls = g_fx.calls, predicts = g_predicts, fetches = g_fetches;
        sc.AnchorLaterFrame(h);
        CHECK_EQ(g_fx.calls, calls + 1); // the effects call at R1b, as today
        CHECK_EQ(g_predicts, predicts);  // nothing about the early anchor is asked
        CHECK_EQ(g_fetches, fetches);
        CHECK(h.Present() == TakeResult::Ok);
        CHECK(h.img.anchoredAfterOffscreenUi);
        if (h.img.texture)
        {
            UINT sp = 0;
            const auto snap = gpu.Read(h.img.texture.Get(), sp);
            CHECK(Is(At(snap, sp, 110, 62), 255, 255, 255)); // the late capture holds the draw after the block
            CHECK(Is(At(snap, sp, 5, 5), 0, 255, 0));
        }
        CHECK_EQ(source.GetStats().earlyAnchors, before.earlyAnchors);
        source.SetAnchorRule(AnchorRule::Early);
    }

    // ---- presets that would run keep the frame on R1b: the effects and the copy run at R1b on the backbuffer ----------------------------
    {
        g_predicted = ApplyResult::Applied;
        g_fx.result = ApplyResult::Applied;
        const auto before = source.GetStats();
        const int calls = g_fx.calls, copies = g_fx.copies, fetches = g_fetches;
        Core::DrawResult r1 {};
        sc.AnchorLaterFrame(h, true, &r1);
        CHECK_EQ(g_fetches, fetches);                  // not even fetched
        CHECK_EQ(g_fx.calls, calls + 1);               // once, at R1b
        CHECK_EQ(g_fx.copies, copies + 1);
        CHECK(g_fx.lastTarget == sc.bb.Get());
        CHECK(h.Present() == TakeResult::Ok);
        if (h.img.texture)
        {
            UINT sp = 0;
            const auto snap = gpu.Read(h.img.texture.Get(), sp);
            CHECK(Is(At(snap, sp, 64, 36), 255, 0, 255)); // the presets (magenta) ran on the backbuffer before the copy
        }
        const auto st = source.GetStats();
        CHECK_EQ(st.earlyAnchors, before.earlyAnchors);
        CHECK_EQ(st.effects[size_t(ApplyResult::Applied)], before.effects[size_t(ApplyResult::Applied)] + 1);
        // ... and an effects call that would fail keeps R1b too: it fails there, as today
        g_predicted = ApplyResult::Failed;
        g_fx.result = ApplyResult::Failed;
        sc.AnchorLaterFrame(h);
        CHECK(h.Present() == TakeResult::Rejected);
        CHECK(std::string(source.LastReason()) == "ReShade effects failed");
        CHECK_EQ(source.GetStats().earlyAnchors, before.earlyAnchors);
        g_predicted = ApplyResult::NothingToApply;
        g_fx.result = ApplyResult::NothingToApply;
    }

    // ---- SkipNextCopy on an early frame: acted on at R1, nothing copied, no effects call, NotThisFrame with the skip reason ---------------
    {
        sc.NormalFrame(h);
        CHECK(h.Present(nullptr, true) == TakeResult::Ok); // this Present says the next frame's image is not wanted
        const auto before = source.GetStats();
        const int calls = g_fx.calls, copies = g_fx.copies, predicts = g_predicts;
        sc.AnchorLaterFrame(h);
        CHECK_EQ(g_fx.copies, copies);
        CHECK_EQ(g_fx.calls, calls);
        CHECK_EQ(g_predicts, predicts + 1);
        CHECK(h.Present() == TakeResult::NotThisFrame);
        CHECK(h.img.texture == nullptr);
        CHECK(std::string(source.LastReason()) == "copy skipped (the coverage guard probes one frame in N)");
        CHECK(h.img.anchoredAfterOffscreenUi);
        const auto st = source.GetStats();
        CHECK_EQ(st.copySkipped, before.copySkipped + 1);
        CHECK_EQ(st.earlyAnchors, before.earlyAnchors); // no copy, no early anchor
        CHECK_EQ(st.captured, before.captured);
        CHECK_EQ(st.rejected, before.rejected);
    }

    // ---- SkipNextCopy on an early frame that never draws UI into the backbuffer: today's outcome, and the skip reason is not used ---------
    {
        sc.NormalFrame(h);
        CHECK(h.Present(nullptr, true) == TakeResult::Ok); // this Present says the next frame's image is not wanted
        const auto before = source.GetStats();
        const int copies = g_fx.copies;
        sc.AnchorLaterFrame(h, false);                     // off-screen UI only: the early action is a skip, no R1b follows
        CHECK_EQ(g_fx.copies, copies);
        CHECK(h.Present() == TakeResult::NotThisFrame);
        // the frame drew no UI into the backbuffer: that is its reason. The skip reason belongs to frames that also reached R1b
        CHECK(std::string(source.LastReason()) == "no boundary in this frame");
        CHECK(h.img.texture == nullptr);
        const auto st = source.GetStats();
        CHECK_EQ(st.copySkipped, before.copySkipped);      // taken back with the frame: r1b does not act on it either
        CHECK_EQ(st.offscreenOnly, before.offscreenOnly + 1);
        CHECK_EQ(st.earlyOffscreenOnly, before.earlyOffscreenOnly + 1);
        CHECK_EQ(st.earlyAnchors, before.earlyAnchors);
        CHECK_EQ(st.rejected, before.rejected);
    }

    // ---- a scene stage after the early capture, before any UI draw reached the backbuffer: nothing was applied there (the presets gate
    //      lets only frames with nothing to apply through), so the capture is taken back and the frame stays open; its R1b then acts as
    //      with r1b (re-armed). After R1b came, a scene stage supersedes the frame as today ------------------------------------------------
    {
        const auto before = source.GetStats();
        const int copies = g_fx.copies, calls = g_fx.calls;
        sc.SceneStage(h);
        const float red[4] { 1, 0, 0, 1 };
        gpu.ctx->ClearRenderTargetView(sc.offRtv.Get(), red);
        gpu.Bind(sc.offRtv.Get(), Scene::kOff);
        h.Draw(sc.psUi.Get());                         // R1 off-screen: captured
        CHECK_EQ(g_fx.copies, copies + 1);
        gpu.Bind(sc.bbRtv.Get(), Scene::kFull);
        const auto stage = h.Draw(sc.psScene2.Get());  // a later scene stage (cyan) drops R1 (no R1b yet)
        CHECK(stage.r1OffscreenChanged);
        CHECK_EQ(stage.r1Offscreen, 0u);
        {
            const auto mid = source.GetStats();        // the early capture is taken back from every counter it went into
            CHECK_EQ(mid.captured, before.captured);
            CHECK_EQ(mid.earlyAnchors, before.earlyAnchors);
            CHECK_EQ(mid.earlyRearmed, before.earlyRearmed + 1);
            CHECK_EQ(mid.effects[size_t(ApplyResult::NothingToApply)], before.effects[size_t(ApplyResult::NothingToApply)]);
            CHECK_EQ(mid.rejected, before.rejected);
        }
        gpu.Bind(sc.bbRtv.Get(), Scene::kHud);
        h.Draw(sc.psUi.Get());                         // R1 == R1b now: the R1b path acts, effects call and copy
        CHECK_EQ(g_fx.copies, copies + 2);
        CHECK_EQ(g_fx.calls, calls + 1);
        CHECK(g_fx.lastTarget == sc.bb.Get());
        CHECK(h.Present() == TakeResult::Ok);
        if (h.img.texture)
        {
            UINT sp = 0;
            const auto snap = gpu.Read(h.img.texture.Get(), sp);
            CHECK(Is(At(snap, sp, 64, 36), 0, 255, 255)); // the second scene stage (cyan): the R1b copy, not the early one
            CHECK(Is(At(snap, sp, 5, 5), 0, 255, 255));   // no HUD
        }
        {
            const auto st = source.GetStats();
            CHECK_EQ(st.rejectedBy[size_t(Reason::Superseded)], before.rejectedBy[size_t(Reason::Superseded)]);
            CHECK_EQ(st.captured, before.captured + 1);
            CHECK_EQ(st.delivered, before.delivered + 1);
            CHECK_EQ(st.earlyAnchors, before.earlyAnchors);
            CHECK_EQ(st.effects[size_t(ApplyResult::NothingToApply)], before.effects[size_t(ApplyResult::NothingToApply)] + 1);
            CHECK_EQ(st.earlyRearmed, 1u);
            CHECK(source.Summary().find("re-armed 1, off-screen only " + std::to_string(st.earlyOffscreenOnly) + ";") != std::string::npos);
        }

        sc.AnchorLaterFrame(h);                        // early capture at R1, R1b at 7 ...
        gpu.Bind(sc.bbRtv.Get(), Scene::kFull);
        h.Draw(sc.psScene.Get());                      // ... then a scene stage drops both
        CHECK(h.Present() == TakeResult::Rejected);
        CHECK(std::string(source.LastReason()) == "boundary moved after the capture");
        const auto st = source.GetStats();
        CHECK_EQ(st.rejectedBy[size_t(Reason::Superseded)], before.rejectedBy[size_t(Reason::Superseded)] + 1);
        CHECK_EQ(st.earlyAnchors, before.earlyAnchors + 1); // that copy was made and stays counted
        CHECK_EQ(st.earlyRearmed, before.earlyRearmed + 1);
    }

    // ---- a take-back, then another off-screen R1: the second early capture is the frame's, from the backbuffer at that draw -----------
    {
        const auto before = source.GetStats();
        const int copies = g_fx.copies, calls = g_fx.calls;
        sc.SceneStage(h);                              // green
        const float red[4] { 1, 0, 0, 1 };
        gpu.ctx->ClearRenderTargetView(sc.offRtv.Get(), red);
        gpu.Bind(sc.offRtv.Get(), Scene::kOff);
        h.Draw(sc.psUi.Get());                         // R1 off-screen: early copy 1 (green)
        gpu.Bind(sc.bbRtv.Get(), Scene::kFull);
        h.Draw(sc.psScene2.Get());                     // a scene stage (cyan): taken back
        gpu.Bind(sc.offRtv.Get(), Scene::kOff);
        h.Draw(sc.psUi.Get());                         // R1 off-screen again: early copy 2 (cyan)
        h.Draw(sc.psUi.Get());
        gpu.Bind(sc.bbRtv.Get(), Scene::kBottomRight);
        h.Draw(sc.psWhite.Get());                      // an untagged backbuffer draw after the block: not in the capture
        gpu.Bind(sc.bbRtv.Get(), Scene::kHud);
        h.Draw(sc.psUi.Get());                         // R1b: confirms copy 2, nothing more
        CHECK_EQ(g_fx.copies, copies + 2);
        CHECK_EQ(g_fx.calls, calls);
        CHECK(h.Present() == TakeResult::Ok);
        CHECK(h.img.anchoredAfterOffscreenUi);
        CHECK(h.img.texture != nullptr);
        if (h.img.texture)
        {
            UINT sp = 0;
            const auto snap = gpu.Read(h.img.texture.Get(), sp);
            CHECK(Is(At(snap, sp, 64, 36), 0, 255, 255));  // the later scene stage: copy 2, not copy 1
            CHECK(Is(At(snap, sp, 110, 62), 0, 255, 255)); // not the untagged draw after the block
            CHECK(Is(At(snap, sp, 5, 5), 0, 255, 255));    // no HUD
        }
        const auto st = source.GetStats();
        CHECK_EQ(st.captured, before.captured + 1);
        CHECK_EQ(st.delivered, before.delivered + 1);
        CHECK_EQ(st.earlyAnchors, before.earlyAnchors + 1);
        CHECK_EQ(st.earlyRearmed, before.earlyRearmed + 1);
        CHECK_EQ(st.effects[size_t(ApplyResult::NothingToApply)], before.effects[size_t(ApplyResult::NothingToApply)] + 1);
        CHECK_EQ(st.rejected, before.rejected);
    }

    // ---- the same re-arm on a frame whose copy the engine skips: the skip is counted once, at the R1b that acts --------------------------
    {
        sc.NormalFrame(h);
        CHECK(h.Present(nullptr, true) == TakeResult::Ok); // the next frame's image is not wanted
        const auto before = source.GetStats();
        const int copies = g_fx.copies;
        sc.SceneStage(h);
        gpu.Bind(sc.offRtv.Get(), Scene::kOff);
        h.Draw(sc.psUi.Get());                         // R1 off-screen: acted on, copy skipped
        gpu.Bind(sc.bbRtv.Get(), Scene::kFull);
        h.Draw(sc.psScene.Get());                      // dropped: re-armed
        CHECK_EQ(source.GetStats().copySkipped, before.copySkipped);
        gpu.Bind(sc.bbRtv.Get(), Scene::kHud);
        h.Draw(sc.psUi.Get());                         // R1b: acted on, copy skipped
        CHECK_EQ(g_fx.copies, copies);
        CHECK(h.Present() == TakeResult::NotThisFrame);
        CHECK(std::string(source.LastReason()) == "copy skipped (the coverage guard probes one frame in N)");
        const auto st = source.GetStats();
        CHECK_EQ(st.copySkipped, before.copySkipped + 1);
        CHECK_EQ(st.earlyRearmed, before.earlyRearmed + 1);
        CHECK_EQ(st.rejected, before.rejected);
    }

    // ---- an early capture taken back by a later scene stage that no R1b follows: the frame is today's no-boundary frame, and its image is
    //      NOT delivered (the taken-back capture stays in the snapshot texture, unused) ----------------------------------------------
    {
        const auto before = source.GetStats();
        const int copies = g_fx.copies;
        sc.SceneStage(h);
        gpu.Bind(sc.offRtv.Get(), Scene::kOff);
        h.Draw(sc.psUi.Get());                         // R1 off-screen: captured
        CHECK_EQ(g_fx.copies, copies + 1);
        gpu.Bind(sc.bbRtv.Get(), Scene::kFull);
        h.Draw(sc.psScene.Get());                      // a scene stage with no R1b before it: the capture is taken back
        CHECK_EQ(source.GetStats().earlyRearmed, before.earlyRearmed + 1);
        CHECK(h.Present() == TakeResult::NotThisFrame);
        CHECK(h.img.texture == nullptr);               // the taken-back capture is not delivered
        CHECK(std::string(source.LastReason()) == "no boundary in this frame");
        const auto st = source.GetStats();
        CHECK_EQ(st.captured, before.captured);        // every counter the early action moved is back
        // the frame's R1 was cleared by the same scene stage, so the whole-frame view is "no boundary" (today's count for such a frame)
        CHECK_EQ(st.noBoundary, before.noBoundary + 1);
        CHECK_EQ(st.unused, before.unused);
        CHECK_EQ(st.delivered, before.delivered);
        CHECK_EQ(st.rejected, before.rejected);
    }

    // ---- the same with the copy skipped: the skip of a taken-back action does not outlive the take-back ------------------------------
    {
        sc.NormalFrame(h);
        CHECK(h.Present(nullptr, true) == TakeResult::Ok); // the next frame's image is not wanted
        const auto before = source.GetStats();
        sc.SceneStage(h);
        gpu.Bind(sc.offRtv.Get(), Scene::kOff);
        h.Draw(sc.psUi.Get());                         // R1 off-screen: acted on, copy skipped
        gpu.Bind(sc.bbRtv.Get(), Scene::kFull);
        h.Draw(sc.psScene.Get());                      // dropped: re-armed, nothing acts again in this frame
        CHECK(h.Present() == TakeResult::NotThisFrame);
        CHECK(h.img.texture == nullptr);
        // the take-back cleared the skip too: the frame is a no-boundary frame, not a skipped copy
        CHECK(std::string(source.LastReason()) == "no boundary in this frame");
        const auto st = source.GetStats();
        CHECK_EQ(st.copySkipped, before.copySkipped);
        CHECK_EQ(st.earlyRearmed, before.earlyRearmed + 1);
        CHECK_EQ(st.rejected, before.rejected);
    }

    // ---- an R1b that came BEFORE the frame's off-screen UI draw is not the R1b that confirms the early capture: the later scene stage
    //      still takes the capture back, and the frame stays the no-boundary frame it is --------------------------------------------------
    {
        const auto before = source.GetStats();
        const int copies = g_fx.copies;
        gpu.Bind(sc.bbRtv.Get(), Scene::kHud);
        h.Draw(sc.psUi.Get());                         // HUD first: R1 and R1b before any scene stage, nothing acts
        gpu.Bind(sc.bbRtv.Get(), Scene::kFull);
        h.Draw(sc.psScene.Get());                      // a scene stage drops both again
        gpu.Bind(sc.offRtv.Get(), Scene::kOff);
        h.Draw(sc.psUi.Get());                         // R1 off-screen: the early capture
        CHECK_EQ(g_fx.copies, copies + 1);
        gpu.Bind(sc.bbRtv.Get(), Scene::kFull);
        h.Draw(sc.psScene.Get());                      // no R1b AFTER the capture: taken back, not superseded
        CHECK_EQ(source.GetStats().earlyRearmed, before.earlyRearmed + 1);
        CHECK(h.Present() == TakeResult::NotThisFrame);
        CHECK(h.img.texture == nullptr);
        CHECK(std::string(source.LastReason()) == "no boundary in this frame");
        const auto st = source.GetStats();
        CHECK_EQ(st.rejectedBy[size_t(Reason::Superseded)], before.rejectedBy[size_t(Reason::Superseded)]);
        CHECK_EQ(st.rejected, before.rejected);
        CHECK_EQ(st.captured, before.captured);
        CHECK_EQ(st.noBoundary, before.noBoundary + 1); // the frame is still counted where it is today
    }

    // ---- a backbuffer fetch that names another texture than the core's backbuffer: the frame keeps R1b ----------------------------------
    {
        auto other = gpu.Target(128, 72);
        g_fetchTexture = other.Get();
        const auto before = source.GetStats();
        const int calls = g_fx.calls, copies = g_fx.copies, fetches = g_fetches;
        sc.AnchorLaterFrame(h);
        CHECK_EQ(g_fetches, fetches + 1);
        CHECK_EQ(g_fx.calls, calls + 1);               // the R1b path ran
        CHECK_EQ(g_fx.copies, copies + 1);
        CHECK(h.Present() == TakeResult::Ok);
        if (h.img.texture)
        {
            UINT sp = 0;
            const auto snap = gpu.Read(h.img.texture.Get(), sp);
            CHECK(Is(At(snap, sp, 110, 62), 255, 255, 255)); // captured at R1b
        }
        CHECK_EQ(source.GetStats().earlyAnchors, before.earlyAnchors);
        CHECK_EQ(source.GetStats().earlyDeclined[size_t(EarlyDecline::Fetch)], before.earlyDeclined[size_t(EarlyDecline::Fetch)] + 1);
        g_fetchTexture = sc.bb.Get();
    }

    // ---- a backbuffer the guard refuses (BGRA8): no early capture; R1b rejects it as today, without side effects -----------------------
    {
        auto bgra = gpu.Target(128, 72, DXGI_FORMAT_B8G8R8A8_UNORM);
        auto bgraRtv = gpu.Rtv(bgra.Get());
        g_bb = bgra.Get();
        g_bbShape = { 128, 72, DXGI_FORMAT_B8G8R8A8_UNORM };
        g_fetchTexture = bgra.Get();
        const auto before = source.GetStats();
        const int calls = g_fx.calls, copies = g_fx.copies;
        const float black[4] {};
        gpu.Bind(bgraRtv.Get(), Scene::kFull);
        gpu.ctx->ClearRenderTargetView(bgraRtv.Get(), black);
        h.Draw(sc.psScene.Get());
        gpu.Bind(sc.offRtv.Get(), Scene::kOff);
        h.Draw(sc.psUi.Get());                         // R1 off-screen: the guard refuses the backbuffer, nothing happens
        gpu.Bind(bgraRtv.Get(), Scene::kHud);
        h.Draw(sc.psUi.Get());                         // R1b: the same guard rejects it
        CHECK_EQ(g_fx.calls, calls);
        CHECK_EQ(g_fx.copies, copies);
        CHECK(h.Present() == TakeResult::Rejected);
        CHECK(std::string(source.LastReason()) == "render target is not the full-size backbuffer");
        const auto st = source.GetStats();
        CHECK_EQ(st.rejectedBy[size_t(Reason::FormatUnsupported)], before.rejectedBy[size_t(Reason::FormatUnsupported)] + 1);
        CHECK_EQ(st.earlyAnchors, before.earlyAnchors);
        CHECK_EQ(st.earlyDeclined[size_t(EarlyDecline::Target)], before.earlyDeclined[size_t(EarlyDecline::Target)] + 1);
        g_bb = sc.bb.Get();
        g_bbShape = { 128, 72, DXGI_FORMAT_R8G8B8A8_UNORM };
        g_fetchTexture = sc.bb.Get();
    }

    // ---- UI only into the off-screen target (no UI draw into the backbuffer): today's outcome, not delivered -------------------------------
    {
        const auto before = source.GetStats();
        const int copies = g_fx.copies;
        sc.AnchorLaterFrame(h, false);
        CHECK_EQ(g_fx.copies, copies + 1);             // the early copy is made at R1 (whether R1b follows is not known yet) ...
        FrameOutcome o;
        CHECK(h.Present(&o) == TakeResult::NotThisFrame); // ... and not delivered: there is no boundary in this frame
        CHECK(h.img.texture == nullptr);
        CHECK(!h.img.anchoredAfterOffscreenUi);
        CHECK(std::string(source.LastReason()) == "no boundary in this frame");
        CHECK_EQ(o.r1b, 0u);
        const auto st = source.GetStats();
        CHECK_EQ(st.offscreenOnly, before.offscreenOnly + 1); // counted where today counts it
        CHECK_EQ(st.noBoundary, before.noBoundary);
        CHECK_EQ(st.delivered, before.delivered);
        CHECK_EQ(st.unused, before.unused);                 // the engine did ask: not an unused capture
        CHECK_EQ(st.rejected, before.rejected);
        // its counters are taken back at the end of the frame, so it counts exactly as without the key; the wasted copy shows in the
        // early detail only
        CHECK_EQ(st.earlyAnchors, before.earlyAnchors);
        CHECK_EQ(st.captured, before.captured);
        CHECK_EQ(st.effects[size_t(ApplyResult::NothingToApply)], before.effects[size_t(ApplyResult::NothingToApply)]);
        CHECK_EQ(st.earlyOffscreenOnly, before.earlyOffscreenOnly + 1);
        CHECK(source.Summary().find("off-screen only " + std::to_string(st.earlyOffscreenOnly) + ";") != std::string::npos);
    }

    // ---- a normal frame (R1 == R1b) with the rule on early: the R1b path, nothing about the early anchor is asked --------------------------
    {
        const int calls = g_fx.calls, copies = g_fx.copies, predicts = g_predicts, fetches = g_fetches;
        sc.NormalFrame(h);
        CHECK_EQ(g_predicts, predicts);
        CHECK_EQ(g_fetches, fetches);
        CHECK_EQ(g_fx.calls, calls + 1);
        CHECK_EQ(g_fx.copies, copies + 1);
        CHECK(h.Present() == TakeResult::Ok);
        CHECK(!h.img.anchoredAfterOffscreenUi);
    }

    // ---- a taken-back early action leaves the ReShade state where it was: nothing ran there, so no "presets at the boundary" line -------
    {
        // ordinary frames whose capture records Applied: the announced state settles to "applied" and the state line's rate limit opens
        g_fx.result = ApplyResult::Applied;
        g_predicted = ApplyResult::NothingToApply;
        for (int i = 0; i < 400; ++i)
        {
            sc.NormalFrame(h);
            (void) h.Present();
        }
        sc.NormalFrame(h);
        CHECK(h.Present() == TakeResult::Ok);
        CHECK(h.lines.find("ReShade presets at the boundary") == std::string::npos); // settled: nothing left to announce
        const int calls = g_fx.calls, copies = g_fx.copies;
        sc.SceneStage(h);
        gpu.Bind(sc.offRtv.Get(), Scene::kOff);
        h.Draw(sc.psUi.Get());                         // the early path predicts "nothing to apply" and records it
        gpu.Bind(sc.bbRtv.Get(), Scene::kFull);
        h.Draw(sc.psScene.Get());                      // re-armed: the recorded prediction is taken back with the action
        CHECK(h.Present() == TakeResult::NotThisFrame);
        CHECK_EQ(g_fx.calls, calls);                   // no effects call ran in that frame
        CHECK_EQ(g_fx.copies, copies + 1);
        // the frame's last effects result is still "applied" (the prediction never ran), so there is no change to report
        CHECK(h.lines.find("ReShade presets at the boundary") == std::string::npos);
        // ... and the same for an early frame that never draws UI into the backbuffer: its prediction is taken back at the frame end
        sc.AnchorLaterFrame(h, false);
        CHECK_EQ(g_fx.copies, copies + 2);
        CHECK(h.Present() == TakeResult::NotThisFrame);
        CHECK(h.lines.find("ReShade presets at the boundary") == std::string::npos);
    }

    // ---- a source whose only early event is a frame without a backbuffer UI draw: the summary's early detail shows it ---------------------
    {
        auto* fresh = new Core; // never freed, like the cores above
        fresh->SetBackbufferProvider(&BackbufferOf);
        sc.Tag(*fresh);
        LiveSource only(env);
        Harness h2 { gpu, *fresh, only, sc.psScene, sc.psUi, {} };
        sc.NormalFrame(h2);
        CHECK(h2.Present() == TakeResult::NotThisFrame); // arms the source
        only.SetAnchorRule(AnchorRule::Early);
        sc.AnchorLaterFrame(h2, false);
        CHECK(h2.Present() == TakeResult::NotThisFrame);
        CHECK_EQ(only.GetStats().earlyOffscreenOnly, 1u);
        CHECK(only.Summary().find("early anchors 0 (re-armed 0, off-screen only 1; declined: backbuffer unknown 0, presets 0, fetch 0, "
                                  "target 0, texture 0); ReShade presets") != std::string::npos);
        only.Release();
    }
    source.Release();
    gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g_fetchTexture = nullptr;
}

// Frames whose first UI draw after the last scene stage is on the backbuffer: identical results, images and counters with the key on and
// off (the frames of the R1b tests above: HUD first, an off-screen block after the HUD, a superseded capture, a menu frame, a skipped copy,
// a guard rejection, an untagged backbuffer draw before the HUD, no backbuffer known; and two frames whose online R1 went off-screen
// before a later scene stage, where early re-arms: one extra copy, nothing else differs).
struct FrameResult
{
    TakeResult take = TakeResult::NoSource;
    std::string reason;
    bool anchorLater = false;
    bool texture = false;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::vector<Px> pixels;
    int calls = 0, copies = 0;
    bool operator==(const FrameResult& o) const
    {
        if (pixels.size() != o.pixels.size())
            return false;
        for (size_t i = 0; i < pixels.size(); ++i)
            if (!Is(pixels[i], o.pixels[i].r, o.pixels[i].g, o.pixels[i].b))
                return false;
        return take == o.take && reason == o.reason && anchorLater == o.anchorLater && texture == o.texture && width == o.width &&
               height == o.height && format == o.format && calls == o.calls && copies == o.copies;
    }
};

std::vector<FrameResult> RunNormalFrames(Gpu& gpu, Scene& sc, AnchorRule rule, Stats& stats)
{
    auto* core = new Core; // never freed, like the cores above (the shader stamps point at its records)
    core->SetBackbufferProvider(&BackbufferOf);
    sc.Tag(*core);
    Env env;
    env.effects = &FakeApply;
    env.copy = &CountingCopy;
    env.predictEffects = &FakePredict;
    env.backbuffer = &FakeBackbuffer;
    LiveSource source(env);
    source.SetAnchorRule(rule);
    Harness h { gpu, *core, source, sc.psScene, sc.psUi, {} };
    g_fx.result = ApplyResult::Applied;
    g_predicted = ApplyResult::NothingToApply; // the early path may act on the off-screen draw of the re-armed frames
    g_bb = sc.bb.Get();
    g_bbShape = { 128, 72, DXGI_FORMAT_R8G8B8A8_UNORM };
    g_fetchTexture = sc.bb.Get();
    const float black[4] {};

    std::vector<FrameResult> out;
    auto frame = [&](auto&& draws, bool skipNext = false)
    {
        FrameResult r;
        const int calls = g_fx.calls, copies = g_fx.copies;
        draws();
        r.calls = g_fx.calls - calls;
        r.copies = g_fx.copies - copies;
        r.take = h.Present(nullptr, skipNext);
        r.reason = source.LastReason();
        r.anchorLater = h.img.anchoredAfterOffscreenUi;
        r.texture = h.img.texture != nullptr;
        r.width = h.img.width;
        r.height = h.img.height;
        r.format = h.img.format;
        if (h.img.texture)
        {
            UINT sp = 0;
            const auto snap = gpu.Read(h.img.texture.Get(), sp);
            const std::pair<UINT, UINT> points[] = { { 64, 36 }, { 110, 8 }, { 110, 62 }, { 5, 5 } };
            for (const auto& [x, y] : points)
                r.pixels.push_back(At(snap, sp, x, y));
        }
        out.push_back(std::move(r));
    };

    frame([&] { sc.NormalFrame(h); });                // arming
    frame([&] { sc.NormalFrame(h); });                // HUD first, the off-screen block after it
    frame([&] {                                       // HUD first, then nothing off-screen
        sc.SceneStage(h);
        gpu.Bind(sc.bbRtv.Get(), Scene::kHud);
        h.Draw(sc.psUi.Get());
        h.Draw(sc.psUi.Get());
    });
    frame([&] {                                       // an untagged backbuffer draw between the scene stage and the HUD
        sc.SceneStage(h);
        gpu.Bind(sc.bbRtv.Get(), Scene::kBottomRight);
        h.Draw(sc.psWhite.Get());
        gpu.Bind(sc.bbRtv.Get(), Scene::kHud);
        h.Draw(sc.psUi.Get());
    });
    frame([&] {                                       // superseded after the capture
        sc.SceneStage(h);
        gpu.Bind(sc.bbRtv.Get(), Scene::kHud);
        h.Draw(sc.psUi.Get());
        gpu.Bind(sc.bbRtv.Get(), Scene::kFull);
        h.Draw(sc.psScene.Get());
        gpu.Bind(sc.bbRtv.Get(), Scene::kHud);
        h.Draw(sc.psUi.Get());
    });
    frame([&] {                                       // a menu frame (no scene stage)
        gpu.Bind(sc.bbRtv.Get(), Scene::kFull);
        gpu.ctx->ClearRenderTargetView(sc.bbRtv.Get(), black);
        h.Draw(sc.psWhite.Get());
        gpu.Bind(sc.bbRtv.Get(), Scene::kHud);
        h.Draw(sc.psUi.Get());
    }, true);                                         // ... whose Present skips the next copy
    frame([&] { sc.NormalFrame(h); });                // the skipped copy
    frame([&] {                                       // a backbuffer the guard refuses
        auto bgra = gpu.Target(128, 72, DXGI_FORMAT_B8G8R8A8_UNORM);
        auto bgraRtv = gpu.Rtv(bgra.Get());
        g_bb = bgra.Get();
        g_bbShape = { 128, 72, DXGI_FORMAT_B8G8R8A8_UNORM };
        g_fetchTexture = bgra.Get();
        gpu.Bind(bgraRtv.Get(), Scene::kFull);
        gpu.ctx->ClearRenderTargetView(bgraRtv.Get(), black);
        h.Draw(sc.psScene.Get());
        gpu.Bind(bgraRtv.Get(), Scene::kHud);
        h.Draw(sc.psUi.Get());
        g_bb = sc.bb.Get();
        g_bbShape = { 128, 72, DXGI_FORMAT_R8G8B8A8_UNORM };
        g_fetchTexture = sc.bb.Get();
    });
    frame([&] {                                       // no backbuffer known to the core
        g_bb = nullptr;
        sc.NormalFrame(h);
        g_bb = sc.bb.Get();
    });
    frame([&] { sc.NormalFrame(h); });                // and an ordinary one
    // A UI draw into the off-screen target, then a scene stage, then the HUD: the final R1 is on the backbuffer (R1 == R1b at the end of
    // the frame), but the online R1 went off-screen first. With early that draw takes a capture the scene stage then takes back (re-armed),
    // so the frame's R1b acts exactly as with r1b; the only difference is that one extra copy (Equality() checks it)
    auto rearmed = [&]
    {
        sc.SceneStage(h);
        const float red[4] { 1, 0, 0, 1 };
        gpu.ctx->ClearRenderTargetView(sc.offRtv.Get(), red);
        gpu.Bind(sc.offRtv.Get(), Scene::kOff);
        h.Draw(sc.psUi.Get());
        gpu.Bind(sc.bbRtv.Get(), Scene::kTopRight);
        h.Draw(sc.psScene.Get());
        gpu.Bind(sc.bbRtv.Get(), Scene::kHud);
        h.Draw(sc.psUi.Get());
    };
    frame(rearmed, true);                             // ... whose Present skips the next copy
    frame(rearmed);                                   // the same with the copy skipped (no copy either way)
    frame([&] { sc.AnchorLaterFrame(h, false); });    // UI only off the backbuffer: early copies, then counts as r1b does
    frame([&] { sc.NormalFrame(h); });                // and a last ordinary one
    stats = source.GetStats();
    source.Release();
    return out;
}

void Equality()
{
    Gpu gpu;
    CHECK(gpu.Init());
    if (!gpu.device)
        return;
    gpu.ctx->VSSetShader(gpu.vs.Get(), nullptr, 0);
    gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Scene sc(gpu);
    Stats off {}, on {};
    const int predicts0 = g_predicts, fetches0 = g_fetches;
    const auto r1b = RunNormalFrames(gpu, sc, AnchorRule::R1b, off);
    CHECK_EQ(g_predicts, predicts0); // the r1b rule never asks
    CHECK_EQ(g_fetches, fetches0);
    const int predicts = g_predicts, fetches = g_fetches;
    auto early = RunNormalFrames(gpu, sc, AnchorRule::Early, on);
    // only the two re-armed frames (10, 11) and the off-screen-only frame (12) consult the early path: the frame without a known backbuffer has an R1 off-screen event, but
    // nothing is asked without a backbuffer identity
    CHECK_EQ(g_predicts, predicts + 3);
    CHECK_EQ(g_fetches, fetches + 3);
    CHECK_EQ(r1b.size(), early.size());
    if (early.size() > 12 && r1b.size() > 12)
    {
        CHECK_EQ(early[10].copies, r1b[10].copies + 1); // the capture taken back
        early[10].copies = r1b[10].copies;
        CHECK_EQ(early[11].copies, 0);                  // skipped either way
        CHECK_EQ(early[12].copies, r1b[12].copies + 1); // the copy of a frame that never reached the backbuffer
        early[12].copies = r1b[12].copies;
    }
    for (size_t i = 0; i < r1b.size() && i < early.size(); ++i)
    {
        if (!(r1b[i] == early[i]))
            std::printf("frame %zu differs: take %d/%d reason '%s'/'%s'\n", i, int(r1b[i].take), int(early[i].take), r1b[i].reason.c_str(),
                        early[i].reason.c_str());
        CHECK(r1b[i] == early[i]);
    }
    // the outcomes the frames were written for
    CHECK(r1b.size() == 14 && r1b[1].take == TakeResult::Ok && r1b[4].take == TakeResult::Rejected && r1b[6].take == TakeResult::NotThisFrame &&
          r1b[7].take == TakeResult::Rejected && r1b[8].take == TakeResult::NotThisFrame && r1b[9].take == TakeResult::Ok &&
          r1b[10].take == TakeResult::Ok && r1b[11].take == TakeResult::NotThisFrame && r1b[12].take == TakeResult::NotThisFrame &&
          r1b[13].take == TakeResult::Ok);
    CHECK(off.frames == on.frames && off.captured == on.captured && off.delivered == on.delivered && off.unused == on.unused &&
          off.noBoundary == on.noBoundary && off.offscreenOnly == on.offscreenOnly && off.anchorLater == on.anchorLater &&
          off.rejected == on.rejected && off.rejectedBy == on.rejectedBy && off.effects == on.effects && off.takes == on.takes &&
          off.copySkipped == on.copySkipped && off.earlyAnchors == on.earlyAnchors);
    CHECK_EQ(on.earlyAnchors, 0u);
    CHECK_EQ(off.earlyRearmed, 0u);
    CHECK_EQ(on.earlyRearmed, 2u);
    CHECK_EQ(off.earlyOffscreenOnly, 0u);
    CHECK_EQ(on.earlyOffscreenOnly, 1u);
    // log only: the early run declined once for the unknown backbuffer, the r1b run never looked
    CHECK_EQ(on.earlyDeclined[size_t(EarlyDecline::UnknownBackbuffer)], 1u);
    CHECK(off.earlyDeclined == decltype(off.earlyDeclined) {});
    gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g_fetchTexture = nullptr;
}
} // namespace

int main()
{
    Run();
    Early();
    Equality();
    return Finish("ui_live_anchor_warp");
}
