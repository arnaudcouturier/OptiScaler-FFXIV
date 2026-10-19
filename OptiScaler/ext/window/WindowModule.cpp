#include "pch.h"

#include "ext/dx11bridge/BridgeAccess.h"
#include "ext/window/WindowController.h"
#include "ext/window/WindowLiveOps.h"
#include "ext/window/WindowSeams.h"
#include "ext/window/WindowWin32.h"

#include <framegen/IFGFeature_Dx12.h>

#include <Config.h>
#include <State.h>

#include <mutex>

// The live side of the window-mode module: the real Env behind Controller, one controller for the process and the entry
// points of WindowSeams.h. The decisions are in WindowController.h / WindowRules.h (tested offline).
namespace ext::window
{
static_assert(live::kPointerError == E_POINTER, "WindowLiveOps.h repeats E_POINTER");

namespace
{
std::recursive_mutex& ModuleLock();

// Entry points hold the module lock once per call (re-entrant calls on the same thread nest). `t_lockDepth` says how often
// this thread holds it, so a call out can give it up completely (live::UnlockedScope, tested on a fake mutex): the controller
// has settled its state before every call out (and clears the request bookkeeping again after the one that asks frame
// generation off), and a window call that waits for another thread must not do so while holding the lock.
thread_local int t_lockDepth = 0;
struct LockDepth : live::LockDepthGuard
{
    LockDepth() : live::LockDepthGuard(t_lockDepth) {}
};
struct Unlocked : live::UnlockedScope<std::recursive_mutex>
{
    Unlocked() : live::UnlockedScope<std::recursive_mutex>(ModuleLock(), t_lockDepth) {}
};

class LiveEnv final : public Env
{
  public:
    bool ForceBorderless() override { return Config::Instance()->FGXeFGForceBorderless.value_or_default(); }
    bool DlssgOutput() override { return State::Instance().activeFgOutput == FGOutput::DLSSG; }
    bool Exclusive() override { return State::Instance().SCExclusiveFullscreen; }

    FgSnapshot Fg() override
    {
        FgSnapshot snapshot;
        if (auto* fg = State::Instance().currentFG)
        {
            snapshot.present = true;
            snapshot.active = fg->IsActive();
            snapshot.paused = fg->IsPaused();
        }
        return snapshot;
    }

    uint32_t CurrentThread() override { return GetCurrentThreadId(); }
    uint32_t OwnerThread(uint64_t hwnd) override { return win32::OwnerThread(hwnd); }
    WindowInfo ReadWindow(uint64_t hwnd) override { return win32::Read(hwnd); }
    uint64_t MonitorOfWindow(uint64_t hwnd) override
    {
        return reinterpret_cast<uint64_t>(MonitorFromWindow(reinterpret_cast<HWND>(static_cast<uintptr_t>(hwnd)), MONITOR_DEFAULTTONEAREST));
    }
    uint64_t FgSwapChain() override
    {
        if (_bridge == nullptr)
            return 0;
        return reinterpret_cast<uint64_t>(static_cast<IDXGISwapChain*>(ext::Access::Bridge::FgSwapChain(*_bridge)));
    }
    // The bridge object, set at a bridge resize and cleared when that bridge is destroyed (both under the module lock). Only
    // read here, never owned.
    void SetBridge(Dx11wDx12SC* sc) { _bridge = sc; }
    Dx11wDx12SC* Bridge() const { return _bridge; }
    std::optional<uint64_t> PresentSerial() override
    {
        if (_bridge == nullptr)
            return std::nullopt;
        return PresentSerialFromIndex(ext::Access::Bridge::FakeBackBufferIndex(*_bridge),
                                      ext::Access::Bridge::BufferCount(*_bridge));
    }

    bool ApplyExStyle(uint64_t hwnd, uint32_t exstyle) override
    {
        Unlocked unlocked;
        return win32::ApplyExStyle(hwnd, exstyle);
    }

    // What upstream's hkSetFullscreenState does first when frame generation is active: fgChanged, UpdateTarget, Deactivate.
    void RequestFgOff() override
    {
        Unlocked unlocked;
        live::RequestOff(State::Instance().currentFG, State::Instance().fgChanged);
    }
    uint64_t NowMs() override { return GetTickCount64(); }

    // No reference is taken on the held call's swapchain (an extra AddRef would change when upstream tears it down); the hold
    // is cancelled when the bridge changes or is destroyed, and the replay checks the pointer against the live bridge.

    // `native` is ReShade's swapchain->get_native(), checked against the bridge swapchain before the call was held. The call
    // goes straight to it, not through ReShade's wrapper, so the module's own event is not entered again. The target is null
    // (upstream then uses the window's monitor): the event's third argument is an hmonitor/output value of undocumented type
    // and is never dereferenced.
    int32_t Replay(uint64_t native, bool fullscreen) override
    {
        Unlocked unlocked;
        return live::ReplaySetFullscreen(reinterpret_cast<IDXGISwapChain*>(static_cast<uintptr_t>(native)), fullscreen);
    }

    // The log macro puts the function name in front of the text; the module's own prefix "Window:" follows it.
    void Log(const std::string& line, bool warn) override
    {
        if (warn)
            LOG_WARN("{}", line);
        else
            LOG_INFO("{}", line);
    }

  private:
    Dx11wDx12SC* _bridge = nullptr;
};

// Heap-allocated and never destroyed: nothing runs at DLL_PROCESS_DETACH (loader lock), and callbacks that may still
// arrive while the process shuts down only ever see live objects.
struct Global
{
    std::recursive_mutex lock;
    Controller controller;
    LiveEnv env;
};

Global& G()
{
    static Global* g = new Global;
    return *g;
}

std::recursive_mutex& ModuleLock() { return G().lock; }

bool Live() { return !State::Instance().isShuttingDown; }
} // namespace

void ResizeDoneSlow(Dx11wDx12SC& sc) noexcept
{
    try
    {
        if (!Live())
            return;
        const uint64_t bridge = reinterpret_cast<uint64_t>(static_cast<IDXGISwapChain*>(&sc));
        const uint64_t hwnd = reinterpret_cast<uint64_t>(ext::Access::Bridge::Window(sc));
        // Size of the FG swapchain the game's follow-up ResizeBuffers would (not) resize: the hold needs it equal to the window.
        uint32_t swapWidth = 0;
        uint32_t swapHeight = 0;
        if (auto* fgChain = ext::Access::Bridge::FgSwapChain(sc))
        {
            DXGI_SWAP_CHAIN_DESC1 desc {};
            if (SUCCEEDED(fgChain->GetDesc1(&desc)))
            {
                swapWidth = desc.Width;
                swapHeight = desc.Height;
            }
        }
        auto& g = G();
        std::lock_guard lock(g.lock);
        LockDepth depth;
        g.env.SetBridge(&sc);
        g.controller.OnResizeDone(g.env, CurrentMode(), bridge, hwnd, swapWidth, swapHeight);
    }
    catch (...)
    {
    }
}

void DestroySlow(Dx11wDx12SC& sc) noexcept
{
    try
    {
        // Also at shutdown: the pointer is only compared, never used.
        const uint64_t bridge = reinterpret_cast<uint64_t>(static_cast<IDXGISwapChain*>(&sc));
        auto& g = G();
        std::lock_guard lock(g.lock);
        LockDepth depth;
        if (g.env.Bridge() == &sc)
            g.env.SetBridge(nullptr); // never read a destroyed bridge
        g.controller.OnBridgeDestroyed(g.env, CurrentMode(), bridge);
    }
    catch (...)
    {
    }
}

void PresentBeginSlow() noexcept
{
    try
    {
        if (!Live())
            return;
        auto& g = G();
        std::lock_guard lock(g.lock);
        LockDepth depth;
        g.controller.OnPresentBegin(g.env, CurrentMode());
    }
    catch (...)
    {
    }
}

void PresentEndSlow() noexcept
{
    try
    {
        if (!Live())
            return;
        auto& g = G();
        std::lock_guard lock(g.lock);
        LockDepth depth;
        g.controller.OnPresentEnd(g.env, CurrentMode());
    }
    catch (...)
    {
    }
}

bool SetFullscreenStateSlow(uint64_t native, uint64_t hwnd, bool fullscreen, uint64_t target) noexcept
{
    try
    {
        if (!Live())
            return false;
        FsEvent ev;
        ev.native = native;
        ev.hwnd = hwnd;
        ev.fullscreen = fullscreen;
        ev.target = target;
        auto& g = G();
        std::lock_guard lock(g.lock);
        LockDepth depth;
        return g.controller.OnSetFullscreenState(g.env, CurrentMode(), ev);
    }
    catch (...)
    {
        return false; // fail open: the game's call goes through
    }
}
} // namespace ext::window
