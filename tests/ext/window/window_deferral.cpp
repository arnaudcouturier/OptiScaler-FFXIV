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
        CHECK(d.OnPresentEnd(false, true, 100, true, true) == WaitResult::Wait);
        CHECK(!d.BeginReplay());
    }

    // The normal path: held, then replayed at the first present that confirms frame generation off.
    {
        Deferral d;
        CHECK(d.Begin(true, 1000));
        CHECK(d.State() == DeferState::Pending);
        CHECK(d.PendingFullscreen());
        CHECK(d.OnPresentEnd(true, false, 1016, true, true) == WaitResult::Wait); // the present during which it went off
        CHECK(d.OnPresentEnd(false, true, 1033, true, true) == WaitResult::Replay);
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
        CHECK(d.OnPresentEnd(true, false, 10, true, true) == WaitResult::Wait);
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
        CHECK(d.OnPresentEnd(false, true, 5, true, true) == WaitResult::Wait); // nothing to replay
        CHECK(d.Begin(false, 9)); // can start again
    }

    // Soft limit by presents: replay anyway, but only with frame generation off at that moment.
    {
        Deferral d; // soft 6 presents
        CHECK(d.Begin(true, 0));
        for (int i = 0; i < 5; ++i)
            CHECK(d.OnPresentEnd(false, false, i + 1, true, true) == WaitResult::Wait); // off, not confirmed
        CHECK(d.OnPresentEnd(false, false, 6, true, true) == WaitResult::ReplayAtLimit);
    }

    // Soft limit by time.
    {
        Deferral d;
        CHECK(d.Begin(true, 5000));
        CHECK(d.OnPresentEnd(false, false, 6999, true, true) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(false, false, 7000, true, true) == WaitResult::ReplayAtLimit);
    }

    // The soft limit needs the controller's permission (`softAllowed`: a present reached the frame generation present since the
    // request, and the floor since the request is over). Without it the wait goes on, by presents and by time, up to the hard
    // limit, which replays regardless; a confirmed gate is not held back by it.
    {
        Deferral d;
        CHECK(d.Begin(true, 0));
        for (int i = 0; i < 20; ++i)
            CHECK(d.OnPresentEnd(false, false, i + 1, true, false) == WaitResult::Wait); // past the soft 6 presents
        CHECK(d.OnPresentEnd(false, false, 21, true, true) == WaitResult::ReplayAtLimit); // allowed again: replays at once
    }
    {
        Deferral d;
        CHECK(d.Begin(true, 5000));
        CHECK(d.OnPresentEnd(false, false, 7000, true, false) == WaitResult::Wait); // soft 2000 ms passed, not allowed
        CHECK(d.OnPresentEnd(false, false, 7001, true, true) == WaitResult::ReplayAtLimit);
    }
    {
        Deferral d; // not allowed: the hard limit still replays, by presents...
        CHECK(d.Begin(true, 0));
        for (int i = 0; i < 23; ++i)
            CHECK(d.OnPresentEnd(false, false, (i + 1) * 25, true, false) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(false, false, 600, true, false) == WaitResult::ReplayAtHardLimit);
    }
    {
        Deferral d; // ... and by time
        CHECK(d.Begin(true, 0));
        CHECK(d.OnPresentEnd(false, false, 5999, true, false) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(false, false, 6000, true, false) == WaitResult::ReplayAtHardLimit);
    }
    {
        Deferral d; // a confirmed gate does not need the permission
        CHECK(d.Begin(true, 0));
        CHECK(d.OnPresentEnd(false, true, 1, true, false) == WaitResult::Replay);
    }
    // Frame generation active (or unknown) at the soft limit: no replay, keep waiting; never replays with it on...
    {
        Deferral d;
        CHECK(d.Begin(true, 0));
        for (int i = 0; i < 20; ++i)
            CHECK(d.OnPresentEnd(true, false, i + 1, true, true) == WaitResult::Wait); // soft 6 passed, hard 24 not yet
        // ...a confirmed gate does not help while the feature is active (the controller asks it off again).
        CHECK(d.OnPresentEnd(true, true, 21, true, true) == WaitResult::Wait);
    }

    // Hard limit by presents and by time: replay regardless (fail open).
    {
        Deferral d; // hard 24 presents (and 500 ms)
        CHECK(d.Begin(true, 0));
        for (int i = 0; i < 23; ++i)
            CHECK(d.OnPresentEnd(true, false, (i + 1) * 25, true, true) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(true, false, 600, true, true) == WaitResult::ReplayAtHardLimit);
    }
    {
        Deferral d; // hard 6000 ms
        CHECK(d.Begin(true, 100));
        CHECK(d.OnPresentEnd(true, false, 6099, true, true) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(true, false, 6100, true, true) == WaitResult::ReplayAtHardLimit);
    }

    // On another thread than the one that made the call: nothing replays, not even a confirmed gate, until the hard limit.
    {
        Deferral d;
        CHECK(d.Begin(true, 0));
        for (int i = 0; i < 23; ++i)
            CHECK(d.OnPresentEnd(false, true, (i + 1) * 25, false, true) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(false, true, 600, false, true) == WaitResult::ReplayAtHardLimit);
    }
    {
        Deferral d;
        CHECK(d.Begin(true, 0));
        CHECK(d.OnPresentEnd(false, true, 6100, false, true) == WaitResult::ReplayAtHardLimit); // by time
    }
    {
        // and back on the right thread the confirmed gate replays at once
        Deferral d;
        CHECK(d.Begin(true, 0));
        CHECK(d.OnPresentEnd(false, true, 1, false, true) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(false, true, 2, true, true) == WaitResult::Replay);
    }

    // L8: the presents limit alone shrinks with the frame rate (24 presents are 100 ms at 240 fps): it also needs 500 ms. The
    // 6 s time limit stays, and does not need the presents.
    {
        Deferral d;
        CHECK(d.Begin(true, 1000));
        for (int i = 0; i < 40; ++i)
            CHECK(d.OnPresentEnd(true, false, 1000 + (i + 1) * 4, true, true) == WaitResult::Wait); // 40 presents in 160 ms
        CHECK(d.OnPresentEnd(true, false, 1499, true, true) == WaitResult::Wait);  // 499 ms: not yet
        CHECK(d.OnPresentEnd(true, false, 1500, true, true) == WaitResult::ReplayAtHardLimit); // 500 ms and well over 24 presents
    }
    {
        // 500 ms but fewer than 24 presents: the presents limit is not reached
        Deferral d;
        CHECK(d.Begin(true, 0));
        for (int i = 0; i < 10; ++i)
            CHECK(d.OnPresentEnd(true, false, 100 * (i + 1), true, true) == WaitResult::Wait); // 1000 ms, 10 presents
    }
    {
        // the floor is a limit field: custom 0 gives the old behaviour, custom 2000 delays it
        DeferralLimits limits;
        limits.hardPresents = 3;
        limits.hardPresentsMinMs = 2000;
        Deferral d(limits);
        CHECK(d.Begin(true, 0));
        CHECK(d.OnPresentEnd(true, false, 1, true, true) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(true, false, 2, true, true) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(true, false, 3, true, true) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(true, false, 1999, true, true) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(true, false, 2000, true, true) == WaitResult::ReplayAtHardLimit);
    }
    // Custom limits are honoured.
    {
        DeferralLimits limits;
        limits.softPresents = 2;
        limits.hardPresents = 3;
        Deferral d(limits);
        CHECK(d.Begin(true, 0));
        CHECK(d.OnPresentEnd(false, false, 1, true, true) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(false, false, 2, true, true) == WaitResult::ReplayAtLimit);
    }
    {
        // the custom hard limit (3 presents) ends the wait with frame generation still on; 2 presents do not
        DeferralLimits limits;
        limits.softPresents = 2;
        limits.hardPresents = 3;
        limits.hardPresentsMinMs = 0;
        Deferral d(limits);
        CHECK(d.Begin(true, 0));
        CHECK(d.OnPresentEnd(true, false, 1, true, true) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(true, false, 2, true, true) == WaitResult::Wait);
        CHECK(d.OnPresentEnd(true, false, 3, true, true) == WaitResult::ReplayAtHardLimit);
    }

    // A clock that goes backwards never produces a huge elapsed time.
    {
        Deferral d;
        CHECK(d.Begin(true, 5000));
        CHECK(d.ElapsedMs(4000) == 0);
    }

    CHECK(std::string(DeferStateName(DeferState::Pending)) == "pending");
    CHECK(std::string(WaitResultName(WaitResult::ReplayAtHardLimit)).find("without a confirmation") != std::string::npos);

    return wt::g_failed ? 1 : 0;
}
