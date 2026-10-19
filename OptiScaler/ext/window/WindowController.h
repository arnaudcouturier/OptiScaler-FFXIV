#pragma once
// The decision core of the window-mode module (docs/FEATURES.md, "Window mode"). Everything the module touches in the
// outside world goes through Env, so the tests drive the same code with a fake window, fake frame generation, a fake clock
// and a fake swapchain. The live Env is in WindowModule.cpp.
//
// Threading: the controller itself has no lock. The live module serialises calls with one recursive mutex, because a window
// call made by the module can run the game's window procedure, which can call back into the bridge on the same thread. The
// controller is written for that: its state is updated BEFORE every call out (the request bookkeeping is also cleared again
// after the call, see AskFgOff).

#include "ext/window/WindowMode.h"
#include "ext/window/WindowProbe.h"
#include "ext/window/WindowDeferral.h"
#include "ext/window/WindowRules.h"

#include <cstdint>
#include <exception>
#include <format>
#include <optional>
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
    virtual uint64_t MonitorOfWindow(uint64_t hwnd) = 0; // HMONITOR of the window, 0 = unknown
    // The bridge's inner frame generation swapchain as the same kind of pointer ReShade passes (IDXGISwapChain*), 0 = unknown.
    // Read-only; it only goes into the probe line, so a session can tell which swapchain ReShade passed.
    virtual uint64_t FgSwapChain() = 0;
    // The bridge's present counter (PresentSerialFromIndex): changes once per game present that reached the frame generation
    // present. nullopt = no counter (no bridge known, or fewer than two buffers).
    virtual std::optional<uint64_t> PresentSerial() = 0;
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
    // The same for the border fix (M3): it needs this many complete off presents, and this long since the first of them.
    void SetFixConfirm(uint32_t presents, uint64_t ms)
    {
        _fixConfirmPresents = presents;
        _fixConfirmMs = ms;
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

        const bool nested = Nested(env);
        const int prev = SampleExclusive(env, mode, "bridge resize");

        ProbeFields f = Fill(env, mode, "resize-done", hwnd);
        f.exclusive = _lastExclusive;
        f.prevExclusive = prev;
        f.nested = nested;
        env.Log(FormatProbeLine(f), false);

        // A bridge resize deactivates frame generation (Streamline applies "off" inside the next present), so what the gate
        // counted before it says nothing about the new state, and no present has reached Streamline since this request.
        _gate.Reset();
        _fgPresentSinceRequest = false;
        // ... and the floors count from this new deactivation, as after a re-request (R3-L1).
        if (_deferral.State() == DeferState::Pending)
            _requestMs = env.NowMs();
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
        _serialBridge = _bridge;
        _serialBegin = env.PresentSerial();
        _gate.OnPresentBegin(env.Fg());
    }

    void OnPresentEnd(Env& env, Mode mode)
    {
        if (mode == Mode::Off)
            return;
        const FgSnapshot fg = env.Fg();
        // A present that returned before the frame generation present says nothing about Streamline (M2): not counted.
        const bool reached = PresentReached(env);
        _gate.OnPresentEnd(fg, reached, env.NowMs());
        if (reached)
            _fgPresentSinceRequest = true; // the soft wait limit needs one such present since the last request

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
        in.targetOk = ev.target == 0 || ev.target == env.MonitorOfWindow(hwnd);
        in.sizeMatchesWindow = true;
        const bool wouldHold = DecideDefer(in) == DeferVerdict::Defer;
        in.sizeMatchesWindow = sizeMatches;

        f.hasCall = true;
        f.callFullscreen = ev.fullscreen;
        f.callTarget = ev.target;
        f.nativeSwapchain = ev.native;
        f.bridgeSwapchain = _bridge;
        f.fgSwapchain = env.FgSwapChain();
        f.verdict = DecideDefer(in);
        if (wouldHold && f.verdict == DeferVerdict::SizeMismatch && _deferral.State() == DeferState::Idle)
            env.Log(std::format("Window: the follow-up ResizeBuffers is not protected: the FG swapchain is {}x{} but the window rect is {}x{}; "
                                "the call passes through, nothing is held",
                                _swapW, _swapH, f.window.right - f.window.left, f.window.bottom - f.window.top),
                    true);

        const DeferState before = _deferral.State();
        f.deferState = DeferStateName(before);

        // L1: decide first, build and write both log lines, and only then change any state. A log that throws (an allocation, a
        // formatter, the logger) must leave nothing held: the entry point then returns false and the game's call passes, and a
        // hold left behind would replay the same call a second time.
        enum class Commit : uint8_t
        {
            None,
            DropMoot, // the latest request equals the state in effect: the held call is dropped
            Replace,  // the held call is replaced by this one
            Hold,     // this call is held, frame generation is asked off
        };
        bool prevent = false;
        const char* action = nullptr;
        Commit commit = Commit::None;
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
                    commit = Commit::DropMoot;
                    action = "latest request equals the state in effect: held call dropped, this call passes";
                }
                else if (!in.threadsAgree)
                {
                    action = "owner, calling and presenting threads differ: passes";
                }
                else
                {
                    commit = Commit::Replace;
                    prevent = true;
                    action = "replaces the held call (only the latest is kept)";
                }
                break;
            case DeferState::Idle:
                if (f.verdict == DeferVerdict::Defer)
                {
                    commit = Commit::Hold;
                    prevent = true;
                    action = "held back: frame generation asked off, replayed once it is confirmed off";
                }
                break;
            }
        }

        f.wouldReturn = before == DeferState::Idle ? f.verdict == DeferVerdict::Defer : prevent;
        f.returned = prevent;
        env.Log(FormatProbeLine(f), false);
        // A hold says what it intends here; the line that says "handler returns true" is written once the hold stands (R3-L4): this
        // line comes before the frame generation request, which can fail.
        if (action != nullptr)
        {
            if (commit == Commit::Hold)
                env.Log(std::format("Window: SetFullscreenState({}) {}", ev.fullscreen ? "TRUE" : "FALSE", action), false);
            else
                env.Log(std::format("Window: SetFullscreenState({}) {}; handler returns {}", ev.fullscreen ? "TRUE" : "FALSE",
                                    action, prevent ? "true (prevent)" : "false"),
                        false);
        }

        // Nothing below logs. State is settled first, then the call out; if the call out throws, the hold is cancelled.
        switch (commit)
        {
        case Commit::None:
            break;
        case Commit::DropMoot:
            _deferral.Cancel();
            _pendingNative = 0;
            _reRequestNoted = false;
            break;
        case Commit::Replace:
            _deferral.Replace(ev.fullscreen);
            break;
        case Commit::Hold:
        {
            const uint64_t now = env.NowMs();
            _pendingNative = ev.native;
            _pendingThread = f.currentThread;
            _deferral.Begin(ev.fullscreen, now);
            try
            {
                AskFgOff(env);
            }
            catch (const std::exception& e)
            {
                CancelFailedHold(env, ev.fullscreen, e.what());
                throw;
            }
            catch (...)
            {
                CancelFailedHold(env, ev.fullscreen, "unknown error");
                throw;
            }
            // The hold stands: say so. A logger that throws here must not change the answer (the held call must not also pass).
            try
            {
                env.Log(std::format("Window: SetFullscreenState({}) held back, frame generation asked off; handler returns true "
                                    "(prevent)",
                                    ev.fullscreen ? "TRUE" : "FALSE"),
                        false);
            }
            catch (...)
            {
            }
            break;
        }
        }
        return prevent;
    }

    // Test access.
    uint32_t AbortedPresents() const { return _abortedPresents; }
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

    // M2. Did the present that just ended reach the frame generation present? Judged by the bridge's present counter, read at
    // the start and at the end of the present (it advances only after the FG present returned success, with frame generation on
    // or off). No counter at either end, or a different bridge by the end: unknown, and an unknown present counts as it always
    // did. The one-time notes make the live path visible in the log.
    bool PresentReached(Env& env)
    {
        const std::optional<uint64_t> begin = _serialBegin;
        _serialBegin.reset(); // an end with no begin since is unknown
        const bool sameBridge = _serialBridge != 0 && _serialBridge == _bridge;
        const std::optional<uint64_t> end = sameBridge ? env.PresentSerial() : std::nullopt;

        if (!begin || !end)
        {
            if (sameBridge && !_noCounterNoted)
            {
                _noCounterNoted = true;
                env.Log("Window: the bridge present counter is not available (fewer than two buffers): every game present is "
                        "counted, a present that failed before the frame generation present is not told apart",
                        false);
            }
            return true;
        }

        if (PresentReachedFg(begin, end))
        {
            if (!_counterLiveNoted)
            {
                _counterLiveNoted = true;
                env.Log("Window: the bridge present counter is live: it changes once per game present that reaches the frame "
                        "generation present, with frame generation on or off",
                        false);
            }
            return true;
        }

        ++_abortedPresents;
        if (_abortLogged < kAbortLogLines)
        {
            ++_abortLogged;
            env.Log(std::format("Window: a game present ended before the frame generation present (bridge present counter "
                                "unchanged at {}): not counted, the frame-generation-off confirmation starts again "
                                "({} so far)",
                                *end, _abortedPresents),
                    false);
        }
        return false;
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
        f.nested = Nested(env);
        return f;
    }

    // While alive: the module is inside one of its own window calls, on the thread that made it (L3). On every exit, a throwing
    // call or log included, the depth and the thread go back.
    struct ActionScope
    {
        ActionScope(int& depth, uint32_t& thread, uint32_t current) : _depth(depth), _thread(thread), _savedThread(thread)
        {
            ++_depth;
            _thread = current;
        }
        ~ActionScope()
        {
            --_depth;
            _thread = _savedThread;
        }
        ActionScope(const ActionScope&) = delete;
        ActionScope& operator=(const ActionScope&) = delete;
        int& _depth;
        uint32_t& _thread;
        uint32_t _savedThread;
    };

    // The same, plus Replaying ends on every exit.
    struct ReplayGuard
    {
        ReplayGuard(Deferral& d, int& depth, uint32_t& thread, uint32_t current) : _d(d), _scope(depth, thread, current) {}
        ~ReplayGuard() { _d.EndReplay(); }
        ReplayGuard(const ReplayGuard&) = delete;
        ReplayGuard& operator=(const ReplayGuard&) = delete;
        Deferral& _d;
        ActionScope _scope;
    };

    // True only on the thread that is inside a window call of this module: with the module lock given up around the calls
    // out, another thread's resize that arrives meanwhile is not nested.
    bool Nested(Env& env) const { return _actionDepth > 0 && _actionThread == env.CurrentThread(); }

    // The frame generation request failed while a call was being held: the hold is cancelled, the call passes (the entry point
    // returns false once the exception has gone through), and the log says so (R3-L4). The line is written under its own guard, so
    // that a failing logger cannot replace the original exception.
    void CancelFailedHold(Env& env, bool fullscreen, const char* what)
    {
        _deferral.Cancel();
        _pendingNative = 0;
        _reRequestNoted = false;
        try
        {
            env.Log(std::format("Window: SetFullscreenState({}) hold cancelled, the frame generation request failed ({}); handler "
                                "returns false (the call passes; a returned=true above is void)",
                                fullscreen ? "TRUE" : "FALSE", what),
                    true);
        }
        catch (...)
        {
        }
    }

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

    // Asks frame generation off (the hold, and every re-request while a call is held). The request time, the gate and the
    // "a present reached Streamline since the request" flag are settled BEFORE the call out (the live call gives up the module
    // lock, so a present on another thread may end inside it), and cleared again after it: whatever raced the call is not
    // ordered against the request and counts for nothing.
    void AskFgOff(Env& env)
    {
        _requestMs = env.NowMs();
        _gate.Reset();
        _fgPresentSinceRequest = false;
        env.RequestFgOff();
        _gate.Reset();
        _fgPresentSinceRequest = false;
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
            AskFgOff(env);
            if (!_reRequestNoted)
            {
                _reRequestNoted = true;
                env.Log("Window: frame generation came back while a call is held, asked off again", false);
            }
        }

        const bool maybeOn = !fg.present || fg.active;
        // A held call is replayed on the thread that made it; another presenting thread leaves it for the hard limit.
        const bool rightThread = env.CurrentThread() == _pendingThread;
        // The soft limit replays only if a game present has reached the frame generation present since the last request AND the
        // same floor as the confirmed replay is over (measured from that request): otherwise a high frame rate, or a frame
        // generation that keeps coming back, would shorten the wait to a few presents, and a run of failed presents would end it
        // with nothing processed by Streamline. The hard limit does not need it (fail open).
        const uint64_t now = env.NowMs();
        const bool softAllowed = _fgPresentSinceRequest && now >= _requestMs && now - _requestMs >= _confirmMs;
        const WaitResult result = _deferral.OnPresentEnd(maybeOn, ReplayConfirmed(env), now, rightThread, softAllowed);
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

        ReplayGuard logGuard(_deferral, _actionDepth, _actionThread, env.CurrentThread()); // a throwing log must not leave Replaying stuck
        // R3-L3: the call first, both lines after it. A line that throws (the formatter, the logger) must not lose the held call:
        // the machine is already Replaying, the guard ends it, and the call has been made.
        const int32_t hr = env.Replay(native, fullscreen);
        env.Log(std::format("Window: replaying SetFullscreenState({}) with a null target: {} (after {} presents, {} ms, {} "
                            "earlier call(s) replaced, fg={})",
                            fullscreen ? "TRUE" : "FALSE", WaitResultName(result), presents, waited, replaced,
                            FgStateName(fg)),
                result == WaitResult::ReplayAtHardLimit);
        env.Log(std::format("Window: replayed SetFullscreenState({}) returned {:#010x}", fullscreen ? "TRUE" : "FALSE",
                            static_cast<uint32_t>(hr)),
                hr < 0);
    }

    bool FixFloorReached(Env& env) const
    {
        const uint64_t now = env.NowMs();
        const uint64_t first = _gate.FirstOffMs();
        return _gate.Count() >= _fixConfirmPresents && now >= first && now - first >= _fixConfirmMs;
    }

    // B1-a. Runs at the end of a game present while the fix is armed. Frame generation must be confirmed off (a complete
    // present with the feature inactive, DLSS-G output) AND be off right now: the pause after a Deactivate is only about ten
    // frames, so the feature can come back; then the fix waits for the next off period.
    void TryFix(Env& env, const FgSnapshot& fg)
    {
        if (!_gate.Confirmed(env.DlssgOutput()) || !FgIsOff(fg))
            return;
        // Like the replay (M3): enough complete off presents AND enough time since the first of them. After a leave that was
        // not held, the next present is already complete and off, and the restyle that follows resizes the swapchain in the
        // present in which Streamline first sees eOff. Not a gated check yet, so kFixMaxChecks counts only checks past it.
        if (!FixFloorReached(env))
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
        bool ok = false;
        {
            ActionScope scope(_actionDepth, _actionThread, current); // restored even if the call throws
            ok = env.ApplyExStyle(_hwnd, *fixed);
        }

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

    // M2: the bridge present counter at the start of the present in progress, and the bridge it was read from.
    static constexpr uint32_t kAbortLogLines = 5; // the abort line is capped; the count is exact
    std::optional<uint64_t> _serialBegin;
    uint64_t _serialBridge = 0;
    uint32_t _abortedPresents = 0;
    uint32_t _abortLogged = 0;
    bool _counterLiveNoted = false;
    bool _noCounterNoted = false;

    // B1-a
    bool _fixArmed = false;
    uint32_t _fixChecks = 0;
    uint32_t _fixPresents = 0;
    uint64_t _fixArmedMs = 0;
    bool _fixWaitNoted = false;
    bool _ownerSkipLogged = false;
    int _actionDepth = 0; // > 0 while the module is inside one of its own window calls ...
    uint32_t _actionThread = 0; // ... made on this thread (the label nested=yes needs both)
    uint32_t _fixConfirmPresents = kReplayConfirmPresents; // complete off presents needed before the fix
    uint64_t _fixConfirmMs = kReplayConfirmMs;            // and this long since the first of them

    // B2-a
    Deferral _deferral;
    uint64_t _requestMs = 0;                            // when frame generation was last asked off
    bool _fgPresentSinceRequest = false;                // a game present reached the FG present since the last request
    uint32_t _confirmPresents = kReplayConfirmPresents; // complete off presents needed before a replay
    uint64_t _confirmMs = kReplayConfirmMs;            // and this long since the request
    uint64_t _pendingNative = 0;
    uint32_t _pendingThread = 0;
    bool _reRequestNoted = false;
};
} // namespace ext::window
