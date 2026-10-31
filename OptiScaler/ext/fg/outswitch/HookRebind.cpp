#include "pch.h"

#include "ext/fg/outswitch/HookAccess.h"
#include "ext/fg/outswitch/HookRebind.h"
#include "ext/fg/outswitch/HookScope.h"

#include <chrono>

// The hook rebind's product glue: the ten slots (HookAccess.h), the process-wide gate (HookScope.h) and the log. The
// algorithm and its rules are in HookRebind.h; nothing here decides anything.

namespace ext::fg::outswitch
{
uint64_t NowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void SleepMs(uint32_t ms) { Sleep(ms); }

// What the switch calls at its unhook step. Ok leaves the gate closed until upstream's own HookFGSwapchain hooks the new
// class; that function reopens the gate with its last statement, whether the attach worked or not. Busy and Failed
// reopen it here, because nothing was detached.
UnhookResult UnhookHookedSwapchain()
{
    HookSlot slots[kHookSlots];
    const size_t count = Access::FgHookSlots(slots, kHookSlots);
    if (count == 0)
        return UnhookResult::Ok; // no set was ever described: nothing to drain, nothing to detach

    size_t detached = 0;
    bool stalled = false;
    const UnhookResult result =
        UnhookSlotsFor(slots, count, HookGateFor(), kHookDrainTimeoutMs, &NowMs, &SleepMs, &detached, &stalled);

    if (stalled)
        LOG_WARN("FG swapchain hooks: the detach window stayed open for {} ms, the suspended threads were resumed",
                 ext::core::kDetourStallMs);

    switch (result)
    {
    case UnhookResult::Ok:
        LOG_INFO("FG swapchain hooks: {} of {} slots detached", detached, count);
        break;
    case UnhookResult::Busy:
        LOG_WARN("FG swapchain hooks: no transaction or no drained body in time, nothing detached", kHookDrainTimeoutMs);
        break;
    case UnhookResult::Failed:
        LOG_ERROR("FG swapchain hooks: the detach failed, nothing was detached");
        break;
    }
    return result;
}

bool HooksTransactable()
{
    const LONG result = DetourTransactionBegin();
    if (result != NO_ERROR)
    {
        // Begin did not take the transaction when it answers 4317: another thread has one open. Any other failure comes
        // after it took ownership, so it is closed here.
        if (result != ERROR_INVALID_OPERATION)
            DetourTransactionAbort();
        return false;
    }
    DetourTransactionAbort(); // nothing was enlisted and nothing was patched
    return true;
}
} // namespace ext::fg::outswitch
