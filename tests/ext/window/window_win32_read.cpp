// The module's window reads against a real (hidden) window: styles, rects, the client edge arithmetic of the border bug
// (a 400x300 window with WS_EX_CLIENTEDGE has a 396x296 client) and the owner thread.
#include "ext/window/WindowWin32.h"

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <thread>

using namespace ext::window;

static int g_failed = 0;
#define CHECK(cond)                                                                                                         \
    do                                                                                                                      \
    {                                                                                                                       \
        if (!(cond))                                                                                                        \
        {                                                                                                                   \
            std::printf("CHECK failed line %d: %s\n", __LINE__, #cond);                                                     \
            ++g_failed;                                                                                                     \
        }                                                                                                                   \
    } while (0)

namespace
{
HWND MakeWindow(DWORD style, DWORD exstyle)
{
    return CreateWindowExW(exstyle, L"STATIC", L"window-mode-test", style, 10, 20, 400, 300, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}
uint64_t Id(HWND h) { return reinterpret_cast<uint64_t>(h); }
} // namespace

int main()
{
    // A window of this thread: owner thread = current thread, styles and rects as created.
    {
        HWND hwnd = MakeWindow(WS_POPUP, WS_EX_CLIENTEDGE);
        CHECK(hwnd != nullptr);
        if (hwnd == nullptr)
            return 1;

        CHECK(win32::OwnerThread(Id(hwnd)) == GetCurrentThreadId());

        const WindowInfo info = win32::Read(Id(hwnd));
        CHECK(info.valid);
        CHECK((info.style & WS_POPUP) != 0);
        CHECK((info.exstyle & WS_EX_CLIENTEDGE) != 0);
        CHECK(info.left == 10 && info.top == 20);
        CHECK(info.right - info.left == 400 && info.bottom - info.top == 300);
        // The border bug in numbers: a client edge costs 2 px on every side of a frame-less window.
        CHECK(info.clientWidth == 396 && info.clientHeight == 296);

        DestroyWindow(hwnd);
        // A destroyed window is unreadable and has no owner thread.
        CHECK(!win32::Read(Id(hwnd)).valid);
        CHECK(win32::OwnerThread(Id(hwnd)) == 0);
    }

    // No handle: unknown, never a crash.
    CHECK(win32::OwnerThread(0) == 0);
    CHECK(!win32::Read(0).valid);

    // A window of another thread reports that thread.
    {
        DWORD otherThread = 0;
        HWND other = nullptr;
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        CHECK(ready != nullptr && done != nullptr);
        if (ready == nullptr || done == nullptr)
            return 1;
        std::thread worker(
            [&]
            {
                other = MakeWindow(WS_POPUP, 0);
                otherThread = GetCurrentThreadId();
                SetEvent(ready);
                WaitForSingleObject(done, 10000);
                if (other != nullptr)
                    DestroyWindow(other);
            });
        CHECK(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0);
        CHECK(other != nullptr);
        CHECK(otherThread != 0 && otherThread != GetCurrentThreadId());
        // The worker sits in WaitForSingleObject and never pumps messages. The module's reads must not send a message to the
        // window (a SendMessage-based read would block here until the worker's 10 s wait ends): they return at once.
        const auto start = std::chrono::steady_clock::now();
        CHECK(win32::OwnerThread(Id(other)) == otherThread);
        const WindowInfo foreign = win32::Read(Id(other));
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        CHECK(foreign.valid);
        CHECK((foreign.style & WS_POPUP) != 0 && foreign.right - foreign.left == 400 && foreign.bottom - foreign.top == 300);
        CHECK(elapsedMs < 2000);
        SetEvent(done);
        worker.join();
        CloseHandle(ready);
        CloseHandle(done);
    }

    return g_failed ? 1 : 0;
}
