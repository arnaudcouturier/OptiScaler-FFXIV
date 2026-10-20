#pragma once
// The order of a DX11 upscaler release (the core of ext::dx11bridge::BeforeReleaseFeature), written against a small `Ops` type so
// the order and the lock discipline are unit-tested with fake queues (tests/ext/dx11bridge/release_sequence_test.cpp).
//
//   1. gate on:  close the handle to new evaluates, wait (bounded) for the ones in flight
//   2. FG attached to the feature:  drain the FG queue (Ops::DrainFg also cancels the FG's pending upscaler work)
//   3. drain the upscale queue
//   4. classify (ReleaseDrain.h)
// Only a Drained release leaves the handle closed (until the entry is extracted from the table, which forgets it); every other
// result - Failed, an exception, NotApplicable - reopens it, so an unproven release behaves as before: evaluates keep running
// through the sleep that stays. With the gate off nothing is closed and nothing is waited for,
// which is the flow before this change. No step runs under the table lock (checked by the test).
//
// Ops:  QueueDrain DrainFg();   QueueDrain DrainUpscale(QueueDrain fgResult);   (the second one knows when both roles share a queue)

#include "ext/dx11bridge/NgxFeatureTable.h"
#include "ext/dx11bridge/ReleaseDrain.h"

#include <chrono>

namespace ext::dx11bridge
{
struct ReleasePlan
{
    unsigned handleId = 0;
    bool fgRequired = false; // FG is attached to this feature: its queue has to be proven idle too
    bool gate = false;       // [NgxRelease] SkipReleaseSleep on and not shutting down
    std::chrono::milliseconds quietTimeout { 1000 };
};

struct ReleaseOutcome
{
    ReleaseDrain result = ReleaseDrain::NotApplicable;
    gpu::QueueDrain fg = gpu::QueueDrain::Absent;
    gpu::QueueDrain upscale = gpu::QueueDrain::Absent;
    Quiescence quiet = Quiescence::NotChecked;
    bool threw = false;
};

template <class Ops> ReleaseOutcome PrepareRelease(EvalRegistry& registry, const ReleasePlan& plan, Ops& ops)
{
    using gpu::QueueDrain;
    ReleaseOutcome out;
    try
    {
        if (plan.gate)
        {
            registry.Close(plan.handleId);
            out.quiet = registry.WaitIdle(plan.handleId, plan.quietTimeout);
        }
        if (plan.fgRequired)
        {
            out.fg = ops.DrainFg();
            if (out.fg == QueueDrain::Failed)
            {
                out.result = ReleaseDrain::Failed;
                if (plan.gate)
                    registry.Reopen(plan.handleId);
                return out;
            }
        }
        out.upscale = ops.DrainUpscale(out.fg);
    }
    catch (...)
    {
        out.threw = true;
        out.result = ReleaseDrain::Failed;
        if (plan.gate)
            registry.Reopen(plan.handleId);
        return out;
    }
    out.result = ClassifyRelease(plan.fgRequired, out.fg, out.upscale, out.quiet);
    // Only a proven release keeps the handle closed (the caller then skips the sleep, so the window is milliseconds). Any
    // other answer falls back to the old flow in full: evaluates keep running through the 500 ms sleep that stays.
    if (out.result != ReleaseDrain::Drained && plan.gate)
        registry.Reopen(plan.handleId);
    return out;
}
} // namespace ext::dx11bridge
