#include "pch.h"

#include "ext/ffxiv/shadows/ShadowsPass.h"
#include "ext/core/D3D11ExplicitState.h"

#include <ShadowLift_Ps.h>
#include <ShadowLift_Vs.h>

#include <cstring>

// Shadow mapping (ext/ffxiv/shadows): the mask lift's own pass (the device side). The register slots and the
// constant-buffer layout of ext/ffxiv/shadows/shaders/ShadowLift.hlsl are mirrored here; change them together.
namespace ext::ffxiv::shadows
{
namespace
{
using Microsoft::WRL::ComPtr;

// g_CameraParameter's layout is not read here (the contact-shadows march owns that); the module's own draw only
// reads the mask's texels.
constexpr D3D11_TEXTURE2D_DESC CopyDescOf(uint32_t typedFormat, uint32_t width, uint32_t height)
{
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = static_cast<DXGI_FORMAT>(typedFormat);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return desc;
}

// The constant buffer of the shaders: the same two float4 rows of ext/ffxiv/shadows/shaders/ShadowLift.hlsl.
struct ParamsBuffer
{
    float window[4]; // xy = the render window's origin in mask pixels, zw = the mask's size
    float lift[4];   // x = the effective lift (0 = identity), y = the tent's reach in mask pixels, zw = 0
};
static_assert(sizeof(ParamsBuffer) == 32, "the constant buffer of the shaders is two float4s");

// What this pass touches on the game's context, in the Spec's fields (psSrvCount, psSamplerCount, psConstantBuffers): the IA layout and
// topology, VS and PS, pixel-shader resource slot 0 (the mask) and constant buffer 0, the rasterizer state and the viewports, blend,
// depth-stencil, and the render targets with the UAV slots. HS, DS, GS, stream-out and predication are what the private device-context state
// used to provide as "none". No sampler is touched: the shader only Loads. Scissor rects and the other constant buffers are not read.
constexpr ext::core::explicit_state::Spec kSpec { 1, 0, 1 };
} // namespace

void MaskLiftPass::Reset()
{
    _copySrv.Reset();
    _copyRtv.Reset();
    _copy.Reset();
    _params.Reset();
    _rasterizer.Reset();
    _ps.Reset();
    _vs.Reset();
    _device.Reset();
    _copyFormat = _copyWidth = _copyHeight = 0;
    _failure = nullptr;
    _copiesCreated = 0;
    _disjoint.Reset();
    _begin.Reset();
    _end.Reset();
    _timingPending = false;
    _haveGpuMs = false;
    _gpuMs = 0.0;
}

bool MaskLiftPass::Init(ID3D11Device* device)
{
    if (FAILED(device->CreateVertexShader(ShadowLift_Vs_cso, sizeof ShadowLift_Vs_cso, nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(ShadowLift_Ps_cso, sizeof ShadowLift_Ps_cso, nullptr, &_ps)))
    {
        _failure = "the shadow lifting shaders could not be created";
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

    // No private pipeline state: the game's own is captured and put back explicitly around the one draw (ext/core/D3D11ExplicitState.h).
    // The one draw's GPU time: a timestamp pair inside a disjoint query, read with the no-flush flag a run or two
    // later, never waited for (the AoPass/CbPatch/SsrPass idiom the FRESH wave fixed across the fork's passes).
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

bool MaskLiftPass::EnsureCopy(ID3D11Device* device, uint32_t format, uint32_t width, uint32_t height)
{
    if (_copy && _copyFormat == format && _copyWidth == width && _copyHeight == height)
        return true;
    _copySrv.Reset();
    _copyRtv.Reset();
    _copy.Reset();
    _copyFormat = _copyWidth = _copyHeight = 0;

    // The copy is built with the typed member of the mask's format family: the engine creates the mask's texture as
    // R8_TYPELESS and the draw samples an R8_UNORM view of it, and D3D11 refuses to create a render target (or any
    // view) with a typeless format. The typed member is also the only format an SRV of the copy may be built with
    // (the light draw requires the paircheck's typed format, ContactShadowsRules.h).
    const uint32_t typedFormat = relight::TypedMaskFormat(format);
    const D3D11_TEXTURE2D_DESC desc = CopyDescOf(typedFormat, width, height);
    D3D11_RENDER_TARGET_VIEW_DESC rtv {};
    rtv.Format = static_cast<DXGI_FORMAT>(typedFormat);
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = static_cast<DXGI_FORMAT>(typedFormat);
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

void MaskLiftPass::CollectGpuTiming(ID3D11DeviceContext* context)
{
    if (!_timingPending || !_disjoint || !_begin || !_end)
        return;
    // D3D11_ASYNC_GETDATA_DONOTFLUSH on every read: a GetData without it can flush the game's own command queue to
    // resolve the query, and this runs on the game's own immediate context a frame or two later (AoPass.cpp's rule).
    constexpr UINT kGetDataFlags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint {};
    if (context->GetData(_disjoint.Get(), &disjoint, sizeof disjoint, kGetDataFlags) != S_OK)
        return; // not finished: looked at again on the next run, never waited for
    UINT64 begin = 0, end = 0;
    const HRESULT haveBegin = _begin ? context->GetData(_begin.Get(), &begin, sizeof begin, kGetDataFlags) : S_FALSE;
    const HRESULT haveEnd = _end ? context->GetData(_end.Get(), &end, sizeof end, kGetDataFlags) : S_FALSE;
    _timingPending = false;
    if (!disjoint.Disjoint && disjoint.Frequency > 0 && haveBegin == S_OK && haveEnd == S_OK && end > begin)
    {
        _gpuMs = double(end - begin) * 1000.0 / double(disjoint.Frequency);
        _haveGpuMs = true;
    }
}

bool MaskLiftPass::BeginGpuTiming(ID3D11DeviceContext* context)
{
    if (!_disjoint || !_begin || !_end || _timingPending)
        return false; // the previous pair is still in the queue: this run is not timed, and it must not end that pair
    context->Begin(_disjoint.Get());
    context->End(_begin.Get());
    _timingPending = true;
    ++_timingBegins;
    return true;
}

ID3D11ShaderResourceView* MaskLiftPass::Run(ID3D11DeviceContext* context, const MaskInputs& inputs)
{
    if (!TryEnter())
        return nullptr; // a second run inside the first (a nested draw hook): the outer one owns the context state
    struct Running
    {
        MaskLiftPass& pass;
        ~Running() { pass.Leave(); } // every path through Run leaves it again, the contact pass's own pattern
    } running { *this };
    // The caller owns the identity: at an identity lift it does not ask (the combine branch or the module's own
    // route checks the lift first). A call that still arrives is answered by the fail-open null - the game's own
    // binding, a decline counted - so an identity lift can never create D3D objects per frame here.
    if (context == nullptr || inputs.mask == nullptr || !(inputs.lift > kIdentityEpsilon))
        return nullptr;
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
    if (!EnsureCopy(device.Get(), inputs.maskFormat, inputs.maskWidth, inputs.maskHeight))
    {
        _failure = "the mask's copy could not be created";
        return nullptr;
    }

    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
    {
        _failure = "ID3D11DeviceContext1 is not available";
        return nullptr;
    }

    // The scalar parameters, one dynamic buffer.
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr)
    {
        _failure = "the parameter buffer could not be mapped";
        return nullptr;
    }
    ParamsBuffer params {};
    params.window[0] = inputs.window[0];
    params.window[1] = inputs.window[1];
    params.window[2] = inputs.window[2];
    params.window[3] = inputs.window[3];
    params.lift[0] = inputs.lift;
    params.lift[1] = inputs.softness;
    std::memcpy(mapped.pData, &params, sizeof params);
    context->Unmap(_params.Get(), 0);

    CollectGpuTiming(context);
    // From here on the game's own state has to come back, also when the draw throws through this fork's hooks: the guard restores it in its
    // destructor on every path, so a throw can never leave the game drawing in the module's own pipeline state.
    ext::core::explicit_state::Guard stateGuard(context, context1.Get(), device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1, kSpec);
    stateGuard.Neutralise(); // what the private state's defaults provided: no HS, DS, GS, stream-out target or predicate

    const float blendFactor[4] {};
    ID3D11Buffer* buffers[1] { _params.Get() };
    ID3D11ShaderResourceView* views[1] { inputs.mask };
    ID3D11RenderTargetView* copyRtv = _copyRtv.Get();
    const D3D11_VIEWPORT viewport { 0.0f, 0.0f, float(inputs.maskWidth), float(inputs.maskHeight), 0.0f, 1.0f };

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(_vs.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, buffers);
    context->RSSetState(_rasterizer.Get());
    context->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);

    // The lift draw: the mask's whole allocation (the window discipline lives in the shader, so pixels outside the
    // render window carry the game's own bytes in the copy, byte for byte).
    const bool timed = BeginGpuTiming(context);
    context->PSSetShaderResources(0, 1, views);
    context->PSSetShader(_ps.Get(), nullptr, 0);
    context->OMSetRenderTargets(1, &copyRtv, nullptr);
    context->RSSetViewports(1, &viewport);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* none[1] {};
    context->PSSetShaderResources(0, 1, none);
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
} // namespace ext::ffxiv::shadows
