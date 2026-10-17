#include "pch.h"

#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/chara/CharaDlssApi.h"
#include "ext/ffxiv/chara/CharaEnhance.h"
#include "ext/ffxiv/chara/CharaIdentify.h"
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

ID3D11DeviceContext* g_immediate = nullptr;
RawDraw g_rawDraw = nullptr;
thread_local bool t_busy = false; // the feature's own full-screen draw is in flight (only used without a raw Draw)

PassState g_pass;
uint64_t g_lastGeometry = 0;
std::atomic<uint64_t> g_frameSerial { 0 };
uint64_t g_seenFrame = 0;
uint64_t g_lastMaintain = 0;
bool g_loggedTags = false;

std::array<std::atomic<uint32_t>, 4> g_tagged {}; // geometry, finisher, equipment display, UI

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

void Maintain(uint64_t now)
{
    const uint64_t serial = g_frameSerial.load(std::memory_order_relaxed);
    if (serial != g_seenFrame)
    {
        g_seenFrame = serial;
        ++g_stats.frames;
        g_pass.End();
        if (DlssWanted() && dlss::Active())
            dlss::OnFrameEnd();
    }

    if (now >= g_lastMaintain && now - g_lastMaintain < kMaintainMs)
        return;
    g_lastMaintain = now;

    if (g_pass.active && now >= g_lastGeometry && now - g_lastGeometry > kPassIdleMs)
        g_pass.End(); // no frame callback (no bridge): the pass ended with the last geometry draw
    identify::Maintain(now);
    if (DlssWanted() && dlss::Active())
        dlss::Maintain(now);
}

void CountDeferred(ID3D11DeviceContext* context, DrawApi api)
{
    if (api != DrawApi::Draw || !AaWanted())
        return;
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    if ((RoleOf(ps.Get()) & RoleEquipDisplay) != 0)
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
    f.passActive = g_pass.active;
    f.passWidth = g_pass.width;
    f.passHeight = g_pass.height;

    D3D11_TEXTURE2D_DESC rtvDesc {};
    if (rtv != nullptr)
    {
        f.rtv0Present = true;
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

// The finisher check reads the state the draw runs with; it is evaluated before the draw.
bool EvaluateFinisher(ID3D11DeviceContext* context, const DrawView& v)
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
    return FinisherAccepted(c);
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

    const uint64_t now = GetTickCount64();
    Maintain(now);

    const table::Snapshot& snap = table::Current(now);
    if (!snap.ready)
    {
        draw();
        return;
    }

    if (!g_loggedTags)
    {
        g_loggedTags = true;
        LOG_INFO("CharaView: shaders tagged so far: geometry {}, finisher {}, equipment display {}, UI {}", g_tagged[0].load(),
                 g_tagged[1].load(), g_tagged[2].load(), g_tagged[3].load());
    }

    DrawView view;
    view.context = context;
    view.api = api;
    Gathered gathered;
    if (!Gather(context, now, snap, view, gathered))
    {
        ++g_stats.classified;
        ++g_stats.kind[static_cast<size_t>(DrawKind::Ignore)];
        draw();
        return;
    }

    view.kind = Classify(view.facts);
    if (view.kind == DrawKind::Prepass || view.kind == DrawKind::GBuffer)
    {
        g_lastGeometry = now;
        view.passStarted = g_pass.OnGeometry(view.facts);
        if (view.passStarted)
            ++g_stats.passes;
    }
    if (view.kind == DrawKind::Finisher)
        view.finisherAccepted = EvaluateFinisher(context, view);
    identify::Observe(view);

    // Route: PreviewDLSS ? Dlss : PreviewAA ? Enhance : native.
    bool handled = false;
    if (DlssWanted())
    {
        if (dlss::Active())
            handled = dlss::Around(view, draw);
    }
    else if (AaWanted() && api == DrawApi::Draw && (view.roles & RoleEquipDisplay) != 0)
    {
        handled = enhance::Around(view, draw);
    }
    if (!handled)
        draw();

    if (view.kind == DrawKind::Finisher)
    {
        identify::OnFinisher(view.finisherAccepted);
        if (!view.finisherAccepted)
            g_pass.End();
    }
}

void OnInstalled(ID3D11DeviceContext* immediate, void* rawDraw)
{
    g_rawDraw = reinterpret_cast<RawDraw>(rawDraw);
    g_immediate = immediate;
}

void OnDetached()
{
    g_immediate = nullptr;
    g_rawDraw = nullptr;
}

void Detach()
{
    enhance::Detach();
    dlss::Detach();
    g_pass.End();
}

void OnShaderCreated(ID3D11PixelShader* shader, uint32_t crc)
{
    if (shader == nullptr)
        return;

    uint32_t roles = RoleOfCrc(crc);
    UINT ui = 0;
    UINT size = sizeof(ui);
    if (SUCCEEDED(shader->GetPrivateData(ext::hdr::FfxivHdrPreview::tag, &size, &ui)) && size == sizeof(ui) && ui != 0)
        roles |= RoleUi;
    if (roles == RoleNone || !SetRoles(shader, roles))
        return;

    if (roles & RoleGeometry)
        ++g_tagged[0];
    if (roles & RoleFinisher)
        ++g_tagged[1];
    if (roles & RoleEquipDisplay)
    {
        ++g_tagged[2];
        LOG_INFO("CharaView: equipment display shader tagged");
    }
    if (roles & RoleUi)
        ++g_tagged[3];
}

void OnCopy(ID3D11DeviceContext* context, ID3D11Resource* dest, ID3D11Resource* source, bool full)
{
    if (t_busy || t_replay || context != g_immediate || !DlssWanted() || !dlss::Active())
        return;
    dlss::OnCopy(context, dest, source, full);
}

void OnClearRenderTargetView(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const FLOAT color[4])
{
    if (t_busy || t_replay || context != g_immediate || !DlssWanted() || !dlss::Active())
        return;
    dlss::OnClearRenderTarget(context, view, color);
}

void OnQueryBegin(ID3D11DeviceContext* context, ID3D11Asynchronous* query)
{
    if (context != g_immediate || !DlssWanted() || !dlss::Active())
        return;
    dlss::OnQueryBegin(context, query);
}

void OnQueryEnd(ID3D11DeviceContext* context, ID3D11Asynchronous* query)
{
    if (context != g_immediate || !DlssWanted() || !dlss::Active())
        return;
    dlss::OnQueryEnd(context, query);
}

void OnVertexShaderCreated(ID3D11Device* device, ID3D11VertexShader* shader, const void* bytecode, SIZE_T size)
{
    if (!DlssWanted() || !dlss::Active())
        return;
    dlss::OnVertexShaderCreated(device, shader, bytecode, size);
}

void OnFrameEnd() { g_frameSerial.fetch_add(1, std::memory_order_relaxed); }

void DrawFullscreenTriangle(ID3D11DeviceContext* context)
{
    if (g_rawDraw != nullptr)
    {
        g_rawDraw(context, 3, 0);
        return;
    }
    const bool previous = t_busy;
    t_busy = true;
    context->Draw(3, 0);
    t_busy = previous;
}
} // namespace ext::ffxiv::chara
