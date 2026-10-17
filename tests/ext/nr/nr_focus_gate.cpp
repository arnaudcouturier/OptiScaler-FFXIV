#include "ext/nr/NrFocusGate.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using ext::nr::FocusGate;

namespace
{
int failures = 0;

void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

constexpr bool kOn = true;
constexpr bool kFocus = true;
constexpr bool kMin = true;
} // namespace

int main()
{
    // Off by default (switch off): always allowed, however the window looks, and no pause is ever remembered.
    {
        FocusGate g;
        CHECK(g.Allowed());
        for (uint64_t t = 0; t < 60000; t += 16)
            CHECK(g.Update(!kOn, !kFocus, kMin, t));
        // Switching it on later does not inherit the time spent unfocused with the switch off.
        CHECK(g.Update(kOn, !kFocus, !kMin, 60000));
        CHECK(g.Update(kOn, !kFocus, !kMin, 60000 + FocusGate::kPauseAfterMs - 1));
        CHECK(!g.Update(kOn, !kFocus, !kMin, 60000 + FocusGate::kPauseAfterMs));
    }

    // Focused with the switch on: never paused.
    {
        FocusGate g;
        for (uint64_t t = 0; t < 10000; t += 16)
            CHECK(g.Update(kOn, kFocus, !kMin, t));
    }

    // Pause debounce: 1 s of continuous unfocus.
    {
        FocusGate g;
        CHECK(g.Update(kOn, kFocus, !kMin, 0));
        CHECK(g.Update(kOn, !kFocus, !kMin, 100));
        CHECK(g.Update(kOn, !kFocus, !kMin, 100 + FocusGate::kPauseAfterMs - 1));
        CHECK(!g.Update(kOn, !kFocus, !kMin, 100 + FocusGate::kPauseAfterMs));
        CHECK(!g.Update(kOn, !kFocus, !kMin, 100000));
    }

    // Resume debounce: 250 ms of continuous focus, and the first focused sample starts the count.
    {
        FocusGate g;
        g.Update(kOn, !kFocus, !kMin, 0);
        CHECK(!g.Update(kOn, !kFocus, !kMin, 1000));
        CHECK(!g.Update(kOn, kFocus, !kMin, 5000));
        CHECK(!g.Update(kOn, kFocus, !kMin, 5000 + FocusGate::kResumeAfterMs - 1));
        CHECK(g.Update(kOn, kFocus, !kMin, 5000 + FocusGate::kResumeAfterMs));
        CHECK(g.Update(kOn, kFocus, !kMin, 9000));
    }

    // A short focus loss (menu popup, IME) never pauses, however often it repeats.
    {
        FocusGate g;
        uint64_t t = 0;
        for (int i = 0; i < 100; ++i)
        {
            CHECK(g.Update(kOn, kFocus, !kMin, t));
            t += 600;
            CHECK(g.Update(kOn, !kFocus, !kMin, t));
            t += 900; // unfocused for 900 ms, then a focused sample breaks the run
            CHECK(g.Update(kOn, !kFocus, !kMin, t));
        }
        // The loop above ends on an unfocused run of 900 ms; the focused sample at the top of each pass reset it.
        CHECK(g.Allowed());
    }

    // A focus bounce while paused does not resume early; the focused run must be unbroken.
    {
        FocusGate g;
        g.Update(kOn, !kFocus, !kMin, 0);
        g.Update(kOn, !kFocus, !kMin, 1000);
        CHECK(!g.Allowed());
        CHECK(!g.Update(kOn, kFocus, !kMin, 2000));
        CHECK(!g.Update(kOn, kFocus, !kMin, 2200));
        CHECK(!g.Update(kOn, !kFocus, !kMin, 2240)); // bounce: the count starts over
        CHECK(!g.Update(kOn, kFocus, !kMin, 2300));
        CHECK(!g.Update(kOn, kFocus, !kMin, 2549));
        CHECK(g.Update(kOn, kFocus, !kMin, 2550));
    }

    // Flapping input (alternating every frame) neither pauses a running pass nor resumes a paused one.
    {
        FocusGate g;
        for (uint64_t t = 0; t < 20000; t += 16)
            CHECK(g.Update(kOn, ((t / 16) & 1) != 0, !kMin, t));

        FocusGate p;
        p.Update(kOn, !kFocus, !kMin, 0);
        p.Update(kOn, !kFocus, !kMin, 1000);
        CHECK(!p.Allowed());
        for (uint64_t t = 2000; t < 22000; t += 16)
            CHECK(!p.Update(kOn, ((t / 16) & 1) != 0, !kMin, t));
    }

    // Minimise pauses at once, even a focused-looking minimised window, and restoring needs the resume debounce.
    {
        FocusGate g;
        CHECK(g.Update(kOn, kFocus, !kMin, 0));
        CHECK(!g.Update(kOn, kFocus, kMin, 10));
        CHECK(!g.Update(kOn, kFocus, kMin, 5000));
        CHECK(!g.Update(kOn, kFocus, !kMin, 6000));
        CHECK(!g.Update(kOn, kFocus, !kMin, 6000 + FocusGate::kResumeAfterMs - 1));
        CHECK(g.Update(kOn, kFocus, !kMin, 6000 + FocusGate::kResumeAfterMs));
    }

    // Switch turned off while paused: the pass is allowed at once and nothing stale is left behind.
    {
        FocusGate g;
        g.Update(kOn, !kFocus, !kMin, 0);
        g.Update(kOn, !kFocus, !kMin, 1000);
        CHECK(!g.Allowed());
        CHECK(g.Update(!kOn, !kFocus, !kMin, 1100));
        CHECK(g.Allowed());
        // Switched on again, still unfocused: a fresh 1 s count, not an instant pause.
        CHECK(g.Update(kOn, !kFocus, !kMin, 1200));
        CHECK(g.Update(kOn, !kFocus, !kMin, 1200 + FocusGate::kPauseAfterMs - 1));
        CHECK(!g.Update(kOn, !kFocus, !kMin, 1200 + FocusGate::kPauseAfterMs));
    }

    // Switch turned on while already unfocused counts from that moment (covered above); a focused window never pauses.
    {
        FocusGate g;
        g.Update(!kOn, !kFocus, !kMin, 0);
        CHECK(g.Update(kOn, kFocus, !kMin, 50000));
    }

    // A clock that steps backwards does not pause or resume anything.
    {
        FocusGate g;
        g.Update(kOn, !kFocus, !kMin, 10000);
        CHECK(g.Update(kOn, !kFocus, !kMin, 5000));
        CHECK(!g.Update(kOn, !kFocus, !kMin, 11000));
    }

    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: NR focus gate (switch off, debounce both ways, minimise, flicker, switch toggled while paused)");
    return 0;
}
