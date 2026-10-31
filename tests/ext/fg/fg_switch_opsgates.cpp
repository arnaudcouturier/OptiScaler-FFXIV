// The product binding's own arithmetic and gate (ext/fg/outswitch/OpsGates.h) against the judgement it feeds
// (ProbationWatch.h): the present's duration, not its start, and the one-shot fault that may only fire inside a
// probation. Both are driven with the values a real session has: the switch's clock is a steady-clock count since boot,
// so it is far above the stall limit from the first present.
#include "ext/fg/outswitch/OpsGates.h"
#include "ext/fg/outswitch/ProbationWatch.h"
#include "ext/fg/outswitch/SwitchSequence.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

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
constexpr uint32_t kFrameMs = 8;
constexpr uint32_t kStallPresentMs = 3000;
constexpr uint32_t kFail = 0x80004005u;
// A machine that has been up for about 14 hours: the start of the first present of a session on the switch's clock.
constexpr uint64_t kBootRelativeStart = 50'000'000ull;

// The least an Ops needs for the present-result path: the clock, the event log and the status.
struct ResultOps
{
    uint64_t now = 0;
    int probationEvents = 0;
    int published = 0;
    Fail lastFail = Fail::None;

    uint64_t NowMs() const { return now; }
    void Report(const Event& event)
    {
        if (event.step == Step::Probation)
        {
            ++probationEvents;
            if (event.fail != Fail::None)
                lastFail = event.fail;
        }
    }
    void Publish(const Status&) { ++published; }
};

// Exactly what the product's present seam does with one present's result (OutputSwitch.cpp FgResultFromSwap): the
// duration comes from the start the present recorded, and the probation fault is only offered while a probation runs.
uint32_t PresentSeam(SwitchState& state, ResultOps& ops, FaultGate& fault, uint32_t result, uint64_t startMs)
{
    const uint32_t presentMs = ElapsedMs(startMs, ops.NowMs());
    if (fault.Take(Step::Probation, state.probation.InProbation()))
        return OnFgPresentResult(state, ops, kFail, presentMs);
    return OnFgPresentResult(state, ops, result, presentMs);
}

void DurationIsNotTheStart()
{
    std::printf("  the judgement is given the present's duration, on a clock that is a count since boot\n");
    CHECK(ElapsedMs(kBootRelativeStart, kBootRelativeStart + kFrameMs) == kFrameMs);
    // A session that started after 49.7 days of uptime: the start does not fit the 32-bit value the judgement takes.
    constexpr uint64_t kLateStart = 0x1'0000'000Aull;
    CHECK(ElapsedMs(kLateStart, kLateStart + kFrameMs) == kFrameMs);
    CHECK(ElapsedMs(5000, 4999) == 0);                    // a clock that went backwards does not wrap
    CHECK(ElapsedMs(0, 0x1'FFFFFFFFull) == 0xFFFFFFFFu);  // a longer present than the type holds is clamped

    // The defect, in one line: the start passed as the duration is a stall on every machine up for more than 2.5 s.
    Probation wrong;
    wrong.Begin(kBootRelativeStart);
    wrong.Present(kOk, kFrameMs, kBootRelativeStart + kFrameMs); // the exempt first present
    const Probation::Note asStart =
        wrong.Present(kOk, static_cast<uint32_t>(kBootRelativeStart), kBootRelativeStart + 16);
    CHECK(asStart.health == Health::Failed);
    CHECK(asStart.rollback);
    CHECK(wrong.LastFail() == Fail::Stall);

    // The same presents through the binding: healthy, twenty of them, and probation judged healthy once.
    SwitchState state;
    ResultOps ops;
    FaultGate fault;
    uint64_t start = kBootRelativeStart;
    state.probation.Begin(start);
    state.active = Target::FsrFg;
    state.target = Target::FsrFg;
    for (uint32_t i = 0; i < kProbationPresents; ++i)
    {
        ops.now = start + kFrameMs;
        const uint32_t answer = PresentSeam(state, ops, fault, kOk, start);
        CHECK(answer == kOk);
        CHECK(!state.probation.RollbackDue());
        start += kFrameMs;
    }
    CHECK(state.probation.Healthy() == kProbationPresents);
    CHECK(!state.probation.InProbation()); // the twentieth present ended the window
    CHECK(ops.probationEvents == 1);       // the one line that says probation ended healthy
    CHECK(ops.published == 1);             // the status goes back from Probation to Idle once
    std::printf("    [ok] 20 healthy presents after a switch stay healthy on a boot-relative clock\n");
}

void StallStillFails()
{
    std::printf("  a real stall inside probation is still a stall\n");
    SwitchState state;
    ResultOps ops;
    FaultGate fault;
    uint64_t start = kBootRelativeStart;
    state.probation.Begin(start);
    state.active = Target::FsrFg;
    state.target = Target::FsrFg;

    // The switching present itself carries the creation: exempt, however long it took.
    ops.now = start + kStallPresentMs;
    CHECK(PresentSeam(state, ops, fault, kOk, start) == kOk);
    CHECK(!state.probation.RollbackDue());

    // The next one is judged, and three seconds is a stall: masked for the game, recorded for the rollback.
    start = ops.now;
    ops.now = start + kStallPresentMs;
    CHECK(PresentSeam(state, ops, fault, kOk, start) == kOk);
    CHECK(state.probation.RollbackDue());
    CHECK(state.probation.LastFail() == Fail::Stall);
    CHECK(ops.probationEvents == 1);
    std::printf("    [ok] an exempt switching present, then a masked 3 s stall\n");
}

void ProbationFaultStaysInProbation()
{
    std::printf("  the probation fault fires once, inside a probation, and the game sees S_OK\n");
    SwitchState state;
    ResultOps ops;
    FaultGate fault;
    fault.step = Step::Probation;
    state.active = Target::FsrFg;
    state.target = Target::FsrFg;

    // Before any switch: nothing is judged, so the fault is not offered and the result is untouched.
    ops.now = kBootRelativeStart;
    CHECK(!state.probation.InProbation());
    CHECK(!fault.Take(Step::Probation, state.probation.InProbation()));
    CHECK(!fault.fired);
    CHECK(PresentSeam(state, ops, fault, kOk, kBootRelativeStart) == kOk);
    CHECK(ops.probationEvents == 0);
    // A failing present before any switch is the game's own result, not a masked one.
    CHECK(PresentSeam(state, ops, fault, kFail, kBootRelativeStart) == kFail);

    // A switch commits: the fault becomes reachable, fires once, and the injected failure is masked.
    state.probation.Begin(ops.now);
    CHECK(state.probation.InProbation());
    const uint32_t masked = PresentSeam(state, ops, fault, kOk, ops.now - kFrameMs);
    CHECK(fault.fired);
    CHECK(masked == kOk);
    CHECK(state.probation.RollbackDue());
    CHECK(state.probation.LastFail() == Fail::Probation);
    CHECK(ops.probationEvents == 1);

    // One shot: the next probation this session is judged normally.
    state.probation.End();
    state.probation.Begin(ops.now + 100);
    CHECK(!fault.Take(Step::Probation, state.probation.InProbation()));
    std::printf("    [ok] nothing before a switch, one masked failure inside probation, nothing after it\n");
}

void EachStepFiresOnce()
{
    std::printf("  each other step fires once, at its own step only\n");
    const Step steps[] = { Step::Drain, Step::Teardown, Step::Create, Step::Adopt, Step::Replay };
    for (Step configured : steps)
    {
        FaultGate gate;
        gate.step = configured;
        for (Step at : { Step::Begin, Step::Preflight, Step::Barrier, Step::Quiesce, Step::Capture, Step::Detach,
                         Step::Unhook, Step::TestPresent, Step::Commit, Step::Rollback })
            CHECK(!gate.Take(at));
        CHECK(gate.Take(configured));
        CHECK(!gate.Take(configured));
        CHECK(gate.fired);
    }

    FaultGate none;
    for (Step at : { Step::Drain, Step::Teardown, Step::Create, Step::Adopt, Step::Replay, Step::Probation })
        CHECK(!none.Take(at));
    CHECK(!none.fired);
    std::printf("    [ok] one fault per session, at the step the key names\n");
}
} // namespace

int main()
{
    DurationIsNotTheStart();
    StallStillFails();
    ProbationFaultStaysInProbation();
    EachStepFiresOnce();

    if (g_failed == 0)
        std::printf("ALL PRESENT-BINDING GATE SCENARIOS PASS\n");
    else
        std::printf("%d CHECK(S) FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
