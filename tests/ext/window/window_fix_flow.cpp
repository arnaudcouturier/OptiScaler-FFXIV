// Step 2 on fakes (B1-a): after the game leaves the fork's borderless "fullscreen" (SCExclusiveFullscreen true -> false, read
// at the bridge resize) a stray window edge is cleared from the end of a game present, and only when the DLSS-G frame
// generation is confirmed off, from the window's owner thread, and never for a real windowed mode.
#include "window_test.h"

using namespace ext::window;
using wt::FakeEnv;

namespace
{
constexpr uint64_t kBridge = 0x1000;
constexpr uint64_t kHwnd = 0xABC;

const FgSnapshot kActive { true, true, false };
const FgSnapshot kPaused { true, false, true };
const FgSnapshot kOff { true, false, false };

// One game present with frame generation in state `entry` at the start and `exit` at the end.
void Present(Controller& c, FakeEnv& env, Mode mode, const FgSnapshot& entry, const FgSnapshot& exit)
{
    env.fg = entry;
    c.OnPresentBegin(env, mode);
    env.fg = exit;
    c.OnPresentEnd(env, mode);
}

// The window as the fork's leave branch plus the game's own style write leave it: frame-less, with a client edge.
void MakeBorderBug(FakeEnv& env)
{
    env.window = wt::BorderlessWindow();
    env.window.exstyle = 0x00000300u; // WS_EX_OVERLAPPEDWINDOW
    env.window.clientWidth = 3836;
    env.window.clientHeight = 2156;
}

// Enter the pseudo fullscreen, then leave it: the second resize is the one that arms the fix.
void EnterThenLeave(Controller& c, FakeEnv& env, Mode mode)
{
    env.exclusive = true;
    c.OnResizeDone(env, mode, kBridge, kHwnd, 3840, 2160);
    env.exclusive = false;
    c.OnResizeDone(env, mode, kBridge, kHwnd, 3840, 2160);
}

int Warnings(const FakeEnv& env)
{
    int n = 0;
    for (bool w : env.warns)
        n += w ? 1 : 0;
    return n;
}
} // namespace

int main()
{
    // The reference case: leave with frame generation off. Applied once, after one complete present, never before.
    {
        FakeEnv env;
        Controller c;
        env.fg = kOff;
        MakeBorderBug(env);
        Present(c, env, Mode::On, kOff, kOff); // earlier presents do not matter
        EnterThenLeave(c, env, Mode::On);
        CHECK(env.applied.empty()); // nothing happens inside the resize
        CHECK_CONTAINS(env.All(), "border fix armed");
        CHECK_CONTAINS(env.Last(), "fixArmed=yes");

        // The first present after the leave starts and ends with the feature off, but the gate also needs the Deactivate
        // that the resize did: here the feature was already off, one complete present confirms.
        Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.size() == 1);
        CHECK(env.applied[0].hwnd == kHwnd);
        CHECK(env.applied[0].exstyle == 0u); // both edge bits gone
        CHECK(!env.applied[0].fgActiveAtCall);
        CHECK(env.applied[0].thread == env.owner);
        CHECK_CONTAINS(env.All(), "border fix applied");
        CHECK(env.window.clientWidth == 3840 && env.window.clientHeight == 2160); // the fake restores the client area

        // Disarmed: later presents change nothing.
        Present(c, env, Mode::On, kOff, kOff);
        Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.size() == 1);
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        CHECK_CONTAINS(env.Last(), "fixArmed=no");
    }

    // The leave with frame generation running (the resize deactivates it): wait for the complete present, apply after.
    {
        FakeEnv env;
        Controller c;
        MakeBorderBug(env);
        env.fg = kActive;
        Present(c, env, Mode::On, kActive, kActive);
        EnterThenLeave(c, env, Mode::On);
        env.fg = kPaused; // the bridge's resize deactivated frame generation
        // The present that was running when it was deactivated started with the feature active: not a confirmation.
        Present(c, env, Mode::On, kActive, kPaused);
        CHECK(env.applied.empty());
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.applied.size() == 1);
        CHECK(!env.applied[0].fgActiveAtCall);
    }

    // The re-activation race: frame generation comes back after a few presents (the pause is only about ten frames). The fix
    // never runs while the feature is active, and still runs in the next off period.
    {
        FakeEnv env;
        Controller c;
        env.window = wt::BorderlessWindow(); // the game has not rewritten its frame-less style yet
        env.window.style = 0x16CF0000u;      // the fork's WS_OVERLAPPEDWINDOW | WS_VISIBLE, with a caption
        env.window.exstyle = 0x300u;
        env.fg = kOff;
        EnterThenLeave(c, env, Mode::On);
        Present(c, env, Mode::On, kOff, kOff);
        Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.empty()); // caption present: left alone for now, still armed
        Present(c, env, Mode::On, kOff, kActive); // frame generation is back
        env.window.style = 0x90000000u;            // the game rewrites its frame-less style while the feature is on
        Present(c, env, Mode::On, kActive, kActive);
        Present(c, env, Mode::On, kActive, kActive);
        CHECK(env.applied.empty()); // the window needs the fix, but the feature is active
        Present(c, env, Mode::On, kActive, kPaused); // asked off again
        CHECK(env.applied.empty()); // the present that ended the activity does not count
        Present(c, env, Mode::On, kPaused, kPaused);
        CHECK(env.applied.size() == 1);
        for (const auto& a : env.applied)
            CHECK(!a.fgActiveAtCall);
    }

    // A real windowed mode (caption and sizing frame) is never touched, however long the check runs.
    {
        FakeEnv env;
        Controller c;
        env.window = wt::BorderlessWindow();
        env.window.style = 0x16CF0000u;
        env.window.exstyle = 0x300u;
        env.fg = kOff;
        EnterThenLeave(c, env, Mode::On);
        for (int i = 0; i < 400; ++i)
            Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.empty());
        CHECK_CONTAINS(env.All(), "border fix gave up");
        CHECK(Warnings(env) == 0);
        // It gave up once: more presents log nothing more.
        const size_t logs = env.logs.size();
        for (int i = 0; i < 50; ++i)
            Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.logs.size() == logs);
    }

    // The logged sequence: the startup enter has no resize behind it, so the last resize saw the flag false. The hook flips the
    // flag on the enter and again on the leave; only presents see it in between. The leave must still arm the fix: at the
    // next present (and, with a resize after the leave, at that resize).
    for (const bool resizeAfterLeave : { false, true })
    {
        FakeEnv env;
        Controller c;
        MakeBorderBug(env);
        env.fg = kOff;
        env.exclusive = false;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160); // the startup resize: flag false
        env.exclusive = true;                           // the hook's enter, no resize after it
        Present(c, env, Mode::On, kOff, kOff);
        Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.empty());
        env.exclusive = false; // the hook's leave
        if (resizeAfterLeave)
        {
            c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
            CHECK_CONTAINS(env.Last(), "exclusive=0 prevExclusive=1"); // the present's sample is the previous value
            CHECK_CONTAINS(env.All(), "seen at a bridge resize");
            CHECK(env.applied.empty());
            Present(c, env, Mode::On, kOff, kOff);
        }
        else
        {
            Present(c, env, Mode::On, kOff, kOff); // armed at this present's end and applied in the same call
            CHECK_CONTAINS(env.All(), "seen at a present");
        }
        CHECK(env.applied.size() == 1);
        CHECK(!env.applied[0].fgActiveAtCall);
        CHECK(env.window.clientWidth == 3840 && env.window.clientHeight == 2160);
    }

    // Not armed without the true -> false change: a first resize with the flag already false, and an enter.
    {
        FakeEnv env;
        Controller c;
        MakeBorderBug(env);
        env.fg = kOff;
        env.exclusive = false;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160); // first sight: no previous value
        Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.empty());
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160); // false -> false is no leave
        Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.empty());
        CHECK(!c.FixArmed());
        env.exclusive = true;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160); // enter
        Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.empty());
    }

    // An enter disarms a fix that was still waiting.
    {
        FakeEnv env;
        Controller c;
        env.window = wt::BorderlessWindow();
        env.window.style = 0x16CF0000u; // still the fork's overlapped style: waiting
        env.window.exstyle = 0x300u;
        env.fg = kOff;
        EnterThenLeave(c, env, Mode::On);
        Present(c, env, Mode::On, kOff, kOff);
        env.exclusive = true;
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        CHECK_CONTAINS(env.Last(), "fixArmed=no");
        MakeBorderBug(env);
        Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.empty());
    }

    // Probe never changes a window: the transition is visible in the line, nothing is armed or applied.
    {
        FakeEnv env;
        Controller c;
        MakeBorderBug(env);
        env.fg = kOff;
        EnterThenLeave(c, env, Mode::Probe);
        CHECK_CONTAINS(env.Last(), "exclusive=0 prevExclusive=1");
        CHECK_CONTAINS(env.Last(), "fixArmed=no");
        for (int i = 0; i < 5; ++i)
            Present(c, env, Mode::Probe, kOff, kOff);
        CHECK(env.applied.empty());
    }

    // A different output (not DLSS-G): the gate never confirms, nothing is applied.
    {
        FakeEnv env;
        Controller c;
        MakeBorderBug(env);
        env.fg = kOff;
        env.dlssg = false;
        EnterThenLeave(c, env, Mode::On);
        for (int i = 0; i < 5; ++i)
            Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.empty());
    }

    // The window belongs to another thread: SetWindowPos is not called, one warning, fail open.
    {
        FakeEnv env;
        Controller c;
        MakeBorderBug(env);
        env.fg = kOff;
        env.owner = 5;
        env.current = 100;
        EnterThenLeave(c, env, Mode::On);
        Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.empty());
        CHECK_CONTAINS(env.All(), "owner thread differs, border fix skipped");
        CHECK(Warnings(env) == 1);
        // A second leave logs nothing more about it.
        EnterThenLeave(c, env, Mode::On);
        Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.empty());
        int mentions = 0;
        for (const auto& l : env.logs)
            mentions += l.find("owner thread differs") != std::string::npos ? 1 : 0;
        CHECK(mentions == 1);
        // Unknown owner thread (0) is the same: not called.
        env.owner = 0;
        EnterThenLeave(c, env, Mode::On);
        Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.empty());
    }

    // A window that cannot be read: skipped with a warning, not retried every present.
    {
        FakeEnv env;
        Controller c;
        env.fg = kOff;
        env.window.valid = false;
        EnterThenLeave(c, env, Mode::On);
        for (int i = 0; i < 5; ++i)
            Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.empty());
        CHECK(Warnings(env) == 1);
    }

    // The window call fails: one warning, not retried at every present.
    {
        FakeEnv env;
        Controller c;
        MakeBorderBug(env);
        env.fg = kOff;
        env.applyResult = false;
        EnterThenLeave(c, env, Mode::On);
        for (int i = 0; i < 5; ++i)
            Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.size() == 1);
        CHECK(Warnings(env) == 1);
        CHECK_CONTAINS(env.All(), "border fix FAILED");
    }

    // The window call runs the game's window procedure, which resizes the swapchain: the controller state is already
    // settled (disarmed) when that happens, the nested resize is flagged, and nothing is applied twice.
    {
        FakeEnv env;
        Controller c;
        MakeBorderBug(env);
        env.fg = kOff;
        EnterThenLeave(c, env, Mode::On);
        env.onApply = [&]
        {
            c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160); // WM_SIZE -> game -> ResizeBuffers -> OnResizeDone
        };
        Present(c, env, Mode::On, kOff, kOff);
        env.onApply = nullptr;
        CHECK(env.applied.size() == 1);
        bool nested = false;
        for (const auto& l : env.logs)
            nested = nested || l.find("nested=yes") != std::string::npos;
        CHECK(nested);
        Present(c, env, Mode::On, kOff, kOff);
        CHECK(env.applied.size() == 1);
        // Outside a window call of the module the flag is not set.
        c.OnResizeDone(env, Mode::On, kBridge, kHwnd, 3840, 2160);
        CHECK(env.Last().find("nested=yes") == std::string::npos);
    }

    // Mode off: no arming, no application, no reads.
    {
        FakeEnv env;
        Controller c;
        MakeBorderBug(env);
        EnterThenLeave(c, env, Mode::Off);
        Present(c, env, Mode::Off, kOff, kOff);
        CHECK(env.applied.empty());
        CHECK(env.logs.empty());
    }

    return wt::g_failed ? 1 : 0;
}
