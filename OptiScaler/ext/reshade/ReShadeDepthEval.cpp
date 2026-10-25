#include "pch.h"

#include "ext/reshade/ReShadeDepthEval.h"

#include <d3d11_1.h>
#include <d3d12.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#include <wrl/client.h>

namespace ext::reshade::depth
{
namespace
{
using Microsoft::WRL::ComPtr;

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
} // namespace

CaptureEngine::CaptureEngine(glue::EvalGate& gate, std::atomic<uint64_t>& frame, ICaptureSink& downstream, IEvalHost& host)
    : _gate(gate), _frame(frame), _downstream(downstream), _host(host)
{
}

CaptureEngine::~CaptureEngine()
{
    delete _copy; // the destructor releases the pass, publishing valid = false first
}

CaptureStats CaptureEngine::Stats() const
{
    CaptureStats s;
    s.captured = _captured.load(std::memory_order_relaxed);
    s.refused = _refused.load(std::memory_order_relaxed);
    s.lastResult = _lastResult.load(std::memory_order_relaxed);
    s.frame = _frame.load(std::memory_order_relaxed);
    return s;
}

// The pass takes its draw at construction. The raw trampoline goes with the native hooked context only; otherwise the context's own
// Draw inside the bypass scope (the other draw hooks ignore it).
DepthCopy& CaptureEngine::PassFor(bool rawDraw)
{
    if (_copy != nullptr && _copyUsesRawDraw != rawDraw)
    {
        delete _copy; // the destructor releases the pass, publishing valid = false first
        _copy = nullptr;
    }
    if (_copy == nullptr)
    {
        _copy = new DepthCopy(_downstream, rawDraw ? _host.RawDraw() : nullptr);
        _copyUsesRawDraw = rawDraw;
    }
    return *_copy;
}

void CaptureEngine::OnEvaluated(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters, const DisplaySource& display) noexcept
{
    if (_gate.Idle()) // the option is off and nothing is pending: one relaxed load
        return;
    try
    {
        Run(context, parameters, display);
    }
    catch (...)
    {
    }
}

void CaptureEngine::OnNgxShutdown() noexcept
{
    try
    {
        if (_evalThread.load(std::memory_order_relaxed) == _host.ThreadId())
        {
            if (_copy != nullptr)
                _copy->Release(); // publishes valid = false first
            return;
        }
        _gate.Request(glue::EvalGate::kRequestWithdraw | glue::EvalGate::kRequestRelease);
    }
    catch (...)
    {
    }
}

void CaptureEngine::Run(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters, const DisplaySource& display)
{
    _evalThread.store(_host.ThreadId(), std::memory_order_relaxed);

    // requests left for this thread: the option was switched off, or NGX ended
    const uint32_t requests = _gate.TakeRequests();
    if (_copy != nullptr)
    {
        if ((requests & glue::EvalGate::kRequestRelease) != 0)
            _copy->Release(); // publishes valid = false first
        else if ((requests & glue::EvalGate::kRequestWithdraw) != 0)
            _copy->Withdraw();
    }

    // ---- the gates, cheapest first
    const int64_t now = _host.NowMs();
    switch (glue::FirstGate(_gate.Bits(), _host.ShuttingDown(), _host.Registered()))
    {
    case glue::Gate::NotRegistered:
        // ReShade may have loaded after us (installed as dxgi.dll): look for it, at most once a second, up to 60 times
        if (!_host.TryLateRegister(_gate.Enabled(), now))
            return;
        break;
    case glue::Gate::Go:
        break;
    default:
        return;
    }
    _host.EnsureSubscribed(); // lazily, at the first evaluate with the option on: our callbacks land after Generic Depth's

    const ConsumerInfo consumer = _host.GetConsumer(now);
    if (glue::ConsumerGate(consumer) != glue::Gate::Go)
    {
        if (_copy != nullptr)
            _copy->Withdraw(); // nobody reads the depth: what was published is withdrawn (on this thread, which owns the pass)
        return;
    }

    // ---- the sizes of this evaluate
    const glue::Extent render = glue::RenderExtent(GetUInt(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width),
                                                   GetUInt(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height),
                                                   GetUInt(parameters, NVSDK_NGX_Parameter_Width), GetUInt(parameters, NVSDK_NGX_Parameter_Height));
    glue::Extent output = glue::OutputExtent(GetUInt(parameters, NVSDK_NGX_Parameter_OutWidth), GetUInt(parameters, NVSDK_NGX_Parameter_OutHeight), {}, {});
    if (!output.Valid())
        output = glue::OutputExtent(0, 0, ReadExtent(GetResource(parameters, NVSDK_NGX_Parameter_Output)), {});
    if (!output.Valid() && display.get != nullptr)
        output = glue::OutputExtent(0, 0, {}, display.get(display.feature)); // the display size the feature was created with
    if (glue::SizeGate(render, output, consumer) != glue::Gate::Go)
        return; // unknown sizes, or not the consumer's size (the character preview): nothing is published

    ID3D11Resource* depth = GetResource(parameters, NVSDK_NGX_Parameter_Depth);
    if (depth == nullptr)
        return;

    // ---- the native immediate context
    ComPtr<ID3D11Device> device;
    depth->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> native;
    if (device != nullptr)
        device->GetImmediateContext(&native);
    ID3D11DeviceContext* const hooked = _host.HookedContext();
    if (!_identityLogged)
    {
        _identityLogged = true;
        LOG_INFO("Automatic ReShade depth: contexts: NGX gave {:#x}, the depth's device has the native immediate context {:#x}, the hooked "
                 "immediate context is {:#x} ({}); evaluate thread {}",
                 reinterpret_cast<uintptr_t>(context), reinterpret_cast<uintptr_t>(native.Get()), reinterpret_cast<uintptr_t>(hooked),
                 context == native.Get() ? "NGX's context is the native one" : "NGX's context is not the native one (a ReShade proxy?)",
                 _host.ThreadId());
    }
    if (native == nullptr)
    {
        _nativeMissing.store(true, std::memory_order_relaxed);
        return;
    }
    _nativeMissing.store(false, std::memory_order_relaxed);

    // ---- the copy
    CaptureArgs args;
    args.outputW = output.w;
    args.outputH = output.h;
    args.baseX = glue::BaseFromInt(static_cast<int>(GetUInt(parameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X)));
    args.baseY = glue::BaseFromInt(static_cast<int>(GetUInt(parameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y)));
    args.renderW = render.w;
    args.renderH = render.h;
    args.frame = _frame.load(std::memory_order_relaxed);
    args.tickMs = now;

    DepthCopy& pass = PassFor(glue::UseRawDraw(hooked, native.Get()));
    CaptureReport report;
    const bool ok = pass.Capture(native.Get(), depth, args, &report);
    _lastResult.store(static_cast<uint32_t>(report.result), std::memory_order_relaxed);
    if (ok)
    {
        _captured.fetch_add(1, std::memory_order_relaxed);
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
        if (_captureLine.Changed(signature))
            LOG_INFO("Automatic ReShade depth: capture: the game's depth is {}x{} format {} bind flags {:#x}, base {},{}, render {}x{}, "
                     "output {}x{}{}",
                     signature.srcW, signature.srcH, signature.format, signature.bindFlags, signature.baseX, signature.baseY,
                     signature.renderW, signature.renderH, signature.outW, signature.outH, report.targetCreated ? " (target created)" : "");
        return;
    }
    _refused.fetch_add(1, std::memory_order_relaxed);
    if (_resultLog.ShouldLog(static_cast<uint32_t>(report.result), now))
        LOG_WARN("Automatic ReShade depth: capture refused: {} (the game's depth {}x{} format {} bind flags {:#x}, base {},{}, render {}x{}, "
                 "output {}x{})",
                 ResultName(report.result), report.source.width, report.source.height, static_cast<uint32_t>(report.source.format),
                 report.source.bindFlags, args.baseX, args.baseY, args.renderW, args.renderH, args.outputW, args.outputH);
}
} // namespace ext::reshade::depth
