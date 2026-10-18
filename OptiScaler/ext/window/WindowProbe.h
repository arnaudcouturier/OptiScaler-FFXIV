#pragma once
// The one-line probe record (docs/FEATURES.md, "Window mode"). Pure formatting: the module fills the fields, this turns them
// into the text that goes to OptiScaler.log. Every line starts with "Window:" so a single search finds all of them.

#include "ext/window/WindowMode.h"
#include "ext/window/WindowRules.h"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace ext::window
{
struct WindowInfo
{
    bool valid = false; // the window handle could be read
    uint32_t style = 0;   // GWL_STYLE
    uint32_t exstyle = 0; // GWL_EXSTYLE
    int32_t left = 0, top = 0, right = 0, bottom = 0; // GetWindowRect
    int32_t clientWidth = 0, clientHeight = 0;       // GetClientRect
};

struct ProbeFields
{
    std::string_view event; // "resize-done" | "set-fullscreen-state"
    Mode mode = Mode::Off;
    uint64_t hwnd = 0;
    WindowInfo window;
    FgSnapshot fg;
    bool fgOffConfirmed = false;
    bool dlssgOutput = false;
    bool exclusive = false;
    int prevExclusive = -1; // -1 = not seen before
    bool forceBorderless = false;
    uint32_t ownerThread = 0;
    uint32_t currentThread = 0;
    uint32_t presentThread = 0;
    bool fixArmed = false;
    bool nested = false; // resize-done ran inside one of the module's own window calls

    // set-fullscreen-state only
    bool hasCall = false;
    bool callFullscreen = false;
    uint64_t callTarget = 0;    // the event's hmonitor argument, as the raw value
    uint64_t nativeSwapchain = 0; // swapchain->get_native()
    uint64_t bridgeSwapchain = 0; // the bridge swapchain stored at the last resize-done
    DeferVerdict verdict = DeferVerdict::UnknownSwapchain;
    const char* deferState = "idle";
    bool wouldReturn = false; // what Mode=on would return from the handler
    bool returned = false;    // what the handler really returned
};

inline std::string FormatProbeLine(const ProbeFields& f)
{
    std::string out = std::format("Window: {} mode={} hwnd={:#x}", f.event, ModeName(f.mode), f.hwnd);

    if (f.window.valid)
    {
        out += std::format(" style={:#010x} exstyle={:#010x} window={},{} {}x{} client={}x{}", f.window.style,
                           f.window.exstyle, f.window.left, f.window.top, f.window.right - f.window.left,
                           f.window.bottom - f.window.top, f.window.clientWidth, f.window.clientHeight);
    }
    else
    {
        out += " window=unreadable";
    }

    out += std::format(" fg={} fgOffConfirmed={} output={} exclusive={}", FgStateName(f.fg), f.fgOffConfirmed ? "yes" : "no",
                       f.dlssgOutput ? "DLSS-G" : "other", f.exclusive ? 1 : 0);
    if (f.prevExclusive >= 0)
        out += std::format(" prevExclusive={}", f.prevExclusive);
    out += std::format(" forceBorderless={} ownerTid={} currentTid={} presentTid={} sameThread={} fixArmed={}",
                       f.forceBorderless ? 1 : 0, f.ownerThread, f.currentThread, f.presentThread,
                       ThreadsAgree(f.ownerThread, f.currentThread, f.presentThread) ? "yes" : "no",
                       f.fixArmed ? "yes" : "no");

    if (f.nested)
        out += " nested=yes(ran inside a window call of this module)";

    if (f.hasCall)
    {
        out += std::format(" call=SetFullscreenState({}) target={:#x} native={:#x} bridge={:#x} sameObject={} state={} verdict={} "
                           "wouldReturn={} returned={}",
                           f.callFullscreen ? "TRUE" : "FALSE", f.callTarget, f.nativeSwapchain, f.bridgeSwapchain,
                           (f.nativeSwapchain != 0 && f.nativeSwapchain == f.bridgeSwapchain) ? "yes" : "no", f.deferState,
                           VerdictName(f.verdict), f.wouldReturn ? "true" : "false", f.returned ? "true" : "false");
    }
    return out;
}
} // namespace ext::window
