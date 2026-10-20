#pragma once
// DLSS-G live preset switch: the state machine that runs on the FG dispatch thread, with the actions injected (so it is tested
// without the runtime). Pure; tests/ext/fg/fg_preset_switch.cpp builds it standalone.
//
// A switch is: prepare the runtime hooks (guards, detours: PresetModule.h), turn FG off, release the DLSS-G resources, make the
// new request the active one. The runtime then creates its next endpoint under the new request and FG resumes on the next
// dispatch with fresh history. Any failing step keeps the old request: the target is remembered as failed (no retry every
// frame) and the menu says "restart needed". Going back to the active request, or asking for another target, forgets the
// failure.

#include "ext/fg/dlssg/PresetRequest.h"

#include <cstdint>
#include <functional>

namespace ext::fg::preset
{
enum class SwitchResult : uint8_t
{
    Unchanged,
    Switched,
    RestartNeeded
};

enum class SwitchFail : uint8_t
{
    None,
    CannotRestoreAuto, // booted with a request and the driver's own answer was never seen
    Prepare,           // guards or hooks of the runtime module
    Off,               // FG could not be switched off
    Release            // the DLSS-G resources could not be released
};

struct SwitchSteps
{
    std::function<bool()> prepare; // verify the runtime and install its hooks (once; later calls are cheap)
    std::function<bool()> off;     // FG mode off
    std::function<bool()> release; // free the DLSS-G resources
    std::function<void()> activated; // after the new request is active (readout reset, history invalidation)
};

class Switcher
{
  public:
    explicit Switcher(RequestState& state) : state_(state) {}

    // `requested` is the configured choice. Only the dispatch thread calls this.
    SwitchResult Apply(int requested, const SwitchSteps& steps)
    {
        if (!ValidRequest(requested))
            return SwitchResult::Unchanged;
        if (requested == state_.Active())
        {
            Forget();
            return SwitchResult::Unchanged;
        }
        if (failed_ == requested)
            return SwitchResult::RestartNeeded;

        if (requested == kAuto && !state_.CanReturnToAuto())
            return Fail(requested, SwitchFail::CannotRestoreAuto);

        if (!Run(steps.prepare))
            return Fail(requested, SwitchFail::Prepare);
        if (!Run(steps.off))
            return Fail(requested, SwitchFail::Off);
        if (!Run(steps.release))
            return Fail(requested, SwitchFail::Release);

        state_.Commit(requested);
        Forget();
        ++generation_;
        if (steps.activated)
            steps.activated();
        return SwitchResult::Switched;
    }

    // The target that failed and is not retried, -1 when none.
    int FailedRequest() const { return failed_; }
    SwitchFail LastFail() const { return lastFail_; }
    bool RestartNeeded(int requested) const { return failed_ == requested && failed_ >= 0; }
    // Number of completed switches (the menu shows "switched" feedback from it).
    uint32_t Generation() const { return generation_; }

  private:
    static bool Run(const std::function<bool()>& step)
    {
        try
        {
            return step && step();
        }
        catch (...)
        {
            return false;
        }
    }

    SwitchResult Fail(int requested, SwitchFail why)
    {
        failed_ = requested;
        lastFail_ = why;
        return SwitchResult::RestartNeeded;
    }

    void Forget()
    {
        failed_ = -1;
        lastFail_ = SwitchFail::None;
    }

    RequestState& state_;
    int failed_ = -1;
    SwitchFail lastFail_ = SwitchFail::None;
    uint32_t generation_ = 0;
};

constexpr const char* FailText(SwitchFail why)
{
    switch (why)
    {
    case SwitchFail::CannotRestoreAuto:
        return "the driver's own answer was not seen at start-up, so Auto cannot be restored while the game runs";
    case SwitchFail::Prepare:
        return "the installed runtime did not pass the safety checks for a live change";
    case SwitchFail::Off:
        return "frame generation could not be switched off";
    case SwitchFail::Release:
        return "the DLSS-G resources could not be released";
    default:
        return "";
    }
}
} // namespace ext::fg::preset
