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
    // Presents that keep failing never confirm the gate: the wait ends at the soft limit (6 presents) like any wait that never
    // confirms (frame generation is off right then), and the replay line says so instead of "confirmed off".
    {
        FakeEnv env;
        Controller c;
        Running(c, env);
        env.nowMs = 1000;
        CHECK(c.OnSetFullscreenState(env, Mode::On, Event(true)));
        for (int i = 0; i < 5; ++i)
        {
            env.nowMs += 50;
            Present(c, env, Mode::On, kOff, kOff, false);
        }
        CHECK(env.replays.empty());
        env.nowMs += 50;
        Present(c, env, Mode::On, kOff, kOff, false);
        CHECK(env.replays.size() == 1);
        CHECK(Count(env, "wait limit reached") >= 1);
        CHECK(Count(env, "frame generation confirmed off") == 0);
    }

    // ---- the border fix uses the same gate ----
    {
        FakeEnv env;
        Controller c;
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
