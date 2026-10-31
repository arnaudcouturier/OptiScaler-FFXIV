// The judgement of a freshly adopted chain (ext/fg/outswitch/ProbationWatch.h): twenty presents or eight seconds of
// probation, the watchdog after it, the stall rule, the lost device, and the rule that a paused provider is not a
// failure (an earlier design rolled a switch back every few seconds of every cutscene because of it).
#include "ext/fg/outswitch/ProbationWatch.h"

#include <cstdint>
#include <cstdio>

using namespace ext::fg::outswitch;

static int g_failed = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                        \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{
constexpr uint32_t kGood = kOk;
constexpr uint32_t kBad = 0x80004005u;
constexpr uint32_t kFrameMs = 8;

void InactiveBeforeACommit()
{
    std::printf("  before a commit nothing is judged\n");
    Probation probation;
    const Probation::Note note = probation.Present(kBad, kFrameMs, 1000);
    CHECK(note.health == Health::Inactive);
    CHECK(!note.mask);
    CHECK(!note.rollback);
    CHECK(!probation.RollbackDue());
    CHECK(!probation.Watching());
    CHECK(probation.Presents() == 0);
    std::printf("    [ok] a failure before any switch does not start a watchdog\n");
}

void TwentyHealthyPresents()
{
    std::printf("  twenty healthy presents judge the chain\n");
    Probation probation;
    probation.Begin(1000);
    CHECK(probation.Watching());
    CHECK(probation.InProbation());
    int judged = 0;
    for (uint32_t i = 0; i < kProbationPresents; ++i)
    {
        const uint64_t now = 1000 + i * 16;
        const Probation::Note note = probation.Present(kGood, kFrameMs, now);
        CHECK(note.health == Health::Healthy);
        CHECK(!note.mask);
        CHECK(!note.rollback);
        if (probation.TakeJudged())
            ++judged;
        CHECK(!probation.RollbackDue());
    }
    CHECK(judged == 1);
    CHECK(probation.TakeJudged() == false); // once only
    CHECK(probation.Presents() == kProbationPresents);
    CHECK(probation.Healthy() == kProbationPresents);
    CHECK(!probation.InProbation());
    CHECK(probation.Watching()); // the watchdog keeps watching
    CHECK(probation.FailureCount() == 0);
    std::printf("    [ok] the 20th present ends probation once, the watchdog stays\n");
}

void TimeWindow()
{
    std::printf("  eight seconds with at least one healthy present also judge it\n");
    Probation probation;
    probation.Begin(0);
    probation.Present(kGood, kFrameMs, 100);
    CHECK(probation.TakeJudged() == false);
    probation.Present(kGood, kFrameMs, kProbationWindowMs - 1);
    CHECK(probation.TakeJudged() == false);
    CHECK(probation.InProbation());
    probation.Present(kGood, kFrameMs, kProbationWindowMs);
    CHECK(probation.TakeJudged());
    CHECK(!probation.InProbation());
    std::printf("    [ok] the time rule fires at 8 s, not before\n");

    std::printf("  eight seconds without any present decide nothing\n");
    Probation paused;
    paused.Begin(0);
    for (uint64_t now = 0; now <= 600000; now += 1000)
    {
        // No present at all: the caller has nothing to report. Ten minutes of it.
    }
    CHECK(paused.InProbation());
    CHECK(paused.Watching());
    CHECK(!paused.RollbackDue());
    CHECK(paused.Presents() == 0);
    CHECK(paused.TakeJudged() == false);
    std::printf("    [ok] a window that does not present stays unjudged and never rolls back\n");
}

void FailureInProbation()
{
    std::printf("  a failed present during probation is masked and rolls back\n");
    Probation probation;
    probation.Begin(0);
    probation.Present(kGood, kFrameMs, 16);
    const Probation::Note note = probation.Present(kBad, kFrameMs, 32);
    CHECK(note.health == Health::Failed);
    CHECK(note.mask);
    CHECK(note.rollback);
    CHECK(probation.RollbackDue());
    CHECK(probation.LastFail() == Fail::Probation);
    CHECK(probation.LastResult() == kBad);
    CHECK(probation.FailureCount() == 1);
    CHECK(!probation.InProbation()); // the judgement is made, the rollback is next
    probation.ClearRollback();
    CHECK(!probation.RollbackDue());
    std::printf("    [ok] one failure is enough during probation, the game sees S_OK\n");
}

void StallInProbation()
{
    std::printf("  a stall during probation is a failure, a stall after it is not\n");
    Probation probation;
    probation.Begin(0);
    // The first present after a commit carries the switch itself (provider creation can take seconds), so the stall
    // clock starts with the SECOND present: a long first present is not a Stall.
    const Probation::Note hitch = probation.Present(kGood, kStallMs + 1, 16);
    CHECK(hitch.health == Health::Healthy);
    CHECK(!probation.RollbackDue());
    CHECK(probation.LastFail() == Fail::None);
    const Probation::Note note = probation.Present(kGood, kStallMs + 1, 32);
    CHECK(note.health == Health::Failed);
    CHECK(note.mask);
    CHECK(note.rollback);
    CHECK(probation.LastFail() == Fail::Stall);
    CHECK(probation.RollbackDue());
    CHECK(probation.FailureCount() == 1);
    std::printf("    [ok] the switch hitch is not a stall; 2501 ms on the next present is\n");

    Probation after;
    after.Begin(0);
    for (uint32_t i = 0; i < kProbationPresents; ++i)
        after.Present(kGood, kFrameMs, i * 16);
    CHECK(!after.InProbation());
    const Probation::Note slow = after.Present(kGood, kStallMs + 1, 5000);
    CHECK(slow.health == Health::Healthy);
    CHECK(!slow.rollback);
    CHECK(!after.RollbackDue());
    std::printf("    [ok] a slow present after probation is not a failure\n");

    Probation exactly;
    exactly.Begin(0);
    exactly.Present(kGood, kFrameMs, 16); // the exempt first present
    const Probation::Note bound = exactly.Present(kGood, kStallMs, 32);
    CHECK(bound.health == Health::Healthy);
    std::printf("    [ok] exactly 2500 ms is still fine\n");
}

void ResultsThatSayNothingAboutTheChain()
{
    std::printf("  results that say nothing about the chain are not failures\n");
    // DXGI_ERROR_WAS_STILL_DRAWING: a game presenting with DXGI_PRESENT_DO_NOT_WAIT, whose frame is still in flight.
    // Counting it rolled a healthy chain back as soon as a game used the flag (REFERENCES.md [MS-3]).
    const uint32_t notFailures[] = { kOk, kOccluded, kClipped, kModeChanged, kModeChangeInProgress, kStillDrawing };
    for (uint32_t result : notFailures)
    {
        Probation probation;
        probation.Begin(0);
        const Probation::Note note = probation.Present(result, kFrameMs, 16);
        CHECK(note.health == Health::Healthy);
        CHECK(!note.rollback);
        CHECK(!probation.RollbackDue());
        CHECK(probation.FailureCount() == 0);
        CHECK(PresentHealthy(result));
    }
    std::printf("    [ok] S_OK, every DXGI_STATUS_* code and WAS_STILL_DRAWING are healthy\n");

    // The same code after probation is still not a failure, so the watchdog never counts it.
    Probation after;
    after.Begin(0);
    for (uint32_t i = 0; i < kProbationPresents; ++i)
        after.Present(kGood, kFrameMs, i * 16);
    for (uint32_t i = 0; i < kWatchdogFailures + 2; ++i)
    {
        const Probation::Note note = after.Present(kStillDrawing, kFrameMs, 4000 + i * 16);
        CHECK(note.health == Health::Healthy);
        CHECK(!note.rollback);
    }
    CHECK(!after.RollbackDue());
    std::printf("    [ok] WAS_STILL_DRAWING never reaches the watchdog\n");
}

void Watchdog()
{
    std::printf("  after probation three failures in a row roll back\n");
    Probation probation;
    probation.Begin(0);
    uint64_t now = 0;
    for (uint32_t i = 0; i < kProbationPresents; ++i)
        probation.Present(kGood, kFrameMs, now += 16);
    CHECK(!probation.InProbation());

    // After probation a failure is passed to the game: no mask.
    for (uint32_t i = 0; i < kWatchdogFailures - 1; ++i)
    {
        const Probation::Note note = probation.Present(kBad, kFrameMs, now += 16);
        CHECK(note.health == Health::Failed);
        CHECK(!note.mask);
        CHECK(!note.rollback);
        CHECK(!probation.RollbackDue());
    }
    const Probation::Note last = probation.Present(kBad, kFrameMs, now += 16);
    CHECK(last.rollback);
    CHECK(!last.mask);
    CHECK(probation.RollbackDue());
    probation.ClearRollback();

    // One healthy present resets the run.
    probation.Present(kGood, kFrameMs, now += 16);
    CHECK(!probation.RollbackDue());
    for (uint32_t i = 0; i < kWatchdogFailures - 1; ++i)
        probation.Present(kBad, kFrameMs, now += 16);
    CHECK(!probation.RollbackDue());
    probation.Present(kBad, kFrameMs, now += 16);
    CHECK(probation.RollbackDue());
    std::printf("    [ok] three consecutive failures, a healthy present resets the count\n");
}

void DeviceLost()
{
    std::printf("  a lost device is never masked and never rolled back\n");
    const uint32_t losts[] = { kDeviceRemoved, kDeviceReset, kDeviceHung };
    for (uint32_t lost : losts)
    {
        Probation probation;
        probation.Begin(0);
        const Probation::Note note = probation.Present(lost, kFrameMs, 16);
        CHECK(note.health == Health::DeviceLost);
        CHECK(!note.mask);
        CHECK(!note.rollback);
        CHECK(!probation.RollbackDue());
        CHECK(!probation.Watching());
        CHECK(!probation.InProbation());
        CHECK(probation.LastResult() == lost);
    }
    std::printf("    [ok] removed, reset and hung are forwarded as they are, with no rollback\n");
}

void EndStopsJudging()
{
    std::printf("  an ended judgment reports nothing\n");
    Probation probation;
    probation.Begin(0);
    probation.Present(kGood, kFrameMs, 16);
    probation.End();
    const Probation::Note note = probation.Present(kBad, kFrameMs, 32);
    CHECK(note.health == Health::Inactive);
    CHECK(!note.mask);
    CHECK(!probation.RollbackDue());
    CHECK(!probation.Watching());
    CHECK(probation.Presents() == 0);
    std::printf("    [ok] after End a failure belongs to nobody\n");

    static_assert(kProbationPresents == 20, "twenty presents of probation");
    static_assert(kProbationWindowMs == 8000, "eight seconds of probation");
    static_assert(kStallMs == 2500, "a present over 2.5 s is a stall");
    static_assert(kWatchdogFailures == 3, "three consecutive failures after probation");
    std::printf("    [ok] 20 presents, 8 s, 2500 ms, 3 failures\n");
}
} // namespace

int main()
{
    InactiveBeforeACommit();
    TwentyHealthyPresents();
    TimeWindow();
    FailureInProbation();
    StallInProbation();
    ResultsThatSayNothingAboutTheChain();
    Watchdog();
    DeviceLost();
    EndStopsJudging();

    if (g_failed == 0)
        std::printf("ALL PROBATION SCENARIOS PASS\n");
    else
        std::printf("%d CHECK(S) FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
