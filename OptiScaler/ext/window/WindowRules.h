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
    // `reachedFg` = the present got as far as the frame generation present (the swapchain Present Streamline sees). A present
    // that returned early (an error path before it) says nothing about Streamline: the count starts again.
    void OnPresentEnd(const FgSnapshot& fg, bool reachedFg = true)
    {
        if (reachedFg && _startedOff && FgIsOff(fg))
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

// ---- the present counter -------------------------------------------------------------------------------------------------

// The bridge (Dx11wDx12SC::Present) advances its back buffer index, modulo the buffer count, only after the frame generation
// swapchain's Present returned success, and does so whether frame generation is on or off. A present that returned early
// never advances it. Read at the start and at the end of a game present, a CHANGED value means "this present reached the FG
// present" (compare for inequality, never "greater": the index wraps). With fewer than two buffers the index can never change,
// so it says nothing: no counter, and the module falls back to counting every present.
constexpr std::optional<uint64_t> PresentSerialFromIndex(uint32_t index, uint32_t bufferCount)
{
    if (bufferCount < 2)
        return std::nullopt;
    return index;
}

// A game present reached the FG present when its counter changed between the start and the end. Without a counter at either
// end (no bridge, fewer than two buffers, a different bridge by the end) nothing is known and the present counts.
constexpr bool PresentReachedFg(const std::optional<uint64_t>& atBegin, const std::optional<uint64_t>& atEnd)
{
    if (!atBegin || !atEnd)
        return true;
    return *atBegin != *atEnd;
}

// ---- threads -----------------------------------------------------------------------------------------------------------

// A window call is only made from the window's own thread, and a deferred call is only replayed on the thread that made
// it. All three ids must be known (non-zero) and equal. 0 = unknown.
constexpr bool ThreadsAgree(uint32_t ownerThread, uint32_t callThread, uint32_t presentThread)
{
    return ownerThread != 0 && ownerThread == callThread && ownerThread == presentThread;
}

// ---- the border rule ---------------------------------------------------------------------------------------------------

// winuser.h values, repeated here so this header needs no SDK header (WindowWin32.cpp static_asserts them).
inline constexpr uint32_t kStyleCaption = 0x00C00000u;     // WS_CAPTION = WS_BORDER | WS_DLGFRAME
inline constexpr uint32_t kStyleThickFrame = 0x00040000u;  // WS_THICKFRAME
inline constexpr uint32_t kExStyleWindowEdge = 0x00000100u; // WS_EX_WINDOWEDGE
inline constexpr uint32_t kExStyleClientEdge = 0x00000200u; // WS_EX_CLIENTEDGE

// B1-a. Upstream's leave branch writes WS_EX_OVERLAPPEDWINDOW (= window edge + client edge) while the game keeps its own
// frame-less style, so the client area shrinks by 2 px on every side. A frame-less window never carries an edge: if the
// style has neither a caption nor a sizing frame and the extended style has one of the two edge bits, those bits are
// cleared and the new extended style is returned. Any frame bit at all (a real windowed mode) or no edge bit: nullopt, the
// window is left alone.
constexpr std::optional<uint32_t> ClearEdge(uint32_t style, uint32_t exstyle)
{
    if ((style & (kStyleCaption | kStyleThickFrame)) != 0)
        return std::nullopt;
    const uint32_t edges = exstyle & (kExStyleClientEdge | kExStyleWindowEdge);
    if (edges == 0)
        return std::nullopt;
    return exstyle & ~edges;
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
    bool sizeMatchesWindow = false; // the FG swapchain is as large as the window rect: the follow-up ResizeBuffers is a same-size no-op
    bool targetOk = false;          // the event target is 0 or the monitor of the game window
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
    SizeMismatch,       // swapchain size differs from the window rect: the game follow-up ResizeBuffers would flush Streamline unprotected
    TargetDiffers,      // the call names an output other than the window monitor: the replay (null target) would pick another monitor
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
    if (!in.targetOk)
        return DeferVerdict::TargetDiffers;
    if (!in.sizeMatchesWindow)
        return DeferVerdict::SizeMismatch;
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
    case DeferVerdict::TargetDiffers:
        return "pass(target output is not the window monitor)";
    case DeferVerdict::SizeMismatch:
        return "pass(swapchain size differs from window)";
    }
    return "pass(?)";
}
} // namespace ext::window
