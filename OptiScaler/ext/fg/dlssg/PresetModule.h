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

// FG dispatch thread. True when the hooks are in place (installs them on the first call). False keeps the old request:
// the reason is in StatusText() and in the log. A "no provider seen yet" answer is retried on the next call, any other failure
// is final for this process.
bool Ensure();

bool Ready();

// One line for the menu: not needed yet / ready (version, whether the file hash is a known one) / why it cannot be done.
std::string StatusText();
} // namespace ext::fg::preset::module
