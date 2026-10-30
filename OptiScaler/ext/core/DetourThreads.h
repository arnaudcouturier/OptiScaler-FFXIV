#pragma once
#include <detours/detours.h>
#include <tlhelp32.h>
#include <vector>

// RAII enlistment of every other thread in the current process into a pending Detours
// transaction, so DetourAttach/DetourDetach can relocate an instruction pointer that happens
// to sit inside a patched prologue when the transaction commits. Extracted from the pattern in
// ext::ffxiv::quality::FfxivNativeQualityHooks (its thread snapshot now lives in
// ext/ffxiv/quality/NativeQualityTransaction.h, which keeps its own copy because it must also skip exited threads;
// this is shared with ext::ffxiv::lighting::FfxivLightingCapture, which patches the same widely-shared D3D11 vtable
// slots while the game runs with multiple live threads).
//
// Usage: construct BEFORE DetourTransactionBegin(), check Ok(), then call Update() in place of
// a bare DetourUpdateThread(GetCurrentThread()) once the transaction is open. Keep the scope
// object alive until after DetourTransactionCommit()/DetourTransactionAbort() returns -- Detours
// resumes every enlisted thread through these same handles at that point, so closing them any
// earlier would leave threads suspended. Letting it go out of scope afterward closes every
// handle on every path (success, partial failure, or construction failure).
//
// Known, accepted risk, CONFIRMED against the real microsoft/Detours source (detours.cpp,
// v4.0.1; this repo vendors only detours.h and a prebuilt detours.lib, so the confirmation came
// from reading the upstream source directly, not from inspecting what's vendored here).
// THREAD_SUSPEND_RESUME (requested on OpenThread, below) is only an access right -- it does not
// suspend anything by itself. The actual suspension happens inside DetourUpdateThread itself
// (immediately, via SuspendThread), and the thread stays suspended through every subsequent
// DetourUpdateThread/DetourAttach call and the whole DetourTransactionCommit -- Detours only
// calls ResumeThread once, at the very end of the commit (or on abort). DetourAttach allocates
// (`new DetourOperation`, trampoline VirtualAlloc) and DetourTransactionCommit frees (`delete`,
// VirtualFree) while threads enlisted earlier in the same transaction are still suspended.
// VirtualAlloc is used for trampoline memory, but that is a separate allocation path (chosen to
// place trampolines near their target, not to avoid heap-lock contention) and is not a
// mitigation: the `new`/`delete` calls above still go through the ordinary CRT/process heap. A
// thread suspended while holding that heap's lock would deadlock the installing thread's own
// next allocation -- this is a real, confirmed hazard, not a theoretical one, and Detours itself
// does nothing to avoid it. Not restructured here regardless: the alternative (not enlisting
// other threads at all) trades this hazard for the different, also-real crash this scope exists
// to prevent -- resuming a thread whose instruction pointer sits inside a prologue this
// transaction just patched. Both are real; this file's callers only enlist on Detach (currently
// unreachable in this build) specifically so the live, every-session Install path doesn't carry
// either risk unnecessarily -- see each Install's own comment and the code-review ledger entry
// for the full trade-off.
namespace ext::core
{
class DetourThreadScope
{
  public:
    DetourThreadScope()
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return; // ok_ stays false

        THREADENTRY32 entry {};
        entry.dwSize = sizeof(entry);
        bool readable = Thread32First(snapshot, &entry) != FALSE;
        bool ready = readable;
        try
        {
            while (readable)
            {
                if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != GetCurrentThreadId())
                {
                    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE,
                                                entry.th32ThreadID);
                    if (thread != nullptr)
                        threads_.push_back(thread);
                    else if (GetLastError() != ERROR_INVALID_PARAMETER) // thread exited between snapshot and open
                        ready = false;
                }
                readable = Thread32Next(snapshot, &entry) != FALSE;
            }
        }
        catch (...)
        {
            // A throwing push_back (bad_alloc) must not leak whatever handles are already open;
            // the constructor won't finish, so our own destructor won't run either.
            ready = false;
        }
        CloseHandle(snapshot);

        if (!ready)
        {
            for (const auto thread : threads_)
                CloseHandle(thread);
            threads_.clear();
            return; // ok_ stays false
        }
        ok_ = true;
    }

    ~DetourThreadScope()
    {
        for (const auto thread : threads_)
            CloseHandle(thread);
    }

    DetourThreadScope(const DetourThreadScope&) = delete;
    DetourThreadScope& operator=(const DetourThreadScope&) = delete;

    // False if the snapshot could not be taken, or a thread could not be opened for a reason
    // other than it having already exited. Callers should abandon the transaction attempt
    // without starting it (DetourTransactionBegin was never called).
    bool Ok() const { return ok_; }

    // Enlists the current thread plus every thread captured at construction. Call once, after
    // DetourTransactionBegin() has returned NO_ERROR.
    LONG Update() const
    {
        LONG result = DetourUpdateThread(GetCurrentThread());
        for (const auto thread : threads_)
        {
            if (result != NO_ERROR)
                break;
            result = DetourUpdateThread(thread);
        }
        return result;
    }

  private:
    std::vector<HANDLE> threads_;
    bool ok_ = false;
};
} // namespace ext::core
