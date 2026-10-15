#pragma once
// Game-module interfaces (docs/SEAMS.md section 3.8). A game implements only the interfaces it needs; feature code
// (nr, fg, hdr, hudless, gpu) reaches the game only through ext::game::Current().
//
// PROVISIONAL: method names come from SEAMS.md, parameter lists are placeholders that the owning series (C1-C5, B5,
// B6, C10, C6, D2) finalise when they port the first implementation. Keep interfaces small and separate.

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
    virtual void Request(uint32_t preset) = 0;
};

// B6, A3
struct ISceneLighting
{
    virtual ~ISceneLighting() = default;
    virtual void OnUpscaleBoundary() = 0;
    // True once per fresh native lighting event. `cursor` is the caller's last seen event count (one per NR consumer);
    // the implementation updates it (A3, was FfxivLightingScan::ConsumeReset).
    virtual bool ConsumeHistoryReset(uint64_t& cursor) = 0;
};

// C10
struct ISceneHdr
{
    virtual ~ISceneHdr() = default;
    virtual bool Open() = 0;
    virtual void PublishReconstructed() = 0;
};

// B5, D2, C5
struct IFgMarkers
{
    virtual ~IFgMarkers() = default;
    virtual uint32_t PresentationGuideDelay() = 0;
    virtual void OnUpscale() = 0;
    virtual void BeforePresent() = 0;
    virtual void NextFrame() = 0;
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
