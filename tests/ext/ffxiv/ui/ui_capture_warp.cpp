// S10-A: WARP D3D11 harness for the draw-time core and the boundary capture (docs/HUDLESS.md section 10).
// Proves on a real (software) device that
//   * the copy taken at the R1 / R2 candidate equals the render target bound at that draw, BEFORE the draw ran, and is not
//     changed by the draw that follows;
//   * shaders stamped through TagShader are recognised per draw, shaders never tagged are counted as created before the hook,
//     draws issued while "Present" runs and draws from another thread are kept apart;
//   * the render target is classified against the backbuffer by pointer identity;
//   * the capture refuses what the engine could not use (no target, multisampled, wrong size) and describes typeless targets.
#define NOMINMAX
#include "check.h"

#include "ext/ffxiv/ui/UiBoundaryCore.h"
#include "ext/ffxiv/ui/UiCapture.h"

#include <d3dcompiler.h>

#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::ui;

namespace
{
const void* g_bb = nullptr;
dfmt::RtShape g_bbShape {};
bool Backbuffer(const void** t, dfmt::RtShape* s)
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
        const char* hlsl = "float4 VS(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}";
        ComPtr<ID3DBlob> code, err;
        if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &code, &err)))
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

    ComPtr<ID3D11RenderTargetView> Rtv(ID3D11Texture2D* t, DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN)
    {
        D3D11_RENDER_TARGET_VIEW_DESC d {};
        d.Format = format;
        d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> v;
        device->CreateRenderTargetView(t, format == DXGI_FORMAT_UNKNOWN ? nullptr : &d, &v);
        return v;
    }

    // Reads back a texture: copies into a staging texture of the same size and format.
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

void Run()
{
    Gpu gpu;
    CHECK(gpu.Init());
    if (!gpu.device)
        return;
    auto* ctx = gpu.ctx.Get();

    auto bb = gpu.Target(128, 72);
    auto bbRtv = gpu.Rtv(bb.Get());
    auto other = gpu.Target(64, 64);
    auto otherRtv = gpu.Rtv(other.Get());
    g_bb = bb.Get();
    g_bbShape = { 128, 72, DXGI_FORMAT_R8G8B8A8_UNORM };

    auto psScene = gpu.Ps(1, 0, 0);  // stage 1 (tonemap)
    auto psGamma = gpu.Ps(0, 1, 0);  // stage 5 (fullscreen gamma)
    auto psUi = gpu.Ps(0, 0, 1);     // UI
    auto psPlain = gpu.Ps(1, 1, 0);  // hooked, no stage
    auto psOld = gpu.Ps(1, 0, 1);    // never passes through TagShader: created before the hook
    CHECK(psScene && psGamma && psUi && psPlain && psOld);

    auto* core = new Core;
    core->SetBackbufferProvider(&Backbuffer);
    core->TagShader(psScene.Get(), 0x85E777EFu, 1512);
    core->TagShader(psGamma.Get(), 0xF6E81A1Bu, 900);
    core->TagShader(psUi.Get(), 3865947726u, 2048);
    core->TagShader(psPlain.Get(), 0x12345678u, 300);

    gpu.ctx->VSSetShader(gpu.vs.Get(), nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT full { 0, 0, 128, 72, 0, 1 };
    D3D11_VIEWPORT corner { 0, 0, 32, 18, 0, 1 };
    ctx->RSSetViewports(1, &full);

    auto draw = [&](ID3D11PixelShader* ps) -> Core::DrawResult
    {
        ctx->PSSetShader(ps, nullptr, 0);
        const auto res = core->OnDraw(ctx);
        ctx->Draw(3, 0);
        return res;
    };

    // ---- frame 1 ---------------------------------------------------------------------------------------------------
    ctx->OMSetRenderTargets(1, bbRtv.GetAddressOf(), nullptr);
    const float black[4] {};
    ctx->ClearRenderTargetView(bbRtv.Get(), black);
    auto r1 = draw(psScene.Get());                                      // #1 stage 1 into the backbuffer
    CHECK_EQ(r1.ordinal, 1u);
    std::thread([&] { core->OnDraw(ctx); }).join();                      // a foreign thread's draw during the frame is ignored
    CHECK_EQ(core->Stats().otherThreadDraws, 1u);
    CHECK_EQ(r1.stage, 1);
    ctx->OMSetRenderTargets(1, otherRtv.GetAddressOf(), nullptr);
    draw(psPlain.Get());                                                // #2 untagged, other target
    draw(psOld.Get());                                                  // #3 shader created before the hook
    ctx->OMSetRenderTargets(1, bbRtv.GetAddressOf(), nullptr);
    draw(psGamma.Get());                                                // #4 stage 5 into the backbuffer (green)

    // The UI draw: ask the core first (the hook does), copy at the candidate, then let the draw run.
    ctx->PSSetShader(psUi.Get(), nullptr, 0);
    ctx->RSSetViewports(1, &corner);
    const auto ui = core->OnDraw(ctx);
    CHECK_EQ(ui.ordinal, 5u);
    CHECK_EQ(ui.stage, kUiStage);
    CHECK(ui.moved);
    CHECK_EQ(ui.change, 3);          // R1 and R2 both fire on this draw
    CHECK_EQ(ui.cand.r1, 5u);
    CHECK_EQ(ui.cand.r2, 5u);
    CHECK_EQ(ui.cand.r3, 5u);
    CHECK_EQ(ui.r1b, 5u);            // S10 E2: the first UI draw into the backbuffer (pointer identity) is also the live anchor
    CHECK(ui.r1bChanged);

    UINT refPitch = 0;
    const auto reference = gpu.Read(bb.Get(), refPitch); // what the render target holds right now, before the UI draw
    ComPtr<ID3D11Texture2D> snapshot;
    capture::Bound bound;
    const auto r = capture::CopyBound(gpu.device.Get(), ctx, true, 128, 72, snapshot, nullptr, bound);
    CHECK(r == capture::Result::Ok);
    CHECK(bound.texture.Get() == bb.Get());
    CHECK_EQ(bound.desc.Width, 128u);
    ctx->Draw(3, 0); // the boundary draw: blue into the top-left corner
    ctx->RSSetViewports(1, &full);

    UINT pitch = 0;
    const auto after = gpu.Read(bb.Get(), pitch);
    UINT snapPitch = 0;
    const auto snap = gpu.Read(snapshot.Get(), snapPitch); // staging read: same content as the staging copy
    CHECK(Is(At(reference, refPitch, 100, 60), 0, 255, 0));
    CHECK(Is(At(reference, refPitch, 5, 5), 0, 255, 0));   // before the UI draw the corner is still green
    CHECK(Is(At(after, pitch, 5, 5), 0, 0, 255));          // after the UI draw it is blue
    CHECK(Is(At(after, pitch, 100, 60), 0, 255, 0));
    bool equal = snapPitch == refPitch;
    for (UINT y = 0; equal && y < 72; ++y)
        equal = std::memcmp(snap.data() + size_t(y) * snapPitch, reference.data() + size_t(y) * refPitch, 128 * 4) == 0;
    CHECK(equal); // the boundary copy equals the render target bound at that draw, before the draw
    CHECK(!Is(At(snap, snapPitch, 5, 5), 0, 0, 255));

    // The render target classification used for R2 and for rt.txt.
    ctx->OMSetRenderTargets(1, bbRtv.GetAddressOf(), nullptr);
    CHECK(core->ClassifyRt(ctx).kind == dfmt::RtKind::Backbuffer);
    ctx->OMSetRenderTargets(1, otherRtv.GetAddressOf(), nullptr);
    const auto rtOther = core->ClassifyRt(ctx);
    CHECK(rtOther.kind == dfmt::RtKind::Other);
    CHECK_EQ(rtOther.shape.width, 64u);
    auto twin = gpu.Target(128, 72);
    auto twinRtv = gpu.Rtv(twin.Get());
    ctx->OMSetRenderTargets(1, twinRtv.GetAddressOf(), nullptr);
    CHECK(core->ClassifyRt(ctx).kind == dfmt::RtKind::SameAsBackbuffer);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    CHECK(core->ClassifyRt(ctx).kind == dfmt::RtKind::None);

    // ---- Present: draws issued inside it are counted apart, a foreign thread's draws are ignored ----------------------
    ctx->OMSetRenderTargets(1, bbRtv.GetAddressOf(), nullptr);
    core->FreezeFrame();
    draw(psPlain.Get());
    draw(psPlain.Get());
    std::thread([&] { core->OnDraw(ctx); }).join();
    auto outcome = core->CompleteFrame();
    const auto& rec = outcome.record;
    CHECK_EQ(rec.frame, 1u);
    CHECK_EQ(rec.summary.draws, 5u);
    CHECK_EQ(rec.presentDraws, 2u);
    CHECK_EQ(rec.present.size(), 2u);
    CHECK(rec.present.size() == 2 && rec.present[0].id == "0x12345678");
    CHECK_EQ(core->Stats().otherThreadDraws, 2u);
    CHECK_EQ(rec.candidates.r1, 5u);
    CHECK_EQ(rec.candidates.r2, 5u);
    CHECK_EQ(outcome.r1b, 5u);
    CHECK_EQ(core->Stats().framesR1b, 1u);
    CHECK_EQ(core->Stats().framesR1bLater, 0u);
    CHECK_EQ(core->Stats().framesUiNoR1b, 0u);
    CHECK_EQ(rec.summary.unknownDraws, 1u);   // the shader created before the hook
    CHECK_EQ(core->PreHookShaders(), 1u);
    // listed draws carry their final flags and render target class
    bool sawUi = false, sawStage5 = false, sawUnknown = false;
    for (const auto& d : rec.draws)
    {
        if (d.ordinal == 5)
        {
            sawUi = d.stage == kUiStage && d.r1 && d.r2 && d.r3 && d.rt == dfmt::RtKind::Backbuffer;
        }
        if (d.ordinal == 4)
            sawStage5 = d.stage == 5 && d.rt == dfmt::RtKind::Backbuffer && !d.r1;
        if (d.ordinal == 3)
            sawUnknown = d.stage == kStageUnknown;
    }
    CHECK(sawUi);
    CHECK(sawStage5);
    CHECK(sawUnknown);
    // the record round-trips through frames.txt
    const auto parsed = dfmt::ParseFrames(dfmt::FrameRecordText(rec));
    CHECK_EQ(parsed.size(), 1u);
    CHECK(parsed.size() == 1 && parsed[0] == rec);

    // shaders.csv rows: the pre-hook shader has no CRC, the tagged ones carry stage and render target samples
    const auto rows = core->ShaderRows();
    bool pre = false, uiRow = false;
    for (const auto& row : rows)
    {
        if (row.preHook)
            pre = !row.hasCrc && row.id == "pre#1" && row.stage == kStageUnknown && row.draws == 1;
        if (row.hasCrc && row.crc == 3865947726u)
            uiRow = row.ui && row.stage == kUiStage && row.draws == 1 && row.bbSamples == 1 && row.rtSamples == 1;
    }
    CHECK(pre);
    CHECK(uiRow);

    // ---- frame 2: a UI draw before the last scene stage is not the boundary; clearing is reported -----------------------
    ctx->OMSetRenderTargets(1, bbRtv.GetAddressOf(), nullptr);
    const auto early = draw(psUi.Get());
    CHECK_EQ(early.cand.r1, 1u);
    CHECK_EQ(early.change & 1, 1);
    const auto later = draw(psScene.Get());     // a scene stage after it drops the candidate
    CHECK_EQ(later.cand.r1, 0u);
    CHECK_EQ(later.change & 1, 1);              // cleared: the consumer must not capture at a draw whose cand.r1 differs
    CHECK(later.cand.r1 != later.ordinal);
    CHECK_EQ(early.r1b, 1u);                    // S10 E2: R1b follows R1 here (UI draw into the backbuffer) ...
    CHECK(early.r1bChanged);
    CHECK_EQ(later.r1b, 0u);                    // ... and is cleared by the same scene stage draw
    CHECK(later.r1bChanged);
    core->FreezeFrame();
    const auto frame2 = core->CompleteFrame();
    CHECK_EQ(frame2.r1b, 0u);
    CHECK_EQ(frame2.record.frame, 2u);
    CHECK_EQ(frame2.record.summary.r1Supersessions, 1u);
    CHECK_EQ(frame2.record.summary.uiBeforeLastScene, 1u);

    // ---- capture refusals ----------------------------------------------------------------------------------------------
    ComPtr<ID3D11Texture2D> dst;
    capture::Bound b2;
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    CHECK(capture::CopyBound(gpu.device.Get(), ctx, true, 0, 0, dst, nullptr, b2) == capture::Result::NoRenderTarget);
    ctx->OMSetRenderTargets(1, bbRtv.GetAddressOf(), nullptr);
    CHECK(capture::CopyBound(gpu.device.Get(), ctx, true, 256, 144, dst, nullptr, b2) == capture::Result::SizeMismatch);
    auto msaa = gpu.Target(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
    if (msaa)
    {
        auto msaaRtv = gpu.Rtv(msaa.Get());
        if (msaaRtv)
        {
            ctx->OMSetRenderTargets(1, msaaRtv.GetAddressOf(), nullptr);
            CHECK(capture::CopyBound(gpu.device.Get(), ctx, true, 0, 0, dst, nullptr, b2) == capture::Result::Multisampled);
        }
    }

    // a typeless target with a typed view: the copy is described with the typed format
    auto typeless = gpu.Target(64, 64, DXGI_FORMAT_R8G8B8A8_TYPELESS);
    auto typedRtv = gpu.Rtv(typeless.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(typeless && typedRtv);
    ctx->OMSetRenderTargets(1, typedRtv.GetAddressOf(), nullptr);
    capture::Bound b3;
    ComPtr<ID3D11Texture2D> dst3;
    CHECK(capture::CopyBound(gpu.device.Get(), ctx, false, 64, 64, dst3, nullptr, b3) == capture::Result::Ok);
    CHECK(capture::TypedFormat(b3) == DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D11_TEXTURE2D_DESC dd {};
    dst3->GetDesc(&dd);
    CHECK_EQ(dd.Usage, D3D11_USAGE_DEFAULT); // the v1 delivery texture is default usage, not staging
    CHECK_EQ(dd.Format, DXGI_FORMAT_R8G8B8A8_TYPELESS);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}
} // namespace

int main()
{
    Run();
    return Finish("ui_capture_warp");
}
