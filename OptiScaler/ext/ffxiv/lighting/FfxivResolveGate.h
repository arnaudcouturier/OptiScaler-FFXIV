#pragma once
// Serialises the Detours transactions of the lighting layer (FfxivLightingCapture.h) and pairs the run-time attach and detach of the
// ResolveSubresource detour. Pure: the transactions are passed in as callables, so the rules are tested without Detours (lighting_resolve_gate).
//
// Why: Detours rejects a second DetourTransactionBegin while one is open. The lighting layer has three transaction users on different
// threads: Install (once, early), the main Detach (the unhook thread) and the ResolveSubresource detour, which the preview probe attaches
// and removes at run time from the draw thread. After a deferred preview teardown the unhook thread opens its main transaction while the
// draw thread's next draw removes the resolve detour through the probe: without one lock the second Begin fails (the main detach then
// leaves every hook attached without a log line) or the main transaction detaches a detour that is already gone and aborts. The flag that
// says whether the detour is attached is written by the draw thread and read by the unhook thread, so it is atomic.

#include <atomic>
#include <mutex>

namespace ext::ffxiv::lighting
{
class TransactionGate
{
  public:
    // Runs `transaction` while no other transaction of the layer runs (Install, the main Detach). The callable may read ResolveAttached()
    // and call ResolveGone().
    template <class Fn> decltype(auto) Run(Fn&& transaction)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return transaction();
    }

    // Run-time attach of the resolve detour. `ready` says whether the main hooks are installed (evaluated under the lock: a main detach that
    // ran first has cleared it); `attach` runs the transaction and returns whether it committed. True when the detour is attached afterwards.
    // An attached detour is not attached again.
    template <class Ready, class AttachFn> bool AttachResolve(Ready&& ready, AttachFn&& attach)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!ready())
            return false;
        if (_attached.load(std::memory_order_acquire))
            return true;
        const bool ok = attach();
        _attached.store(ok, std::memory_order_release);
        return ok;
    }

    // Run-time detach. Nothing happens when the detour is not attached (the main detach may have removed it already); a transaction that
    // fails leaves it attached.
    template <class DetachFn> void DetachResolve(DetachFn&& detach)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_attached.load(std::memory_order_acquire))
            return;
        if (detach())
            _attached.store(false, std::memory_order_release);
    }

    bool ResolveAttached() const { return _attached.load(std::memory_order_acquire); }
    // The main detach committed and took the resolve detour with it.
    void ResolveGone() { _attached.store(false, std::memory_order_release); }

  private:
    std::mutex _mutex;
    std::atomic<bool> _attached { false };
};
} // namespace ext::ffxiv::lighting
