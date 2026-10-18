#pragma once
// The character preview probe (config key [FFXIV] PreviewProbe, debug aid, off by default). It answers how the preview image
// reaches the screen: which textures hold a copy of it, which draws sample the preview or one of those copies, and whether the
// swap the AA and DLSS paths make is bound on the draw that puts the image on screen.
//
// Everything runs on the draw thread, from the dispatcher (CharaDispatch.cpp) and with the immediate context only. The menu
// reads the counters in g_stats and Live(). While the probe is off every entry point returns after one flag check.

#include <array>
#include <atomic>
#include <cstdint>
#include <d3d11.h>
#include <string>
#include <wrl/client.h>

#include "ext/ffxiv/chara/CharaDraw.h"
#include "ext/ffxiv/chara/CharaProbeRules.h"
#include "ext/ffxiv/chara/CharaTable.h"

namespace ext::ffxiv::chara::probe
{
namespace detail
{
inline std::atomic<bool> g_live { false };           // the probe is switched on (set and cleared by Update)
inline std::atomic<uint64_t> g_lastActivity { 0 };   // GetTickCount64() of the last draw that rendered into a table entry or was geometry
} // namespace detail

// True from the moment Update saw PreviewProbe or PreviewDebugFlood on until it saw both off. The dispatcher keeps running while
// this is set, so the probe can release its objects after a key was switched off. (The flood test follows copies of the preview
// the same way the probe does, but writes none of the probe's log lines.)
inline bool Live() { return detail::g_live.load(std::memory_order_relaxed); }

// Cheap: remembers that the preview renderer is active (called for every draw that renders into a table entry and for geometry
// draws). The per-draw scan only runs for a short time after it.
inline void NoteActivity(uint64_t now)
{
    if (Live())
        detail::g_lastActivity.store(now, std::memory_order_relaxed);
}

// Reads PreviewProbe: on a change, switches the probe on (one INFO line) or off (releases everything, one INFO line). Draw thread,
// at the start of every dispatched draw; costs one config read and one atomic load. While the probe is on, about every 100 ms it
// also finishes a preview image dump that is in flight, starts a requested one and (every 500 ms) polls the command file; those
// need the immediate context (`context`, null skips them).
void Update(uint64_t now, ID3D11DeviceContext* context = nullptr);

// "Save preview images" (menu thread or command): asks for the preview images and state.txt to be saved. Executed on the draw thread
// by Update. False when the probe is off or a request is already waiting or in flight.
bool RequestDump();

// What the menu shows about the dumps (any thread).
struct DumpStatus
{
    bool probeOn = false;
    bool busy = false;        // a request is waiting or a dump is in flight
    uint32_t dumps = 0;       // dumps that wrote files this session
    uint32_t refusals = 0;    // dumps refused by the output budget
    uint32_t files = 0;       // files written this session
    uint64_t bytes = 0;
    std::string lastFolder;   // empty until a dump was written
    std::string lastMessage;  // outcome of the last request
};
DumpStatus GetDumpStatus();

// One of the first eight pixel-shader resources that is a preview output (table entries 0..7) or a texture derived from one.
struct ScanHit
{
    UINT slot = 0;
    uintptr_t texture = 0;
    int entry = -1;       // preview table entry when the texture is a preview output, else -1
    bool derived = false; // the texture is in the derived set
    int sourceEntry = -1; // the preview entry its image came from
    uint32_t hops = 0;    // derived only
    DerivedKind kind = DerivedKind::Copy;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource; // the texture (a reference, released with the result)
};

// What the per-draw scan found. `slotMask` has bit n set for every hit at pixel-shader slot n. Used by the flood test and (later)
// by the dump of the preview images.
struct ScanResult
{
    std::array<ScanHit, 8> hits {};
    size_t count = 0;
    uint32_t slotMask = 0;
};

// Per draw, before the draw is classified (the draws that matter here sample a copy of the preview and have no role): compares the
// first eight pixel-shader resources with the preview outputs (table entries 0..7) and the derived set. While the probe is on it
// logs new cases and grows the derived set with the render target of a draw that sampled one; while only the flood test is on it
// scans every draw and follows copies without writing log lines. Returns the hits (empty when both are off or nothing matched).
ScanResult OnDraw(ID3D11DeviceContext* context, DrawApi api, const table::Snapshot& snapshot, uint64_t now);

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
