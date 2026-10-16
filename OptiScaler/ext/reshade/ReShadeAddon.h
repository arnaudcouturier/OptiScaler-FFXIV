#pragma once
// Public face of the ReShade built-in add-on for the rest of the fork (menu card, the future boundary detector).
// No ReShade or ImGui types here.

#include "ext/reshade/ReShadeRegister.h"

#include <cstdint>

namespace ext::reshade
{
struct AddonStatus
{
    bool attempted = false;   // OnReshadeLoaded ran
    bool registered = false;  // ReShade accepted the registration (API 20)
    RegisterStatus reason = RegisterStatus::NoModule; // why not, when !registered
    bool probeEnabled = false; // [HudLess] ReShadeProbe (or Diagnostics once stacked)
    uint32_t apiVersion = kApiVersion;
};

// Cheap, any thread.
AddonStatus Status();

// True when the diagnostics are on. The ONE place that decides it (see ReShadeConfig.h).
bool ProbeEnabled();
} // namespace ext::reshade
