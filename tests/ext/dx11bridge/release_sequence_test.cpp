// [REL] The release orchestration of ext::dx11bridge::BeforeReleaseFeature, with fake queues (ext/dx11bridge/ReleaseSequence.h).
//
// Order under test:  close the handle  ->  bounded wait for evaluates in flight  ->  drain the FG queue (when FG is attached to
// the feature)  ->  drain the upscale queue  ->  classify.  A failed or throwing release reopens the handle. The key off ("gate"
// false) closes nothing and waits for nothing, which is the old flow. No step runs under the table lock.
#include "ext/dx11bridge/NgxFeatureTable.h"
#include "ext/dx11bridge/ReleaseSequence.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace ext::dx11bridge;
using ext::gpu::QueueDrain;
using namespace std::chrono_literals;

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

struct FakeOps
{
    EvalRegistry* reg = nullptr;
    unsigned id = 0;
    QueueDrain fg = QueueDrain::Drained;
    QueueDrain upscale = QueueDrain::Drained;
    bool throwInUpscale = false;
    std::vector<std::string> calls;
    bool lockHeld = false;
    bool closedAtDrain = false;   // the handle was closed when the FG drain ran
    bool closedAtUpscale = false; // ... when the upscale drain ran
    bool sameQueue = false;

    QueueDrain DrainFg()
    {
        calls.push_back("fg");
        lockHeld |= TableLockHeldByThisThread();
        closedAtDrain = reg->IsClosed(id);
        return fg;
    }
    QueueDrain DrainUpscale(QueueDrain fgResult)
    {
        calls.push_back("upscale");
        lockHeld |= TableLockHeldByThisThread();
        closedAtUpscale = reg->IsClosed(id);
        if (throwInUpscale)
            throw std::runtime_error("driver");
        return sameQueue ? fgResult : upscale;
    }
};

static ReleasePlan Plan(bool fgRequired, bool gate)
{
    ReleasePlan p;
    p.handleId = 42;
    p.fgRequired = fgRequired;
    p.gate = gate;
    p.quietTimeout = 200ms;
    return p;
}

int main()
{
    // ---- the quiet wait is short: a long evaluate must not stretch the release (kept sleep covers it) --------------------------
    CHECK(ReleasePlan {}.quietTimeout <= 250ms);
    CHECK(ReleasePlan {}.quietTimeout >= 100ms);

    // ---- all good: drained, handle left closed for the caller's destroy --------------------------------------------------
    {
        EvalRegistry reg;
        FakeOps ops;
        ops.reg = &reg;
        ops.id = 42;
        const auto out = PrepareRelease(reg, Plan(true, true), ops);
        CHECK(out.result == ReleaseDrain::Drained);
        CHECK(out.quiet == Quiescence::Idle);
        CHECK(ops.calls == (std::vector<std::string> { "fg", "upscale" })); // FG first, then upscale
        CHECK(!ops.lockHeld);
        CHECK(ops.closedAtDrain); // evaluates were already refused while draining
        CHECK(ops.closedAtUpscale);
        CHECK(reg.IsClosed(42));  // stays closed until the entry is extracted (Extract forgets it)
    }
    // ---- FG not attached: only the upscale queue ----------------------------------------------------------------------------
    {
        EvalRegistry reg;
        FakeOps ops;
        ops.reg = &reg;
        ops.id = 42;
        const auto out = PrepareRelease(reg, Plan(false, true), ops);
        CHECK(out.result == ReleaseDrain::Drained);
        CHECK(ops.calls == (std::vector<std::string> { "upscale" }));
    }
    // ---- one queue serves both roles: the second drain gets the first result -------------------------------------------------
    {
        EvalRegistry reg;
        FakeOps ops;
        ops.reg = &reg;
        ops.id = 42;
        ops.sameQueue = true;
        ops.fg = QueueDrain::Drained;
        CHECK(PrepareRelease(reg, Plan(true, true), ops).result == ReleaseDrain::Drained);
    }
    // ---- FG drain fails: the upscale queue is not touched, the release is refused, the handle reopens ------------------------
    {
        EvalRegistry reg;
        FakeOps ops;
        ops.reg = &reg;
        ops.id = 42;
        ops.fg = QueueDrain::Failed;
        const auto out = PrepareRelease(reg, Plan(true, true), ops);
        CHECK(out.result == ReleaseDrain::Failed);
        CHECK(ops.calls == (std::vector<std::string> { "fg" }));
        CHECK(!reg.IsClosed(42));
    }
    // ---- upscale drain fails ---------------------------------------------------------------------------------------------------
    {
        EvalRegistry reg;
        FakeOps ops;
        ops.reg = &reg;
        ops.id = 42;
        ops.upscale = QueueDrain::Failed;
        const auto out = PrepareRelease(reg, Plan(false, true), ops);
        CHECK(out.result == ReleaseDrain::Failed);
        CHECK(!reg.IsClosed(42));
    }
    // ---- exception while draining = Failed, and the handle reopens -----------------------------------------------------------
    {
        EvalRegistry reg;
        FakeOps ops;
        ops.reg = &reg;
        ops.id = 42;
        ops.throwInUpscale = true;
        const auto out = PrepareRelease(reg, Plan(false, true), ops);
        CHECK(out.result == ReleaseDrain::Failed);
        CHECK(!reg.IsClosed(42));
    }
    // ---- unknown / lost queues: not applicable (proceed, keep the sleep) ----------------------------------------------------
    {
        EvalRegistry reg;
        FakeOps ops;
        ops.reg = &reg;
        ops.id = 42;
        ops.upscale = QueueDrain::Absent;
        CHECK(PrepareRelease(reg, Plan(false, true), ops).result == ReleaseDrain::NotApplicable);
        CHECK(!reg.IsClosed(42)); // fail open: no proof, so the old flow - evaluates run through the kept sleep
        ops.upscale = QueueDrain::DeviceLost;
        CHECK(PrepareRelease(reg, Plan(false, true), ops).result == ReleaseDrain::NotApplicable);
        CHECK(!reg.IsClosed(42));
    }
    // ---- an evaluate in flight on another thread: the wait is bounded, the result is not a proof ------------------------
    {
        EvalRegistry reg;
        std::atomic<bool> entered { false }, release { false };
        std::thread evaluator([&] {
            EvaluatePin pin(reg, 42);
            entered = true;
            while (!release)
                std::this_thread::sleep_for(1ms);
        });
        while (!entered)
            std::this_thread::sleep_for(1ms);
        FakeOps ops;
        ops.reg = &reg;
        ops.id = 42;
        const auto out = PrepareRelease(reg, Plan(false, true), ops);
        CHECK(out.quiet == Quiescence::EvaluateInFlight);
        CHECK(out.result == ReleaseDrain::NotApplicable); // drains were fine, the CPU side was not proven
        CHECK(ops.calls == (std::vector<std::string> { "upscale" })); // the drain still ran
        CHECK(!ops.closedAtUpscale); // reopened as soon as the wait failed: the drain and the kept sleep run the old flow
        CHECK(!reg.IsClosed(42)); // reopened: the kept sleep must not refuse evaluates
        release = true;
        evaluator.join();
    }
    // ---- the releasing thread is inside an evaluate of the handle -------------------------------------------------------------
    {
        EvalRegistry reg;
        FakeOps ops;
        ops.reg = &reg;
        ops.id = 42;
        EvaluatePin pin(reg, 42);
        const auto out = PrepareRelease(reg, Plan(false, true), ops);
        CHECK(out.quiet == Quiescence::SelfInside);
        CHECK(!ops.closedAtUpscale); // reopened before the drains
        CHECK(out.result == ReleaseDrain::NotApplicable);
        CHECK(!reg.IsClosed(42));
    }
    // ---- the key off (gate false): no close, no wait, no proof; the drain and the Failed answer are as before -------------
    {
        EvalRegistry reg;
        std::atomic<bool> entered { false }, release { false };
        std::thread evaluator([&] {
            EvaluatePin pin(reg, 42);
            entered = true;
            while (!release)
                std::this_thread::sleep_for(1ms);
        });
        while (!entered)
            std::this_thread::sleep_for(1ms);
        FakeOps ops;
        ops.reg = &reg;
        ops.id = 42;
        const auto t0 = std::chrono::steady_clock::now();
        const auto out = PrepareRelease(reg, Plan(false, false), ops);
        CHECK(std::chrono::steady_clock::now() - t0 < 150ms); // did not wait for the evaluator (timeout is 200 ms)
        CHECK(!reg.IsClosed(42));
        CHECK(out.quiet == Quiescence::NotChecked);
        CHECK(out.result == ReleaseDrain::NotApplicable); // never Drained with the key off
        CHECK(!ops.closedAtDrain);
        ops.upscale = QueueDrain::Failed;
        CHECK(PrepareRelease(reg, Plan(false, false), ops).result == ReleaseDrain::Failed);
        release = true;
        evaluator.join();
    }
    // ---- a closed handle refuses new evaluates while the release runs --------------------------------------------------------
    {
        EvalRegistry reg;
        struct RefusingOps : FakeOps
        {
            bool refusedDuringDrain = false;
            QueueDrain DrainUpscale(QueueDrain r)
            {
                EvaluatePin pin(*reg, id);
                refusedDuringDrain = !static_cast<bool>(pin);
                return FakeOps::DrainUpscale(r);
            }
        } ops;
        ops.reg = &reg;
        ops.id = 42;
        CHECK(PrepareRelease(reg, Plan(false, true), ops).result == ReleaseDrain::Drained);
        CHECK(ops.refusedDuringDrain);
    }

    if (g_failed == 0)
        std::printf("release_sequence: all checks passed\n");
    return g_failed;
}
