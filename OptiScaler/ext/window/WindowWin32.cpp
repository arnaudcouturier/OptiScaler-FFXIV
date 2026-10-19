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

    // SetWindowLongPtr returns the previous value; 0 is also a valid previous extended style, so only "0 AND a non-zero last
    // error" is a failure. The game's window procedure (and everything subclassed on it) runs synchronously inside this call
    // and any API that fails benignly there leaves a stale last error behind, so the error alone proves nothing. (The border
    // fix only calls this when an edge bit is set, so the previous value is never 0 in production.)
    SetLastError(0);
    const LONG_PTR previous = SetWindowLongPtrW(window, GWL_EXSTYLE, static_cast<LONG_PTR>(exstyle));
    const bool styleOk = !(previous == 0 && GetLastError() != 0);

    // Always recompute the frame: the new style is in place whatever the last error says.
    const bool framed = SetWindowPos(window, nullptr, 0, 0, 0, 0,
                                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED) != FALSE;
    return styleOk && framed;
}
} // namespace ext::window::win32
