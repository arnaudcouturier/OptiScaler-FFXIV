#pragma once
// S10 contract: the game-specific half of the HUD-less engine (docs/HUDLESS.md section 4.1).
//
// A game module (FFXIV: ext/ffxiv/ui/FfxivUiBoundary) implements IHudlessSource and registers it as
// ext::game::Current().hudless. The generic engine (ext/hudless/) only ever talks to this interface, so another game
// needs its own boundary detector and nothing else.
//
// Threading: Active() may be called from any thread and must be a cheap atomic read. Take(), LastReason() and
// EndFrame() are called only from the game's Present thread (the bridge's Present), once per presented frame.

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace ext::hudless
{
// One UI-free image of one game frame, on the game's D3D11 device. The engine copies out of `texture` on the same
// immediate context before the game's next frame begins, so the source may reuse the texture after EndFrame().
struct FrameImage
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture; // single sample, not an array, same size as the swapchain backbuffer
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;        // typed format of `texture`
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t sequence = 0; // the source's own frame counter value at capture time (diagnostics only)
    // Set by the FFXIV live source (ui::live::LiveSource::Take) whatever its result; the boundary provider returns early with it false
    // while the detector is not armed or wanted or the shader catalog is not usable. Meaning: this frame's first UI draw into the
    // backbuffer was preceded by UI draws into another render target (an off-screen window, for instance). Only counted and logged;
    // false for a source that does not know.
    bool anchoredAfterOffscreenUi = false;
};

enum class TakeResult : uint8_t
{
    NoSource,     // the source is not armed (Mode off, unsupported build, hooks not installed)
    NotThisFrame, // armed, but no boundary was seen in this frame (loading screen, cutscene fade, unknown shaders)
    Rejected,     // a boundary was seen but the capture is unusable; LastReason() says why
    Ok,           // `out` holds this frame's UI-free image
};

struct IHudlessSource
{
    virtual ~IHudlessSource() = default;

    // True while the boundary detector is armed (cheap, atomic). The engine and the hook sites use it to early-out
    // at zero cost when the feature is off.
    virtual bool Active() = 0;

    // This frame's UI-free image, newest wins, consumed by the call. Never blocks, never waits on the GPU.
    virtual TakeResult Take(FrameImage& out) = 0;

    // Static string: why the last Take() was not Ok (shown in the menu and in the periodic summary log).
    virtual const char* LastReason() = 0;

    // End of the bridge Present: forgets this frame's boundary state.
    virtual void EndFrame() = 0;
};
} // namespace ext::hudless
