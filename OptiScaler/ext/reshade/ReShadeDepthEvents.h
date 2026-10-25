#pragma once
// Automatic ReShade scene depth: the ReShade side of the event wiring. Six distinct callbacks (begin_effects, finish_effects,
// destroy_effect_runtime, reshade_reloaded_effects, reshade_set_technique_state, reshade_set_current_preset_path) hand the effect
// runtime to the EventDispatcher (ReShadeDepthDispatch.h). They are free functions in ReShadeDepthEvents.cpp, none of them shared with
// the HudLess boundary tracking or the diagnostics probe, which subscribe the same ReShade events with callbacks of their own.
//
// A callback checks ONLY that the process is not shutting down and that a dispatcher exists. It does not look at the option: see the
// top of ReShadeDepthDispatch.h. They are subscribed whatever the option says, and do nothing while the facade does not exist (no
// dispatcher): nothing is built before the first evaluate with the option on.
//
// ORDER. REST (ReshadeEffectShaderToggler) and ReShade's Generic Depth register their begin_effects at device creation and rebind their
// textures there; our begin_effects reads what they bound, so it must run AFTER theirs, and ReShade calls the callbacks of an event in
// registration order. On the load path (ReShade was there when OptiScaler loaded) OnReshadeLoaded therefore subscribes only FIVE events
// (init_effect_runtime, destroy, reloaded, technique state, preset path); the init_effect_runtime callback subscribes finish and begin,
// at the first effect runtime, which is created after the device and after the add-ons loaded, on the thread that runs the events (no
// race with ReShade's dispatch loop, which register_event from the evaluate thread had). That callback does NOT look at the dispatcher or
// the option either: the option may be switched on much later and the init has fired by then. A ReShade that loaded after OptiScaler (late
// path) has its runtime and its add-ons already: all six are subscribed at once. Nothing but the late path ever subscribes from the
// evaluate thread.
//
// No ReShade or ImGui types in this header.

#include "ext/reshade/ReShadeDepthCore.h"

#include <array>
#include <cstdint>

namespace ext::reshade::depth
{
class EventDispatcher;

// The dispatcher the callbacks call. Set once by the facade (or by a test); the callbacks do nothing while it is null.
void SetDispatcher(EventDispatcher* dispatcher) noexcept;

// When the subscription is made: at LOAD (OnReshadeLoaded, right after the registration, before anything can dispatch: no race with
// ReShade's event lists, which it indexes while it dispatches; only five events, begin and finish follow from init_effect_runtime) or
// LATE (a ReShade that loaded after OptiScaler: right after the late registration, on the evaluate thread, which may race ReShade's
// dispatch on another thread; the log line says the thread; all six events).
enum class SubscribeWhen : uint8_t
{
    Late,
    Load,
};

// Subscribes what the path needs, each event once, the first time it is called after ReShade accepted the registration (RegisterAddon
// returned Ok: the ReShade API helpers cache the module they resolved on their first use, so this must never run before). The FIRST call
// decides the path and later calls of the other kind change nothing. Idempotent. A subscription that failed is remembered per event and
// not tried again. Load path: the five events of LateRegistrationPlanner::kLoadEvents (init_effect_runtime last); begin and finish are
// left to the init callback. Late path: the six of kEvents, begin LAST and only if all the others are (a begin without its finish would
// leave our copy in ReShade's map). True = everything this path needs NOW is subscribed (on the load path that is the five).
bool EnsureSubscribed(SubscribeWhen when = SubscribeWhen::Late);

// Which path subscribed (None until the first EnsureSubscribed). The evaluate thread's safety net subscribes only while this is None.
enum class SubscribedVia : uint8_t
{
    None,
    Load,
    Late,
};
SubscribedVia SubscriptionPath() noexcept;

// The thread that runs the NGX evaluate (the depth copy is made there), for the log line of the first begin_effects callback of each thread:
// begin_effects arrives from at least two paths (the HudLess UI-boundary render_effects call and ReShade's own pass in Present), and the
// line tells whether they, and the evaluate, are on the same thread. The facade sets the source; null = unknown.
using ThreadIdFn = uint32_t (*)();
void SetEvaluateThreadSource(ThreadIdFn source) noexcept;

// Bit i is set when the DepthEvent with value i is subscribed / failed to subscribe.
uint32_t SubscribedEvents() noexcept;
uint32_t FailedEvents() noexcept;

constexpr uint32_t EventBit(DepthEvent event) { return 1u << static_cast<uint32_t>(event); }
template <size_t N> constexpr uint32_t EventMask(const std::array<DepthEvent, N>& events)
{
    uint32_t mask = 0;
    for (const DepthEvent event : events)
        mask |= EventBit(event);
    return mask;
}
// Every DepthEvent, what the load path subscribes at once, and what the late path subscribes (all but init_effect_runtime).
inline constexpr uint32_t kAllDepthEvents = (1u << static_cast<uint32_t>(DepthEvent::Count)) - 1;
inline constexpr uint32_t kLoadDepthEvents = EventMask(LateRegistrationPlanner::kLoadEvents);
inline constexpr uint32_t kRuntimeDepthEvents = EventMask(LateRegistrationPlanner::kRuntimeEvents);
inline constexpr uint32_t kLateDepthEvents = EventMask(LateRegistrationPlanner::kEvents);

// The lists must name every DepthEvent exactly once between them: the compiler checks it, so a new event cannot be added to the enum and
// forgotten here. (Load + runtime = every event, disjoint; late = every event but init_effect_runtime.)
constexpr bool EventListIsComplete()
{
    return (kLoadDepthEvents & kRuntimeDepthEvents) == 0 && (kLoadDepthEvents | kRuntimeDepthEvents) == kAllDepthEvents &&
           LateRegistrationPlanner::kLoadEvents.size() + LateRegistrationPlanner::kRuntimeEvents.size() ==
               static_cast<size_t>(DepthEvent::Count) &&
           kLateDepthEvents == (kAllDepthEvents & ~EventBit(DepthEvent::InitEffectRuntime)) &&
           LateRegistrationPlanner::kEvents.size() == static_cast<size_t>(DepthEvent::Count) - 1;
}
static_assert(EventListIsComplete(), "the load, runtime and late event lists must name every DepthEvent exactly once");
} // namespace ext::reshade::depth
