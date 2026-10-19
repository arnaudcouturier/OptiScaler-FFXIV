// The border fix against a real (hidden) window: clearing WS_EX_CLIENTEDGE with ApplyExStyle gives the 400x300 window its
// full 400x300 client back, keeps its rectangle and its other styles, and runs the window procedure synchronously inside
// the call (the WM_SIZE that can reach the game's own code, which is why the module only calls it from the owner thread
// and never inside ResizeBuffers).
#include "ext/window/WindowRules.h"
#include "ext/window/WindowWin32.h"

#include <Windows.h>

#include <cstdio>

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
int g_sizeMessages = 0;
bool g_inApply = false;
int g_sizeMessagesInsideApply = 0;
DWORD g_staleError = 0; // when non-zero the window procedure leaves this last error behind, like any API that fails benignly in it

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if ((msg == WM_STYLECHANGING || msg == WM_STYLECHANGED) && g_staleError != 0)
        SetLastError(g_staleError);
    if (msg == WM_SIZE)
    {
        ++g_sizeMessages;
        if (g_inApply)
            ++g_sizeMessagesInsideApply;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND MakeWindow(DWORD style, DWORD exstyle)
{
    WNDCLASSW wc {};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OptiWindowModeTest";
    RegisterClassW(&wc);
    return CreateWindowExW(exstyle, L"OptiWindowModeTest", L"window-mode-test", style, 10, 20, 400, 300, nullptr, nullptr,
                           wc.hInstance, nullptr);
}
uint64_t Id(HWND h) { return reinterpret_cast<uint64_t>(h); }
} // namespace

int main()
{
    HWND hwnd = MakeWindow(WS_POPUP, WS_EX_CLIENTEDGE | WS_EX_APPWINDOW | WS_EX_TOPMOST);
    CHECK(hwnd != nullptr);
    if (hwnd == nullptr)
        return 1;

    WindowInfo before = win32::Read(Id(hwnd));
    CHECK(before.valid);
    CHECK(before.clientWidth == 396 && before.clientHeight == 296); // the 3836x2156-in-3840x2160 picture in small

    const auto fixed = ClearEdge(before.style, before.exstyle);
    CHECK(fixed.has_value());
    if (!fixed)
        return 1;
    CHECK((*fixed & WS_EX_CLIENTEDGE) == 0);
    CHECK((*fixed & WS_EX_APPWINDOW) != 0 && (*fixed & WS_EX_TOPMOST) != 0);

    g_sizeMessages = 0;
    g_inApply = true;
    const bool ok = win32::ApplyExStyle(Id(hwnd), *fixed);
    g_inApply = false;
    CHECK(ok);

    const WindowInfo after = win32::Read(Id(hwnd));
    CHECK(after.valid);
    CHECK((after.exstyle & WS_EX_CLIENTEDGE) == 0);
    CHECK((after.exstyle & WS_EX_APPWINDOW) != 0 && (after.exstyle & WS_EX_TOPMOST) != 0);
    CHECK(after.style == before.style);
    // The window rectangle is untouched, the client area is the whole window.
    CHECK(after.left == before.left && after.top == before.top && after.right == before.right && after.bottom == before.bottom);
    CHECK(after.clientWidth == 400 && after.clientHeight == 300);
    // The client size changed, and the window procedure saw it inside the call.
    CHECK(g_sizeMessages >= 1);
    CHECK(g_sizeMessagesInsideApply >= 1);
    // Nothing left to fix.
    CHECK(!ClearEdge(after.style, after.exstyle).has_value());

    DestroyWindow(hwnd);

    // M2: SetWindowLongPtr runs the game's window procedure synchronously; whatever it leaves in the last error must not turn a
    // style change that worked into a failure. The return value (the previous exstyle) decides, the error only when it is 0.
    {
        HWND stale = MakeWindow(WS_POPUP, WS_EX_CLIENTEDGE | WS_EX_APPWINDOW | WS_EX_TOPMOST);
        CHECK(stale != nullptr);
        if (stale == nullptr)
            return 1;
        const WindowInfo b = win32::Read(Id(stale));
        CHECK(b.clientWidth == 396 && b.clientHeight == 296);
        const auto fix = ClearEdge(b.style, b.exstyle);
        CHECK(fix.has_value());
        g_staleError = ERROR_INVALID_PARAMETER;
        const bool okStale = win32::ApplyExStyle(Id(stale), fix ? *fix : 0);
        g_staleError = 0;
        CHECK(okStale); // previous exstyle non-zero, stale error: success
        const WindowInfo a = win32::Read(Id(stale));
        CHECK((a.exstyle & WS_EX_CLIENTEDGE) == 0);
        CHECK(a.clientWidth == 400 && a.clientHeight == 300); // SetWindowPos(FRAMECHANGED) still ran
        DestroyWindow(stale);
    }
    {
        // a previous exstyle of 0 is a valid value: no error, success; the style is applied
        HWND zero = MakeWindow(WS_POPUP, 0);
        CHECK(zero != nullptr);
        if (zero == nullptr)
            return 1;
        SetWindowLongPtrW(zero, GWL_EXSTYLE, 0); // make the previous value exactly 0 whatever the system added
        SetLastError(ERROR_INVALID_PARAMETER); // an error left by something else before the call must not count either
        CHECK(win32::ApplyExStyle(Id(zero), WS_EX_TOOLWINDOW));
        CHECK((win32::Read(Id(zero)).exstyle & WS_EX_TOOLWINDOW) != 0);
        DestroyWindow(zero);
    }

    {
        // A style change that really fails (previous value 0 AND an error: a window of another process is refused) is still a
        // failure. The desktop window belongs to the system; nothing visible happens to it either way.
        const HWND desktop = GetDesktopWindow();
        CHECK(desktop != nullptr);
        CHECK(!win32::ApplyExStyle(Id(desktop), WS_EX_TOOLWINDOW));
    }
    // A destroyed window cannot be changed, and says so.
    CHECK(!win32::ApplyExStyle(Id(hwnd), 0));
    CHECK(!win32::ApplyExStyle(0, 0));

    return g_failed ? 1 : 0;
}
