#pragma once
// E5: window lookup for composition swapchains (docs/SEAMS-catalog.md "present"). A composition swapchain
// (IDXGIFactory2::CreateSwapChainForComposition) has no HWND of its own, so the wrapper has to find the application
// window the overlay should attach to. Ported from the oracle's hooks/DxgiSwapchainSizing.h; the 0x0-to-window
// resolve that lived next to these helpers is NOT carried (upstream fixed the misclassification it was written for,
// SEAMS.md section 5).
//
// <windows.h> only, so a tests/ext recipe can build it standalone.

#include <windows.h>

namespace ext::present
{
// A window the overlay may attach to: in this process, not the console, not owned, and with a client area of at
// least 100x100 (anything smaller is a helper window, the same threshold the swapchain hook uses).
inline bool IsCompositionWindow(HWND window)
{
    DWORD pid = 0;
    RECT rect {};
    return window != nullptr && window != GetConsoleWindow() && GetWindow(window, GW_OWNER) == nullptr &&
           GetWindowThreadProcessId(window, &pid) != 0 && pid == GetCurrentProcessId() &&
           GetClientRect(window, &rect) && rect.right - rect.left >= 100 && rect.bottom - rect.top >= 100;
}

// The foreground window when it qualifies, else the one qualifying top-level window of this process. nullptr when
// none or more than one exists: another application's foreground window is never borrowed.
inline HWND FindCompositionWindow()
{
    const HWND foreground = GetForegroundWindow();
    if (IsCompositionWindow(foreground))
        return foreground;

    struct Search
    {
        HWND window = nullptr;
        unsigned int count = 0;
    } search;

    EnumWindows(
        [](HWND window, LPARAM context) -> BOOL
        {
            auto& found = *reinterpret_cast<Search*>(context);
            if (IsCompositionWindow(window))
            {
                found.window = window;
                ++found.count;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));

    return search.count == 1 ? search.window : nullptr;
}
} // namespace ext::present
