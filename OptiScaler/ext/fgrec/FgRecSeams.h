#pragma once
// Seams of the FG event recorder (FgRecorder.h holds the core), included by ext/Seams.h. Bodies:
// ext/fgrec/FgRecorder.cpp (the core: queue, writer, files) and ext/fgrec/FgRecSeams.cpp (the process
// reads: the key, the provider and frame ids, the session info). Nothing here throws into the caller.

#include "ext/fgrec/FgRecorder.h" // kinds, core Shutdown (dllmain calls it directly: it needs no key)

#include <windows.h>

#include <cstdint>

namespace ext::fgrec
{
// ---- Upstream hosts (one line each, beside the pacing scopes that already time these presents) ----
// hooks/FG_Hooks.cpp FGHooks::FGPresent: the FG swapchain present, with its HRESULT.
void FgPresentBegin(bool willPresent, UINT sync, UINT flags) noexcept;
void FgPresentEnd(bool willPresent, HRESULT hr) noexcept;
// wrapped/wrapped_swapchain.cpp LocalPresent: the visible output present, with its HRESULT.
void OutPresentBegin(bool willPresent, UINT sync, UINT flags) noexcept;
void OutPresentEnd(bool willPresent, HRESULT hr) noexcept;
// dllmain.cpp DLL_PROCESS_DETACH calls core Shutdown (declared above): signal the worker, never wait.

// ---- ext-internal taps (zero new upstream lines: the seam already exists at each site) ----
// A Reflex sleep the pacing scope just timed (FramePacingTrace.cpp): ticks are QPC ticks.
bool RecArmed() noexcept; // the cached key: one relaxed load, true while records are wanted
void OnReflexSleep(int64_t ticks) noexcept;
// The hidden DX11 present (FramePacingTrace.cpp HiddenPresent): one record with HRESULT and duration.
void OnHiddenPresent(HRESULT hr, int64_t ticks) noexcept;
// Every AutoMFG log line (DynamicMfgSeams.cpp): the switch, the verdict and the stall seam arrive as text.
void OnAutoMfg(bool warn, const char* text) noexcept;
// An NR model rebuild hitched its frame (DynamicMfgSeams.cpp NoteModelRebuild). Park and build of one
// rebuild share the frame, so the second same-frame call is dropped: one rebuild is one record.
void OnNrRebuild() noexcept;
// Provider (re)activation and teardown (Activation.cpp): a new generation starts on activation.
void OnProviderOn(const char* label) noexcept;
void OnProviderOff(const char* label) noexcept;
// A live DLSS-G render preset switch (PresetSeams.cpp).
void OnPresetSwitch(const char* from, const char* to) noexcept;
// A provider module load, by file name only, never a path (the XeFG and Streamline loader seams).
void OnModule(const wchar_t* fileName) noexcept;
// A XeFG dynamic factor change (the XeFG dispatch seam).
void OnXeFactor(int from, int to) noexcept;

// Process reads shared by the taps: the numeric provider id and the short label of an FG output,
// by its underlying value (so this header stays free of the upstream State type).
unsigned MapProvider(uint32_t out);
const char* ProviderLabel(uint32_t out);
} // namespace ext::fgrec
