#pragma once
// Entry points of the window-mode module (docs/FEATURES.md, "Window mode"). Called from the existing seam bodies in
// ext/hdr/BridgeHdr.cpp (OnResizeDone, PresentScope) and from the ReShade event in ext/reshade/ReShadeEvents.cpp; no
// upstream file names anything in here.
//
// Every entry point is inline-guarded: with [Window] Mode = off the cost is one relaxed atomic load and one branch. The
// out-of-line bodies never throw and fail open (a problem means "do nothing", never "stop the game's call").

#include "ext/window/WindowMode.h"

#include <atomic>
#include <cstdint>

class Dx11wDx12SC;

namespace ext::window
{
namespace detail
{
// Plain static of a trivial type: no destructor to run under the loader lock.
inline std::atomic<uint8_t> g_mode { static_cast<uint8_t>(Mode::Off) };
} // namespace detail

inline Mode CurrentMode() noexcept { return static_cast<Mode>(detail::g_mode.load(std::memory_order_relaxed)); }
inline void Publish(Mode mode) noexcept { detail::g_mode.store(static_cast<uint8_t>(mode), std::memory_order_relaxed); }

// Out-of-line bodies, WindowModule.cpp.
void ResizeDoneSlow(Dx11wDx12SC& sc) noexcept;
void PresentBeginSlow() noexcept;
void PresentEndSlow() noexcept;
bool SetFullscreenStateSlow(uint64_t native, uint64_t hwnd, bool fullscreen, uint64_t target) noexcept;

// ext::dx11bridge::OnResizeDone: after a successful bridge ResizeBuffers.
inline void OnResizeDone(Dx11wDx12SC& sc) noexcept
{
    if (CurrentMode() != Mode::Off)
        ResizeDoneSlow(sc);
}

// ext::dx11bridge::PresentScope: entry and exit of the game's Present on the bridge swapchain.
inline void OnPresentBegin() noexcept
{
    if (CurrentMode() != Mode::Off)
        PresentBeginSlow();
}
inline void OnPresentEnd() noexcept
{
    if (CurrentMode() != Mode::Off)
        PresentEndSlow();
}

// ReShade add-on event set_fullscreen_state (ext/reshade/ReShadeEvents.cpp). True = prevent the call.
inline bool OnSetFullscreenState(uint64_t native, uint64_t hwnd, bool fullscreen, uint64_t target) noexcept
{
    return CurrentMode() != Mode::Off && SetFullscreenStateSlow(native, hwnd, fullscreen, target);
}
} // namespace ext::window
