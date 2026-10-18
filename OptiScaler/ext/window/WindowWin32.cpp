#include "pch.h"

#include "ext/window/WindowRules.h"
#include "ext/window/WindowWin32.h"

namespace ext::window
{
// The pure rules carry their own copies of the winuser.h bit values so they need no SDK header.
static_assert(kStyleCaption == WS_CAPTION && kStyleThickFrame == WS_THICKFRAME, "winuser.h style bits changed");
static_assert(kExStyleClientEdge == WS_EX_CLIENTEDGE && kExStyleWindowEdge == WS_EX_WINDOWEDGE,
              "winuser.h extended style bits changed");
} // namespace ext::window

namespace ext::window::win32
{
uint32_t OwnerThread(uint64_t hwnd)
{
    HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(hwnd));
    if (window == nullptr || !IsWindow(window))
        return 0;
    return GetWindowThreadProcessId(window, nullptr);
}

WindowInfo Read(uint64_t hwnd)
{
    WindowInfo info;
    HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(hwnd));
    if (window == nullptr || !IsWindow(window))
        return info;

    RECT rect {};
    RECT client {};
    if (!GetWindowRect(window, &rect) || !GetClientRect(window, &client))
        return info;

    info.valid = true;
    info.style = static_cast<uint32_t>(GetWindowLongPtrW(window, GWL_STYLE));
    info.exstyle = static_cast<uint32_t>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    info.left = rect.left;
    info.top = rect.top;
    info.right = rect.right;
    info.bottom = rect.bottom;
    info.clientWidth = client.right - client.left;
    info.clientHeight = client.bottom - client.top;
    return info;
}
} // namespace ext::window::win32

namespace ext::window::win32
{
bool ApplyExStyle(uint64_t hwnd, uint32_t exstyle)
{
    HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(hwnd));
    if (window == nullptr || !IsWindow(window))
        return false;

    // SetWindowLongPtr returns the previous value, and 0 is a valid previous extended style: the last error tells.
    SetLastError(0);
    SetWindowLongPtrW(window, GWL_EXSTYLE, static_cast<LONG_PTR>(exstyle));
    if (GetLastError() != 0)
        return false;

    return SetWindowPos(window, nullptr, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED) != FALSE;
}
} // namespace ext::window::win32
