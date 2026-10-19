#pragma once
// The two calls out of the live Env (WindowModule.cpp) whose ORDER or ARGUMENTS matter, as templates over the type they call,
// so a test drives the very same code with fakes that record what they were asked. No Windows or upstream types here.

#include <cstdint>

namespace ext::window::live
{
// What upstream's hkSetFullscreenState does first while frame generation is active (hooks/FG_Hooks.cpp, hkSetFullscreenState):
// fgChanged = true, then UpdateTarget(), then Deactivate(), in that order. The module asks for the same thing, so the
// feature goes off exactly as it would for a call that is not held back. No feature object: nothing is touched.
template <class Fg>
void RequestOff(Fg* fg, bool& fgChanged)
{
    if (fg == nullptr)
        return;
    fgChanged = true;
    fg->UpdateTarget();
    fg->Deactivate();
}

inline constexpr int32_t kPointerError = static_cast<int32_t>(0x80004003u); // E_POINTER

// IDXGISwapChain::SetFullscreenState(fullscreen, nullptr) on the swapchain ReShade passed (get_native()), not through
// ReShade's wrapper. The target is null on purpose: upstream then restyles for the window's own monitor (the hold only happens
// when the call's target was null or that monitor). A null swapchain is an error, not a call.
template <class Chain>
int32_t ReplaySetFullscreen(Chain* chain, bool fullscreen)
{
    if (chain == nullptr)
        return kPointerError;
    return static_cast<int32_t>(chain->SetFullscreenState(fullscreen ? 1 : 0, nullptr));
}
} // namespace ext::window::live
