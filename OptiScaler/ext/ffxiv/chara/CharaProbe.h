#pragma once
// The character preview probe (config key [FFXIV] PreviewProbe, debug aid, off by default). It answers how the preview image
// reaches the screen: which textures hold a copy of it, which draws sample the preview or one of those copies, and whether the
// swap the AA and DLSS paths make is bound on the draw that puts the image on screen.
//
// Everything runs on the draw thread, from the dispatcher (CharaDispatch.cpp) and with the immediate context only. The menu
// reads the counters in g_stats and Live(). While the probe is off every entry point returns after one flag check.

#include <atomic>
#include <cstdint>
#include <d3d11.h>

#include "ext/ffxiv/chara/CharaDraw.h"
#include "ext/ffxiv/chara/CharaTable.h"

namespace ext::ffxiv::chara::probe
{
namespace detail
{
inline std::atomic<bool> g_live { false };           // the probe is switched on (set and cleared by Update)
inline std::atomic<uint64_t> g_lastActivity { 0 };   // GetTickCount64() of the last draw that rendered into a table entry or was geometry
} // namespace detail

// True from the moment Update saw the key on until it saw it off. The dispatcher keeps running while this is set, so the probe
// can release its objects after the key was switched off.
inline bool Live() { return detail::g_live.load(std::memory_order_relaxed); }

// Cheap: remembers that the preview renderer is active (called for every draw that renders into a table entry and for geometry
// draws). The per-draw scan only runs for a short time after it.
inline void NoteActivity(uint64_t now)
{
    if (Live())
        detail::g_lastActivity.store(now, std::memory_order_relaxed);
}

// Reads PreviewProbe: on a change, switches the probe on (one INFO line) or off (releases everything, one INFO line). Draw thread,
// at the start of every dispatched draw; costs one config read and one atomic load.
void Update(uint64_t now);

// Per draw, before the draw is classified (the draws that matter here sample a copy of the preview and have no role): compares the
// first eight pixel-shader resources with the preview outputs (table entries 0..7) and the derived set, logs new cases and grows
// the derived set with the render target of a draw that sampled one.
void OnDraw(ID3D11DeviceContext* context, DrawApi api, const table::Snapshot& snapshot, uint64_t now);

// Copies and resolves on the immediate context (not made by the feature itself). Called by the dispatcher before the DLSS gate:
// when the source is a preview output (entries 0..7) or a derived texture, the destination becomes derived.
void OnCopy(ID3D11DeviceContext* context, ID3D11Resource* dest, ID3D11Resource* source);
void OnCopyRegion(ID3D11DeviceContext* context, ID3D11Resource* dest, UINT destSub, UINT x, UINT y, UINT z, ID3D11Resource* source,
                  UINT sourceSub, const D3D11_BOX* box);
void OnResolve(ID3D11DeviceContext* context, ID3D11Resource* dest, UINT destSub, ID3D11Resource* source, UINT sourceSub,
               DXGI_FORMAT format);

// Forgets the derived textures, the logged cases and the counters (the `reset` command of the later command channel).
void Reset();

// Releases everything and removes the sink (the hooks are going away, the device is still valid).
void Detach();
} // namespace ext::ffxiv::chara::probe
