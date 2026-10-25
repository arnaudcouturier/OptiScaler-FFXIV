#pragma once
// The registration state of OptiScaler as a ReShade add-on, apart from the config-reading parts of ReShadeAddon.cpp so that it can be
// tested on its own (tests/ext/reshade/reshade_registration_state.cpp). ReShadeAddon.cpp builds Status() and OnReshadeLoaded on it;
// ReShadeAddon.h declares Registered() and NoteRegistration(), defined here.
//
// No ReShade or ImGui types.

#include "ext/reshade/ReShadeRegister.h"

#include <atomic>
#include <cstdint>

namespace ext::reshade
{
// Heap-allocated and never destroyed: nothing runs at DLL_PROCESS_DETACH (loader lock), and callbacks that ReShade may still deliver
// while the process shuts down only ever read these atomics.
struct RegistrationState
{
    std::atomic<bool> attempted { false };  // the load-time attempt (OnReshadeLoaded) ran
    std::atomic<bool> registered { false }; // ReShade accepted the registration (API 20)
    std::atomic<uint8_t> reason { static_cast<uint8_t>(RegisterStatus::NoModule) }; // RegisterStatus of the last attempt
};
RegistrationState& Registration();
} // namespace ext::reshade
