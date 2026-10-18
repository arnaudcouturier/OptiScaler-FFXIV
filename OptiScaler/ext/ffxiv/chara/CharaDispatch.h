#pragma once
// Entry points of the character preview feature (CharaView) for the hook layer (ext/ffxiv/lighting/FfxivLightingCapture.h)
// and the once-per-frame seam (ext/Seams.cpp). Everything attaches there; no upstream file is involved.
//
// Per draw the dispatcher routes:  (PreviewDLSS or PreviewNR) and the plumbing alive ? Dlss (SR and/or NR) : PreviewAA ? Enhance (spatial AA) : the native draw.
// Any doubt (accessor not verified, table empty, resource or format not understood, a deferred context) runs the native
// draw untouched.
//
// Own re-entrancy flags: the feature's draws never go through the lighting layer's `inside` flag, which is not set on
// every path, and a replay of a native draw by another module is marked with ReplayScope.

#include <atomic>
#include <d3d11.h>
#include <type_traits>

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaNr.h"
#include "ext/ffxiv/chara/CharaNrPlumbing.h"
#include "ext/ffxiv/chara/CharaDraw.h"
#include "ext/ffxiv/chara/CharaProbe.h"

namespace ext::ffxiv::chara
{
inline thread_local bool t_replay = false; // a replay of a native draw by another module: not a draw of the game

// Wraps the replays other modules make through the draw lambda (HDR mask replays), so the feature does not count or touch
// them: the real draw is the one that follows.
struct ReplayScope
{
    ReplayScope() : _previous(t_replay) { t_replay = true; }
    ~ReplayScope() { t_replay = _previous; }
    ReplayScope(const ReplayScope&) = delete;
    ReplayScope& operator=(const ReplayScope&) = delete;

  private:
    bool _previous;
};

namespace detail
{
inline std::atomic<bool> g_inert { false }; // latched when the game build or the table accessor cannot be verified
} // namespace detail

inline bool DlssWanted() { return ext::Cfg().ffxiv.chara.previewDlss.value_or_default(); }
inline bool AaWanted() { return ext::Cfg().ffxiv.chara.previewAA.value_or_default(); }

// Cheap gate in front of every draw: false means the feature does nothing for this draw. The debug aids (the probe and the
// flood test) keep the dispatcher running too; probe::Live() keeps it running for the draw that sees the probe switched off,
// so the probe can release what it holds.
inline bool Wanted()
{
    if (detail::g_inert.load(std::memory_order_relaxed))
        return false;
    const auto& c = ext::Cfg().ffxiv.chara;
    // PreviewNR keeps the gate open while it holds models after the key was switched off, so maintenance can free them.
    return nr::DispatcherWanted(c.previewAA.value_or_default(), c.previewDlss.value_or_default(), c.previewNr.value_or_default(),
                                nr::HoldsResources(),
                                c.previewProbe.value_or_default() || c.previewDebugFlood.value_or_default() || probe::Live());
}

void AroundSlow(ID3D11DeviceContext* context, DrawApi api, DrawCall draw);

// Wraps one native draw. `raw` runs the draw; it is called exactly once whatever the feature does.
template <class F> inline void Around(ID3D11DeviceContext* context, DrawApi api, F&& raw)
{
    if (!Wanted())
    {
        raw();
        return;
    }
    using Fn = std::remove_reference_t<F>;
    AroundSlow(context, api, DrawCall { [](void* p) { (*static_cast<Fn*>(p))(); }, const_cast<Fn*>(&raw) });
}

// The hook layer attached: `immediate` is the game's immediate context, `rawDraw` the original (unhooked) Draw.
void OnInstalled(ID3D11DeviceContext* immediate, void* rawDraw);
void OnDetached();
// The hooks are about to be removed: release every GPU object (device calls are still valid).
void Detach();

// CreatePixelShader succeeded; `crc` is the CRC-32 of the DXBC. Call after the other shader tags were written.
void OnShaderCreated(ID3D11PixelShader* shader, uint32_t crc, uint32_t dxbcSize);

// Hook forwarders. All of them return immediately unless the isolated DLSS path is on and active (the three copy forwarders also
// serve the preview probe, which does not depend on DLSS).
void OnCopy(ID3D11DeviceContext* context, ID3D11Resource* dest, ID3D11Resource* source, bool full); // CopyResource
void OnCopyRegion(ID3D11DeviceContext* context, ID3D11Resource* dest, UINT destSub, UINT x, UINT y, UINT z, ID3D11Resource* source,
                  UINT sourceSub, const D3D11_BOX* box);                                             // CopySubresourceRegion
void OnResolve(ID3D11DeviceContext* context, ID3D11Resource* dest, UINT destSub, ID3D11Resource* source, UINT sourceSub,
               DXGI_FORMAT format);                                                                 // ResolveSubresource
void OnClearRenderTargetView(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const FLOAT color[4]);
void OnQueryBegin(ID3D11DeviceContext* context, ID3D11Asynchronous* query);
void OnQueryEnd(ID3D11DeviceContext* context, ID3D11Asynchronous* query);
void OnVertexShaderCreated(ID3D11Device* device, ID3D11VertexShader* shader, const void* bytecode, SIZE_T size);

// Once per presented frame, from the DX11-on-DX12 bridge's frame callback. Any thread: it only bumps a counter that the
// draw thread consumes.
void OnFrameEnd();

// Issues a full-screen triangle (Draw(3, 0)) through the original, unhooked Draw when the hook layer provided it.
void DrawFullscreenTriangle(ID3D11DeviceContext* context);
} // namespace ext::ffxiv::chara
