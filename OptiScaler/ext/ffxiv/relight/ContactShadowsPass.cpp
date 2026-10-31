#include "pch.h"

#include "ext/ffxiv/relight/ContactShadowsPass.h"

#include <ContactShadows_Combine.h>
#include <ContactShadows_March.h>
#include <ContactShadows_Vs.h>

#include "ext/ffxiv/relight/shaders/CameraRegisters.h"

#include <cstring>

namespace ext::ffxiv::relight
{
using Microsoft::WRL::ComPtr;

namespace
{
// The register slots and the constant-buffer layout of ext/ffxiv/relight/shaders/ContactShadows.hlsl. Change them together.
constexpr uint32_t kSlotDepth = 0;
constexpr uint32_t kSlotCamera = 2;
constexpr uint32_t kSlotSun = 3;
constexpr uint32_t kSlotMask = 4;
constexpr uint32_t kSlotTerm = 5;

// g_CameraParameter's layout, at the offsets the header shared with the shaders has: the view matrix's three rows at +0x00, the
// view-projection at +0x60 and the inverse projection at +0xE0, row-major, four rows of a float4 each.
constexpr uint32_t kCameraBytes = CS_CAMERA_BYTES;
static_assert(CS_CAMERA_VIEW_REGISTER * 16 == CS_CAMERA_VIEW_OFFSET, "the view matrix's register and offset must agree");
static_assert(CS_CAMERA_VIEW_PROJECTION_REGISTER * 16 == CS_CAMERA_VIEW_PROJECTION_OFFSET, "the view-projection's register and offset agree");
static_assert(CS_CAMERA_INVERSE_PROJECTION_REGISTER * 16 == CS_CAMERA_INVERSE_PROJECTION_OFFSET,
              "the inverse projection's register and offset agree");
static_assert(kCameraBytes / 16 == 22, "the shaders read 22 float4 rows of g_CameraParameter");
constexpr uint32_t kSunOffset = 0x10; // g_LightParam +0x10: the direction, a float3 padded to a float4
constexpr uint32_t kSunBytes = 16;

struct ParamsBuffer
{
    float window[4]; // xy = origin in mask pixels, zw = extent in mask pixels
    float march[4];  // x = length, y = steps, z = strength, w = 0 world space / 1 view space
    float sign[4];   // x = +1 toward the sun / -1 the way the light travels, yz = the mask's size
};
static_assert(sizeof(ParamsBuffer) == 48, "the constant buffer of the shaders is three float4s");

// A byte range of a constant buffer as the box a partial copy takes. For a buffer the box is in bytes: left..right, one row, one slice.
D3D11_BOX ByteBox(uint32_t offset, uint32_t bytes)
{
    D3D11_BOX box {};
    box.left = offset;
    box.top = 0;
    box.front = 0;
    box.right = offset + bytes;
    box.bottom = 1;
    box.back = 1;
    return box;
}
} // namespace

void Pass::Reset()
{
    _copySrv.Reset();
    _copyRtv.Reset();
    _copy.Reset();
    _termSrv.Reset();
    _termRtv.Reset();
    _term.Reset();
    _cameraView.Reset();
    _camera.Reset();
    _sunView.Reset();
    _sun.Reset();
    _params.Reset();
    _rasterizer.Reset();
    _combine.Reset();
    _march.Reset();
    _vs.Reset();
    _state.Reset();
    _device.Reset();
    _copyFormat = _copyWidth = _copyHeight = 0;
    _termWidth = _termHeight = 0;
    _failure = nullptr;
    _copiesCreated = _termsCreated = 0;
    _disjoint.Reset();
    _begin.Reset();
    _end.Reset();
    // `_running` stays as it is: a run in progress keeps its guard (this can be reached from that run's own device-change path).
    _timingPending = false;
    _haveGpuMs = false;
    _gpuMs = 0.0;
}

bool Pass::Init(ID3D11Device* device)
{
    ComPtr<ID3D11Device1> device1;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))))
    {
        _failure = "ID3D11Device1 is not available";
        return false;
    }
    if (FAILED(device->CreateVertexShader(ContactShadows_Vs_cso, sizeof ContactShadows_Vs_cso, nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(ContactShadows_March_cso, sizeof ContactShadows_March_cso, nullptr, &_march)) ||
        FAILED(device->CreatePixelShader(ContactShadows_Combine_cso, sizeof ContactShadows_Combine_cso, nullptr, &_combine)))
    {
        _failure = "the contact shadow shaders could not be created";
        return false;
    }

    D3D11_RASTERIZER_DESC raster {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&raster, &_rasterizer)))
    {
        _failure = "the rasterizer state could not be created";
        return false;
    }

    D3D11_BUFFER_DESC params {};
    params.ByteWidth = sizeof(ParamsBuffer);
    params.Usage = D3D11_USAGE_DYNAMIC;
    params.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    params.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&params, nullptr, &_params)))
    {
        _failure = "the parameter buffer could not be created";
        return false;
    }

    // The two buffers the game's constant buffers are copied into, GPU side. A destination this pass reads through a shader resource view
    // rather than a constant buffer: a partial-box copy into a constant buffer is not something every driver accepts.
    D3D11_BUFFER_DESC sun {};
    sun.ByteWidth = kSunBytes;
    sun.Usage = D3D11_USAGE_DEFAULT;
    sun.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sunView {};
    sunView.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    sunView.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sunView.Buffer.FirstElement = 0;
    sunView.Buffer.NumElements = 1;
    D3D11_BUFFER_DESC camera {};
    camera.ByteWidth = kCameraBytes;
    camera.Usage = D3D11_USAGE_DEFAULT;
    camera.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC cameraView {};
    cameraView.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    cameraView.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    cameraView.Buffer.FirstElement = 0;
    cameraView.Buffer.NumElements = kCameraBytes / 16;
    if (FAILED(device->CreateBuffer(&sun, nullptr, &_sun)) || FAILED(device->CreateShaderResourceView(_sun.Get(), &sunView, &_sunView)) ||
        FAILED(device->CreateBuffer(&camera, nullptr, &_camera)) ||
        FAILED(device->CreateShaderResourceView(_camera.Get(), &cameraView, &_cameraView)))
    {
        _failure = "the constant-buffer copies could not be created";
        return false;
    }

    // The private pipeline state: the game's own is captured and put back after the two draws.
    const D3D_FEATURE_LEVEL levels[] { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL chosen {};
    const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    if (FAILED(device1->CreateDeviceContextState(flags, levels, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), &chosen, &_state)) ||
        _state == nullptr)
    {
        _failure = "the private device-context state could not be created";
        return false;
    }

    // The two draws' own GPU time: a timestamp pair inside a disjoint query, read one run later without ever waiting for the GPU.
    D3D11_QUERY_DESC disjointDesc { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    D3D11_QUERY_DESC stampDesc { D3D11_QUERY_TIMESTAMP, 0 };
    if (FAILED(device->CreateQuery(&disjointDesc, &_disjoint)) || FAILED(device->CreateQuery(&stampDesc, &_begin)) ||
        FAILED(device->CreateQuery(&stampDesc, &_end)))
    {
        _disjoint.Reset();
        _begin.Reset();
        _end.Reset();
        _device = device; // no timings, the pass still runs: the measurement is the only thing that is lost
    }
    _device = device;
    return true;
}

void Pass::CollectGpuTiming(ID3D11DeviceContext* context)
{
    if (!_timingPending || !_disjoint || !_begin || !_end)
        return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint {};
    if (context->GetData(_disjoint.Get(), &disjoint, sizeof disjoint, 0) != S_OK)
        return; // not finished: looked at again on the next run, never waited for
    UINT64 begin = 0, end = 0;
    const HRESULT haveBegin = _begin ? context->GetData(_begin.Get(), &begin, sizeof begin, 0) : S_FALSE;
    const HRESULT haveEnd = _end ? context->GetData(_end.Get(), &end, sizeof end, 0) : S_FALSE;
    _timingPending = false;
    if (!disjoint.Disjoint && disjoint.Frequency > 0 && haveBegin == S_OK && haveEnd == S_OK && end > begin)
    {
        _gpuMs = double(end - begin) * 1000.0 / double(disjoint.Frequency);
        _haveGpuMs = true;
    }
}

bool Pass::BeginGpuTiming(ID3D11DeviceContext* context)
{
    if (!_disjoint || !_begin || !_end || _timingPending)
        return false; // the previous pair is still in the queue: this run is not timed, and it must not end that pair either
    context->Begin(_disjoint.Get());
    context->End(_begin.Get());
    _timingPending = true;
    ++_timingBegins;
    return true;
}

bool Pass::TryEnter() noexcept
{
    if (_running)
        return false; // a second run inside the first (a nested draw hook): the outer one owns the context state
    _running = true;
    return true;
}

void Pass::Leave() noexcept { _running = false; }

bool Pass::EnsureCopy(ID3D11Device* device, uint32_t format, uint32_t width, uint32_t height)
{
    if (_copy && _copyFormat == format && _copyWidth == width && _copyHeight == height)
        return true;
    _copySrv.Reset();
    _copyRtv.Reset();
    _copy.Reset();
    _copyFormat = _copyWidth = _copyHeight = 0;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = static_cast<DXGI_FORMAT>(format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC rtv {};
    rtv.Format = static_cast<DXGI_FORMAT>(format);
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = static_cast<DXGI_FORMAT>(format);
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &_copy)) || FAILED(device->CreateRenderTargetView(_copy.Get(), &rtv, &_copyRtv)) ||
        FAILED(device->CreateShaderResourceView(_copy.Get(), &srv, &_copySrv)))
    {
        _copySrv.Reset();
        _copyRtv.Reset();
        _copy.Reset();
        return false;
    }
    _copyFormat = format;
    _copyWidth = width;
    _copyHeight = height;
    ++_copiesCreated;
    return true;
}

bool Pass::EnsureTerm(ID3D11Device* device, uint32_t width, uint32_t height)
{
    if (_term && _termWidth == width && _termHeight == height)
        return true;
    _termSrv.Reset();
    _termRtv.Reset();
    _term.Reset();
    _termWidth = _termHeight = 0;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC rtv {};
    rtv.Format = DXGI_FORMAT_R8_UNORM;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = DXGI_FORMAT_R8_UNORM;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &_term)) || FAILED(device->CreateRenderTargetView(_term.Get(), &rtv, &_termRtv)) ||
        FAILED(device->CreateShaderResourceView(_term.Get(), &srv, &_termSrv)))
    {
        _termSrv.Reset();
        _termRtv.Reset();
        _term.Reset();
        return false;
    }
    _termWidth = width;
    _termHeight = height;
    ++_termsCreated;
    return true;
}

ID3D11ShaderResourceView* Pass::Run(ID3D11DeviceContext* context, const Inputs& inputs)
{
    if (context == nullptr || inputs.mask == nullptr || inputs.depth == nullptr || inputs.lightParam == nullptr ||
        inputs.cameraParam == nullptr)
        return nullptr;
    if (!TryEnter())
        return nullptr; // a second run inside the first (a nested draw hook): the outer one owns the context state
    struct Running
    {
        Pass& pass;
        ~Running() { pass.Leave(); }
    } running { *this };

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (device == nullptr)
        return nullptr;
    if (device.Get() != _device.Get())
    {
        Reset();
        if (!Init(device.Get()))
            return nullptr;
    }

    // The camera buffer has to hold the matrices the shaders read: a smaller binding (another build, another member) is a refusal, not a
    // copy that would read past the end of the buffer.
    D3D11_BUFFER_DESC cameraDesc {};
    inputs.cameraParam->GetDesc(&cameraDesc);
    D3D11_BUFFER_DESC lightDesc {};
    inputs.lightParam->GetDesc(&lightDesc);
    if (cameraDesc.ByteWidth < kCameraBytes || lightDesc.ByteWidth < kSunOffset + kSunBytes)
        return nullptr;

    // The term is the size of half the mask's allocation, which a dynamic-resolution change does not touch: the render window inside the
    // allocation is what moves, and it is a viewport on that texture. Only a real allocation change (a window resize) re-creates it.
    if (!EnsureCopy(device.Get(), inputs.maskFormat, inputs.maskWidth, inputs.maskHeight) ||
        !EnsureTerm(device.Get(), HalfExtent(float(inputs.maskWidth)), HalfExtent(float(inputs.maskHeight))))
    {
        _failure = "the mask copy or the term target could not be created";
        return nullptr;
    }

    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
    {
        _failure = "ID3D11DeviceContext1 is not available";
        return nullptr;
    }

    // The sun direction and the view matrices, GPU side: two small copies, no map, no readback.
    const D3D11_BOX sunBox = ByteBox(kSunOffset, kSunBytes);
    const D3D11_BOX cameraBox = ByteBox(0, kCameraBytes);
    context->CopySubresourceRegion(_sun.Get(), 0, 0, 0, 0, inputs.lightParam, 0, &sunBox);
    context->CopySubresourceRegion(_camera.Get(), 0, 0, 0, 0, inputs.cameraParam, 0, &cameraBox);

    // The scalar parameters, one dynamic buffer.
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr)
        return nullptr;
    ParamsBuffer params {};
    params.window[0] = inputs.window.x;
    params.window[1] = inputs.window.y;
    params.window[2] = inputs.window.width;
    params.window[3] = inputs.window.height;
    params.march[0] = inputs.params.length;
    params.march[1] = float(inputs.params.steps);
    params.march[2] = inputs.params.strength;
    params.march[3] = inputs.params.space == SunSpace::View ? 1.0f : 0.0f;
    params.sign[0] = inputs.params.sign == SunSign::Toward ? 1.0f : -1.0f;
    params.sign[1] = float(inputs.maskWidth);
    params.sign[2] = float(inputs.maskHeight);
    std::memcpy(mapped.pData, &params, sizeof params);
    context->Unmap(_params.Get(), 0);

    CollectGpuTiming(context);
    ComPtr<ID3DDeviceContextState> previous;
    context1->SwapDeviceContextState(_state.Get(), &previous);
    if (previous == nullptr)
    {
        context1->SwapDeviceContextState(_state.Get(), nullptr); // best effort, as the other fork passes do
        _failure = "the previous device-context state was not returned";
        return nullptr;
    }
    // From here on the game's own device-context state has to come back, also when one of the two draws throws through this fork's hooks (a
    // developer diagnostic that allocates, a menu widget): the guard swaps it back on every path, so a throw can never leave the game
    // drawing in this module's private state.
    struct StateRestore
    {
        ID3D11DeviceContext1* context = nullptr;
        ID3DDeviceContextState* previous = nullptr;
        ~StateRestore()
        {
            if (previous != nullptr)
                context->SwapDeviceContextState(previous, nullptr);
        }
    } stateRestore { context1.Get(), previous.Get() };

    const float blendFactor[4] {};
    ID3D11Buffer* buffers[1] { _params.Get() };
    ID3D11SamplerState* samplers[1] {};

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(_vs.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, buffers);
    context->PSSetSamplers(0, 1, samplers);
    context->RSSetState(_rasterizer.Get());
    context->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);

    // The march: the window's half, at the window's own origin in the term texture. The term texture is the render target here, so it is
    // not also bound as a shader resource: D3D11 would drop that binding and the combine below would then read nothing at all.
    ID3D11ShaderResourceView* marchViews[6] {};
    marchViews[kSlotDepth] = inputs.depth;
    marchViews[kSlotCamera] = _cameraView.Get();
    marchViews[kSlotSun] = _sunView.Get();
    ID3D11RenderTargetView* termRtv = _termRtv.Get();
    const uint32_t termWidth = HalfExtent(inputs.window.width);
    const uint32_t termHeight = HalfExtent(inputs.window.height);
    const D3D11_VIEWPORT termViewport { float(uint32_t(inputs.window.x * 0.5f)), float(uint32_t(inputs.window.y * 0.5f)),
                                        float(termWidth), float(termHeight), 0.0f, 1.0f };
    // The pair covers both draws: the march and the combine are one pass. Only the run that began the pair ends it (a run that found the
    // previous pair still in the queue is not timed at all), so no End is ever issued without its Begin.
    const bool timed = BeginGpuTiming(context);
    context->PSSetShaderResources(0, 6, marchViews);
    context->PSSetShader(_march.Get(), nullptr, 0);
    context->OMSetRenderTargets(1, &termRtv, nullptr);
    context->RSSetViewports(1, &termViewport);
    context->Draw(3, 0);

    // The combine: the mask's whole allocation, so every pixel outside the render window is the engine's own value. Its own bind list,
    // because the term is only readable now that it is no longer the render target.
    // The render target is switched first: while the term is still bound as a render target, D3D11 drops a shader-resource view of it, and
    // the combine would then read zero instead of the march's result.
    ID3D11RenderTargetView* copyRtv = _copyRtv.Get();
    const D3D11_VIEWPORT copyViewport { 0.0f, 0.0f, float(inputs.maskWidth), float(inputs.maskHeight), 0.0f, 1.0f };
    context->OMSetRenderTargets(1, &copyRtv, nullptr);
    context->RSSetViewports(1, &copyViewport);
    ID3D11ShaderResourceView* combineViews[6] {};
    combineViews[kSlotDepth] = inputs.depth;
    combineViews[kSlotMask] = inputs.mask;
    combineViews[kSlotTerm] = _termSrv.Get();
    context->PSSetShaderResources(0, 6, combineViews);
    context->PSSetShader(_combine.Get(), nullptr, 0);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* none[6] {};
    context->PSSetShaderResources(0, 6, none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    if (timed && _end && _disjoint)
    {
        context->End(_end.Get());
        context->End(_disjoint.Get());
        ++_timingEnds;
        // `_timingPending` stays set: the pair is in the queue until CollectGpuTiming reads it on a later run.
    }

    return _copySrv.Get();
}
} // namespace ext::ffxiv::relight
