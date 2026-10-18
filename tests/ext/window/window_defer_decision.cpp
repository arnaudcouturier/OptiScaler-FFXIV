// The mode-switch decision: which SetFullscreenState calls are deferred (B2) and which pass through untouched.
#include "window_test.h"

using namespace ext::window;

namespace
{
// The call the fix is for: borderless fork mode, DLSS-G on, frame generation running, one thread, the bridge swapchain,
// a real change (enter "fullscreen" from windowed).
DeferInputs Deferred()
{
    DeferInputs in;
    in.forceBorderless = true;
    in.dlssgOutput = true;
    in.fgActive = true;
    in.exclusiveNow = false;
    in.requestedFullscreen = true;
    in.threadsAgree = true;
    in.swapchainIsBridge = true;
    return in;
}
} // namespace

int main()
{
    CHECK(DecideDefer(Deferred()) == DeferVerdict::Defer);

    // Both directions of a real change are deferred.
    {
        DeferInputs in = Deferred();
        in.exclusiveNow = true;
        in.requestedFullscreen = false;
        CHECK(DecideDefer(in) == DeferVerdict::Defer);
    }

    // A re-assertion of the state in effect passes (29 such calls were logged without harm): both TRUE/TRUE and FALSE/FALSE.
    {
        DeferInputs in = Deferred();
        in.exclusiveNow = true;
        in.requestedFullscreen = true;
        CHECK(DecideDefer(in) == DeferVerdict::NoWindowChange);
        in.exclusiveNow = false;
        in.requestedFullscreen = false;
        CHECK(DecideDefer(in) == DeferVerdict::NoWindowChange);
    }
    CHECK(WouldChangeWindow(true, false) && WouldChangeWindow(false, true));
    CHECK(!WouldChangeWindow(true, true) && !WouldChangeWindow(false, false));

    // Every other condition on its own passes the call through.
    {
        DeferInputs in = Deferred();
        in.forceBorderless = false; // the call reaches Streamline's own guard
        CHECK(DecideDefer(in) == DeferVerdict::NotForceBorderless);
    }
    {
        DeferInputs in = Deferred();
        in.dlssgOutput = false;
        CHECK(DecideDefer(in) == DeferVerdict::NotDlssg);
    }
    {
        DeferInputs in = Deferred();
        in.fgActive = false; // frame generation already off: the call is safe as it is
        CHECK(DecideDefer(in) == DeferVerdict::FgNotActive);
    }
    {
        DeferInputs in = Deferred();
        in.threadsAgree = false;
        CHECK(DecideDefer(in) == DeferVerdict::ThreadMismatch);
    }
    {
        DeferInputs in = Deferred();
        in.swapchainIsBridge = false;
        CHECK(DecideDefer(in) == DeferVerdict::UnknownSwapchain);
    }

    // Defaults are the fail-open state.
    CHECK(DecideDefer(DeferInputs {}) != DeferVerdict::Defer);

    // Threads: owner, call and present thread must be one known thread.
    CHECK(ThreadsAgree(10, 10, 10));
    CHECK(!ThreadsAgree(10, 11, 10));
    CHECK(!ThreadsAgree(10, 10, 11));
    CHECK(!ThreadsAgree(11, 10, 10));
    CHECK(!ThreadsAgree(0, 0, 0));   // unknown is not "equal"
    CHECK(!ThreadsAgree(10, 10, 0)); // no present seen yet

    // Names are distinct and carry the reason.
    CHECK(std::string(VerdictName(DeferVerdict::Defer)) == "defer");
    CHECK(std::string(VerdictName(DeferVerdict::NoWindowChange)).find("no window change") != std::string::npos);
    CHECK(std::string(VerdictName(DeferVerdict::ThreadMismatch)) != VerdictName(DeferVerdict::UnknownSwapchain));

    return wt::g_failed ? 1 : 0;
}
