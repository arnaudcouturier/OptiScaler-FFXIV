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

// Writes the report header (registration line) if no callback has done it yet, which also creates the report folder and file.
// InstallProbe runs inside DllMain and must not do file I/O; the first ReShade callback normally starts the report, so when
// ReShade delivers NO event at all (the most likely failure) nothing would exist to send back: the menu card calls this so
// that opening it leaves a report that says so. Not for DllMain.
void EnsureReportStarted();

// nullptr until InstallProbe succeeded. Heap-allocated, never destroyed.
probe::Session* ProbeSession();
probe::FileReport* ProbeReport();
} // namespace ext::reshade
