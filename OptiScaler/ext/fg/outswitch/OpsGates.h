#pragma once
// The two pieces of the product binding that are arithmetic and a one-shot gate rather than device work: how long the
// present that carried the result took, and the diagnostic fault the benchmark key injects. Both live here so a test can
// drive them with the values a real session has, which is where the two defects they fix were invisible.
//
// A present's start is a steady-clock millisecond count since boot (HookRebind.cpp NowMs), and the judgement asks for a
// DURATION: ProbationWatch.h compares its `presentMs` with kStallMs. Handing it the start made every chain on a machine
// up for more than 2.5 s look stalled on its first judged present, which rolled a healthy switch back and remembered the
// target for the session.
//
// The fault key injects one failure per session, at the step it names. A step that only exists inside a running
// probation must not fire outside one: nothing would be judged, and the injected error HRESULT would reach the game on
// the first present of the session while the feature looked broken for the rest of it.
//
// Pure: no Windows or upstream include (tests/ext/fg/fg_switch_opsgates.cpp).

#include "ext/fg/outswitch/SwitchTypes.h"

#include <cstdint>

namespace ext::fg::outswitch
{
// The duration of one present, from its start on the switch's clock. A start read after the clock now (a suspend, a
// re-based reading) answers 0 rather than wrapping, and a longer present than a 32-bit count can hold is clamped: the
// judgement only compares it with a few hundred milliseconds.
inline uint32_t ElapsedMs(uint64_t startMs, uint64_t nowMs)
{
    const uint64_t elapsed = nowMs > startMs ? nowMs - startMs : 0;
    constexpr uint64_t kMax = 0xFFFFFFFFull;
    return static_cast<uint32_t>(elapsed > kMax ? kMax : elapsed);
}

// One injected failure per session, fired at the step it names and only where that step is reachable. `Take` consumes
// the fault, so a later present at the same step is left alone even when the condition comes back.
struct FaultGate
{
    Step step = Step::Idle;
    bool fired = false;

    // `active` is the condition the named step exists in: false means this step cannot be reached on this present, and
    // the fault waits for one where it can.
    bool Take(Step at, bool active = true)
    {
        if (step == Step::Idle || fired || at != step || !active)
            return false;
        fired = true;
        return true;
    }

    bool Configured(Step at) const { return step != Step::Idle && !fired && at == step; }
};
} // namespace ext::fg::outswitch
