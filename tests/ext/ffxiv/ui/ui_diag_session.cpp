// S10-A: a whole D1 session against an in-memory sink on a WARP device: Core -> Recorder -> files. Proves that arming the
// diagnostics produces every file with sane, parseable contents on the documented schedule, that the anomaly rate limit and
// the byte budgets hold, and that the numbers in the files match what was drawn.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "check.h"

#include "ext/ffxiv/ui/UiBoundaryCore.h"
#include "ext/ffxiv/ui/UiDiagRecorder.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <map>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::ui;

namespace
{
const void* g_bb = nullptr;
bool Backbuffer(const void** t, dfmt::RtShape* s)
{
    *t = g_bb;
    *s = { 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM };
    return g_bb != nullptr;
}

struct MemSink final : diag::Sink
{
    std::map<std::string, std::string> files;
    std::map<std::string, int> rewrites, appends;
    void Append(const char* f, std::string t) override
    {
        files[f] += t;
        ++appends[f];
    }
    void Rewrite(const char* f, std::string t) override
    {
        files[f] = std::move(t);
        ++rewrites[f];
    }
    uint64_t Dropped() override { return 3; }
};

uintptr_t FakeContext() { return 0xABCD00; }

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
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> bb, off;
    device->CreateTexture2D(&td, nullptr, &bb);
    device->CreateTexture2D(&td, nullptr, &off);
    ComPtr<ID3D11RenderTargetView> bbRtv, offRtv;
    device->CreateRenderTargetView(bb.Get(), nullptr, &bbRtv);
    device->CreateRenderTargetView(off.Get(), nullptr, &offRtv);
    g_bb = bb.Get();

    auto psScene = Ps(device.Get(), 0.1f), psGamma = Ps(device.Get(), 0.2f), psUi = Ps(device.Get(), 0.3f),
         psPlain = Ps(device.Get(), 0.4f), psOld = Ps(device.Get(), 0.5f);

    auto* core = new Core;
    core->SetBackbufferProvider(&Backbuffer);
    core->TagShader(psScene.Get(), 0x85E777EFu, 1512);
    core->TagShader(psGamma.Get(), 0xF6E81A1Bu, 900);
    core->TagShader(psUi.Get(), 3865947726u, 2048);
    core->TagShader(psPlain.Get(), 0x12345678u, 300);

    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT vp { 0, 0, 32, 32, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    auto draw = [&](ID3D11PixelShader* ps, bool toBackbuffer)
    {
        ctx->OMSetRenderTargets(1, toBackbuffer ? bbRtv.GetAddressOf() : offRtv.GetAddressOf(), nullptr);
        ctx->PSSetShader(ps, nullptr, 0);
        core->OnDraw(ctx.Get());
        ctx->Draw(3, 0);
    };

    MemSink sink;
    diag::Recorder::Options options;
    options.runId = "20261016-120000";
    options.earlyOutNs = 1.25;
    options.latchedContext = &FakeContext;
    diag::Recorder recorder(sink, options);
    recorder.Begin();
    CHECK(sink.files["README.txt"].find("20261016-120000") != std::string::npos);
    CHECK(sink.files["frames.txt"].find("format 1") != std::string::npos);

    ContextTable others;
    others.Note(ctx.Get(), 1); // a draw from a context the hooks did not latch
    constexpr uint64_t kFrames = 1000;
    for (uint64_t f = 1; f <= kFrames; ++f)
    {
        const bool anomalous = f > 600; // after frame 600: an untagged backbuffer draw between the gamma pass and the UI
        draw(psScene.Get(), true);
        if (f == 3)
            draw(psOld.Get(), false); // a shader created before the hook, once
        draw(psPlain.Get(), false);
        draw(psGamma.Get(), true);
        if (anomalous)
            draw(psPlain.Get(), true);
        for (int i = 0; i < 3; ++i)
            draw(psUi.Get(), true);

        core->FreezeFrame();
        draw(psPlain.Get(), true); // inside "Present"
        auto outcome = core->CompleteFrame();
        CHECK_EQ(outcome.record.frame, f);
        if (f == 5)
        {
            // normal frame: S1, plain, S5, UI x3
            CHECK_EQ(outcome.record.summary.draws, 6u);
            CHECK_EQ(outcome.record.candidates.r1, 4u);
            CHECK_EQ(outcome.record.candidates.r2, 4u);
            CHECK_EQ(outcome.record.presentDraws, 1u);
        }
        if (f == 601)
        {
            CHECK_EQ(outcome.record.candidates.r2, 4u); // the untagged backbuffer draw
            CHECK_EQ(outcome.record.candidates.r1, 5u);
            CHECK_EQ(outcome.record.candidates.r3, 4u);
            CHECK(std::string(outcome.anomaly) == "r1r2differ");
        }
        recorder.AddDiagUs(5.0);
        recorder.OnFrame(*core, std::move(outcome), others.Get());
    }
    recorder.Flush();

    // frames.txt: 600 + the anomalies (every 20 frames after 600, never on a scheduled frame) + 700, 800, 900, 1000
    const auto frames = dfmt::ParseFrames(sink.files["frames.txt"]);
    size_t first600 = 0, every100 = 0, anomalies = 0;
    for (const auto& r : frames)
    {
        if (r.reason == "first600")
            ++first600;
        else if (r.reason == "every100")
            ++every100;
        else if (r.reason == "anomaly:r1r2differ")
            ++anomalies;
        else
            CHECK(false);
    }
    CHECK_EQ(first600, 600u);
    CHECK_EQ(every100, 4u);
    CHECK_EQ(anomalies, 20u);
    CHECK_EQ(frames.size(), 624u);
    CHECK_EQ(recorder.Anomalies(), 20u);
    if (frames.size() > 5)
    {
        const auto& r = frames[4]; // frame 5
        CHECK_EQ(r.frame, 5u);
        CHECK(r.summary.sawSceneStage);
        CHECK_EQ(r.summary.uiDraws, 3u);
        CHECK_EQ(r.candidates.r3, 4u);
        CHECK_EQ(r.present.size(), 1u);
        bool ui = false;
        for (const auto& d : r.draws)
            ui |= d.ordinal == 4 && d.stage == kUiStage && d.r1 && d.r2 && d.rt == dfmt::RtKind::Backbuffer;
        CHECK(ui);
    }
    // the first logged frame after 600 is the first anomaly (601) and 3 is the only frame with the pre-hook shader
    bool sawAnomaly601 = false;
    for (const auto& r : frames)
        sawAnomaly601 |= r.frame == 601 && r.reason == "anomaly:r1r2differ" && r.candidates.r2 < r.candidates.r1;
    CHECK(sawAnomaly601);
    for (const auto& r : frames)
        if (r.frame == 3)
            CHECK_EQ(r.summary.unknownDraws, 1u);

    // cost.txt / rt.txt: rewritten at frames 60, 300, 600
    CHECK_EQ(sink.rewrites["cost.txt"], 3);
    CHECK_EQ(sink.rewrites["rt.txt"], 3);
    const std::string& cost = sink.files["cost.txt"];
    CHECK(cost.find("early_out_ns_per_call=1.25") != std::string::npos);
    CHECK(cost.find("draws_per_frame n=600") != std::string::npos);
    CHECK(cost.find("writer_jobs_dropped=3") != std::string::npos);
    CHECK(cost.find("other_context_draws immediate=1") != std::string::npos);
    CHECK(cost.find("frames 1-600 |") != std::string::npos);
    const std::string& rt = sink.files["rt.txt"];
    CHECK(rt.find("latched immediate context (lighting hooks): 0xABCD00") != std::string::npos);
    CHECK(rt.find("CreateDeferredContext calls are NOT observable") != std::string::npos);
    CHECK(rt.find("R3  BB=") != std::string::npos);
    CHECK(rt.find("context 0x") != std::string::npos);

    // shaders.csv: rewritten at frames 120 and 600, the last version parses and carries the session's numbers
    CHECK_EQ(sink.rewrites["shaders.csv"], 2);
    const auto rows = dfmt::ParseShadersCsv(sink.files["shaders.csv"]);
    CHECK_EQ(rows.size(), 5u); // scene, gamma, ui, plain, and one shader created before the hook
    bool uiRow = false, preRow = false;
    for (const auto& row : rows)
    {
        if (row.hasCrc && row.crc == 3865947726u)
            uiRow = row.ui && row.stage == kUiStage && row.draws == 1800u && row.instances == 1; // 3 x 600 frames
        if (row.preHook)
            preRow = !row.hasCrc && row.stage == kStageUnknown && row.draws == 1 && row.firstFrame == 3;
    }
    CHECK(uiRow);
    CHECK(preRow);
    CHECK(rows.size() > 1 && rows.front().draws >= rows.back().draws); // sorted by draws

    // budgets: a tiny frames budget stops the records with exactly one TRUNCATED marker
    MemSink tightSink;
    diag::Recorder::Options tight = options;
    tight.framesBudget = 3000;
    tight.probeBudget = 100;
    diag::Recorder limited(tightSink, tight);
    limited.Begin();
    auto* core2 = new Core;
    core2->SetBackbufferProvider(&Backbuffer);
    core2->TagShader(psUi.Get(), 3865947726u, 2048);
    for (uint64_t f = 1; f <= 40; ++f)
    {
        draw(psUi.Get(), true);
        core2->FreezeFrame();
        limited.OnFrame(*core2, core2->CompleteFrame(), others.Get());
    }
    limited.Flush();
    const std::string& text = tightSink.files["frames.txt"];
    size_t markers = 0;
    for (size_t at = text.find("# TRUNCATED"); at != std::string::npos; at = text.find("# TRUNCATED", at + 1))
        ++markers;
    CHECK_EQ(markers, 1u);
    CHECK(limited.FramesBudget().truncated);
    limited.AppendProbe(std::string(80, 'a'));
    limited.AppendProbe(std::string(80, 'b'));
    limited.AppendProbe(std::string(80, 'c'));
    size_t probeMarkers = 0;
    const std::string& probe = tightSink.files["probe.txt"];
    for (size_t at = probe.find("# TRUNCATED"); at != std::string::npos; at = probe.find("# TRUNCATED", at + 1))
        ++probeMarkers;
    CHECK_EQ(probeMarkers, 1u);
    CHECK(probe.find(std::string(80, 'a')) != std::string::npos);
    CHECK(probe.find(std::string(80, 'b')) == std::string::npos);
    CHECK(probe.find(std::string(80, 'c')) == std::string::npos);
}
} // namespace

int main()
{
    Run();
    return Finish("ui_diag_session");
}
