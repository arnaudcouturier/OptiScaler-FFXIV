// Step 3 on fakes (B2-a): a window-mode call made while DLSS-G frame generation is running is held back, frame generation is
// asked off, and the call is replayed from the end of a game present once DLSS-G is confirmed off. The replay never runs
// with frame generation active (before the hard limit), a repeated request keeps only the latest, and everything that is
// not that exact case passes through untouched.
#include "window_test.h"

using namespace ext::window;
using wt::FakeEnv;

namespace
{
constexpr uint64_t kBridge = 0x1000;
constexpr uint64_t kHwnd = 0xABC;

const FgSnapshot kActive { true, true, false };
const FgSnapshot kPaused { true, false, true };
const FgSnapshot kOff { true, false, false };

FsEvent Event(bool fullscreen, uint64_t native = kBridge)
{
    FsEvent ev;
    ev.native = native;
    ev.hwnd = kHwnd;
    ev.fullscreen = fullscreen;
    ev.target = 0;
    return ev;
}

void Present(Controller& c, FakeEnv& env, Mode mode, const FgSnapshot& entry, const FgSnapshot& exit)
{
    env.fg = entry;
    c.OnPresentBegin(env, mode);
    env.fg = exit;
    c.OnPresentEnd(env, mode);
}

// The state the crash happened in: DLSS-G frame generation running, one thread, the bridge known, pseudo fullscreen off.
void Running(Controller& c, FakeEnv& env)
{
    env.fg = kActive;
    Present(c, env, Mode::On, kActive, kActive); // the present thread is recorded
    c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160); // the bridge is recorded
    env.fg = kActive;
}

int Warnings(const FakeEnv& env)
{
    int n = 0;
    for (bool w : env.warns)
        n += w ? 1 : 0;
    return n;
}

bool AnyLogContains(const FakeEnv& env, const char* text)
{
    for (const auto& l : env.logs)
        if (l.find(text) != std::string::npos)
            return true;
    return false;
}
} // namespace

int main()
{
    // The case the fix is for: enter with frame generation on. Held, frame generation asked off, replayed after one complete
    // present with it off; the handler returned true (prevent) for the held call.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        const bool prevented = c.OnSetFullscreenState(env, Mode::On, Event(true));
        CHECK(prevented);
        CHECK(env.fgOffRequests == 1);
        CHECK(env.retained == 1 && env.released == 0);
        CHECK(env.replays.empty());
        CHECK_CONTAINS(env.Last(), "held back");
        CHECK_CONTAINS(env.Last(), "handler returns true (prevent)");
        CHECK(AnyLogContains(env, "verdict=defer wouldReturn=true returned=true"));

        // The present in progress when it went off, and the next one: first does not confirm...
        Present(c, env, Mode::On, kActive, kPaused);
        CHECK(env.replays.empty());
        // ...the next complete present with the feature off does.
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1);
        CHECK(env.replays[0].native == kBridge);
        CHECK(env.replays[0].fullscreen);
        CHECK(!env.replays[0].fgActiveAtCall);
        CHECK(env.replays[0].thread == 100);
        CHECK(env.released == 1); // the reference taken for the held call is given back
        CHECK_CONTAINS(env.All(), "replaying SetFullscreenState(TRUE) with a null target: frame generation confirmed off");
        CHECK_CONTAINS(env.All(), "replayed SetFullscreenState(TRUE) returned 0x00000000");
        CHECK(Warnings(env) == 0);

        // Idle again: nothing more is replayed.
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1);
    }

    // The replay passes through the handler untouched, and a resize inside it is flagged as nested.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        bool replayPassed = false;
        env.onReplay = [&]
        {
            replayPassed = !c.OnSetFullscreenState(env, Mode::On, Event(true)); // ReShade would call the handler again
            env.exclusive = true;                                              // the hook flips SCExclusiveFullscreen
            c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);                     // and the game resizes
        };
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1);
        CHECK(replayPassed);
        CHECK(env.retained == 1 && env.released == 1);
        CHECK(env.fgOffRequests == 1); // the replay was not deferred again
        CHECK(AnyLogContains(env, "state=replaying"));
        CHECK(AnyLogContains(env, "nested=yes"));
    }

    // Everything else passes untouched and holds nothing.
    {
        // frame generation already off
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.fg = kOff;
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.fg = kPaused;
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true)));
        // a re-assertion of the state in effect (the frequent harmless call)
        env.fg = kActive;
        env.exclusive = true;
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.exclusive = false;
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(false)));
        // not ForceBorderless
        env.forceBorderless = false;
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.forceBorderless = true;
        // not the DLSS-G output
        env.dlssg = false;
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.dlssg = true;
        // the window belongs to another thread / unknown
        env.owner = 5;
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.owner = 0;
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.owner = 100;
        // another swapchain than the bridge
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true, 0x2000)));
        // the call comes from another thread than the one that presents
        env.current = 7;
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.current = 100;

        CHECK(env.fgOffRequests == 0 && env.retained == 0 && env.replays.empty());
        // And with everything right again it is held, so the cases above were each decisive.
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        CHECK(env.fgOffRequests == 1 && env.retained == 1);
    }

    // H1: the FG swapchain is not the window size (the border leftover: 3836x2156 in a 3840x2160 window). The game follow-up
    // ResizeBuffers would be a real resize and flush Streamline unprotected, so nothing is held: the call passes, with a warning,
    // and the probe line shows both sizes. Once the sizes agree the same call is held.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3836, 2156);
        env.fg = kActive;
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true)));
        CHECK(env.fgOffRequests == 0 && env.retained == 0);
        CHECK(AnyLogContains(env, "swapchain=3836x2156"));
        CHECK(AnyLogContains(env, "client=3840x2160"));
        CHECK(AnyLogContains(env, "verdict=pass(swapchain size differs from window)"));
        CHECK(AnyLogContains(env, "the follow-up ResizeBuffers is not protected"));
        CHECK(Warnings(env) == 1);
        // Probe mode logs the same and still holds nothing.
        CHECK(!c.OnSetFullscreenState(env, Mode::Probe, Event(true)));
        // An unknown swapchain size (the desc could not be read) is also a pass.
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 0, 0);
        env.fg = kActive;
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true)));
        // Sizes agree again: held.
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        env.fg = kActive;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        CHECK(AnyLogContains(env, "swapchain=3840x2160"));
    }

    // Probe never holds or prevents; off does nothing.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        CHECK(!c.OnSetFullscreenState(env, Mode::Probe, Event(true)));
        CHECK(AnyLogContains(env, "verdict=defer wouldReturn=true returned=false"));
        CHECK(env.fgOffRequests == 0 && env.retained == 0);
        for (int i = 0; i < 40; ++i)
            Present(c, env, Mode::Probe, kActive, kActive);
        CHECK(env.replays.empty());
        const size_t logs = env.logs.size();
        CHECK(!c.OnSetFullscreenState(env, Mode::Off, Event(true)));
        CHECK(env.logs.size() == logs && env.fgOffRequests == 0);
    }

    // Asking frame generation off resets the gate: a count from before does not stand in for the present that must follow.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        Present(c, env, Mode::On, kOff, kOff); // a confirmed off period...
        env.fg = kActive;                      // ...then frame generation is activated between presents
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true))); // replaces; its record shows the gate
        CHECK_CONTAINS(env.logs[env.logs.size() - 2], "fgOffConfirmed=no");
    }

    // A second request while one is held keeps only the latest, and is held too.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true))); // same direction: replaces, still prevented
        CHECK(env.fgOffRequests == 1 && env.retained == 1); // not asked or retained twice
        CHECK_CONTAINS(env.All(), "replaces the held call");
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1); // one replay, not two
        CHECK_CONTAINS(env.All(), "1 earlier call(s) replaced");
    }

    // The latest request asks for the state already in effect: the held call is dropped, nothing is replayed, this call passes.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.exclusive = false;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));   // enter held
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(false))); // back to windowed = the state in effect
        CHECK(env.released == 1); // the held reference is given back
        CHECK_CONTAINS(env.All(), "held call dropped");
        for (int i = 0; i < 10; ++i)
            Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.empty());
        // A later real change is deferred as usual (the machine is Idle again).
        env.fg = kActive;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        CHECK(env.retained == 2);
    }

    // Repeated identical requests and the other direction while held.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.exclusive = true;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(false))); // leave held
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true))); // equals the state in effect (exclusive): dropped
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.empty());
    }

    // A request for another swapchain while one is held passes and leaves the held call alone.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true, 0x2000)));
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1 && env.replays[0].native == kBridge);
    }

    // Frame generation comes back during the wait (the pause is about ten frames). The replay never runs while it is active,
    // even past the soft limit; each present that finds it active asks it off again; the first off present replays.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        for (int i = 0; i < 12; ++i) // soft limit is 6 presents
        {
            Present(c, env, Mode::On, kActive, kActive);
            CHECK(env.replays.empty());
        }
        CHECK(env.fgOffRequests == 1 + 12);
        CHECK(AnyLogContains(env, "frame generation came back while a call is held"));
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1);
        for (const auto& r : env.replays)
            CHECK(!r.fgActiveAtCall);
    }

    // The hard limit replays regardless (fail open), loudly.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.autoOff = false; // frame generation cannot be turned off
        for (int i = 0; i < 23; ++i)
            Present(c, env, Mode::On, kActive, kActive);
        CHECK(env.replays.empty());
        Present(c, env, Mode::On, kActive, kActive);
        CHECK(env.replays.size() == 1);
        CHECK(env.replays[0].fgActiveAtCall); // documented: this is the fail-open case
        CHECK_CONTAINS(env.All(), "hard wait limit reached with frame generation still on");
        CHECK(Warnings(env) >= 1);
        CHECK(env.released == 1);
    }

    // The soft limit by presents: frame generation is off at the end of each present but never off for a complete one.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        for (int i = 0; i < 5; ++i)
            Present(c, env, Mode::On, kActive, kPaused); // starts active: never a complete off present
        CHECK(env.replays.empty());
        Present(c, env, Mode::On, kActive, kPaused);
        CHECK(env.replays.size() == 1);
        CHECK(!env.replays[0].fgActiveAtCall);
        CHECK_CONTAINS(env.All(), "wait limit reached");
    }

    // The soft limit by time (a fake clock): a slow frame ends the wait.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 10000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.nowMs = 11000;
        Present(c, env, Mode::On, kActive, kPaused);
        CHECK(env.replays.empty());
        env.nowMs = 12000; // 2 s
        Present(c, env, Mode::On, kActive, kPaused);
        CHECK(env.replays.size() == 1);
        CHECK_CONTAINS(env.All(), "2000 ms");
    }

    // The replay is made on the thread that made the call: another presenting thread leaves the call held until the hard limit.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.current = 7;
        for (int i = 0; i < 20; ++i)
            Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.empty());
        for (int i = 0; i < 4; ++i)
            Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1); // hard limit, fail open
        CHECK(env.replays[0].thread == 7);
        CHECK(Warnings(env) >= 1);
    }

    // A failing replay is reported and the machine is free again.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.replayResult = static_cast<int32_t>(0x887A0001);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1);
        CHECK_CONTAINS(env.All(), "returned 0x887a0001");
        CHECK(Warnings(env) == 1);
        env.fg = kActive;
        env.replayResult = 0;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true))); // held again
        CHECK(env.retained == 2);
    }

    // End to end, leave with frame generation on: held, replayed once it is off (the hook then flips the flag and restyles),
    // the flag change arms the border fix, and the fix is applied only after frame generation is confirmed off again. The
    // pseudo fullscreen was entered at startup with no resize behind it (the logged startup enter has none), so only a
    // present has seen the flag true. Once with the game resizing after the leave, once without.
    for (const bool gameResizes : { true, false })
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.exclusive = true; // the pseudo fullscreen is in effect; a present has sampled it, no resize has
        Present(c, env, Mode::On, kActive, kActive);
        env.fg = kActive;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(false)));
        env.onReplay = [&]
        {
            // upstream's hook: SCExclusiveFullscreen true -> false, overlapped style plus WS_EX_OVERLAPPEDWINDOW; the game then
            // writes its own frame-less style (and maybe resizes).
            env.exclusive = false;
            env.window.style = 0x90000000u;
            env.window.exstyle = 0x300u;
            env.window.clientWidth = 3836;
            env.window.clientHeight = 2156;
            if (gameResizes)
                c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        };
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1 && !env.replays[0].fgActiveAtCall);
        CHECK(c.FixArmed() == gameResizes); // without a resize the next present sampling arms it
        CHECK(env.applied.empty());
        // The pause ends: frame generation comes back before the fix had a complete off present.
        Present(c, env, Mode::On, kPaused, kActive);
        Present(c, env, Mode::On, kActive, kActive);
        CHECK(env.applied.empty());
        // Off again: the fix goes in.
        Present(c, env, Mode::On, kActive, kPaused);
        CHECK(env.applied.empty());
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.applied.size() == 1);
        CHECK(!env.applied[0].fgActiveAtCall);
        CHECK(env.window.clientWidth == 3840 && env.window.clientHeight == 2160);
    }

    return wt::g_failed ? 1 : 0;
}
