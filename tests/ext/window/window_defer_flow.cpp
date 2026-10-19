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
    if (!env.serial)
        env.serial = 0; // these flows run with the bridge present counter live (M2): every present here reaches the FG present
    env.fg = entry;
    c.OnPresentBegin(env, mode);
    env.CompletePresent();
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
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
        Running(c, env);
        const bool prevented = c.OnSetFullscreenState(env, Mode::On, Event(true));
        CHECK(prevented);
        CHECK(env.fgOffRequests == 1);
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
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
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
        CHECK(env.fgOffRequests == 1); // the replay was not deferred again
        CHECK(AnyLogContains(env, "state=replaying"));
        CHECK(AnyLogContains(env, "nested=yes"));
    }

    // Everything else passes untouched and holds nothing.
    {
        // frame generation already off
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
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

        CHECK(env.fgOffRequests == 0 && env.replays.empty());
        // And with everything right again it is held, so the cases above were each decisive.
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        CHECK(env.fgOffRequests == 1);
    }

    // H1: the FG swapchain is not the window size (the border leftover: 3836x2156 in a 3840x2160 window). The game follow-up
    // ResizeBuffers would be a real resize and flush Streamline unprotected, so nothing is held: the call passes, with a warning,
    // and the probe line shows both sizes. Once the sizes agree the same call is held.
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
        Running(c, env);
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3836, 2156);
        env.fg = kActive;
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true)));
        CHECK(env.fgOffRequests == 0);
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

    // M1: a held call belongs to its swapchain. Another bridge (device recreation) or the destruction of this one drops it;
    // nothing is replayed on the old pointer and no extra reference exists to leak.
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        c.OnResizeDone(env, Mode::On, 0x2000, kHwnd, 3840, 2160); // a new bridge
        CHECK_CONTAINS(env.All(), "held SetFullscreenState dropped, not replayed: the bridge swapchain changed");
        for (int i = 0; i < 30; ++i)
            Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.empty());
        // The machine is free: a new real change on the new bridge is held again.
        env.fg = kActive;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true, 0x2000)));
    }
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        c.OnBridgeDestroyed(env, Mode::On, 0x9999); // some other swapchain: the hold stays
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1);
    }
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        c.OnBridgeDestroyed(env, Mode::On, kBridge); // the bridge is torn down while the call is held
        CHECK_CONTAINS(env.All(), "the bridge swapchain is being destroyed");
        for (int i = 0; i < 30; ++i)
            Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.empty());
        // The destroyed bridge is forgotten: the same pointer is unknown until a resize reports a bridge again.
        env.fg = kActive;
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true)));
        // Off and probe ignore the destruction.
        c.OnBridgeDestroyed(env, Mode::Off, kBridge);
    }

    // M3, with the production defaults: the replay needs 2 complete off presents AND 50 ms since the request. One present is
    // not enough, a confirmed gate in the first present after the request does not replay, and 2 presents in 20 ms do not.
    {
        FakeEnv env;
        Controller c; // defaults
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.nowMs = 1030;
        Present(c, env, Mode::On, kPaused, kPaused); // 1 complete off present, 30 ms
        CHECK(env.replays.empty());
        env.nowMs = 1040;
        Present(c, env, Mode::On, kPaused, kPaused); // 2 presents but only 40 ms
        CHECK(env.replays.empty());
        env.nowMs = 1050;
        Present(c, env, Mode::On, kPaused, kPaused); // 3 presents, 50 ms: replay
        CHECK(env.replays.size() == 1);
        CHECK_CONTAINS(env.All(), "ms,"); // the elapsed time is in the replaying line
    }
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.nowMs = 1500; // plenty of time, but only one complete present
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.empty());
        env.nowMs = 1510;
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1);
    }

    // M6: at the hard limit on a thread that does not own the window the hold is dropped with a warning, never replayed there.
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0);
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.autoOff = false;
        env.owner = 5; // the presenting thread is not the window thread
        for (int i = 0; i < 24; ++i)
            Present(c, env, Mode::On, kActive, kActive);
        CHECK(env.replays.empty());
        CHECK_CONTAINS(env.All(), "does not own the window");
        CHECK(Warnings(env) >= 1);
        env.owner = 100;
        env.fg = kActive;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true))); // the machine is free again
    }

    // L1: a second call from another thread while one is held passes; the held call is still replayed on its own thread.
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0);
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.current = 7;
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(true)));
        CHECK_CONTAINS(env.All(), "threads differ: passes");
        env.current = 100;
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1);
    }

    // L2: a present that runs inside the replay's own window call must not run the border fix there.
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0);
        c.SetFixConfirm(1, 0); // the floor would stop the fix anyway: relaxed, so only the guard under test can stop it
        Running(c, env);
        env.exclusive = true;
        Present(c, env, Mode::On, kActive, kActive);
        env.fg = kActive;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(false)));
        env.onReplay = [&]
        {
            env.exclusive = false;
            env.window.style = 0x90000000u;
            env.window.exstyle = 0x300u;
            Present(c, env, Mode::On, kPaused, kPaused); // the game presents from the window procedure
        };
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1);
        CHECK(env.applied.empty());
    }

    // L4: a log that throws during the replay must not leave the machine stuck in Replaying.
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0);
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.throwOn = "replaying SetFullscreenState";
        bool threw = false;
        try
        {
            Present(c, env, Mode::On, kPaused, kPaused);
        }
        catch (...)
        {
            threw = true;
        }
        CHECK(threw);
        env.throwOn.clear();
        env.fg = kActive;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true))); // held again: not stuck
    }

    // M4: the event target. 0 and the window's own monitor are held; any other output passes (the replay drops the target).
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0);
        Running(c, env);
        FsEvent ev = Event(true);
        ev.target = 0x99; // some other output
        CHECK(!c.OnSetFullscreenState(env, Mode::On, ev));
        CHECK(AnyLogContains(env, "target=0x99"));
        CHECK(AnyLogContains(env, "verdict=pass(target output is not the window monitor)"));
        CHECK(env.fgOffRequests == 0);
        ev.target = 0x77; // the window's monitor (FakeEnv::monitor)
        CHECK(c.OnSetFullscreenState(env, Mode::On, ev));
        c.OnBridgeDestroyed(env, Mode::On, kBridge);
    }

    // L1: a log (or the frame generation request) that throws while a call is being held must leave nothing held: the entry
    // point then returns false and the game's call passes, and a hold left behind would replay the same call a second time.
    // Nothing is asked of frame generation when a line throws before the state change.
    for (const char* needle : { "set-fullscreen-state", "held back" })
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0);
        Running(c, env);
        env.throwOn = needle;
        bool threw = false;
        try
        {
            c.OnSetFullscreenState(env, Mode::On, Event(true));
        }
        catch (...)
        {
            threw = true;
        }
        CHECK(threw);
        CHECK(env.fgOffRequests == 0);
        env.throwOn.clear();
        for (int i = 0; i < 30; ++i)
            Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.empty()); // nothing was held, so nothing is replayed
        env.fg = kActive;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true))); // and the next call is held normally
        CHECK(env.fgOffRequests == 1);
    }
    {
        // the frame generation request itself throws: the hold is cancelled
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0);
        Running(c, env);
        env.onRequestOff = [&] { throw std::runtime_error("request"); };
        bool threw = false;
        try
        {
            c.OnSetFullscreenState(env, Mode::On, Event(true));
        }
        catch (...)
        {
            threw = true;
        }
        CHECK(threw);
        env.onRequestOff = nullptr;
        for (int i = 0; i < 30; ++i)
            Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.empty());
        env.fg = kActive;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
    }
    {
        // a throwing line while a held call is replaced leaves the held call as it was
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0);
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.throwOn = "replaces the held call";
        bool threw = false;
        try
        {
            c.OnSetFullscreenState(env, Mode::On, Event(true));
        }
        catch (...)
        {
            threw = true;
        }
        CHECK(threw);
        env.throwOn.clear();
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1);
        CHECK_CONTAINS(env.All(), "0 earlier call(s) replaced");
    }

    // L3: "nested=yes" means a resize that runs inside a window call of this module ON THE SAME THREAD. The live module gives up
    // its lock around the calls out, so another thread's resize that arrives meanwhile is not nested.
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0);
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.onReplay = [&]
        {
            env.current = 7; // another thread's bridge resize while the replay call is out
            c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
            env.current = 100;
        };
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1);
        for (const auto& l : env.logs)
            CHECK(l.find("nested=yes") == std::string::npos);
    }
    // Probe never holds or prevents; off does nothing.
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
        Running(c, env);
        CHECK(!c.OnSetFullscreenState(env, Mode::Probe, Event(true)));
        CHECK(AnyLogContains(env, "verdict=defer wouldReturn=true returned=false"));
        CHECK(env.fgOffRequests == 0);
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
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
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
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true))); // same direction: replaces, still prevented
        CHECK(env.fgOffRequests == 1); // not asked or retained twice
        CHECK_CONTAINS(env.All(), "replaces the held call");
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.size() == 1); // one replay, not two
        CHECK_CONTAINS(env.All(), "1 earlier call(s) replaced");
    }

    // The latest request asks for the state already in effect: the held call is dropped, nothing is replayed, this call passes.
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
        Running(c, env);
        env.exclusive = false;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));   // enter held
        CHECK(!c.OnSetFullscreenState(env, Mode::On, Event(false))); // back to windowed = the state in effect
        CHECK_CONTAINS(env.All(), "held call dropped");
        for (int i = 0; i < 10; ++i)
            Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.empty());
        // A later real change is deferred as usual (the machine is Idle again).
        env.fg = kActive;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
    }

    // Repeated identical requests and the other direction while held.
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
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
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
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
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
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
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.autoOff = false; // frame generation cannot be turned off
        for (int i = 0; i < 23; ++i)
            Present(c, env, Mode::On, kActive, kActive);
        CHECK(env.replays.empty());
        Present(c, env, Mode::On, kActive, kActive);
        CHECK(env.replays.size() == 1);
        CHECK(env.replays[0].fgActiveAtCall); // documented: this is the fail-open case
        CHECK_CONTAINS(env.All(), "hard wait limit reached without a confirmation");
        CHECK_CONTAINS(env.All(), "fg=active"); // the replay line names the frame generation state
        CHECK(Warnings(env) >= 1);
    }

    // L4: the request bookkeeping (request time, gate, "a present reached Streamline since the request") is settled BEFORE the
    // call out, which gives up the module lock in the live build, and cleared again AFTER it: a present that ends inside the
    // call is not ordered against the request and counts for nothing.
    {
        FakeEnv env;
        Controller c; // strict defaults
        Running(c, env);
        env.nowMs = 500;
        Present(c, env, Mode::On, kOff, kOff);
        Present(c, env, Mode::On, kOff, kOff);
        CHECK(c.Gate().Count() == 2); // a count to lose
        env.fg = kActive;
        uint32_t gateInside = 99;
        env.onRequestOff = [&]
        {
            gateInside = c.Gate().Count();
            Present(c, env, Mode::On, kOff, kOff); // a present ends inside the call
            env.onRequestOff = nullptr;
        };
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        CHECK(gateInside == 0);          // settled before the call
        CHECK(c.Gate().Count() == 0);    // and what the call raced is forgotten
        for (int i = 0; i < 10; ++i)
        {
            env.nowMs += 100; // the 50 ms floor is long over; presents that never reach Streamline
            env.fg = kOff;
            c.OnPresentBegin(env, Mode::On);
            c.OnPresentEnd(env, Mode::On);
            CHECK(env.replays.empty()); // no soft replay: the present inside the call does not count as "since the request"
        }
    }
    // The soft limit by presents: frame generation is off at the end of each present but never off for a complete one.
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
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
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
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
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
        Running(c, env);
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.current = 7;
        for (int i = 0; i < 20; ++i)
            Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.empty());
        for (int i = 0; i < 4; ++i)
            Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.replays.empty()); // M6: the hard limit on a thread that does not own the window drops the hold, no replay
        CHECK_CONTAINS(env.All(), "does not own the window");
        CHECK(Warnings(env) >= 1);
    }

    // A failing replay is reported and the machine is free again.
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
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
    }

    // End to end, leave with frame generation on: held, replayed once it is off (the hook then flips the flag and restyles),
    // the flag change arms the border fix, and the fix is applied only after frame generation is confirmed off again. The
    // pseudo fullscreen was entered at startup with no resize behind it (the logged startup enter has none), so only a
    // present has seen the flag true. Once with the game resizing after the leave, once without.
    for (const bool gameResizes : { true, false })
    {
        FakeEnv env;
        Controller c;
        c.SetReplayConfirm(1, 0); // these cases are about other rules; the strict default has its own cases below
        c.SetFixConfirm(1, 0);
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
