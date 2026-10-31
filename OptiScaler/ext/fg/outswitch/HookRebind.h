#pragma once
// The hook rebind: taking one class's swapchain detours off so the next class can be hooked.
//
// The rules are written once in SwitchOpsContract.h ("The hook rebind"); this file is the code that keeps them, and the
// comment here only says which line of that text each statement is:
//
//   1  gate.DetachFor(): no body may reach an original after the trampolines are gone. Busy reopens the gate and returns;
//      nothing was touched.
//   2  A slot is ours when its original is a Detours trampoline (MEM_PRIVATE). A raw vtable address lives in the image
//      (MEM_IMAGE), which is how the XeFG-only extras and a slot the vtable did not have are recognised without
//      replicating upstream's attach conditions: DetourDetach on a raw address fails the whole transaction.
//   3  The thread enumeration happens before DetourTransactionBegin(): it opens handles and allocates, and the contract
//      keeps everything that allocates outside the window between the drained detach and the commit.
//   4  All or nothing: one failed DetourDetach aborts the transaction rather than committing half a set, and a gate that
//      did not confirm the drain aborts it too.
//   5  After the commit every original is nulled. A committed detach puts the real target back into the variable; leaving
//      it there would let a later HookFGSwapchain find a non-null `o_FGSCPresent` and return without hooking the new class.
//
// The product glue that names the ten slots is HookAccess.h/HookRebind.cpp; the tests bind the same code to a dummy class
// (tests/ext/fg/fg_switch_hookrebind.cpp), which is where the round trip of a real Detours attach and detach is proven.

#include "ext/fg/outswitch/HookLifecycle.h"
#include "ext/fg/outswitch/SwitchTypes.h"

#include <windows.h> // first: detours.h reads the SDK's _AMD64_ family, which the Windows headers define

#include <detours/detours.h>
#include <ext/core/DetourThreads.h>

#include <cstddef>
#include <cstdint>

namespace ext::fg::outswitch
{
// One slot of a hook set: the variable Detours wrote the trampoline into, and the body that was attached over it.
struct HookSlot
{
    const char* name;
    void** original; // &o_X: the trampoline after an attach, the real target after a detach, null when never attached
    void* detour;    // hkX
};

// True for an address Detours allocated (a trampoline). False for null, for a raw vtable function in an image, and for a
// pointer that is not mapped any more.
inline bool IsTrampoline(const void* address)
{
    if (address == nullptr)
        return false;
    MEMORY_BASIC_INFORMATION info {};
    if (VirtualQuery(address, &info, sizeof(info)) == 0)
        return false;
    return info.Type == MEM_PRIVATE;
}

// How many of the slots are attached trampolines, and where they are, in set order.
inline size_t AttachedSlots(const HookSlot* slots, size_t count, HookSlot* out, size_t maxOut)
{
    size_t used = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if (slots[i].original == nullptr || !IsTrampoline(*slots[i].original))
            continue;
        if (out != nullptr && used < maxOut)
            out[used] = slots[i];
        ++used;
    }
    return used;
}

// How many Begin/Update attempts one detach makes. A foreign Detours transaction (another thread's LoadLibrary-time
// attach, a library create that reaches the hook) and a thread that exited between the listing and the suspension both
// clear within a few milliseconds, and both used to end as a hard ban of the target after the old chain was gone. A
// detach that still cannot open its transaction answers Busy: nothing was touched and the target is not at fault.
inline constexpr uint32_t kDetachAttempts = 3;
inline constexpr uint32_t kDetachRetryMs = 3;

// The window itself, with no local that needs unwinding (MSVC C2712): enlist the calling thread and the snapshot's handles,
// then detach every slot. `enlistResult` and `detachResult` are separate on purpose: a failed enlist is a thread that
// exited before it was suspended (retried with a fresh list), a failed detach is a slot Detours would not take off
// (Failed, never retried). The caller owns the transaction and the handles.
inline void EnlistAndDetach(HookSlot* attached, size_t attachedCount, const std::vector<HANDLE>& threads,
                            LONG& enlistResult, LONG& detachResult)
{
    enlistResult = DetourUpdateThread(GetCurrentThread());
    for (size_t i = 0; enlistResult == NO_ERROR && i < threads.size(); ++i)
        enlistResult = DetourUpdateThread(threads[i]);

    detachResult = NO_ERROR;
    for (size_t i = 0; enlistResult == NO_ERROR && i < attachedCount; ++i)
        detachResult = DetourDetach(reinterpret_cast<PVOID*>(attached[i].original), attached[i].detour);
}

// 0xE06D7363 = 'msc', a C++ throw; anything else is not understood here and keeps propagating.
inline int OnlyCppExceptions(unsigned long code)
{
    return code == 0xE06D7363UL ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

// The same window with a net under it. Detours allocates with operator new inside the window (DetourUpdateThread and
// DetourDetach) and frees on the commit, and the product builds with /EHsc, where a catch that guards only an extern "C"
// declaration is dropped (measured; the reasoning is in ext/ffxiv/quality/NativeQualityTransaction.h). False means a C++
// exception escaped with the transaction open: the caller aborts, which resumes every enlisted thread.
inline bool EnlistAndDetachGuarded(HookSlot* attached, size_t attachedCount, const std::vector<HANDLE>& threads,
                                   LONG& enlistResult, LONG& detachResult)
{
    __try
    {
        EnlistAndDetach(attached, attachedCount, threads, enlistResult, detachResult);
        return true;
    }
    __except (OnlyCppExceptions(GetExceptionCode()))
    {
        enlistResult = ERROR_NOT_ENOUGH_MEMORY;
        detachResult = NO_ERROR;
        return false;
    }
}

// The thread list one detach attempt starts from. The default is the process's own: a fresh snapshot per attempt, so a
// thread that exited since the previous attempt is not enlisted again (that exit is the race the retry exists for).
struct FreshThreadList
{
    ext::core::DetourThreadScope operator()(uint32_t /*attempt*/) const { return ext::core::DetourThreadScope(); }
};

// The whole rebind for one hook set, with the wait's clock passed in so the simulation can drive it.
//   Ok      the attached slots are detached, their originals nulled and the gate stays closed (cleared) until the new
//           class is hooked: upstream's HookFGSwapchain ends with HookGateFor().Reattached(), on its attach-failure path
//           too, so the bodies run again once the new set exists. Nothing was attached: the gate is reopened here.
//   Busy    a body or a pass-through did not leave within timeoutMs, the thread listing failed, or the transaction could
//           not be opened or enlisted within kDetachAttempts (a foreign transaction, a thread that exited): nothing was
//           touched, the gate is reopened, and the caller retries after its own pause without remembering the target.
//   Failed  a slot Detours refused to detach, a gate that did not confirm the drain, or a failed commit: nothing was
//           detached, the gate is reopened, and the caller remembers the target (Fail::HookFailed).
template <class NowFn, class SleepFn, class ThreadListFn = FreshThreadList>
UnhookResult UnhookSlotsFor(HookSlot* slots, size_t count, HookGate& gate, uint32_t timeoutMs, NowFn now, SleepFn sleep,
                            size_t* detachedOut = nullptr, bool* stalledOut = nullptr,
                            ThreadListFn listThreads = ThreadListFn {})
{
    if (detachedOut != nullptr)
        *detachedOut = 0;
    if (stalledOut != nullptr)
        *stalledOut = false;

    // 1: wait for the bodies that may still reach an original.
    if (!gate.DetachFor(timeoutMs, now, sleep))
    {
        gate.Reattached();
        return UnhookResult::Busy;
    }

    // 2: what is really attached.
    HookSlot attached[kHookSlots];
    const size_t attachedCount = AttachedSlots(slots, count, attached, kHookSlots);
    if (attachedCount == 0)
    {
        // A plain target has no hooks to detach: the detach is over and the gate reopens at once. The originals are
        // nulled as well, so a leftover raw pointer cannot make the next HookFGSwapchain return at its guard.
        if (!gate.Complete())
        {
            gate.Reattached();
            return UnhookResult::Failed;
        }
        for (size_t i = 0; i < count; ++i)
            if (slots[i].original != nullptr)
                *slots[i].original = nullptr;
        gate.Reattached();
        return UnhookResult::Ok;
    }
    if (attachedCount > kHookSlots)
    {
        gate.Reattached();
        return UnhookResult::Failed; // cannot happen with the ten slots; refuse rather than detach half a set
    }

    // 3: the thread enumeration, then the transaction, retried as one attempt. The enumeration runs before
    // DetourTransactionBegin (it opens handles and allocates), and every attempt takes a FRESH list: a thread that died
    // since the previous attempt is not enlisted again, which is the exit race that used to fail the whole detach. The
    // only allocations left inside the window are Detours' own, and DetourStallWatch bounds the stall they can cause.
    for (uint32_t attempt = 0; attempt < kDetachAttempts; ++attempt)
    {
        if (attempt != 0)
            sleep(kDetachRetryMs);

        ext::core::DetourThreadScope threads = listThreads(attempt);
        if (!threads.Ok())
            continue; // the listing itself failed: a moment, not a detach that was refused

        LONG result = DetourTransactionBegin();
        if (result != NO_ERROR)
        {
            // ERROR_INVALID_OPERATION is exactly "Begin did not take the transaction": another thread has one open. Any
            // other result means Begin took it before failing, so it is closed here or every later Begin answers 4317.
            if (result != ERROR_INVALID_OPERATION)
                DetourTransactionAbort();
            continue;
        }

        ext::core::DetourStallWatch watch(threads.Handles());
        LONG enlist = NO_ERROR;
        LONG detach = NO_ERROR;
        const bool ran = EnlistAndDetachGuarded(attached, attachedCount, threads.Handles(), enlist, detach);

        // 4: the gate confirms the drain before the transaction commits, and a refused slot aborts the whole set.
        const bool drained = detach == NO_ERROR && gate.Complete();
        bool committed = false;
        if (ran && drained)
        {
            committed = DetourTransactionCommit() == NO_ERROR;
            watch.Disarm();
        }
        else
        {
            DetourTransactionAbort();
            watch.Disarm();
        }
        if (stalledOut != nullptr && watch.Resumed())
            *stalledOut = true;

        if (committed)
        {
            // 5: the committed detach restored the real targets; nulling them is what lets the next class be hooked, and
            // stops a body from calling code Detours freed with the transaction.
            for (size_t i = 0; i < count; ++i)
                if (slots[i].original != nullptr)
                    *slots[i].original = nullptr;

            if (detachedOut != nullptr)
                *detachedOut = attachedCount;
            return UnhookResult::Ok;
        }

        if (detach != NO_ERROR)
        {
            gate.Reattached();
            return UnhookResult::Failed; // a slot Detours would not take off: the trampolines are still valid
        }
        if (enlist != NO_ERROR)
            continue; // a thread that exited before it was suspended: a fresh list may enlist all of them

        // The window ran and the gate did not confirm the drain, or the commit failed: nothing was detached.
        gate.Reattached();
        return UnhookResult::Failed;
    }

    gate.Reattached();
    return UnhookResult::Busy; // the transaction stayed unavailable: retried later, never remembered
}

// Product glue (HookRebind.cpp). UnhookHookedSwapchain() names the ten slots and logs the answer; it is called only from
// the bridge's present thread and never from inside a hooked body.
UnhookResult UnhookHookedSwapchain();

// A read-only look at the one part of a detach that cannot be retried for long: another thread's pending Detours
// transaction. Begin answers ERROR_INVALID_OPERATION exactly when it did not take the process-wide transaction, so this
// opens and aborts one (nothing enlisted, nothing patched). The switch asks before it detaches and tears anything down, so
// a foreign transaction is a refusal that costs nothing instead of a teardown followed by a hard ban.
bool HooksTransactable();
} // namespace ext::fg::outswitch
