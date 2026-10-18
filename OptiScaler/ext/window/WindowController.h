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
    virtual void Log(const std::string& line, bool warn) = 0;
};

class Controller
{
  public:
    // Bridge swapchain resize finished (ext::dx11bridge::OnResizeDone). `bridge` identifies the bridge swapchain.
    void OnResizeDone(Env& env, Mode mode, uint64_t bridge, uint64_t hwnd)
    {
        if (mode == Mode::Off)
            return;
        Announce(env, mode);

        _bridge = bridge;
        _hwnd = hwnd;

        const bool exclusive = env.Exclusive();
        const int prev = _haveExclusive ? (_lastExclusive ? 1 : 0) : -1;
        _haveExclusive = true;
        _lastExclusive = exclusive;

        ProbeFields f = Fill(env, mode, "resize-done", hwnd);
        f.exclusive = exclusive;
        f.prevExclusive = prev;
        env.Log(FormatProbeLine(f), false);
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
        _gate.OnPresentEnd(env.Fg());
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
        return f;
    }

    bool _announced = false;
    uint64_t _bridge = 0;
    uint64_t _hwnd = 0;
    bool _haveExclusive = false;
    bool _lastExclusive = false;
    uint32_t _presentThread = 0;
    FgOffGate _gate;
};
} // namespace ext::window
