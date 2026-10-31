// The paste cadence counter (ext/hudless/UiPasteCadence.h): which real frame's UI image each output received. Pure, no
// GPU, no clock: the outputs are fed in by the test.
#include "ext/hudless/UiPasteCadence.h"

#include <cstdio>
#include <string>

using namespace ext::hudless::cadence;

namespace
{
int g_failures = 0;
#define EXPECT(cond, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s  ", __LINE__, #cond);                                                    \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

void SteadyTwoOutputs()
{
    std::printf("  every image on exactly two outputs\n");
    Tracker t;
    for (uint64_t frame = 1; frame <= 100; ++frame)
    {
        t.OnPaste(frame, frame, false, false);
        t.OnPaste(frame, frame, false, false);
    }
    const Window w = t.Take();
    EXPECT(w.pastes == 200, "pastes %llu", (unsigned long long) w.pastes);
    EXPECT(w.runs[1] == 99 && w.runs[0] == 0 && w.runs[2] == 0 && w.runs[3] == 0, "99 closed runs of 2 (the last is open): %llu/%llu/%llu/%llu",
           (unsigned long long) w.runs[0], (unsigned long long) w.runs[1], (unsigned long long) w.runs[2], (unsigned long long) w.runs[3]);
    EXPECT(w.age[0] == 200, "age 0 everywhere");
    EXPECT(w.framesSkipped == 0 && w.breaks == 0 && w.newestPending == 0, "nothing skipped");
}

void OneFrameBehindIsStillSteady()
{
    std::printf("  always one real frame behind: steady\n");
    Tracker t;
    for (uint64_t frame = 2; frame <= 51; ++frame)
    {
        t.OnPaste(frame, frame - 1, true, false);
        t.OnPaste(frame, frame - 1, true, false);
    }
    const Window w = t.Take();
    EXPECT(w.runs[1] == 49 && w.runs[0] == 0 && w.runs[2] == 0, "runs of 2 only");
    EXPECT(w.age[1] == 100 && w.age[0] == 0, "age 1 everywhere");
    EXPECT(w.newestPending == 100, "pending counted on every paste");
    EXPECT(w.framesSkipped == 0, "nothing skipped");
}

void RaceGivesUnevenRuns()
{
    std::printf("  the newest image completes before the outputs on some frames only\n");
    Tracker t;
    // frame 1: (1, 1); frame 2: still on the GPU at both outputs -> (1, 1); frame 3: done -> (3, 3); frame 4: done
    // only for the second output -> (3, 4); frame 5: (5, 5)
    t.OnPaste(1, 1, false, false);
    t.OnPaste(1, 1, false, false);
    t.OnPaste(2, 1, true, false);
    t.OnPaste(2, 1, true, false);
    t.OnPaste(3, 3, false, false);
    t.OnPaste(3, 3, false, false);
    t.OnPaste(4, 3, true, false);
    t.OnPaste(4, 4, false, false);
    t.OnPaste(5, 5, false, false);
    const Window w = t.Take();
    // image 1 on 4 outputs, image 3 on 3, image 4 on 1; image 5 is open; frame 2 never shown
    EXPECT(w.runs[0] == 1 && w.runs[1] == 0 && w.runs[2] == 1 && w.runs[3] == 1, "runs 1/2/3/4+ = %llu/%llu/%llu/%llu",
           (unsigned long long) w.runs[0], (unsigned long long) w.runs[1], (unsigned long long) w.runs[2], (unsigned long long) w.runs[3]);
    EXPECT(w.framesSkipped == 1, "frame 2 never pasted: %llu", (unsigned long long) w.framesSkipped);
    EXPECT(w.age[0] == 6 && w.age[1] == 3, "ages %llu/%llu", (unsigned long long) w.age[0], (unsigned long long) w.age[1]);
    EXPECT(w.newestPending == 3, "pending %llu", (unsigned long long) w.newestPending);
}

void BreakEndsTheRunUncounted()
{
    std::printf("  a break ends the open run without counting it, and no skip is counted across it\n");
    Tracker t;
    t.OnPaste(1, 1, false, false);
    t.OnPaste(1, 1, false, false);
    t.Break();
    t.Break(); // a second break with no open run counts nothing
    t.OnPaste(9, 9, false, true);
    t.OnPaste(9, 9, false, true);
    t.OnPaste(10, 10, false, true);
    const Window w = t.Take();
    EXPECT(w.breaks == 1, "one break: %llu", (unsigned long long) w.breaks);
    EXPECT(w.runs[1] == 1 && w.runs[0] == 0, "only the run of image 9 is closed");
    EXPECT(w.framesSkipped == 0, "no skip across the break");
    EXPECT(w.steady == 3, "steady counted");
}

void TakeCarriesTheOpenRun()
{
    std::printf("  Take starts a new window, the open run carries over\n");
    Tracker t;
    t.OnPaste(1, 1, false, false);
    const Window first = t.Take();
    EXPECT(first.pastes == 1 && first.runs[0] == 0, "nothing closed yet");
    t.OnPaste(1, 1, false, false);
    t.OnPaste(2, 2, false, false);
    const Window second = t.Take();
    EXPECT(second.pastes == 2 && second.runs[1] == 1, "the run of image 1 has two outputs across the windows");
    const Window third = t.Take();
    EXPECT(third.pastes == 0 && third.runs[1] == 0, "empty window");
}

void LongRunsAndOldImages()
{
    std::printf("  runs of 4 and more share one bucket, age 3 and more share one bucket, age 2 has its own\n");
    Tracker t;
    for (int i = 0; i < 6; ++i)
        t.OnPaste(10, 5, true, false);
    t.OnPaste(11, 6, false, false);
    t.OnPaste(13, 11, false, false); // two real frames behind the newest one: an age-2 paste, not an age-3+ one
    const Window w = t.Take();
    EXPECT(w.runs[3] == 1, "run of 6 in 4+");
    EXPECT(w.age[3] == 7, "age 5 in 3+");
    EXPECT(w.age[2] == 1, "age 2 has its own bucket: %llu", (unsigned long long) w.age[2]);
}

void LineFormat()
{
    std::printf("  the log line\n");
    Window w;
    w.pastes = 1200;
    w.newestPending = 300;
    w.age = { 900, 300, 0, 0 };
    w.runs = { 40, 520, 40, 0 };
    w.framesSkipped = 40;
    w.breaks = 1;
    const std::string line = Line(w, 600, 10.0);
    const std::string expected = "HudLess paste cadence (10.0 s): 1200 outputs pasted, 600 images built (2.00 outputs per image); "
                                 "newest image still on the GPU at 300 pastes (25.0%); pasted image age 0/1/2/3+ real frames "
                                 "900/300/0/0; outputs per image 1/2/3/4+ 40/520/40/0; real frames never pasted 40; breaks 1; "
                                 "steady timing 0 of 1200";
    EXPECT(line == expected, "\n      got      %s\n      expected %s", line.c_str(), expected.c_str());
    const std::string empty = Line(Window {}, 0, 0.0);
    EXPECT(empty.find("0 outputs pasted, 0 images built (0.00 outputs per image)") != std::string::npos &&
               empty.find("(0.0%)") != std::string::npos,
           "no division by zero: %s", empty.c_str());
}

void ClockTenSeconds()
{
    std::printf("  the line comes every 10 s of pasting, with the images built in that time\n");
    WindowClock clock;
    int lines = 0;
    uint64_t produced = 0;
    double lastSeconds = 0.0;
    uint64_t lastBuilt = 0;
    for (int i = 0; i < 3000; ++i) // 8 ms per paste, one image per two pastes: 24 s
    {
        const double now = 1000.0 + i * 8.0;
        if (i % 2 == 0)
            ++produced;
        const auto before = clock.BeforePaste(now, produced);
        EXPECT(!before.due, "no pause, nothing ends before a paste (i %d)", i);
        const auto after = clock.AfterPaste(now, produced);
        if (after.due)
        {
            ++lines;
            lastSeconds = after.seconds;
            lastBuilt = after.built;
        }
    }
    EXPECT(lines == 2, "two full windows in 24 s: %d", lines);
    EXPECT(lastSeconds >= 10.0 && lastSeconds < 10.01, "a window is 10 s: %f", lastSeconds);
    EXPECT(lastBuilt >= 624 && lastBuilt <= 626, "about 625 images in 10 s: %llu", (unsigned long long) lastBuilt);
}

void ClockPauseEndsTheWindow()
{
    std::printf("  a pause in the pasting ends the window at its last paste; the gap is in no line\n");
    WindowClock clock;
    uint64_t produced = 100;
    for (int i = 0; i < 500; ++i) // 4 s of pasting
    {
        produced += i % 2;
        clock.BeforePaste(i * 8.0, produced);
        EXPECT(!clock.AfterPaste(i * 8.0, produced).due, "not 10 s yet");
    }
    // a fallback: 30 s without a paste while the producer keeps building probe images
    produced += 2000;
    const double resume = 499 * 8.0 + 30000.0;
    const auto ended = clock.BeforePaste(resume, produced);
    EXPECT(ended.due, "the pause ended the window");
    EXPECT(ended.seconds > 3.98 && ended.seconds < 4.0, "the window ends at its last paste: %f s", ended.seconds);
    EXPECT(ended.built == 250, "only the images of its own time: %llu", (unsigned long long) ended.built);
    // the next window starts with the paste that resumed
    EXPECT(!clock.AfterPaste(resume, produced).due, "a new window has just started");
    const auto next = clock.AfterPaste(resume + 10000.0, produced + 600);
    EXPECT(next.due && next.built == 600 && next.seconds >= 10.0 && next.seconds < 10.01, "the next window: %f s, %llu built",
           next.seconds, (unsigned long long) next.built);
    // a short hitch (under the gap) does not end a window
    WindowClock steady;
    steady.BeforePaste(0.0, 0);
    steady.AfterPaste(0.0, 0);
    EXPECT(!steady.BeforePaste(WindowClock::kGapMs - 1.0, 1).due, "a gap under kGapMs keeps the window");
    EXPECT(!steady.BeforePaste(WindowClock::kGapMs, 2).due, "a gap of exactly kGapMs is not a pause (the test is >, not >=)");
}

} // namespace

int main()
{
    SteadyTwoOutputs();
    OneFrameBehindIsStillSteady();
    RaceGivesUnevenRuns();
    BreakEndsTheRunUncounted();
    TakeCarriesTheOpenRun();
    LongRunsAndOldImages();
    LineFormat();
    ClockTenSeconds();
    ClockPauseEndsTheWindow();
    std::printf("[ui_paste_cadence] failures: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
