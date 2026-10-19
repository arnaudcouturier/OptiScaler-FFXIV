// The probe record: one line, prefix "Window:", every field the findings ask for (style, exstyle, rects, frame generation
// state, SCExclusiveFullscreen, ForceBorderless, owner thread against the current thread, the handler result). The values are
// asymmetric on purpose (left 10, top 20, rect 3830x2140, client 3826x2136, swapchain 3840x2160) and the WHOLE line is
// asserted, so a swapped or misprinted field cannot pass.
#include "window_test.h"

using namespace ext::window;

#define CHECK_LINE(actual, expected)                                                                                        \
    do                                                                                                                      \
    {                                                                                                                       \
        const std::string a_ = (actual);                                                                                    \
        const std::string e_ = (expected);                                                                                  \
        if (a_ != e_)                                                                                                       \
        {                                                                                                                   \
            std::printf("CHECK failed %s:%d: line differs\n  actual:   %s\n  expected: %s\n", __FILE__, __LINE__,         \
                        a_.c_str(), e_.c_str());                                                                            \
            ++wt::g_failed;                                                                                                 \
        }                                                                                                                   \
    } while (0)

namespace
{
// The positive fixture: the 3836x2156-style border bug in a window that does not start at the origin.
ProbeFields Positive()
{
    ProbeFields f;
    f.event = "resize-done";
    f.mode = Mode::Probe;
    f.hwnd = 0xA0B0C;
    f.window.valid = true;
    f.window.style = 0x16CF0000u;
    f.window.exstyle = 0x00000200u;
    f.window.left = 10;
    f.window.top = 20;
    f.window.right = 3840; // 10 + 3830
    f.window.bottom = 2160; // 20 + 2140
    f.window.clientWidth = 3826;
    f.window.clientHeight = 2136;
    f.swapWidth = 3840;
    f.swapHeight = 2160;
    f.fg = FgSnapshot { true, false, true };
    f.fgOffConfirmed = true;
    f.dlssgOutput = true;
    f.exclusive = false;
    f.prevExclusive = 1;
    f.forceBorderless = true;
    f.ownerThread = 4400;
    f.currentThread = 4400;
    f.presentThread = 4400;
    return f;
}

// The negative fixture: every flag the other way round, negative window origin, three different threads.
ProbeFields Negative()
{
    ProbeFields f;
    f.event = "resize-done";
    f.mode = Mode::On;
    f.hwnd = 0x10;
    f.window.valid = true;
    f.window.style = 0x90000000u;
    f.window.exstyle = 0x00040000u;
    f.window.left = -5;
    f.window.top = -7;
    f.window.right = 1915; // -5 + 1920
    f.window.bottom = 1073; // -7 + 1080
    f.window.clientWidth = 1918;
    f.window.clientHeight = 1076;
    f.swapWidth = 1920;
    f.swapHeight = 1080;
    f.fg = FgSnapshot { false, false, false };
    f.fgOffConfirmed = false;
    f.dlssgOutput = false;
    f.exclusive = true;
    f.prevExclusive = 0;
    f.forceBorderless = false;
    f.ownerThread = 1;
    f.currentThread = 2;
    f.presentThread = 3;
    f.fixArmed = true;
    f.nested = true;
    return f;
}
} // namespace

int main()
{
    // A resize-done record after the leave: the border bug in numbers, every field in its place.
    {
        const std::string line = FormatProbeLine(Positive());
        CHECK(line.find('\n') == std::string::npos); // one line
        CHECK_LINE(line,
                   "Window: resize-done mode=probe hwnd=0xa0b0c style=0x16cf0000 exstyle=0x00000200 window=10,20 3830x2140 "
                   "client=3826x2136 swapchain=3840x2160 fg=paused fgOffConfirmed=yes output=DLSS-G exclusive=0 "
                   "prevExclusive=1 forceBorderless=1 ownerTid=4400 currentTid=4400 presentTid=4400 sameThread=yes "
                   "fixArmed=no");
    }

    // The negative branches, all in one record: no frame generation object, not confirmed, other output, exclusive, no
    // ForceBorderless, three different threads, armed fix, nested.
    {
        ProbeFields f = Negative();
        CHECK_LINE(FormatProbeLine(f),
                   "Window: resize-done mode=on hwnd=0x10 style=0x90000000 exstyle=0x00040000 window=-5,-7 1920x1080 "
                   "client=1918x1076 swapchain=1920x1080 fg=none fgOffConfirmed=no output=other exclusive=1 prevExclusive=0 "
                   "forceBorderless=0 ownerTid=1 currentTid=2 presentTid=3 sameThread=no fixArmed=yes "
                   "nested=yes(ran inside a window call of this module)");
        // the other frame generation states, same line otherwise
        f.fg = FgSnapshot { true, false, false };
        CHECK_LINE(FormatProbeLine(f),
                   "Window: resize-done mode=on hwnd=0x10 style=0x90000000 exstyle=0x00040000 window=-5,-7 1920x1080 "
                   "client=1918x1076 swapchain=1920x1080 fg=off fgOffConfirmed=no output=other exclusive=1 prevExclusive=0 "
                   "forceBorderless=0 ownerTid=1 currentTid=2 presentTid=3 sameThread=no fixArmed=yes "
                   "nested=yes(ran inside a window call of this module)");
        f.fg = FgSnapshot { true, true, false };
        CHECK(FormatProbeLine(f).find(" fg=active ") != std::string::npos);
        f.fg = FgSnapshot { true, false, true };
        CHECK(FormatProbeLine(f).find(" fg=paused ") != std::string::npos);
    }

    // An unreadable window, an unknown swapchain size and an owner thread that differs from the current thread are visible; a
    // previous exclusive flag of -1 (never seen) is not printed.
    {
        ProbeFields f;
        f.event = "resize-done";
        f.mode = Mode::On;
        f.hwnd = 0x1;
        f.ownerThread = 5;
        f.currentThread = 6;
        f.presentThread = 6;
        f.nested = true;
        f.fixArmed = true;
        CHECK_LINE(FormatProbeLine(f),
                   "Window: resize-done mode=on hwnd=0x1 window=unreadable swapchain=unknown fg=none fgOffConfirmed=no "
                   "output=other exclusive=0 forceBorderless=0 ownerTid=5 currentTid=6 presentTid=6 sameThread=no "
                   "fixArmed=yes nested=yes(ran inside a window call of this module)");
    }

    // A set-fullscreen-state record carries the call, the three swapchain pointers and what the handler returns. Here ReShade
    // passed the FG swapchain, not the bridge: native equals fgswapchain and sameObject is no.
    {
        ProbeFields f = Positive();
        f.event = "set-fullscreen-state";
        f.hasCall = true;
        f.callFullscreen = true;
        f.callTarget = 0x77;
        f.nativeSwapchain = 0x1000;
        f.bridgeSwapchain = 0x2000;
        f.fgSwapchain = 0x1000;
        f.verdict = DeferVerdict::Defer;
        f.deferState = "pending";
        f.wouldReturn = true;
        f.returned = false;
        CHECK_LINE(FormatProbeLine(f),
                   "Window: set-fullscreen-state mode=probe hwnd=0xa0b0c style=0x16cf0000 exstyle=0x00000200 window=10,20 "
                   "3830x2140 client=3826x2136 swapchain=3840x2160 fg=paused fgOffConfirmed=yes output=DLSS-G exclusive=0 "
                   "prevExclusive=1 forceBorderless=1 ownerTid=4400 currentTid=4400 presentTid=4400 sameThread=yes "
                   "fixArmed=no call=SetFullscreenState(TRUE) target=0x77 native=0x1000 bridge=0x2000 fgswapchain=0x1000 "
                   "sameObject=no state=pending verdict=defer wouldReturn=true returned=false");
    }

    // The same object: native equals the bridge; an unknown FG swapchain prints 0x0.
    {
        ProbeFields f = Positive();
        f.event = "set-fullscreen-state";
        f.hasCall = true;
        f.callFullscreen = false;
        f.callTarget = 0;
        f.nativeSwapchain = 0x1000;
        f.bridgeSwapchain = 0x1000;
        f.fgSwapchain = 0;
        f.verdict = DeferVerdict::UnknownSwapchain;
        f.deferState = "idle";
        f.wouldReturn = false;
        f.returned = false;
        const std::string line = FormatProbeLine(f);
        CHECK_CONTAINS(line, " call=SetFullscreenState(FALSE) target=0x0 native=0x1000 bridge=0x1000 fgswapchain=0x0 sameObject=yes "
                             "state=idle verdict=pass(not the bridge swapchain) wouldReturn=false returned=false");
        CHECK(line.rfind("Window: set-fullscreen-state ", 0) == 0); // prefix and event first
        // An unknown native (0) never counts as the same object even if the bridge is unknown too.
        f.nativeSwapchain = 0;
        f.bridgeSwapchain = 0;
        CHECK_CONTAINS(FormatProbeLine(f), "native=0x0 bridge=0x0 fgswapchain=0x0 sameObject=no");
    }

    // Probe mode with the other verdicts: the names are the ones the in-game session greps for.
    {
        ProbeFields f = Positive();
        f.hasCall = true;
        f.verdict = DeferVerdict::SizeMismatch;
        CHECK_CONTAINS(FormatProbeLine(f), "verdict=pass(swapchain size differs from window)");
        f.verdict = DeferVerdict::TargetDiffers;
        CHECK_CONTAINS(FormatProbeLine(f), "verdict=pass(target output is not the window monitor)");
        f.verdict = DeferVerdict::NoWindowChange;
        CHECK_CONTAINS(FormatProbeLine(f), "verdict=pass(no window change)");
    }

    return wt::g_failed ? 1 : 0;
}
