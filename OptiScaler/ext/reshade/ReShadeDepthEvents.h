#pragma once
// Automatic ReShade scene depth: the ReShade side of the event wiring. Six distinct callbacks (begin_effects, finish_effects,
// destroy_effect_runtime, reshade_reloaded_effects, reshade_set_technique_state, reshade_set_current_preset_path) hand the effect
// runtime to the EventDispatcher (ReShadeDepthDispatch.h). They are free functions in ReShadeDepthEvents.cpp, none of them shared with
// the HudLess boundary tracking or the diagnostics probe, which subscribe the same ReShade events with callbacks of their own.
//
// A callback checks ONLY that the process is not shutting down and that a dispatcher exists. It does not look at the option: see the
// top of ReShadeDepthDispatch.h. The six are subscribed ONCE AT LOAD (OnReshadeLoaded, right after the registration), whatever the option
// says, and do nothing while the facade does not exist (no dispatcher): nothing is built before the first evaluate with the option on.
//
// No ReShade or ImGui types in this header.

#include "ext/reshade/ReShadeDepthCore.h"

#include <cstdint>

namespace ext::reshade::depth
{
class EventDispatcher;

// The dispatcher the callbacks call. Set once by the facade (or by a test); the callbacks do nothing while it is null.
void SetDispatcher(EventDispatcher* dispatcher) noexcept;

// When the subscription is made: at LOAD (OnReshadeLoaded, right after the registration, before anything can dispatch: no race with
// ReShade's event lists, which it indexes while it dispatches) or LATE (a ReShade that loaded after OptiScaler: from the first evaluate with
// the option on, on the evaluate thread, which may race ReShade's dispatch on another thread; the log line says the thread).
enum class SubscribeWhen : uint8_t
{
    Late,
    Load,
};

// Subscribes exactly the six depth events, each once, the first time it is called after ReShade accepted the registration
// (RegisterAddon returned Ok: the ReShade API helpers cache the module they resolved on their first use, so this must never run
// before). Idempotent and callable from the evaluate thread. A subscription that failed is remembered per event and not tried again.
// The begin callback is subscribed LAST and only if all the others are: a begin without its finish would leave our copy in ReShade's
// map. True = all six are subscribed.
bool EnsureSubscribed(SubscribeWhen when = SubscribeWhen::Late);

// Bit i is set when the DepthEvent with value i is subscribed / failed to subscribe.
uint32_t SubscribedEvents() noexcept;
uint32_t FailedEvents() noexcept;

// One bit per event of LateRegistrationPlanner::kEvents (the list of what the depth binder subscribes), which must name every DepthEvent
// exactly once: the compiler checks it, so a new event cannot be added to the enum and forgotten here.
inline constexpr uint32_t kAllDepthEvents = (1u << LateRegistrationPlanner::kEvents.size()) - 1;
constexpr bool EventListIsComplete()
{
    uint32_t seen = 0;
    for (const DepthEvent event : LateRegistrationPlanner::kEvents)
    {
        const uint32_t bit = 1u << static_cast<uint32_t>(event);
        if (static_cast<size_t>(event) >= LateRegistrationPlanner::kEvents.size() || (seen & bit) != 0)
            return false;
        seen |= bit;
    }
    return seen == kAllDepthEvents && LateRegistrationPlanner::kEvents.size() == static_cast<size_t>(DepthEvent::Count);
}
static_assert(EventListIsComplete(), "LateRegistrationPlanner::kEvents must list every DepthEvent exactly once");
} // namespace ext::reshade::depth
