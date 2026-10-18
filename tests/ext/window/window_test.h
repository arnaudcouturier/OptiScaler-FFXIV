#pragma once
// Shared pieces of the window-mode tests: a tiny check macro (kept active whatever the build flags are) and a fake Env.
// The fake grows with the module: this file is included by every test of tests/ext/window.

#include "ext/window/WindowController.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace wt
{
inline int g_failed = 0;
} // namespace wt

#define CHECK(cond)                                                                                                         \
    do                                                                                                                      \
    {                                                                                                                       \
        if (!(cond))                                                                                                        \
        {                                                                                                                   \
            std::printf("CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond);                                             \
            ++wt::g_failed;                                                                                                 \
        }                                                                                                                   \
    } while (0)

#define CHECK_CONTAINS(text, part)                                                                                          \
    do                                                                                                                      \
    {                                                                                                                       \
        if (std::string(text).find(part) == std::string::npos)                                                              \
        {                                                                                                                   \
            std::printf("CHECK failed %s:%d: \"%s\" not found in: %s\n", __FILE__, __LINE__, std::string(part).c_str(),     \
                        std::string(text).c_str());                                                                         \
            ++wt::g_failed;                                                                                                 \
        }                                                                                                                   \
    } while (0)

namespace wt
{
using namespace ext::window;

// A 3840x2160 borderless window as the fork leaves it after the startup restyle.
inline WindowInfo BorderlessWindow()
{
    WindowInfo w;
    w.valid = true;
    w.style = 0x90000000u; // WS_POPUP | WS_VISIBLE
    w.exstyle = 0x00040000u; // WS_EX_APPWINDOW
    w.left = 0;
    w.top = 0;
    w.right = 3840;
    w.bottom = 2160;
    w.clientWidth = 3840;
    w.clientHeight = 2160;
    return w;
}

struct ApplyCall
{
    uint64_t hwnd = 0;
    uint32_t exstyle = 0;
    bool fgActiveAtCall = false;
    uint32_t thread = 0;
};

struct ReplayCall
{
    uint64_t native = 0;
    bool fullscreen = false;
    bool fgActiveAtCall = false;
    uint32_t thread = 0;
};

struct FakeEnv final : Env
{
    bool forceBorderless = true;
    bool dlssg = true;
    bool exclusive = false;
    FgSnapshot fg { true, false, false };
    uint32_t current = 100;
    uint32_t owner = 100;
    WindowInfo window = BorderlessWindow();

    // Window call of the border fix
    bool applyResult = true;
    std::vector<ApplyCall> applied;
    std::function<void()> onApply; // runs inside the call, like the game's window procedure would

    // Frame generation request, clock, swapchain references and the replayed call
    uint64_t nowMs = 0;
    bool autoOff = true; // RequestFgOff behaves like Deactivate: inactive and paused at once
    int fgOffRequests = 0;
    std::vector<ReplayCall> replays;
    int32_t replayResult = 0; // S_OK
    std::function<void()> onReplay; // runs inside the call, like the hook and the game's window procedure would

    std::string throwOn; // a log line containing this text throws (to test cleanup)
    std::vector<std::string> logs;
    std::vector<bool> warns;

    bool ForceBorderless() override { return forceBorderless; }
    bool DlssgOutput() override { return dlssg; }
    bool Exclusive() override { return exclusive; }
    FgSnapshot Fg() override { return fg; }
    uint32_t CurrentThread() override { return current; }
    uint32_t OwnerThread(uint64_t) override { return owner; }
    WindowInfo ReadWindow(uint64_t) override { return window; }
    uint64_t monitor = 0x77;
    uint64_t MonitorOfWindow(uint64_t) override { return monitor; }
    bool ApplyExStyle(uint64_t hwnd, uint32_t exstyle) override
    {
        applied.push_back({ hwnd, exstyle, fg.present && fg.active, current });
        if (applyResult)
        {
            window.exstyle = exstyle;
            if ((exstyle & (kExStyleClientEdge | kExStyleWindowEdge)) == 0)
            {
                window.clientWidth = window.right - window.left;
                window.clientHeight = window.bottom - window.top;
            }
        }
        if (onApply)
            onApply();
        return applyResult;
    }
    void RequestFgOff() override
    {
        ++fgOffRequests;
        if (autoOff)
        {
            fg.active = false;
            fg.paused = true;
        }
    }
    uint64_t NowMs() override { return nowMs; }
    int32_t Replay(uint64_t native, bool fullscreen) override
    {
        replays.push_back({ native, fullscreen, fg.present && fg.active, current });
        if (onReplay)
            onReplay();
        return replayResult;
    }
    void Log(const std::string& line, bool warn) override
    {
        if (!throwOn.empty() && line.find(throwOn) != std::string::npos)
            throw std::runtime_error("log");
        logs.push_back(line);
        warns.push_back(warn);
    }

    std::string Last() const { return logs.empty() ? std::string() : logs.back(); }
    std::string All() const
    {
        std::string all;
        for (const auto& l : logs)
            all += l + "\n";
        return all;
    }
};
} // namespace wt
