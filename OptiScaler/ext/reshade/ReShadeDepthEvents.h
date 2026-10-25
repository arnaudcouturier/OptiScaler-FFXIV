#pragma once
// Automatic ReShade scene depth: the ReShade side of the event wiring. Six distinct callbacks (begin_effects, finish_effects,
// destroy_effect_runtime, reshade_reloaded_effects, reshade_set_technique_state, reshade_set_current_preset_path) hand the effect
// runtime to the EventDispatcher (ReShadeDepthDispatch.h). They are free functions in ReShadeDepthEvents.cpp, none of them shared with
// the HudLess boundary tracking or the diagnostics probe, which subscribe the same ReShade events with callbacks of their own.
//
// A callback checks ONLY that the process is not shutting down and that a dispatcher exists. It does not look at the option: see the
// top of ReShadeDepthDispatch.h. The callbacks cost nothing while nobody subscribed them; the subscription is lazy.
//
// No ReShade or ImGui types in this header.

#include <cstdint>

namespace ext::reshade::depth
{
class EventDispatcher;

// The dispatcher the callbacks call. Set once by the facade (or by a test); the callbacks do nothing while it is null.
void SetDispatcher(EventDispatcher* dispatcher) noexcept;

// Subscribes exactly the six depth events, each once, the first time it is called after ReShade accepted the registration
// (RegisterAddon returned Ok: the ReShade API helpers cache the module they resolved on their first use, so this must never run
// before). Idempotent and callable from the evaluate thread. A subscription that failed is remembered per event and not tried again.
// The begin callback is subscribed LAST and only if all the others are: a begin without its finish would leave our copy in ReShade's
// map. True = all six are subscribed.
bool EnsureSubscribed();

// Bit i is set when the DepthEvent with value i is subscribed / failed to subscribe.
uint32_t SubscribedEvents() noexcept;
uint32_t FailedEvents() noexcept;
inline constexpr uint32_t kAllDepthEvents = 0x3F;
} // namespace ext::reshade::depth
