// The rate limit of the frame generator's frame-count re-sync warning (ext/fg/FrameJumpLog.h), driven with a fake clock as the seam
// drives it: the first jump logs its counters verbatim, the jumps inside one second only count, and the first jump after a second
// logs how many it swallowed. A provider that keeps being started used to write thousands of identical lines a minute.
#include "ext/fg/FrameJumpLog.h"

#include <cstdio>

using namespace ext::fg;

static int g_failed = 0;
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

namespace
{
void TestFirstJump()
{
    JumpLimiter limiter;
    CHECK(JumpLimiter::kIntervalMs == 1000);
    const JumpNote first = limiter.Note(5000);
    CHECK(first.log);
    CHECK(first.suppressed == 0 && first.sinceMs == 0); // the caller writes the two counters verbatim
}

void TestBurstOfOneSecond()
{
    // 3,000 jumps inside one second: exactly one line, and it says how many were not written
    JumpLimiter limiter;
    CHECK(limiter.Note(10000).log);
    uint64_t lines = 1;
    for (uint64_t ms = 10000; ms < 11000; ++ms)
    {
        for (int i = 0; i < 3; ++i)
            if (limiter.Note(ms).log)
                ++lines;
    }
    CHECK(lines == 1);
    const JumpNote after = limiter.Note(12000); // the first jump after the interval
    CHECK(after.log);
    CHECK(after.sinceMs == 2000);
    CHECK(after.suppressed == 3000); // the 3,000 jumps of that second, none of which was written
    // ... and the count starts over with that line
    const JumpNote quiet = limiter.Note(12500);
    CHECK(!quiet.log);
    const JumpNote next = limiter.Note(13500);
    CHECK(next.log && next.suppressed == 1 && next.sinceMs == 1500);
}

void TestJumpsTwoSecondsApart()
{
    // One jump every two seconds: every jump logs, nothing is swallowed
    JumpLimiter limiter;
    uint64_t lines = 0;
    for (uint64_t ms = 0; ms <= 8000; ms += 2000)
    {
        const JumpNote note = limiter.Note(ms);
        if (note.log)
        {
            ++lines;
            CHECK(note.suppressed == 0);
            CHECK(ms == 0 || note.sinceMs == 2000);
        }
    }
    CHECK(lines == 5);
}

void TestBoundary()
{
    JumpLimiter limiter;
    CHECK(limiter.Note(0).log);
    // Exactly one interval is a new line (>= kIntervalMs), one tick less is not
    CHECK(!limiter.Note(999).log);
    CHECK(limiter.Note(1000).log);
    // A clock that went backwards logs instead of hiding the jump (unsigned arithmetic reads it as a huge gap)
    JumpLimiter backwards;
    CHECK(backwards.Note(50000).log);
    const JumpNote early = backwards.Note(1000);
    CHECK(early.log && early.sinceMs > JumpLimiter::kIntervalMs);
    // Reset starts the whole rule over (a new provider's first jump is logged with its counters)
    limiter.Reset();
    const JumpNote again = limiter.Note(1500);
    CHECK(again.log && again.suppressed == 0 && again.sinceMs == 0);
}
} // namespace

int main()
{
    TestFirstJump();
    TestBurstOfOneSecond();
    TestJumpsTwoSecondsApart();
    TestBoundary();
    if (g_failed != 0)
    {
        std::printf("frame_jump_log: %d failure(s)\n", g_failed);
        return 2;
    }
    std::puts("frame_jump_log: PASS");
    return 0;
}
