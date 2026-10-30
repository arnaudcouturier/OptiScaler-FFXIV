// HDR census, part 2: the HUD draw census of the D1 diagnostics (ui-census.txt). Real draws on a WARP device through Core::OnDraw:
// every draw into the backbuffer at or after the boundary R1b falls in exactly one class, the boundary restarts the count when a
// later scene stage draw supersedes it, other render targets are not counted, present-time draws are counted apart, the census
// switches itself off two frames after the last KeepCensus() call, and the text is exact.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "check.h"

#include "ext/ffxiv/ui/UiBoundaryCore.h"
#include "ext/ffxiv/ui/UiDiagRecorder.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::ui;
using dfmt::CensusClass;

namespace
{
const void* g_bb = nullptr;
bool Backbuffer(const void** t, dfmt::RtShape* s)
{
    *t = g_bb;
    *s = { 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM };
    return g_bb != nullptr;
}

ComPtr<ID3D11PixelShader> Ps(ID3D11Device* d, float r)
{
    char hlsl[128];
    snprintf(hlsl, sizeof hlsl, "float4 PS():SV_Target{return float4(%f,0,0,1);}", r);
    ComPtr<ID3DBlob> code, err;
    ComPtr<ID3D11PixelShader> ps;
    if (SUCCEEDED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &code, &err)))
        d->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps);
    return ps;
}

ComPtr<ID3D11BlendState> MakeBlend(ID3D11Device* d, bool enable, D3D11_BLEND src, D3D11_BLEND dest, D3D11_BLEND_OP op = D3D11_BLEND_OP_ADD,
                                   UINT mask = 15, bool a2c = false)
{
    D3D11_BLEND_DESC b {};
    b.AlphaToCoverageEnable = a2c;
    auto& rt = b.RenderTarget[0];
    rt.BlendEnable = enable;
    rt.SrcBlend = src;
    rt.DestBlend = dest;
    rt.BlendOp = op;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = static_cast<UINT8>(mask);
    ComPtr<ID3D11BlendState> s;
    d->CreateBlendState(&b, &s);
    return s;
}

void PureText()
{
    // the class function, on its own
    CHECK(dfmt::ClassifyCensus(kUiStage, 1, false, true) == CensusClass::TaggedStandard);
    CHECK(dfmt::ClassifyCensus(kUiStage, 0, false, true) == CensusClass::TaggedStandard);
    CHECK(dfmt::ClassifyCensus(kUiStage, 2, true, true) == CensusClass::TaggedRefusedBlend);
    CHECK(dfmt::ClassifyCensus(kUiStage, 0, true, true) == CensusClass::TaggedRefusedBlend); // opaque blend but a partial write mask / a2c
    CHECK(dfmt::ClassifyCensus(kUiStage, 1, true, false) == CensusClass::TaggedRefusedBlend); // the blend is judged before the texture
    CHECK(dfmt::ClassifyCensus(kUiStage, 1, false, false) == CensusClass::TaggedNoTexture);
    CHECK(dfmt::ClassifyCensus(kStageUnknown, 2, true, true) == CensusClass::UnknownShader);
    CHECK(dfmt::ClassifyCensus(kStageNone, 0, false, false) == CensusClass::UntaggedOpaque);
    CHECK(dfmt::ClassifyCensus(kStageNone, 1, false, true) == CensusClass::UntaggedAlpha);
    CHECK(dfmt::ClassifyCensus(kStageNone, 2, true, true) == CensusClass::UntaggedAdditive);
    CHECK(dfmt::ClassifyCensus(kStageNone, 3, true, true) == CensusClass::UntaggedOther);

    dfmt::CensusTotals t;
    t.frames = 10;
    t.framesWithBoundary = 8;
    t.framesSceneNoBoundary = 1;
    t.presentDraws = 30;
    t.presentBackbufferDraws = 20;
    t.tally.n[static_cast<size_t>(CensusClass::TaggedStandard)] = 60;
    t.tally.n[static_cast<size_t>(CensusClass::TaggedRefusedBlend)] = 10;
    t.tally.taggedAdditive = 4;
    t.tally.n[static_cast<size_t>(CensusClass::UntaggedAlpha)] = 30;
    CHECK_EQ(t.tally.Total(), 100u);
    const std::string text = dfmt::CensusText("20261018-120000", 600, t);
    CHECK(text.find("run=20261018-120000 frame=600\n") != std::string::npos);
    CHECK(text.find("frames=10 with_boundary=8 scene_but_no_boundary=1\n") != std::string::npos);
    CHECK(text.find("draws_after_boundary total=100 mean_per_boundary_frame=12.5\n") != std::string::npos);
    // class lines: name, count, share (parsed, so the column padding is not part of the contract)
    auto line = [&](const char* name, unsigned long long& count, double& share)
    {
        const size_t at = text.find(std::string("\n") + name + " ");
        if (at == std::string::npos)
            return false;
        char word[64];
        return std::sscanf(text.c_str() + at + 1, "%63s %llu %lf", word, &count, &share) >= 2; // tagged_additive has no share
    };
    unsigned long long n = 0;
    double share = 0;
    CHECK(line("tagged_standard_blend", n, share) && n == 60 && share > 59.99 && share < 60.01);
    CHECK(line("tagged_refused_blend", n, share) && n == 10 && share > 9.99 && share < 10.01);
    CHECK(line("untagged_alpha", n, share) && n == 30 && share > 29.99 && share < 30.01);
    CHECK(line("untagged_opaque", n, share) && n == 0);
    CHECK(line("tagged_additive", n, share) && n == 4);
    CHECK(text.find("(subset of tagged_refused_blend)\n") != std::string::npos);
    CHECK(text.find("mask_coverable_share_pct=60.0   # tagged_standard_blend / total\n") != std::string::npos);
    CHECK(text.find("present_draws=30 present_draws_into_backbuffer=20\n") != std::string::npos);
    // an empty census prints zeros and no division by zero
    const std::string empty = dfmt::CensusText("r", 1, dfmt::CensusTotals {});
    CHECK(empty.find("draws_after_boundary total=0 mean_per_boundary_frame=0.0\n") != std::string::npos);
    CHECK(empty.find("mask_coverable_share_pct=0.0") != std::string::npos);
}

void Run()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx)))
    {
        CHECK(false);
        return;
    }
    const char* vsHlsl = "float4 VS(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}";
    ComPtr<ID3DBlob> code, err;
    ComPtr<ID3D11VertexShader> vs;
    D3DCompile(vsHlsl, strlen(vsHlsl), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &code, &err);
    device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs);

    D3D11_TEXTURE2D_DESC td {};
    td.Width = td.Height = 32;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> bb, off, tex;
    device->CreateTexture2D(&td, nullptr, &bb);
    device->CreateTexture2D(&td, nullptr, &off);
    device->CreateTexture2D(&td, nullptr, &tex);
    ComPtr<ID3D11RenderTargetView> bbRtv, offRtv;
    device->CreateRenderTargetView(bb.Get(), nullptr, &bbRtv);
    device->CreateRenderTargetView(off.Get(), nullptr, &offRtv);
    ComPtr<ID3D11ShaderResourceView> texView;
    device->CreateShaderResourceView(tex.Get(), nullptr, &texView);
    g_bb = bb.Get();

    auto standard = MakeBlend(device.Get(), true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA);
    auto additive = MakeBlend(device.Get(), true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE);
    auto coverage = MakeBlend(device.Get(), true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, 15, true);
    auto subtract = MakeBlend(device.Get(), true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_SUBTRACT);

    auto psScene1 = Ps(device.Get(), 0.1f), psScene5 = Ps(device.Get(), 0.2f), psUi = Ps(device.Get(), 0.3f), psPlain = Ps(device.Get(), 0.4f),
         psOld = Ps(device.Get(), 0.5f);

    auto* core = new Core;
    core->SetBackbufferProvider(&Backbuffer);
    core->TagShader(psScene1.Get(), 0x85E777EFu, 1512);
    core->TagShader(psScene5.Get(), 0xF6E81A1Bu, 900);
    core->TagShader(psUi.Get(), 3865947726u, 2048);
    core->TagShader(psPlain.Get(), 0x12345678u, 300);
    // psOld is never registered: a shader created before the hook

    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT vp { 0, 0, 32, 32, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    auto draw = [&](ID3D11PixelShader* ps, bool toBackbuffer, ID3D11BlendState* blend, bool withTexture)
    {
        ctx->OMSetRenderTargets(1, toBackbuffer ? bbRtv.GetAddressOf() : offRtv.GetAddressOf(), nullptr);
        ctx->OMSetBlendState(blend, nullptr, ~0u);
        ID3D11ShaderResourceView* views[8] {};
        if (withTexture)
            views[0] = texView.Get();
        ctx->PSSetShaderResources(0, 8, views);
        ctx->PSSetShader(ps, nullptr, 0);
        core->OnDraw(ctx.Get());
        ctx->Draw(3, 0);
    };
    auto count = [&](CensusClass c) { return core->Census().tally.Of(c); };

    // ---- the census is off until KeepCensus(): nothing is counted, nothing is looked up
    CHECK(!core->CensusOn());
    draw(psScene1.Get(), true, nullptr, false);
    draw(psUi.Get(), true, standard.Get(), true);
    core->FreezeFrame();
    draw(psPlain.Get(), true, nullptr, false);
    (void) core->CompleteFrame(); // frame 1 -> 2
    CHECK_EQ(core->Census().frames, 0u);
    CHECK_EQ(core->Census().tally.Total(), 0u);
    CHECK_EQ(core->Census().presentDraws, 0u);

    // ---- frame 2: every class once (Present entry of frame 1 would have armed it; here it is armed directly)
    core->KeepCensus(); // frame 2: on for frames 2 and 3
    CHECK(core->CensusOn());
    draw(psScene1.Get(), true, nullptr, false);              // scene stage: no boundary yet
    draw(psPlain.Get(), true, nullptr, false);               // untagged backbuffer draw BEFORE the boundary: not counted
    draw(psUi.Get(), true, standard.Get(), true);            // R1b: tagged_standard_blend (counted from here on)
    draw(psUi.Get(), true, additive.Get(), true);            // tagged_refused_blend, additive
    draw(psUi.Get(), true, standard.Get(), false);           // tagged_no_texture
    draw(psUi.Get(), true, coverage.Get(), true);            // tagged_refused_blend (alpha to coverage), not additive
    draw(psPlain.Get(), true, standard.Get(), true);         // untagged_alpha
    draw(psPlain.Get(), true, additive.Get(), true);         // untagged_additive
    draw(psPlain.Get(), true, subtract.Get(), true);         // untagged_other
    draw(psPlain.Get(), true, nullptr, false);               // untagged_opaque (no blend state bound = the default)
    draw(psOld.Get(), true, standard.Get(), true);           // unknown_shader (created before the hook)
    draw(psUi.Get(), false, standard.Get(), true);           // a UI draw into another render target: not counted
    draw(psPlain.Get(), false, nullptr, false);              // untagged into another target: not counted
    core->FreezeFrame();
    draw(psPlain.Get(), true, nullptr, false);               // inside Present, into the backbuffer
    draw(psPlain.Get(), false, nullptr, false);              // inside Present, elsewhere
    (void) core->CompleteFrame();                            // frame 2 -> 3
    CHECK_EQ(core->Census().frames, 1u);
    CHECK_EQ(core->Census().framesWithBoundary, 1u);
    CHECK_EQ(count(CensusClass::TaggedStandard), 1u);
    CHECK_EQ(count(CensusClass::TaggedRefusedBlend), 2u);
    CHECK_EQ(core->Census().tally.taggedAdditive, 1u);
    CHECK_EQ(count(CensusClass::TaggedNoTexture), 1u);
    CHECK_EQ(count(CensusClass::UntaggedAlpha), 1u);
    CHECK_EQ(count(CensusClass::UntaggedAdditive), 1u);
    CHECK_EQ(count(CensusClass::UntaggedOther), 1u);
    CHECK_EQ(count(CensusClass::UntaggedOpaque), 1u);
    CHECK_EQ(count(CensusClass::UnknownShader), 1u);
    CHECK_EQ(core->Census().tally.Total(), 9u); // the classes are exclusive and nothing else was counted
    CHECK_EQ(core->Census().presentDraws, 2u);
    CHECK_EQ(core->Census().presentBackbufferDraws, 1u);

    // ---- frame 3: a later scene stage draw supersedes the boundary: the count restarts at the new one
    draw(psScene1.Get(), true, nullptr, false);
    draw(psUi.Get(), true, standard.Get(), true);            // first boundary: counted ...
    draw(psPlain.Get(), true, nullptr, false);               // ... and this one ...
    draw(psScene5.Get(), true, nullptr, false);              // ... until a scene stage draw clears the boundary: both are dropped
    draw(psPlain.Get(), true, nullptr, false);               // no boundary now: not counted
    draw(psUi.Get(), true, additive.Get(), true);            // the new boundary: tagged_refused_blend, additive
    draw(psPlain.Get(), true, standard.Get(), true);         // untagged_alpha
    core->FreezeFrame();
    (void) core->CompleteFrame();                            // frame 3 -> 4
    CHECK_EQ(core->Census().frames, 2u);
    CHECK_EQ(core->Census().framesWithBoundary, 2u);
    CHECK_EQ(count(CensusClass::TaggedStandard), 1u);        // unchanged: frame 3's standard draw was superseded
    CHECK_EQ(count(CensusClass::TaggedRefusedBlend), 3u);
    CHECK_EQ(core->Census().tally.taggedAdditive, 2u);
    CHECK_EQ(count(CensusClass::UntaggedAlpha), 2u);
    CHECK_EQ(count(CensusClass::UntaggedOpaque), 1u);        // unchanged
    CHECK_EQ(core->Census().tally.Total(), 9u + 2u);

    // ---- frame 4: two frames after the last KeepCensus() it has stopped by itself (Diagnostics off, or only [HudLess] Mode on)
    CHECK(!core->CensusOn());
    const auto before = core->Census();
    draw(psScene1.Get(), true, nullptr, false);
    draw(psUi.Get(), true, standard.Get(), true);
    core->FreezeFrame();
    draw(psPlain.Get(), true, nullptr, false);
    (void) core->CompleteFrame();                            // frame 4 -> 5
    CHECK_EQ(core->Census().frames, before.frames);
    CHECK_EQ(core->Census().tally.Total(), before.tally.Total());
    CHECK_EQ(core->Census().presentDraws, before.presentDraws);

    // ---- frames 5 and 6: a scene frame without any UI draw, and a menu-only frame
    core->KeepCensus(); // frame 5
    draw(psScene1.Get(), true, nullptr, false);
    draw(psScene5.Get(), true, nullptr, false);
    (void) core->CompleteFrame();                            // frame 5 -> 6
    draw(psPlain.Get(), true, nullptr, false);               // no scene stage, no UI: a menu or loading screen
    (void) core->CompleteFrame();                            // frame 6 -> 7
    CHECK_EQ(core->Census().frames, 4u);
    CHECK_EQ(core->Census().framesWithBoundary, 2u);
    CHECK_EQ(core->Census().framesSceneNoBoundary, 1u);
    CHECK_EQ(core->Census().tally.Total(), 9u + 2u);

    // ---- the Recorder writes the file with the frame-60/300/600 schedule
    struct Sink final : diag::Sink
    {
        std::string text;
        int rewrites = 0;
        void Append(const char*, std::string) override {}
        void Rewrite(const char* f, std::string t) override
        {
            if (std::string(f) == "ui-census.txt")
            {
                text = std::move(t);
                ++rewrites;
            }
        }
    } sink;
    diag::Recorder::Options options;
    options.runId = "20261018-120000";
    diag::Recorder recorder(sink, options);
    recorder.Begin();
    auto* core2 = new Core;
    core2->SetBackbufferProvider(&Backbuffer);
    ContextTable others;
    for (uint64_t f = 1; f <= 600; ++f)
    {
        core2->FreezeFrame();
        recorder.OnFrame(*core2, core2->CompleteFrame(), others.Get());
    }
    CHECK_EQ(sink.rewrites, 3); // frames 60, 300, 600
    CHECK(sink.text.find("run=20261018-120000 frame=601\n") != std::string::npos || sink.text.find("run=20261018-120000 frame=600\n") != std::string::npos);
    CHECK(sink.text.find("frames=0 with_boundary=0 scene_but_no_boundary=0\n") != std::string::npos);
}
} // namespace

int main()
{
    PureText();
    Run();
    return Finish("ui_census");
}
