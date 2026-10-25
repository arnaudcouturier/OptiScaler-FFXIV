#include "pch.h"

#include "ext/reshade/ReShadeDepth.h"
#include "ext/reshade/ReShadeSeams.h" // the seam declarations: the definitions below must match them

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/FfxivConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/lighting/FfxivLightingCapture.h"
#include "ext/reshade/ReShadeAddon.h"
#include "ext/reshade/ReShadeDepthBackend.h"
#include "ext/reshade/ReShadeDepthDispatch.h"
#include "ext/reshade/ReShadeDepthEval.h"
#include "ext/reshade/ReShadeDepthEvents.h"
#include "ext/reshade/ReShadeDepthGlue.h"
#include "ext/reshade/ReShadeDepthLate.h"

#include <State.h>
#include <upscalers/IFeature_Dx11.h>

#include <atomic>

namespace ext::reshade::depth
{
namespace
{
// ---- state that needs no construction -----------------------------------------------------------------------------------------------
// The option and the requests for the evaluate thread: one word. Config load and reload only STORE into it (they may run inside DllMain,
// where nothing is constructed); the facade below is built by the first evaluate that finds the option on.
glue::EvalGate g_gate;
std::atomic<uint64_t> g_frame { 1 }; // the depth-owned frame counter: one tick per capture, by the capture engine (no ReShade event moves it)

int64_t NowMsImpl() { return static_cast<int64_t>(GetTickCount64()); }
uint64_t FrameImpl() { return g_frame.load(std::memory_order_relaxed); }
uint64_t PrimaryDeviceImpl() { return reinterpret_cast<uint64_t>(State::Instance().currentD3D11Device); }

void LogCounter(Counter counter, uint64_t count)
{
    LOG_INFO("Automatic ReShade depth: {} = {}", CounterName(counter), count);
}

// ---- the process, as the capture engine sees it -----------------------------------------------------------------------------------
class ProcessHost final : public IEvalHost
{
  public:
    explicit ProcessHost(DepthBinder& binder) : _binder(binder) {}

    bool ShuttingDown() override { return State::Instance().isShuttingDown; }
    int64_t NowMs() override { return NowMsImpl(); }
    uint32_t ThreadId() override { return GetCurrentThreadId(); }
    bool Registered() override { return ext::reshade::Registered(); }
    bool TryLateRegister(bool enabled, int64_t nowMs) override { return ext::reshade::depth::TryLateRegister(enabled, nowMs); }
    void EnsureSubscribed() override
    {
        // A safety net for a registration that neither path has subscribed for. The load path (OnReshadeLoaded) leaves begin and finish to
        // the init_effect_runtime callback, so that they come after the begin_effects of REST and Generic Depth: the evaluate thread must not
        // subscribe them (before theirs, and from the wrong thread). The late path subscribes all six right after its registration.
        if (SubscriptionPath() == SubscribedVia::None && FailedEvents() == 0)
            ext::reshade::depth::EnsureSubscribed();
    }
    ConsumerInfo GetConsumer(int64_t nowMs) override { return _binder.GetConsumer(nowMs); }
    ID3D11DeviceContext* HookedContext() override
    {
        return ext::ffxiv::lighting::FfxivLightingCapture::installed.load() ? ext::ffxiv::lighting::FfxivLightingCapture::context : nullptr;
    }
    DrawTriangleFn RawDraw() override { return &ext::ffxiv::chara::DrawFullscreenTriangle; }

  private:
    DepthBinder& _binder;
};

// ---- the singletons (heap, never destroyed) ----------------------------------------------------------------------------------------
struct Facade final : ICaptureSink
{
    LiveDepthBackend backend { LiveRawApi(), BackendEnv { &NowMsImpl, &FrameImpl, &PrimaryDeviceImpl } };
    DepthBinder binder { backend, &LogCounter };
    EventDispatcher dispatcher { binder, backend };
    ProcessHost host { binder };
    CaptureEngine engine { g_gate, g_frame, *this, host };

    Facade()
    {
        binder.SetEnabled(g_gate.Enabled());
        SetDispatcher(&dispatcher);
    }

    // Every capture the pass publishes goes to the binder.
    void Publish(const DepthCapture& capture) noexcept override { binder.PublishCapture(capture); }
};

std::atomic<Facade*> g_facade { nullptr };

// The game gate: 0 = not known yet (State::gameExe is set early in startup), 1 = ffxiv_dx11.exe, 2 = anything else. Decided once, the
// first time it is asked after the one-load gate, and never on the path of an option that is off.
std::atomic<int> g_game { 0 };

int GameState()
{
    int state = g_game.load(std::memory_order_relaxed);
    if (state != 0)
        return state;
    const std::string& exe = State::Instance().gameExe;
    if (exe.empty())
        return 0;
    state = glue::IsFfxivExe(exe) ? 1 : 2;
    g_game.store(state, std::memory_order_relaxed);
    return state;
}

Facade& Build()
{
    static Facade* facade = new Facade; // never destroyed
    g_facade.store(facade, std::memory_order_release);
    return *facade;
}

glue::Extent DisplayOf(void* feature)
{
    auto* f = static_cast<IFeature_Dx11*>(feature);
    return glue::Extent { f->DisplayWidth(), f->DisplayHeight() };
}

// [FFXIV] AutomaticReShadeDepth was read (config load and reload): a pure store, nothing is constructed
void OnConfigLoaded(bool on) { SetEnabled(on); }

// Runs once, when this translation unit's statics are initialized. The order of that across translation units is unspecified, so a
// config load that ran BEFORE it (it found no hook) is made up for here with the value the config holds; Cfg() is a plain function
// static that loads nothing.
bool RegisterLoadHook()
{
    FfxivConfig::automaticReShadeDepthOnLoaded = &OnConfigLoaded;
    OnConfigLoaded(ext::Cfg().ffxiv.automaticReShadeDepth.value_or_default());
    return true;
}
[[maybe_unused]] const bool kLoadHookRegistered = RegisterLoadHook();
} // namespace

bool Enabled() noexcept { return g_gate.Enabled(); }

void SetEnabled(bool on) noexcept
{
    try
    {
        if (on == g_gate.Enabled())
            return;
        if (Facade* f = g_facade.load(std::memory_order_acquire))
            f->binder.SetEnabled(on); // no new pass; a pass that is open still ends with its finish_effects
        g_gate.SetEnabled(on);        // (switching off leaves the requests for the evaluate thread)
    }
    catch (...)
    {
    }
}

std::string Status()
{
    try
    {
        glue::StatusInputs in;
        in.enabled = g_gate.Enabled();
        in.gameSupported = GameState() != 2; // (not known yet is not "another game")
        const AddonStatus addon = ext::reshade::Status();
        in.registered = addon.registered;
        in.reason = addon.reason;
        in.lateGaveUp = GetLateStatus().gaveUp;
        in.subscriptionFailed = FailedEvents() != 0;
        if (Facade* f = g_facade.load(std::memory_order_acquire))
        {
            in.nativeContextMissing = f->engine.NativeContextMissing();
            const uint32_t last = f->engine.Stats().lastResult;
            in.captureRefusal = last != 0 ? ResultName(static_cast<Result>(last)) : nullptr;
            in.binderStatus = f->binder.Status();
        }
        return glue::ComposeStatus(in);
    }
    catch (...)
    {
        return {};
    }
}

Counters GetCounters()
{
    Facade* f = g_facade.load(std::memory_order_acquire);
    return f != nullptr ? f->binder.GetCounters() : Counters {};
}

CaptureStats GetCaptureStats()
{
    Facade* f = g_facade.load(std::memory_order_acquire);
    CaptureStats s = f != nullptr ? f->engine.Stats() : CaptureStats {};
    s.frame = g_frame.load(std::memory_order_relaxed);
    return s;
}

ConsumerInfo GetConsumer(int64_t nowMs)
{
    Facade* f = g_facade.load(std::memory_order_acquire);
    return f != nullptr ? f->binder.GetConsumer(nowMs) : ConsumerInfo {};
}

void OnEvaluated(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters, IFeature_Dx11* feature) noexcept
{
    if (g_gate.Idle()) // the option is off and nothing is pending: one relaxed load
        return;
    try
    {
        Facade* f = g_facade.load(std::memory_order_acquire);
        if (f == nullptr)
        {
            // nothing exists yet, so a request (the option was switched off) has nothing to carry out
            if (!g_gate.Enabled())
            {
                g_gate.TakeRequests();
                return;
            }
            if (GameState() != 1) // another game with the ini key on: nothing is built, nothing is done
                return;
            f = &Build(); // first evaluate with the option on, on the evaluate thread and outside DllMain
        }
        f->engine.OnEvaluated(context, parameters, DisplaySource { feature != nullptr ? &DisplayOf : nullptr, feature });
    }
    catch (...)
    {
    }
}

void OnNgxShutdown() noexcept
{
    try
    {
        if (Facade* f = g_facade.load(std::memory_order_acquire))
            f->engine.OnNgxShutdown();
    }
    catch (...)
    {
    }
}
} // namespace ext::reshade::depth
