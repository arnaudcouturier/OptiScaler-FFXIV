#pragma once
// Result of waiting for a D3D12 queue to go idle (ext::gpu::DrainQueueEx, GpuQueue.cpp). Pure, no D3D types, so the
// classification is unit-tested on its own (tests/ext/dx11bridge/release_drain_test.cpp).

namespace ext::gpu
{
enum class QueueDrain
{
    Absent,     // no queue to wait for: proves nothing
    Drained,    // the fence signalled behind everything submitted so far, on a live device
    DeviceLost, // the device is (or became) removed: nothing can still run, but the wait proves nothing either
    Failed,     // setup, signal or event failed, or the cap (5 s) was hit
};

// What happened around the fence wait.
struct FenceOutcome
{
    bool setupOk = false;        // GetDevice, CreateFence and CreateEvent all worked
    bool signalOk = false;       // Signal and SetEventOnCompletion both succeeded
    bool waitSignaled = false;   // the wait ended because the fence completed (not a timeout or wait failure)
    bool removedBefore = false;  // the device was already removed before anything was submitted
    bool removedAfter = false;   // the device is removed after the wait
};

inline QueueDrain ClassifyFence(const FenceOutcome& o)
{
    if (o.removedBefore)
        return QueueDrain::DeviceLost;
    if (!o.setupOk || !o.signalOk)
        return QueueDrain::Failed;
    // A device removal signals every fence, so a completed wait on a device that is removed afterwards cannot be told from a
    // normal completion: it is never a proof (DeviceLost), whichever came first.
    if (o.removedAfter)
        return QueueDrain::DeviceLost;
    return o.waitSignaled ? QueueDrain::Drained : QueueDrain::Failed;
}

// The answer ext::gpu::DrainQueue (bool) has always given: only a failed drain is false.
inline bool DrainedOk(QueueDrain d) { return d != QueueDrain::Failed; }
} // namespace ext::gpu
