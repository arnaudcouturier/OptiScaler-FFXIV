#pragma once
// Seams of the E5 composition-swapchain piece (docs/SEAMS-catalog.md "present"), included by ext/Seams.h.
// Bodies: ext/present/CompositionSwapchain.cpp. The window lookup is ext/present/CompositionWindow.h; the pure
// pass-through decision is ext/present/CompositionPassThrough.h (unit-testable without DXGI).
//
// The other ext::present seams (PlanDx12Presenter, FrameInterval, ...) belong to other pieces and are declared in
// their own headers (ext/hdr/HdrSeams.h, ext/fg/FgSeams.h).

#include <windows.h>

namespace ext::present
{
// WrappedIDXGISwapChain4::Present / Present1, only when the wrapper's `_composition` flag is set (a swapchain made
// by CreateSwapChainForComposition, which has no native HWND). Re-resolves `handle` when it is not a usable window
// of this process. Returns false when no unambiguous window exists yet: the caller then presents plainly on the real
// swapchain and the overlay initialises on a later Present.
bool ResolveCompositionHandle(HWND& handle);
} // namespace ext::present
