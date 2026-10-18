// B2-a: the deferral state machine on a fake clock and a fake frame counter.
#include "window_test.h"

#include "ext/window/WindowDeferral.h"

using namespace ext::window;

int main()
{
    // Idle by default; waiting in Idle does nothing.
    {
        Deferral d;
        CHECK(d.State() == DeferState::Idle);
        CHECK(d.OnPresentEnd(false, true, 100) == WaitResult::Wait);
        CHECK(!d.BeginReplay());
    }

    // The normal path: held, then replayed at the first present that confirms frame generation off.
    {
        Deferral d;
        CHECK(d.Begin(true, 1000));
        CHECK(d.State() == DeferState::Pending);
        CHECK(d.PendingFullscreen());
        CHECK(d.OnPresentEnd(true, false, 1016) == WaitResult::Wait); // the present during which it went off
        CHECK(d.OnPresentEnd(false, true, 1033) == WaitResult::Replay);
        CHECK(d.Presents() == 2);
        CHECK(d.BeginReplay());
        CHECK(d.State() == DeferState::Replaying);
        // Nothing starts or replaces while replaying.
        CHECK(!d.Begin(false, 1040));
        CHECK(!d.Replace(false));
        d.EndReplay();
        CHECK(d.State() == DeferState::Idle);
    }

    // Begin only from Idle.
    {
        Deferral d;
        CHECK(d.Begin(true, 0));
        CHECK(!d.Begin(false, 5));
        CHECK(d.PendingFullscreen()); // unchanged
    }

    // "Keep only the latest": a second call replaces the first; the wait budget is not restarted.
    {
        Deferral d;
        CHECK(d.Begin(true, 0));
        CHECK(d.OnPresentEnd(true, false, 10) == WaitResult::Wait);
        CHECK(d.Replace(false));
        CHECK(!d.PendingFullscreen());
        CHECK(d.Replaced() == 1);
        CHECK(d.Presents() == 1); // not reset
        CHECK(d.Replace(true));
        CHECK(d.Replaced() == 2 && d.PendingFullscreen());
        CHECK(!Deferral().Replace(true)); // nothing to replace in Idle
    }

    // Cancel drops the held call.
    {
        Deferral d;
        CHECK(d.Begin(true, 0));
        d.Cancel();
        CHECK(d.State() == DeferState::Idle);
        CHECK(d.OnPresentEnd(false, true, 5) == WaitResult::Wait); // nothing to replay
        CHECK(d.Begin(false, 9)); // can start again
    }

    // Soft limit by presents: replay anyway, but only with frame generation off at that moment.
    {
        Deferral d; // soft 6 presents
        CHECK(d.Begin(true, 0));
        for (int i = 0; i < 5; ++i)
            CHECK(d.OnPresentEnd(false, false, i + 1) == WaitResult::Wait); // off, not confirmed
        CHECK(d.OnPresentEnd(false, false, 6) == WaitResult::ReplayAtLimit);
    }

    // Soft limit by time.
    {
        Deferral d;
        CHECK(d.Begin(true, 5000));
        CHECK(d.OnPresentEnd(false, false, 6999) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(false, false, 7000) == WaitResult::ReplayAtLimit);
    }

    // Frame generation active (or unknown) at the soft limit: no replay, keep waiting; never replays with it on...
    {
        Deferral d;
        CHECK(d.Begin(true, 0));
        for (int i = 0; i < 20; ++i)
            CHECK(d.OnPresentEnd(true, false, i + 1) == WaitResult::Wait); // soft 6 passed, hard 24 not yet
        // ...a confirmed gate does not help while the feature is active (the controller asks it off again).
        CHECK(d.OnPresentEnd(true, true, 21) == WaitResult::Wait);
    }

    // Hard limit by presents and by time: replay regardless (fail open).
    {
        Deferral d; // hard 24 presents
        CHECK(d.Begin(true, 0));
        for (int i = 0; i < 23; ++i)
            CHECK(d.OnPresentEnd(true, false, i + 1) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(true, false, 24) == WaitResult::ReplayAtHardLimit);
    }
    {
        Deferral d; // hard 6000 ms
        CHECK(d.Begin(true, 100));
        CHECK(d.OnPresentEnd(true, false, 6099) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(true, false, 6100) == WaitResult::ReplayAtHardLimit);
    }

    // On another thread than the one that made the call: nothing replays, not even a confirmed gate, until the hard limit.
    {
        Deferral d;
        CHECK(d.Begin(true, 0));
        for (int i = 0; i < 23; ++i)
            CHECK(d.OnPresentEnd(false, true, i + 1, false) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(false, true, 24, false) == WaitResult::ReplayAtHardLimit);
    }
    {
        Deferral d;
        CHECK(d.Begin(true, 0));
        CHECK(d.OnPresentEnd(false, true, 6100, false) == WaitResult::ReplayAtHardLimit); // by time
    }
    {
        // and back on the right thread the confirmed gate replays at once
        Deferral d;
        CHECK(d.Begin(true, 0));
        CHECK(d.OnPresentEnd(false, true, 1, false) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(false, true, 2, true) == WaitResult::Replay);
    }

    // Custom limits are honoured.
    {
        DeferralLimits limits;
        limits.softPresents = 2;
        limits.hardPresents = 3;
        Deferral d(limits);
        CHECK(d.Begin(true, 0));
        CHECK(d.OnPresentEnd(false, false, 1) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(false, false, 2) == WaitResult::ReplayAtLimit);
    }
    {
        // the custom hard limit (3 presents) ends the wait with frame generation still on; 2 presents do not
        DeferralLimits limits;
        limits.softPresents = 2;
        limits.hardPresents = 3;
        Deferral d(limits);
        CHECK(d.Begin(true, 0));
        CHECK(d.OnPresentEnd(true, false, 1) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(true, false, 2) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(true, false, 3) == WaitResult::ReplayAtHardLimit);
    }

    // A clock that goes backwards never produces a huge elapsed time.
    {
        Deferral d;
        CHECK(d.Begin(true, 5000));
        CHECK(d.ElapsedMs(4000) == 0);
    }

    CHECK(std::string(DeferStateName(DeferState::Pending)) == "pending");
    CHECK(std::string(WaitResultName(WaitResult::ReplayAtHardLimit)).find("still on") != std::string::npos);

    return wt::g_failed ? 1 : 0;
}
