// [QUICK-QR] The Detours half of the native-quality install (ext/ffxiv/quality/NativeQualityTransaction.h) against the REAL
// detours.lib, on two local functions: dead threads (an exited thread whose handle is still held cannot be suspended, so
// enlisting it fails the transaction; FilterLive removes it), a transaction another thread holds open (Detours transactions
// are process-wide), a failing attach (the transaction is aborted, the next one can begin) and a second attempt after a
// failure on a poisoned target pointer. The tests print the real error codes they see, which settles the codes the retry
// classification in NativeQualityRetry.h is built on.
#include "ext/ffxiv/quality/NativeQualityTransaction.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

using namespace ext::ffxiv::quality;
using retry::Outcome;
using retry::Stage;
using transaction::HookSpec;

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

// ---- an allocator that can fail on demand ------------------------------------------------------------------------------------
// DetourUpdateThread and DetourAttach allocate with the throwing operator new, so a failed allocation inside the transaction is
// a C++ exception thrown from inside the Detours library. Only the watched thread is counted and only one allocation fails.
static DWORD g_watchThread = 0;     // 0 = nothing is counted
static int g_newCount = 0;          // allocations made by the watched thread since it was last reset
static int g_failAt = 0;            // the 1-based allocation of the watched thread that fails, 0 = none
static bool g_failWithSeh = false;  // fail with a plain SEH exception instead of std::bad_alloc

void* operator new(size_t size)
{
    if (g_watchThread != 0 && GetCurrentThreadId() == g_watchThread)
    {
        ++g_newCount;
        if (g_failAt != 0 && g_newCount == g_failAt)
        {
            g_failAt = 0;
            if (g_failWithSeh)
                RaiseException(0xE0000001UL, 0, 0, nullptr);
            throw std::bad_alloc();
        }
    }
    if (void* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
// ---- two targets and their detours -----------------------------------------------------------------------------------------

static volatile int g_sink = 0;
__declspec(noinline) static int TargetA(int x)
{
    g_sink = g_sink + 1;
    return x * 3 + 1;
}
__declspec(noinline) static int TargetB(int x)
{
    g_sink = g_sink + 2;
    return x * 5 + 2;
}
static int (*g_origA)(int) = nullptr;
static int (*g_origB)(int) = nullptr;
static int DetourA(int x) { return g_origA(x) + 1000; }
static int DetourB(int x) { return g_origB(x) + 2000; }

// Called through volatile pointers so the compiler cannot fold the calls: the patched code is what runs.
static int (*volatile g_callA)(int) = &TargetA;
static int (*volatile g_callB)(int) = &TargetB;

static HookSpec MakeSpecA() { return { reinterpret_cast<void**>(&g_origA), reinterpret_cast<void*>(&TargetA), reinterpret_cast<void*>(&DetourA), Stage::AttachQuerySize }; }
static HookSpec MakeSpecB() { return { reinterpret_cast<void**>(&g_origB), reinterpret_cast<void*>(&TargetB), reinterpret_cast<void*>(&DetourB), Stage::AttachRendererUpdate }; }

static void Detach()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<PVOID*>(&g_origA), reinterpret_cast<PVOID>(&DetourA));
    DetourDetach(reinterpret_cast<PVOID*>(&g_origB), reinterpret_cast<PVOID>(&DetourB));
    DetourTransactionCommit();
}

static bool Unhooked() { return g_callA(2) == 7 && g_callB(2) == 12; }
static bool Hooked() { return g_callA(2) == 1007 && g_callB(2) == 2012; }

// ---- helper threads --------------------------------------------------------------------------------------------------------

static DWORD WINAPI ExitAtOnce(LPVOID) { return 0; }

// The two events a parked helper uses. `parked` is signalled by the helper itself in the same kernel call that starts the
// wait (SignalObjectAndWait), so once it is set the thread holds no user-mode lock (heap, loader) and may be suspended.
struct ParkArgs
{
    HANDLE parked;
    HANDLE release;
};
static DWORD WINAPI ParkUntilEvent(LPVOID arg)
{
    const auto* events = static_cast<const ParkArgs*>(arg);
    SignalObjectAndWait(events->parked, events->release, INFINITE, FALSE);
    return 0;
}

// A thread that has exited; the handle stays open (the race: the snapshot listed the thread, then it exited).
static HANDLE MakeDeadThread()
{
    HANDLE thread = CreateThread(nullptr, 0, ExitAtOnce, nullptr, 0, nullptr);
    WaitForSingleObject(thread, INFINITE);
    return thread;
}

struct Parked
{
    HANDLE thread = nullptr;
    HANDLE release = nullptr;
    HANDLE parked = nullptr; // set by the helper once it is inside its wait
    ParkArgs* args = nullptr;
    DWORD id = 0;
};
// Returns only after the helper is inside its wait: a transaction that suspends a thread still in its start-up code could
// catch it holding the process heap lock, and the next allocation Detours makes would then block forever.
static Parked StartParked()
{
    Parked p;
    p.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    p.parked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    p.args = new ParkArgs { p.parked, p.release };
    p.thread = CreateThread(nullptr, 0, ParkUntilEvent, p.args, 0, &p.id);
    const DWORD reached = WaitForSingleObject(p.parked, 5000);
    CHECK(reached == WAIT_OBJECT_0);
    return p;
}
static void StopParked(Parked& p)
{
    SetEvent(p.release);
    // Bounded: a thread that a failed transaction left suspended never finishes, and that must fail the test, not hang it.
    const DWORD waited = WaitForSingleObject(p.thread, 5000);
    CHECK(waited == WAIT_OBJECT_0);
    if (waited == WAIT_OBJECT_0)
        delete p.args; // the helper is gone; a stuck helper keeps its (tiny) argument block
    CloseHandle(p.thread);
    CloseHandle(p.release);
    CloseHandle(p.parked);
    p = Parked {};
}

// RunHookTransaction owns (closes) the handles it is given, so every call gets its own duplicates.
static HANDLE Dup(HANDLE h)
{
    HANDLE copy = nullptr;
    DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return copy;
}

static bool Closed(HANDLE h)
{
    DWORD flags = 0;
    return GetHandleInformation(h, &flags) == FALSE; // a closed handle value is no longer valid in this process
}

static void Reset()
{
    g_origA = nullptr;
    g_origB = nullptr;
}

// ---- tests ----------------------------------------------------------------------------------------------------------------------

static void ADeadThreadFailsTheEnlistAndNothingIsHooked()
{
    Reset();
    HANDLE dead = MakeDeadThread();
    const HookSpec specs[] = { MakeSpecA(), MakeSpecB() };
    std::vector<HANDLE> threads { Dup(dead) };
    const HANDLE given = threads[0];
    const auto result = transaction::RunHookTransaction(specs, 2, threads);
    CHECK(Closed(given)); // closed on the failure path too, not leaked
    std::printf("probe: enlisting an exited thread -> error %ld at '%s'\n", result.error, retry::StageName(result.stage));

    CHECK(result.error != 0);
    CHECK(result.stage == Stage::EnlistThread);
    CHECK(result.threadId == GetThreadId(dead));
    CHECK(threads.empty()); // the handles were consumed
    CHECK(Unhooked());      // aborted: no hook in place
    CHECK(retry::ClassifyDetours(result.stage, result.error) == Outcome::Transient);
    CloseHandle(dead);
}

static void FilterLiveDropsAndClosesExitedThreadsAndTheRestInstallsFirstTime()
{
    Reset();
    HANDLE dead = MakeDeadThread();
    Parked live = StartParked();
    HANDLE deadCopy = Dup(dead);
    std::vector<HANDLE> threads { deadCopy, Dup(live.thread) };
    transaction::FilterLive(threads);

    CHECK(threads.size() == 1);
    CHECK(!threads.empty() && GetThreadId(threads[0]) == live.id);
    DWORD flags = 0;
    CHECK(GetHandleInformation(deadCopy, &flags) == FALSE); // the dropped handle was closed, not leaked

    const HookSpec specs[] = { MakeSpecA(), MakeSpecB() };
    const auto result = transaction::RunHookTransaction(specs, 2, threads);
    CHECK(result.error == 0);
    CHECK(result.stage == Stage::None);
    CHECK(Hooked());
    Detach();
    CHECK(Unhooked());
    StopParked(live);
    CloseHandle(dead);
}

static void ASecondAttemptSucceedsAfterAFailureEvenOnAPoisonedTargetPointer()
{
    Reset();
    HANDLE dead = MakeDeadThread();
    Parked live = StartParked();
    const HookSpec specs[] = { MakeSpecA(), MakeSpecB() };

    std::vector<HANDLE> first { Dup(dead) };
    CHECK(transaction::RunHookTransaction(specs, 2, first).error != 0);

    // Whatever a failed attempt left in the pointers the detours call through, the next attempt starts from the raw address.
    g_origA = reinterpret_cast<int (*)(int)>(static_cast<uintptr_t>(0x1234));
    g_origB = reinterpret_cast<int (*)(int)>(static_cast<uintptr_t>(0x5678));

    std::vector<HANDLE> second { Dup(live.thread) };
    const HANDLE givenSecond = second[0];
    const auto result = transaction::RunHookTransaction(specs, 2, second);
    CHECK(Closed(givenSecond)); // and on the success path
    CHECK(result.error == 0);
    CHECK(g_origA != reinterpret_cast<int (*)(int)>(static_cast<uintptr_t>(0x1234)));
    CHECK(g_origA != reinterpret_cast<int (*)(int)>(&TargetA)); // the trampoline, not the raw address
    CHECK(Hooked());
    Detach();
    CHECK(Unhooked());
    StopParked(live);
    CloseHandle(dead);
}

struct Holder
{
    HANDLE ready = nullptr;
    HANDLE go = nullptr;
    LONG beginResult = -1;
    LONG abortResult = -1;
};
static DWORD WINAPI HoldATransaction(LPVOID arg)
{
    auto* h = static_cast<Holder*>(arg);
    h->beginResult = DetourTransactionBegin();
    SetEvent(h->ready);
    WaitForSingleObject(h->go, INFINITE);
    h->abortResult = DetourTransactionAbort();
    return 0;
}

static void ATransactionHeldByAnotherThreadIsReportedAndLeftAlone()
{
    Reset();
    Parked live = StartParked();
    Holder holder;
    holder.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    holder.go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE holderThread = CreateThread(nullptr, 0, HoldATransaction, &holder, 0, nullptr);
    WaitForSingleObject(holder.ready, INFINITE);
    CHECK(holder.beginResult == NO_ERROR);

    const HookSpec specs[] = { MakeSpecA(), MakeSpecB() };
    std::vector<HANDLE> threads { Dup(live.thread) };
    const auto result = transaction::RunHookTransaction(specs, 2, threads);
    std::printf("probe: Begin while another thread's transaction is open -> error %ld at '%s'\n", result.error,
                retry::StageName(result.stage));
    CHECK(result.error == static_cast<long>(ERROR_INVALID_OPERATION));
    CHECK(result.stage == Stage::Begin);
    CHECK(threads.empty());
    CHECK(Unhooked());
    CHECK(retry::ClassifyDetours(result.stage, result.error) == Outcome::Transient);

    SetEvent(holder.go);
    WaitForSingleObject(holderThread, INFINITE);
    CloseHandle(holderThread);
    CHECK(holder.abortResult == NO_ERROR); // the other component's transaction was intact until it ended it itself
    CloseHandle(holder.ready);
    CloseHandle(holder.go);

    std::vector<HANDLE> retry { Dup(live.thread) };
    const auto again = transaction::RunHookTransaction(specs, 2, retry);
    CHECK(again.error == 0);
    CHECK(Hooked());
    Detach();
    CHECK(Unhooked());
    StopParked(live);
}

// Detours answers 4317 (ERROR_INVALID_OPERATION) exactly when the transaction was NOT taken (another thread owns one, or this
// thread already has its own open). Any other failing Begin has already taken ownership: it failed while making the
// trampoline regions writable, and the caller must Abort or the process-wide transaction stays open for ever.
// The failure is injected by decommitting one page in the middle of an existing 64 KB trampoline region: Begin calls
// VirtualProtect over the whole region, and that fails when any page of the range is not committed.
static void ABeginThatFailsForAnotherReasonThanABusyTransactionIsAbortedNotLeftOpen()
{
    Reset();
    const HookSpec specA = MakeSpecA();
    const HookSpec specB = MakeSpecB();
    // Hook A first and keep it hooked: Detours releases a trampoline region as soon as nothing uses it any more.
    std::vector<HANDLE> none;
    CHECK(transaction::RunHookTransaction(&specA, 1, none).error == 0);
    const uintptr_t trampolineA = reinterpret_cast<uintptr_t>(g_origA);

    const uintptr_t base = trampolineA & ~static_cast<uintptr_t>(0xFFFF);
    uintptr_t victim = 0; // a page of the region that does not hold the trampoline of A
    for (const uintptr_t offset : { 0x8000u, 0x4000u, 0xC000u })
    {
        if ((trampolineA & ~static_cast<uintptr_t>(0xFFF)) != base + offset)
        {
            victim = base + offset;
            break;
        }
    }
    const BOOL decommitted = VirtualFree(reinterpret_cast<void*>(victim), 0x1000, MEM_DECOMMIT);
    if (!decommitted)
        std::printf("probe: could not decommit the page at %p (error %lu), trampoline of A at %p\n",
                    reinterpret_cast<void*>(victim), GetLastError(), reinterpret_cast<void*>(trampolineA));
    CHECK(decommitted != FALSE);

    std::vector<HANDLE> threads;
    const auto result = transaction::RunHookTransaction(&specB, 1, threads);
    std::printf("probe: Begin with an uncommitted page inside a trampoline region -> error %ld at '%s'\n", result.error,
                retry::StageName(result.stage));
    CHECK(result.error != 0 && result.error != static_cast<long>(ERROR_INVALID_OPERATION));
    CHECK(result.stage == Stage::Begin);

    // Give the page back, then ask whether the failed Begin left the transaction open (an open one answers 4317).
    CHECK(VirtualAlloc(reinterpret_cast<void*>(victim), 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE) != nullptr);
    const LONG next = DetourTransactionBegin();
    CHECK(next == NO_ERROR);
    DetourTransactionAbort(); // leaves the process clean even when the check above failed

    std::vector<HANDLE> again;
    CHECK(transaction::RunHookTransaction(&specB, 1, again).error == 0); // and the install works afterwards
    CHECK(Hooked());
    Detach();
    CHECK(Unhooked());
}
// The other side of the rule: a Begin refused with 4317 owns nothing, so it must not Abort. If this thread already has its own
// transaction open, an Abort here would tear that outer transaction down.
static void ABeginInsideTheSameThreadsOwnTransactionLeavesThatTransactionAlone()
{
    Reset();
    const HookSpec specs[] = { MakeSpecA(), MakeSpecB() };
    CHECK(DetourTransactionBegin() == NO_ERROR); // an outer transaction of this very thread
    std::vector<HANDLE> threads;
    const auto result = transaction::RunHookTransaction(specs, 2, threads);
    CHECK(result.error == static_cast<long>(ERROR_INVALID_OPERATION));
    CHECK(result.stage == Stage::Begin);
    CHECK(DetourTransactionAbort() == NO_ERROR); // still open and still ours (a torn-down transaction answers 4317)
    CHECK(Unhooked());
}
static void AFailingAttachNamesItsStageAbortsAndLetsTheNextTransactionBegin()
{
    Reset();
    Parked live = StartParked();
    HookSpec bad = MakeSpecB();
    bad.rawAddress = nullptr; // Detours refuses a null function
    const HookSpec specs[] = { MakeSpecA(), bad };

    std::vector<HANDLE> threads { Dup(live.thread) };
    const auto result = transaction::RunHookTransaction(specs, 2, threads);
    std::printf("probe: attaching a null function -> error %ld at '%s'\n", result.error, retry::StageName(result.stage));
    CHECK(result.error != 0);
    CHECK(result.stage == Stage::AttachRendererUpdate); // the stage of the spec that failed, not of the first
    CHECK(Unhooked());                                  // the first attach was rolled back with the transaction
    CHECK(retry::ClassifyDetours(result.stage, result.error) == Outcome::Permanent);

    // The transaction was aborted, not left open: the next one can begin (a leaked open transaction would answer 4317).
    const HookSpec good[] = { MakeSpecA(), MakeSpecB() };
    std::vector<HANDLE> next { Dup(live.thread) };
    const auto again = transaction::RunHookTransaction(good, 2, next);
    CHECK(again.error == 0);
    CHECK(Hooked());
    Detach();
    CHECK(Unhooked());
    StopParked(live);
}

// The helper must have reached its wait (and so hold no heap or loader lock) before a transaction may suspend it: a thread
// suspended while it still owns the process heap lock would block the allocation Detours makes next, and hang the test.
static void AParkedHelperIsInsideItsWaitWhenStartParkedReturns()
{
    Parked live = StartParked();
    CHECK(live.parked != nullptr && WaitForSingleObject(live.parked, 0) == WAIT_OBJECT_0);
    StopParked(live);
}

static void TheSnapshotListsOtherLiveThreadsWithTheRightsTheTransactionNeeds()
{
    Parked live = StartParked();
    auto snapshot = transaction::SnapshotLiveThreads();
    CHECK(snapshot.ok);
    bool foundParked = false;
    bool foundSelf = false;
    for (const HANDLE thread : snapshot.threads)
    {
        const DWORD id = GetThreadId(thread); // needs THREAD_QUERY_LIMITED_INFORMATION
        foundParked = foundParked || id == live.id;
        foundSelf = foundSelf || id == GetCurrentThreadId();
        CHECK(transaction::IsThreadLive(thread));
    }
    CHECK(foundParked);
    CHECK(!foundSelf);
    for (const HANDLE thread : snapshot.threads)
        CloseHandle(thread);
    StopParked(live);
}

static void AttemptHooksTakesAFreshSnapshotAndInstalls()
{
    Reset();
    Parked live = StartParked();
    const HookSpec specs[] = { MakeSpecA(), MakeSpecB() };
    const auto result = transaction::AttemptHooks(specs, 2);
    CHECK(result.outcome == Outcome::Installed);
    CHECK(result.stage == Stage::None);
    CHECK(result.error == 0);
    CHECK(Hooked());
    Detach();
    CHECK(Unhooked());
    StopParked(live);
}

static void AttemptHooksCallsAnotherComponentsOpenTransactionTransientThenInstalls()
{
    Reset();
    Parked live = StartParked();
    Holder holder;
    holder.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    holder.go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE holderThread = CreateThread(nullptr, 0, HoldATransaction, &holder, 0, nullptr);
    WaitForSingleObject(holder.ready, INFINITE);

    const HookSpec specs[] = { MakeSpecA(), MakeSpecB() };
    const auto blocked = transaction::AttemptHooks(specs, 2);
    CHECK(blocked.outcome == Outcome::Transient);
    CHECK(blocked.stage == Stage::Begin);
    CHECK(blocked.error == static_cast<long>(ERROR_INVALID_OPERATION));
    CHECK(Unhooked());

    SetEvent(holder.go);
    WaitForSingleObject(holderThread, INFINITE);
    CloseHandle(holderThread);
    CloseHandle(holder.ready);
    CloseHandle(holder.go);

    const auto again = transaction::AttemptHooks(specs, 2); // the very next attempt, as the retry makes it
    CHECK(again.outcome == Outcome::Installed);
    CHECK(Hooked());
    Detach();
    CHECK(Unhooked());
    StopParked(live);
}

static void AttemptHooksCallsARefusedAttachPermanent()
{
    Reset();
    Parked live = StartParked();
    HookSpec bad = MakeSpecA();
    bad.rawAddress = nullptr;
    const HookSpec specs[] = { bad, MakeSpecB() };
    const auto result = transaction::AttemptHooks(specs, 2);
    CHECK(result.outcome == Outcome::Permanent); // classified from the stage and the code, not assumed transient
    CHECK(result.stage == Stage::AttachQuerySize);
    CHECK(result.error != 0);
    CHECK(Unhooked());
    StopParked(live);
}

static transaction::ThreadSnapshot FailingSnapshot()
{
    transaction::ThreadSnapshot snapshot;
    snapshot.ok = false;
    snapshot.error = 5;
    snapshot.threadId = 9;
    return snapshot;
}

static HANDLE g_deadForSnapshot = nullptr;
static transaction::ThreadSnapshot SnapshotWithTheDeadThread()
{
    transaction::ThreadSnapshot snapshot;
    snapshot.ok = true;
    snapshot.threads.push_back(Dup(g_deadForSnapshot));
    return snapshot;
}

static void AFailedSnapshotIsTransientNamesItsStageAndStartsNoTransaction()
{
    Reset();
    Parked live = StartParked();
    const HookSpec specs[] = { MakeSpecA(), MakeSpecB() };
    const auto result = transaction::AttemptHooks(specs, 2, &FailingSnapshot);
    CHECK(result.outcome == Outcome::Transient);
    CHECK(result.stage == Stage::ThreadSnapshot);
    CHECK(result.error == 5);
    CHECK(result.threadId == 9);
    CHECK(Unhooked());
    // No transaction was left open by the failed attempt: the next attempt begins and installs.
    const auto again = transaction::AttemptHooks(specs, 2);
    CHECK(again.outcome == Outcome::Installed);
    CHECK(Hooked());
    Detach();
    CHECK(Unhooked());
    StopParked(live);
}

static void AttemptHooksUsesTheHandlesOfItsSnapshotAndReportsTheEnlistFailure()
{
    Reset();
    g_deadForSnapshot = MakeDeadThread();
    const HookSpec specs[] = { MakeSpecA(), MakeSpecB() };
    const auto result = transaction::AttemptHooks(specs, 2, &SnapshotWithTheDeadThread);
    CHECK(result.outcome == Outcome::Transient); // an exited thread is the transient race, never a refusal
    CHECK(result.stage == Stage::EnlistThread);
    CHECK(result.error == 5);
    CHECK(result.threadId == GetThreadId(g_deadForSnapshot));
    CHECK(Unhooked());
    CloseHandle(g_deadForSnapshot);
    g_deadForSnapshot = nullptr;
}

// ---- an exception inside the Begin..Commit window --------------------------------------------------------------------------------
// The call goes through an SEH handler of the test's own, so a missing guard in the product code fails a check instead of
// ending the test binary (a C++ try/catch in this file could be dropped by the optimiser, see NativeQualityTransaction.h).
struct GuardedCall
{
    const HookSpec* specs;
    size_t count;
    std::vector<HANDLE>* threads;
    transaction::TransactionResult result;
    DWORD code;
};
static bool CallAndCatchAnyException(GuardedCall* call)
{
    __try
    {
        call->result = transaction::RunHookTransaction(call->specs, call->count, *call->threads);
        return false;
    }
    __except (call->code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
    {
        return true;
    }
}

// Leaves the process usable after a run that did not clean up after itself: closes the open transaction (which resumes the
// enlisted threads) and the handles that were never consumed.
static void CleanUpAfterAnEscapedException(GuardedCall& call)
{
    DetourTransactionAbort();
    for (const HANDLE h : *call.threads)
        CloseHandle(h);
    call.threads->clear();
}

static void AnExceptionInsideTheWindowResumesTheThreadsAndAbortsTheTransaction()
{
    Reset();
    Parked live = StartParked();
    const HookSpec specs[] = { MakeSpecA(), MakeSpecB() };

    // How many allocations does one successful transaction make on this thread? One per enlisted thread (the calling thread
    // is dropped before Detours allocates) and one per attach: here the live thread, the attach of A and the attach of B.
    {
        std::vector<HANDLE> probe { Dup(live.thread) };
        g_newCount = 0;
        g_watchThread = GetCurrentThreadId();
        CHECK(transaction::RunHookTransaction(specs, 2, probe).error == 0);
        g_watchThread = 0;
        Detach();
    }
    const int allocations = g_newCount;
    CHECK(allocations == 3);

    // Fail each of them in turn. The step in progress at the failure is what the retry log and the classification are built
    // on, so every step has to report itself: enlisting the live thread (nothing is suspended yet), the attach of A (the live
    // thread is suspended) and the attach of B (the live thread is suspended and A's attach is pending).
    const Stage expected[] = { Stage::EnlistThread, Stage::AttachQuerySize, Stage::AttachRendererUpdate };
    for (int failAt = 1; failAt <= allocations && failAt <= 3; ++failAt)
    {
        std::vector<HANDLE> threads { Dup(live.thread) };
        GuardedCall call { specs, 2, &threads, {}, 0 };
        g_failWithSeh = false;
        g_newCount = 0;
        g_failAt = failAt;
        g_watchThread = GetCurrentThreadId();
        const bool escaped = CallAndCatchAnyException(&call);
        g_watchThread = 0;
        g_failAt = 0;
        if (escaped)
            CleanUpAfterAnEscapedException(call);

        std::printf("probe: allocation %d of the window fails -> error %ld at '%s'\n", failAt, call.result.error,
                    retry::StageName(call.result.stage));
        CHECK(!escaped); // the exception stays inside RunHookTransaction
        CHECK(call.result.error == static_cast<long>(ERROR_NOT_ENOUGH_MEMORY));
        CHECK(call.result.stage == expected[failAt - 1]); // where it happened
        CHECK(retry::ClassifyDetours(call.result.stage, call.result.error) == Outcome::Transient);
        CHECK(threads.empty()); // the handles were consumed
        CHECK(Unhooked());

        const DWORD suspendCount = SuspendThread(live.thread); // the count BEFORE this call: 0 = the thread was running
        CHECK(suspendCount == 0);
        if (suspendCount != static_cast<DWORD>(-1))
            ResumeThread(live.thread);
        const LONG next = DetourTransactionBegin(); // an open transaction would answer 4317
        CHECK(next == NO_ERROR);
        DetourTransactionAbort();
    }

    std::vector<HANDLE> again { Dup(live.thread) }; // and the next attempt installs
    CHECK(transaction::RunHookTransaction(specs, 2, again).error == 0);
    CHECK(Hooked());
    Detach();
    CHECK(Unhooked());
    StopParked(live);
}
// Only C++ exceptions are turned into a failed attempt. Anything else (an access violation inside the window, say) is not
// understood here and is left to propagate, not swallowed.
static void AnExceptionThatIsNotACppExceptionIsNotSwallowed()
{
    Reset();
    Parked live = StartParked();
    const HookSpec specs[] = { MakeSpecA(), MakeSpecB() };
    {
        std::vector<HANDLE> probe { Dup(live.thread) };
        g_newCount = 0;
        g_watchThread = GetCurrentThreadId();
        CHECK(transaction::RunHookTransaction(specs, 2, probe).error == 0);
        g_watchThread = 0;
        Detach();
    }
    const int allocations = g_newCount;

    std::vector<HANDLE> threads { Dup(live.thread) };
    GuardedCall call { specs, 2, &threads, {}, 0 };
    g_failWithSeh = true;
    g_newCount = 0;
    g_failAt = allocations;
    g_watchThread = GetCurrentThreadId();
    const bool escaped = CallAndCatchAnyException(&call);
    g_watchThread = 0;
    g_failAt = 0;
    g_failWithSeh = false;
    CHECK(escaped);
    CHECK(call.code == 0xE0000001UL);
    CleanUpAfterAnEscapedException(call); // nothing handled it, so the test closes the transaction itself
    CHECK(Unhooked());
    StopParked(live);
}
int main()
{
    ADeadThreadFailsTheEnlistAndNothingIsHooked();
    FilterLiveDropsAndClosesExitedThreadsAndTheRestInstallsFirstTime();
    ASecondAttemptSucceedsAfterAFailureEvenOnAPoisonedTargetPointer();
    ATransactionHeldByAnotherThreadIsReportedAndLeftAlone();
    ABeginThatFailsForAnotherReasonThanABusyTransactionIsAbortedNotLeftOpen();
    ABeginInsideTheSameThreadsOwnTransactionLeavesThatTransactionAlone();
    AFailingAttachNamesItsStageAbortsAndLetsTheNextTransactionBegin();
    AParkedHelperIsInsideItsWaitWhenStartParkedReturns();
    TheSnapshotListsOtherLiveThreadsWithTheRightsTheTransactionNeeds();
    AttemptHooksTakesAFreshSnapshotAndInstalls();
    AttemptHooksCallsAnotherComponentsOpenTransactionTransientThenInstalls();
    AttemptHooksCallsARefusedAttachPermanent();
    AFailedSnapshotIsTransientNamesItsStageAndStartsNoTransaction();
    AttemptHooksUsesTheHandlesOfItsSnapshotAndReportsTheEnlistFailure();
    AnExceptionInsideTheWindowResumesTheThreadsAndAbortsTheTransaction();
    AnExceptionThatIsNotACppExceptionIsNotSwallowed();

    if (g_failed != 0)
    {
        std::printf("native_quality_detours: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("native_quality_detours: all checks passed\n");
    return 0;
}
