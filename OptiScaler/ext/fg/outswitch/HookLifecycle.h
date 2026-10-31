#pragma once
// The swapchain-hook rebind as a state machine: what makes it safe to restore the ten vtable slots.
//
// The hook bodies call the `o_*` trampolines, and another thread may already be inside one of them when a switch runs
// (a present on the game's thread, a resize on a second thread, the XeFG present worker). Detours suspends the threads
// whose instruction pointer is inside a patched prologue; it does not stop a thread that has already entered a hook
// body. The trampoline memory is freed when the detach commits, so a body that calls one after that commit runs off
// freed code. A detach therefore has to wait for every body that may still reach an original.
//
// A body asks the gate how it must behave:
//   Counted      the normal path: the gate is open, the body runs the hook's own logic and calls Leave() when it exits
//   PassThrough  a detach is in progress: the body must run none of the hook's logic, call the original for its slot and
//                return that result. The detach waits for it (a pass-through is counted in its own counter), so the
//                original is still valid for the whole call
//   Refused      the detach restored the originals: there is no original left to call, so the body returns the fixed
//                answer its slot defines (stated per body in SwitchOpsContract.h)
//
// Closing the counted gate BEFORE the count is read is what makes the counted path sound: a body that increments after
// the detach closed the gate observes the closed gate on its second read and leaves the count again before it can call
// an original, so a detach that saw zero cannot have an uncounted body behind it. A pass-through runs the same
// double-check against the pass-through phase: it increments that counter first and only calls the original when the
// phase is still open, so a detach that closes the phase and then sees the counter at zero cannot have a call behind it.
// Every load and store on both sides is sequentially consistent, which is what rules out the store-buffer interleaving
// (each side writes its flag and reads the other's); the two phases are the same argument applied twice.
//
// One gate per hook body, not one per class or per slot: the ten slots are attached and detached as one set, and the
// next class's slot pointers are written only after the transaction committed, so a per-slot gate would multiply the
// wait without making anything safer. The gate outlives every class (HookGateFor).
//
// The counter is written by hook bodies on any thread and read by the switch on the present thread.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ext::fg::outswitch
{
// How long a detach waits for the hook bodies and the pass-throughs before it gives up and leaves the hooks attached.
inline constexpr uint32_t kHookDrainTimeoutMs = 100;
// How often the wait looks at the counters. Small: a body that leaves 20 ms in is seen at once, and the whole wait is
// one present's worth of time at most.
inline constexpr uint32_t kHookDrainPollMs = 2;
// The swapchain vtable slots upstream hooks (SwitchOpsContract.h holds the answer of every one). One set of ten; a class
// attaches a subset (the XeFG-only extras), so which slots are attached is decided per slot when the set is detached.
inline constexpr size_t kHookSlots = 10;

enum class GateEntry : uint8_t
{
    Counted,
    PassThrough,
    Refused
};

class HookGate
{
  public:
    // A hook body starts. Counted: run the hook's logic and call Leave(). PassThrough: call the original for the slot and
    // call PassDone(). Refused: return the slot's fixed answer, call nothing.
    GateEntry Enter()
    {
        // The count is taken first and the gate read again after it. A detach that closes the gate between the two
        // reads is seen by the second one, and this entrant then gives the count back before it can call an original.
        if (draining_.load(std::memory_order_seq_cst))
            return TryPassThrough();
        if (seam_ != nullptr)
            seam_(seamContext_); // a test can start the detach exactly between the read above and the increment below
        inFlight_.fetch_add(1, std::memory_order_seq_cst);
        if (draining_.load(std::memory_order_seq_cst))
        {
            inFlight_.fetch_sub(1, std::memory_order_seq_cst);
            return TryPassThrough();
        }
        return GateEntry::Counted;
    }

    // A counted hook body ends. The release makes the count visible to the detach that is waiting for it.
    void Leave() { inFlight_.fetch_sub(1, std::memory_order_seq_cst); }

    // A pass-through body ends: it called the original (or decided not to, when the phase had already closed).
    void PassDone() { passThroughs_.fetch_sub(1, std::memory_order_seq_cst); }

    // Starts a detach. True means no counted body is in flight and the caller may close the pass-through phase; false
    // means the caller must wait. A detach that was abandoned can be started again: the store is idempotent and the
    // counters are what decide.
    bool BeginDetach()
    {
        draining_.store(true, std::memory_order_seq_cst);
        return inFlight_.load(std::memory_order_seq_cst) == 0;
    }

    bool Ready() const
    {
        return draining_.load(std::memory_order_seq_cst) && inFlight_.load(std::memory_order_seq_cst) == 0;
    }

    // The whole wait a detach does before it clears an original: close the counted gate, wait for the bodies inside,
    // close the pass-through phase, then wait for the pass-throughs that are still inside their call. One timeout bounds
    // both waits, so a body that never leaves cannot hold the switch longer than kHookDrainTimeoutMs.
    //   true   no body can reach an original any more; Complete() may clear the slots
    //   false  a body or a pass-through did not leave in time: the caller reopens the gate (Reattached) and reports Busy
    template <class NowFn, class SleepFn> bool DetachFor(uint32_t timeoutMs, NowFn now, SleepFn sleep)
    {
        BeginDetach();
        const uint64_t start = now();
        while (!Ready())
        {
            if (now() - start >= timeoutMs)
                return false;
            sleep(kHookDrainPollMs);
        }

        passClosed_.store(true, std::memory_order_seq_cst);
        while (passThroughs_.load(std::memory_order_seq_cst) != 0)
        {
            if (now() - start >= timeoutMs)
                return false;
            sleep(kHookDrainPollMs);
        }
        drained_.store(true, std::memory_order_seq_cst);
        return true;
    }

    // The originals were cleared and the new class may be hooked. False means DetachFor never answered true (the caller
    // did not wait, or the wait was abandoned): nothing is cleared, and the caller treats the detach as Busy rather than
    // assume the originals are gone. The counted bodies are not re-checked here: an entry that arrives after the wait
    // sees the closed gate on its second read and backs out without calling an original, which is exactly what the
    // double check is for, so a transient count here is not a reason to fail a drained detach.
    bool Complete()
    {
        if (!drained_.load(std::memory_order_seq_cst) || !passClosed_.load(std::memory_order_seq_cst))
            return false;
        detached_.store(true, std::memory_order_release);
        cleared_.store(true, std::memory_order_release);
        return true;
    }

    bool Draining() const { return draining_.load(std::memory_order_seq_cst); }
    bool Detached() const { return detached_.load(std::memory_order_seq_cst); }
    bool Cleared() const { return cleared_.load(std::memory_order_seq_cst); }
    uint32_t InFlight() const { return inFlight_.load(std::memory_order_seq_cst); }
    uint32_t Passing() const { return passThroughs_.load(std::memory_order_seq_cst); }

    // A new class was hooked, or the detach was abandoned: back to normal. Called by the rebind after every attempt, so
    // an abandoned detach does not leave the gate shut for the session (the old class stays patched as dead code).
    void Reattached()
    {
        cleared_.store(false, std::memory_order_seq_cst);
        detached_.store(false, std::memory_order_seq_cst);
        passClosed_.store(false, std::memory_order_seq_cst);
        drained_.store(false, std::memory_order_seq_cst);
        draining_.store(false, std::memory_order_seq_cst);
    }

    // A deterministic seam for the test that has to start a detach between the first gate read and the increment. The
    // product never sets one; one predictable branch per entry is the whole cost.
    using EnterSeam = void (*)(void* context);
    void SetEnterSeam(EnterSeam seam, void* context)
    {
        seam_ = seam;
        seamContext_ = context;
    }

  private:
    GateEntry TryPassThrough()
    {
        // Same argument as the counted path: increment first, then read the phase. A detach that closed the phase and
        // then saw this counter at zero cannot have this call behind it, because the two reads are sequentially
        // consistent and at least one side must see the other.
        passThroughs_.fetch_add(1, std::memory_order_seq_cst);
        if (passClosed_.load(std::memory_order_seq_cst))
        {
            passThroughs_.fetch_sub(1, std::memory_order_seq_cst);
            return GateEntry::Refused;
        }
        return GateEntry::PassThrough;
    }

    std::atomic<uint32_t> inFlight_ { 0 };
    std::atomic<uint32_t> passThroughs_ { 0 };
    std::atomic<bool> draining_ { false };
    std::atomic<bool> passClosed_ { false };
    std::atomic<bool> drained_ { false }; // DetachFor answered true: both phases were waited for
    std::atomic<bool> cleared_ { false };
    std::atomic<bool> detached_ { false };
    EnterSeam seam_ = nullptr;
    void* seamContext_ = nullptr;
};
} // namespace ext::fg::outswitch
