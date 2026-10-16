#pragma once
// Public face of the ReShade built-in add-on for the rest of the fork (menu card, the future boundary detector).
// No ReShade or ImGui types here.

#include "ext/reshade/ReShadeRegister.h"

#include <cstdint>

struct ID3D11DeviceContext;

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

// Entry point of the invasive probe: package A's boundary detector calls this at its candidate boundary (immediate
// context, before the boundary draw). One atomic load and a failed compare when the probe is not armed from the menu
// card; never throws, never lets an access violation out. Safe to call unconditionally.
void ProbeAtBoundary(ID3D11DeviceContext* context);

// True when the diagnostics are on. The ONE place that decides it (see ReShadeConfig.h).
bool ProbeEnabled();
} // namespace ext::reshade
