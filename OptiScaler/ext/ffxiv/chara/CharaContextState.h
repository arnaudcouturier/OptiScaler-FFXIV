#pragma once
// The flags a private device-context state is created with (the spatial AA and the DLSS guides both swap the game's context into a
// state of their own). The state has to agree with the threading mode of the device: a device created single-threaded needs the
// single-threaded flag on the state, otherwise the runtime may refuse it and the feature would fail for that setup only.

#include <cstdint>

namespace ext::ffxiv::chara
{
inline constexpr uint32_t kDeviceCreateSingleThreaded = 0x1; // D3D11_CREATE_DEVICE_SINGLETHREADED (checked against the SDK where used)
inline constexpr uint32_t kContextStateSingleThreaded = 0x1; // D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED

constexpr uint32_t ContextStateFlags(uint32_t deviceCreationFlags)
{
    return (deviceCreationFlags & kDeviceCreateSingleThreaded) != 0 ? kContextStateSingleThreaded : 0u;
}
} // namespace ext::ffxiv::chara
