// Step 1 of the window-mode module on fakes: with Mode=off nothing is logged and nothing is read; with probe and on one
// "Window:" line is logged per bridge resize and per set_fullscreen_state call, and the handler never prevents a call.
#include "window_test.h"

using namespace ext::window;
using wt::FakeEnv;

namespace
{
constexpr uint64_t kBridge = 0x1000;
constexpr uint64_t kHwnd = 0xABC;

FsEvent Event(bool fullscreen, uint64_t native = kBridge)
{
    FsEvent ev;
    ev.native = native;
    ev.hwnd = kHwnd;
    ev.fullscreen = fullscreen;
    ev.target = 0;
    return ev;
}

// A counting Env: Mode=off must not even read anything.
struct CountingEnv final : Env
{
    int calls = 0;
    bool ForceBorderless() override { ++calls; return false; }
    bool DlssgOutput() override { ++calls; return false; }
    bool Exclusive() override { ++calls; return false; }
    FgSnapshot Fg() override { ++calls; return {}; }
    uint32_t CurrentThread() override { ++calls; return 1; }
    uint32_t OwnerThread(uint64_t) override { ++calls; return 1; }
    WindowInfo ReadWindow(uint64_t) override { ++calls; return {}; }
    bool ApplyExStyle(uint64_t, uint32_t) override { ++calls; return false; }
    void RequestFgOff() override { ++calls; }
    uint64_t NowMs() override { ++calls; return 0; }
    void Retain(uint64_t) override { ++calls; }
    void Release(uint64_t) override { ++calls; }
    int32_t Replay(uint64_t, bool) override { ++calls; return 0; }
    void Log(const std::string&, bool) override { ++calls; }
};
} // namespace

int main()
{
    // Mode=off: nothing runs, nothing is logged, the call is not prevented.
    {
        CountingEnv env;
        Controller c;
        c.OnResizeDone(env, Mode::Off, kBridge, kHwnd, 3840, 2160);
        c.OnPresentBegin(env, Mode::Off);
        c.OnPresentEnd(env, Mode::Off);
        CHECK(!c.OnSetFullscreenState(env, Mode::Off, Event(true)));
        CHECK(env.calls == 0);
    }

    // Probe: the module announces itself once, then one line per resize.
    {
        FakeEnv env;
        Controller c;
        c.OnResizeDone(env, Mode::Probe, kBridge, kHwnd, 3840, 2160);
        CHECK(env.logs.size() == 2);
        CHECK_CONTAINS(env.logs[0], "Window: module active, mode=probe");
        CHECK(env.logs[1].rfind("Window: resize-done ", 0) == 0);
        c.OnResizeDone(env, Mode::Probe, kBridge, kHwnd, 3840, 2160);
        CHECK(env.logs.size() == 3);
        for (bool warn : env.warns)
            CHECK(!warn);
    }

    // The resize line shows the styles, the rects, the exclusive flag and its previous value.
    {
        FakeEnv env;
        env.window.style = 0x16CF0000u;
        env.window.exstyle = 0x200u;
        env.window.clientWidth = 3836;
        env.window.clientHeight = 2156;
        env.exclusive = true;
        Controller c;
        c.OnResizeDone(env, Mode::Probe, kBridge, kHwnd, 3840, 2160);
        env.exclusive = false;
        c.OnResizeDone(env, Mode::Probe, kBridge, kHwnd, 3840, 2160);
        const std::string line = env.Last();
        CHECK_CONTAINS(line, "style=0x16cf0000");
        CHECK_CONTAINS(line, "exstyle=0x00000200");
        CHECK_CONTAINS(line, "client=3836x2156");
        CHECK_CONTAINS(line, "exclusive=0 prevExclusive=1"); // true -> false is visible
        CHECK_CONTAINS(line, "forceBorderless=1");
    }

    // The present thread is recorded at the present, and the probe shows it next to the owner and current thread.
    {
        FakeEnv env;
        env.current = 77;
        env.owner = 77;
        Controller c;
        c.OnPresentBegin(env, Mode::Probe);
        c.OnPresentEnd(env, Mode::Probe);
        CHECK(c.PresentThread() == 77);
        c.OnResizeDone(env, Mode::Probe, kBridge, kHwnd, 3840, 2160);
        CHECK_CONTAINS(env.Last(), "ownerTid=77 currentTid=77 presentTid=77 sameThread=yes");
        env.owner = 5; // the window belongs to another thread
        c.OnResizeDone(env, Mode::Probe, kBridge, kHwnd, 3840, 2160);
        CHECK_CONTAINS(env.Last(), "sameThread=no");
    }

    // "DLSS-G off confirmed" in the line follows the gate: not before a complete present with the feature inactive.
    {
        FakeEnv env;
        env.fg = FgSnapshot { true, true, false }; // active
        Controller c;
        c.OnPresentBegin(env, Mode::Probe);
        c.OnPresentEnd(env, Mode::Probe);
        c.OnResizeDone(env, Mode::Probe, kBridge, kHwnd, 3840, 2160);
        CHECK_CONTAINS(env.Last(), "fg=active fgOffConfirmed=no");
        env.fg = FgSnapshot { true, false, true }; // Deactivate: paused, inactive
        c.OnPresentBegin(env, Mode::Probe);
        c.OnPresentEnd(env, Mode::Probe);
        c.OnResizeDone(env, Mode::Probe, kBridge, kHwnd, 3840, 2160);
        CHECK_CONTAINS(env.Last(), "fg=paused fgOffConfirmed=yes");
        // A resize deactivates frame generation (Streamline applies "off" inside the next present): what was confirmed before
        // it no longer counts, the next line says no until a complete present has gone by.
        c.OnResizeDone(env, Mode::Probe, kBridge, kHwnd, 3840, 2160);
        CHECK_CONTAINS(env.Last(), "fgOffConfirmed=no");
        c.OnPresentBegin(env, Mode::Probe);
        c.OnPresentEnd(env, Mode::Probe);
        c.OnResizeDone(env, Mode::Probe, kBridge, kHwnd, 3840, 2160);
        CHECK_CONTAINS(env.Last(), "fgOffConfirmed=yes");
        // Not for another output.
        env.dlssg = false;
        c.OnResizeDone(env, Mode::Probe, kBridge, kHwnd, 3840, 2160);
        CHECK_CONTAINS(env.Last(), "output=other");
        CHECK_CONTAINS(env.Last(), "fgOffConfirmed=no");
    }

    // set_fullscreen_state: one line, the verdict and the result Mode=on would return; the probe itself never prevents.
    {
        FakeEnv env;
        env.fg = FgSnapshot { true, true, false };
        Controller c;
        c.OnPresentBegin(env, Mode::Probe); // the present thread is known
        c.OnPresentEnd(env, Mode::Probe);
        c.OnResizeDone(env, Mode::Probe, kBridge, kHwnd, 3840, 2160); // the bridge is known
        const size_t before = env.logs.size();
        const bool prevented = c.OnSetFullscreenState(env, Mode::Probe, Event(true));
        CHECK(!prevented);
        CHECK(env.logs.size() == before + 1);
        CHECK_CONTAINS(env.Last(), "Window: set-fullscreen-state ");
        CHECK_CONTAINS(env.Last(), "call=SetFullscreenState(TRUE)");
        CHECK_CONTAINS(env.Last(), "verdict=defer wouldReturn=true returned=false");
        CHECK_CONTAINS(env.Last(), "sameObject=yes");

        // A re-assertion of the state in effect: would pass.
        env.exclusive = true;
        CHECK(!c.OnSetFullscreenState(env, Mode::Probe, Event(true)));
        CHECK_CONTAINS(env.Last(), "verdict=pass(no window change) wouldReturn=false returned=false");

        // Another swapchain: would pass, and the line shows both pointers.
        env.exclusive = false;
        CHECK(!c.OnSetFullscreenState(env, Mode::Probe, Event(true, 0x2000)));
        CHECK_CONTAINS(env.Last(), "native=0x2000 bridge=0x1000 sameObject=no");
        CHECK_CONTAINS(env.Last(), "wouldReturn=false");

        // Frame generation off: would pass.
        env.fg = FgSnapshot { true, false, false };
        CHECK(!c.OnSetFullscreenState(env, Mode::Probe, Event(true)));
        CHECK_CONTAINS(env.Last(), "verdict=pass(FG not active)");
    }

    // A call before any resize or present is seen: the bridge and the present thread are unknown, the verdict says so.
    {
        FakeEnv env;
        env.fg = FgSnapshot { true, true, false };
        Controller c;
        CHECK(!c.OnSetFullscreenState(env, Mode::Probe, Event(true)));
        CHECK_CONTAINS(env.Last(), "wouldReturn=false returned=false");
        CHECK_CONTAINS(env.Last(), "hwnd=0xabc"); // the event's window handle stands in for the unknown stored one
    }

    return wt::g_failed ? 1 : 0;
}
