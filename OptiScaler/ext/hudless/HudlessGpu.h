#pragma once
// S10 contract: the GPU half of the HUD-less engine (docs/HUDLESS.md section 4.2). Work package C implements it
// (ext/hudless/UiImage.cpp, UiPaste.cpp, shaders/*.hlsl); work package B (the engine core) is the only caller.
//
// Both modules take two D3D12 images of the SAME real frame: the final image (with UI) and the UI-free image, already
// copied across the D3D11 -> D3D12 interop and resting in D3D12_RESOURCE_STATE_COMMON (they are returned to it).
// The mask is detected on these two images as given; in HDR the engine passes the SDR pair and `MaskParams::hdr` tells
// the paste to take its colours from the converted PQ image (see docs/HUDLESS.md section 8).

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>

namespace ext::hudless
{
struct MaskParams
{
    float threshold = 0.008f; // largest per-channel difference that still counts as "same pixel"
    uint32_t dilation = 1;    // grow the UI area by this many pixels, clamped to 0..4
    bool hdr = false;         // mask in SDR, pixels from the PQ image the engine passes as `hdrFinalImage`
    bool cleanup = true;      // also paste where the previous frame had UI and this one does not (clears ghosts)
};

struct PasteStatus
{
    uint64_t produced = 0; // paste images built (one per real frame)
    uint64_t pasted = 0;   // output presents (real or generated) the UI was drawn on
    uint64_t skipped = 0;  // output presents left untouched although frame generation was active
    float coverage = -1.f; // share of pixels flagged as UI in the newest completed readback (0..1), < 0 = unknown yet
    // Amplitude profile of the same readback (docs/HUDLESS.md section 7): share of the picture whose windowed per-channel
    // difference reaches 0.008 / 0.05 / 0.2, whatever MaskThreshold is configured. They never feed a decision; -1 = unknown.
    float coverage008 = -1.f;
    float coverage05 = -1.f;
    float coverage2 = -1.f;
    // Ordinal of the paste image `coverage` was read back from: the value `produced` had right after that image was
    // built (1-based, cumulative, never reused). 0 until the first readback completes (and again after Release / a size
    // or device change, together with coverage = -1). A reader tells a fresh readback from a stale one by comparing it
    // with the value it saw last; `produced - coverageSequence` is how far the readback lags behind the producer.
    uint64_t coverageSequence = 0;
    char lastMessage[160] = {};
};

// Post frame-generation UI paste (UI-free mode).
namespace UiPaste
{
// Real-frame thread, once per real frame, after `queue` waited for the interop copy of both images. Never waits on the
// GPU: a slot is reused only after its previous write and every paste reading it have completed (otherwise the frame is
// skipped and counted). `hdrFinalImage` is null unless params.hdr.
void Produce(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* finalImage, DXGI_FORMAT finalFormat,
             ID3D12Resource* hudlessImage, DXGI_FORMAT hudlessFormat, ID3D12Resource* hdrFinalImage,
             const MaskParams& params);

// Real-frame thread, once per real frame that does not call Produce: stops pasting until the next Produce.
void Invalidate(const char* reason);

// UI-free input is allowed only after a matching completed paste image exists (size, device, not stale).
bool Ready(ID3D12Device* device, uint32_t width, uint32_t height);

// Resize / teardown: drops the images (freed once the GPU is done with them).
void Release();

// Any thread that presents the frame-generation swapchain (real and generated frames). Records and submits on
// `presentQueue`: draws the newest COMPLETED paste image over the current backbuffer, never older than the last one
// pasted and never older than 250 ms. Draws nothing and counts a skip otherwise. Never stalls pacing.
void Paste(IDXGISwapChain* swapChain, ID3D12CommandQueue* presentQueue);

// Output-side switch, any thread; on at start. While it is off Paste() returns at once: nothing is drawn and no skip is
// counted (a skip means a paste that was wanted and failed). Produce() and Invalidate() are not affected, so the paste
// images and their coverage readbacks keep coming. The engine sets it once per real frame, before the frame generator's
// outputs of that frame are presented.
void SetPasteEnabled(bool enabled);

void SetDebugTint(bool enabled);  // "Show detected UI"
void SetSteadyTiming(bool enabled); // pick the image of one real frame behind (steadier, slightly older UI)
PasteStatus Snapshot();
} // namespace UiPaste

// Recomposition mode: builds the "UI colour + alpha" image that DLSS-G / XeFG take as their UI input.
namespace UiImage
{
// Records the mask pass on `commandList` (the frame-generation provider's UI command list for `slot`) and returns the
// R8G8B8A8_UNORM image (alpha strictly 0 or 1), valid until `slot` is reused. Null when the pass cannot run.
ID3D12Resource* Build(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, int slot,
                      ID3D12Resource* finalImage, DXGI_FORMAT finalFormat, ID3D12Resource* hudlessImage,
                      DXGI_FORMAT hudlessFormat, const MaskParams& params);
void Release();
} // namespace UiImage
} // namespace ext::hudless
