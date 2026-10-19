#pragma once
// The module's Win32 calls on the game window (docs/FEATURES.md, "Window mode"). Kept apart from the controller so a test can
// drive them on a real window. No upstream types.

#include "ext/window/WindowProbe.h"

#include <cstdint>

namespace ext::window::win32
{
// GetWindowThreadProcessId; 0 when the handle is null or not a window.
uint32_t OwnerThread(uint64_t hwnd);

// GWL_STYLE, GWL_EXSTYLE, GetWindowRect, GetClientRect. valid = false when the handle is not a window.
WindowInfo Read(uint64_t hwnd);

// SetWindowLongPtr(GWL_EXSTYLE, exstyle) then SetWindowPos(NOMOVE | NOSIZE | NOZORDER | NOACTIVATE | FRAMECHANGED): the new
// extended style takes effect and the client area is recomputed. The window procedure runs inside the call (WM_NCCALCSIZE,
// WM_SIZE), so it must be made on the window's owner thread and never from inside a ResizeBuffers or a paced present. False
// when the handle is not a window, SetWindowLongPtr failed (previous value 0 AND a non-zero last error: a stale error left by
// the window procedure with a non-zero previous value is not a failure) or SetWindowPos failed; SetWindowPos always runs.
bool ApplyExStyle(uint64_t hwnd, uint32_t exstyle);
} // namespace ext::window::win32
