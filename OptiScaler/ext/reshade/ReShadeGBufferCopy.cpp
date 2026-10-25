#include "pch.h"

#include "ext/core/HooksBypass.h"
#include "ext/ffxiv/chara/CharaContextState.h"
#include "ext/reshade/ReShadeD3D11.h"
#include "ext/reshade/ReShadeDepthGlue.h"
#include "ext/reshade/ReShadeGBufferCopy.h"

#include <GBufferRemap_Ps_Shader.h>
#include <GBufferRemap_Vs_Shader.h>

#include <bit>
#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>

namespace ext::reshade::depth
{
using Microsoft::WRL::ComPtr;

namespace
{
// The device tag of a failed set-up: its own, so a device the depth pass gave up on is still tried by this one, and the other way round.
constexpr GUID kInitFailedTag = { 0x9d41c7a2, 0x5be8, 0x4f63, { 0xa1, 0x0c, 0x38, 0xe7, 0x2b, 0x6d, 0x95, 0xf4 } };

// (the log lines only: the same names GBufferLabel gives, kept here so this file links without the binder's source)
[[maybe_unused]] const char* KindName(GBuffer kind)
{
    switch (kind)
    {
    case GBuffer::Normals:
        return "normals";
    case GBuffer::Decals:
        return "decals";
    case GBuffer::DecalsWater:
        return "water decals";
    case GBuffer::MotionVectors:
        return "motion";
    case GBuffer::Material:
        return "material";
    case GBuffer::Count:
        break;
    }
    return "?";
}

ID3D11ShaderResourceView* ViewOf(uint64_t value) { return reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<uintptr_t>(value)); }
uint64_t ValueOf(const void* pointer) { return reinterpret_cast<uintptr_t>(pointer); }
} // namespace

// ---- pure helpers -------------------------------------------------------------------------------------------------------
RemapFormat ClassifyRemapFormat(DXGI_FORMAT view)
{
    // The same view format for the render target view and the srv of the copy; the 8888 twins get a typeless parent and an sRGB twin of
    // the srv, nothing else does. Float and SNORM are the classes whose values may be scaled; UNORM is not (it would clip).
    const auto plain = [view](bool scalable) { return RemapFormat { true, scalable, view, view, view, DXGI_FORMAT_UNKNOWN }; };
    const auto withTwin = [view](DXGI_FORMAT typeless, DXGI_FORMAT srgb) { return RemapFormat { true, false, typeless, view, view, srgb }; };
    switch (view)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return withTwin(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return withTwin(DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
        return plain(false);
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8_SNORM:
        return plain(true);
    default:
        return RemapFormat {};
    }
}

RemapStatus CheckRemap(const ViewDesc& source, const RemapRequest& request)
{
    if (!source.ok || !source.plain2D)
        return RemapStatus::NotPlain2D;
    const RemapFormat format = ClassifyRemapFormat(static_cast<DXGI_FORMAT>(source.format));
    if (!format.supported)
        return RemapStatus::UnsupportedFormat;
    // Vector values cannot be scaled in UNORM (they would clip): such a view is left to the add-on.
    if (request.buffer == GBuffer::MotionVectors && request.scaleMotion && !format.valueScalable)
        return RemapStatus::UnsupportedFormat;
    const auto edge = [](uint32_t v) { return v >= 1 && v <= kMaxEdge; };
    if (!edge(request.outputW) || !edge(request.outputH) || !edge(request.renderW) || !edge(request.renderH) || !edge(source.width) ||
        !edge(source.height))
        return RemapStatus::Refused;
    // origin 0,0: the render window lies inside the source when it is not larger than it, and it is never larger than the output
    if (request.renderW > source.width || request.renderH > source.height || request.renderW > request.outputW ||
        request.renderH > request.outputH)
        return RemapStatus::Refused;
    return RemapStatus::Ok;
}

// The product build is /fp:fast; the multiplier must be the correctly rounded quotient (2.0 at 3840 over 1920 exactly), not a reciprocal.
#pragma float_control(precise, on, push)
ValueMultiplier MotionMultiplier(GBuffer buffer, bool scaleMotion, uint32_t outputW, uint32_t outputH, uint32_t renderW, uint32_t renderH,
                                 float mvScaleX, float mvScaleY)
{
    ValueMultiplier m;
    if (buffer != GBuffer::MotionVectors || !scaleMotion)
        return m;
    // DERIVED, not yet confirmed in game. Under DLSS's convention the stored vector times the scale the game reports is a displacement in
    // RENDER pixels, so the screen-UV multiplier is that scale / render size per axis (a game that reports a scale equal to the render
    // size gets exactly 1). Without a usable reported scale (0, negative, NaN, infinite) the vectors are taken to be in render pixels or a
    // fraction of the render frame and the multiplier is output / render. A render size of 0 is not divided by (the request check
    // refuses it before any draw).
    const auto usable = [](float scale) { return std::isfinite(scale) && scale > 0.0f; };
    if (renderW != 0)
        m.x = (usable(mvScaleX) ? mvScaleX : static_cast<float>(outputW)) / static_cast<float>(renderW);
    if (renderH != 0)
        m.y = (usable(mvScaleY) ? mvScaleY : static_cast<float>(outputH)) / static_cast<float>(renderH);
    return m;
}

#pragma float_control(pop)

RemapConstants MakeRemapConstants(const RemapRequest& request)
{
    RemapConstants k;
    k.render = PackPair(request.renderW, request.renderH);
    k.output = PackPair(request.outputW, request.outputH);
    const ValueMultiplier m = MotionMultiplier(request.buffer, request.scaleMotion, request.outputW, request.outputH, request.renderW, request.renderH,
                                            request.motionScaleX, request.motionScaleY);
    k.mulX = m.x;
    k.mulY = m.y;
    return k;
}

bool IsPermanentCreateFailure(HRESULT hr)
{
    // "This format / size cannot be a render target here" answers: they come again for the same request. Everything else - out of memory,
    // a removed or reset device, a driver error, E_FAIL, anything unknown - may pass, so the next call tries again.
    switch (hr)
    {
    case E_INVALIDARG:
    case E_NOTIMPL:
    case DXGI_ERROR_UNSUPPORTED:
    case DXGI_ERROR_INVALID_CALL:
        return true;
    default:
        return false;
    }
}

bool RemapDeviceReplaced(const void* owned, const void* given, HRESULT removedReason)
{
    return owned == nullptr || owned != given || FAILED(removedReason);
}

void MarkRemapDeviceFailed(ID3D11Device* device)
{
    const uint32_t value = 1;
    device->SetPrivateData(kInitFailedTag, sizeof(value), &value);
}

bool RemapDeviceMarkedFailed(ID3D11Device* device)
{
    UINT size = sizeof(uint32_t);
    uint32_t value = 0;
    return SUCCEEDED(device->GetPrivateData(kInitFailedTag, &size, &value)) && value == 1;
}

// ---- the add-on's view ---------------------------------------------------------------------------------------------------
// The objects with destructors live in the job (in the frame of the caller), not in the frame of the guarded step, which an access
// violation unwinds without running destructors.
namespace
{
struct DescribeJob
{
    ID3D11ShaderResourceView* view = nullptr;
    bool read = false; // the description of the view could be read
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
    D3D11_TEXTURE2D_DESC textureDesc {};
    bool isTexture = false;
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11Device> device;
};

void DescribeThunk(void* p)
{
    DescribeJob& j = *static_cast<DescribeJob*>(p);
    j.view->GetDesc(&j.viewDesc);
    j.read = true;
    j.view->GetResource(&j.resource);
    if (j.resource == nullptr)
        return;
    j.resource->GetDevice(&j.device);
    if (FAILED(j.resource->QueryInterface(IID_PPV_ARGS(&j.texture))) || j.texture == nullptr)
        return;
    j.texture->GetDesc(&j.textureDesc);
    j.isTexture = true;
}

struct ContextProbe
{
    ID3D11DeviceContext* context = nullptr;
    bool immediate = false;
};

void ContextTypeThunk(void* p)
{
    ContextProbe& j = *static_cast<ContextProbe*>(p);
    j.immediate = j.context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
}
} // namespace

bool ReadViewDesc(ID3D11ShaderResourceView* view, ViewDesc& out) noexcept
{
    out = ViewDesc {};
    if (view == nullptr)
        return false;
    try
    {
        DescribeJob j;
        j.view = view;
        if (d3d11::GuardedCall(&DescribeThunk, &j) != 0 || !j.read)
            return false;
        out.ok = true;
        out.format = static_cast<uint32_t>(j.viewDesc.Format); // the format of the VIEW (the resource may be typeless)
        out.device = ValueOf(j.device.Get());                  // information only
        if (j.isTexture)
        {
            out.width = j.textureDesc.Width;
            out.height = j.textureDesc.Height;
            out.plain2D = j.viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && j.viewDesc.Texture2D.MostDetailedMip == 0 &&
                          j.textureDesc.ArraySize == 1 && j.textureDesc.SampleDesc.Count == 1;
        }
        return true;
    }
    catch (...)
    {
        out = ViewDesc {};
        return false;
    }
}

// ---- one remap -----------------------------------------------------------------------------------------------------------
// Everything a guarded step reads or writes. The steps are plain functions on raw pointers and members of this struct: the objects
// with destructors live here (in the frame of RemapImpl), not in the frames of the steps, which an access violation unwinds without
// running destructors (a step only makes trivial locals; logging in InitThunk aside).
struct GBufferRemap::Job
{
    GBufferRemap* self = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11ShaderResourceView* srv = nullptr; // the add-on's view
    const RemapRequest* request = nullptr;
    RemapFormat format;                      // what the pass does with the view's format
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;

    // ProbeThunk
    bool immediate = false;
    bool initFailed = false;  // the device carries the tag of a failed set-up
    bool sameAsStuck = false; // the context is the one a swap back is outstanding for
    HRESULT removed = S_OK;
    ComPtr<ID3D11Device> device;
    ComPtr<IUnknown> contextIdentity, stuckIdentity;
    // PrepareThunk
    bool prepared = false;
    bool targetFailed = false; // a resource of the target could not be created (not: a fault) - remembered, see _targetFailure
    Target fresh; // the new target when the output size or the format changed
    // DrawThunk, UnbindThunk, SwapBackThunk, ScrubThunk
    bool drawn = false; // the draw was started: the target may be half written
    bool drawFailed = false;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3DDeviceContextState> previous;  // the game's state, non-null once it has been swapped out
    ComPtr<ID3DDeviceContextState> displaced; // the state the swap back replaced (the private one, unless the context was elsewhere)
};

struct GBufferRemap::InitJob
{
    GBufferRemap* self = nullptr;
    ID3D11Device* device = nullptr;
    ComPtr<ID3D11Device1> device1;
    bool ok = false;
};

GBufferRemap::GBufferRemap(GBuffer kind, DrawTriangleFn draw) : _kind(kind), _draw(draw) {}

GBufferRemap::~GBufferRemap()
{
    Release();
    // The context may still be in the private state, and the game's state object is then reachable only through this reference: the
    // destructor must not be the one that destroys it.
    if (_stuckPrevious != nullptr)
        DropStuck("the pass is being destroyed");
}

void GBufferRemap::Teardown()
{
    _key = Key {};
    _targetFailure = TargetFailure {};
    _target = Target {};
    _constants.Reset();
    _ps.Reset();
    _vs.Reset();
    _state.Reset();
    _device.Reset();
}

void GBufferRemap::Release()
{
    RecoverState();
    Teardown();
}

void GBufferRemap::InitThunk(void* p)
{
    InitJob& j = *static_cast<InitJob*>(p);
    GBufferRemap& s = *j.self;
    ID3D11Device* device = j.device;

    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&j.device1))))
    {
        LOG_ERROR("Automatic ReShade G-buffers: the device has no ID3D11Device1 (private context state unavailable)");
        return;
    }

    // The state agrees with the threading mode of the device (as the depth pass and the character preview passes do).
    static_assert(ext::ffxiv::chara::kDeviceCreateSingleThreaded == D3D11_CREATE_DEVICE_SINGLETHREADED);
    static_assert(ext::ffxiv::chara::kContextStateSingleThreaded == D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED);
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(j.device1->CreateDeviceContextState(ext::ffxiv::chara::ContextStateFlags(device->GetCreationFlags()), levels, 1,
                                                 D3D11_SDK_VERSION, __uuidof(ID3D11Device1), &chosen, &s._state)) ||
        s._state == nullptr)
    {
        LOG_ERROR("Automatic ReShade G-buffers: the private context state could not be created");
        return;
    }
    if (FAILED(device->CreateVertexShader(GBufferRemap_Vs_cso, sizeof(GBufferRemap_Vs_cso), nullptr, &s._vs)) ||
        FAILED(device->CreatePixelShader(GBufferRemap_Ps_cso, sizeof(GBufferRemap_Ps_cso), nullptr, &s._ps)))
    {
        LOG_ERROR("Automatic ReShade G-buffers: the shaders could not be created");
        return;
    }

    D3D11_BUFFER_DESC bd {};
    bd.ByteWidth = sizeof(RemapConstants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &s._constants)))
    {
        LOG_ERROR("Automatic ReShade G-buffers: the constant buffer could not be created");
        return;
    }
    s._device = device;
    j.ok = true;
}

bool GBufferRemap::Init(ID3D11Device* device)
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

void GBufferRemap::ProbeThunk(void* p)
{
    Job& j = *static_cast<Job*>(p);
    j.immediate = j.context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE; // first: a deferred context gets nothing else
    if (!j.immediate)
        return;
    j.context->GetDevice(&j.device);
    if (j.device != nullptr)
    {
        j.removed = j.device->GetDeviceRemovedReason();
        if (j.device.Get() != j.self->_device.Get()) // only a device we have not set up can carry the tag
            j.initFailed = RemapDeviceMarkedFailed(j.device.Get());
    }
    if (j.self->_stuckContext != nullptr) // the same object, compared by its IUnknown identity
        j.sameAsStuck = SUCCEEDED(j.context->QueryInterface(IID_PPV_ARGS(&j.contextIdentity))) &&
                        SUCCEEDED(j.self->_stuckContext->QueryInterface(IID_PPV_ARGS(&j.stuckIdentity))) &&
                        j.contextIdentity.Get() == j.stuckIdentity.Get();
}

void GBufferRemap::PrepareThunk(void* p)
{
    Job& j = *static_cast<Job*>(p);
    GBufferRemap& s = *j.self;
    const RemapRequest& a = *j.request;
    j.prepared = false;

    // the target is kept until the output size or the format of the add-on's view changes
    if (s._target.texture != nullptr && s._target.width == a.outputW && s._target.height == a.outputH && s._target.format == j.viewFormat)
    {
        j.prepared = true;
        return;
    }
    D3D11_TEXTURE2D_DESC td {};
    td.Width = a.outputW;
    td.Height = a.outputH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = j.format.resourceFormat;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC rd {};
    rd.Format = j.format.rtvFormat;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd {};
    sd.Format = j.format.srvFormat;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MostDetailedMip = 0;
    sd.Texture2D.MipLevels = 1;
    HRESULT hr = s._device->CreateTexture2D(&td, nullptr, &j.fresh.texture);
    if (SUCCEEDED(hr))
        hr = s._device->CreateRenderTargetView(j.fresh.texture.Get(), &rd, &j.fresh.rtv);
    if (SUCCEEDED(hr))
        hr = s._device->CreateShaderResourceView(j.fresh.texture.Get(), &sd, &j.fresh.srv);
    if (SUCCEEDED(hr) && j.format.srgbTwin != DXGI_FORMAT_UNKNOWN)
    {
        sd.Format = j.format.srgbTwin;
        hr = s._device->CreateShaderResourceView(j.fresh.texture.Get(), &sd, &j.fresh.srvSrgb);
    }
    if (FAILED(hr))
    {
        j.fresh = Target {};
        j.targetFailed = IsPermanentCreateFailure(hr); // only an answer that will not change is remembered (see _targetFailure)
        return;
    }
    j.fresh.width = a.outputW;
    j.fresh.height = a.outputH;
    j.fresh.format = j.viewFormat;
    j.prepared = true;
}

void GBufferRemap::DrawThunk(void* p)
{
    Job& j = *static_cast<Job*>(p);
    GBufferRemap& s = *j.self;
    const RemapRequest& a = *j.request;
    ID3D11DeviceContext* c = j.context;

    if (FAILED(c->QueryInterface(IID_PPV_ARGS(&j.context1))))
    {
        j.drawFailed = true;
        return;
    }
    j.context1->SwapDeviceContextState(s._state.Get(), &j.previous);
    if (j.previous == nullptr || j.previous == s._state)
    {
        // The game's state could not be captured (or the context was in the private state already, after a swap whose answer was
        // lost): there is nothing to draw in. Never draw there, and never count it as the game's state.
        j.drawFailed = true;
        return;
    }

    const Target& target = j.fresh.texture != nullptr ? j.fresh : s._target;
    ID3D11Buffer* constants = s._constants.Get();
    ID3D11ShaderResourceView* srv = j.srv;
    ID3D11RenderTargetView* rtv = target.rtv.Get();
    const D3D11_VIEWPORT viewport { 0.0f, 0.0f, static_cast<float>(a.outputW), static_cast<float>(a.outputH), 0.0f, 1.0f };

    // Belt and braces: every slot this pass uses is set below, so a test cannot tell this from its absence.
    c->ClearState();
    // render size, output size and the value multiplier, the only things that follow the quality mode. Written here, in the private
    // state: the game's predication would skip an UpdateSubresource in its own state, and a fresh buffer is all zeros.
    const RemapConstants k = MakeRemapConstants(a);
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

void GBufferRemap::UnbindThunk(void* p)
{
    Job& j = *static_cast<Job*>(p);
    if (j.previous == nullptr)
        return; // never left the game's state
    // Leave no view of the add-on's G-buffer or of our target bound in the private state. Explicit unbinds, not ClearState: if one of
    // them fails, the swap back (its own step) still runs.
    ID3D11DeviceContext* c = j.context;
    ID3D11ShaderResourceView* noView[1] = {};
    c->OMSetRenderTargets(0, nullptr, nullptr);
    c->PSSetShaderResources(0, 1, noView);
}

void GBufferRemap::SwapBackThunk(void* p)
{
    Job& j = *static_cast<Job*>(p);
    if (j.previous == nullptr || j.context1 == nullptr)
        return;
    j.displaced.Reset();
    j.context1->SwapDeviceContextState(j.previous.Get(), &j.displaced);
}

// Enters the private state and clears it: what the game bound there while the context was stuck in it must not stay alive until the
// next remap. Best effort; the swap back that follows does not depend on it.
void GBufferRemap::ScrubThunk(void* p)
{
    Job& j = *static_cast<Job*>(p);
    GBufferRemap& s = *j.self;
    if (j.context1 == nullptr || s._state == nullptr)
        return;
    j.context1->SwapDeviceContextState(s._state.Get(), nullptr);
    j.context1->ClearState();
}

// The context-state lock stayed busy for kContextStateLockTimeoutMs: the pass was skipped before any state swap. Counted for tests, and
// logged at most once a minute per pass (a holder that does not come back would otherwise log every frame).
void GBufferRemap::NoteLockTimeout()
{
    ++_lockTimeouts;
    const uint64_t now = GetTickCount64();
    if (_lockLogged && now - _lockLogTick < 60'000)
        return;
    _lockLogged = true;
    _lockLogTick = now;
    LOG_WARN("Automatic ReShade G-buffers ({}): context state lock busy for {} ms: pass skipped", KindName(_kind), d3d11::kContextStateLockTimeoutMs);
}

// A swap back that failed (it may or may not have happened) is done again before anything else, with the game's own state object,
// which is kept alive until then. If the context really was in the private state, that state is cleared (the swap back is done, then
// the private state entered, cleared and left again). A failed attempt is counted; the last one gives the state up.
bool GBufferRemap::RecoverState()
{
    if (_stuckPrevious == nullptr)
        return true;
    ext::core::BypassScope bypass;
    // The swaps below are a swap-in sequence like the remap's own: no other pass may be inside the private state meanwhile (recursive, so
    // a Release that is called under the lock, or by a remap that holds it, is fine). A lock that stays busy skips the recovery BEFORE
    // anything is swapped: nothing was attempted, so it is not a failed attempt (the next call tries again).
    std::unique_lock<std::recursive_timed_mutex> lock(d3d11::ContextStateMutex(), std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(d3d11::kContextStateLockTimeoutMs)))
    {
        NoteLockTimeout();
        return false;
    }
    Job job;
    job.self = this;
    job.context1 = _stuckContext;
    job.previous = _stuckPrevious;
    bool done = d3d11::GuardedCall(&SwapBackThunk, &job) == 0;
    if (done && job.displaced != nullptr && job.displaced == _state)
    {
        (void)d3d11::GuardedCall(&ScrubThunk, &job);
        done = d3d11::GuardedCall(&SwapBackThunk, &job) == 0; // the context may be in the private state again
    }
    if (!done)
    {
        if (++_stuckAttempts >= kMaxRecoverAttempts)
            DropStuck("the swap back failed again");
        return false;
    }
    _stuckPrevious.Reset();
    _stuckContext.Reset();
    _stuckAttempts = 0;
    return true;
}

// The context may be in the private state for good. The game's state object must outlive that, so the reference is leaked instead of
// released; the context reference is only an extra one and goes.
void GBufferRemap::DropStuck([[maybe_unused]] const char* why)
{
    LOG_ERROR("Automatic ReShade G-buffers ({}): giving up on putting the game's context state back ({}); its state object is kept alive on purpose",
              KindName(_kind), why);
    (void)_stuckPrevious.Detach();
    _stuckContext.Reset();
    _stuckAttempts = 0;
}

void GBufferRemap::Fill(const Target& target, RemapOut& out)
{
    out.srv = ValueOf(target.srv.Get());
    out.srvSrgb = target.srvSrgb != nullptr ? ValueOf(target.srvSrgb.Get()) : out.srv; // no twin: the same view twice
    out.format = static_cast<uint32_t>(target.format);                                // the srv's format is the add-on's view format
}

RemapStatus GBufferRemap::RemapImpl(ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv, const RemapRequest& request, RemapOut& out,
                                    RemapReport& report)
{
    // Our own D3D11 calls (the target, the constants, the draw) must not look like game work to the draw hooks.
    ext::core::BypassScope bypass;

    if (context == nullptr)
        return RemapStatus::NoContext;
    if (request.buffer != _kind) // the kind is the pass's own: it keeps one target per kind and its cache key has no buffer in it
        return RemapStatus::Refused;

    Job job;
    job.self = this;
    job.context = context;
    job.srv = srv;
    job.request = &request;
    if (d3d11::GuardedCall(&ProbeThunk, &job) != 0)
        return RemapStatus::Failed;
    if (!job.immediate)
        return RemapStatus::NoContext;
    if (job.device == nullptr)
        return RemapStatus::Failed;
    // A swap back that failed on an earlier call comes before anything else that touches the context, but only for the context it is
    // outstanding for: a deferred context was refused above, and another context or a removed device is no reason to wait for it.
    if (_stuckPrevious != nullptr)
    {
        if (FAILED(job.removed) || !job.sameAsStuck)
            DropStuck(FAILED(job.removed) ? "the device was removed" : "the call is for another context");
        else if (!RecoverState())
            return RemapStatus::Failed;
    }

    // The same key as the last successful remap, on the live target: the same views, nothing drawn, not even a state swap.
    if (_key.valid && _target.texture != nullptr && SUCCEEDED(job.removed) && job.device.Get() == _device.Get() && _key.srv == srv &&
        _key.stamp == request.stamp && _key.renderW == request.renderW && _key.renderH == request.renderH && _key.outputW == request.outputW &&
        _key.outputH == request.outputH && _key.scaleMotion == request.scaleMotion &&
        _key.motionScaleX == std::bit_cast<uint32_t>(request.motionScaleX) && _key.motionScaleY == std::bit_cast<uint32_t>(request.motionScaleY))
    {
        Fill(_target, out);
        out.cached = true;
        report.cached = true;
        return RemapStatus::Ok;
    }

    // The request is checked before the device: a refused request on a removed device changes nothing.
    if (srv == nullptr)
        return RemapStatus::NotPlain2D;
    if (!ReadViewDesc(srv, report.source))
        return RemapStatus::Failed;
    if (const RemapStatus refusal = CheckRemap(report.source, request); refusal != RemapStatus::Ok)
        return refusal;
    job.viewFormat = static_cast<DXGI_FORMAT>(report.source.format);
    job.format = ClassifyRemapFormat(job.viewFormat);

    // another device (or a removed one): everything goes and is set up again
    if (RemapDeviceReplaced(_device.Get(), job.device.Get(), job.removed))
    {
        if (job.initFailed) // given up on: the pass that is running (for another device) is left alone
            return RemapStatus::Failed;
        Release();
        if (FAILED(job.removed))
            return RemapStatus::Failed;
        if (!Init(job.device.Get()))
        {
            MarkRemapDeviceFailed(job.device.Get());
            return RemapStatus::Failed;
        }
    }

    // From here on the target may be written: whatever happens, this key is not the live one any more.
    _key = Key {};
    // A target that could not be made for this view format and output size is not tried again every frame: the GPU will refuse it
    // again. Only the failure to create the target's resources is remembered, never a fault or a failure of the source side.
    if (_targetFailure.valid && _targetFailure.format == job.viewFormat && _targetFailure.width == request.outputW &&
        _targetFailure.height == request.outputH)
        return RemapStatus::Failed;
    if (d3d11::GuardedCall(&PrepareThunk, &job) != 0)
        return RemapStatus::Failed;
    if (!job.prepared)
    {
        if (job.targetFailed)
            _targetFailure = TargetFailure { true, job.viewFormat, request.outputW, request.outputH };
        return RemapStatus::Failed;
    }
    _targetFailure = TargetFailure {}; // (a record is for one format and size; this one works)

    // The draw runs in the private state. The way back is its own guarded step, so a failed draw cannot skip it.
    // The whole swap-in .. draw .. unbind .. swap-back sequence is one critical section process-wide (d3d11::ContextStateMutex): begin_effects
    // can run on two paths (the HudLess UI-boundary render_effects call and ReShade's own pass in Present) next to the depth pass on the
    // evaluate thread, and an interleaved sequence would put the wrong state back. Nothing else is under the lock: not the target's
    // creation above, not the publishing below, and a cache hit (which swaps nothing) never takes it. The lock is timed: a holder that
    // does not come back (a third-party callback we cannot audit) skips this remap BEFORE the state swap, so nothing is half done; the
    // add-on's binding is kept and the cache key stays unset.
    uint32_t drawCode = 0, unbindCode = 0, swapCode = 0;
    {
        std::unique_lock<std::recursive_timed_mutex> lock(d3d11::ContextStateMutex(), std::defer_lock);
        if (!lock.try_lock_for(std::chrono::milliseconds(d3d11::kContextStateLockTimeoutMs)))
        {
            NoteLockTimeout();
            return RemapStatus::Failed;
        }
        drawCode = d3d11::GuardedCall(&DrawThunk, &job);
        unbindCode = d3d11::GuardedCall(&UnbindThunk, &job);
        swapCode = d3d11::GuardedCall(&SwapBackThunk, &job);
        if (swapCode != 0)
        {
            // It may not have happened: the next call (or Release) does it again, and the game's state object must not die before.
            _stuckContext = job.context1;
            _stuckPrevious = job.previous;
        }
    }
    if (drawCode != 0 || unbindCode != 0 || swapCode != 0 || job.drawFailed)
        return RemapStatus::Failed; // the new target (if any) is discarded with the job, the old one is kept

    // The new target is drawn and ready: the old one (if the output size or the format changed) is released only now.
    if (job.fresh.texture != nullptr)
    {
        _target = std::move(job.fresh);
        report.targetCreated = true;
    }
    _key.valid = true;
    _key.srv = srv;
    _key.stamp = request.stamp;
    _key.renderW = request.renderW;
    _key.renderH = request.renderH;
    _key.outputW = request.outputW;
    _key.outputH = request.outputH;
    _key.scaleMotion = request.scaleMotion;
    _key.motionScaleX = std::bit_cast<uint32_t>(request.motionScaleX);
    _key.motionScaleY = std::bit_cast<uint32_t>(request.motionScaleY);
    Fill(_target, out);
    return RemapStatus::Ok;
}

RemapStatus GBufferRemap::Remap(ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv, [[maybe_unused]] ID3D11ShaderResourceView* srvSrgb,
                                const RemapRequest& request, RemapOut& out, RemapReport* report) noexcept
{
    RemapReport local;
    RemapReport& r = report != nullptr ? *report : local;
    r = RemapReport {};
    out = RemapOut {};
    try
    {
        const RemapStatus status = RemapImpl(context, srv, request, out, r);
        r.status = status;
        if (status != RemapStatus::Ok)
            out = RemapOut {};
        return status;
    }
    catch (...)
    {
        r.status = RemapStatus::Failed;
        out = RemapOut {};
        return RemapStatus::Failed;
    }
}

// ---- the engine ----------------------------------------------------------------------------------------------------------
LiveRemapEngine::LiveRemapEngine(DrawTriangleFn rawDraw, HookedContextFn hookedContext) : _rawDraw(rawDraw), _hookedContext(hookedContext) {}

LiveRemapEngine::~LiveRemapEngine() = default; // each pass releases itself (and keeps a stuck game state alive on purpose)

bool LiveRemapEngine::DescribeView(uint64_t view, ViewDesc& out) noexcept
{
    return ReadViewDesc(ViewOf(view), out);
}

RemapStatus LiveRemapEngine::Remap(const RemapRequest& request, RemapOut& out) noexcept
{
    out = RemapOut {};
    try
    {
        if (request.context == 0)
            return RemapStatus::NoContext;
        const size_t index = static_cast<size_t>(request.buffer);
        if (index >= kGBuffers)
            return RemapStatus::Refused;
        ID3D11DeviceContext* context = reinterpret_cast<ID3D11DeviceContext*>(static_cast<uintptr_t>(request.context));
        // The raw draw only for the context the lighting hooks hooked; the pass takes its draw at construction, so a change of the
        // decision makes a new pass (it happens when the hooks come or go, not per frame).
        const bool raw = _rawDraw != nullptr && _hookedContext != nullptr && glue::UseRawDraw(_hookedContext(), context);
        Slot& slot = _slots[index];
        if (slot.pass == nullptr || slot.rawDraw != raw)
        {
            // Only a real immediate context may replace the live pass. A deferred or garbage context makes the raw-draw decision
            // differ too, and is answered here: the pass (its target, its cache, a swap back it may still owe) is left alone.
            ContextProbe probe;
            probe.context = context;
            if (d3d11::GuardedCall(&ContextTypeThunk, &probe) != 0)
                return RemapStatus::Failed;
            if (!probe.immediate)
                return RemapStatus::NoContext;
            slot.pass = std::make_unique<GBufferRemap>(request.buffer, raw ? _rawDraw : nullptr); // (the old pass, if any, goes first)
            slot.rawDraw = raw;
        }
        const RemapStatus status = slot.pass->Remap(context, ViewOf(request.srv), ViewOf(request.srvSrgb), request, out);
        if (status != RemapStatus::Ok)
            out = RemapOut {};
        return status;
    }
    catch (...)
    {
        out = RemapOut {};
        return RemapStatus::Failed;
    }
}

void LiveRemapEngine::Release() noexcept
{
    for (Slot& slot : _slots)
    {
        if (slot.pass == nullptr)
            continue;
        try
        {
            slot.pass->Release();
        }
        catch (...)
        {
        }
    }
}
} // namespace ext::reshade::depth
