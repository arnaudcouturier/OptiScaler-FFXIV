#pragma once
// Focus pause for more than one NR consumer: the scene NR pass and the character preview NR share ONE sample of the game
// window (taken at most every 50 ms) and each has its OWN FocusGate. One shared gate would not work: the gate is fed with
// the consumer's own switch, and a consumer whose switch is off clears it (FocusGate::Update with enabled == false), so two
// consumers with different switches would reset each other every frame.
//
// Pure: the window sampler is a template parameter, the clock is an argument. Unit-tested in
// tests/ext/nr/nr_focus_tracker.cpp. The Win32 sampler and the mutex live in NrFocus.cpp.

#include "ext/nr/NrFocusGate.h"

#include <cstdint>
#include <utility>

namespace ext::nr
{
struct WindowSample
{
    bool focused = true;
    bool minimised = false;
};

enum class FocusConsumer : unsigned
{
    Scene = 0,
    Preview = 1,
};

template <class Sampler> class FocusTracker
{
  public:
    static constexpr uint64_t kSampleIntervalMs = 50; // the foreground window does not need a per-frame answer
    static constexpr unsigned kConsumers = 2;

    explicit FocusTracker(Sampler sampler) : _sampler(std::move(sampler)) {}

    // May this consumer's NR run now? `switchOn` is that consumer's own switch (DisableOnFocusLoss combined with "this NR is
    // on"). With the switch off the answer is true, that consumer's gate is cleared and the window is not sampled for it.
    bool Allows(FocusConsumer who, bool switchOn, uint64_t nowMs)
    {
        FocusGate& gate = _gates[static_cast<unsigned>(who)];
        if (!switchOn)
            return gate.Update(false, true, false, nowMs);

        // A clock that steps backwards counts as a fresh sample.
        if (!_haveSample || nowMs < _sampledAt || nowMs - _sampledAt >= kSampleIntervalMs)
        {
            _sample = _sampler();
            _sampledAt = nowMs;
            _haveSample = true;
        }
        return gate.Update(true, _sample.focused, _sample.minimised, nowMs);
    }

  private:
    Sampler _sampler;
    FocusGate _gates[kConsumers];
    WindowSample _sample;
    uint64_t _sampledAt = 0;
    bool _haveSample = false;
};

// One log line per pause episode, each way: fed with a consumer's answer on every call, it reports the call on which the answer flipped
// (allowed -> paused, paused -> allowed) and nothing while it stays the same. Pure so the flip is unit-tested (the log line itself is
// written by NrFocus.cpp). The caller serialises the calls.
class PauseEpisodes
{
  public:
    enum class Event
    {
        None,
        Paused,
        Resumed,
    };

    Event Note(bool allowed)
    {
        if (allowed == _paused)
        {
            _paused = !allowed;
            return _paused ? Event::Paused : Event::Resumed;
        }
        return Event::None;
    }

  private:
    bool _paused = false;
};
} // namespace ext::nr
