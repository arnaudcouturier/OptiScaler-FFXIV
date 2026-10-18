#pragma once
// The deferral state machine of the mode switch (B2-a). Pure: time and frame counts come in as arguments, so the tests run it
// on a fake clock and a fake frame counter.
//
// Idle      no call held.
// Pending   a SetFullscreenState call was held back and frame generation was asked off; waiting for the gate.
// Replaying the held call is being replayed right now (the replay itself must pass through the ReShade handler untouched).
//
// Waiting is bounded, in two stages. After the soft limit (a few presents or 2 s) the call is replayed anyway IF frame
// generation is off at that moment. Frame generation can come back during the wait (the pause after a Deactivate is only
// about ten frames); every present that finds it active asks the controller to switch it off again, and the replay never
// runs with it active until the hard limit, which replays regardless: the call then goes through as it would today (fail
// open), and the controller logs that loudly.

#include <cstdint>

namespace ext::window
{
enum class DeferState : uint8_t
{
    Idle,
    Pending,
    Replaying,
};

constexpr const char* DeferStateName(DeferState s)
{
    switch (s)
    {
    case DeferState::Pending:
        return "pending";
    case DeferState::Replaying:
        return "replaying";
    default:
        return "idle";
    }
}

struct DeferralLimits
{
    uint32_t softPresents = 6; // well below the ~10 frame pause after a Deactivate
    uint64_t softMs = 2000;
    uint32_t hardPresents = 24;
    uint64_t hardMs = 6000;
};

enum class WaitResult : uint8_t
{
    Wait,              // keep waiting
    Replay,            // frame generation is confirmed off
    ReplayAtLimit,     // soft limit reached, frame generation is off right now but not confirmed
    ReplayAtHardLimit, // hard limit reached: replay regardless of frame generation
};

constexpr const char* WaitResultName(WaitResult r)
{
    switch (r)
    {
    case WaitResult::Replay:
        return "frame generation confirmed off";
    case WaitResult::ReplayAtLimit:
        return "wait limit reached";
    case WaitResult::ReplayAtHardLimit:
        return "hard wait limit reached with frame generation still on";
    default:
        return "waiting";
    }
}

class Deferral
{
  public:
    explicit Deferral(DeferralLimits limits = {}) : _limits(limits) {}

    DeferState State() const { return _state; }
    bool PendingFullscreen() const { return _fullscreen; }
    uint32_t Presents() const { return _presents; }
    uint32_t Replaced() const { return _replaced; }
    uint64_t ElapsedMs(uint64_t nowMs) const { return nowMs >= _startMs ? nowMs - _startMs : 0; }

    // Idle -> Pending. False (and no change) in any other state.
    bool Begin(bool fullscreen, uint64_t nowMs)
    {
        if (_state != DeferState::Idle)
            return false;
        _state = DeferState::Pending;
        _fullscreen = fullscreen;
        _startMs = nowMs;
        _presents = 0;
        _replaced = 0;
        return true;
    }

    // Pending: a later call replaces the held one ("keep only the latest"). The wait budget is NOT restarted, so a game that
    // keeps calling cannot hold the first call back for ever. False in any other state.
    bool Replace(bool fullscreen)
    {
        if (_state != DeferState::Pending)
            return false;
        _fullscreen = fullscreen;
        ++_replaced;
        return true;
    }

    // Drops a held call without replaying it. Pending or Replaying -> Idle.
    void Cancel() { _state = DeferState::Idle; }

    // One complete game present went by while Pending. `fgMaybeOn` = frame generation is active or unknown;
    // `gateConfirmed` = the FgOffGate says DLSS-G is off. `rightThread` = this present runs on the thread that made the held
    // call; the call is replayed there only, so on another thread only the hard limit can end the wait.
    WaitResult OnPresentEnd(bool fgMaybeOn, bool gateConfirmed, uint64_t nowMs, bool rightThread = true)
    {
        if (_state != DeferState::Pending)
            return WaitResult::Wait;
        ++_presents;
        const uint64_t elapsed = ElapsedMs(nowMs);

        if (rightThread && !fgMaybeOn && gateConfirmed)
            return WaitResult::Replay;
        if (_presents >= _limits.hardPresents || elapsed >= _limits.hardMs)
            return WaitResult::ReplayAtHardLimit;
        if (rightThread && !fgMaybeOn && (_presents >= _limits.softPresents || elapsed >= _limits.softMs))
            return WaitResult::ReplayAtLimit;
        return WaitResult::Wait;
    }

    // Pending -> Replaying. False in any other state.
    bool BeginReplay()
    {
        if (_state != DeferState::Pending)
            return false;
        _state = DeferState::Replaying;
        return true;
    }

    // Replaying -> Idle.
    void EndReplay()
    {
        if (_state == DeferState::Replaying)
            _state = DeferState::Idle;
    }

  private:
    DeferralLimits _limits;
    DeferState _state = DeferState::Idle;
    bool _fullscreen = false;
    uint64_t _startMs = 0;
    uint32_t _presents = 0;
    uint32_t _replaced = 0;
};
} // namespace ext::window
