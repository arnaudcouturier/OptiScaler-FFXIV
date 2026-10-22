#pragma once
// Seams of the E5 composition-swapchain piece (docs/SEAMS-catalog.md "present"), included by ext/Seams.h.
// Bodies: ext/present/CompositionSwapchain.cpp. The window lookup is ext/present/CompositionWindow.h; the pure
// pass-through decision is ext/present/CompositionPassThrough.h (unit-testable without DXGI).
//
// The other ext::present seams (PlanDx12Presenter, FrameInterval, ...) belong to other pieces and are declared in
// their own headers (ext/hdr/HdrSeams.h, ext/fg/FgSeams.h).

#include "ext/present/FramePacingCore.h"

#include <windows.h>

struct IDXGIFactory;

namespace ext::present
{
// DxgiFactoryHooks::HookToFactory, last statement (after the upstream transaction and its failure reset). Hooks
// IDXGIFactory2::CreateSwapChainForComposition (vtable slot 24) in a Detours transaction of its own; idempotent
// (returns at once when already hooked or when `factory` has no IDXGIFactory2), failure leaves it unhooked. `factory`
// is the real factory HookToFactory just hooked. The hook wraps composition swapchains of at least 100x100 in
// WrappedIDXGISwapChain4 (composition flag), and passes everything else through untouched, including every call made
// while ext::core::HooksBypassed() is true (the Companion nameplate layer's own swapchain).
void HookFactoryComposition(IDXGIFactory* factory);

// WrappedIDXGISwapChain4::Present / Present1, only when the wrapper's `_composition` flag is set (a swapchain made
// by CreateSwapChainForComposition, which has no native HWND). Re-resolves `handle` when it is not a usable window
// of this process. Returns false when no unambiguous window exists yet: the caller then presents plainly on the real
// swapchain and the overlay initialises on a later Present.
bool ResolveCompositionHandle(HWND& handle);
} // namespace ext::present

// ---- Frame-pacing CPU trace and the hidden-present switch (ext/present/FramePacingTrace.cpp, rules in FramePacingCore.h and
// HiddenPresentPolicy.h). Keys: [FramePacing] Trace, HiddenPresentDoNotWait (ext/ini/FramePacing.ini). Nothing here throws.

struct IDXGISwapChain;

namespace ext::present
{
// Times the rest of the enclosing block as one call of `phase` (CPU time) and feeds the thread's 10 s window. Inactive (one call and a
// few loads) unless the trace is on: see Decide() in FramePacingCore.h. Used by wrapped_swapchain.cpp LocalPresent (OutputPresent, its
// first statement), FG_Hooks.cpp (ReflexSleep, directly before StreamlineProxy::ReflexSleep()) and DlssNr_Dx12::Dispatch (SceneNr).
// LocalPresent is only reached for real presents: both of its callers (Present, Present1) send DXGI_PRESENT_TEST presents straight to
// the real swapchain, so no output-present sample is a TEST present and nothing needs to be filtered here.
class PacingScope
{
  public:
    explicit PacingScope(Phase phase) noexcept;
    ~PacingScope();
    PacingScope(const PacingScope&) = delete;
    PacingScope& operator=(const PacingScope&) = delete;

    bool Active() const { return _active; }

  private:
    Phase _phase;
    int64_t _startUs;
    bool _active;
};

// An externally measured duration (microseconds) for `phase`, into the same windows. CharaNr.cpp feeds the preview NR recording time.
void RecordSample(Phase phase, uint64_t durationUs);

// ext::dx11bridge::PresentScope constructor / destructor: the whole DX11 bridge Present as one "native present" call.
void NativeBegin();
void NativeEnd();

// DLSS-G dispatch: FgDispatchBegin is the first statement of ext::fg::PresetSwitchTick, FgDispatchEnd the first statement of
// ext::fg::PostDispatch, so the span covers SetOptions and SetConstants of DLSSG_Dx12::Dispatch. A dispatch that leaves early gives no
// sample (the next Begin restarts the span; a span older than 1 s is dropped).
void FgDispatchBegin();
void FgDispatchEnd();

// dx11_with_dx12_sc.cpp, Dx11wDx12SC::Present: replaces `_real->Present(0, realFlags)` on the hidden swapchain. Times it ("hidden DX11
// present") and, only with [FramePacing] HiddenPresentDoNotWait on, adds DXGI_PRESENT_DO_NOT_WAIT: a busy chain answers
// DXGI_ERROR_WAS_STILL_DRAWING, which is counted and returned as S_OK (the caller's failure warning stays quiet). Every other result
// is returned unchanged. DXGI rejecting the flag (E_INVALIDARG, DXGI_ERROR_INVALID_CALL) switches it off for the session. With the key
// off this is exactly `swapChain->Present(0, flags)`.
HRESULT HiddenPresent(IDXGISwapChain* swapChain, UINT flags);
} // namespace ext::present
