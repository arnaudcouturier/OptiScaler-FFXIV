#pragma once
// Teardown of the character preview feature when the hook layer is removed. Pure: the rules and the hand-over flag are tested without a
// device (chara_lifecycle).
//
// All the feature's GPU objects belong to the thread that issues draws on the immediate context: nothing is synchronised, and the draw
// path may be inside any of them at any moment. The unhook path calls Detach() from a thread of its own choosing (before the hooks are
// removed, while draws can still run), so releasing the objects there could free them under a draw in flight. Detach() therefore
// releases at once only when it runs on the draw thread itself; on any other thread it raises a request and the draw thread performs
// the teardown at its next entry.

#include <atomic>
#include <cstdint>

namespace ext::ffxiv::chara
{
enum class DetachAction : uint8_t
{
    Now,   // the caller is the draw thread: nothing is in flight, release now
    Defer, // another thread: leave the release to the draw thread
};

constexpr DetachAction DecideDetach(bool onDrawThread) { return onDrawThread ? DetachAction::Now : DetachAction::Defer; }

enum class GateOverride : uint8_t
{
    None, // the configuration decides
    On,   // a teardown is pending: the draw has to enter the dispatcher so the draw thread can perform it
    Off,  // the feature is inert or already torn down: the draw runs untouched
};

// What the dispatcher's cheap gate does before it looks at the configuration.
constexpr GateOverride GateOverrideFor(bool inert, bool detachPending, bool detached)
{
    if (inert)
        return GateOverride::Off;
    if (detachPending)
        return GateOverride::On;
    if (detached)
        return GateOverride::Off;
    return GateOverride::None;
}

class DetachGate
{
  public:
    // Any thread: a teardown is wanted.
    void Request() { _pending.store(true, std::memory_order_release); }
    bool Pending() const { return _pending.load(std::memory_order_acquire); }
    // The draw thread: true once per request; the caller then tears down.
    bool Take() { return _pending.exchange(false, std::memory_order_acq_rel); }
    // The teardown ran: the dispatcher stays off until the hooks are installed again.
    void Completed() { _detached.store(true, std::memory_order_release); }
    bool Detached() const { return _detached.load(std::memory_order_acquire); }
    // The hook layer was installed (again).
    void Reset()
    {
        _pending.store(false, std::memory_order_release);
        _detached.store(false, std::memory_order_release);
    }

  private:
    std::atomic<bool> _pending { false };
    std::atomic<bool> _detached { false };
};
} // namespace ext::ffxiv::chara
