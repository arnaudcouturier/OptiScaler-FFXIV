#include "pch.h"

#include "ext/dx11bridge/BridgeAccess.h"
#include "ext/window/WindowController.h"
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
namespace
{
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
    bool ApplyExStyle(uint64_t hwnd, uint32_t exstyle) override { return win32::ApplyExStyle(hwnd, exstyle); }

    // What upstream's hkSetFullscreenState does first when frame generation is active: fgChanged, UpdateTarget, Deactivate.
    void RequestFgOff() override
    {
        if (auto* fg = State::Instance().currentFG)
        {
            State::Instance().fgChanged = true;
            fg->UpdateTarget();
            fg->Deactivate();
        }
    }
    uint64_t NowMs() override { return GetTickCount64(); }

    // The held call keeps the swapchain alive (the bridge swapchain counts its references normally).
    void Retain(uint64_t native) override
    {
        if (native != 0)
            reinterpret_cast<IUnknown*>(native)->AddRef();
    }
    void Release(uint64_t native) override
    {
        // Not at shutdown: upstream tears the swapchain down itself and a late Release could run its destructor under the
        // loader lock.
        if (native != 0 && !State::Instance().isShuttingDown)
            reinterpret_cast<IUnknown*>(native)->Release();
    }

    // `native` is ReShade's swapchain->get_native(), checked against the bridge swapchain before the call was held. The call
    // goes straight to it, not through ReShade's wrapper, so the module's own event is not entered again. The target is null
    // (upstream then uses the window's monitor): the event's third argument is an hmonitor/output value of undocumented type
    // and is never dereferenced.
    int32_t Replay(uint64_t native, bool fullscreen) override
    {
        if (native == 0)
            return E_POINTER;
        return static_cast<int32_t>(
            reinterpret_cast<IDXGISwapChain*>(native)->SetFullscreenState(fullscreen ? TRUE : FALSE, nullptr));
    }

    // The log macro puts the function name in front of the text; the module's own prefix "Window:" follows it.
    void Log(const std::string& line, bool warn) override
    {
        if (warn)
            LOG_WARN("{}", line);
        else
            LOG_INFO("{}", line);
    }
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
        g.controller.OnResizeDone(g.env, CurrentMode(), bridge, hwnd, swapWidth, swapHeight);
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
        return g.controller.OnSetFullscreenState(g.env, CurrentMode(), ev);
    }
    catch (...)
    {
        return false; // fail open: the game's call goes through
    }
}
} // namespace ext::window
