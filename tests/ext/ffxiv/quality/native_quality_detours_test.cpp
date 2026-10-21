// [QUICK-QR] The Detours half of the native-quality install (ext/ffxiv/quality/NativeQualityTransaction.h) against the REAL
// detours.lib, on two local functions: dead threads (an exited thread whose handle is still held cannot be suspended, so
// enlisting it fails the transaction; FilterLive removes it), a transaction another thread holds open (Detours transactions
// are process-wide), a failing attach (the transaction is aborted, the next one can begin) and a second attempt after a
// failure on a poisoned target pointer. The tests print the real error codes they see, which settles the codes the retry
// classification in NativeQualityRetry.h is built on.
#include "ext/ffxiv/quality/NativeQualityTransaction.h"

#include <cstdint>
#include <cstdio>

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
static DWORD WINAPI ParkUntilEvent(LPVOID event)
{
    WaitForSingleObject(static_cast<HANDLE>(event), INFINITE);
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
    DWORD id = 0;
};
static Parked StartParked()
{
    Parked p;
    p.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    p.thread = CreateThread(nullptr, 0, ParkUntilEvent, p.release, 0, &p.id);
    return p;
}
static void StopParked(Parked& p)
{
    SetEvent(p.release);
    WaitForSingleObject(p.thread, INFINITE);
    CloseHandle(p.thread);
    CloseHandle(p.release);
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

int main()
{
    ADeadThreadFailsTheEnlistAndNothingIsHooked();
    FilterLiveDropsAndClosesExitedThreadsAndTheRestInstallsFirstTime();
    ASecondAttemptSucceedsAfterAFailureEvenOnAPoisonedTargetPointer();
    ATransactionHeldByAnotherThreadIsReportedAndLeftAlone();
    AFailingAttachNamesItsStageAbortsAndLetsTheNextTransactionBegin();
    TheSnapshotListsOtherLiveThreadsWithTheRightsTheTransactionNeeds();
    AttemptHooksTakesAFreshSnapshotAndInstalls();
    AttemptHooksCallsAnotherComponentsOpenTransactionTransientThenInstalls();
    AttemptHooksCallsARefusedAttachPermanent();
    AFailedSnapshotIsTransientNamesItsStageAndStartsNoTransaction();
    AttemptHooksUsesTheHandlesOfItsSnapshotAndReportsTheEnlistFailure();

    if (g_failed != 0)
    {
        std::printf("native_quality_detours: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("native_quality_detours: all checks passed\n");
    return 0;
}
