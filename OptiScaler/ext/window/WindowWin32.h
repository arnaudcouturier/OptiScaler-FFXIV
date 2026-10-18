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
} // namespace ext::window::win32
