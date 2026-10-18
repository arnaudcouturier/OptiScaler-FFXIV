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
#include "ext/window/WindowDeferral.h"
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

    // The existing feature API, exactly what upstream's hook does first: fgChanged = true, UpdateTarget(), Deactivate().
    virtual void RequestFgOff() = 0;
    virtual uint64_t NowMs() = 0;
    // IDXGISwapChain::SetFullscreenState(fullscreen, nullptr) on `native` (the swapchain ReShade passed, NOT through ReShade's
    // wrapper). Returns the HRESULT.
    virtual int32_t Replay(uint64_t native, bool fullscreen) = 0;

    virtual void Log(const std::string& line, bool warn) = 0;
};

class Controller
{
  public:
    // How many checks (presents with frame generation confirmed off) an armed border fix waits for the game to show a
    // frame-less window with an edge before it gives up and leaves the window as it is.
    static constexpr uint32_t kFixMaxChecks = 120;
    // ... and it expires this many presents or ms after it was armed, whichever comes first (frame generation can stay on for
    // hours, so gated checks alone never end it). Evaluated at presents only.
    static constexpr uint32_t kFixMaxPresents = 600;
    static constexpr uint64_t kFixMaxMs = 30000;
    // The replay needs this many COMPLETE presents with frame generation off, counted from the request, and at least this many
    // ms since the request: Streamline applies eOff inside a present and its pacer is a separate worker, so one present is too
    // short a proof.
    static constexpr uint32_t kReplayConfirmPresents = 2;
    static constexpr uint64_t kReplayConfirmMs = 50;

    // Tests may relax the replay confirmation to focus on other rules; production keeps the defaults.
    void SetReplayConfirm(uint32_t presents, uint64_t ms)
    {
        _confirmPresents = presents;
        _confirmMs = ms;
    }

    // Bridge swapchain resize finished (ext::dx11bridge::OnResizeDone). `bridge` identifies the bridge swapchain.
    // `swapWidth/swapHeight` = the bridge's FG swapchain size after this resize.
    void OnResizeDone(Env& env, Mode mode, uint64_t bridge, uint64_t hwnd, uint32_t swapWidth, uint32_t swapHeight)
    {
        if (mode == Mode::Off)
            return;
        Announce(env, mode);

        // A held call belongs to the swapchain it was made on: another bridge (device recreation) drops it, unreplayed.
        if (_bridge != 0 && bridge != _bridge)
            DropHold(env, "the bridge swapchain changed");

        _bridge = bridge;
        _hwnd = hwnd;
        _swapW = swapWidth;
        _swapH = swapHeight;

        const bool nested = _actionDepth > 0;
        const int prev = SampleExclusive(env, mode, "bridge resize");

        ProbeFields f = Fill(env, mode, "resize-done", hwnd);
        f.exclusive = _lastExclusive;
        f.prevExclusive = prev;
        f.nested = nested;
        env.Log(FormatProbeLine(f), false);

        // A bridge resize deactivates frame generation (Streamline applies "off" inside the next present), so what the gate
        // counted before it says nothing about the new state.
        _gate.Reset();
    }

    // A bridge swapchain is being destroyed (ext::dx11bridge::OnDestroy): a call held for it is dropped, unreplayed, and the
    // module forgets it, so nothing is ever replayed on a dead pointer.
    void OnBridgeDestroyed(Env& env, Mode mode, uint64_t bridge)
    {
        if (mode == Mode::Off || bridge == 0 || bridge != _bridge)
            return;
        DropHold(env, "the bridge swapchain is being destroyed");
        _bridge = 0;
        _hwnd = 0;
        _swapW = 0;
        _swapH = 0;
        _fixArmed = false;
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

        // The flag changes inside upstream's SetFullscreenState hook, and a game does not necessarily resize its swapchain
        // after the switch (the logged startup enter has no resize behind it), so a present boundary is a sampling point too.
        SampleExclusive(env, mode, "present");

        if (mode != Mode::On)
            return;

        // B2-a first: a held call is replayed at the end of the present that confirms frame generation off. The border fix
        // does not run in the same present (the replay's restyle may be followed by the game's own style write).
        if (_deferral.State() == DeferState::Pending)
            StepDeferral(env, fg);
        else if (_fixArmed && _deferral.State() == DeferState::Idle) // not inside the replay's own window call
        {
            // The armed fix also expires by presents and by wall time since arming (checked at presents): FG can stay on for
            // hours, and a stale fix would then restyle the window at an arbitrary later off period.
            const uint64_t now = env.NowMs();
            if (++_fixPresents > kFixMaxPresents || (now >= _fixArmedMs && now - _fixArmedMs > kFixMaxMs))
            {
                _fixArmed = false;
                env.Log(std::format("Window: border fix expired: armed {} presents / {} ms ago without a chance to apply",
                                    _fixPresents, now >= _fixArmedMs ? now - _fixArmedMs : 0),
                        false);
                return;
            }
            TryFix(env, fg);
        }
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
        // The game's own follow-up ResizeBuffers is only a same-size no-op (Dx11wDx12SC skips the FG swapchain resize, so no
        // Streamline flush) when the FG swapchain already has the window's size. The hold cannot cover a size-changing one.
        const bool sizeMatches = _swapW != 0 && f.window.valid && f.window.right - f.window.left == static_cast<int32_t>(_swapW) &&
                                 f.window.bottom - f.window.top == static_cast<int32_t>(_swapH);
        in.sizeMatchesWindow = true;
        const bool wouldHold = DecideDefer(in) == DeferVerdict::Defer;
        in.sizeMatchesWindow = sizeMatches;

        f.hasCall = true;
        f.callFullscreen = ev.fullscreen;
        f.callTarget = ev.target;
        f.nativeSwapchain = ev.native;
        f.bridgeSwapchain = _bridge;
        f.verdict = DecideDefer(in);
        if (wouldHold && f.verdict == DeferVerdict::SizeMismatch && _deferral.State() == DeferState::Idle)
            env.Log(std::format("Window: the follow-up ResizeBuffers is not protected: the FG swapchain is {}x{} but the window rect is {}x{}; "
                                "the call passes through, nothing is held",
                                _swapW, _swapH, f.window.right - f.window.left, f.window.bottom - f.window.top),
                    true);

        const DeferState before = _deferral.State();
        f.deferState = DeferStateName(before);

        bool prevent = false;
        const char* action = nullptr;
        if (mode == Mode::On)
        {
            switch (before)
            {
            case DeferState::Replaying:
                break; // the module's own replay: through, untouched
            case DeferState::Pending:
                if (ev.native != _pendingNative)
                {
                    action = "another swapchain: passes";
                }
                else if (!WouldChangeWindow(ev.fullscreen, f.exclusive))
                {
                    // The latest request asks for the state that is in effect: the held call is moot, a replay would only
                    // cost another frame generation outage. Drop it; this call passes (a no-op for the window).
                    _deferral.Cancel();
                    _pendingNative = 0;
                    action = "latest request equals the state in effect: held call dropped, this call passes";
                }
                else if (!in.threadsAgree)
                {
                    action = "owner, calling and presenting threads differ: passes";
                }
                else
                {
                    _deferral.Replace(ev.fullscreen);
                    prevent = true;
                    action = "replaces the held call (only the latest is kept)";
                }
                break;
            case DeferState::Idle:
                if (f.verdict == DeferVerdict::Defer)
                {
                    // State is settled first, then the calls out.
                    _pendingNative = ev.native;
                    _pendingThread = f.currentThread;
                    _deferral.Begin(ev.fullscreen, env.NowMs());
                    env.RequestFgOff();
                    _requestMs = env.NowMs();
                    _gate.Reset();
                    prevent = true;
                    action = "held back: frame generation asked off, replayed once it is confirmed off";
                }
                break;
            }
        }

        f.wouldReturn = before == DeferState::Idle ? f.verdict == DeferVerdict::Defer : prevent;
        f.returned = prevent;
        env.Log(FormatProbeLine(f), false);
        if (action != nullptr)
            env.Log(std::format("Window: SetFullscreenState({}) {}; handler returns {}", ev.fullscreen ? "TRUE" : "FALSE",
                                action, prevent ? "true (prevent)" : "false"),
                    false);
        return prevent;
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

    // Reads SCExclusiveFullscreen and keeps the last value seen at ANY sampling point (bridge resize or present end). A true ->
    // false change arms the border fix wherever it is first seen; an enter disarms it. Returns the previous value (-1 = none).
    int SampleExclusive(Env& env, Mode mode, const char* where)
    {
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
                _fixPresents = 0;
                _fixArmedMs = env.NowMs();
                _fixWaitNoted = false;
                env.Log(std::format("Window: border fix armed (SCExclusiveFullscreen true -> false, seen at a {}); it runs after "
                                    "a complete present with frame generation off",
                                    where),
                        false);
            }
        }
        return prev;
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
        f.swapWidth = _swapW;
        f.swapHeight = _swapH;
        f.fixArmed = _fixArmed;
        f.nested = _actionDepth > 0;
        return f;
    }

    // While alive: the module is inside its own replay (depth > 0). On every exit, a throwing log included, Replaying ends.
    struct ReplayGuard
    {
        ReplayGuard(Deferral& d, int& depth) : _d(d), _depth(depth) { ++_depth; }
        ~ReplayGuard()
        {
            --_depth;
            _d.EndReplay();
        }
        ReplayGuard(const ReplayGuard&) = delete;
        ReplayGuard& operator=(const ReplayGuard&) = delete;
        Deferral& _d;
        int& _depth;
    };

    // Drops a held call without replaying it (nothing else to release: no reference is taken on the swapchain).
    void DropHold(Env& env, const char* why, bool warn = false)
    {
        if (_deferral.State() != DeferState::Pending)
            return;
        _deferral.Cancel();
        _pendingNative = 0;
        _reRequestNoted = false;
        env.Log(std::format("Window: held SetFullscreenState dropped, not replayed: {}", why), warn);
    }

    // DLSS-G confirmed off AND enough complete off presents AND enough time since the request (M3).
    bool ReplayConfirmed(Env& env)
    {
        const uint64_t now = env.NowMs();
        return _gate.Confirmed(env.DlssgOutput()) && _gate.Count() >= _confirmPresents && now >= _requestMs &&
               now - _requestMs >= _confirmMs;
    }

    // B2-a. Runs at the end of every game present while a call is held. The replay never runs while frame generation is
    // active: the pause after a Deactivate is only about ten frames, so the feature can come back during the wait; each
    // present that finds it active asks it off again. Only the hard limit replays regardless (fail open).
    void StepDeferral(Env& env, const FgSnapshot& fg)
    {
        if (fg.present && fg.active)
        {
            env.RequestFgOff();
            _requestMs = env.NowMs();
            _gate.Reset();
            if (!_reRequestNoted)
            {
                _reRequestNoted = true;
                env.Log("Window: frame generation came back while a call is held, asked off again", false);
            }
        }

        const bool maybeOn = !fg.present || fg.active;
        // A held call is replayed on the thread that made it; another presenting thread leaves it for the hard limit.
        const bool rightThread = env.CurrentThread() == _pendingThread;
        const WaitResult result =
            _deferral.OnPresentEnd(maybeOn, ReplayConfirmed(env), env.NowMs(), rightThread);
        if (result == WaitResult::Wait)
            return;

        // At the hard limit the replay is the fail-open path, but never on a thread that is not the window's owner thread: a
        // window call from another thread is a cross-thread SendMessage. Drop the hold with a warning instead.
        if (result == WaitResult::ReplayAtHardLimit)
        {
            const uint32_t owner = env.OwnerThread(_hwnd);
            if (owner == 0 || owner != env.CurrentThread())
            {
                DropHold(env, "hard wait limit reached on a thread that does not own the window", true);
                return;
            }
        }

        // Replay only on the live bridge swapchain the call was made on, never on a stale pointer.
        if (_pendingNative == 0 || _pendingNative != _bridge)
        {
            DropHold(env, "the held swapchain is not the live bridge");
            return;
        }

        const bool fullscreen = _deferral.PendingFullscreen();
        const uint64_t native = _pendingNative;
        const uint32_t presents = _deferral.Presents();
        const uint64_t waited = _deferral.ElapsedMs(env.NowMs());
        const uint32_t replaced = _deferral.Replaced();

        _deferral.BeginReplay(); // settled before the call out: the replay passes through the handler and may resize
        _pendingNative = 0;
        _reRequestNoted = false;

        ReplayGuard logGuard(_deferral, _actionDepth); // a throwing log must not leave Replaying stuck
        int32_t hr = 0;
        env.Log(std::format("Window: replaying SetFullscreenState({}) with a null target: {} (after {} presents, {} ms, {} "
                            "earlier call(s) replaced)",
                            fullscreen ? "TRUE" : "FALSE", WaitResultName(result), presents, waited, replaced),
                result == WaitResult::ReplayAtHardLimit);
        hr = env.Replay(native, fullscreen);
        env.Log(std::format("Window: replayed SetFullscreenState({}) returned {:#010x}", fullscreen ? "TRUE" : "FALSE",
                            static_cast<uint32_t>(hr)),
                hr < 0);
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
    uint32_t _swapW = 0; // size of the FG swapchain at the last resize
    uint32_t _swapH = 0;
    uint64_t _bridge = 0;
    uint64_t _hwnd = 0;
    bool _haveExclusive = false;
    bool _lastExclusive = false;
    uint32_t _presentThread = 0;
    FgOffGate _gate;

    // B1-a
    bool _fixArmed = false;
    uint32_t _fixChecks = 0;
    uint32_t _fixPresents = 0;
    uint64_t _fixArmedMs = 0;
    bool _fixWaitNoted = false;
    bool _ownerSkipLogged = false;
    int _actionDepth = 0; // > 0 while the module is inside one of its own window calls

    // B2-a
    Deferral _deferral;
    uint64_t _requestMs = 0;                            // when frame generation was last asked off
    uint32_t _confirmPresents = kReplayConfirmPresents; // complete off presents needed before a replay
    uint64_t _confirmMs = kReplayConfirmMs;            // and this long since the request
    uint64_t _pendingNative = 0;
    uint32_t _pendingThread = 0;
    bool _reRequestNoted = false;
};
} // namespace ext::window
