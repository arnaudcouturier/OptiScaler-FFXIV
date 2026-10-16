#pragma once
#include <detours/detours.h>
#include <tlhelp32.h>
#include <vector>

// RAII enlistment of every other thread in the current process into a pending Detours
// transaction, so DetourAttach/DetourDetach can relocate an instruction pointer that happens
// to sit inside a patched prologue when the transaction commits. Extracted from the pattern in
// ext::ffxiv::quality::FfxivNativeQualityHooks::DoInstall (that file still owns its own copy;
// this is shared with the sites in ext::ffxiv::companion::Native and
// ext::ffxiv::lighting::FfxivLightingCapture, which patch the same widely-shared D3D11 vtable
// slots while the game runs with multiple live threads).
//
// Usage: construct BEFORE DetourTransactionBegin(), check Ok(), then call Update() in place of
// a bare DetourUpdateThread(GetCurrentThread()) once the transaction is open. Keep the scope
// object alive until after DetourTransactionCommit()/DetourTransactionAbort() returns -- Detours
// resumes every enlisted thread through these same handles at that point, so closing them any
// earlier would leave threads suspended. Letting it go out of scope afterward closes every
// handle on every path (success, partial failure, or construction failure).
//
// Known, accepted risk: THREAD_SUSPEND_RESUME (requested on OpenThread, below) is only an
// access right -- it does not suspend anything by itself. The actual suspension happens inside
// Update()'s DetourUpdateThread calls, each of which may allocate its own per-thread bookkeeping
// while earlier threads in the same call are already suspended; DetourAttach/DetourDetach and
// DetourTransactionCommit can allocate too, all before the transaction resumes anyone. Whether
// that bookkeeping comes from the CRT heap (hazardous: another suspended thread could hold its
// lock) or elsewhere is not verified: this repo vendors only detours.h and a prebuilt
// detours.lib, no source, so Detours' internal allocator cannot be inspected here. Trampolines
// themselves are conventionally VirtualAlloc'd, but that is a different allocation path and is
// not a mitigation for this one. Not restructured here for the same reason: a wrong fix for an
// unverified hazard risks introducing a real bug in exchange for a theoretical one (see the
// longer note in DoInstall).
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
