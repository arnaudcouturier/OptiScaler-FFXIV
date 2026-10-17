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
    // S7 C2/C3: refined from the placeholder Extent-out signature (owner-finalised, same latitude C1/C5 used for
    // INativeQuality::Request). The oracle's adfbea9d correction (docs/SEAMS.md 5 "RenderSizeFromWidthHeight") has
    // to compare the game's own NVSDK_NGX_Parameter_Width/Height (`paramW`/`paramH`, 0 = absent/unreadable) against
    // the subrect-derived render size (`w`/`h`, in/out), which a single output Extent cannot express. Gated entirely
    // inside the implementation (config flag, default off); returns whether `w`/`h` were corrected -- false is a
    // no-op, upstream's own size stands.
    virtual bool ResolveRenderExtent(unsigned int paramW, unsigned int paramH, unsigned int& w, unsigned int& h) = 0;
    // S7 C4: refined from the placeholder zero-arg signature, same owner-finalised latitude as ResolveRenderExtent.
    // True when the colour input is genuinely allocated at display size even though the live render size is
    // smaller: `renderWidth`/`renderHeight` (0 = not yet known) against `allocWidth`/`allocHeight` (the colour
    // resource's own GetDesc(); `allocWidth` is uint64_t to match D3D12_RESOURCE_DESC::Width exactly) must show a
    // real, in-bounds shrink, and the game's own `subrectWidth`/`subrectHeight`
    // (NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_*, read by the caller so this interface never sees an NGX
    // type) must describe the whole allocation, not some other crop. Delegates to
    // ext::upscaler::ColorIsDisplayAllocationValue for FFXIV (ext/ffxiv/geometry/GeometryProvider.cpp), the same
    // pure-math split as ResolveRenderExtent/ResolveRenderExtentValue; also carries the RenderSizeFromWidthHeight
    // config read, coupling this scale to the same flag as ResolveRenderExtent's correction (the oracle's single
    // !Available() gate covers both).
    virtual bool ColorIsDisplayAllocation(unsigned int renderWidth, unsigned int renderHeight, uint64_t allocWidth,
                                          unsigned int allocHeight, unsigned int subrectWidth,
                                          unsigned int subrectHeight) = 0;
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
    // Side-effect-free twin of Open(.., presentation = true) for a caller that only needs to know whether the scene would
    // own this frame (the HUD-less engine asks before the bridge's own Open): true exactly when Open(device, width,
    // height, true) would currently return a scene. Touches no request/fallback counter, log line, reservation or cached
    // import, so it never changes what Open reports later. It has no device argument: the clauses that compare against
    // Open's device (the owner of a published reconstructed image, the device a capture was imported on) are presumed to
    // hold (a bridge has one D3D12 device), and a capture that was not imported yet counts as importable when its shared
    // images are shareable (an import that fails inside the driver cannot be predicted without doing it).
    virtual bool WouldSupplyScene(UINT width, UINT height) = 0;
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
