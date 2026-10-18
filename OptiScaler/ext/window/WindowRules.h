#pragma once
// Pure rules of the window-mode module (docs/FEATURES.md, "Window mode"). No Win32, no upstream types: every function here
// is a plain function of its arguments, so the tests run them on fakes.
//
// Background: with [XeFG] ForceBorderless the fork's hook swallows the game's
// SetFullscreenState and restyles the window itself. A switch made while DLSS-G is on can stall Streamline's frame pacer
// (seen once as a crash), and leaving the pseudo fullscreen leaves a client edge on a frame-less window.

#include <cstdint>
#include <optional>

namespace ext::window
{
// ---- frame generation state --------------------------------------------------------------------------------------------

// What the module can see of the frame generation feature at one instant.
struct FgSnapshot
{
    bool present = false; // a feature object exists (State::currentFG)
    bool active = false;  // IFGFeature::IsActive()
    bool paused = false;  // IFGFeature::IsPaused()
};

constexpr const char* FgStateName(const FgSnapshot& fg)
{
    if (!fg.present)
        return "none";
    if (fg.active)
        return "active";
    return fg.paused ? "paused" : "off";
}

// "Frame generation is off" for the module's decisions. A missing feature object is NOT off: nothing can be confirmed.
constexpr bool FgIsOff(const FgSnapshot& fg) { return fg.present && !fg.active; }

// The signal that Streamline's DLSS-G is really off. DLSSG_Common::Deactivate only requests eOff; Streamline applies it
// inside the next Present (5-16 ms later in 29 logged harmless no-op switches). So "off" is confirmed after
// kPresentsNeeded COMPLETE game presents that started and ended with the feature inactive; any present that starts or ends
// with it active starts the count again. Only meaningful when the output is DLSS-G.
class FgOffGate
{
  public:
    static constexpr uint32_t kPresentsNeeded = 1;

    // Frame generation was asked to go off (or came back on): forget what was seen before.
    void Reset()
    {
        _count = 0;
        _startedOff = false;
    }
    void OnPresentBegin(const FgSnapshot& fg) { _startedOff = FgIsOff(fg); }
    void OnPresentEnd(const FgSnapshot& fg)
    {
        if (_startedOff && FgIsOff(fg))
        {
            if (_count < UINT32_MAX)
                ++_count;
        }
        else
        {
            _count = 0;
        }
        _startedOff = false;
    }
    bool Confirmed(bool dlssgOutput) const { return dlssgOutput && _count >= kPresentsNeeded; }
    uint32_t Count() const { return _count; }

  private:
    uint32_t _count = 0;
    bool _startedOff = false;
};

// ---- threads -----------------------------------------------------------------------------------------------------------

// A window call is only made from the window's own thread, and a deferred call is only replayed on the thread that made
// it. All three ids must be known (non-zero) and equal. 0 = unknown.
constexpr bool ThreadsAgree(uint32_t ownerThread, uint32_t callThread, uint32_t presentThread)
{
    return ownerThread != 0 && ownerThread == callThread && ownerThread == presentThread;
}

// ---- the mode switch decision ------------------------------------------------------------------------------------------

// With ForceBorderless the hook maps SetFullscreenState(TRUE) to FALSE and flips SCExclusiveFullscreen; the window is
// restyled only when that flag changes. A repeated request for the state that is already in effect changes nothing.
constexpr bool WouldChangeWindow(bool requestedFullscreen, bool exclusiveNow) { return requestedFullscreen != exclusiveNow; }

struct DeferInputs
{
    bool forceBorderless = false;  // [XeFG] ForceBorderless
    bool dlssgOutput = false;      // State::activeFgOutput == DLSSG
    bool fgActive = false;         // frame generation is running
    bool exclusiveNow = false;     // State::SCExclusiveFullscreen
    bool requestedFullscreen = false;
    bool threadsAgree = false;     // ThreadsAgree(owner, handler thread, present thread)
    bool swapchainIsBridge = false; // the call's swapchain is the bridge swapchain the module knows
};

enum class DeferVerdict : uint8_t
{
    Defer,              // request frame generation off, keep the call, replay it later
    NotForceBorderless, // the call reaches Streamline's own guard (it is not swallowed by the hook)
    NotDlssg,           // the stall is a DLSS-G pacer stall
    FgNotActive,        // nothing to turn off first
    NoWindowChange,     // a re-assertion of the current state: no restyle, no stall
    ThreadMismatch,     // owner thread, call thread and present thread are not one thread
    UnknownSwapchain,   // not the bridge swapchain, or none known yet
};

constexpr DeferVerdict DecideDefer(const DeferInputs& in)
{
    if (!in.forceBorderless)
        return DeferVerdict::NotForceBorderless;
    if (!in.dlssgOutput)
        return DeferVerdict::NotDlssg;
    if (!in.fgActive)
        return DeferVerdict::FgNotActive;
    if (!WouldChangeWindow(in.requestedFullscreen, in.exclusiveNow))
        return DeferVerdict::NoWindowChange;
    if (!in.threadsAgree)
        return DeferVerdict::ThreadMismatch;
    if (!in.swapchainIsBridge)
        return DeferVerdict::UnknownSwapchain;
    return DeferVerdict::Defer;
}

constexpr const char* VerdictName(DeferVerdict v)
{
    switch (v)
    {
    case DeferVerdict::Defer:
        return "defer";
    case DeferVerdict::NotForceBorderless:
        return "pass(no ForceBorderless)";
    case DeferVerdict::NotDlssg:
        return "pass(output is not DLSS-G)";
    case DeferVerdict::FgNotActive:
        return "pass(FG not active)";
    case DeferVerdict::NoWindowChange:
        return "pass(no window change)";
    case DeferVerdict::ThreadMismatch:
        return "pass(threads differ)";
    case DeferVerdict::UnknownSwapchain:
        return "pass(not the bridge swapchain)";
    }
    return "pass(?)";
}
} // namespace ext::window
