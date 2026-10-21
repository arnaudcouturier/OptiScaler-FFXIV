#pragma once
// Retry policy and installer state machine for the native-quality hook install (FfxivNativeQualityHooks.h). Pure C++:
// no Windows, no Detours, no logging, so the pure tests (tests/ext/ffxiv/quality/) include this file as it is.
//
// RetryingInstaller replaces the old std::call_once: the same "one thread installs, the others wait for the result"
// behaviour, the same one relaxed-cost load on the fast path, and one step run at a time. What a step reports decides
// what happens next (see Outcome).

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

// ---- Installer ------------------------------------------------------------------------------------------------------

// Step: StepResult operator()(). Clock: uint64_t operator()() in milliseconds (production: GetTickCount64). Log:
// Failed / GaveUp / Recovered callbacks (see below). All three are copied in.
template <class Step, class Clock, class Log> class RetryingInstaller
{
  public:
    constexpr RetryingInstaller(Step step, Clock clock, Log log) : step_(step), clock_(clock), log_(log) {}

    RetryingInstaller(const RetryingInstaller&) = delete;
    RetryingInstaller& operator=(const RetryingInstaller&) = delete;

    // True once installed. Cheap after the first call: one atomic load, no lock, no clock.
    bool Ensure()
    {
        const int state = state_.load(std::memory_order_acquire);
        if (state != kPending)
            return state == kInstalled;

        std::lock_guard<std::mutex> lock(mutex_);
        const int recheck = state_.load(std::memory_order_relaxed); // another thread may have finished while we waited
        if (recheck != kPending)
            return recheck == kInstalled;

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

        const bool installed = result.outcome == Outcome::Installed;
        state_.store(installed ? kInstalled : kOff, std::memory_order_release);
        return installed;
    }

  private:
    static constexpr int kPending = 0;
    static constexpr int kInstalled = 1;
    static constexpr int kOff = -1;

    Step step_;
    Clock clock_;
    Log log_;
    std::atomic<int> state_ { kPending };
    std::mutex mutex_;
};
} // namespace ext::ffxiv::quality::retry
