#include "pch.h"

#include "ext/core/HooksBypass.h"
#include "ext/ffxiv/chara/CharaContextState.h"
#include "ext/reshade/ReShadeD3D11.h"
#include "ext/reshade/ReShadeDepthCopy.h"

#include <DepthCrop_Ps_Shader.h>
#include <DepthCrop_Vs_Shader.h>

#include <utility>

namespace ext::reshade::depth
{
using Microsoft::WRL::ComPtr;

namespace
{
constexpr GUID kInitFailedTag = { 0x3b0c5e61, 0x7d24, 0x4a8e, { 0x9f, 0x15, 0xc2, 0x6a, 0x41, 0xd8, 0x07, 0xe3 } };
} // namespace

// ---- pure helpers -------------------------------------------------------------------------------------------------------
DXGI_FORMAT SrvFormatFor(DXGI_FORMAT source)
{
    switch (source)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

const char* ResultName(Result result)
{
    switch (result)
    {
    case Result::Ok:
        return "ok";
    case Result::NullArgument:
        return "no context or source";
    case Result::DeferredContext:
        return "deferred context";
    case Result::DeviceUnavailable:
        return "device unavailable";
    case Result::BadOutputSize:
        return "bad output size";
    case Result::BadRenderSize:
        return "bad render size";
    case Result::BadSourceSize:
        return "bad source size";
    case Result::NotTexture2D:
        return "not a 2D texture";
    case Result::ArrayTexture:
        return "texture array";
    case Result::Multisampled:
        return "multisampled";
    case Result::NoShaderResource:
        return "no shader resource bind flag";
    case Result::UnsupportedFormat:
        return "unsupported format";
    case Result::NotContained:
        return "render region outside the source";
    case Result::ResourceFailure:
        return "resource creation failed";
    case Result::Fault:
        return "fault";
    }
    return "unknown";
}

Result CheckSource(const SourceDesc& source, const CaptureArgs& args)
{
    const auto edge = [](uint32_t v) { return v >= 1 && v <= kMaxEdge; };
    if (!edge(args.outputW) || !edge(args.outputH))
        return Result::BadOutputSize;
    if (!edge(args.renderW) || !edge(args.renderH))
        return Result::BadRenderSize;
    if (!edge(source.width) || !edge(source.height))
        return Result::BadSourceSize;
    if (source.arraySize != 1)
        return Result::ArrayTexture;
    if (source.sampleCount != 1)
        return Result::Multisampled;
    if ((source.bindFlags & D3D11_BIND_SHADER_RESOURCE) == 0)
        return Result::NoShaderResource;
    if (SrvFormatFor(source.format) == DXGI_FORMAT_UNKNOWN)
        return Result::UnsupportedFormat;
    // Subtract, never add: a base beyond the source must not wrap around (unsigned), and base + render could overflow.
    const bool insideX = args.baseX <= source.width && source.width - args.baseX >= args.renderW;
    const bool insideY = args.baseY <= source.height && source.height - args.baseY >= args.renderH;
    return insideX && insideY ? Result::Ok : Result::NotContained;
}

CropConstants MakeConstants(const CaptureArgs& args)
{
    CropConstants k;
    k.origin = PackPair(args.baseX, args.baseY);
    k.render = PackPair(args.renderW, args.renderH);
    k.output = PackPair(args.outputW, args.outputH);
    return k;
}

bool DeviceReplaced(const void* owned, const void* given, HRESULT removedReason)
{
    return owned == nullptr || owned != given || FAILED(removedReason);
}

void MarkDeviceFailed(ID3D11Device* device)
{
    const uint32_t value = 1;
    device->SetPrivateData(kInitFailedTag, sizeof(value), &value);
}

bool DeviceMarkedFailed(ID3D11Device* device)
{
    UINT size = sizeof(uint32_t);
    uint32_t value = 0;
    return SUCCEEDED(device->GetPrivateData(kInitFailedTag, &size, &value)) && value == 1;
}

// ---- one capture ---------------------------------------------------------------------------------------------------------
// Everything a guarded step reads or writes. The steps are plain functions on raw pointers and members of this struct: the objects
// with destructors live here (in the frame of CaptureImpl), not in the frames of the steps, which an access violation unwinds without
// running destructors (a step only makes trivial locals; logging in InitThunk aside).
struct DepthCopy::Job
{
    DepthCopy* self = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Resource* source = nullptr;
    const CaptureArgs* args = nullptr;
    CaptureReport* report = nullptr;

    // ProbeThunk
    bool immediate = false;
    bool initFailed = false; // the device carries the tag of a failed set-up
    HRESULT removed = S_OK;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Texture2D> texture;
    // PrepareThunk
    Result result = Result::Ok;
    ComPtr<ID3D11ShaderResourceView> srv;
    Target fresh; // the new target when the output size changed
    // DrawThunk, RestoreThunk
    bool drawn = false; // the draw was started: the target may be half written
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3DDeviceContextState> previous; // the game's state, non-null once it has been swapped out
};

struct DepthCopy::InitJob
{
    DepthCopy* self = nullptr;
    ID3D11Device* device = nullptr;
    ComPtr<ID3D11Device1> device1;
    bool ok = false;
};

DepthCopy::DepthCopy(ICaptureSink& sink, DrawTriangleFn draw) : _sink(sink), _draw(draw) {}

DepthCopy::~DepthCopy()
{
    Release();
}

void DepthCopy::Withdraw()
{
    if (!_published)
        return;
    _published = false;
    _sink.Publish(DepthCapture {});
}

void DepthCopy::Teardown()
{
    _target = Target {};
    _constants.Reset();
    _ps.Reset();
    _vs.Reset();
    _state.Reset();
    _device.Reset();
}

void DepthCopy::Release()
{
    RecoverState();
    Withdraw(); // the sink drops the capture before its view goes
    Teardown();
}

void DepthCopy::InitThunk(void* p)
{
    InitJob& j = *static_cast<InitJob*>(p);
    DepthCopy& s = *j.self;
    ID3D11Device* device = j.device;

    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&j.device1))))
    {
        LOG_ERROR("Automatic ReShade depth: the device has no ID3D11Device1 (private context state unavailable)");
        return;
    }

    // The state agrees with the threading mode of the device (as the character preview passes do).
    static_assert(ext::ffxiv::chara::kDeviceCreateSingleThreaded == D3D11_CREATE_DEVICE_SINGLETHREADED);
    static_assert(ext::ffxiv::chara::kContextStateSingleThreaded == D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED);
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(j.device1->CreateDeviceContextState(ext::ffxiv::chara::ContextStateFlags(device->GetCreationFlags()), levels, 1,
                                                 D3D11_SDK_VERSION, __uuidof(ID3D11Device1), &chosen, &s._state)) ||
        s._state == nullptr)
    {
        LOG_ERROR("Automatic ReShade depth: the private context state could not be created");
        return;
    }
    if (FAILED(device->CreateVertexShader(DepthCrop_Vs_cso, sizeof(DepthCrop_Vs_cso), nullptr, &s._vs)) ||
        FAILED(device->CreatePixelShader(DepthCrop_Ps_cso, sizeof(DepthCrop_Ps_cso), nullptr, &s._ps)))
    {
        LOG_ERROR("Automatic ReShade depth: the shaders could not be created");
        return;
    }

    D3D11_BUFFER_DESC bd {};
    bd.ByteWidth = sizeof(CropConstants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &s._constants)))
    {
        LOG_ERROR("Automatic ReShade depth: the constant buffer could not be created");
        return;
    }
    s._device = device;
    j.ok = true;
}

bool DepthCopy::Init(ID3D11Device* device)
{
    Release();
    if (device == nullptr)
        return false;
    InitJob job;
    job.self = this;
    job.device = device;
    try
    {
        if (d3d11::GuardedCall(&InitThunk, &job) != 0)
            job.ok = false;
    }
    catch (...)
    {
        job.ok = false;
    }
    if (!job.ok)
    {
        Teardown();
        ++_initFailures;
    }
    return job.ok;
}

void DepthCopy::ProbeThunk(void* p)
{
    Job& j = *static_cast<Job*>(p);
    j.immediate = j.context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE; // first: a deferred context gets nothing else
    if (!j.immediate)
        return;
    j.context->GetDevice(&j.device);
    if (j.device != nullptr)
    {
        j.removed = j.device->GetDeviceRemovedReason();
        j.initFailed = DeviceMarkedFailed(j.device.Get());
    }
    if (FAILED(j.source->QueryInterface(IID_PPV_ARGS(&j.texture))))
    {
        j.texture.Reset();
        return;
    }
    D3D11_TEXTURE2D_DESC td {};
    j.texture->GetDesc(&td);
    SourceDesc& s = j.report->source;
    s.width = td.Width;
    s.height = td.Height;
    s.arraySize = td.ArraySize;
    s.sampleCount = td.SampleDesc.Count;
    s.bindFlags = td.BindFlags;
    s.format = td.Format;
}

void DepthCopy::PrepareThunk(void* p)
{
    Job& j = *static_cast<Job*>(p);
    DepthCopy& s = *j.self;
    const CaptureArgs& a = *j.args;

    // the typed view of the game's depth (a transient: it is gone when the call returns)
    D3D11_SHADER_RESOURCE_VIEW_DESC vd {};
    vd.Format = j.report->viewFormat;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MostDetailedMip = 0;
    vd.Texture2D.MipLevels = 1;
    if (FAILED(s._device->CreateShaderResourceView(j.texture.Get(), &vd, &j.srv)))
    {
        j.result = Result::ResourceFailure;
        return;
    }

    // the target, recreated only when the output size changed
    if (s._target.texture == nullptr || s._target.width != a.outputW || s._target.height != a.outputH)
    {
        D3D11_TEXTURE2D_DESC td {};
        td.Width = a.outputW;
        td.Height = a.outputH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(s._device->CreateTexture2D(&td, nullptr, &j.fresh.texture)) ||
            FAILED(s._device->CreateRenderTargetView(j.fresh.texture.Get(), nullptr, &j.fresh.rtv)) ||
            FAILED(s._device->CreateShaderResourceView(j.fresh.texture.Get(), nullptr, &j.fresh.srv)))
        {
            j.fresh.texture.Reset();
            j.fresh.rtv.Reset();
            j.fresh.srv.Reset();
            j.result = Result::ResourceFailure;
            return;
        }
        j.fresh.width = a.outputW;
        j.fresh.height = a.outputH;
    }
}

void DepthCopy::DrawThunk(void* p)
{
    Job& j = *static_cast<Job*>(p);
    DepthCopy& s = *j.self;
    const CaptureArgs& a = *j.args;
    ID3D11DeviceContext* c = j.context;

    if (FAILED(c->QueryInterface(IID_PPV_ARGS(&j.context1))))
    {
        j.result = Result::ResourceFailure;
        return;
    }
    j.context1->SwapDeviceContextState(s._state.Get(), &j.previous);
    if (j.previous == nullptr || j.previous == s._state)
    {
        // The game's state could not be captured (or the context was in the private state already, after a swap whose answer was
        // lost): there is nothing to draw in and nothing to swap back to. Never swap the private state into itself again.
        j.previous.Reset();
        j.result = Result::Fault;
        return;
    }

    const Target& target = j.fresh.texture != nullptr ? j.fresh : s._target;
    ID3D11Buffer* constants = s._constants.Get();
    ID3D11ShaderResourceView* srv = j.srv.Get();
    ID3D11RenderTargetView* rtv = target.rtv.Get();
    const D3D11_VIEWPORT viewport { 0.0f, 0.0f, static_cast<float>(a.outputW), static_cast<float>(a.outputH), 0.0f, 1.0f };

    // Anything a failed unbind left in the private state goes first.
    c->ClearState();
    // origin, render size and output size, the only thing that follows the quality mode and the subrect. Written here, in the private
    // state: the game's predication would skip an UpdateSubresource in its own state, and a fresh buffer is all zeros.
    const CropConstants k = MakeConstants(a);
    c->UpdateSubresource(s._constants.Get(), 0, nullptr, &k, 0, 0);
    c->RSSetViewports(1, &viewport);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(s._vs.Get(), nullptr, 0);
    c->PSSetShader(s._ps.Get(), nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &constants);
    c->PSSetShaderResources(0, 1, &srv);
    c->OMSetRenderTargets(1, &rtv, nullptr);
    j.drawn = true;
    if (s._draw != nullptr)
        s._draw(c);
    else
        c->Draw(3, 0);
}

void DepthCopy::UnbindThunk(void* p)
{
    Job& j = *static_cast<Job*>(p);
    if (j.previous == nullptr)
        return; // never left the game's state
    // Leave nothing bound in the private state: it must not keep the game's depth or our target alive. Explicit unbinds, not
    // ClearState: if one of them fails, the swap back (its own step) still runs.
    ID3D11DeviceContext* c = j.context;
    ID3D11ShaderResourceView* noView[1] = {};
    ID3D11Buffer* noBuffer[1] = {};
    c->OMSetRenderTargets(0, nullptr, nullptr);
    c->PSSetShaderResources(0, 1, noView);
    c->PSSetConstantBuffers(0, 1, noBuffer);
    c->VSSetShader(nullptr, nullptr, 0);
    c->PSSetShader(nullptr, nullptr, 0);
}

void DepthCopy::SwapBackThunk(void* p)
{
    Job& j = *static_cast<Job*>(p);
    if (j.previous == nullptr || j.context1 == nullptr)
        return;
    j.context1->SwapDeviceContextState(j.previous.Get(), nullptr);
}

// A swap back that failed (it may or may not have happened) is done again before anything else, with the game's own state object,
// which is kept alive until then.
bool DepthCopy::RecoverState()
{
    if (_stuckPrevious == nullptr)
        return true;
    ext::core::BypassScope bypass;
    Job job;
    job.context1 = _stuckContext;
    job.previous = _stuckPrevious;
    if (d3d11::GuardedCall(&SwapBackThunk, &job) != 0)
        return false;
    _stuckPrevious.Reset();
    _stuckContext.Reset();
    return true;
}
void DepthCopy::PublishValid(const Target& target, const CaptureArgs& args)
{
    DepthCapture c;
    c.valid = true;
    c.outputW = target.width;
    c.outputH = target.height;
    c.renderW = args.renderW;
    c.renderH = args.renderH;
    c.frame = args.frame;
    c.tickMs = args.tickMs;
    c.copyView = reinterpret_cast<uintptr_t>(target.srv.Get());
    c.device = reinterpret_cast<uintptr_t>(_device.Get());
    _published = true;
    _sink.Publish(c);
}

bool DepthCopy::CaptureImpl(ID3D11DeviceContext* context, ID3D11Resource* source, const CaptureArgs& args, CaptureReport& report)
{
    report.args = args;
    // Our own D3D11 calls (the views, the constants, the draw) must not look like game work to the draw hooks.
    ext::core::BypassScope bypass;

    // A swap back that failed on an earlier call comes before anything else.
    if (!RecoverState())
    {
        report.result = Result::Fault;
        return false;
    }
    if (context == nullptr || source == nullptr)
    {
        report.result = Result::NullArgument;
        return false;
    }

    Job job;
    job.self = this;
    job.context = context;
    job.source = source;
    job.args = &args;
    job.report = &report;
    if (d3d11::GuardedCall(&ProbeThunk, &job) != 0)
    {
        report.result = Result::Fault;
        return false;
    }
    if (!job.immediate)
    {
        report.result = Result::DeferredContext;
        return false;
    }
    if (job.device == nullptr)
    {
        report.result = Result::DeviceUnavailable;
        return false;
    }
    if (job.texture == nullptr)
    {
        report.result = Result::NotTexture2D;
        return false;
    }
    // The request is checked before the device: a refused request on a removed device withdraws nothing (the capture ages out).
    report.viewFormat = SrvFormatFor(report.source.format);
    if (const Result refusal = CheckSource(report.source, args); refusal != Result::Ok)
    {
        report.result = refusal;
        return false;
    }

    // another device (or a removed one): what is published is withdrawn, then everything goes and is set up again
    if (DeviceReplaced(_device.Get(), job.device.Get(), job.removed))
    {
        if (job.initFailed) // given up on: the pass that is running (for another device) is left alone
        {
            report.result = Result::DeviceUnavailable;
            return false;
        }
        Release();
        if (FAILED(job.removed))
        {
            report.result = Result::DeviceUnavailable;
            return false;
        }
        if (!Init(job.device.Get()))
        {
            MarkDeviceFailed(job.device.Get());
            report.result = Result::DeviceUnavailable;
            return false;
        }
    }

    if (d3d11::GuardedCall(&PrepareThunk, &job) != 0)
    {
        report.result = Result::Fault;
        return false;
    }
    if (job.result != Result::Ok)
    {
        report.result = job.result;
        return false;
    }

    // The draw runs in the private state. The way back is its own guarded step, so a failed draw cannot skip it.
    const uint32_t drawCode = d3d11::GuardedCall(&DrawThunk, &job);
    const uint32_t unbindCode = d3d11::GuardedCall(&UnbindThunk, &job);
    const uint32_t swapCode = d3d11::GuardedCall(&SwapBackThunk, &job);
    if (swapCode != 0)
    {
        // It may not have happened: the next call (or Release) does it again, and the game's state object must not die before.
        _stuckContext = job.context1;
        _stuckPrevious = job.previous;
    }
    if (drawCode != 0 || unbindCode != 0 || swapCode != 0 || job.result != Result::Ok)
    {
        // Once the draw started the target may be half written, and the capture that is published reads it.
        if (job.drawn)
            Withdraw();
        report.result = job.result != Result::Ok ? job.result : Result::Fault;
        return false;
    }

    // The new capture goes out first; the old target (if the output size changed) is released after that.
    PublishValid(job.fresh.texture != nullptr ? job.fresh : _target, args);
    if (job.fresh.texture != nullptr)
    {
        _target = std::move(job.fresh);
        report.targetCreated = true;
    }
    report.result = Result::Ok;
    return true;
}

bool DepthCopy::Capture(ID3D11DeviceContext* context, ID3D11Resource* source, const CaptureArgs& args, CaptureReport* report) noexcept
{
    CaptureReport local;
    CaptureReport& r = report != nullptr ? *report : local;
    r = CaptureReport {};
    try
    {
        return CaptureImpl(context, source, args, r);
    }
    catch (...)
    {
        r.result = Result::Fault;
        return false;
    }
}
} // namespace ext::reshade::depth
