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
// Two modes, one stream table (keyed by the preview image's texture pointer, one stream per image):
//   bring-up   : NR on the native image (PreviewNR alone, and renders the super-resolution path did not evaluate): colour only, synthetic
//                guides (a cleared depth, zero motion); the result replaces the native image in the UI draws that sample it.
//   production : NR on the super-resolution output (PreviewDLSS and PreviewNR): the output (RGBA16F, twice the render size) and copies of the
//                slot's depth and motion (render size, the upscaler's own conventions) are handed over; the result (same size) is what the alpha
//                pass of the super-resolution path recomposes the original alpha onto. Always the same-frame hand-off (PreviewNRLate applies
//                to the bring-up only: the alpha recomposition needs this render's colour). Fail open: any outcome other than Ran leaves the
//                super-resolution output as it is.
// The stream is built for one mode at a time (StreamShape): a change of the wanted mode holds still for the settle time and rebuilds the
// stream, so a flapping render does not rebuild. Fences, command lists, the compute pass recorder, model creation, linger and release are
// shared. A bring-up consumer is never given a production result (ResultKind).
//
// Entry points for the preview dispatcher (all on the thread that issues draws on the immediate context):
//   RunBringUp    : after the finisher drew the native image; colour only, synthetic guides (a cleared depth, zero motion)
//   RunProduction : after the super-resolution evaluate, with the real guides
//   ResultFor     : a UI draw that samples a preview image asks for the bring-up NR result to draw instead
//   Maintain      : releases streams (linger) and keeps the status current; call at least every 100 ms of draw activity
//   OnBridgeShutdown / Detach : release everything

#include <atomic>
#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

#include "ext/ffxiv/chara/CharaNrRecipe.h"
#include "ext/ffxiv/chara/CharaNrRules.h"

namespace ext::ffxiv::chara::nr
{
struct RunRequest
{
    ID3D11DeviceContext* context = nullptr; // the immediate context the preview draws on
    ID3D11Texture2D* native = nullptr;      // the finisher's output (the preview image), B8G8R8A8_UNORM; also the key of the stream
    uint32_t width = 0;                     // the size NR works at: the native size (bring-up), twice the render size (production)
    uint32_t height = 0;
    uint64_t now = 0;
};

// Failed is enumerator 0 on purpose: Guarded answers with a value-initialised outcome when even a failure handler throws, and that value must
// never read as "a result image exists".
enum class RunOutcome : uint8_t
{
    Failed,       // this call failed; preview NR is latched off
    Ran,          // a result image exists for this render (drawn instead of the native one by ResultFor)
    Preparing,    // the model is being built; the native image stays for now
    NotAvailable, // preview NR is off, latched or its prerequisites are missing
    Skipped,      // no stream or list was free this time; nothing changed
};

// Config switch [FFXIV] PreviewNR.
bool Wanted();
// The NR latch (independent of the super-resolution latch).
bool Latched();
// The last availability check passed (kept current by Maintain; true before the first check so the first render can try).
bool Available();
// True while a stream or GPU object exists, also with the config off: the dispatcher keeps calling Maintain until it is released.
bool HoldsResources();

// The latch pair shared with the super-resolution path (CharaDlss.cpp is meant to use SrLatched/LatchSr of the same object).
Latches& SharedLatches();

RunOutcome RunBringUp(const RunRequest& request);

// Runs NR on the super-resolution output (between the DLSS evaluate and the alpha pass of CharaDlss.cpp RunSlot), with the real depth
// and motion guides. `base.width/height` is the size of `colour`; the guides are described by `guides` (MakeProductionGuides).
struct ProductionRequest
{
    RunRequest base;                       // base.native: the preview image (the key); base.width/height: the colour image's size
    ID3D11Texture2D* colour = nullptr;     // the super-resolution output: R16G16B16A16_FLOAT, base.width x base.height
    ID3D11Texture2D* depth = nullptr;      // the slot's depth: R32_FLOAT, guides.width x guides.height
    ID3D11Texture2D* motion = nullptr;     // the slot's motion: R16G16_FLOAT, guides.width x guides.height
    GuideSetup guides;                     // render size, depth convention and motion scale of the super-resolution evaluate
};

struct ProductionResult
{
    RunOutcome outcome = RunOutcome::NotAvailable; // anything but Ran: `view` is empty and the caller keeps the super-resolution output
    // The NR output (same size as the colour, RGBA16F), with its own reference. The inline D3D11 wait on the D3D12 work was queued by the run,
    // so a draw issued on the immediate context after this call samples the finished image.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    uint64_t serial = 0;
};
ProductionResult RunProduction(const ProductionRequest& request);
// The dispatcher declined to offer a render to the production entry (no preview texture, or a size NR does not take): counted as a production
// fallback and as a render that kept the super-resolution output.
void NoteProductionDeclined();

// A bring-up result a UI draw samples instead of the native image. `serial` identifies the result (never reused): the debug tint is made
// once per result.
struct ResultView
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    uint64_t serial = 0;
};

// The bring-up result a UI draw should sample instead of `key` (the preview image's texture pointer): an empty view when the stream has no
// bring-up result that is current (a production stream never answers). In the late variant this issues the D3D11 wait on the previous run's
// completion. The view is returned with its own reference: a shutdown on another thread cannot free it while the draw that uses it is still
// being issued.
ResultView ResultFor(ID3D11DeviceContext* context, uintptr_t key, uint64_t now);

// A render of the preview image `key` happened and preview NR produced no result for it: the older result of that image is withdrawn
// (ResultFor answers null until the next run). Called by the dispatcher for every finisher whose bring-up did not produce a result.
void Invalidate(uintptr_t key);

// The NR result texture (R16G16B16A16_FLOAT) of the stream that ran most recently, or null when none is current, with its own reference.
// `kind` (optional) says what it is: a bring-up result (native size, original alpha) or a production result (twice the render size, the raw
// NR output before the alpha pass: it has no recomposed alpha). `waitForResult`: issue the same D3D11 wait as ResultFor (the late variant is
// not ordered by an inline wait), for a caller that copies from the texture right away (the dump); false for a caller that only asks whether
// a result exists. Draw thread only. Used by the preview image dump (nr.png).
Microsoft::WRL::ComPtr<ID3D11Texture2D> ResultTexture(uint64_t now, bool waitForResult, ResultKind* kind = nullptr);

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
    uint64_t runs = 0;         // successful NR runs (both modes)
    uint64_t fallbacks = 0;    // renders that kept the native image or the super-resolution output (not available, preparing, skipped, failed)
    double lastGpuMs = 0.0;    // GPU time of the last run, from the module's own timestamps (0 = not measured)
    double lastCpuMs = 0.0;    // CPU time of recording and submitting the last run
    uint64_t productionRuns = 0;      // ... of which production runs
    uint64_t productionFallbacks = 0; // production calls that left the super-resolution output as it was
    double productionGpuMs = 0.0;     // GPU time of the last production run (0 = none measured yet)
    double productionCpuMs = 0.0;
    uint64_t videoMemoryMb = 0; // video memory the streams' images and model instances added (measured around their creation, given back when a stream is released; 0 = not measured)
    bool late = false;
};
Status GetStatus();
} // namespace ext::ffxiv::chara::nr
