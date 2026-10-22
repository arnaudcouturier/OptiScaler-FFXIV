#pragma once
// The Detours half of the native-quality install, moved out of FfxivNativeQualityHooks.h so each attempt can be run (and
// tested against the real detours.lib) on its own: list the game's live threads, then run ONE hook transaction over them.
// Windows and detours.h only: no product headers, no logging (a suspended thread may hold the log lock, so nothing here
// logs between DetourTransactionBegin and Commit/Abort; the caller logs the returned result afterwards).
//
// The "known, accepted risk" comment above FfxivNativeQualityHooks.h's Attempt() (what DetourUpdateThread suspends and what
// Detours allocates while other threads are suspended) applies unchanged to every attempt made through this file.

#include <windows.h>

#include <detours/detours.h>
#include <tlhelp32.h>

#include <cstddef>
#include <vector>

#include "ext/ffxiv/quality/NativeQualityRetry.h"

namespace ext::ffxiv::quality::transaction
{
// A thread handle is usable for the transaction while the thread has not exited. A handle to an exited thread cannot be
// suspended, so enlisting it fails the whole transaction (the race the retry exists for).
inline bool IsThreadLive(HANDLE thread)
{
    DWORD code = 0;
    return GetExitCodeThread(thread, &code) != FALSE && code == STILL_ACTIVE;
}

// Drops (and closes) every handle whose thread has exited; keeps the order of the rest.
inline void FilterLive(std::vector<HANDLE>& threads)
{
    size_t kept = 0;
    for (const HANDLE thread : threads)
    {
        if (IsThreadLive(thread))
            threads[kept++] = thread;
        else
            CloseHandle(thread);
    }
    threads.resize(kept); // shrinking never allocates
}

struct ThreadSnapshot
{
    bool ok = false;
    long error = 0;          // the Win32 error of the failing call when !ok
    unsigned threadId = 0;   // the thread whose open failed, 0 when not applicable
    std::vector<HANDLE> threads; // open handles, live at the time the snapshot ended; empty when !ok
};

// Every other thread of this process, opened with the rights the transaction needs and filtered to the ones still running.
// A thread that vanished between the listing and the open (ERROR_INVALID_PARAMETER) is skipped; any other open error means
// "not ready" for this attempt. THREAD_QUERY_LIMITED_INFORMATION is there for the exit-code check and GetThreadId.
inline ThreadSnapshot SnapshotLiveThreads()
{
    ThreadSnapshot out;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        out.error = static_cast<long>(GetLastError());
        return out;
    }

    THREADENTRY32 entry {};
    entry.dwSize = sizeof(entry);
    bool readable = Thread32First(snapshot, &entry) != FALSE;
    bool ready = readable;
    if (!readable)
        out.error = static_cast<long>(GetLastError());

    const DWORD self = GetCurrentThreadId();
    const DWORD process = GetCurrentProcessId();
    while (readable)
    {
        if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self)
        {
            const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                                 THREAD_QUERY_LIMITED_INFORMATION,
                                             FALSE, entry.th32ThreadID);
            if (thread != nullptr)
            {
                try
                {
                    out.threads.push_back(thread);
                }
                catch (...) // bad_alloc: do not leak the handle just opened
                {
                    CloseHandle(thread);
                    ready = false;
                    out.error = static_cast<long>(ERROR_NOT_ENOUGH_MEMORY);
                    break;
                }
            }
            else
            {
                const DWORD error = GetLastError();
                if (error != ERROR_INVALID_PARAMETER) // thread exited between the listing and the open: skip it
                {
                    ready = false;
                    out.error = static_cast<long>(error);
                    out.threadId = entry.th32ThreadID;
                    break;
                }
            }
        }
        readable = Thread32Next(snapshot, &entry) != FALSE;
    }
    CloseHandle(snapshot);

    if (!ready)
    {
        for (const HANDLE thread : out.threads)
            CloseHandle(thread);
        out.threads.clear();
        return out;
    }

    FilterLive(out.threads); // threads that exited while the list was being built
    out.ok = true;
    return out;
}

// One hook of the transaction. `target` is the variable the detour code later calls through: it is set to `rawAddress`
// immediately before DetourAttach (Detours then points it at the trampoline on commit; a detour that called a pointer still
// holding the raw address would recurse into itself), so an earlier failed attempt can never leave it stale.
struct HookSpec
{
    void** target;
    void* rawAddress;
    void* detour;
    retry::Stage stage; // what to report when this attach fails
};

struct TransactionResult
{
    long error = 0; // NO_ERROR (0) when the hooks are in place
    retry::Stage stage = retry::Stage::None;
    unsigned threadId = 0; // the thread an enlist failure was about
};

// Everything between a successful DetourTransactionBegin and the Commit or Abort that ends the transaction: enlist the calling
// thread and every handle in `threads`, attach every spec, commit, and Abort on any failure before the commit. It does not
// call Begin and does not own the handles (RunHookTransaction does both). `at` always names the step being run, so a caller
// that has to give up half way can say where.
inline void RunWindow(const HookSpec* specs, size_t count, std::vector<HANDLE>& threads, TransactionResult& out, retry::Stage& at)
{
    at = retry::Stage::EnlistCurrent;
    LONG result = DetourUpdateThread(GetCurrentThread());
    if (result != NO_ERROR)
    {
        out.stage = retry::Stage::EnlistCurrent;
        out.error = result;
    }
    for (size_t i = 0; result == NO_ERROR && i < threads.size(); ++i)
    {
        at = retry::Stage::EnlistThread;
        result = DetourUpdateThread(threads[i]);
        if (result != NO_ERROR)
        {
            out.stage = retry::Stage::EnlistThread;
            out.error = result;
            out.threadId = GetThreadId(threads[i]);
        }
    }
    for (size_t i = 0; result == NO_ERROR && i < count; ++i)
    {
        at = specs[i].stage;
        *specs[i].target = specs[i].rawAddress;
        result = DetourAttach(specs[i].target, specs[i].detour);
        if (result != NO_ERROR)
        {
            out.stage = specs[i].stage;
            out.error = result;
        }
    }
    if (result == NO_ERROR)
    {
        // Detours' own Commit allocates nothing, so no C++ exception can come out of it by itself. One that does arrive here
        // (only a third-party hook on a Win32 call Commit makes could throw it; the project has none) is NOT undone cleanly:
        // the guard answers it with Abort, and Abort frees the trampolines of targets that may already be patched. That is
        // still the lesser evil. Passing the exception on instead would reach the retry layer's catch (...), which would
        // swallow it and leave every enlisted thread suspended.
        at = retry::Stage::Commit;
        result = DetourTransactionCommit();
        if (result != NO_ERROR)
        {
            out.stage = retry::Stage::Commit;
            out.error = result;
        }
    }
    else
    {
        DetourTransactionAbort();
    }
}

inline int OnlyCppExceptions(unsigned long code)
{
    return code == 0xE06D7363UL ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH; // 0xE06D7363 = 'msc', a C++ throw
}

// RunWindow with a net under it: false when a C++ exception escaped. The net is structured exception handling, not try/catch,
// on purpose. Detours is declared extern "C"; the product builds with /EHsc, where the compiler assumes extern "C" functions
// never throw and, optimised (/O2, with or without /GL), drops a catch that only guards them. That was measured with the
// product's flags: a plain try/catch, and even a noinline noexcept(false) wrapper around each call, compile to no handler at
// all and the exception ends the process (/EHs, which the project does not use, would keep it). A __try/__except block is not
// subject to that assumption; the detours test is built with the product's optimisation flags to keep proving it. Any other
// kind of exception is not understood here and keeps propagating. What the guard can undo is a failure while enlisting or
// attaching; an exception during the Commit step is not cleanly recoverable (see the comment at that step). No object with a
// destructor lives in this function (C2712).
inline bool RunWindowGuarded(const HookSpec* specs, size_t count, std::vector<HANDLE>& threads, TransactionResult& out,
                             retry::Stage& at)
{
    __try
    {
        RunWindow(specs, count, threads, out, at);
        return true;
    }
    __except (OnlyCppExceptions(GetExceptionCode()))
    {
        return false;
    }
}

// One hook transaction: Begin, then the window (RunWindowGuarded). Takes ownership of the handles in `threads`: they are closed
// on every path, after Detours has resumed the threads through them, and the vector is left empty.
// Who owns the transaction after a failing Begin (read from the vendored detours.lib): Begin answers
// ERROR_INVALID_OPERATION (4317) exactly when it did NOT take the process-wide transaction, because another thread (or this
// thread, in an outer transaction) already has one open. That one is not ours and is left alone: no Abort. Any other failure
// comes after Begin took ownership (it makes the trampoline regions writable first and returns the result of that), so the
// transaction is open and must be aborted here, or every later Begin in the process answers 4317 for the rest of the session.
// A C++ exception escaping the window (std::bad_alloc from the operator new inside DetourUpdateThread / DetourAttach) is
// answered with Abort, which resumes every enlisted thread, and reported as ERROR_NOT_ENOUGH_MEMORY at the step in progress.
inline TransactionResult RunHookTransaction(const HookSpec* specs, size_t count, std::vector<HANDLE>& threads)
{
    TransactionResult out;
    LONG result = DetourTransactionBegin();
    if (result != NO_ERROR)
    {
        out.error = result;
        out.stage = retry::Stage::Begin;
        if (result != ERROR_INVALID_OPERATION)
            DetourTransactionAbort(); // Begin took the transaction before it failed: close it
    }
    else
    {
        retry::Stage at = retry::Stage::EnlistCurrent;
        if (!RunWindowGuarded(specs, count, threads, out, at))
        {
            // A C++ exception (std::bad_alloc from the operator new inside DetourUpdateThread / DetourAttach) escaped with
            // the transaction open and every enlisted thread suspended: abort resumes them. Reported as a failed step.
            DetourTransactionAbort();
            out = TransactionResult {};
            out.error = static_cast<long>(ERROR_NOT_ENOUGH_MEMORY);
            out.stage = at;
        }
    }

    for (const HANDLE thread : threads)
        CloseHandle(thread);
    threads.clear();
    return out;
}

// One complete install attempt as the retry sees it: a FRESH thread snapshot every time (a thread that died since the last
// attempt is no longer listed), then one transaction over it. A failed snapshot is transient; a failed transaction is
// classified by the stage and the error code (retry::ClassifyDetours). The caller does the guards before and the success
// work after. `snapshotThreads` is a parameter only so a test can make the snapshot fail or hand over chosen handles.
inline retry::StepResult AttemptHooks(const HookSpec* specs, size_t count,
                                      ThreadSnapshot (*snapshotThreads)() = &SnapshotLiveThreads)
{
    ThreadSnapshot snapshot = snapshotThreads();
    if (!snapshot.ok)
        return { retry::Outcome::Transient, retry::Stage::ThreadSnapshot, snapshot.error, snapshot.threadId };

    const TransactionResult done = RunHookTransaction(specs, count, snapshot.threads);
    return { retry::ClassifyDetours(done.stage, done.error), done.stage, done.error, done.threadId };
}
} // namespace ext::ffxiv::quality::transaction
