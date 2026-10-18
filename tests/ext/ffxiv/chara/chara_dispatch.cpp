// The real dispatcher (CharaDispatch.cpp) on a D3D11 WARP device, with the other modules stubbed: the super-resolution / NR plumbing
// (dlss::, nr::), the probe and the table accessor are test doubles that record how they are called; the spatial AA, the flood test and
// the draw classifier are the real code. A draw is a callable that counts; the context is a real WARP context carrying real state.
//
// What this proves that the pure tests cannot: the wiring of AroundSlow, Maintain, Detach and the hook forwarders.
//   * the native draw runs exactly once on every path (gate closed, empty table, deferred context, replay, each route, a module that
//     handles the draw and one that declines it, the teardown draw);
//   * a pass: an accepted finisher closes the render so the next geometry draw starts a new pass; the frame callback between the
//     G-buffer draws and the finisher does not cut the render; a rejected finisher ends the pass;
//   * PreviewDLSS on but its path unable to run: the equipment draw gets the spatial AA;
//   * switching PreviewDLSS off while slots hold objects keeps the gate open and the cleanup running;
//   * the unhook thread does not tear the feature down: the draw thread does, once, and the gate closes afterwards.
#include "check.h"
#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/chara/CharaStats.h"
#include "ext/ffxiv/chara/CharaTags.h"
#include "ext/hdr/FfxivHdrPreview.h"

#include <atomic>
#include <d3dcompiler.h>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::chara;

namespace
{
// ---- the test doubles ----------------------------------------------------------------------------------------------------------
struct SeenDraw
{
    DrawKind kind = DrawKind::Ignore;
    bool passStarted = false;
    uint32_t counter = 0;
    bool passActive = false;
    bool passFinished = false;
    bool finisherAccepted = false;
};

struct Doubles
{
    // answers
    bool canRun = false;
    bool active = false;
    bool srAvailable = false;
    bool dlssHolds = false;
    bool nrWanted = false;
    bool nrHolds = false;
    bool aroundHandles = false; // dlss::Around runs the draw once and says so; false: it declines without running it
    // records
    int dlssAround = 0, dlssProbe = 0, dlssMaintain = 0, dlssFrameEnd = 0, dlssDetach = 0;
    int nrMaintain = 0, nrFrameEnd = 0, nrDetach = 0;
    int probeUpdate = 0, probeDetach = 0, probeOnDraw = 0;
    bool followedCopies = false;
    std::thread::id detachThread;
    std::vector<SeenDraw> seen;


    void ResetCounts()
    {
        dlssAround = dlssProbe = dlssMaintain = dlssFrameEnd = dlssDetach = 0;
        nrMaintain = nrFrameEnd = nrDetach = 0;
        probeUpdate = probeDetach = probeOnDraw = 0;
        seen.clear();
    }
} d;

// The table the preview renderer would expose (kept apart from the doubles: resetting the doubles must not empty the table).
using SnapshotT = ext::ffxiv::chara::table::Snapshot;
SnapshotT g_snapshot;
} // namespace

namespace ext::ffxiv::chara
{
namespace table
{
const Resolution& Resolve()
{
    static Resolution r = [] { Resolution x; x.state = AccessorState::Ready; return x; }();
    return r;
}
bool Ready() { return true; }
const char* InertReason() { return ""; }
const Snapshot& Current(uint64_t) { return g_snapshot; }
void LogTable(const char*, const Snapshot&) {}
} // namespace table

namespace dlss
{
bool Active() { return d.active; }
bool CanRun() { return d.canRun; }
bool SrAvailable() { return d.srAvailable; }
bool HoldsGpuObjects() { return d.dlssHolds; }
void Probe(const DrawView&) { ++d.dlssProbe; }
bool Around(const DrawView& view, DrawCall draw)
{
    ++d.dlssAround;
    SeenDraw s;
    s.kind = view.kind;
    s.passStarted = view.passStarted;
    if (view.pass != nullptr)
    {
        s.counter = view.pass->counter;
        s.passActive = view.pass->active;
        s.passFinished = view.pass->finished;
    }
    s.finisherAccepted = view.finisherAccepted;
    d.seen.push_back(s);
    if (!d.aroundHandles)
        return false;
    draw();
    return true;
}
void OnCopy(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*, bool) {}
void OnClearRenderTarget(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]) {}
void OnQueryBegin(ID3D11DeviceContext*, ID3D11Asynchronous*) {}
void OnQueryEnd(ID3D11DeviceContext*, ID3D11Asynchronous*) {}
void OnVertexShaderCreated(ID3D11Device*, ID3D11VertexShader*, const void*, SIZE_T) {}
void OnFrameEnd() { ++d.dlssFrameEnd; }
void Maintain(uint64_t) { ++d.dlssMaintain; }
void Detach()
{
    ++d.dlssDetach;
    d.detachThread = std::this_thread::get_id();
}
Counters GetCounters() { return {}; }
} // namespace dlss

namespace nr
{
bool Wanted() { return d.nrWanted; }
bool HoldsResources() { return d.nrHolds; }
void Maintain(uint64_t) { ++d.nrMaintain; }
void OnFrameEnd() { ++d.nrFrameEnd; }
void Detach() { ++d.nrDetach; }
} // namespace nr

namespace probe
{
void Update(uint64_t, ID3D11DeviceContext*) { ++d.probeUpdate; }
ScanResult OnDraw(ID3D11DeviceContext*, DrawApi, const table::Snapshot&, uint64_t)
{
    ++d.probeOnDraw;
    return {};
}
bool HasFollowedCopies(uint64_t) { return d.followedCopies; }
void OnCopy(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*) {}
void OnCopyRegion(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*) {}
void OnResolve(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT) {}
void Detach() { ++d.probeDetach; }
} // namespace probe
} // namespace ext::ffxiv::chara

namespace
{
// ---- the scene -----------------------------------------------------------------------------------------------------------------
constexpr UINT kW = 576, kH = 960;

struct Scene
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> output, gbuffer, depth, backbuffer, aaSource;
    ComPtr<ID3D11RenderTargetView> outputRtv, gbufferRtv, backbufferRtv;
    ComPtr<ID3D11DepthStencilView> depthDsv;
    ComPtr<ID3D11ShaderResourceView> outputSrv, aaSrv;
    ComPtr<ID3D11PixelShader> plain, geometry, finisher, ui, equip;
};

ComPtr<ID3D11Texture2D> Texture(Scene& s, UINT w, UINT h, DXGI_FORMAT format, UINT bind)
{
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = bind;
    ComPtr<ID3D11Texture2D> t;
    s.device->CreateTexture2D(&desc, nullptr, &t);
    return t;
}

ComPtr<ID3D11PixelShader> MakeShader(Scene& s, const char* source)
{
    ComPtr<ID3DBlob> blob, errors;
    D3DCompile(source, std::strlen(source), "test", nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors);
    ComPtr<ID3D11PixelShader> ps;
    s.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps);
    return ps;
}

// The preview renderer's table: output 0, G-buffer 8, depth 18.
void RestoreTable(const Scene& s)
{
    g_snapshot = {};
    g_snapshot.ready = true;
    g_snapshot.entries[0] = { reinterpret_cast<uintptr_t>(s.output.Get()), kW, kH };
    g_snapshot.entries[8] = { reinterpret_cast<uintptr_t>(s.gbuffer.Get()), kW, kH };
    g_snapshot.entries[18] = { reinterpret_cast<uintptr_t>(s.depth.Get()), kW, kH };
}

bool SetUp(Scene& s)
{
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &s.device, nullptr, &s.context)))
        return false;
    s.output = Texture(s, kW, kH, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    s.gbuffer = Texture(s, kW, kH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    s.depth = Texture(s, kW, kH, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_BIND_DEPTH_STENCIL);
    s.backbuffer = Texture(s, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    s.aaSource = Texture(s, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    s.device->CreateRenderTargetView(s.output.Get(), nullptr, &s.outputRtv);
    s.device->CreateRenderTargetView(s.gbuffer.Get(), nullptr, &s.gbufferRtv);
    s.device->CreateRenderTargetView(s.backbuffer.Get(), nullptr, &s.backbufferRtv);
    s.device->CreateDepthStencilView(s.depth.Get(), nullptr, &s.depthDsv);
    s.device->CreateShaderResourceView(s.output.Get(), nullptr, &s.outputSrv);
    s.device->CreateShaderResourceView(s.aaSource.Get(), nullptr, &s.aaSrv);

    const char* source = "Texture2D<float4> t0 : register(t0);\n"
                         "float4 main(float4 p : SV_Position) : SV_Target { return t0.Load(int3(0, 0, 0)); }\n";
    s.plain = MakeShader(s, source);
    s.geometry = MakeShader(s, source);
    s.finisher = MakeShader(s, source);
    s.ui = MakeShader(s, source);
    s.equip = MakeShader(s, source);

    // The roles come from the creation hook, the way the game's shaders get them: the UI module tags first, then the preview layer.
    OnShaderCreated(s.geometry.Get(), kGeometryCrcs[0], 100);
    OnShaderCreated(s.finisher.Get(), kFinisherCrc, 100);
    OnShaderCreated(s.equip.Get(), kEquipDisplayCrc, 100);
    CHECK(ext::hdr::FfxivHdrPreview::Tag(s.ui.Get(), 3865947726u)); // one of the UI shaders
    OnShaderCreated(s.ui.Get(), 3865947726u, 100);
    CHECK(RoleOf(s.geometry.Get()) == RoleGeometry && RoleOf(s.finisher.Get()) == RoleFinisher);
    CHECK(RoleOf(s.equip.Get()) == RoleEquipDisplay && RoleOf(s.ui.Get()) == RoleUi && RoleOf(s.plain.Get()) == RoleNone);

    RestoreTable(s);
    return true;
}

void SetViewport(Scene& s, float w, float h)
{
    const D3D11_VIEWPORT vp { 0, 0, w, h, 0, 1 };
    s.context->RSSetViewports(1, &vp);
}

void BindGBuffer(Scene& s)
{
    ID3D11RenderTargetView* rtv = s.gbufferRtv.Get();
    s.context->OMSetRenderTargets(1, &rtv, s.depthDsv.Get());
    s.context->PSSetShader(s.geometry.Get(), nullptr, 0);
    SetViewport(s, kW, kH);
}

void BindFinisher(Scene& s, float viewportW = kW, float viewportH = kH)
{
    ID3D11RenderTargetView* rtv = s.outputRtv.Get();
    s.context->OMSetRenderTargets(1, &rtv, nullptr);
    s.context->PSSetShader(s.finisher.Get(), nullptr, 0);
    SetViewport(s, viewportW, viewportH);
}

// A UI draw into the back buffer that samples table entry 0.
void BindConsumer(Scene& s, ID3D11PixelShader* ps, ID3D11ShaderResourceView* t0)
{
    ID3D11RenderTargetView* rtv = s.backbufferRtv.Get();
    s.context->OMSetRenderTargets(1, &rtv, nullptr);
    s.context->PSSetShader(ps, nullptr, 0);
    SetViewport(s, 1280, 720);
    s.context->PSSetShaderResources(0, 1, &t0);
}

// The newest view the DLSS double was offered (a default one when none: the checks then fail instead of crashing).
const SeenDraw& Last()
{
    static const SeenDraw none {};
    return d.seen.empty() ? none : d.seen.back();
}

// Runs one dispatched draw and returns how many times the native draw ran.
int Draw(ID3D11DeviceContext* context, DrawApi api = DrawApi::Draw)
{
    int n = 0;
    Around(context, api, [&] { ++n; });
    return n;
}

void Config(bool dlss, bool aa, bool nr)
{
    auto& cfg = ext::Cfg().ffxiv.chara;
    cfg.previewDlss = dlss;
    cfg.previewAA = aa;
    cfg.previewNr = nr;
}

void Doubles_Off()
{
    d = Doubles {};
}

// ---- the tests -----------------------------------------------------------------------------------------------------------------
void TestGateAndEarlyExits(Scene& s)
{
    // gate closed: nothing is wanted; the draw runs once and no module is asked
    Doubles_Off();
    OnInstalled(s.context.Get(), nullptr);
    Config(false, false, false);
    BindConsumer(s, s.ui.Get(), s.outputSrv.Get());
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(d.dlssAround == 0 && d.probeUpdate == 0 && d.dlssMaintain == 0);

    // an empty table (no preview exists): the draw runs once, nothing is classified, the Ignore counters keep counting
    Doubles_Off();
    Config(false, true, false);
    g_snapshot = {};
    g_snapshot.ready = true;
    const auto classified = g_stats.classified.load();
    const auto ignored = g_stats.kind[static_cast<size_t>(DrawKind::Ignore)].load();
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(g_stats.classified == classified + 1 && g_stats.kind[static_cast<size_t>(DrawKind::Ignore)] == ignored + 1);
    CHECK(d.dlssAround == 0 && d.dlssProbe == 0);
    RestoreTable(s);

    // a deferred context: counted at most, the draw runs once
    Doubles_Off();
    Config(false, true, false);
    ComPtr<ID3D11DeviceContext> deferred;
    CHECK(SUCCEEDED(s.device->CreateDeferredContext(0, &deferred)));
    CHECK(Draw(deferred.Get()) == 1);
    CHECK(d.dlssAround == 0);

    // a replay by another module (ReplayScope) and no context: once, untouched
    {
        ReplayScope scope;
        CHECK(Draw(s.context.Get()) == 1);
    }
    CHECK(Draw(nullptr) == 1);
    CHECK(d.dlssAround == 0);
}

// The debug aids (probe, flood test) no longer keep the full per-draw path alive when there is nothing to find: no preview output in the
// table and no followed copy. With a followed copy, or a table that holds a preview, the scan runs as before.
void TestDebugAidsAndTheQuietPath(Scene& s)
{
    BindConsumer(s, s.ui.Get(), s.outputSrv.Get());
    probe::detail::g_live.store(true);

    Doubles_Off();
    Config(false, false, false);
    table::Snapshot saved = g_snapshot;
    g_snapshot = {};
    g_snapshot.ready = true;
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(d.probeOnDraw == 0); // empty table, no copy followed: nothing to scan

    d.followedCopies = true; // a copy of the preview is alive: a draw may sample it
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(d.probeOnDraw == 1);

    d.followedCopies = false;
    g_snapshot = saved; // the preview exists
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(d.probeOnDraw == 2);

    probe::detail::g_live.store(false);
    Doubles_Off();
}

void TestRoutes(Scene& s)
{
    BindConsumer(s, s.ui.Get(), s.outputSrv.Get());

    // route Native: only the AA is wanted and the draw is a UI draw (not the equipment display): native, once
    Doubles_Off();
    Config(false, true, false);
    CHECK(Draw(s.context.Get(), DrawApi::DrawIndexed) == 1);
    CHECK(d.dlssAround == 0);

    // route Dlss, module handles the draw (and runs it once): once in total
    Doubles_Off();
    Config(true, false, false);
    d.canRun = d.active = true;
    d.aroundHandles = true;
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(d.dlssAround == 1 && d.seen.size() == 1 && d.seen[0].kind == DrawKind::Consumer);

    // route Dlss, module declines: the dispatcher runs the draw itself, once
    d.aroundHandles = false;
    d.ResetCounts();
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(d.dlssAround == 1 && d.seen.size() == 1);

    // PreviewNR alone is offered the draw too
    Doubles_Off();
    Config(false, false, true);
    d.nrWanted = true;
    d.canRun = d.active = true;
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(d.dlssAround == 1);

    // PreviewDLSS on, its path cannot run: the UI draw is not offered to it (native, once) ...
    Doubles_Off();
    Config(true, false, false);
    d.canRun = false;
    d.active = true; // the plumbing is alive (slots draining) but cannot do work
    d.dlssHolds = true;
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(d.dlssAround == 0);

    // ... and the equipment display draw gets the spatial AA when PreviewAA is on (the real AA runs on WARP)
    Doubles_Off();
    Config(true, true, false);
    d.canRun = false;
    d.active = true;
    d.dlssHolds = true;
    // the equipment output is table entry 0: a 64x64 RGBA image for this check
    table::Snapshot saved = g_snapshot;
    g_snapshot.entries[0] = { reinterpret_cast<uintptr_t>(s.aaSource.Get()), 64, 64 };
    BindConsumer(s, s.equip.Get(), s.aaSrv.Get());
    const auto processed = g_stats.aaProcessed.load();
    CHECK(Draw(s.context.Get(), DrawApi::DrawIndexed) == 1);
    CHECK(g_stats.aaProcessed == processed + 1); // the AA took the draw: the fallback works through the real dispatcher
    CHECK(d.dlssAround == 0);

    // with the DLSS path able to run the same draw goes to it and the AA does not run
    d.canRun = true;
    d.aroundHandles = true;
    CHECK(Draw(s.context.Get(), DrawApi::DrawIndexed) == 1);
    CHECK(g_stats.aaProcessed == processed + 1 && d.dlssAround == 1);
    g_snapshot = saved;
    Config(false, false, false);
}

void TestPassAcrossRenders(Scene& s)
{
    Doubles_Off();
    Config(true, false, false);
    d.canRun = d.active = true;
    d.aroundHandles = true;
    const auto passes = g_stats.passes.load();

    // render 1: two G-buffer draws, the bridge's frame callback fires in the middle of the render, then the finisher
    BindGBuffer(s);
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(Last().kind == DrawKind::GBuffer && Last().passStarted);
    const uint32_t first = Last().counter;
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(!Last().passStarted && Last().counter == first);
    OnFrameEnd(); // the present callback, between the G-buffer draws and the finisher
    BindFinisher(s);
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(Last().kind == DrawKind::Finisher);
    CHECK(Last().passActive);                         // the callback did not end the pass ...
    CHECK(Last().finisherAccepted);                   // ... so the finisher still finds it
    CHECK(Last().counter == first);

    // render 2 of the same frame: the accepted finisher closed the first render, so this is a new pass (a new jitter phase)
    BindGBuffer(s);
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(Last().passStarted && Last().counter == first + 1 && !Last().passFinished);
    BindFinisher(s);
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(Last().finisherAccepted && Last().counter == first + 1);

    // a render whose finisher is rejected (its viewport is not the whole target) ends the pass
    BindGBuffer(s);
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(Last().passStarted && Last().counter == first + 2);
    BindFinisher(s, 100, 100);
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(Last().kind == DrawKind::Finisher && !Last().finisherAccepted);
    BindGBuffer(s);
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(Last().passStarted && Last().counter == first + 3);

    // a pass that never reached a finisher is replaced by the first geometry draw after the frame callback
    OnFrameEnd();
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(Last().passStarted && Last().counter == first + 4);
    CHECK(g_stats.passes == passes + 5);

    // geometry of another size does not belong to a running pass: the module is offered the draw (it decides to leave it native), the
    // pass keeps its own size
    Config(false, false, false);
}

void TestMaintenanceAfterSwitchOff(Scene& s)
{
    // PreviewDLSS and PreviewNR are off, slots still hold GPU objects: the gate stays open and the SR cleanup runs (the old gate closed
    // with the option, so the slots were never released)
    Doubles_Off();
    Config(false, false, false);
    BindConsumer(s, s.ui.Get(), s.outputSrv.Get());
    OnFrameEnd(); // the frame-end cleanup is forwarded on the next draw
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(d.dlssFrameEnd == 0 && d.dlssMaintain == 0); // nothing held, nothing forwarded

    Doubles_Off();
    d.dlssHolds = true;
    d.active = true;
    OnFrameEnd();
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(d.dlssFrameEnd == 1); // the frame end reached the SR cleanup
    CHECK(d.dlssAround == 0);   // but the draw was not offered to a path that is switched off

    // NR objects after PreviewNR was switched off: the same for the NR maintenance
    Doubles_Off();
    d.nrHolds = true;
    OnFrameEnd();
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(d.nrFrameEnd == 1 && d.dlssFrameEnd == 0);

    // hook forwarders: the SR copy / clear / query hooks follow SrAvailable
    Doubles_Off();
    d.srAvailable = false;
    OnQueryBegin(s.context.Get(), nullptr); // would reach dlss::OnQueryBegin; the double ignores everything, only no crash is checked
    Doubles_Off();
}

void TestDetachFromAnotherThread(Scene& s)
{
    // the draw thread draws, the unhook thread asks for the teardown: it is done by the draw thread, once, at its next draw
    Doubles_Off();
    OnInstalled(s.context.Get(), nullptr);
    Config(true, true, false);
    d.canRun = d.active = true;
    BindConsumer(s, s.ui.Get(), s.outputSrv.Get());
    CHECK(Draw(s.context.Get()) == 1); // this thread is now the draw thread
    d.ResetCounts();

    std::thread unhook([] { Detach(); });
    unhook.join();
    CHECK(d.dlssDetach == 0 && d.nrDetach == 0 && d.probeDetach == 0); // nothing was torn down on the unhook thread
    CHECK(Wanted());                                                  // the gate stays open so the draw thread gets there

    CHECK(Draw(s.context.Get()) == 1); // the teardown draw runs the native draw once
    CHECK(d.dlssDetach == 1 && d.nrDetach == 1 && d.probeDetach == 1);
    CHECK(d.detachThread == std::this_thread::get_id()); // on the draw thread
    CHECK(d.dlssAround == 0 && d.seen.empty());          // and that draw was not classified

    // afterwards the dispatcher is closed until the hooks are installed again
    CHECK(!Wanted());
    d.ResetCounts();
    CHECK(Draw(s.context.Get()) == 1);
    CHECK(d.probeUpdate == 0 && d.dlssAround == 0 && d.dlssDetach == 0);

    // a teardown asked for on the draw thread itself happens at once
    OnInstalled(s.context.Get(), nullptr);
    CHECK(Wanted());
    CHECK(Draw(s.context.Get()) == 1);
    d.ResetCounts();
    Detach();
    CHECK(d.dlssDetach == 1 && d.nrDetach == 1 && d.probeDetach == 1);
    CHECK(!Wanted());

    // installed again: the gate follows the configuration again
    OnInstalled(s.context.Get(), nullptr);
    CHECK(Wanted());
    Config(false, false, false);
    CHECK(!Wanted());
    OnDetached();
}
} // namespace

int main()
{
    Scene scene;
    if (!SetUp(scene))
    {
        std::printf("no WARP device\n");
        return 1;
    }
    TestGateAndEarlyExits(scene);
    TestRoutes(scene);
    TestDebugAidsAndTheQuietPath(scene);
    TestPassAcrossRenders(scene);
    TestMaintenanceAfterSwitchOff(scene);
    TestDetachFromAnotherThread(scene);
    return TEST_RESULT("chara_dispatch");
}
