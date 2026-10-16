#pragma once
// Event subscription and the live ReShade backend of the probe (S10 package D0). Only used when the diagnostics are
// on ([HudLess] ReShadeProbe). No ReShade or ImGui types in this header.

#include "ext/reshade/ReShadeProbe.h"

#include <filesystem>
#include <string>

namespace ext::reshade
{
// Creates the probe session (report below `baseFolder` = <game>\OptiScaler\hudless-diag) and subscribes to the runtime
// events. Called once, from DllMain context (no file I/O happens here: the report folder is created on its first line).
// ReShade must already have accepted the registration. False = subscription failed (logged by the caller).
bool InstallProbe(const std::filesystem::path& baseFolder, const std::string& registrationLine);

// nullptr until InstallProbe succeeded. Heap-allocated, never destroyed.
probe::Session* ProbeSession();
probe::FileReport* ProbeReport();
} // namespace ext::reshade
