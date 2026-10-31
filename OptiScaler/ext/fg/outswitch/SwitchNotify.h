#pragma once
// The live output switch's observer: one notification per switch milestone, for readers that record a session rather
// than act on it (the frame-generation event recorder is the only one). The switch itself never waits for an observer:
// they are plain function pointers called on the present thread, and an empty list costs one compare per reported step.
//
// The phases are the eight the recorder writes (its `switch.*` kinds): a begin, a refusal, a teardown of the chain that
// was on the window, the creation of the new one, its adoption, the commit, the probation verdict, and a rollback to
// plain presentation. The event carries the targets, the generation and the same text the `[fg-switch]` log line has, so
// a recording and the log never describe a step differently.
//
// Pure: no Windows, DXGI or upstream include (tests/ext/fg/fg_switch_notify.cpp).

#include "ext/fg/outswitch/SwitchTypes.h"

#include <cstddef>
#include <cstdint>

namespace ext::fg::outswitch
{
enum class NotifyPhase : uint8_t
{
    Begin,
    Refused,
    Teardown,
    Create,
    Adopt,
    Commit,
    Probation,
    Rollback
};

struct SwitchEvent
{
    NotifyPhase phase = NotifyPhase::Begin;
    Target from = Target::Plain; // what was on the window when the step ran
    Target to = Target::Plain;   // what the step is about: the target; for a rollback, the target remembered as failed
    Fail fail = Fail::None;
    uint32_t generation = 0;
    uint32_t number = 0; // the step's own count when it has one (the probation's presents)
    const char* text = ""; // the formatted line, without the `[fg-switch] ` prefix
};

// Two slots: the recorder in a session, and a test or a future reader. A duplicate registration is ignored.
inline constexpr size_t kSwitchObservers = 2;
using SwitchObserver = void (*)(const SwitchEvent&) noexcept;

inline SwitchObserver* SwitchObserverList() noexcept
{
    static SwitchObserver list[kSwitchObservers] = { nullptr, nullptr };
    return list;
}

// True when the observer is in the list afterwards (false when every slot is taken by another observer).
inline bool AddSwitchObserver(SwitchObserver observer) noexcept
{
    if (observer == nullptr)
        return false;
    SwitchObserver* list = SwitchObserverList();
    for (size_t i = 0; i < kSwitchObservers; ++i)
    {
        if (list[i] == observer)
            return true;
        if (list[i] == nullptr)
        {
            list[i] = observer;
            return true;
        }
    }
    return false;
}

inline void RemoveSwitchObserver(SwitchObserver observer) noexcept
{
    SwitchObserver* list = SwitchObserverList();
    for (size_t i = 0; i < kSwitchObservers; ++i)
        if (list[i] == observer)
            list[i] = nullptr;
}

inline void Notify(const SwitchEvent& event) noexcept
{
    SwitchObserver* list = SwitchObserverList();
    for (size_t i = 0; i < kSwitchObservers; ++i)
        if (list[i] != nullptr)
            list[i](event);
}

// The phase a reported step belongs to. A probation step that failed is a rollback (that is what follows it), and the
// steps without a recorder row answer an invalid phase.
struct PhaseOfStep
{
    bool valid = false;
    NotifyPhase phase = NotifyPhase::Begin;
};

inline PhaseOfStep PhaseOf(Step step, Fail fail) noexcept
{
    (void) fail; // every step maps to one kind, whatever its failure; the text carries the failure
    switch (step)
    {
    case Step::Begin:
        return { true, NotifyPhase::Begin };
    case Step::Refused:
        return { true, NotifyPhase::Refused };
    case Step::Teardown:
        return { true, NotifyPhase::Teardown };
    case Step::Create:
        return { true, NotifyPhase::Create };
    case Step::Adopt:
        return { true, NotifyPhase::Adopt };
    case Step::Commit:
        return { true, NotifyPhase::Commit };
    case Step::Probation:
        // A failed probation is a probation record with the failure in its text: the rollback, when one follows, is its
        // own record (Step::Rollback), and a watchdog failure that does not reach the rollback count is no rollback.
        return { true, NotifyPhase::Probation };
    case Step::Rollback:
        return { true, NotifyPhase::Rollback };
    default:
        return {};
    }
}
} // namespace ext::fg::outswitch
