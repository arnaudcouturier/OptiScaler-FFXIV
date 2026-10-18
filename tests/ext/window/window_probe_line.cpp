// The probe record: one line, prefix "Window:", every field the findings ask for (style, exstyle, rects, frame generation
// state, SCExclusiveFullscreen, ForceBorderless, owner thread against the current thread, the handler result).
#include "window_test.h"

using namespace ext::window;

int main()
{
    // A resize-done record after the leave: the 3836x2156 client of the border bug inside a 3840x2160 window.
    {
        ProbeFields f;
        f.event = "resize-done";
        f.mode = Mode::Probe;
        f.hwnd = 0xA0B0C;
        f.window.valid = true;
        f.window.style = 0x16CF0000u;
        f.window.exstyle = 0x00000200u;
        f.window.left = 0;
        f.window.top = 0;
        f.window.right = 3840;
        f.window.bottom = 2160;
        f.window.clientWidth = 3836;
        f.window.clientHeight = 2156;
        f.fg = FgSnapshot { true, false, true };
        f.fgOffConfirmed = true;
        f.dlssgOutput = true;
        f.exclusive = false;
        f.prevExclusive = 1;
        f.forceBorderless = true;
        f.ownerThread = 4400;
        f.currentThread = 4400;
        f.presentThread = 4400;

        const std::string line = FormatProbeLine(f);
        CHECK(line.rfind("Window: resize-done ", 0) == 0); // prefix and event first
        CHECK(line.find('\n') == std::string::npos);      // one line
        CHECK_CONTAINS(line, "mode=probe");
        CHECK_CONTAINS(line, "hwnd=0xa0b0c");
        CHECK_CONTAINS(line, "style=0x16cf0000");
        CHECK_CONTAINS(line, "exstyle=0x00000200");
        CHECK_CONTAINS(line, "window=0,0 3840x2160");
        CHECK_CONTAINS(line, "client=3836x2156");
        CHECK_CONTAINS(line, "fg=paused");
        CHECK_CONTAINS(line, "fgOffConfirmed=yes");
        CHECK_CONTAINS(line, "output=DLSS-G");
        CHECK_CONTAINS(line, "exclusive=0");
        CHECK_CONTAINS(line, "prevExclusive=1");
        CHECK_CONTAINS(line, "forceBorderless=1");
        CHECK_CONTAINS(line, "ownerTid=4400 currentTid=4400 presentTid=4400 sameThread=yes");
        CHECK_CONTAINS(line, "fixArmed=no");
        CHECK(line.find("call=") == std::string::npos); // no call part on a resize record
        CHECK(line.find("nested") == std::string::npos);
    }

    // An unreadable window and an owner thread that differs from the current thread are visible.
    {
        ProbeFields f;
        f.event = "resize-done";
        f.mode = Mode::On;
        f.ownerThread = 5;
        f.currentThread = 6;
        f.presentThread = 6;
        f.nested = true;
        f.fixArmed = true;
        const std::string line = FormatProbeLine(f);
        CHECK_CONTAINS(line, "window=unreadable");
        CHECK_CONTAINS(line, "sameThread=no");
        CHECK_CONTAINS(line, "fixArmed=yes");
        CHECK_CONTAINS(line, "nested=yes");
        CHECK(line.find("prevExclusive") == std::string::npos); // -1 = not seen before, not printed
    }

    // A set-fullscreen-state record carries the call and what the handler returns.
    {
        ProbeFields f;
        f.event = "set-fullscreen-state";
        f.mode = Mode::On;
        f.hasCall = true;
        f.callFullscreen = true;
        f.callTarget = 0;
        f.nativeSwapchain = 0x1000;
        f.bridgeSwapchain = 0x1000;
        f.verdict = DeferVerdict::Defer;
        f.deferState = "idle";
        f.wouldReturn = true;
        f.returned = true;
        const std::string line = FormatProbeLine(f);
        CHECK(line.rfind("Window: set-fullscreen-state ", 0) == 0);
        CHECK_CONTAINS(line, "call=SetFullscreenState(TRUE)");
        CHECK_CONTAINS(line, "target=0x0");
        CHECK_CONTAINS(line, "native=0x1000 bridge=0x1000 sameObject=yes");
        CHECK_CONTAINS(line, "state=idle");
        CHECK_CONTAINS(line, "verdict=defer");
        CHECK_CONTAINS(line, "wouldReturn=true returned=true");
    }

    // Probe mode: the verdict says defer, the handler returned false. A different swapchain pointer is not "the same object".
    {
        ProbeFields f;
        f.event = "set-fullscreen-state";
        f.mode = Mode::Probe;
        f.hasCall = true;
        f.callFullscreen = false;
        f.nativeSwapchain = 0x2000;
        f.bridgeSwapchain = 0x1000;
        f.verdict = DeferVerdict::UnknownSwapchain;
        f.wouldReturn = false;
        f.returned = false;
        const std::string line = FormatProbeLine(f);
        CHECK_CONTAINS(line, "SetFullscreenState(FALSE)");
        CHECK_CONTAINS(line, "sameObject=no");
        CHECK_CONTAINS(line, "verdict=pass(not the bridge swapchain)");
        CHECK_CONTAINS(line, "wouldReturn=false returned=false");
        // An unknown native (0) never counts as the same object even if the bridge is unknown too.
        f.nativeSwapchain = 0;
        f.bridgeSwapchain = 0;
        CHECK_CONTAINS(FormatProbeLine(f), "sameObject=no");
    }

    return wt::g_failed ? 1 : 0;
}
