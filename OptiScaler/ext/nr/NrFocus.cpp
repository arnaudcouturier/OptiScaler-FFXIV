#include "pch.h"

#include "ext/nr/NrFocus.h"
#include "ext/nr/NrFocusTracker.h"

#include <windows.h>

#include <mutex>

namespace ext::nr
{
namespace
{
constexpr ULONGLONG kWindowRetryMs = 1000; // how often a missing window handle is looked for again

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

// One look at the game window, for every consumer (the tracker asks at most every 50 ms).
struct WindowSampler
{
    WindowSample operator()() const
    {
        static HWND window = nullptr;
        static ULONGLONG lastWindowSearch = 0;
        const ULONGLONG now = GetTickCount64();

        if (!IsOurWindow(window) && now - lastWindowSearch >= kWindowRetryMs)
        {
            lastWindowSearch = now;
            window = FindGameWindow();
        }

        // No window to judge: never pause on a guess.
        if (window == nullptr)
            return { true, false };

        // Same test the companion layer uses: the foreground window belongs to this process. A game dialog or a
        // helper window of ours counts as the game having focus; another application does not.
        DWORD foreground = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
        return { foreground == GetCurrentProcessId(), IsIconic(window) != FALSE };
    }
};

// The scene NR pass and the character preview NR share the window sample and each have their own gate (see
// NrFocusTracker.h). A leaf mutex: the two consumers run on different threads, and nothing is called while it is held
// except the window sampler and a log line.
FocusTracker<WindowSampler>& Tracker()
{
    static FocusTracker<WindowSampler> tracker { WindowSampler {} };
    return tracker;
}

std::mutex& TrackerMutex()
{
    static std::mutex mutex;
    return mutex;
}
} // namespace

bool FocusAllowsNr(bool switchOn)
{
    std::lock_guard lock(TrackerMutex());
    return Tracker().Allows(FocusConsumer::Scene, switchOn, GetTickCount64());
}

bool FocusAllowsPreviewNr(bool switchOn)
{
    static PauseEpisodes episodes; // under the mutex: one log line per episode, each way

    std::lock_guard lock(TrackerMutex());
    const bool allowed = Tracker().Allows(FocusConsumer::Preview, switchOn, GetTickCount64());
    switch (episodes.Note(allowed))
    {
    case PauseEpisodes::Event::Paused:
        LOG_INFO("CharaView NR: paused, window not focused");
        break;
    case PauseEpisodes::Event::Resumed:
        LOG_INFO("CharaView NR: resumed");
        break;
    case PauseEpisodes::Event::None:
        break;
    }
    return allowed;
}
} // namespace ext::nr
