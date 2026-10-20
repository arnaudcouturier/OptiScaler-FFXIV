#pragma once
// DLSS-G render preset, live path: the in-memory hooks into NVIDIA's runtime (nvngx_dlssg.dll). Nothing on disk is touched and
// nothing is hooked until the first live change is asked for; with the request left on Auto the process never gets here.
//
// What is hooked, once, after the guards of PresetGuards.h passed on the module that asked the driver for its preset:
//   * the cached-settings reader answers the preset id with the ACTIVE request (PresetRequest.h);
//   * the endpoint create function sets a thread-local "creating with this context" scope;
//   * the configuration copy function, when called from the two copy sites of create, lets NVIDIA's own code rebuild the new
//     endpoint's private configuration (defaults + settings read with that context). The shared configuration and endpoints
//     that already exist are not touched.
// The module is kept loaded for the rest of the process, so the hooks and their trampolines never outlive it.

#include <string>

namespace ext::fg::preset::module
{
// nvapi/NvApiHooks.cpp seam (through PresetDrs.cpp): the module at `caller` asked the driver for the preset. Remembers it as a
// possible provider (a DLL that exports the NGX D3D12 entry points); cheap, any thread.
void ObserveDriverQuery(void* caller);

// FG dispatch thread, FG still running: READ-ONLY. Re-checks the recorded modules, runs the guards, chooses the provider and pins
// it. Hooks nothing and suspends nothing. False keeps the old request (reason in StatusText() and the log). A module that is not
// mapped any more is not a refusal and is retried; a module the guards refused stays refused (it is the same identity).
bool Prepare();

// FG dispatch thread, AFTER FG was switched off and its resources released: installs the three hooks in one Detours transaction
// that enlists only the calling thread (nothing is suspended). Final on failure for this process. Cheap once installed.
bool Install();

bool Ready();

// One line for the menu: not needed yet / ready (version, whether the file hash is a known one) / why it cannot be done.
std::string StatusText();
} // namespace ext::fg::preset::module
