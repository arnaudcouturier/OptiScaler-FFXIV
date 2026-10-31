#pragma once
// The one bridge this module drives, and the refusal of a second one.
//
// The switch owns ONE chain on ONE window: the whole lifetime protocol (LiveChain, the hook rebind, the model table and
// the provider's per-window state) describes a single chain. Two windows would need two of everything, and the provider
// libraries keep one frame-generation chain per window anyway. So the first bridge the module configures is recorded
// here and a second, different one latches the refusal for the rest of the session: the preflight then answers
// "the game created a second swapchain bridge" and nothing is touched.
//
// The pointer is also how the switch and the probe find the bridge after a switch: the FG library's own factory re-entry
// overwrites State::currentWrappedSwapchain with its inner chain, so the global cannot be used.
//
// Pure: no Windows or upstream include (tests/ext/fg/fg_switch_runtime.cpp). The bridge pointer is opaque here; the
// runtime casts it (BridgeHdr.cpp).

#include <atomic>

namespace ext::fg::outswitch
{
class BridgeRegistry
{
  public:
    // Called from the bridge's creation seam. A different bridge while one is set is a second window: latch the refusal.
    void NoteConfigured(void* bridge)
    {
        if (bridge == nullptr)
            return;
        void* expected = nullptr;
        if (bridge_.compare_exchange_strong(expected, bridge, std::memory_order_acq_rel))
            return;
        if (expected == bridge)
            return;
        // A second window: the refusal is latched for the session. The newest bridge is recorded as well, so a reader
        // (the read-only probe) still finds a bridge after the first one goes away instead of reporting that none was
        // ever seen; the switch itself refuses either way.
        second_.store(true, std::memory_order_relaxed);
        other_.store(bridge, std::memory_order_release);
    }

    // Called from the bridge's destruction seam: clears the pointer if it still names this bridge.
    void NoteDestroyed(void* bridge)
    {
        if (bridge == nullptr)
            return;
        void* expected = bridge;
        bridge_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
        expected = bridge;
        other_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }

    void* Bridge() const
    {
        if (void* first = bridge_.load(std::memory_order_acquire); first != nullptr)
            return first;
        return other_.load(std::memory_order_acquire);
    }
    bool SecondBridge() const { return second_.load(std::memory_order_relaxed); }

  private:
    std::atomic<void*> bridge_ { nullptr };
    std::atomic<void*> other_ { nullptr }; // the second bridge, kept only so a reader can still find one
    std::atomic<bool> second_ { false };
};

inline BridgeRegistry& Bridges()
{
    static BridgeRegistry registry;
    return registry;
}
} // namespace ext::fg::outswitch
