#pragma once
// Retry policy and installer state machine for the native-quality hook install (FfxivNativeQualityHooks.h). Pure C++:
// no Windows, no Detours, no logging, so the pure tests (tests/ext/ffxiv/quality/) include this file as it is.
//
// RetryingInstaller replaces the old std::call_once: the same "one thread installs, the others wait for the result"
// behaviour, the same one-load fast path once the outcome is final, and one step at a time. What it adds is a second and a
// third chance for a failure that can clear by itself (a game thread exiting while the Detours transaction enlists it,
// another component's transaction being open at that moment). A deterministic refusal (wrong build, byte mismatch) is
// still final after one attempt, exactly as before.

#include <atomic>
#include <cstdint>
#include <mutex>

namespace ext::ffxiv::quality::retry
{
enum class Outcome
{
    Installed, // done, terminal
    Permanent, // deterministic for this exe, never retried (wrong build, byte mismatch)
    Transient, // may succeed later (thread race, another component's transaction open)
};

// Where an attempt stopped. The names are the words the log prints.
enum class Stage
{
    None,           // installed
    Guard,          // build or byte checks: the step logged its own warning
    ThreadSnapshot, // listing or opening the game's threads
    Begin,          // DetourTransactionBegin
    EnlistCurrent,  // DetourUpdateThread on the installing thread
    EnlistThread,   // DetourUpdateThread on another game thread
    AttachQuerySize,
    AttachRendererUpdate,
    Commit,
    Exception, // the step threw
};

inline const char* StageName(Stage stage)
{
    switch (stage)
    {
    case Stage::None:
        return "none";
    case Stage::Guard:
        return "guard";
    case Stage::ThreadSnapshot:
        return "thread snapshot";
    case Stage::Begin:
        return "begin transaction";
    case Stage::EnlistCurrent:
        return "enlist current thread";
    case Stage::EnlistThread:
        return "enlist game thread";
    case Stage::AttachQuerySize:
        return "attach size query";
    case Stage::AttachRendererUpdate:
        return "attach renderer update";
    case Stage::Commit:
        return "commit";
    case Stage::Exception:
        return "exception";
    }
    return "unknown";
}

struct StepResult
{
    Outcome outcome = Outcome::Transient;
    Stage stage = Stage::None;
    long error = 0;        // the Win32 / Detours error code of the failing call
    unsigned threadId = 0; // the thread an enlist failure was about, 0 when not applicable
};

// ---- Policy ---------------------------------------------------------------------------------------------------------------

inline constexpr int kMaxAttempts = 3; // attempts in total, the first included

// Time between a failed attempt and the next one, by how many attempts have failed so far. A thread-exit race clears in
// microseconds, so the first gap is short; the second covers load-time thread churn.
inline constexpr uint64_t RetryDelayMs(int failedAttempts) { return failedAttempts <= 1 ? 250 : 2000; }

// Win32 codes Detours reports, spelled out so this file needs no Windows header.
inline constexpr long kErrorInvalidHandle = 6;     // ERROR_INVALID_HANDLE
inline constexpr long kErrorInvalidBlock = 9;      // ERROR_INVALID_BLOCK
inline constexpr long kErrorInvalidOperation = 4317; // ERROR_INVALID_OPERATION (a transaction is already open elsewhere)

// What a Detours failure means. Everything is worth another try except a refusal to attach to this exact function
// (invalid block / invalid handle at an attach stage): that is a property of the exe, not of the moment.
inline Outcome ClassifyDetours(Stage stage, long error)
{
    if (error == 0)
        return Outcome::Installed;
    const bool attach = stage == Stage::AttachQuerySize || stage == Stage::AttachRendererUpdate;
    if (attach && (error == kErrorInvalidBlock || error == kErrorInvalidHandle))
        return Outcome::Permanent;
    return Outcome::Transient;
}

// ---- Installer ------------------------------------------------------------------------------------------------------------

// Step:  StepResult operator()() const-callable; one install attempt, never throws on purpose (a throw maps to Permanent).
// Clock: uint64_t operator()(), milliseconds, monotonic (production: GetTickCount64).
// Log:   Failed(int attempt, int maxAttempts, const StepResult&)  a failed attempt that will be retried;
//        GaveUp(int attempt, int maxAttempts, const StepResult&)  the failure that ended it (not called for a Guard refusal,
//                                                                 the step already logged that one);
//        Recovered(int attempt)                                   installed after at least one failure.
// A Log callback that throws is swallowed: logging must never change the outcome.
template <class Step, class Clock, class Log> class RetryingInstaller
{
  public:
    constexpr RetryingInstaller(Step step, Clock clock, Log log) : step_(step), clock_(clock), log_(log) {}

    RetryingInstaller(const RetryingInstaller&) = delete;
    RetryingInstaller& operator=(const RetryingInstaller&) = delete;

    // True once installed. Final outcomes (installed, or given up) cost one atomic load: no lock, no clock. While a retry is
    // pending, a poll costs two loads and one clock read until the attempt is due.
    bool Ensure()
    {
        const int state = state_.load(std::memory_order_acquire);
        if (state != kPending)
            return state == kInstalled;
        if (waiting_.load(std::memory_order_acquire) && clock_() < nextAttemptMs_.load(std::memory_order_acquire))
            return false;

        std::lock_guard<std::mutex> lock(mutex_);
        const int recheck = state_.load(std::memory_order_relaxed); // another thread may have finished while we waited
        if (recheck != kPending)
            return recheck == kInstalled;
        if (attempts_ > 0 && clock_() < nextAttemptMs_.load(std::memory_order_relaxed))
            return false; // the attempt that just ran failed; its successor is not due yet

        const int attempt = ++attempts_;
        StepResult result;
        try
        {
            result = step_();
        }
        catch (...)
        {
            result.outcome = Outcome::Permanent;
            result.stage = Stage::Exception;
        }

        if (result.outcome == Outcome::Installed)
        {
            state_.store(kInstalled, std::memory_order_release);
            if (attempt > 1)
                Guarded([&] { log_.Recovered(attempt); });
            return true;
        }

        if (result.outcome == Outcome::Permanent || attempt >= kMaxAttempts)
        {
            state_.store(kOff, std::memory_order_release);
            if (result.stage != Stage::Guard)
                Guarded([&] { log_.GaveUp(attempt, kMaxAttempts, result); });
            return false;
        }

        nextAttemptMs_.store(clock_() + RetryDelayMs(attempt), std::memory_order_release);
        waiting_.store(true, std::memory_order_release);
        Guarded([&] { log_.Failed(attempt, kMaxAttempts, result); });
        return false;
    }

  private:
    static constexpr int kPending = 0;
    static constexpr int kInstalled = 1;
    static constexpr int kOff = -1;

    template <class F> static void Guarded(F&& f)
    {
        try
        {
            f();
        }
        catch (...)
        {
        }
    }

    Step step_;
    Clock clock_;
    Log log_;
    std::atomic<int> state_ { kPending };
    std::atomic<bool> waiting_ { false };        // a failed attempt has set nextAttemptMs_
    std::atomic<uint64_t> nextAttemptMs_ { 0 };
    int attempts_ = 0;                           // guarded by mutex_
    std::mutex mutex_;
};
} // namespace ext::ffxiv::quality::retry
