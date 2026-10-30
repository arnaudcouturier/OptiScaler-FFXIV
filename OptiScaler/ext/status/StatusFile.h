#pragma once
// The generic status.json file beside the DLL (docs/notes/status-json.md): written while [Status] Enabled is true,
// throttled (first write about 2 s after the first present, then on a state change at most once a second, plus a
// heartbeat every 10 s), atomic (a temporary file renamed over the old one), by a below-normal-priority worker
// thread that starts on the first present. Nothing here runs on the render-critical path: the scope hooks below are
// one cheap phase test when the phase is ignored, one config read when the key is off, a timestamp and a ring push
// when it is on. Every entry point is noexcept: a failure is counted into the file's own error tally and skipped,
// the game is never blocked and no feature behaviour changes (providers only read).

#include "ext/present/FramePacingCore.h" // Phase, for the scope hooks below

namespace ext::status
{
// Banner (startup): remembers the session start, the process id and the DLL folder. The worker thread still starts
// on the first present, so nothing is created in DllMain.
void OnStartup() noexcept;

// Bridge PresentScope entry (ext/hdr/BridgeHdr.cpp): the base cadence (the DX11 bridge present) for the frame-time
// percentiles. PacingScope constructor (ext/present/FramePacingTrace.cpp): the output cadence (the visible present).
// Other phases return at once, before the key read, the clock or the lock.
void OnScopeBegin(ext::present::Phase phase) noexcept;

// DLL_PROCESS_DETACH with lpReserved == null (FreeLibrary): signal the worker, delete our own file
// and lock when we still own them. Never waits: the worker may need the loader lock this detach holds.
// With any other lpReserved the process is going away: delete our own file and lock, nothing else.
void OnShutdown(bool freeLibrary) noexcept;
} // namespace ext::status
