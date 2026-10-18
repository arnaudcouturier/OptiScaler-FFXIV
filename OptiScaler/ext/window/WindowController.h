#pragma once
// The decision core of the window-mode module (docs/FEATURES.md, "Window mode"). Everything the module touches in the
// outside world goes through Env, so the tests drive the same code with a fake window, fake frame generation, a fake clock
// and a fake swapchain. The live Env is in WindowModule.cpp.
//
// Threading: the controller itself has no lock. The live module serialises calls with one recursive mutex, because a window
// call made by the module can run the game's window procedure, which can call back into the bridge on the same thread. The
// controller is written for that: its state is updated BEFORE every call out.

#include "ext/window/WindowMode.h"
#include "ext/window/WindowProbe.h"
#include "ext/window/WindowRules.h"

#include <cstdint>
#include <format>
#include <string>

namespace ext::window
{
// A ReShade set_fullscreen_state event (api::swapchain*, bool fullscreen, void* hmonitor), reduced to plain values.
struct FsEvent
{
    uint64_t native = 0; // swapchain->get_native()
    uint64_t hwnd = 0;   // swapchain->get_hwnd()
    bool fullscreen = false;
    uint64_t target = 0; // hmonitor, raw value; never dereferenced
};

class Env
{
  public:
    virtual ~Env() = default;

    virtual bool ForceBorderless() = 0;
    virtual bool DlssgOutput() = 0;
    virtual bool Exclusive() = 0; // State::SCExclusiveFullscreen
    virtual FgSnapshot Fg() = 0;
    virtual uint32_t CurrentThread() = 0;
    virtual uint32_t OwnerThread(uint64_t hwnd) = 0; // 0 = unknown
    virtual WindowInfo ReadWindow(uint64_t hwnd) = 0;
    // SetWindowLongPtr(GWL_EXSTYLE) + SetWindowPos(NOMOVE | NOSIZE | NOZORDER | NOACTIVATE | FRAMECHANGED). Only ever called
    // on the window's owner thread. The window procedure runs inside it.
    virtual bool ApplyExStyle(uint64_t hwnd, uint32_t exstyle) = 0;
    virtual void Log(const std::string& line, bool warn) = 0;
};

class Controller
{
  public:
    // How many checks (presents with frame generation confirmed off) an armed border fix waits for the game to show a
    // frame-less window with an edge before it gives up and leaves the window as it is.
    static constexpr uint32_t kFixMaxChecks = 120;

    // Bridge swapchain resize finished (ext::dx11bridge::OnResizeDone). `bridge` identifies the bridge swapchain.
    void OnResizeDone(Env& env, Mode mode, uint64_t bridge, uint64_t hwnd)
    {
        if (mode == Mode::Off)
            return;
        Announce(env, mode);

        _bridge = bridge;
        _hwnd = hwnd;

        const bool nested = _actionDepth > 0;
        const bool exclusive = env.Exclusive();
        const int prev = _haveExclusive ? (_lastExclusive ? 1 : 0) : -1;
        const bool left = prev == 1 && !exclusive;
        _haveExclusive = true;
        _lastExclusive = exclusive;

        // The border fix follows the pseudo fullscreen: armed by the leave, dropped by an enter.
        if (mode == Mode::On)
        {
            if (exclusive)
            {
                _fixArmed = false;
            }
            else if (left)
            {
                _fixArmed = true;
                _fixChecks = 0;
                _fixWaitNoted = false;
                env.Log("Window: border fix armed (SCExclusiveFullscreen true -> false); it runs after a complete present with "
                        "frame generation off",
                        false);
            }
        }

        ProbeFields f = Fill(env, mode, "resize-done", hwnd);
        f.exclusive = exclusive;
        f.prevExclusive = prev;
        f.nested = nested;
        env.Log(FormatProbeLine(f), false);

        // A bridge resize deactivates frame generation (Streamline applies "off" inside the next present), so what the gate
        // counted before it says nothing about the new state.
        _gate.Reset();
    }

    // Entry and exit of the game's Present on the bridge swapchain (ext::dx11bridge::PresentScope).
    void OnPresentBegin(Env& env, Mode mode)
    {
        if (mode == Mode::Off)
            return;
        Announce(env, mode);
        _presentThread = env.CurrentThread();
        _gate.OnPresentBegin(env.Fg());
    }

    void OnPresentEnd(Env& env, Mode mode)
    {
        if (mode == Mode::Off)
            return;
        const FgSnapshot fg = env.Fg();
        _gate.OnPresentEnd(fg);

        if (mode == Mode::On && _fixArmed)
            TryFix(env, fg);
    }

    // ReShade set_fullscreen_state, before the call. True = prevent the call.
    bool OnSetFullscreenState(Env& env, Mode mode, const FsEvent& ev)
    {
        if (mode == Mode::Off)
            return false;
        Announce(env, mode);

        const uint64_t hwnd = _hwnd != 0 ? _hwnd : ev.hwnd;
        ProbeFields f = Fill(env, mode, "set-fullscreen-state", hwnd);

        DeferInputs in;
        in.forceBorderless = f.forceBorderless;
        in.dlssgOutput = f.dlssgOutput;
        in.fgActive = f.fg.present && f.fg.active;
        in.exclusiveNow = f.exclusive;
        in.requestedFullscreen = ev.fullscreen;
        in.threadsAgree = ThreadsAgree(f.ownerThread, f.currentThread, f.presentThread);
        in.swapchainIsBridge = _bridge != 0 && ev.native == _bridge;

        f.hasCall = true;
        f.callFullscreen = ev.fullscreen;
        f.callTarget = ev.target;
        f.nativeSwapchain = ev.native;
        f.bridgeSwapchain = _bridge;
        f.verdict = DecideDefer(in);
        f.wouldReturn = f.verdict == DeferVerdict::Defer;
        f.returned = false; // step 3 turns this on
        env.Log(FormatProbeLine(f), false);
        return false;
    }

    // Test access.
    uint32_t PresentThread() const { return _presentThread; }
    const FgOffGate& Gate() const { return _gate; }
    bool FixArmed() const { return _fixArmed; }

  private:
    void Announce(Env& env, Mode mode)
    {
        if (_announced)
            return;
        _announced = true;
        env.Log(std::format("Window: module active, mode={} (one line per bridge resize and per set_fullscreen_state call)",
                            ModeName(mode)),
                false);
    }

    ProbeFields Fill(Env& env, Mode mode, std::string_view event, uint64_t hwnd)
    {
        ProbeFields f;
        f.event = event;
        f.mode = mode;
        f.hwnd = hwnd;
        f.window = env.ReadWindow(hwnd);
        f.fg = env.Fg();
        f.dlssgOutput = env.DlssgOutput();
        f.fgOffConfirmed = _gate.Confirmed(f.dlssgOutput);
        f.exclusive = env.Exclusive();
        f.prevExclusive = _haveExclusive ? (_lastExclusive ? 1 : 0) : -1;
        f.forceBorderless = env.ForceBorderless();
        f.ownerThread = env.OwnerThread(hwnd);
        f.currentThread = env.CurrentThread();
        f.presentThread = _presentThread;
        f.fixArmed = _fixArmed;
        f.nested = _actionDepth > 0;
        return f;
    }

    // B1-a. Runs at the end of a game present while the fix is armed. Frame generation must be confirmed off (a complete
    // present with the feature inactive, DLSS-G output) AND be off right now: the pause after a Deactivate is only about ten
    // frames, so the feature can come back; then the fix waits for the next off period.
    void TryFix(Env& env, const FgSnapshot& fg)
    {
        if (!_gate.Confirmed(env.DlssgOutput()) || !FgIsOff(fg))
            return;

        if (++_fixChecks > kFixMaxChecks)
        {
            _fixArmed = false;
            env.Log(std::format("Window: border fix gave up: no frame-less style with an edge seen in {} checks, the window is "
                                "left as it is",
                                kFixMaxChecks),
                    false);
            return;
        }

        const uint32_t owner = env.OwnerThread(_hwnd);
        const uint32_t current = env.CurrentThread();
        if (owner == 0 || owner != current)
        {
            _fixArmed = false;
            if (!_ownerSkipLogged)
            {
                _ownerSkipLogged = true;
                env.Log(std::format("Window: owner thread differs, border fix skipped (window thread {}, present thread {})",
                                    owner, current),
                        true);
            }
            return;
        }

        const WindowInfo before = env.ReadWindow(_hwnd);
        if (!before.valid)
        {
            _fixArmed = false;
            env.Log("Window: border fix skipped: the window cannot be read", true);
            return;
        }

        const auto fixed = ClearEdge(before.style, before.exstyle);
        if (!fixed)
        {
            // A caption is still there (the fork's overlapped style, before the game wrote its own) or there is no edge. Keep
            // checking: the game's style write may come later.
            if (!_fixWaitNoted)
            {
                _fixWaitNoted = true;
                env.Log(std::format("Window: border fix waiting: nothing to clear yet (style={:#010x} exstyle={:#010x})",
                                    before.style, before.exstyle),
                        false);
            }
            return;
        }

        _fixArmed = false; // settled before the call out: the call runs the game's window procedure
        ++_actionDepth;
        const bool ok = env.ApplyExStyle(_hwnd, *fixed);
        --_actionDepth;

        const WindowInfo after = env.ReadWindow(_hwnd);
        env.Log(std::format("Window: border fix {} exstyle {:#010x} -> {:#010x} client {}x{} -> {}x{}",
                            ok ? "applied" : "FAILED", before.exstyle, *fixed, before.clientWidth, before.clientHeight,
                            after.clientWidth, after.clientHeight),
                !ok);
    }

    bool _announced = false;
    uint64_t _bridge = 0;
    uint64_t _hwnd = 0;
    bool _haveExclusive = false;
    bool _lastExclusive = false;
    uint32_t _presentThread = 0;
    FgOffGate _gate;

    // B1-a
    bool _fixArmed = false;
    uint32_t _fixChecks = 0;
    bool _fixWaitNoted = false;
    bool _ownerSkipLogged = false;
    int _actionDepth = 0; // > 0 while the module is inside one of its own window calls
};
} // namespace ext::window
