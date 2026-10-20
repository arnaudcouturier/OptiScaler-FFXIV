#pragma once
// DLSS-G live preset switch: the state machine that runs on the FG dispatch thread, with the actions and the clock injected (so it
// is tested without the runtime). Pure; tests/ext/fg/fg_preset_switch.cpp builds it standalone.
//
// A switch takes several dispatch ticks:
//   tick 1   evaluate the runtime READ-ONLY (guards, provider choice; nothing is hooked), then turn FG off;
//   ticks    wait: at least kMinWaitTicks ticks and kMinWaitMs milliseconds, so the frames FG still had in flight are done;
//   later    release the DLSS-G resources, install the hooks into the runtime (FG is quiet: no endpoint is being created and
//            the runtime's threads are idle), commit the new request. The runtime then creates its next endpoint under it and
//            FG resumes on the next dispatch with fresh history.
// Any failing step keeps the old request: the target is remembered as failed (no retry every frame) and the menu says "restart
// needed". Going back to the active request cancels a switch that is waiting; asking for another target starts over.

#include "ext/fg/dlssg/PresetRequest.h"

#include <cstdint>
#include <functional>

namespace ext::fg::preset
{
inline constexpr int kMinWaitTicks = 3;          // dispatch ticks between "off" and "release"
inline constexpr uint64_t kMinWaitMs = 100;      // and milliseconds

enum class SwitchResult : uint8_t
{
    Unchanged,
    InProgress, // a switch is under way: the caller skips this frame's dispatch (FG stays off)
    Switched,   // the new request is active: skip this frame too
    RestartNeeded
};

enum class SwitchFail : uint8_t
{
    None,
    CannotRestoreAuto, // booted with a request and the driver's own answer was never seen
    Prepare,           // the runtime did not pass the read-only checks
    Off,               // FG could not be switched off
    Release,           // the DLSS-G resources could not be released
    Install            // the hooks into the runtime could not be installed
};

struct SwitchSteps
{
    std::function<bool()> evaluate;  // read-only: verify the runtime and choose the module; changes nothing
    std::function<bool()> off;       // FG mode off
    std::function<bool()> release;   // free the DLSS-G resources
    std::function<bool()> install;   // install the hooks into the runtime (once; later calls are cheap)
    std::function<void()> activated; // after the new request is active (readout reset, history invalidation)
};

class Switcher
{
  public:
    explicit Switcher(RequestState& state) : state_(state) {}

    // `requested` is the configured choice, `nowMs` a millisecond clock. Only the dispatch thread calls this, once per tick.
    SwitchResult Apply(int requested, const SwitchSteps& steps, uint64_t nowMs)
    {
        if (!ValidRequest(requested))
            return SwitchResult::Unchanged;

        if (pending_)
        {
            if (requested == state_.Active())
            {
                pending_ = false; // the user went back before anything was released: nothing to undo, FG resumes by itself
                Forget();
                return SwitchResult::Unchanged;
            }
            if (requested != target_)
                pending_ = false; // another target: start over below
            else
            {
                ++waited_;
                if (waited_ < kMinWaitTicks || nowMs - offAt_ < kMinWaitMs)
                    return SwitchResult::InProgress;
                pending_ = false;
                if (!Run(steps.release))
                    return Fail(requested, SwitchFail::Release);
                if (!Run(steps.install))
                    return Fail(requested, SwitchFail::Install);
                state_.Commit(requested);
                Forget();
                ++generation_;
                if (steps.activated)
                    steps.activated();
                return SwitchResult::Switched;
            }
        }

        if (requested == state_.Active())
        {
            Forget();
            return SwitchResult::Unchanged;
        }
        if (failed_ == requested)
            return SwitchResult::RestartNeeded;

        if (requested == kAuto && !state_.CanReturnToAuto())
            return Fail(requested, SwitchFail::CannotRestoreAuto);

        if (!Run(steps.evaluate))
            return Fail(requested, SwitchFail::Prepare);
        if (!Run(steps.off))
            return Fail(requested, SwitchFail::Off);
        pending_ = true;
        target_ = requested;
        waited_ = 0;
        offAt_ = nowMs;
        return SwitchResult::InProgress;
    }

    // A switch is waiting between "off" and "release".
    bool Pending() const { return pending_; }
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
    bool pending_ = false;
    int target_ = -1;
    int waited_ = 0;
    uint64_t offAt_ = 0;
};

// Why the live control is off. The built-in NVIDIA runtime is the only one a request can reach: the RTX 20/30 companion runtime
// reads its own settings file, a replacement runtime is not NVIDIA's code, and another frame generation output has no DLSS-G.
enum class Unavailable : uint8_t
{
    None,
    AmpereRuntime,
    ReplacementRuntime,
    OtherOutput
};

constexpr Unavailable CheckAvailable(bool ampereRuntimeConfigured, bool replacementRuntime, bool dlssgOutput)
{
    if (ampereRuntimeConfigured)
        return Unavailable::AmpereRuntime;
    if (replacementRuntime)
        return Unavailable::ReplacementRuntime;
    if (!dlssgOutput)
        return Unavailable::OtherOutput;
    return Unavailable::None;
}

constexpr const char* UnavailableText(Unavailable why)
{
    switch (why)
    {
    case Unavailable::AmpereRuntime:
        return "Not available with the RTX 20/30 frame-generation runtime: it reads its own settings file (its Preset is under the unlocks).";
    case Unavailable::ReplacementRuntime:
        return "Not available with a replacement DLSS-G runtime: the preset belongs to NVIDIA's own runtime.";
    case Unavailable::OtherOutput:
        return "The frame generation output is not DLSS-G: the preset only applies to NVIDIA's frame generation.";
    default:
        return "";
    }
}

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
    case SwitchFail::Install:
        return "the hooks into the runtime could not be installed";
    default:
        return "";
    }
}
} // namespace ext::fg::preset
