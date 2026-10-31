#pragma once
#include <detours/detours.h>
#include <tlhelp32.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
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
// transaction just patched. Both are real, so the callers that do enlist another thread (the
// lighting capture's patch, and the live output switch's detach) pair this scope with
// DetourStallWatch below: the watch resumes every enlisted thread after a bounded stall, so a
// thread suspended while holding the heap lock costs the bound instead of the session.
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
    // Move: the scopes are handed to the caller by value, so a caller that already listed the threads (a test, the
    // retry's injected first attempt) can supply the list. A moved-from scope owns nothing and closes nothing.
    DetourThreadScope(DetourThreadScope&& other) noexcept : threads_(std::move(other.threads_)), ok_(other.ok_)
    {
        other.threads_.clear();
        other.ok_ = false;
    }
    DetourThreadScope& operator=(DetourThreadScope&&) = delete;

    // A scope over handles the caller already opened: the test that enlists a thread which exited between the listing and
    // the suspension (the race the failed update of a transaction retries). Ok() stays true: the listing itself was fine.
    explicit DetourThreadScope(std::vector<HANDLE> threads) : threads_(std::move(threads)), ok_(true) {}

    // False if the snapshot could not be taken, or a thread could not be opened for a reason
    // other than it having already exited. Callers should abandon the transaction attempt
    // without starting it (DetourTransactionBegin was never called).
    bool Ok() const { return ok_; }

    // The handles this scope keeps alive until after the commit or abort. Read by DetourStallWatch, which resumes them
    // through the same handles when the window stalls.
    const std::vector<HANDLE>& Handles() const { return threads_; }

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

// How long a transaction window may stay open before the watch resumes every enlisted thread. The number cannot tell a
// thread suspended inside the heap lock from a commit that is slow for another reason (a hard page fault, an antivirus
// scan) at this bound, and the threads resumed at the boundary would then run a prologue that is only half written. That
// race is accepted because the transaction cannot be abandoned safely either, and the number is not lowered for it: it is
// the time a legitimate window needs on this process (a few milliseconds, measured at boot).
inline constexpr uint32_t kDetourStallMs = 500;

// The bound on the hazard above. Detours allocates and frees through the process heap between the first
// DetourUpdateThread and the commit, so a thread suspended inside that heap's lock stalls the installing thread for as
// long as the commit never returns. This object is constructed BEFORE DetourTransactionBegin (it starts a thread and
// copies the handle list, and both allocate: they must not happen once other threads are suspended), waits up to stallMs,
// and resumes every enlisted thread through the same handles Detours resumes itself. A second ResumeThread on a running
// thread is harmless (it answers the previous suspend count, which Detours ignores), and the transaction keeps working:
// its own resume at the end finds nothing left to resume. Disarm (also the destructor) joins the worker and tells whether
// it fired, so the caller can log it after the window closed.
class DetourStallWatch
{
  public:
    explicit DetourStallWatch(std::vector<HANDLE> threads, uint32_t stallMs = kDetourStallMs)
        : threads_(std::move(threads))
    {
        if (threads_.empty())
        {
            // Only the calling thread is enlisted: it cannot be suspended, so the window cannot stall on itself.
            armed_.store(true, std::memory_order_release);
            return;
        }

        worker_ = std::thread([this, stallMs] {
            std::unique_lock<std::mutex> lock(mutex_);
            // The constructor waits for this store. A worker that has not reached its wait yet is still in its own
            // start-up callbacks, TLS state and the process heap, and a stalled thread holding exactly that lock would
            // block it too: a watch that returned before this point could never resume anything. Nothing has been
            // suspended yet while the constructor runs, so the wait is short and cannot deadlock.
            {
                std::lock_guard<std::mutex> arm(armMutex_);
                armed_.store(true, std::memory_order_release);
            }
            armCv_.notify_all();

            const bool finished = done_.load();
            if (finished || resume_.wait_for(lock, std::chrono::milliseconds(stallMs), [this] { return done_.load(); }))
                return;

            resumed_.store(true, std::memory_order_release);
            for (const HANDLE thread : threads_)
                ResumeThread(thread);
        });

        std::unique_lock<std::mutex> arm(armMutex_);
        armCv_.wait(arm, [this] { return armed_.load(std::memory_order_acquire); });
    }

    DetourStallWatch(const DetourStallWatch&) = delete;
    DetourStallWatch& operator=(const DetourStallWatch&) = delete;

    ~DetourStallWatch() { Disarm(); }

    // The window is over (commit or abort returned). Idempotent.
    void Disarm()
    {
        done_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            resume_.notify_all();
        }
        if (worker_.joinable())
            worker_.join();
    }

    // True when the stall was long enough that the threads were resumed early: the transaction still completed, but a
    // suspended thread held something the installing thread needed.
    bool Resumed() const { return resumed_.load(std::memory_order_acquire); }

    // True once the worker is inside its wait, which is the point the constructor returns on.
    bool Armed() const { return armed_.load(std::memory_order_acquire); }

  private:
    std::vector<HANDLE> threads_;
    std::mutex mutex_;
    std::condition_variable resume_;
    std::mutex armMutex_;
    std::condition_variable armCv_;
    std::atomic<bool> done_ { false };
    std::atomic<bool> resumed_ { false };
    std::atomic<bool> armed_ { false };
    std::thread worker_;
};
} // namespace ext::core
