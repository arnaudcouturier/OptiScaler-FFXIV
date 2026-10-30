#include "pch.h"

#include "ext/ffxiv/plates/PlateNative.h"

#include "ext/ffxiv/plates/PlateClassify.h"
#include "ext/ffxiv/plates/PlateShaderKind.h"
#include "ext/ffxiv/ui/PlateLayoutTag.h"
#include "ext/hudless/PlateEngine.h"
#include "ext/hudless/PlateSource.h"

#include <d3d11.h>

#include <wrl/client.h>

#include <atomic>
#include <cstring>

// Game side of the native nameplates, first build: a draw-level classifier with no game memory.
//
// A plate draw is recognised from the live D3D11 state of the immediate context and nothing else: one 8-bit SDR colour
// target, standard alpha blending, a depth test with GREATER_EQUAL that writes nothing and uses no stencil, one of the
// four nameplate pixel shaders and a nameplate input layout. Every condition is checked at the draw itself, before the
// frame's first ask to the engine, so nothing that can fail runs after a frame may be hidden.
//
// The engine answers once per frame. Its published record serial changes exactly at each present seam (the same thread
// runs the draws and the seam), so the frame's answer is remembered with that serial and no seam of our own is needed.
//
// The redirect is one routine for a shadow frame (the image is built, the game draws its own plates too) and a hidden
// frame (the image is built, the game's own plates are skipped): the same call, and the caller decides the native draw.
// A refusal before the draw returns "native"; nothing falls from a hidden frame into a shadow-only path.

namespace ext::ffxiv::plates
{
namespace
{
using Microsoft::WRL::ComPtr;

using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);

// The pinned values PlateClassify.h carries instead of the D3D headers.
static_assert(kDepthGreaterEqual == static_cast<uint8_t>(D3D11_COMPARISON_GREATER_EQUAL), "depth function value");
static_assert(IsSdrFormat(DXGI_FORMAT_B8G8R8A8_UNORM), "B8G8R8A8_UNORM");
static_assert(IsSdrFormat(DXGI_FORMAT_R8G8B8A8_UNORM), "R8G8B8A8_UNORM");

constexpr UINT kMaxTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;

struct Counts
{
    std::atomic<uint64_t> drawsSeen { 0 };
    std::atomic<uint64_t> drawsMatched { 0 };
    std::atomic<uint64_t> reserveAsks { 0 };
    std::atomic<uint64_t> framesDeclined { 0 };
    std::atomic<uint64_t> drawsShadowed { 0 };
    std::atomic<uint64_t> drawsHidden { 0 };
    std::atomic<uint64_t> drawsFallback { 0 };
    std::atomic<uint64_t> framesFinished { 0 };
};
Counts g_counts;

// The frame's latched answer. Render thread only: the draws and the frame finish run on the same thread (the engine
// latches the same assumption and refuses to hide without it).
struct Latch
{
    bool valid = false; // an answer was taken for `serial`
    bool cleared = false; // the slot was cleared for this frame (the first redirected draw clears it once)
    uint64_t serial = 0; // the engine's published record serial when the answer was taken
    ext::hudless::plates::FrameReservation reservation;
};
thread_local Latch g_latch;

void Bump(std::atomic<uint64_t>& counter) { counter.fetch_add(1, std::memory_order_relaxed); }

// One draw's state, read from the immediate context. Everything here is a CPU-side Get* call: nothing is bound, drawn,
// mapped or copied. A Get* call that fails leaves its field at "not a plate draw", which fails closed.
struct DrawInfo
{
    DrawState state;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    unsigned width = 0;
    unsigned height = 0;
};

void ReadDrawState(ID3D11DeviceContext* context, DrawInfo& info)
{
    DrawState& s = info.state;
    s.immediateContext = true; // the caller tested the context identity

    ID3D11RenderTargetView* targets[kMaxTargets] = {};
    ID3D11DepthStencilView* depth = nullptr;
    context->OMGetRenderTargets(kMaxTargets, targets, &depth);
    // The UAV range starts at the first slot the render targets do not use (D3D11 refuses to return both in one call
    // unless the ranges do not overlap), which for a plate draw means "no UAV bound at all".
    ID3D11UnorderedAccessView* uavs[kMaxTargets] = {};
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, kMaxTargets, uavs);

    ComPtr<ID3D11RenderTargetView> target;
    unsigned targetCount = 0;
    for (UINT i = 0; i < kMaxTargets; ++i)
    {
        if (targets[i] != nullptr)
        {
            ++targetCount;
            if (targetCount == 1)
                target.Attach(targets[i]);
            else
                targets[i]->Release();
        }
    }
    bool uavBound = false;
    for (UINT i = 0; i < kMaxTargets; ++i)
    {
        if (uavs[i] != nullptr)
        {
            uavBound = true;
            uavs[i]->Release();
        }
    }
    ComPtr<ID3D11DepthStencilView> depthView;
    depthView.Attach(depth);

    s.singleTarget = targetCount == 1;
    s.noUav = !uavBound;
    if (!s.singleTarget)
        return; // nothing below can matter for a draw with another target count

    ComPtr<ID3D11Resource> colour;
    target->GetResource(&colour);
    D3D11_TEXTURE2D_DESC colourDesc {};
    if (colour)
    {
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(colour.As(&texture)) && texture)
        {
            texture->GetDesc(&colourDesc);
            info.format = colourDesc.Format;
            info.width = colourDesc.Width;
            info.height = colourDesc.Height;
            s.sdrTarget = IsSdrFormat(static_cast<unsigned>(colourDesc.Format));
            s.singleSample = colourDesc.SampleDesc.Count == 1;
        }
    }
    if (!s.sdrTarget || !s.singleSample || info.width == 0 || info.height == 0)
        return;

    if (depthView)
    {
        ComPtr<ID3D11Resource> depthResource;
        depthView->GetResource(&depthResource);
        ComPtr<ID3D11Texture2D> depthTexture;
        D3D11_TEXTURE2D_DESC depthDesc {};
        if (depthResource && SUCCEEDED(depthResource.As(&depthTexture)) && depthTexture)
        {
            depthTexture->GetDesc(&depthDesc);
            s.depthView = true;
            s.depthSameSize = depthDesc.Width == colourDesc.Width && depthDesc.Height == colourDesc.Height;
        }
    }

    ComPtr<ID3D11DepthStencilState> depthState;
    UINT stencilRef = 0;
    context->OMGetDepthStencilState(&depthState, &stencilRef);
    if (depthState)
    {
        D3D11_DEPTH_STENCIL_DESC desc {};
        depthState->GetDesc(&desc);
        s.depthTested = desc.DepthEnable != FALSE;
        s.depthWriteOff = desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO;
        s.depthGreaterEqual = static_cast<uint8_t>(desc.DepthFunc) == kDepthGreaterEqual;
        s.stencilOff = desc.StencilEnable == FALSE;
    }

    // The standard alpha blend the game's plates use. No blend state bound is D3D11's default (blending off), which is
    // not a plate draw.
    ComPtr<ID3D11BlendState> blendState;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0;
    context->OMGetBlendState(&blendState, blendFactor, &sampleMask);
    if (blendState)
    {
        D3D11_BLEND_DESC desc {};
        blendState->GetDesc(&desc);
        const D3D11_RENDER_TARGET_BLEND_DESC& rt = desc.RenderTarget[0];
        s.standardBlend = !desc.AlphaToCoverageEnable && !desc.IndependentBlendEnable && rt.BlendEnable &&
                          rt.SrcBlend == D3D11_BLEND_SRC_ALPHA && rt.DestBlend == D3D11_BLEND_INV_SRC_ALPHA &&
                          rt.BlendOp == D3D11_BLEND_OP_ADD && rt.SrcBlendAlpha == D3D11_BLEND_ONE &&
                          rt.DestBlendAlpha == D3D11_BLEND_INV_SRC_ALPHA && rt.BlendOpAlpha == D3D11_BLEND_OP_ADD &&
                          rt.RenderTargetWriteMask == D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    if (!s.standardBlend)
        return; // the two shader reads below only matter once the target and the blend matched

    ComPtr<ID3D11PixelShader> pixel;
    context->PSGetShader(&pixel, nullptr, nullptr);
    s.nameplateShader = KindOf(pixel.Get()) != 0;
    if (!s.nameplateShader)
        return;

    // The layout tag is written when the game creates an input layout for one of its three nameplate vertex shaders, so
    // a nameplate pixel shader on another layout is not a nameplate draw.
    ComPtr<ID3D11InputLayout> layout;
    context->IAGetInputLayout(&layout);
    if (layout)
    {
        ext::ffxiv::ui::plate::Layout tag {};
        UINT size = sizeof(tag);
        if (SUCCEEDED(layout->GetPrivateData(ext::ffxiv::ui::plate::layoutTag, &size, &tag)) && size == sizeof(tag) &&
            tag.count != 0)
            s.nameplateLayout = true;
    }
}

// Puts back the render targets, the depth view and the viewports the game had bound, whatever happens below.
struct RestoreTargets
{
    ID3D11DeviceContext* context = nullptr;
    ComPtr<ID3D11RenderTargetView> targets[kMaxTargets];
    ComPtr<ID3D11DepthStencilView> depth;
    D3D11_VIEWPORT viewports[kMaxTargets] {};
    UINT viewportCount = 0;

    explicit RestoreTargets(ID3D11DeviceContext* c) : context(c)
    {
        ID3D11RenderTargetView* raw[kMaxTargets] = {};
        ID3D11DepthStencilView* rawDepth = nullptr;
        context->OMGetRenderTargets(kMaxTargets, raw, &rawDepth);
        for (UINT i = 0; i < kMaxTargets; ++i)
            targets[i].Attach(raw[i]);
        depth.Attach(rawDepth);
        UINT count = kMaxTargets;
        context->RSGetViewports(&count, viewports);
        viewportCount = count <= kMaxTargets ? count : kMaxTargets;
    }
    ~RestoreTargets()
    {
        ID3D11RenderTargetView* raw[kMaxTargets] = {};
        for (UINT i = 0; i < kMaxTargets; ++i)
            raw[i] = targets[i].Get();
        context->OMSetRenderTargets(kMaxTargets, raw, depth.Get());
        if (viewportCount != 0)
            context->RSSetViewports(viewportCount, viewports);
    }
    RestoreTargets(const RestoreTargets&) = delete;
    RestoreTargets& operator=(const RestoreTargets&) = delete;
};

// The one redirect routine: clear the slot once per frame, bind it with the game's depth view, draw the same triangles
// through the real DrawIndexed and put the game's state back. True = the triangles reached the slot.
bool RedirectDraw(ID3D11DeviceContext* context, const ext::hudless::plates::FrameReservation& reservation, unsigned count,
                  unsigned start, int baseVertex, void* original)
{
    if (reservation.target == nullptr || count == 0 || original == nullptr)
        return false;
    RestoreTargets restore(context);
    if (!g_latch.cleared)
    {
        // The image is composited by the pass with premultiplied-alpha blending, so an untouched pixel of it means "no
        // plate here". Cleared once per frame, before the frame's first plate triangles.
        const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
        context->ClearRenderTargetView(reservation.target, clear);
        g_latch.cleared = true;
    }
    const D3D11_VIEWPORT viewport { 0.f, 0.f, static_cast<float>(reservation.width),
                                    static_cast<float>(reservation.height), 0.f, 1.f };
    context->OMSetRenderTargets(1, &reservation.target, restore.depth.Get());
    context->RSSetViewports(1, &viewport);
    reinterpret_cast<DrawIndexedFn>(original)(context, count, start, baseVertex);
    return true;
}

// The engine's frame finish (called on the render thread while no draw is in flight). This build has no tracker, so the
// frame carries no movers and the pass draws the image unshifted: the endpoint stage.
void FinishFrame(ext::hudless::plates::PlateFrame& frame, void* /*user*/)
{
    frame.viewportScale = 1.0f;
    frame.moverCount = 0;
    const uint64_t finished = g_counts.framesFinished.fetch_add(1, std::memory_order_relaxed) + 1;
    if (finished % 600 == 0)
        LOG_INFO("Plates: draws {} matched {} asks {} declined {} shadowed {} hidden {} fallback {}",
                 g_counts.drawsSeen.load(std::memory_order_relaxed), g_counts.drawsMatched.load(std::memory_order_relaxed),
                 g_counts.reserveAsks.load(std::memory_order_relaxed),
                 g_counts.framesDeclined.load(std::memory_order_relaxed),
                 g_counts.drawsShadowed.load(std::memory_order_relaxed),
                 g_counts.drawsHidden.load(std::memory_order_relaxed),
                 g_counts.drawsFallback.load(std::memory_order_relaxed));
}

// Registered once, at load: the engine pulls the frame finish at its own present seam, so the game side keeps no tick
// of its own and nothing here has to be called from the bridge.
struct RegisterFinish
{
    RegisterFinish() { ext::hudless::plates::SetPlateFinishHook(&FinishFrame, nullptr); }
};
const RegisterFinish g_registerFinish;
} // namespace

namespace detail
{
bool InterceptIndexedSlow(ID3D11DeviceContext* context, unsigned count, unsigned start, int baseVertex, void* original)
{
    if (context == nullptr || original == nullptr || count == 0)
        return false;
    Bump(g_counts.drawsSeen);

    // The frame's answer: it is taken once, at the first accepted plate draw, and reused until the present seam
    // publishes the next record. A declined frame stays native, so its later draws do not even read the state.
    const uint64_t serial = ext::hudless::plates::FrameSerial();
    if (g_latch.valid && g_latch.serial == serial)
    {
        if (g_latch.reservation.mode == ext::hudless::plates::FrameMode::Declined)
            return false;
    }

    DrawInfo info;
    ReadDrawState(context, info);
    if (!IsPlateDraw(info.state))
        return false;
    Bump(g_counts.drawsMatched);

    if (!g_latch.valid || g_latch.serial != serial)
    {
        g_latch.valid = true;
        g_latch.cleared = false;
        g_latch.serial = serial;
        Bump(g_counts.reserveAsks);
        g_latch.reservation = ext::hudless::plates::ReserveFrame(info.width, info.height,
                                                                static_cast<unsigned>(info.format));
        if (!g_latch.reservation)
        {
            Bump(g_counts.framesDeclined);
            return false; // the whole frame stays native: an older hidden image is cleared at this present
        }
    }

    const auto& reservation = g_latch.reservation;
    // The frame's slot has the size and format of the draw that reserved it. A later draw of the same frame with another
    // target is left to the game: the redirect would bind it to a view of the wrong size and the driver would drop the
    // draw without a word, so the frame would lose that plate with nothing to show it happened.
    if (info.width != reservation.width || info.height != reservation.height ||
        static_cast<unsigned>(info.format) != reservation.format)
        return false;
    const bool hide = reservation.mode == ext::hudless::plates::FrameMode::Hidden;
    if (!RedirectDraw(context, reservation, count, start, baseVertex, original))
    {
        // Nothing reached the slot: the draw stays native. A hidden frame then holds fewer plates in its image, which
        // is bounded by the frame itself; there is no second (shadow) routine to fall into.
        Bump(g_counts.drawsFallback);
        return false;
    }
    ext::hudless::plates::NotePlateDraw();
    if (hide)
    {
        Bump(g_counts.drawsHidden);
        return true; // the plate pass draws these triangles back over every output of the frame
    }
    Bump(g_counts.drawsShadowed);
    return false; // shadow: the game's own draw follows, unchanged
}
} // namespace detail

Counters Snapshot()
{
    Counters out;
    out.drawsSeen = g_counts.drawsSeen.load(std::memory_order_relaxed);
    out.drawsMatched = g_counts.drawsMatched.load(std::memory_order_relaxed);
    out.reserveAsks = g_counts.reserveAsks.load(std::memory_order_relaxed);
    out.framesDeclined = g_counts.framesDeclined.load(std::memory_order_relaxed);
    out.drawsShadowed = g_counts.drawsShadowed.load(std::memory_order_relaxed);
    out.drawsHidden = g_counts.drawsHidden.load(std::memory_order_relaxed);
    out.drawsFallback = g_counts.drawsFallback.load(std::memory_order_relaxed);
    out.framesFinished = g_counts.framesFinished.load(std::memory_order_relaxed);
    return out;
}
} // namespace ext::ffxiv::plates
