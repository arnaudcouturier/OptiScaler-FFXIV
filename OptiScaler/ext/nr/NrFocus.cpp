#include "pch.h"

#include "ext/nr/NrFocus.h"
#include "ext/nr/NrFocusGate.h"

#include <windows.h>

namespace ext::nr
{
namespace
{
constexpr ULONGLONG kSampleIntervalMs = 50;   // the foreground window does not need a per-frame answer
constexpr ULONGLONG kWindowRetryMs = 1000;    // how often a missing window handle is looked for again

// The game window the way the companion layer finds it: this process' visible, unowned top-level window with the
// largest client area. EnumWindows runs only when the cached handle is missing or stale, never per frame.
HWND FindGameWindow()
{
    struct Find
    {
        HWND result = nullptr;
        LONG area = 0;
    } find;

    EnumWindows(
        [](HWND window, LPARAM data) -> BOOL
        {
            DWORD pid = 0;
            RECT rect {};
            auto& f = *reinterpret_cast<Find*>(data);
            GetWindowThreadProcessId(window, &pid);
            if (pid == GetCurrentProcessId() && window != GetConsoleWindow() && IsWindowVisible(window) &&
                !GetWindow(window, GW_OWNER) && GetClientRect(window, &rect))
            {
                const LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
                if (area > f.area)
                {
                    f.result = window;
                    f.area = area;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));

    return find.result;
}

bool IsOurWindow(HWND window)
{
    DWORD pid = 0;
    return window != nullptr && IsWindow(window) && GetWindowThreadProcessId(window, &pid) != 0 &&
           pid == GetCurrentProcessId();
}
} // namespace

bool FocusAllowsNr(bool switchOn)
{
    static FocusGate gate;
    static HWND window = nullptr;
    static ULONGLONG lastSample = 0;
    static ULONGLONG lastWindowSearch = 0;
    static bool focused = true;
    static bool minimised = false;

    const ULONGLONG now = GetTickCount64();

    if (!switchOn)
    {
        // Keep the gate clean so that switching it on later starts a fresh count. Not sampled.
        lastSample = 0;
        return gate.Update(false, true, false, now);
    }

    if (lastSample == 0 || now - lastSample >= kSampleIntervalMs)
    {
        lastSample = now;

        if (!IsOurWindow(window) && now - lastWindowSearch >= kWindowRetryMs)
        {
            lastWindowSearch = now;
            window = FindGameWindow();
        }

        if (window == nullptr)
        {
            // No window to judge: never pause on a guess.
            focused = true;
            minimised = false;
        }
        else
        {
            // Same test the companion layer uses: the foreground window belongs to this process. A game dialog or
            // a helper window of ours counts as the game having focus; another application does not.
            DWORD foreground = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
            focused = foreground == GetCurrentProcessId();
            minimised = IsIconic(window) != FALSE;
        }
    }

    return gate.Update(true, focused, minimised, now);
}
} // namespace ext::nr
