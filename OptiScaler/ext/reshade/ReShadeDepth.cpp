#include "pch.h"

#include "ext/reshade/ReShadeDepth.h"

#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/lighting/FfxivLightingCapture.h"
#include "ext/reshade/ReShadeAddon.h"
#include "ext/reshade/ReShadeDepthBackend.h"
#include "ext/reshade/ReShadeDepthCopy.h"
#include "ext/reshade/ReShadeDepthDispatch.h"
#include "ext/reshade/ReShadeDepthEvents.h"
#include "ext/reshade/ReShadeDepthGlue.h"
#include "ext/reshade/ReShadeDepthLate.h"
#include "ext/upscaler/MipBias.h"

#include <State.h>

#include <atomic>

namespace ext::reshade::depth
{
namespace
{
using Microsoft::WRL::ComPtr;

// ---- state that needs no construction -----------------------------------------------------------------------------------------------
glue::EvalGate g_gate;                      // the hot-path word: the option and the requests for the evaluate thread
std::atomic<uint64_t> g_frame { 1 };         // the depth-owned frame counter (ticked by the finish_effects callback)
std::atomic<bool> g_facadeCreated { false }; // the binder exists
std::atomic<DWORD> g_evalThread { 0 };       // the thread of the last OnEvaluated that got past the early-out
std::atomic<uint64_t> g_captured { 0 }, g_refused { 0 };
std::atomic<uint32_t> g_lastResult { 0 };
std::atomic<bool> g_nativeMissing { false };

int64_t NowMsImpl() { return static_cast<int64_t>(GetTickCount64()); }
uint64_t FrameImpl() { return g_frame.load(std::memory_order_relaxed); }
uint64_t PrimaryDeviceImpl() { return reinterpret_cast<uint64_t>(State::Instance().currentD3D11Device); }

void LogCounter(Counter counter, uint64_t count)
{
    LOG_INFO("Automatic ReShade depth: {} = {}", CounterName(counter), count);
}

// ---- the singletons ------------------------------------------------------------------------------------------------------------------
struct Facade final : ICaptureSink
{
    LiveDepthBackend backend { LiveRawApi(), BackendEnv { &NowMsImpl, &FrameImpl, &PrimaryDeviceImpl } };
    DepthBinder binder { backend, &LogCounter };
    EventDispatcher dispatcher { binder, backend, g_frame };

    // ---- the evaluate thread's
    DepthCopy* copy = nullptr; // created on the first capture, once the draw it wants to pass is known
    bool copyUsesRawDraw = false;
    glue::ChangeTracker captureLine;
    glue::ResultLog resultLog;
    bool identityLogged = false;

    Facade()
    {
        SetDispatcher(&dispatcher);
        g_facadeCreated.store(true, std::memory_order_release);
    }

    // Every capture the pass publishes goes to the binder; the backend learns the device the copy lives on.
    void Publish(const DepthCapture& capture) noexcept override
    {
        backend.SetCaptureDevice(capture.valid ? capture.device : 0);
        binder.PublishCapture(capture);
    }

    void WithdrawNow()
    {
        if (copy != nullptr)
            copy->Withdraw();
    }
    void ReleaseNow()
    {
        if (copy != nullptr)
            copy->Release();
    }

    // The pass takes its draw at construction. The raw trampoline goes with the native hooked context only; otherwise the context's own
    // Draw inside the bypass scope (the other draw hooks ignore it).
    DepthCopy& PassFor(bool rawDraw)
    {
        if (copy != nullptr && copyUsesRawDraw != rawDraw)
        {
            copy->Release(); // publishes valid = false first
            delete copy;
            copy = nullptr;
        }
        if (copy == nullptr)
        {
            copy = new DepthCopy(*this, rawDraw ? &ext::ffxiv::chara::DrawFullscreenTriangle : nullptr);
            copyUsesRawDraw = rawDraw;
        }
        return *copy;
    }
};

Facade& F()
{
    static Facade* facade = new Facade; // never destroyed
    return *facade;
}

glue::Extent ReadExtent(ID3D11Resource* resource)
{
    if (resource == nullptr)
        return {};
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))))
        return {};
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    return glue::Extent { desc.Width, desc.Height };
}

ID3D11Resource* GetResource(NVSDK_NGX_Parameter* parameters, const char* name)
{
    ID3D11Resource* resource = nullptr;
    if (parameters->Get(name, &resource) != NVSDK_NGX_Result_Success || resource == nullptr)
        parameters->Get(name, reinterpret_cast<void**>(&resource));
    return resource;
}

unsigned GetUInt(NVSDK_NGX_Parameter* parameters, const char* name)
{
    unsigned value = 0;
    parameters->Get(name, &value);
    return value;
}

void Evaluate(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters)
{
    g_evalThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    Facade& f = F();

    // requests left for this thread: the option was switched off, or NGX ended
    const uint32_t requests = g_gate.TakeRequests();
    if ((requests & glue::EvalGate::kRequestWithdraw) != 0)
        f.WithdrawNow();
    if ((requests & glue::EvalGate::kRequestRelease) != 0)
        f.ReleaseNow();

    // ---- the gates, cheapest first
    const int64_t now = NowMsImpl();
    switch (glue::FirstGate(g_gate.Bits(), State::Instance().isShuttingDown, Registered()))
    {
    case glue::Gate::NotRegistered:
        // ReShade may have loaded after us (installed as dxgi.dll): look for it, at most once a second, up to 60 times
        if (!TryLateRegister(true, now))
            return;
        break;
    case glue::Gate::Go:
        break;
    default:
        return;
    }
    if (SubscribedEvents() != kAllDepthEvents && FailedEvents() == 0)
        EnsureSubscribed(); // lazily, at the first evaluate with the option on: our callbacks land after Generic Depth's

    const ConsumerInfo consumer = f.binder.GetConsumer(now);
    if (glue::ConsumerGate(consumer) != glue::Gate::Go)
    {
        f.WithdrawNow(); // nobody reads the depth: what was published is withdrawn (on this thread, which owns the pass)
        return;
    }

    // ---- the sizes of this evaluate
    const glue::Extent render = glue::RenderExtent(GetUInt(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width),
                                                   GetUInt(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height),
                                                   GetUInt(parameters, NVSDK_NGX_Parameter_Width), GetUInt(parameters, NVSDK_NGX_Parameter_Height));
    glue::Extent output = glue::OutputExtent(GetUInt(parameters, NVSDK_NGX_Parameter_OutWidth), GetUInt(parameters, NVSDK_NGX_Parameter_OutHeight), {}, {});
    if (!output.Valid())
        output = glue::OutputExtent(0, 0, ReadExtent(GetResource(parameters, NVSDK_NGX_Parameter_Output)), {});
    if (!output.Valid())
    {
        const auto sizes = ext::upscaler::mipbias::CurrentStatus().sizes; // the display size the feature was created with
        output = glue::OutputExtent(0, 0, {}, glue::Extent { sizes.displayW, sizes.displayH });
    }
    if (glue::SizeGate(render, output, consumer) != glue::Gate::Go)
        return; // unknown sizes, or not the consumer's size (the character preview): nothing is published

    ID3D11Resource* depth = GetResource(parameters, NVSDK_NGX_Parameter_Depth);
    if (depth == nullptr)
        return;

    // ---- the native immediate context (NGX's own may be ReShade's proxy, whose state swaps would go through ReShade)
    ComPtr<ID3D11Device> device;
    depth->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> native;
    if (device != nullptr)
        device->GetImmediateContext(&native);
    ID3D11DeviceContext* const hooked = ext::ffxiv::lighting::FfxivLightingCapture::installed.load() ? ext::ffxiv::lighting::FfxivLightingCapture::context : nullptr;
    if (!f.identityLogged)
    {
        f.identityLogged = true;
        LOG_INFO("Automatic ReShade depth: contexts: NGX gave {:#x}, the depth's device has the native immediate context {:#x}, the hooked "
                 "immediate context is {:#x} ({})",
                 reinterpret_cast<uintptr_t>(context), reinterpret_cast<uintptr_t>(native.Get()), reinterpret_cast<uintptr_t>(hooked),
                 context == native.Get() ? "NGX's context is the native one" : "NGX's context is not the native one (a ReShade proxy?)");
    }
    if (native == nullptr)
    {
        g_nativeMissing.store(true, std::memory_order_relaxed);
        return;
    }
    g_nativeMissing.store(false, std::memory_order_relaxed);

    // ---- the copy
    CaptureArgs args;
    args.outputW = output.w;
    args.outputH = output.h;
    args.baseX = glue::BaseFromInt(static_cast<int>(GetUInt(parameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X)));
    args.baseY = glue::BaseFromInt(static_cast<int>(GetUInt(parameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y)));
    args.renderW = render.w;
    args.renderH = render.h;
    args.frame = g_frame.load(std::memory_order_relaxed);
    args.tickMs = now;

    DepthCopy& pass = f.PassFor(glue::UseRawDraw(hooked, native.Get()));
    CaptureReport report;
    const bool ok = pass.Capture(native.Get(), depth, args, &report);
    g_lastResult.store(static_cast<uint32_t>(report.result), std::memory_order_relaxed);
    if (ok)
    {
        g_captured.fetch_add(1, std::memory_order_relaxed);
        glue::CaptureSignature signature;
        signature.srcW = report.source.width;
        signature.srcH = report.source.height;
        signature.format = static_cast<uint32_t>(report.source.format);
        signature.bindFlags = report.source.bindFlags;
        signature.baseX = args.baseX;
        signature.baseY = args.baseY;
        signature.renderW = args.renderW;
        signature.renderH = args.renderH;
        signature.outW = args.outputW;
        signature.outH = args.outputH;
        if (f.captureLine.Changed(signature))
            LOG_INFO("Automatic ReShade depth: capture: the game's depth is {}x{} format {} bind flags {:#x}, base {},{}, render {}x{}, "
                     "output {}x{}{}",
                     signature.srcW, signature.srcH, signature.format, signature.bindFlags, signature.baseX, signature.baseY,
                     signature.renderW, signature.renderH, signature.outW, signature.outH, report.targetCreated ? " (target created)" : "");
        return;
    }
    g_refused.fetch_add(1, std::memory_order_relaxed);
    if (f.resultLog.ShouldLog(static_cast<uint32_t>(report.result), now))
        LOG_WARN("Automatic ReShade depth: capture refused: {} (the game's depth {}x{} format {} bind flags {:#x}, base {},{}, render {}x{}, "
                 "output {}x{})",
                 ResultName(report.result), report.source.width, report.source.height, static_cast<uint32_t>(report.source.format),
                 report.source.bindFlags, args.baseX, args.baseY, args.renderW, args.renderH, args.outputW, args.outputH);
}
} // namespace

bool Enabled() noexcept { return g_gate.Enabled(); }

void SetEnabled(bool on) noexcept
{
    try
    {
        if (on == g_gate.Enabled())
            return;
        if (on)
            F().binder.SetEnabled(true);
        else if (g_facadeCreated.load(std::memory_order_acquire))
            F().binder.SetEnabled(false); // no new pass; a pass that is open still ends with its finish_effects
        g_gate.SetEnabled(on);
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
        in.registered = Registered();
        in.lateGaveUp = GetLateStatus().gaveUp;
        in.subscriptionFailed = FailedEvents() != 0;
        in.nativeContextMissing = g_nativeMissing.load(std::memory_order_relaxed);
        const uint32_t last = g_lastResult.load(std::memory_order_relaxed);
        in.captureRefusal = last != 0 ? ResultName(static_cast<Result>(last)) : nullptr;
        if (g_facadeCreated.load(std::memory_order_acquire))
            in.binderStatus = F().binder.Status();
        return glue::ComposeStatus(in);
    }
    catch (...)
    {
        return {};
    }
}

Counters GetCounters()
{
    return g_facadeCreated.load(std::memory_order_acquire) ? F().binder.GetCounters() : Counters {};
}

CaptureStats GetCaptureStats()
{
    CaptureStats s;
    s.captured = g_captured.load(std::memory_order_relaxed);
    s.refused = g_refused.load(std::memory_order_relaxed);
    s.lastResult = g_lastResult.load(std::memory_order_relaxed);
    s.frame = g_frame.load(std::memory_order_relaxed);
    return s;
}

ConsumerInfo GetConsumer(int64_t nowMs)
{
    return g_facadeCreated.load(std::memory_order_acquire) ? F().binder.GetConsumer(nowMs) : ConsumerInfo {};
}

void OnEvaluated(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters) noexcept
{
    if (g_gate.Idle()) // the option is off and nothing is pending: one relaxed load
        return;
    try
    {
        Evaluate(context, parameters);
    }
    catch (...)
    {
    }
}

void OnNgxShutdown() noexcept
{
    try
    {
        if (!g_facadeCreated.load(std::memory_order_acquire))
            return;
        if (g_evalThread.load(std::memory_order_relaxed) == GetCurrentThreadId())
        {
            Facade& f = F();
            f.ReleaseNow(); // publishes valid = false first
            return;
        }
        g_gate.Request(glue::EvalGate::kRequestWithdraw | glue::EvalGate::kRequestRelease);
    }
    catch (...)
    {
    }
}
} // namespace ext::reshade::depth
