// [REL] The tri-state of ext::dx11bridge::BeforeReleaseFeature, pure CPU (ext/gpu/DrainResult.h, ext/dx11bridge/ReleaseDrain.h).
//
// The rules under test:
//   * a queue is "drained" only when its fence wait completed on a live device; a null queue and a removed device prove
//     nothing; a signal/event failure or a timeout is a failure
//   * the release is Drained only when the upscale queue drained, the FG queue drained too when FG is attached to this
//     feature, and the handle was proven quiet (no evaluate in flight); a failure on a required queue is Failed
//   * the 500 ms sleep is skipped ONLY on Drained, only with the key on, never while shutting down
#include "ext/dx11bridge/ReleaseDrain.h"
#include "ext/gpu/DrainResult.h"

#include <cstdio>
#include <string>

using namespace ext::dx11bridge;
using ext::gpu::ClassifyFence;
using ext::gpu::FenceOutcome;
using ext::gpu::QueueDrain;

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

static FenceOutcome Ok()
{
    FenceOutcome o;
    o.setupOk = true;
    o.signalOk = true;
    o.waitSignaled = true;
    o.removedBefore = false;
    o.removedAfter = false;
    return o;
}

int main()
{
    // ---- fence outcome -> QueueDrain ---------------------------------------------------------------------------------
    CHECK(ClassifyFence(Ok()) == QueueDrain::Drained);
    {
        auto o = Ok();
        o.removedBefore = true; // nothing can run on a removed device: no proof, nothing to wait for either
        o.setupOk = false;      // (the wait is not even attempted)
        CHECK(ClassifyFence(o) == QueueDrain::DeviceLost);
    }
    {
        auto o = Ok();
        o.setupOk = false; // GetDevice / CreateFence / CreateEvent failed
        CHECK(ClassifyFence(o) == QueueDrain::Failed);
    }
    {
        auto o = Ok();
        o.signalOk = false; // Signal or SetEventOnCompletion failed
        o.waitSignaled = false;
        CHECK(ClassifyFence(o) == QueueDrain::Failed);
    }
    {
        auto o = Ok();
        o.waitSignaled = false; // 5 s cap hit
        CHECK(ClassifyFence(o) == QueueDrain::Failed);
    }
    {
        auto o = Ok();
        o.waitSignaled = false;
        o.removedAfter = true; // device removed while waiting: not a stall, not a proof
        CHECK(ClassifyFence(o) == QueueDrain::DeviceLost);
    }
    {
        auto o = Ok();
        o.waitSignaled = true;
        o.removedAfter = true; // a removal signals every fence, so "completed" and "removed" look alike: no proof, DeviceLost
        CHECK(ClassifyFence(o) == QueueDrain::DeviceLost);
    }
    // the bool wrapper keeps today's DrainQueue answers
    CHECK(ext::gpu::DrainedOk(QueueDrain::Drained));
    CHECK(ext::gpu::DrainedOk(QueueDrain::Absent));     // DrainQueue(nullptr) == true today
    CHECK(ext::gpu::DrainedOk(QueueDrain::DeviceLost)); // removed device == true today
    CHECK(!ext::gpu::DrainedOk(QueueDrain::Failed));

    // ---- queues -> ReleaseDrain ----------------------------------------------------------------------------------------
    const auto Q = Quiescence::Idle;
    // FG not attached: only the upscale queue counts
    CHECK(ClassifyRelease(false, QueueDrain::Absent, QueueDrain::Drained, Q) == ReleaseDrain::Drained);
    CHECK(ClassifyRelease(false, QueueDrain::Failed, QueueDrain::Drained, Q) == ReleaseDrain::Drained); // fg result ignored when not required
    CHECK(ClassifyRelease(false, QueueDrain::Absent, QueueDrain::Absent, Q) == ReleaseDrain::NotApplicable); // no bridge queue
    CHECK(ClassifyRelease(false, QueueDrain::Absent, QueueDrain::DeviceLost, Q) == ReleaseDrain::NotApplicable);
    CHECK(ClassifyRelease(false, QueueDrain::Absent, QueueDrain::Failed, Q) == ReleaseDrain::Failed);
    // FG attached to this feature: both must have drained
    CHECK(ClassifyRelease(true, QueueDrain::Drained, QueueDrain::Drained, Q) == ReleaseDrain::Drained);
    CHECK(ClassifyRelease(true, QueueDrain::Absent, QueueDrain::Drained, Q) == ReleaseDrain::NotApplicable); // FG queue unknown
    CHECK(ClassifyRelease(true, QueueDrain::DeviceLost, QueueDrain::Drained, Q) == ReleaseDrain::NotApplicable);
    CHECK(ClassifyRelease(true, QueueDrain::Failed, QueueDrain::Drained, Q) == ReleaseDrain::Failed);
    CHECK(ClassifyRelease(true, QueueDrain::Drained, QueueDrain::Failed, Q) == ReleaseDrain::Failed);
    CHECK(ClassifyRelease(true, QueueDrain::Failed, QueueDrain::Failed, Q) == ReleaseDrain::Failed);
    CHECK(ClassifyRelease(true, QueueDrain::Drained, QueueDrain::Absent, Q) == ReleaseDrain::NotApplicable);
    // a failure always wins, even without proof of quiet
    CHECK(ClassifyRelease(false, QueueDrain::Absent, QueueDrain::Failed, Quiescence::EvaluateInFlight) == ReleaseDrain::Failed);
    // GPU drained but the handle was not proven quiet: not a proof, the sleep stays (and the release proceeds)
    CHECK(ClassifyRelease(false, QueueDrain::Absent, QueueDrain::Drained, Quiescence::NotChecked) == ReleaseDrain::NotApplicable);
    CHECK(ClassifyRelease(false, QueueDrain::Absent, QueueDrain::Drained, Quiescence::EvaluateInFlight) == ReleaseDrain::NotApplicable);
    CHECK(ClassifyRelease(false, QueueDrain::Absent, QueueDrain::Drained, Quiescence::SelfInside) == ReleaseDrain::NotApplicable);

    // ---- sleep decision ------------------------------------------------------------------------------------------------
    CHECK(SkipReleaseSleep(ReleaseDrain::Drained, true, false));
    CHECK(!SkipReleaseSleep(ReleaseDrain::NotApplicable, true, false)); // no proof: today's sleep
    CHECK(!SkipReleaseSleep(ReleaseDrain::Failed, true, false));
    CHECK(!SkipReleaseSleep(ReleaseDrain::Drained, false, false)); // kill switch: today's behaviour
    CHECK(!SkipReleaseSleep(ReleaseDrain::Drained, true, true));   // shutting down: the caller never sleeps, nothing to skip
    // the names are the pinned log words
    CHECK(std::string(Name(ReleaseDrain::Drained)) == "drained");
    CHECK(std::string(Name(ReleaseDrain::NotApplicable)) == "not-applicable");
    CHECK(std::string(Name(ReleaseDrain::Failed)) == "failed");

    if (g_failed == 0)
        std::printf("release_drain: all checks passed\n");
    return g_failed;
}
