#include "pch.h"

#include "ext/window/WindowRules.h"
#include "ext/window/WindowWin32.h"

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
