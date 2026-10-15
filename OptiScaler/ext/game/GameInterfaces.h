#pragma once
// Game-module interfaces (docs/SEAMS.md section 3.8). A game implements only the interfaces it needs; feature code
// (nr, fg, hdr, hudless, gpu) reaches the game only through ext::game::Current().
//
// PROVISIONAL: method names come from SEAMS.md, parameter lists are placeholders that the owning series (C1-C5, B5,
// B6, C10, C6, D2) finalise when they port the first implementation. Keep interfaces small and separate.

#include "ext/hdr/SceneInput.h"

#include <cstdint>

namespace ext::game
{
// A rectangle inside a texture, in pixels
struct Extent
{
    uint32_t x = 0, y = 0, width = 0, height = 0;
};

// C2, C3, C4
struct IRenderGeometry
{
    virtual ~IRenderGeometry() = default;
    // Fills the game's real render extent inside the allocated texture; false = use upstream's size
    virtual bool ResolveRenderExtent(Extent& out) = 0;
    // True when the colour input is allocated at display size even though the render size is smaller
    virtual bool ColorIsDisplayAllocation() = 0;
};

// C1, C5
struct INativeQuality
{
    virtual ~INativeQuality() = default;
    virtual bool EnsureHooks() = 0; // lazy, call_once
    virtual bool Live() = 0;
    virtual bool DlaaRequired() = 0; // cached per frame
    // Signed to carry the oracle's -1 "game's choice" sentinel directly (finalised by the owning series, C1/C5:
    // the placeholder was uint32_t, which made -1 a silent wraparound at every call site).
    virtual void Request(int preset) = 0;
};

// A3 menu status of the native lighting scan (was FfxivLightingScan::Latest/Fresh and FfxivLightingCapture::installed,
// read by the NR menu).
struct LightingStatus
{
    bool hooksInstalled = false; // the native shader hooks are in place
    bool failed = false;         // the scan stopped after a capture or readback error (until restart)
    bool valid = false;          // a reading exists
    bool fresh = false;          // the lighting scan is enabled and the last reading is recent (Fresh())
    float gain = 0.0f;           // adapted gain of the last reading
    uint64_t samples = 0, events = 0;
    uint64_t ageMs = 0;      // age of the last reading
    const char* reason = ""; // why there is no usable reading, shown when !fresh and !valid
};

// B6, A3
struct ISceneLighting
{
    virtual ~ISceneLighting() = default;
    // A3 menu: status of the scan, polled while the NR card is open
    virtual LightingStatus Status() = 0;
    virtual void OnUpscaleBoundary(ID3D11DeviceContext* context, ID3D11Resource* color, bool hdr) = 0;
    // True once per fresh native lighting event. `cursor` is the caller's last seen event count (one per NR consumer);
    // the implementation updates it (A3, was FfxivLightingScan::ConsumeReset).
    virtual bool ConsumeHistoryReset(uint64_t& cursor) = 0;
};

// C10. Implemented only for a game whose renderer exposes the pre-clipping scene (FFXIV: the RenoDX tonemap stages
// replayed into FP16; ext/hdr/scene/FfxivSceneProvider.cpp supplies the implementation object, the game module only
// points `Game::hdr` at it). A non-null `ext::game::Current().hdr` is also the test OptiHDR uses for "this game
// supports OptiHDR" (was `gameExe == "ffxiv_dx11.exe"`), so a game module that sets it opts in to HDR10 output and
// scene HDR.
struct ISceneHdr
{
    virtual ~ISceneHdr() = default;
    // Same-frame HDR scene and SDR reference matching (width, height). `presentation` = the image the presentation
    // path converts (a reconstructed display image published this frame wins). Empty when scene HDR is not requested
    // or no scene matches (was FfxivSceneHdr::Open).
    virtual hdr::SceneInput Open(ID3D12Device* device, UINT width, UINT height, bool presentation) = 0;
    // The reconstructed display image of this frame (was FfxivSceneHdr::PublishReconstructed).
    virtual void PublishReconstructed(hdr::SceneInput&& image) = 0;
    // End of the bridge's Present: forgets the frame's captures (was FfxivSceneHdr::EndFrame).
    virtual void EndFrame() = 0;
};

// B5, D2, C5. Implemented only for a game whose frame-generation bridge needs scene pairing and native dynamic-MFG
// markers (FFXIV). A non-null `ext::game::Current().fgMarkers` is also the test the FG code uses for "scene-paired
// history applies" (was `gameExe == "ffxiv_dx11.exe"`), so a game module that sets it opts in to all of the below.
struct IFgMarkers
{
    virtual ~IFgMarkers() = default;
    // B5: age in real frames (0..2) of the NR guides to pair with this upscale, -1 = none. Called once per upscale and
    // consumed by the caller; the NR side is DlssNr::ConsumePresentationGuideDelay (was SetPresentationGuideDelay).
    virtual int PresentationGuideDelay() = 0;
    // D2: the bridge's native dynamic-MFG interval markers (simulation / render submit), see FfxivDynamicMarkers. They
    // reach Streamline PCL under ScopedOptiScalerReflex, which upstream's Reflex input already treats as OptiScaler's own.
    virtual void OnUpscale(uint64_t frame) = 0;     // FG upscale boundary of frame `frame`
    virtual void BeforePresent(uint32_t frameId) = 0; // FGPresent, before the Streamline frame token is fetched
    virtual void NextFrame(uint64_t frame) = 0;     // FGPresent, after ReflexSleep, `frame` = next frame count
};

// A1-A8, B5 (NR). NR behaviours that follow the FFXIV DX11 bridge path
// (was `State::gameExe == "ffxiv_dx11.exe"` inside the NR pass, PreSR and the exposure scan).
struct INrQuirks
{
    virtual ~INrQuirks() = default;
    virtual bool BridgedNativeDx11() = 0;
};

// C6
struct INgxLifecycle
{
    virtual ~INgxLifecycle() = default;
    virtual bool IsolatesShutdown() = 0;
};
} // namespace ext::game
