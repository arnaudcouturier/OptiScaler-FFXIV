// The hook rebind (ext/fg/outswitch/HookRebind.h) against real Detours and a dummy COM class: the round trip the sim can
// only model. The gate rules live in HookLifecycle.h and are unit-tested next to it; this file proves the part that needs
// the real library: a vtable detour is attached, the rebind detaches exactly the trampolines (never a raw address), the
// originals are nulled so the next class can be hooked, and a body inside the gate stops the detach before anything is
// touched.
#include "ext/fg/outswitch/HookRebind.h"
#include "ext/fg/outswitch/HookScope.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <new>
#include <thread>
#include <windows.h>

using namespace ext::fg::outswitch;

static int g_failed = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("  FAIL line %d: %s\n", __LINE__, #cond);                                                      \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{
// A dummy swapchain: two vtable slots with distinct real bodies plus a non-virtual function used as the "raw address"
// slot (a body upstream never attached on this class, the XeFG-only extras).
struct FakeChain
{
    void** vtable;
};

int g_presentCalls = 0;
int g_resizeCalls = 0;
int g_presentDetourCalls = 0;
int g_resizeDetourCalls = 0;
int g_rawCalls = 0;

HRESULT __stdcall RealPresent(FakeChain* self, UINT sync, UINT flags)
{
    (void)self;
    (void)sync;
    (void)flags;
    ++g_presentCalls;
    return 0;
}

HRESULT __stdcall RealResize(FakeChain* self, UINT width, UINT height)
{
    (void)self;
    (void)width;
    (void)height;
    ++g_resizeCalls;
    return 0;
}

HRESULT __stdcall DetourPresent(FakeChain* self, UINT sync, UINT flags)
{
    (void)self;
    (void)sync;
    (void)flags;
    ++g_presentDetourCalls;
    return 700;
}

HRESULT __stdcall DetourResize(FakeChain* self, UINT width, UINT height)
{
    (void)self;
    (void)width;
    (void)height;
    ++g_resizeDetourCalls;
    return 701;
}

HRESULT __stdcall RawOnly(FakeChain* self)
{
    (void)self;
    ++g_rawCalls;
    return 702;
}

using PresentFn = HRESULT(__stdcall*)(FakeChain*, UINT, UINT);
using ResizeFn = HRESULT(__stdcall*)(FakeChain*, UINT, UINT);

// The two originals the bodies would call, exactly as the `o_*` statics hold them.
PresentFn o_Present = nullptr;
ResizeFn o_Resize = nullptr;
HRESULT(__stdcall* o_Raw)(FakeChain*) = nullptr;

uint64_t g_clock = 0;
uint64_t NowFake() { return g_clock; }
void SleepFake(uint32_t ms) { g_clock += ms; }

size_t Slots(HookSlot* out, size_t maxOut)
{
    const HookSlot all[] = {
        { "Present", reinterpret_cast<void**>(&o_Present), reinterpret_cast<void*>(&DetourPresent) },
        { "Resize", reinterpret_cast<void**>(&o_Resize), reinterpret_cast<void*>(&DetourResize) },
        { "Raw", reinterpret_cast<void**>(&o_Raw), reinterpret_cast<void*>(&RawOnly) },
    };
    size_t written = 0;
    for (const HookSlot& slot : all)
        if (written < maxOut)
            out[written++] = slot;
    return written;
}

// The attach upstream does: read the real bodies into the originals, then one transaction over them.
void AttachAll(FakeChain& chain, HookSlot* slots, size_t count)
{
    (void) chain;
    o_Present = &RealPresent;
    o_Resize = &RealResize;
    o_Raw = &RawOnly;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    for (size_t i = 0; i < count; ++i)
    {
        // Only the two vtable slots are attached; the third is a raw address on purpose.
        if (slots[i].name[0] == 'R' && slots[i].name[1] == 'a')
            continue;
        DetourAttach(reinterpret_cast<PVOID*>(slots[i].original), slots[i].detour);
    }
    CHECK(DetourTransactionCommit() == NO_ERROR);
    // The vtable keeps the real addresses: Detours patches the function itself, so every caller goes to the detour
    // while it is attached and straight back to the real body afterwards.
}

void RoundTrip()
{
    std::printf("  a real Detours attach and the rebind's detach\n");
    void* vtable[2] = { reinterpret_cast<void*>(&RealPresent), reinterpret_cast<void*>(&RealResize) };
    FakeChain chain { vtable };
    HookSlot slots[8] {};
    const size_t count = Slots(slots, 8);
    CHECK(count == 3);

    AttachAll(chain, slots, count);
    CHECK(IsTrampoline(*slots[0].original));
    CHECK(IsTrampoline(*slots[1].original));
    CHECK(!IsTrampoline(*slots[2].original)); // a raw function lives in an image, and the rebind must leave it alone

    // Through the hooked vtable: the detour answers.
    const auto present = reinterpret_cast<PresentFn>(chain.vtable[0]);
    const auto resize = reinterpret_cast<ResizeFn>(chain.vtable[1]);
    CHECK(present(&chain, 1, 0) == 700);
    CHECK(resize(&chain, 8, 8) == 701);
    CHECK(g_presentDetourCalls == 1 && g_resizeDetourCalls == 1);
    CHECK(g_presentCalls == 0 && g_resizeCalls == 0);

    // The rebind: one Ok, both trampolines gone, the originals nulled, the gate closed for the next class.
    HookGate gate;
    size_t detached = 0;
    CHECK(UnhookSlotsFor(slots, count, gate, kHookDrainTimeoutMs, &NowFake, &SleepFake, &detached) == UnhookResult::Ok);
    CHECK(detached == 2); // the raw slot was not counted, and not passed to DetourDetach
    CHECK(slots[0].original != nullptr && *slots[0].original == nullptr);
    CHECK(slots[1].original != nullptr && *slots[1].original == nullptr);
    // Every original is nulled, the raw one included: it names the old class's vtable entry, and upstream's rehook
    // reads the new class's. Not being a trampoline, it was never passed to DetourDetach (detached == 2 above).
    CHECK(slots[2].original != nullptr && *slots[2].original == nullptr);
    CHECK(gate.Cleared());
    CHECK(gate.Draining()); // the gate stays closed until the new class is hooked

    // Through the restored vtable: the real bodies answer again.
    CHECK(present(&chain, 1, 0) == 0);
    CHECK(resize(&chain, 8, 8) == 0);
    CHECK(g_presentCalls == 1 && g_resizeCalls == 1);

    // The next class is hooked through upstream's own path, which starts over only because the originals are null.
    CHECK(o_Present == nullptr && o_Resize == nullptr);
    std::printf("    [ok] two slots detached with real Detours, the raw slot untouched, originals nulled\n");
}

void BusyBodyStopsTheDetach()
{
    std::printf("  a body inside the gate stops the detach before anything is touched\n");
    void* vtable[2] = { reinterpret_cast<void*>(&RealPresent), reinterpret_cast<void*>(&RealResize) };
    FakeChain chain { vtable };
    HookSlot slots[8] {};
    const size_t count = Slots(slots, 8);
    AttachAll(chain, slots, count);

    HookGate gate;
    CHECK(gate.Enter() == GateEntry::Counted); // the game's present thread is inside the hook
    size_t detached = 0;
    CHECK(UnhookSlotsFor(slots, count, gate, kHookDrainTimeoutMs, &NowFake, &SleepFake, &detached) ==
          UnhookResult::Busy);
    CHECK(detached == 0);
    CHECK(slots[0].original != nullptr && *slots[0].original != nullptr);
    CHECK(!gate.Draining());     // an abandoned detach reopened the gate
    CHECK(IsTrampoline(*slots[0].original));
    const auto present = reinterpret_cast<PresentFn>(chain.vtable[0]);
    CHECK(present(&chain, 1, 0) == 700); // the detour is still attached and still answers
    gate.Leave();

    // With the body gone the same call detaches.
    CHECK(UnhookSlotsFor(slots, count, gate, kHookDrainTimeoutMs, &NowFake, &SleepFake, &detached) == UnhookResult::Ok);
    CHECK(detached == 2);
    CHECK(present(&chain, 1, 0) == 0);
    std::printf("    [ok] Busy touched nothing and reopened the gate; the retry detaches\n");
}

// A foreign transaction: another thread holds the one process-wide Detours transaction open while the rebind runs. Begin
// answers ERROR_INVALID_OPERATION for as long as it does, which is a moment of someone else's work, not a broken hook set:
// the rebind retries and answers Busy, and Busy is not remembered, so the target is never banned for it.
void ForeignTransactionIsBusy()
{
    std::printf("  another thread's pending transaction is Busy, never Failed\n");
    void* vtable[2] = { reinterpret_cast<void*>(&RealPresent), reinterpret_cast<void*>(&RealResize) };
    FakeChain chain { vtable };
    HookSlot slots[8] {};
    const size_t count = Slots(slots, 8);
    AttachAll(chain, slots, count);

    const HANDLE opened = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<LONG> began { -1 };
    std::thread other([&] {
        began.store(DetourTransactionBegin());
        SetEvent(opened);
        WaitForSingleObject(release, 5000);
        DetourTransactionAbort();
    });
    WaitForSingleObject(opened, 5000);
    CHECK(began.load() == NO_ERROR);

    HookGate gate;
    size_t detached = 0;
    CHECK(UnhookSlotsFor(slots, count, gate, kHookDrainTimeoutMs, &NowFake, &SleepFake, &detached) ==
          UnhookResult::Busy);
    CHECK(detached == 0);                                  // nothing was detached
    CHECK(IsTrampoline(*slots[0].original));                // the trampolines are still there
    CHECK(!gate.Draining());                                // and the gate is open again
    const auto present = reinterpret_cast<PresentFn>(chain.vtable[0]);
    CHECK(present(&chain, 1, 0) == 700);                   // the old class is still hooked and working

    SetEvent(release);
    other.join();
    CloseHandle(opened);
    CloseHandle(release);

    CHECK(UnhookSlotsFor(slots, count, gate, kHookDrainTimeoutMs, &NowFake, &SleepFake, &detached) == UnhookResult::Ok);
    CHECK(detached == 2);
    CHECK(present(&chain, 1, 0) == 0);
    std::printf("    [ok] a foreign transaction costs a Busy retry, and the retry after it detaches\n");
}

// The stall bound. Detours allocates and frees through the process heap while the threads it enlisted are suspended, so a
// thread suspended inside that heap's lock would hold the installing thread forever. The watch resumes the enlisted
// threads after kDetourStallMs; a second ResumeThread on a running thread is what Detours' own resume does too.
void StallWatchResumesSuspendedThreads()
{
    std::printf("  the stall watch resumes a suspended thread after its bound\n");
    // The watch is armed when its constructor returns: the worker is already inside its wait, so a window that opens in
    // the next instant still finds a worker that can resume the threads it suspended.
    {
        ext::core::DetourStallWatch armed { std::vector<HANDLE> { GetCurrentThread() }, 10'000 };
        CHECK(armed.Armed());
        CHECK(!armed.Resumed());
        armed.Disarm();
    }

    const HANDLE start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE thread = CreateThread(
        nullptr, 0,
        [](void* context) -> DWORD {
            WaitForSingleObject(static_cast<HANDLE>(context), 5000);
            return 0;
        },
        start, 0, nullptr);
    CHECK(thread != nullptr);
    CHECK(SuspendThread(thread) == 0); // the state Detours leaves an enlisted thread in

    {
        ext::core::DetourStallWatch watch { std::vector<HANDLE> { thread }, 100 };
        Sleep(300); // longer than the bound, shorter than the thread's own wait
        CHECK(watch.Resumed());
        SetEvent(start); // only a resumed thread reaches this
        CHECK(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0); // it ran to its end: it was resumed
    }
    CloseHandle(start);
    CloseHandle(thread);

    // A window that closes in time is never resumed, and the watch owns the thread it started.
    const HANDLE second = CreateThread(nullptr, 0, [](void*) -> DWORD { return 0; }, nullptr, 0, nullptr);
    WaitForSingleObject(second, 2000);
    {
        ext::core::DetourStallWatch quick { std::vector<HANDLE> { second }, 5000 };
        quick.Disarm();
        CHECK(!quick.Resumed());
    }
    CloseHandle(second);
    std::printf("    [ok] the bound resumes what Detours suspended, and a closed window is left alone\n");
}

// The same foreign transaction, but it closes during the wait between two attempts: the rebind's retry must then detach. A
// single attempt (the mutant) answers Busy here even though the transaction was gone a millisecond later, and the switch
// would roll back for nothing.
void ForeignTransactionThatClearsIsRetried()
{
    std::printf("  a transaction that clears between two attempts is retried, not given up on\n");
    void* vtable[2] = { reinterpret_cast<void*>(&RealPresent), reinterpret_cast<void*>(&RealResize) };
    FakeChain chain { vtable };
    HookSlot slots[8] {};
    const size_t count = Slots(slots, 8);
    AttachAll(chain, slots, count);

    const HANDLE opened = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread other([&] {
        if (DetourTransactionBegin() != NO_ERROR)
            return;
        SetEvent(opened);
        WaitForSingleObject(release, 5000);
        DetourTransactionAbort();
    });
    WaitForSingleObject(opened, 5000);

    HookGate gate;
    size_t detached = 0;
    uint64_t clock = 0;
    const UnhookResult result = UnhookSlotsFor(
        slots, count, gate, kHookDrainTimeoutMs, [&] { return clock; },
        [&](uint32_t ms) {
            clock += ms;
            SetEvent(release); // the other transaction closes while this detach waits for its next attempt
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        },
        &detached);
    other.join();
    CloseHandle(opened);
    CloseHandle(release);

    CHECK(result == UnhookResult::Ok); // the second attempt found the transaction free
    CHECK(detached == 2);
    const auto present = reinterpret_cast<PresentFn>(chain.vtable[0]);
    CHECK(present(&chain, 1, 0) == 0); // the real body answers again
    std::printf("    [ok] the retry after a foreign transaction detaches the set\n");
}

// The retry that exists for a thread which exited between the listing and the suspension: the first attempt's list
// carries a dead handle, Detours refuses to enlist it, and a fresh list on the next attempt detaches the set. A list that
// is dead on every attempt is Busy, never Failed: nothing was touched and the target is not at fault.
void DeadThreadHandleIsRetried()
{
    std::printf("  a thread that exited before the suspension costs one attempt, not the detach\n");
    void* vtable[2] = { reinterpret_cast<void*>(&RealPresent), reinterpret_cast<void*>(&RealResize) };
    FakeChain chain { vtable };
    HookSlot slots[8] {};
    const size_t count = Slots(slots, 8);
    AttachAll(chain, slots, count);

    // A thread handle the thread has already left: DetourUpdateThread answers a failure for it, and the transaction has
    // to be abandoned and retried with a list that no longer holds it.
    // A fresh thread that has already exited, per attempt: the scope owns the handle and closes it, so passing the same
    // value twice would close (and later reuse) a handle the process may have handed out again.
    auto exitedThread = []() {
        const HANDLE thread = CreateThread(nullptr, 0, [](void*) -> DWORD { return 0; }, nullptr, 0, nullptr);
        if (thread != nullptr)
            WaitForSingleObject(thread, 5000);
        return thread;
    };

    HookGate gate;
    size_t detached = 0;
    uint32_t lists = 0;
    const UnhookResult result = UnhookSlotsFor(
        slots, count, gate, kHookDrainTimeoutMs, &NowFake, &SleepFake, &detached, nullptr, [&](uint32_t attempt) {
            ++lists;
            if (attempt == 0)
                return ext::core::DetourThreadScope { std::vector<HANDLE> { exitedThread() } };
            return ext::core::DetourThreadScope {};
        });
    CHECK(lists == 2); // the failed attempt, then the one that detached
    CHECK(result == UnhookResult::Ok);
    CHECK(detached == 2);
    const auto present = reinterpret_cast<PresentFn>(chain.vtable[0]);
    CHECK(present(&chain, 1, 0) == 0); // the real body answers again
    std::printf("    [ok] a dead thread handle costs the first attempt, and the retry detaches the set\n");

    // Every attempt carries it: nothing is detached, and the answer is Busy (transient, never a ban of the target).
    AttachAll(chain, slots, count);
    HookGate busyGate;
    uint32_t attempts = 0;
    const UnhookResult busy = UnhookSlotsFor(
        slots, count, busyGate, kHookDrainTimeoutMs, &NowFake, &SleepFake, nullptr, nullptr, [&](uint32_t) {
            ++attempts;
            return ext::core::DetourThreadScope { std::vector<HANDLE> { exitedThread() } };
        });
    CHECK(attempts == kDetachAttempts);
    CHECK(busy == UnhookResult::Busy);
    CHECK(IsTrampoline(*slots[0].original)); // the trampolines are still valid: nothing was detached
    // Leave the globals as they were found: the set is detached with a gate of its own, so the next scenario starts from
    // a process with nothing attached.
    HookGate after;
    CHECK(UnhookSlotsFor(slots, count, after, kHookDrainTimeoutMs, &NowFake, &SleepFake) == UnhookResult::Ok);
}

void NothingAttached()
{
    std::printf("  a hook set with no trampolines (a plain class) answers Ok and reopens the gate\n");
    HookSlot slots[8] {};
    const size_t count = Slots(slots, 8);
    HookGate gate;
    CHECK(gate.Enter() == GateEntry::Counted);
    gate.Leave();
    size_t detached = 99;
    CHECK(UnhookSlotsFor(slots, count, gate, kHookDrainTimeoutMs, &NowFake, &SleepFake, &detached) == UnhookResult::Ok);
    CHECK(detached == 0);
    CHECK(!gate.Cleared()); // nothing to rehook: the gate is open, not waiting for a new class
    CHECK(!gate.Draining());
    // A gate that was never drained refuses to confirm: the detach reports Failed rather than clearing anything.
    HookGate never;
    CHECK(never.Complete() == false);
    std::printf("    [ok] an empty set costs one wait and no detach\n");
}
} // namespace

int main()
{
    RoundTrip();
    BusyBodyStopsTheDetach();
    ForeignTransactionIsBusy();
    ForeignTransactionThatClearsIsRetried();
    DeadThreadHandleIsRetried();
    StallWatchResumesSuspendedThreads();
    NothingAttached();

    if (g_failed == 0)
        std::printf("ALL HOOK REBIND SCENARIOS PASS\n");
    else
        std::printf("%d CHECK(S) FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
