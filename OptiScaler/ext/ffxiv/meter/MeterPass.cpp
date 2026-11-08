#include "pch.h"

#include "ext/ffxiv/meter/MeterPass.h"
#include "ext/core/D3D11ExplicitState.h"

#include <Meter_Vs.h>
#include <Meter_Ps.h>

#include <cstring>

// Effect meter: the pass (MeterPass.h). One draw between an explicit save and restore of the state it touches, one constant buffer mapped
// D3D11_MAP_WRITE_DISCARD, one copy into a staging texture and one event query behind it, all read without ever waiting for the GPU.
namespace ext::ffxiv::meter
{
using Microsoft::WRL::ComPtr;

namespace
{
// What this pass touches on the game's context, in the Spec's fields: pixel-shader resource slots 0 and 1 and constant buffer 0, the IA layout and topology, VS and
// PS, the rasterizer state and the viewports, blend, depth-stencil and the one render target. HS, DS, GS, stream-out and predication are what the private
// device-context state used to provide as "none". No sampler is touched: the shader only Loads.
constexpr ext::core::explicit_state::Spec kSpec { 2, 0, 1 };

// The constant-buffer layout of Meter.hlsl (four 16-byte registers).
struct ParamsBuffer
{
    uint32_t box[4];
    uint32_t geom[4];
    uint32_t mode[4];
    float meas[4];
};
static_assert(sizeof(ParamsBuffer) == 4 * 16, "the pass's constant buffer is four 16-byte registers");

constexpr DXGI_FORMAT kTargetFormat = DXGI_FORMAT_R32G32B32A32_FLOAT; // a render target on every feature level 10 and up; one texel is one block
constexpr uint32_t kTargetSide = kMaxBinsSide;
} // namespace

bool Pass::FormatReadable(uint32_t format) noexcept
{
    switch (DXGI_FORMAT(format))
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return true;
    default:
        return false; // UINT, SINT, SRGB (a Load would linearise what the stage stored), typeless, depth, block-compressed
    }
}

void Pass::Reset()
{
    _vs.Reset();
    _ps.Reset();
    _rasterizer.Reset();
    _params.Reset();
    _targetRtv.Reset();
    _target.Reset();
    _staging.Reset();
    _done.Reset();
    _disjoint.Reset();
    _begin.Reset();
    _mid.Reset();
    _end.Reset();
    _device.Reset();
    _pending = false;
    _timed = false;
    _geometry = Geometry {};
    _boxWidth = _boxHeight = 0;
    _drawsLastStart = 0;
    _failure = nullptr;
}

bool Pass::Init(ID3D11Device* device)
{
    if (device == nullptr)
        return false;
    if (FAILED(device->CreateVertexShader(Meter_Vs_cso, sizeof Meter_Vs_cso, nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(Meter_Ps_cso, sizeof Meter_Ps_cso, nullptr, &_ps)))
    {
        _failure = "the meter shaders could not be created";
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
    D3D11_TEXTURE2D_DESC target {};
    target.Width = kTargetSide;
    target.Height = kTargetSide;
    target.MipLevels = 1;
    target.ArraySize = 1;
    target.Format = kTargetFormat;
    target.SampleDesc.Count = 1;
    target.Usage = D3D11_USAGE_DEFAULT;
    target.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_TEXTURE2D_DESC staging = target;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&target, nullptr, &_target)) || FAILED(device->CreateRenderTargetView(_target.Get(), nullptr, &_targetRtv)) ||
        FAILED(device->CreateTexture2D(&staging, nullptr, &_staging)))
    {
        _failure = "the meter's target could not be created";
        return false;
    }
    D3D11_QUERY_DESC eventDesc { D3D11_QUERY_EVENT, 0 };
    if (FAILED(device->CreateQuery(&eventDesc, &_done)))
    {
        _failure = "the meter's event query could not be created";
        return false;
    }
    // The pass's own GPU time: queries whose reads never wait. No queries: the time is the only thing that is lost.
    D3D11_QUERY_DESC disjointDesc { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    D3D11_QUERY_DESC stampDesc { D3D11_QUERY_TIMESTAMP, 0 };
    if (FAILED(device->CreateQuery(&disjointDesc, &_disjoint)) || FAILED(device->CreateQuery(&stampDesc, &_begin)) ||
        FAILED(device->CreateQuery(&stampDesc, &_mid)) || FAILED(device->CreateQuery(&stampDesc, &_end)))
    {
        _disjoint.Reset();
        _begin.Reset();
        _mid.Reset();
        _end.Reset();
    }
    _device = device;
    return true;
}

bool Pass::Start(ID3D11DeviceContext* context, const Request& request)
{
    _failure = nullptr;
    _drawsLastStart = 0;
    if (context == nullptr || request.in == nullptr || request.out == nullptr)
    {
        _failure = "the meter was handed no view";
        return false;
    }
    if (_pending)
    {
        _failure = "a measurement is still in flight";
        return false;
    }
    if (uint32_t(request.id) >= kEffectCount || !InfoOf(request.id).metered)
    {
        _failure = "the effect makes no picture";
        return false;
    }
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        _failure = "the context is not the immediate one";
        return false;
    }

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (device == nullptr)
    {
        _failure = "the context has no device";
        return false;
    }
    if (device.Get() != _device.Get())
    {
        Reset();
        if (!Init(device.Get()))
            return false;
    }

    // Both views have to be a plain 2D view of mip 0 of a single-sample texture in a format Load returns as the stored value, and the two textures one size.
    D3D11_TEXTURE2D_DESC desc[2] {};
    ID3D11ShaderResourceView* views[2] { request.in, request.out };
    for (int i = 0; i < 2; ++i)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
        views[i]->GetDesc(&viewDesc);
        if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || viewDesc.Texture2D.MostDetailedMip != 0)
        {
            _failure = "a view is not a plain 2D view of the top mip";
            return false;
        }
        if (!FormatReadable(uint32_t(viewDesc.Format)))
        {
            _failure = "a view's format is not one the meter reads";
            return false;
        }
        ComPtr<ID3D11Resource> resource;
        views[i]->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource == nullptr || FAILED(resource.As(&texture)) || texture == nullptr)
        {
            _failure = "a view is not a texture";
            return false;
        }
        texture->GetDesc(&desc[i]);
        if (desc[i].SampleDesc.Count != 1 || desc[i].ArraySize != 1)
        {
            _failure = "a texture is multisampled or an array";
            return false;
        }
    }
    if (desc[0].Width != desc[1].Width || desc[0].Height != desc[1].Height)
    {
        _failure = "the stage's input and output differ in size";
        return false;
    }

    // The box is the dump's own crop (the window floored, clamped to the texture), so the numbers describe the picture the .bin files show.
    const ext::ffxiv::diag::StageBox box = ext::ffxiv::diag::StageCrop(request.x, request.y, request.width, request.height, desc[0].Width, desc[0].Height);
    if (!box.valid)
    {
        _failure = "the stage's window is outside the texture";
        return false;
    }
    const Geometry geometry = GeometryFor(box.Width(), box.Height());
    if (!geometry.Valid())
    {
        _failure = "the box is too wide for the meter's target";
        return false;
    }

    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
    {
        _failure = "ID3D11DeviceContext1 is not available";
        return false;
    }

    const Params p = ParamsOf(request.id, request.white);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr)
    {
        _failure = "the parameter buffer could not be written";
        return false;
    }
    ParamsBuffer buffer {};
    buffer.box[0] = box.left;
    buffer.box[1] = box.top;
    buffer.box[2] = box.Width();
    buffer.box[3] = box.Height();
    buffer.geom[0] = geometry.block;
    buffer.geom[1] = geometry.tapsPerSide;
    buffer.geom[2] = geometry.cell;
    buffer.geom[3] = request.seed;
    buffer.mode[0] = p.channels;
    buffer.mode[1] = uint32_t(p.region);
    buffer.mode[2] = uint32_t(p.unit);
    buffer.meas[0] = p.white;
    buffer.meas[1] = kVisibleLevels;
    std::memcpy(mapped.pData, &buffer, sizeof buffer);
    context->Unmap(_params.Get(), 0);

    {
        // The game's own state has to come back on every path, also when the draw throws through this fork's hooks: the guard restores it in its destructor.
        ext::core::explicit_state::Guard stateGuard(context, context1.Get(), device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1, kSpec);
        stateGuard.Neutralise();

        const D3D11_VIEWPORT viewport { 0.0f, 0.0f, float(geometry.binsX), float(geometry.binsY), 0.0f, 1.0f };
        const float blendFactor[4] {};
        ID3D11Buffer* buffers[1] { _params.Get() };
        ID3D11RenderTargetView* rtv = _targetRtv.Get();
        ID3D11ShaderResourceView* bound[2] { request.in, request.out };
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(_vs.Get(), nullptr, 0);
        context->PSSetShader(_ps.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, buffers);
        context->RSSetState(_rasterizer.Get());
        context->RSSetViewports(1, &viewport);
        context->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->PSSetShaderResources(0, 2, bound);

        _timed = _disjoint && _begin && _mid && _end;
        if (_timed)
        {
            context->Begin(_disjoint.Get());
            context->End(_begin.Get());
        }
        context->Draw(3, 0);
        ++_drawsLastStart;
        if (_timed)
            context->End(_mid.Get());

        ID3D11ShaderResourceView* none[2] {};
        context->PSSetShaderResources(0, 2, none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    // The target's used corner to the staging texture, and the event behind the copy: the answer is read when the event says the GPU has got past it.
    const D3D11_BOX copyBox { 0, 0, 0, geometry.binsX, geometry.binsY, 1 };
    context->CopySubresourceRegion(_staging.Get(), 0, 0, 0, 0, _target.Get(), 0, &copyBox);
    if (_timed)
    {
        // The measured time is the draw AND the copy: the whole GPU cost of one sample.
        context->End(_end.Get());
        context->End(_disjoint.Get());
    }
    context->End(_done.Get());

    _pending = true;
    _geometry = geometry;
    _id = request.id;
    _boxWidth = box.Width();
    _boxHeight = box.Height();
    ++_started;
    return true;
}

bool Pass::Collect(ID3D11DeviceContext* context, Result& result)
{
    if (!_pending || context == nullptr)
        return false;
    // The read never waits and never flushes: a measurement that is not finished is looked at again on a later call.
    if (context->GetData(_done.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return false;
    D3D11_MAPPED_SUBRESOURCE mapped {};
    const HRESULT hr = context->Map(_staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        return false;
    _pending = false;
    if (FAILED(hr) || mapped.pData == nullptr)
    {
        _failure = "the staging texture could not be read";
        return false;
    }
    result = Result {};
    result.totals = Reduce(static_cast<const float*>(mapped.pData), _geometry.binsX, _geometry.binsY, mapped.RowPitch / sizeof(float));
    context->Unmap(_staging.Get(), 0);
    result.geometry = _geometry;
    result.id = _id;
    result.boxWidth = _boxWidth;
    result.boxHeight = _boxHeight;

    if (_timed)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint {};
        UINT64 begin = 0, mid = 0, end = 0;
        if (context->GetData(_disjoint.Get(), &disjoint, sizeof disjoint, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            context->GetData(_begin.Get(), &begin, sizeof begin, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            context->GetData(_mid.Get(), &mid, sizeof mid, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            context->GetData(_end.Get(), &end, sizeof end, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !disjoint.Disjoint && disjoint.Frequency > 0 && end > begin && mid >= begin &&
            end >= mid)
        {
            result.gpuMs = double(end - begin) * 1000.0 / double(disjoint.Frequency);
            result.gpuDrawMs = double(mid - begin) * 1000.0 / double(disjoint.Frequency);
            result.haveGpuMs = true;
        }
    }
    ++_collected;
    return true;
}
} // namespace ext::ffxiv::meter
