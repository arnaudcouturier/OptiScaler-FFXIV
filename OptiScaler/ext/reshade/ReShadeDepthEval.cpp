#include "pch.h"

#include "ext/reshade/ReShadeDepthEval.h"

#include "ext/reshade/ReShadeD3D11.h"

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

// ---- the reads on the upscaler's pointers ----------------------------------------------------------------------------------------
// The parameter block, the Output and Depth resources, the depth's device and the feature are objects the upscaler has just used, so a
// fault in them is not expected; but a C++ catch does not see a hardware access violation (/EHsc), so the reads run as POD thunks under
// d3d11::GuardedCall (SEH), and a fault ends that evaluate and nothing else. The copy pass guards its own D3D calls.
struct SizeReads
{
    NVSDK_NGX_Parameter* parameters = nullptr;
    DisplaySource display;
    glue::Extent render, output;
    uint32_t baseX = 0, baseY = 0;
};

void ReadSizes(void* arg)
{
    auto& in = *static_cast<SizeReads*>(arg);
    NVSDK_NGX_Parameter* const p = in.parameters;
    in.render = glue::RenderExtent(GetUInt(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width),
                                   GetUInt(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height), GetUInt(p, NVSDK_NGX_Parameter_Width),
                                   GetUInt(p, NVSDK_NGX_Parameter_Height));
    in.output = glue::OutputExtent(GetUInt(p, NVSDK_NGX_Parameter_OutWidth), GetUInt(p, NVSDK_NGX_Parameter_OutHeight), {}, {});
    if (!in.output.Valid())
        in.output = glue::OutputExtent(0, 0, ReadExtent(GetResource(p, NVSDK_NGX_Parameter_Output)), {});
    if (!in.output.Valid() && in.display.get != nullptr)
        in.output = glue::OutputExtent(0, 0, {}, in.display.get(in.display.feature)); // the display size the feature was created with
    // the base of the depth subrect is never clamped: a negative value becomes a huge one, which the pass refuses
    in.baseX = glue::BaseFromInt(static_cast<int>(GetUInt(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X)));
    in.baseY = glue::BaseFromInt(static_cast<int>(GetUInt(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y)));
}

struct DepthReads
{
    NVSDK_NGX_Parameter* parameters = nullptr;
    ID3D11Resource* depth = nullptr;
    ID3D11DeviceContext* native = nullptr; // holds a reference when set: the caller attaches it
};

void ReadDepth(void* arg)
{
    auto& in = *static_cast<DepthReads*>(arg);
    in.depth = GetResource(in.parameters, NVSDK_NGX_Parameter_Depth);
    if (in.depth == nullptr)
        return;
    ComPtr<ID3D11Device> device;
    in.depth->GetDevice(&device);
    if (device != nullptr)
        device->GetImmediateContext(&in.native);
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
// One thunk under SEH. False = it faulted (or an exception crossed it): said once with the code, and this evaluate is skipped.
bool CaptureEngine::Guarded(void (*thunk)(void*), void* arg, [[maybe_unused]] const char* what)
{
    const uint32_t code = d3d11::GuardedCall(thunk, arg);
    if (code == 0)
        return true;
    if (!_faultLogged)
    {
        _faultLogged = true;
        LOG_WARN("Automatic ReShade depth: reading {} failed with {:#x}; this evaluate is skipped", what, code);
    }
    return false;
}

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
        if (_host.ShuttingDown()) // the process is going down: nothing is touched
            return;
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
    if (parameters == nullptr) // (the upscaler always passes one; a null has nothing to read)
        return;
    SizeReads sizes;
    sizes.parameters = parameters;
    sizes.display = display;
    if (!Guarded(&ReadSizes, &sizes, "the sizes of the evaluate"))
        return;
    const glue::Extent render = sizes.render;
    const glue::Extent output = sizes.output;
    if (glue::SizeGate(render, output, consumer) != glue::Gate::Go)
        return; // unknown sizes, or not the consumer's size (the character preview): nothing is published

    DepthReads reads;
    reads.parameters = parameters;
    const bool readDepth = Guarded(&ReadDepth, &reads, "the depth resource");
    ComPtr<ID3D11DeviceContext> native;
    native.Attach(reads.native);
    ID3D11Resource* const depth = reads.depth;
    if (!readDepth || depth == nullptr)
        return;

    // ---- the native immediate context
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
    args.baseX = sizes.baseX;
    args.baseY = sizes.baseY;
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
