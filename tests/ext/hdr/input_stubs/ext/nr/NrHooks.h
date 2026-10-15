#pragma once

// The scene adapter only needs the menu/status enum from NR's hook surface.
namespace ext::nr
{
enum class SceneState
{
    Off,
    Ordinary,
    PreSR,
    Alternating,
    WaitingScene,
    WaitingGpu,
    WaitingNR,
    UnsupportedFormat,
    AllocationFailed,
    UnavailableHooks,
    RequiresBeforeSR,
    HDRInactive,
    NRDisabled
};
} // namespace ext::nr
