#pragma once
// Automatic ReShade scene depth: registration with a ReShade that loaded after OptiScaler. The load-time attempt
// (ext::reshade::OnReshadeLoaded) runs once inside DllMain and misses a ReShade that is loaded later (installed as dxgi.dll, or loaded
// by another proxy). This retries from the capture seam, outside the loader lock, while the option is on.
//
// No ReShade or ImGui types in this header.

#include <cstdint>

namespace ext::reshade::depth
{
// One late attempt, if the planner (LateRegistrationPlanner: option on, the load-time attempt found no module, fewer than 60 late
// attempts, the last one at least a second old) says so. Single-flight: the last attempt time is taken with a compare-exchange, so two
// threads never attempt at once. The module scan and RegisterAddon run under SEH. On success the module is pinned (like OnReshadeLoaded
// does), the registration state is recorded (ext::reshade::NoteRegistration) and the six depth events are subscribed: nothing else (no
// boundary tracking, no probe, no window event). Returns true when this call registered. `nowMs` is a monotonic clock in ms.
bool TryLateRegister(bool enabled, int64_t nowMs);

struct LateStatus
{
    uint32_t attempts = 0;  // late attempts made so far
    bool gaveUp = false;    // the attempts are used up (about a minute) and no ReShade was found
    int64_t lastAttemptMs = 0;
};
LateStatus GetLateStatus() noexcept;
} // namespace ext::reshade::depth
