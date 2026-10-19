// M2 on fakes: a game present counts as complete for the frame-generation-off gate only if it really reached the frame
// generation present. The module reads the bridge's present counter (it advances once per FG present that returned success,
// with frame generation on or off) at the start and at the end of a game present: a present whose counter did not change is
// not counted and the gate starts again. Unknown counters (no bridge, fewer than two buffers) fall back to counting.
#include "window_test.h"

using namespace ext::window;
using wt::FakeEnv;

namespace
{
constexpr uint64_t kBridge = 0x1000;
constexpr uint64_t kOtherBridge = 0x2000;
constexpr uint64_t kHwnd = 0xABC;

const FgSnapshot kActive { true, true, false };
const FgSnapshot kOff { true, false, false };
const FgSnapshot kPaused { true, false, true };

int Warns(const FakeEnv& env)
{
    int n = 0;
    for (bool w : env.warns)
        n += w ? 1 : 0;
    return n;
}

FsEvent Event(bool fullscreen)
{
    FsEvent ev;
    ev.native = kBridge;
    ev.hwnd = kHwnd;
    ev.fullscreen = fullscreen;
    return ev;
}

// One game present: `reaches` = it got as far as the frame generation present (the bridge counter advances).
void Present(Controller& c, FakeEnv& env, Mode mode, const FgSnapshot& entry, const FgSnapshot& exit, bool reaches)
{
    env.fg = entry;
    c.OnPresentBegin(env, mode);
    if (reaches)
        env.CompletePresent();
    env.fg = exit;
    c.OnPresentEnd(env, mode);
}

// The state the crash happened in, with a live counter: frame generation running, one thread, the bridge known.
void Running(Controller& c, FakeEnv& env)
{
    env.serial = 0;
    env.fg = kActive;
    Present(c, env, Mode::On, kActive, kActive, true); // the present thread is recorded
    c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
    env.fg = kActive;
}

int Count(const FakeEnv& env, const char* text)
{
    int n = 0;
    for (const auto& l : env.logs)
        n += l.find(text) != std::string::npos ? 1 : 0;
    return n;
}
} // namespace

int main()
{
    // ---- the pure pieces ----
    static_assert(!PresentSerialFromIndex(0, 0).has_value());
    static_assert(!PresentSerialFromIndex(0, 1).has_value()); // one buffer: the index can never change
    static_assert(PresentSerialFromIndex(1, 2) == std::optional<uint64_t>(1));
    static_assert(PresentSerialFromIndex(2, 3) == std::optional<uint64_t>(2));
    static_assert(PresentReachedFg(0, 1));
    static_assert(PresentReachedFg(2, 0)); // wrapped: changed, not greater
    static_assert(PresentReachedFg(1, 0));
    static_assert(!PresentReachedFg(1, 1));
    static_assert(!PresentReachedFg(0, 0));
    static_assert(PresentReachedFg(std::nullopt, 1)); // no counter at one end: unknown, the present counts
    static_assert(PresentReachedFg(1, std::nullopt));
    static_assert(PresentReachedFg(std::nullopt, std::nullopt));

    // The gate: a present that did not reach the FG present resets the count and is not counted.
    {
        FgOffGate gate;
        gate.OnPresentBegin(kOff);
        gate.OnPresentEnd(kOff, true);
        CHECK(gate.Count() == 1);
        gate.OnPresentBegin(kOff);
        gate.OnPresentEnd(kOff, false);
        CHECK(gate.Count() == 0);
        gate.OnPresentBegin(kOff);
        gate.OnPresentEnd(kOff); // the default is a counted present
        CHECK(gate.Count() == 1);
        gate.OnPresentBegin(kOff);
        gate.OnPresentEnd(kOff, false);
        gate.OnPresentEnd(kOff, true); // an end without a begin since the aborted present counts nothing
        CHECK(gate.Count() == 0);
    }

    // ---- the controller: an aborted present is not counted ----
    {
        FakeEnv env;
        Controller c;
        env.serial = 0;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        Present(c, env, Mode::On, kOff, kOff, false); // returned early, never reached the FG present
        CHECK(c.Gate().Count() == 0);
        CHECK(c.AbortedPresents() == 1);
        CHECK(Count(env, "ended before the frame generation present") == 1);
        Present(c, env, Mode::On, kOff, kOff, true);
        CHECK(c.Gate().Count() == 1);
        Present(c, env, Mode::On, kOff, kOff, true);
        CHECK(c.Gate().Count() == 2);
        // an aborted present between good ones starts the count again
        Present(c, env, Mode::On, kOff, kOff, false);
        CHECK(c.Gate().Count() == 0);
        CHECK(c.AbortedPresents() == 2);
        Present(c, env, Mode::On, kOff, kOff, true);
        CHECK(c.Gate().Count() == 1);
    }

    // Probe mode keeps the same gate (its log line reports it).
    {
        FakeEnv env;
        Controller c;
        env.serial = 7;
        c.OnResizeDone(env, Mode::Probe, kBridge, kHwnd, 3840, 2160);
        Present(c, env, Mode::Probe, kOff, kOff, true);
        CHECK(c.Gate().Count() == 1);
        Present(c, env, Mode::Probe, kOff, kOff, false);
        CHECK(c.Gate().Count() == 0);
    }

    // Regression: normal frame-generation-off presents (the counter advances) are counted exactly as before.
    {
        FakeEnv env;
        Controller c;
        env.serial = 0;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        for (uint32_t i = 1; i <= 5; ++i)
        {
            Present(c, env, Mode::On, kOff, kOff, true);
            CHECK(c.Gate().Count() == i);
        }
        CHECK(c.AbortedPresents() == 0);
        CHECK(Count(env, "ended before the frame generation present") == 0);
        CHECK(Count(env, "present counter is live") == 1); // the first advance is announced once, later ones are silent
    }

    // The index wraps with two buffers: 1 -> 0 and 0 -> 1 are presents, 0 -> 0 and 1 -> 1 are not.
    {
        FakeEnv env;
        Controller c;
        env.serialBuffers = 2;
        env.serial = 1;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        Present(c, env, Mode::On, kOff, kOff, true); // 1 -> 0
        CHECK(*env.serial == 0);
        CHECK(c.Gate().Count() == 1);
        Present(c, env, Mode::On, kOff, kOff, true); // 0 -> 1
        CHECK(*env.serial == 1);
        CHECK(c.Gate().Count() == 2);
        Present(c, env, Mode::On, kOff, kOff, false); // 1 -> 1
        CHECK(c.Gate().Count() == 0);
        Present(c, env, Mode::On, kOff, kOff, true); // 1 -> 0
        Present(c, env, Mode::On, kOff, kOff, false); // 0 -> 0
        CHECK(c.Gate().Count() == 0);
        CHECK(c.AbortedPresents() == 2);
    }

    // Unknown counter (fewer than two buffers): every present counts as before, and the module says so once.
    {
        FakeEnv env; // serial stays unknown
        Controller c;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        Present(c, env, Mode::On, kOff, kOff, false);
        Present(c, env, Mode::On, kOff, kOff, false);
        Present(c, env, Mode::On, kOff, kOff, false);
        CHECK(c.Gate().Count() == 3);
        CHECK(c.AbortedPresents() == 0);
        CHECK(Count(env, "present counter is not available") == 1);
        CHECK(Count(env, "ended before the frame generation present") == 0);
    }
    // ... and silently when no bridge is known yet (there is nothing to read from).
    {
        FakeEnv env;
        Controller c;
        Present(c, env, Mode::On, kOff, kOff, false);
        CHECK(c.Gate().Count() == 1);
        CHECK(Count(env, "present counter is not available") == 0);
    }

    // The counter shows up only at the end (the bridge became known inside the present): unknown, the present counts.
    {
        FakeEnv env;
        Controller c;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        env.fg = kOff;
        c.OnPresentBegin(env, Mode::On); // no counter yet
        env.serial = 3;
        c.OnPresentEnd(env, Mode::On);
        CHECK(c.Gate().Count() == 1);
    }

    // A different bridge by the end of the present (nested resize on another swapchain): the two reads are incomparable, the
    // present counts as before instead of being judged by a counter that belongs to another object.
    {
        FakeEnv env;
        Controller c;
        env.serial = 5;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        env.fg = kOff;
        c.OnPresentBegin(env, Mode::On);
        c.OnResizeDone(env, Mode::On, kOtherBridge, kHwnd, 3840, 2160); // resets the gate, changes the bridge
        c.OnPresentEnd(env, Mode::On);                                  // serial unchanged: would be an abort on one bridge
        CHECK(c.AbortedPresents() == 0); // (the resize reset the gate, so the count itself is 0 either way)
        CHECK(Count(env, "ended before the frame generation present") == 0);
    }

    // An End with no Begin (module attached mid-present) counts as unknown, never as an abort.
    {
        FakeEnv env;
        Controller c;
        env.serial = 2;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        env.fg = kOff;
        c.OnPresentBegin(env, Mode::On);
        c.OnPresentEnd(env, Mode::On); // counter unchanged: one abort
        CHECK(c.AbortedPresents() == 1);
        c.OnPresentEnd(env, Mode::On); // no begin since: unknown, not a second abort from the stale start value
        CHECK(c.AbortedPresents() == 1);
    }

    // The abort line is capped (a stuck swapchain must not flood the log); the counter is exact.
    {
        FakeEnv env;
        Controller c;
        env.serial = 0;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        for (int i = 0; i < 12; ++i)
            Present(c, env, Mode::On, kOff, kOff, false);
        CHECK(c.AbortedPresents() == 12);
        CHECK(Count(env, "ended before the frame generation present") == 5);
    }

    // Mode off reads nothing (the guard test proves the call sites; this proves the controller).
    {
        FakeEnv env;
        Controller c;
        c.OnPresentBegin(env, Mode::Off);
        c.OnPresentEnd(env, Mode::Off);
        CHECK(env.serialReads == 0);
    }

    // ---- the deferred switch at the strict defaults (2 complete off presents and 50 ms) ----

    // Regression: with a counter that advances every present the switch replays at the second off present, as before M2.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.nowMs = 1100;
        Present(c, env, Mode::On, kOff, kOff, true);
        CHECK(env.replays.empty()); // one present is not enough
        Present(c, env, Mode::On, kOff, kOff, true);
        CHECK(env.replays.size() == 1);
        CHECK(env.replays[0].fullscreen);
        CHECK(!env.replays[0].fgActiveAtCall);
    }
    // ... also when the counter wraps with two buffers.
    {
        FakeEnv env;
        Controller c;
        env.serialBuffers = 2;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.nowMs = 1100;
        Present(c, env, Mode::On, kOff, kOff, true);
        Present(c, env, Mode::On, kOff, kOff, true);
        CHECK(env.replays.size() == 1);
    }

    // The first off present is aborted: it is not counted, so the replay needs two more complete presents.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.nowMs = 1100;
        Present(c, env, Mode::On, kOff, kOff, false);
        Present(c, env, Mode::On, kOff, kOff, true);
        CHECK(env.replays.empty()); // two presents went by, only one of them reached the FG present
        Present(c, env, Mode::On, kOff, kOff, true);
        CHECK(env.replays.size() == 1);
    }
    // A present aborted between two good ones starts the count again.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.nowMs = 1100;
        Present(c, env, Mode::On, kOff, kOff, true);
        Present(c, env, Mode::On, kOff, kOff, false); // count 1 -> 0 (an unchanged counter would have confirmed here)
        CHECK(env.replays.empty());
        Present(c, env, Mode::On, kOff, kOff, true);
        CHECK(env.replays.empty());
        Present(c, env, Mode::On, kOff, kOff, true);
        CHECK(env.replays.size() == 1);
    }
    // Presents that keep failing never confirm the gate AND never reach Streamline, so the SOFT limit must not replay them (the
    // decided fix): the wait goes on to the hard limit (24 presents), which replays regardless, loudly, and the line says there
    // was no confirmation. Presents 1-23 give no replay.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        for (int i = 1; i <= 23; ++i)
        {
            env.nowMs += 50;
            Present(c, env, Mode::On, kOff, kOff, false);
            CHECK(env.replays.empty());
        }
        env.nowMs += 50;
        Present(c, env, Mode::On, kOff, kOff, false); // present 24
        CHECK(env.replays.size() == 1);
        CHECK(Count(env, "hard wait limit reached without a confirmation") == 1);
        CHECK(Count(env, "fg=off") >= 1); // the replay line names the frame generation state
        CHECK(Count(env, "frame generation confirmed off") == 0);
        CHECK(Warns(env) >= 1);
    }
    // L8: the hard limit by presents also needs 500 ms. Failed presents 5 ms apart (a 200 fps game): no replay at present 24, the
    // replay comes at 500 ms.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        for (int i = 1; i <= 99; ++i)
        {
            env.nowMs = 1000 + 5 * i;
            Present(c, env, Mode::On, kOff, kOff, false);
            CHECK(env.replays.empty()); // past 24 presents after present 24, but under 500 ms
        }
        env.nowMs = 1500;
        Present(c, env, Mode::On, kOff, kOff, false); // present 100, 500 ms
        CHECK(env.replays.size() == 1);
        CHECK(Count(env, "hard wait limit reached without a confirmation") == 1);
        CHECK_CONTAINS(env.All(), "after 100 presents, 500 ms");
    }    // The same by time: 6 s pass with fewer than 24 failed presents.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        for (int i = 1; i <= 5; ++i)
        {
            env.nowMs += 1000;
            Present(c, env, Mode::On, kOff, kOff, false);
            CHECK(env.replays.empty()); // 2 s and more pass, still no soft replay: nothing reached Streamline
        }
        env.nowMs += 1000; // 6000 ms since the hold
        Present(c, env, Mode::On, kOff, kOff, false);
        CHECK(env.replays.size() == 1);
        CHECK_CONTAINS(env.All(), "after 6 presents, 6000 ms");
        CHECK(Count(env, "hard wait limit reached without a confirmation") == 1);
        CHECK(Warns(env) >= 1);
    }
    // Failing presents past the soft limit, then ONE present that reaches Streamline: the soft limit replays at that present
    // (not confirmed: one complete present is not enough), provided 50 ms have passed since the request.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        for (int i = 0; i < 8; ++i)
        {
            env.nowMs += 5;
            Present(c, env, Mode::On, kOff, kOff, false);
        }
        env.nowMs = 1045; // 45 ms since the request: below the floor
        Present(c, env, Mode::On, kOff, kOff, true);
        CHECK(env.replays.empty());
    }
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        for (int i = 0; i < 8; ++i)
        {
            env.nowMs += 10;
            Present(c, env, Mode::On, kOff, kOff, false);
        }
        CHECK(env.replays.empty());
        env.nowMs = 1090; // 90 ms since the request, 9th present, the first one that reached Streamline
        Present(c, env, Mode::On, kOff, kOff, true);
        CHECK(env.replays.size() == 1);
        CHECK(!env.replays[0].fgActiveAtCall);
        CHECK(Count(env, "wait limit reached") >= 1);
        CHECK(Count(env, "hard wait limit") == 0);
        CHECK(Count(env, "frame generation confirmed off") == 0);
    }
    // A re-request clears "a present reached Streamline since the request": one good present before the re-request must not let
    // the next aborted presents soft-replay.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.nowMs = 1100;
        Present(c, env, Mode::On, kOff, kOff, true); // reached, one complete present: not confirmed yet
        CHECK(env.replays.empty());
        env.nowMs = 1110;
        Present(c, env, Mode::On, kPaused, kActive, true); // frame generation comes back: asked off again
        CHECK(env.fgOffRequests == 2);
        for (int i = 0; i < 10; ++i)
        {
            env.nowMs += 100; // far past the 50 ms floor every time
            Present(c, env, Mode::On, kOff, kOff, false);
            CHECK(env.replays.empty()); // past 6 presents, but none reached Streamline since the re-request
        }
        env.nowMs += 100;
        Present(c, env, Mode::On, kOff, kOff, true); // the first one that does: soft replay
        CHECK(env.replays.size() == 1);
        CHECK(Count(env, "wait limit reached") >= 1);
    }
    // A bridge resize deactivates frame generation again and clears it too.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.nowMs = 1100;
        Present(c, env, Mode::On, kOff, kOff, true);
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        for (int i = 0; i < 10; ++i)
        {
            env.nowMs += 100;
            Present(c, env, Mode::On, kOff, kOff, false);
            CHECK(env.replays.empty());
        }
    }
    // Unknown counter (no serial): every present counts as having reached Streamline, so the soft limit works as before M2, with
    // the 50 ms floor: presents 5 ms apart do not replay at present 6 (30 ms), the first one at 50 ms does.
    {
        FakeEnv env; // serial stays unknown
        Controller c;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        Present(c, env, Mode::On, kActive, kActive, false); // the present thread is recorded
        env.fg = kActive;
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        for (int i = 1; i <= 9; ++i)
        {
            env.nowMs = 1000 + 5 * i;
            Present(c, env, Mode::On, kActive, kPaused, false); // starts active: never a complete off present
            CHECK(env.replays.empty());
        }
        env.nowMs = 1050;
        Present(c, env, Mode::On, kActive, kPaused, false); // present 10, 50 ms
        CHECK(env.replays.size() == 1);
        CHECK(Count(env, "wait limit reached") >= 1);
    }
    // M1 at the strict defaults, scenario 1: frame generation off, a high frame rate. Complete off presents 5 ms apart do not
    // replay (the soft limit used to fire at present 6 = 30 ms); the replay waits for the 50 ms floor and then reports a
    // confirmation.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        for (int i = 1; i <= 9; ++i)
        {
            env.nowMs = 1000 + 5 * i;
            Present(c, env, Mode::On, kOff, kOff, true);
            CHECK(env.replays.empty());
        }
        env.nowMs = 1050;
        Present(c, env, Mode::On, kOff, kOff, true); // present 10, 50 ms
        CHECK(env.replays.size() == 1);
        CHECK(Count(env, "frame generation confirmed off") == 1);
        CHECK(Warns(env) == 0);
    }
    // M1, scenario 2: frame generation keeps coming back for six presents (each present asks it off again), then a present ends
    // with it off. The soft limit used to fire one present after the last re-request. Now it needs the floor measured from that
    // re-request, and a present that reached Streamline after it.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        for (int i = 1; i <= 6; ++i)
        {
            env.nowMs = 1000 + 5 * i;
            Present(c, env, Mode::On, kActive, kActive, true);
        }
        CHECK(env.fgOffRequests == 1 + 6);
        env.nowMs = 1035; // 5 ms after the last re-request
        Present(c, env, Mode::On, kPaused, kPaused, true); // present 7
        CHECK(env.replays.empty());
        env.nowMs = 1040;
        Present(c, env, Mode::On, kPaused, kPaused, true); // present 8: two complete off presents, 10 ms
        CHECK(env.replays.empty());
        env.nowMs = 1085; // 55 ms after the last re-request
        Present(c, env, Mode::On, kPaused, kPaused, true);
        CHECK(env.replays.size() == 1);
    }
    {
        // the same, but present 7 never reaches Streamline and the floor is long over: still no soft replay at present 7
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        for (int i = 1; i <= 6; ++i)
        {
            env.nowMs = 1000 + 100 * i;
            Present(c, env, Mode::On, kActive, kActive, true);
        }
        env.nowMs = 1700; // 100 ms after the last re-request
        Present(c, env, Mode::On, kPaused, kPaused, false); // present 7, aborted
        CHECK(env.replays.empty());
        env.nowMs = 1800;
        Present(c, env, Mode::On, kPaused, kPaused, true); // present 8, the first that reached Streamline since: soft replay
        CHECK(env.replays.size() == 1);
        CHECK(Count(env, "wait limit reached") >= 1);
    }


    // R3-M1: a re-request restarts the 50 ms floor. The hold is at 1000 and frame generation comes back at 1200 (the present
    // asks it off again: the request time is now 1200). Complete off presents at 1205, 1210, 1220 and 1240 are two or more, and
    // more than 50 ms after the HOLD, but not 50 ms after the re-request: no replay. The replay comes at 1250. (A floor measured
    // from the hold would replay at 1210.)
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        env.nowMs = 1200;
        Present(c, env, Mode::On, kActive, kActive, true); // frame generation is back: asked off again
        CHECK(env.fgOffRequests == 2);
        CHECK(env.replays.empty());
        const uint64_t offTimes[] = { 1205, 1210, 1220, 1240 };
        for (const uint64_t t : offTimes)
        {
            env.nowMs = t;
            Present(c, env, Mode::On, kPaused, kPaused, true);
            CHECK(env.replays.empty());
        }
        env.nowMs = 1250; // 50 ms after the re-request
        Present(c, env, Mode::On, kPaused, kPaused, true);
        CHECK(env.replays.size() == 1);
        CHECK(Count(env, "frame generation confirmed off") == 1);
    }
    // The same for the soft limit: six presents have gone by (none reached Streamline), frame generation comes back at 1040, and
    // a present that did reach it ends with the feature off at 1070 and 1080: 30 and 40 ms after the re-request, 70 and 80 ms
    // after the hold. No replay; the floor is over at 1090. (A floor measured from the hold would soft-replay at 1070.)
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        for (int i = 1; i <= 6; ++i)
        {
            env.nowMs = 1000 + 5 * i;
            Present(c, env, Mode::On, kOff, kOff, false);
        }
        CHECK(env.replays.empty());
        env.nowMs = 1040;
        Present(c, env, Mode::On, kActive, kActive, true); // frame generation is back: asked off again
        CHECK(env.fgOffRequests == 2);
        env.nowMs = 1070;
        Present(c, env, Mode::On, kPaused, kPaused, true);
        CHECK(env.replays.empty());
        env.nowMs = 1080;
        Present(c, env, Mode::On, kPaused, kPaused, true);
        CHECK(env.replays.empty());
        env.nowMs = 1090; // 50 ms after the re-request
        Present(c, env, Mode::On, kPaused, kPaused, true);
        CHECK(env.replays.size() == 1);
    }
    // R3-L1: a bridge resize while a call is held deactivates frame generation again, so it restarts the floor too. Five failed
    // presents, a resize at 1100, then presents that reach Streamline with the feature off at 1105, 1110 and 1140: no replay
    // (present 6 would soft-replay at once with a floor measured from the original request); the replay comes at 1150.
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        for (int i = 1; i <= 5; ++i)
        {
            env.nowMs = 1000 + 5 * i;
            Present(c, env, Mode::On, kOff, kOff, false);
        }
        env.nowMs = 1100;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        const uint64_t offTimes[] = { 1105, 1110, 1140 };
        for (const uint64_t t : offTimes)
        {
            env.nowMs = t;
            Present(c, env, Mode::On, kOff, kOff, true);
            CHECK(env.replays.empty());
        }
        env.nowMs = 1150; // 50 ms after the resize
        Present(c, env, Mode::On, kOff, kOff, true);
        CHECK(env.replays.size() == 1);
    }

    // ---- the border fix uses the same gate ----
    {
        FakeEnv env;
        Controller c;
        c.SetFixConfirm(1, 0); // about the counter here; the floor has its own cases in window_fix_flow
        env.serial = 0;
        env.fg = kOff;
        Present(c, env, Mode::On, kOff, kOff, true); // the present thread is recorded
        env.exclusive = true;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        env.exclusive = false;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160); // the leave arms the fix
        env.window = wt::BorderlessWindow();
        env.window.exstyle = 0x00000300u;
        env.window.clientWidth = 3836;
        env.window.clientHeight = 2156;
        CHECK(c.FixArmed());
        Present(c, env, Mode::On, kOff, kOff, false);
        CHECK(env.applied.empty()); // not a complete present
        CHECK(c.FixArmed());
        Present(c, env, Mode::On, kOff, kOff, true);
        CHECK(env.applied.size() == 1);
    }

    return wt::g_failed ? 1 : 0;
}
