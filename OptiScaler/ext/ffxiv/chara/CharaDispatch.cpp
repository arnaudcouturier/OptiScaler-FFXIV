#include "pch.h"

#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/chara/CharaDlssApi.h"
#include "ext/ffxiv/chara/CharaEnhance.h"
#include "ext/ffxiv/chara/CharaFlood.h"
#include "ext/ffxiv/chara/CharaIdentify.h"
#include "ext/ffxiv/chara/CharaRoute.h"
#include "ext/ffxiv/chara/CharaStats.h"
#include "ext/ffxiv/chara/CharaTable.h"
#include "ext/ffxiv/chara/CharaTags.h"
#include "ext/hdr/FfxivHdrPreview.h"

#include <array>
#include <atomic>

namespace ext::ffxiv::chara
{
using Microsoft::WRL::ComPtr;

namespace
{
using RawDraw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);

// The hook layer clears these from the unhook thread while a draw may be in flight: atomic, so the draw sees either value whole.
std::atomic<ID3D11DeviceContext*> g_immediate { nullptr };
std::atomic<RawDraw> g_rawDraw { nullptr };
std::atomic<uint32_t> g_drawThread { 0 }; // the thread that last drew on the immediate context through the dispatcher (see Detach)
thread_local bool t_busy = false; // the feature's own full-screen draw is in flight (only used without a raw Draw)

PassState g_pass;
uint64_t g_lastGeometry = 0;
std::atomic<uint64_t> g_frameSerial { 0 };
uint64_t g_seenFrame = 0;
uint64_t g_lastMaintain = 0;
bool g_loggedTags = false;


constexpr uint64_t kMaintainMs = 100;
constexpr uint64_t kPassIdleMs = 100; // a pass with no geometry draw for this long is over (no frame callback)
constexpr UINT kFallbackSrvSlots = 16;

// Everything a draw's gathered state keeps alive for the duration of the call.
struct Gathered
{
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11Resource> rtvResource;
    ComPtr<ID3D11Resource> dsvResource;
    ComPtr<ID3D11Texture2D> rtvTexture;
    ComPtr<ID3D11Texture2D> dsvTexture;
};

// The hooks only the super-resolution path needs (copy, clear, query, vertex shader events): on while the SR path may run or still
// holds objects. Preview NR alone does not need them.
bool SrHooksOn() { return dlss::SrAvailable(); }

void Maintain(uint64_t now)
{
    const uint64_t serial = g_frameSerial.load(std::memory_order_relaxed);
    if (serial != g_seenFrame)
    {
        g_seenFrame = serial;
        ++g_stats.frames;
        // The frame callback must not end the pass here: Maintain runs at an arbitrary draw, possibly between the G-buffer draws and
        // the finisher of a render in progress. The pass only learns the counter; the next prepass or G-buffer draw starts a new pass.
        g_pass.NoteFrame(serial);
        const nr::MaintenancePlan plan =
            nr::PlanMaintenance(DlssWanted(), nr::Wanted(), dlss::Active(), nr::HoldsResources(), dlss::HoldsGpuObjects());
        if (plan.sr)
            dlss::OnFrameEnd();
        if (plan.nr)
            nr::OnFrameEnd();
    }

    if (now >= g_lastMaintain && now - g_lastMaintain < kMaintainMs)
        return;
    g_lastMaintain = now;

    if (g_pass.active && now >= g_lastGeometry && now - g_lastGeometry > kPassIdleMs)
        g_pass.End(); // no frame callback (no bridge): the pass ended with the last geometry draw
    identify::Maintain(now);
    const nr::MaintenancePlan plan =
        nr::PlanMaintenance(DlssWanted(), nr::Wanted(), dlss::Active(), nr::HoldsResources(), dlss::HoldsGpuObjects());
    if (plan.sr)
        dlss::Maintain(now);
    if (plan.nr)
        nr::Maintain(now); // also with PreviewNR off while models exist: that is how they are freed
}

void CountDeferred(ID3D11DeviceContext* context, DrawApi api)
{
    if (!AaWanted() || g_stats.deferredSeen.load(std::memory_order_relaxed) != 0) // the menu only needs to know whether any was seen
        return;
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    if (CountsAsDeferredEquip(true, RoleOf(ps.Get()), api))
        ++g_stats.deferredSeen;
}

// Table entry of the first of the first `count` bound pixel-shader resources that is a preview image; -1 when none.
// `entry8` receives whether any of them is table entry 8.
int FirstPreviewSrv(ID3D11DeviceContext* context, const table::Snapshot& snap, UINT count, bool* entry8)
{
    ID3D11ShaderResourceView* views[kFallbackSrvSlots] {};
    context->PSGetShaderResources(0, count, views);
    int first = kEntryNone;
    for (UINT i = 0; i < count; ++i)
    {
        if (views[i] == nullptr)
            continue;
        ComPtr<ID3D11Resource> resource;
        views[i]->GetResource(&resource);
        const int entry = table::IndexOfPointer(snap, reinterpret_cast<uintptr_t>(resource.Get()));
        if (entry >= 0 && first == kEntryNone)
            first = entry;
        if (entry == 8 && entry8 != nullptr)
            *entry8 = true;
        views[i]->Release();
    }
    return first;
}

// Collects the facts of a draw. Returns false when the draw is certainly not a preview draw (no role on the pixel shader,
// no render target or depth texture of the table): then nothing else is read.
bool Gather(ID3D11DeviceContext* context, uint64_t now, const table::Snapshot& snap, DrawView& v, Gathered& g)
{
    context->PSGetShader(&g.ps, nullptr, nullptr);
    v.roles = RoleOf(g.ps.Get());

    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(1, &rtv, &dsv);
    g.rtv.Attach(rtv);
    g.dsv.Attach(dsv);

    int rtvPointer = kEntryNone;
    int dsvPointer = kEntryNone;
    if (rtv != nullptr)
    {
        rtv->GetResource(&g.rtvResource);
        rtvPointer = table::IndexOfPointer(snap, reinterpret_cast<uintptr_t>(g.rtvResource.Get()));
    }
    if (dsv != nullptr)
    {
        dsv->GetResource(&g.dsvResource);
        dsvPointer = table::IndexOfPointer(snap, reinterpret_cast<uintptr_t>(g.dsvResource.Get()));
    }
    if (v.roles == RoleNone && rtvPointer < 0 && dsvPointer < 0)
        return false;

    DrawFacts& f = v.facts;
    f = DrawFacts {};
    f.psGeometry = (v.roles & RoleGeometry) != 0;
    f.psFinisher = (v.roles & RoleFinisher) != 0;
    f.psEquipDisplay = (v.roles & RoleEquipDisplay) != 0;
    f.psUi = (v.roles & RoleUi) != 0;
    f.finisherTagKnown = g_stats.tagged[1].load(std::memory_order_relaxed) != 0;
    f.passActive = g_pass.active;
    f.passWidth = g_pass.width;
    f.passHeight = g_pass.height;

    D3D11_TEXTURE2D_DESC rtvDesc {};
    if (rtv != nullptr)
    {
        f.rtv0Present = true;
        D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
        rtv->GetDesc(&viewDesc);
        f.rtvFormat = static_cast<uint32_t>(viewDesc.Format);
        if (g.rtvResource != nullptr && SUCCEEDED(g.rtvResource.As(&g.rtvTexture)))
        {
            g.rtvTexture->GetDesc(&rtvDesc);
            f.rtv0Entry = table::Lookup(snap, reinterpret_cast<uintptr_t>(g.rtvTexture.Get()), rtvDesc.Width, rtvDesc.Height);
            f.samples = rtvDesc.SampleDesc.Count;
            f.targetWidth = rtvDesc.Width;
            f.targetHeight = rtvDesc.Height;
            f.targetFormat = static_cast<uint32_t>(rtvDesc.Format);
        }
    }
    if (dsv != nullptr)
    {
        f.dsvPresent = true;
        D3D11_TEXTURE2D_DESC dsvDesc {};
        if (g.dsvResource != nullptr && SUCCEEDED(g.dsvResource.As(&g.dsvTexture)))
        {
            g.dsvTexture->GetDesc(&dsvDesc);
            f.dsvEntry = table::Lookup(snap, reinterpret_cast<uintptr_t>(g.dsvTexture.Get()), dsvDesc.Width, dsvDesc.Height);
            if (!f.rtv0Present)
            {
                f.samples = dsvDesc.SampleDesc.Count;
                f.targetWidth = dsvDesc.Width;
                f.targetHeight = dsvDesc.Height;
            }
        }
    }

    // A depth-only draw into a depth candidate is a prepass if its vertex shader's constant buffer 0 is the camera block.
    const bool prepassCandidate = !f.rtv0Present && f.dsvPresent && (f.dsvEntry == 18 || f.dsvEntry == 19);
    if (prepassCandidate)
    {
        ComPtr<ID3D11Buffer> cb;
        context->VSGetConstantBuffers(0, 1, &cb);
        if (cb != nullptr)
        {
            D3D11_BUFFER_DESC bd {};
            cb->GetDesc(&bd);
            f.vsCb0Bytes = bd.ByteWidth;
        }
    }

    // Viewport 0, only where a rule reads it.
    const bool geometryCandidate = prepassCandidate || f.rtv0Entry == 8 || f.rtv0Entry == 13;
    if (geometryCandidate || f.psFinisher)
    {
        UINT count = 1;
        context->RSGetViewports(&count, &v.viewport);
        v.haveViewport = count >= 1;
        f.viewportMatchesTarget = v.haveViewport && v.viewport.Width == static_cast<float>(f.targetWidth) &&
                                  v.viewport.Height == static_cast<float>(f.targetHeight);
    }

    if (prepassCandidate)
        identify::ObserveDepthOnly(v);

    // UI draws: does the shader sample a preview image?
    if ((v.roles & (RoleUi | RoleEquipDisplay)) != 0)
        f.srvEntry = FirstPreviewSrv(context, snap, 8, nullptr);

    // A finisher created before the hooks carries no tag: recognise it by its shape (a pass is active, an output at the
    // pass size in B8G8R8A8 that samples G-buffer entry 8).
    if (!f.psFinisher && f.passActive && f.rtv0Entry >= 0 && f.rtv0Entry <= 7 && f.targetWidth == f.passWidth &&
        f.targetHeight == f.passHeight && f.targetFormat == kFormatB8G8R8A8Unorm)
    {
        FirstPreviewSrv(context, snap, kFallbackSrvSlots, &f.anySrvIsEntry8);
        if (f.anySrvIsEntry8)
        {
            UINT count = 1;
            context->RSGetViewports(&count, &v.viewport);
            v.haveViewport = count >= 1;
        }
    }

    v.pixelShader = g.ps.Get();
    v.rtv0 = g.rtv.Get();
    v.dsv = g.dsv.Get();
    v.rtvTexture = g.rtvTexture.Get();
    v.dsvTexture = g.dsvTexture.Get();
    v.now = now;
    v.table = &snap;
    v.pass = &g_pass;
    return true;
}

// The finisher check reads the state the draw runs with; it is evaluated before the draw. Fills the verdict and the values read.
void EvaluateFinisher(ID3D11DeviceContext* context, DrawView& v)
{
    FinisherCheck c;
    c.rtv0Present = v.facts.rtv0Present;
    c.passActive = v.facts.passActive;
    c.targetWidth = v.facts.targetWidth;
    c.targetHeight = v.facts.targetHeight;
    c.passWidth = v.facts.passWidth;
    c.passHeight = v.facts.passHeight;
    c.targetFormat = v.facts.targetFormat;
    if (v.haveViewport)
    {
        c.viewportX = v.viewport.TopLeftX;
        c.viewportY = v.viewport.TopLeftY;
        c.viewportWidth = v.viewport.Width;
        c.viewportHeight = v.viewport.Height;
    }

    if (v.facts.dsvPresent)
    {
        ComPtr<ID3D11DepthStencilState> state;
        UINT reference = 0;
        context->OMGetDepthStencilState(&state, &reference);
        if (state == nullptr)
            c.depthOrStencilTest = true; // the default state tests depth
        else
        {
            D3D11_DEPTH_STENCIL_DESC desc {};
            state->GetDesc(&desc);
            c.depthOrStencilTest = desc.DepthEnable || desc.StencilEnable;
        }
    }
    v.finisherCheck = c;
    v.finisherVerdict = CheckFinisher(c);
    v.finisherAccepted = v.finisherVerdict == FinisherVerdict::Accepted;
}

// Releases every GPU object of the feature. Draw thread only (or the unhook thread when it is the draw thread). After it the gate stays
// closed until the hooks are installed again.
void DetachNow()
{
    probe::Detach();
    enhance::Detach();
    nr::Detach(); // before the SR path: the NR module owns D3D12 objects that wait for the GPU
    dlss::Detach();
    g_pass.End();
    detail::g_detach.Completed();
    // Hooks installed again after the teardown was requested (a new device): the hold-off that nr::Detach set belongs to the old hooks.
    if (!detail::g_detach.Detached())
        nr::OnBridgeInit();
}
} // namespace

void AroundSlow(ID3D11DeviceContext* context, DrawApi api, DrawCall draw)
{
    if (t_busy || t_replay || context == nullptr)
    {
        draw();
        return;
    }

    // The accessor of the preview renderer's table must be verified before anything is read; if the build or the accessor
    // cannot be verified the feature stays off for this session (Resolve logged the reason once).
    if (!table::Ready())
    {
        detail::g_inert.store(true, std::memory_order_relaxed);
        draw();
        return;
    }

    if (g_immediate == nullptr)
    {
        draw();
        return;
    }
    if (context != g_immediate)
    {
        CountDeferred(context, api);
        draw();
        return;
    }

    // This is the thread that draws on the immediate context. A teardown that the unhook path asked for from another thread happens
    // here, between two draws, with nothing of the feature in flight.
    g_drawThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (detail::g_detach.Take())
    {
        DetachNow();
        draw();
        return;
    }

    const uint64_t now = GetTickCount64();
    Maintain(now);
    probe::Update(now, context); // switches the preview probe on or off (releasing what it holds); while on, pumps the image dump and the command file

    const table::Snapshot& snap = table::Current(now);
    if (!snap.ready)
    {
        draw();
        return;
    }

    if (!g_loggedTags)
    {
        g_loggedTags = true;
        LOG_INFO("CharaView: shaders tagged so far: geometry {}, finisher {}, equipment display {}, UI {}", g_stats.tagged[0].load(),
                 g_stats.tagged[1].load(), g_stats.tagged[2].load(), g_stats.tagged[3].load());
    }

    // Nothing of the preview renderer exists (the table holds no valid image) and no followed copy of it is alive: no draw can be a
    // preview draw or sample one, so the per-draw classification (about ten COM calls) and the probe scan are skipped, with the probe or the
    // flood test on or off. What it would have produced for an ignored draw is kept:
    // the counters and the one-time table line. Not kept: the diagnostics about the finisher shader drawing outside the preview
    // outputs appear once a preview image exists instead of before. No timing was measured; this only removes work.
    if (!ClassificationNeeded(snap.AnyValid(), probe::HasFollowedCopies(now)))
    {
        identify::NoteQuietDraw(snap);
        draw();
        return;
    }

    // The probe compares every draw's first resources with the preview textures before the draw is classified: the draws that put
    // a copy of the preview on screen have no role and no table render target, so Gather would drop them. It reads state only. The
    // result also tells the flood test which slots hold the preview or a copy of it.
    probe::ScanResult scan;
    if (probe::Live())
        scan = probe::OnDraw(context, api, snap, now);
    const bool floodThisDraw = scan.count != 0 && flood::On();

    DrawView view;
    view.context = context;
    view.api = api;
    Gathered gathered;
    if (!Gather(context, now, snap, view, gathered))
    {
        ++g_stats.classified;
        ++g_stats.kind[static_cast<size_t>(DrawKind::Ignore)];
        // No role and no render target of the table: this is where a UI draw that samples a copy of the preview ends up. The flood
        // test gives it the magenta image (our own replays never get here: t_replay / t_busy returned above).
        if (!floodThisDraw || !flood::SwapAndDraw(context, scan.slotMask, draw))
            draw();
        return;
    }

    view.kind = Classify(view.facts);
    if (view.facts.rtv0Entry >= 0 || view.kind == DrawKind::Prepass || view.kind == DrawKind::GBuffer)
        probe::NoteActivity(now); // the preview renderer is active: the probe scans the draws that follow
    if (view.kind == DrawKind::Prepass || view.kind == DrawKind::GBuffer)
    {
        g_lastGeometry = now;
        view.passStarted = g_pass.OnGeometry(view.facts);
        if (view.passStarted)
            ++g_stats.passes;
    }
    if (view.kind == DrawKind::Finisher)
        EvaluateFinisher(context, view);
    else if (view.kind == DrawKind::Ignore && IsOffTableFinisher(view.facts))
        identify::OnOffTableFinisher(view);
    else if (IsShapeOnlyFinisher(view.facts))
        identify::OnShapeOnlyFinisher(view);
    if (view.facts.rtv0Entry >= 0 || view.facts.srvEntry >= 0)
        identify::ObserveShader(view);
    identify::Observe(view);

    // Route: (PreviewDLSS or a usable PreviewNR) and a path that can run ? Dlss : PreviewAA ? Enhance : native. The draw API plays no
    // part (see CharaRoute.h). With PreviewDLSS on but its path unable to run, the spatial AA still takes the equipment display draw.
    bool handled = false;
    const bool dlssWanted = DlssWanted();
    const bool nrWanted = nr::Wanted();
    const bool aaWanted = AaWanted();
    if (dlssWanted)
        dlss::Probe(view);
    const bool nrUsable = nrWanted && dlss::NrCanRun();
    const Route route = ChooseRoute(dlssWanted, (dlssWanted || nrWanted) && dlss::CanRun(), aaWanted, view.roles, api, nrUsable);
    switch (route)
    {
    case Route::Dlss: handled = dlss::Around(view, draw); break;
    case Route::Enhance: handled = enhance::Around(view, draw); break;
    case Route::Native: break;
    }
    // The plumbing was offered the draw and left it alone (it never ran it): the AA is not lost for that.
    if (FallBackToAa(route, handled, dlssWanted, aaWanted, view.roles))
        handled = enhance::Around(view, draw);
    if (!handled)
    {
        // A draw no module took: the flood test gives it the magenta image in place of the preview (or a copy of it), unless it
        // renders into one of the preview renderer's own textures.
        const bool targetIsTableEntry = gathered.rtvResource != nullptr &&
                                        table::IndexOfPointer(snap, reinterpret_cast<uintptr_t>(gathered.rtvResource.Get())) >= 0;
        if (!floodThisDraw || targetIsTableEntry || !flood::SwapAndDraw(context, scan.slotMask, draw))
            draw();
    }

    if (view.kind == DrawKind::Finisher)
    {
        identify::OnFinisher(view);
        if (view.finisherAccepted && route != Route::Dlss)
            ++(dlssWanted ? g_stats.finisherNoDlss : g_stats.finisherNotWanted); // the check passed, the DLSS path did not see it
        g_pass.OnFinisher(view.finisherVerdict);
        if (view.finisherAccepted)
            probe::OnPreviewComplete(context, now); // the preview image is complete: a waiting image dump starts here
    }
}

void OnInstalled(ID3D11DeviceContext* immediate, void* rawDraw)
{
    detail::g_detach.Installed(); // a teardown still pending for the previous device stays pending (see DetachGate::Installed)
    nr::OnBridgeInit();           // preview NR was held off by nr::Detach: the new hooks may use it again (a pending teardown holds it off once more
                                  // and reopens it when it is done, see DetachNow)
    g_rawDraw = reinterpret_cast<RawDraw>(rawDraw);
    g_immediate = immediate;
}

void OnDetached()
{
    if (detail::g_detach.Pending())
        LOG_INFO("CharaView: the hooks were removed before another draw could release the preview objects; they are left to the "
                 "device's own teardown");
    g_immediate = nullptr;
    g_rawDraw = nullptr;
}

void Detach()
{
    // Only the draw thread may release the objects: on any other thread a draw can be inside them right now. A request is raised
    // instead and the next draw performs the teardown (AroundSlow).
    detail::g_detach.Request();
    if (DecideDetach(GetCurrentThreadId() == g_drawThread.load(std::memory_order_relaxed)) == DetachAction::Now)
    {
        (void)detail::g_detach.Take();
        DetachNow();
    }
}

void OnShaderCreated(ID3D11PixelShader* shader, uint32_t crc, uint32_t dxbcSize)
{
    if (shader == nullptr)
        return;
    // The UI module tagged the shader just before (the hook layer calls it first): its tag is read here (see TagCreatedShader).
    const TagOutcome tagged = TagCreatedShader(shader, crc, dxbcSize, ext::hdr::FfxivHdrPreview::tag);
    if (!tagged.written)
        return;
    const uint32_t roles = tagged.roles;

    // The first shader of each role is named in the log: shaders are created whenever the game first needs them, which can be
    // after the first preview draw, so the one-time "tagged so far" summary alone can read 0 for a role that is tagged later.
    if ((roles & RoleGeometry) && g_stats.tagged[0]++ == 0)
        LOG_INFO("CharaView: first geometry shader tagged (CRC {:08X}, {} bytes)", crc, dxbcSize);
    if ((roles & RoleFinisher) && g_stats.tagged[1]++ == 0)
        LOG_INFO("CharaView: finisher shader tagged (CRC {:08X}, {} bytes)", crc, dxbcSize);
    if (roles & RoleEquipDisplay)
    {
        ++g_stats.tagged[2];
        LOG_INFO("CharaView: equipment display shader tagged (CRC {:08X}, {} bytes)", crc, dxbcSize);
    }
    if (roles & RoleUi)
        ++g_stats.tagged[3];
}

void OnCopy(ID3D11DeviceContext* context, ID3D11Resource* dest, ID3D11Resource* source, bool full)
{
    if (t_busy || t_replay || context != g_immediate)
        return;
    if (probe::Live())
        probe::OnCopy(context, dest, source); // before the DLSS gate: the probe does not depend on DLSS
    if (!SrHooksOn())
        return;
    dlss::OnCopy(context, dest, source, full);
}

void OnCopyRegion(ID3D11DeviceContext* context, ID3D11Resource* dest, UINT destSub, UINT x, UINT y, UINT z, ID3D11Resource* source,
                  UINT sourceSub, const D3D11_BOX* box)
{
    if (t_busy || t_replay || context != g_immediate)
        return;
    if (probe::Live())
        probe::OnCopyRegion(context, dest, destSub, x, y, z, source, sourceSub, box);
    if (!SrHooksOn())
        return;
    const bool full = !destSub && !sourceSub && !x && !y && !z && !box; // the whole of subresource 0
    dlss::OnCopy(context, dest, source, full);
}

void OnResolve(ID3D11DeviceContext* context, ID3D11Resource* dest, UINT destSub, ID3D11Resource* source, UINT sourceSub,
               DXGI_FORMAT format)
{
    if (t_busy || t_replay || context != g_immediate || !probe::Live())
        return;
    probe::OnResolve(context, dest, destSub, source, sourceSub, format);
}

void OnClearRenderTargetView(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const FLOAT color[4])
{
    if (t_busy || t_replay || context != g_immediate || !SrHooksOn())
        return;
    dlss::OnClearRenderTarget(context, view, color);
}

void OnQueryBegin(ID3D11DeviceContext* context, ID3D11Asynchronous* query)
{
    if (context != g_immediate || !SrHooksOn())
        return;
    dlss::OnQueryBegin(context, query);
}

void OnQueryEnd(ID3D11DeviceContext* context, ID3D11Asynchronous* query)
{
    if (context != g_immediate || !SrHooksOn())
        return;
    dlss::OnQueryEnd(context, query);
}

void OnVertexShaderCreated(ID3D11Device* device, ID3D11VertexShader* shader, const void* bytecode, SIZE_T size)
{
    if (!SrHooksOn())
        return;
    dlss::OnVertexShaderCreated(device, shader, bytecode, size);
}

void OnFrameEnd() { g_frameSerial.fetch_add(1, std::memory_order_relaxed); }

void DrawFullscreenTriangle(ID3D11DeviceContext* context)
{
    if (const RawDraw raw = g_rawDraw.load(); raw != nullptr)
    {
        raw(context, 3, 0);
        return;
    }
    const bool previous = t_busy;
    t_busy = true;
    context->Draw(3, 0);
    t_busy = previous;
}
} // namespace ext::ffxiv::chara
