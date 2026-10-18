#pragma once
// Neural Rendering (NR) for the character previews. New work, not part of Nickle's build (see docs/RECONSTRUCTION-NICKLE.md).
//
// The NR model only runs on Direct3D 12 (the D3D11 entry points of the model refuse the network), so a preview image is handed to
// the DX11-on-12 bridge's D3D12 device through shared textures, processed there and drawn from a shared result texture:
//
//   D3D11: native image -> colour copy (shared)      Signal(ready)
//   D3D12: Wait(ready); Encode -> model -> Resolve -> result (shared); Signal(done)
//   D3D11: Wait(done); UI draws sample the result
//
// One NR stream per preview image (a model instance, the shared textures and private scratch images), created on the first use
// and released a few seconds after the preview was last drawn. The module never touches the main NR pass's state, owns its own
// fence, and treats any failure as final for the session (one log line, the native image stays).
//
// Entry points for the preview dispatcher (all on the thread that issues draws on the immediate context):
//   RunBringUp    : after the finisher drew the native image; colour only, synthetic guides (a cleared depth, zero motion)
//   RunProduction : not implemented yet (stub): after the super-resolution evaluate, with the real guides
//   ResultFor     : a UI draw that samples a preview image asks for the NR result to draw instead
//   Maintain      : releases streams (linger) and keeps the status current; call at least every 100 ms of draw activity
//   OnBridgeShutdown / Detach : release everything

#include <atomic>
#include <cstdint>

#include <d3d11.h>

#include "ext/ffxiv/chara/CharaNrRules.h"

namespace ext::ffxiv::chara::nr
{
struct RunRequest
{
    ID3D11DeviceContext* context = nullptr; // the immediate context the preview draws on
    ID3D11Texture2D* native = nullptr;      // the finisher's output (the preview image), B8G8R8A8_UNORM
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t now = 0;
};

enum class RunOutcome : uint8_t
{
    Ran,          // a result image exists for this render (drawn instead of the native one by ResultFor)
    Preparing,    // the model is being built; the native image stays for now
    NotAvailable, // preview NR is off, latched or its prerequisites are missing
    Skipped,      // no stream or list was free this time; nothing changed
    Failed,       // this call failed; preview NR is latched off
    NotImplemented,
};

// Config switch [FFXIV] PreviewNR.
bool Wanted();
// The NR latch (independent of the super-resolution latch).
bool Latched();
// True while a stream or GPU object exists, also with the config off: the dispatcher keeps calling Maintain until it is released.
bool HoldsResources();

// The latch pair shared with the super-resolution path (CharaDlss.cpp is meant to use SrLatched/LatchSr of the same object).
Latches& SharedLatches();

RunOutcome RunBringUp(const RunRequest& request);

// Runs NR on the super-resolution output (between the DLSS evaluate and the alpha pass of CharaDlss.cpp RunSlot), with the real depth
// and motion guides. Not implemented yet: it returns NotImplemented.
RunOutcome RunProduction(const RunRequest& request);

// The result image a UI draw should sample instead of `key` (the preview image's texture pointer), or null when the stream has no
// result that is current. In the late variant this issues the D3D11 wait on the previous run's completion.
ID3D11ShaderResourceView* ResultFor(ID3D11DeviceContext* context, uintptr_t key, uint64_t now);

// The NR result texture (R16G16B16A16_FLOAT, native size) of the stream that ran most recently, or null when none is current. Borrowed:
// valid until the stream is released; draw thread only. Used by the preview image dump (nr.png).
ID3D11Texture2D* ResultTexture(uint64_t now);

void Maintain(uint64_t now);
void OnFrameEnd();
// The graphics bridge is shutting down (any thread): everything is released before the driver's NGX shutdown.
void OnBridgeShutdown();
// The bridge initialised again (NGX re-initialisation): preview NR may rebuild.
void OnBridgeInit();
// The hooks are being removed (draw thread, device still valid).
void Detach();

struct Status
{
    Availability availability = Availability::ConfigOff;
    const char* reason = "";
    uint32_t streams = 0;      // streams that exist
    uint32_t liveStreams = 0;  // streams whose preview was drawn in the last 250 ms
    uint64_t runs = 0;         // successful NR runs
    uint64_t fallbacks = 0;    // renders that kept the native image (not available, preparing, skipped, failed)
    double lastGpuMs = 0.0;    // GPU time of the last run, from the module's own timestamps (0 = not measured)
    double lastCpuMs = 0.0;    // CPU time of recording and submitting the last run
    uint64_t videoMemoryMb = 0; // video memory the model instances added (measured around their creation; 0 = not measured)
    bool late = false;
};
Status GetStatus();
} // namespace ext::ffxiv::chara::nr
