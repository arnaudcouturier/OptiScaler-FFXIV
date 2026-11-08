#include "pch.h"

#include "ext/ffxiv/enginelight/EngineLightPass.h"
#include "ext/core/D3D11ExplicitState.h"

#include <ExposureHold_Vs.h>
#include <ExposureHold_Hold.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// Engine light controls, the exposure hold's pass and readers (EngineLightPass.h). One draw between an explicit save and restore of the state it touches (a scope guard
// puts it back), one constant buffer mapped D3D11_MAP_WRITE_DISCARD per run, one timestamp pair inside a disjoint query read without ever waiting for the GPU.
namespace ext::ffxiv::enginelight
{
using Microsoft::WRL::ComPtr;

namespace
{
// The register slots and the constant-buffer layouts of ext/ffxiv/enginelight/shaders/ExposureHold.hlsl. Change them together.
constexpr uint32_t kSlotScene = 0;
constexpr uint32_t kSlotParams = 0;
constexpr uint32_t kSlotCommon = 1;

// What this pass touches on the game's context, in the Spec's fields (psSrvCount, psSamplerCount, psConstantBuffers): pixel-shader resource slot 0 and constant buffers 0
// and 1, the IA layout and topology, VS and PS, the rasterizer state and the viewports, blend, depth-stencil and the one render target. No sampler: the shader only Loads.
constexpr ext::core::explicit_state::Spec kSpec { 1, 0, 2 };

struct Float4
{
    float v[4];
};

void Set(Float4& out, float a, float b, float c, float d)
{
    out.v[0] = a;
    out.v[1] = b;
    out.v[2] = c;
    out.v[3] = d;
}

struct ParamsBuffer
{
    Float4 mode;
    Float4 limits;
    Float4 valid;
};

static_assert(sizeof(ParamsBuffer) == 3 * 16, "the pass's constant buffer is three float4s");

// The scene views: the engine's texture may be typeless, so the copy takes the texture's own format and the views take the FP16 the tone map reads.
constexpr DXGI_FORMAT kSceneViewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

float HalfToFloat(uint16_t h)
{
    const uint32_t sign = uint32_t(h >> 15) & 1u;
    const uint32_t exponent = uint32_t(h >> 10) & 31u;
    const uint32_t mantissa = uint32_t(h) & 1023u;
    float value;
    if (exponent == 0)
        value = std::ldexp(float(mantissa), -24);
    else if (exponent == 31)
        value = mantissa == 0 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
    else
        value = std::ldexp(float(mantissa | 1024u), int(exponent) - 25);
    return sign != 0 ? -value : value;
}
} // namespace

// ---- the exposure reader ---------------------------------------------------------------------------------------------------------------

void ExposureReader::Reset()
{
    _device.Reset();
    _live.Reset();
    _liveFrame = 0;
    for (Slot& slot : _ring)
        slot = Slot {};
    _next = 0;
    _lastAcceptedFrame = 0;
    _haveAccepted = false;
}

bool ExposureReader::Ensure(ID3D11Device* device)
{
    if (device == nullptr)
        return false;
    if (device != _device.Get())
    {
        Reset();
        _device = device;
    }
    if (_live)
        return true;
    D3D11_BUFFER_DESC live {};
    live.ByteWidth = kCommonParameterBytes;
    live.Usage = D3D11_USAGE_DEFAULT;
    live.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&live, nullptr, &_live)))
        return false;
    for (Slot& slot : _ring)
    {
        D3D11_BUFFER_DESC staging {};
        staging.ByteWidth = kCommonParameterBytes;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateBuffer(&staging, nullptr, &slot.staging)))
        {
            Reset();
            return false;
        }
    }
    return true;
}

bool ExposureReader::Capture(ID3D11DeviceContext* context, ID3D11Buffer* common, uint32_t firstByte, uint64_t frame)
{
    if (context == nullptr || common == nullptr)
        return false;
    D3D11_BUFFER_DESC desc {};
    common->GetDesc(&desc);
    if (uint64_t(firstByte) + kCommonParameterBytes > desc.ByteWidth)
        return false; // not the 64-byte buffer
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!Ensure(device.Get()))
        return false;
    const D3D11_BOX box { firstByte, 0, 0, firstByte + kCommonParameterBytes, 1, 1 };
    context->CopySubresourceRegion(_live.Get(), 0, 0, 0, 0, common, 0, &box);
    _liveFrame = frame;
    Slot& slot = _ring[_next];
    _next = (_next + 1) % kExposureRing;
    context->CopyResource(slot.staging.Get(), _live.Get());
    slot.frame = frame;
    slot.written = true;
    return true;
}

ExposureRead ExposureReader::Read(ID3D11DeviceContext* context, uint64_t frame)
{
    ExposureRead out;
    if (context == nullptr)
        return out;
    // The newest copy of an earlier frame that has not been read: tried newest first, an older one when the newer has not landed.
    int order[kExposureRing];
    int count = 0;
    for (uint32_t i = 0; i < kExposureRing; ++i)
        if (_ring[i].written && _ring[i].frame < frame && (!_haveAccepted || _ring[i].frame > _lastAcceptedFrame))
            order[count++] = int(i);
    std::sort(order, order + count, [this](int a, int b) { return _ring[a].frame > _ring[b].frame; });
    for (int k = 0; k < count; ++k)
    {
        Slot& slot = _ring[order[k]];
        D3D11_MAPPED_SUBRESOURCE mapped {};
        const HRESULT hr = context->Map(slot.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (FAILED(hr) || mapped.pData == nullptr)
            continue; // DXGI_ERROR_WAS_STILL_DRAWING: look at an older copy
        float misc2[4];
        std::memcpy(misc2, static_cast<const unsigned char*>(mapped.pData) + kMisc2Offset, sizeof misc2);
        context->Unmap(slot.staging.Get(), 0);
        out.landed = true;
        out.s = misc2[0];
        out.y = misc2[1];
        out.valid = ExposureValid(out.s, out.y);
        out.writtenFrame = slot.frame;
        _haveAccepted = true;
        _lastAcceptedFrame = slot.frame;
        return out;
    }
    return out;
}

// ---- the table sampler -----------------------------------------------------------------------------------------------------------------

void TableSampler::Reset()
{
    for (int i = 0; i < 2; ++i)
    {
        _staging[i].Reset();
        _pending[i] = false;
    }
    _next = 0;
    _device.Reset();
}

bool TableSampler::Sample(ID3D11DeviceContext* context, ID3D11ShaderResourceView* table)
{
    if (context == nullptr || table == nullptr)
        return false;
    ComPtr<ID3D11Resource> resource;
    table->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (resource == nullptr || FAILED(resource.As(&texture)) || texture == nullptr)
        return false;
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    if (desc.Width != kTableTexels || desc.Height != 1 || desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || desc.ArraySize != 1 || desc.SampleDesc.Count != 1)
        return false;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (device == nullptr)
        return false;
    if (device.Get() != _device.Get())
    {
        Reset();
        _device = device;
    }
    if (_pending[_next])
        return false; // the slot has not been read yet: skip this sample rather than overwrite it
    if (!_staging[_next])
    {
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        staging.MipLevels = 1;
        if (FAILED(device->CreateTexture2D(&staging, nullptr, &_staging[_next])))
            return false;
    }
    context->CopyResource(_staging[_next].Get(), texture.Get());
    _pending[_next] = true;
    _next = (_next + 1) % 2;
    return true;
}

TableRead TableSampler::Read(ID3D11DeviceContext* context)
{
    TableRead out;
    if (context == nullptr)
        return out;
    for (int i = 0; i < 2; ++i)
    {
        if (!_pending[i] || !_staging[i])
            continue;
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(_staging[i].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)) || mapped.pData == nullptr)
            continue;
        const uint16_t* texels = static_cast<const uint16_t*>(mapped.pData);
        auto mean = [&](uint32_t texel) {
            const uint16_t* p = texels + size_t(texel) * 4;
            return (HalfToFloat(p[0]) + HalfToFloat(p[1]) + HalfToFloat(p[2])) / 3.0f;
        };
        out.low = mean(128);
        out.mid = mean(512);
        out.high = mean(896);
        context->Unmap(_staging[i].Get(), 0);
        _pending[i] = false;
        out.landed = true;
    }
    return out;
}

// ---- the pass ----------------------------------------------------------------------------------------------------------------------------

void Pass::Reset()
{
    _sceneSrv.Reset();
    _sceneRtv.Reset();
    _scene.Reset();
    _params.Reset();
    _rasterizer.Reset();
    _holdShader.Reset();
    _vs.Reset();
    _device.Reset();
    _disjoint.Reset();
    _begin.Reset();
    _end.Reset();
    _sceneResourceFormat = _sceneWidth = _sceneHeight = 0;
    _scenesCreated = 0;
    _drawsLastRun = 0;
    _timingPending = false;
    _haveGpuMs = false;
    _gpuMs = 0.0;
    _failure = nullptr;
}

bool Pass::Init(ID3D11Device* device)
{
    if (device == nullptr)
        return false;
    if (FAILED(device->CreateVertexShader(ExposureHold_Vs_cso, sizeof ExposureHold_Vs_cso, nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(ExposureHold_Hold_cso, sizeof ExposureHold_Hold_cso, nullptr, &_holdShader)))
    {
        _failure = "the exposure hold shaders could not be created";
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

    // The pass's own GPU time: queries whose reads never wait for the GPU. No queries: the measurement is the only thing that is lost.
    D3D11_QUERY_DESC disjointDesc { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    D3D11_QUERY_DESC stampDesc { D3D11_QUERY_TIMESTAMP, 0 };
    if (FAILED(device->CreateQuery(&disjointDesc, &_disjoint)) || FAILED(device->CreateQuery(&stampDesc, &_begin)) || FAILED(device->CreateQuery(&stampDesc, &_end)))
    {
        _disjoint.Reset();
        _begin.Reset();
        _end.Reset();
    }
    _device = device;
    return true;
}

bool Pass::EnsureScene(ID3D11Device* device, D3D11_TEXTURE2D_DESC sceneDesc)
{
    // The copy's own description: the engine's texture may carry a smaller bind-flag set than the pass needs, so the copy asks for both binds of its own.
    D3D11_TEXTURE2D_DESC copyDesc = sceneDesc;
    copyDesc.MipLevels = 1;
    copyDesc.ArraySize = 1;
    copyDesc.SampleDesc.Count = 1;
    copyDesc.SampleDesc.Quality = 0;
    copyDesc.Usage = D3D11_USAGE_DEFAULT;
    copyDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    copyDesc.CPUAccessFlags = 0;
    copyDesc.MiscFlags = 0;
    if (_scene && _sceneResourceFormat == uint32_t(copyDesc.Format) && _sceneWidth == copyDesc.Width && _sceneHeight == copyDesc.Height)
        return true;
    _sceneSrv.Reset();
    _sceneRtv.Reset();
    _scene.Reset();
    _sceneResourceFormat = _sceneWidth = _sceneHeight = 0;
    D3D11_RENDER_TARGET_VIEW_DESC rtv {};
    rtv.Format = kSceneViewFormat;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = kSceneViewFormat;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, &_scene)) || FAILED(device->CreateRenderTargetView(_scene.Get(), &rtv, &_sceneRtv)) ||
        FAILED(device->CreateShaderResourceView(_scene.Get(), &srv, &_sceneSrv)))
    {
        _sceneSrv.Reset();
        _sceneRtv.Reset();
        _scene.Reset();
        _failure = "this pass's copy of the scene could not be created";
        return false;
    }
    _sceneResourceFormat = uint32_t(copyDesc.Format);
    _sceneWidth = sceneDesc.Width;
    _sceneHeight = sceneDesc.Height;
    ++_scenesCreated;
    return true;
}

void Pass::CollectGpuTiming(ID3D11DeviceContext* context)
{
    if (!_timingPending || !_disjoint || !_begin || !_end)
        return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint {};
    // The read never waits and never flushes: a query that is not finished is looked at again on the next run.
    if (context->GetData(_disjoint.Get(), &disjoint, sizeof disjoint, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return;
    UINT64 begin = 0, end = 0;
    const HRESULT haveBegin = context->GetData(_begin.Get(), &begin, sizeof begin, D3D11_ASYNC_GETDATA_DONOTFLUSH);
    const HRESULT haveEnd = context->GetData(_end.Get(), &end, sizeof end, D3D11_ASYNC_GETDATA_DONOTFLUSH);
    _timingPending = false;
    if (!disjoint.Disjoint && disjoint.Frequency > 0 && haveBegin == S_OK && haveEnd == S_OK && end > begin)
    {
        _gpuMs = double(end - begin) * 1000.0 / double(disjoint.Frequency);
        _haveGpuMs = true;
    }
}

ID3D11ShaderResourceView* Pass::Run(ID3D11DeviceContext* context, const Inputs& inputs)
{
    _failure = nullptr;
    _drawsLastRun = 0;
    if (context == nullptr || inputs.scene == nullptr)
    {
        _failure = "the draw bound no scene";
        return nullptr;
    }
    // Nothing to do: the engine's own picture is the answer (null, no failure, no draw).
    if (inputs.mode == kExposureAuto)
    {
        if (inputs.exposureCopy == nullptr || !(inputs.sHold > 0.0f))
            return nullptr;
    }
    else if (inputs.mode == kExposureManual)
    {
        if (GainIsIdentity(inputs.manualGain) || !(inputs.manualGain > 0.0f))
            return nullptr;
    }
    else
        return nullptr;
    if (!shafts::IsSceneFormat(inputs.sceneFormat))
    {
        _failure = "the scene is not FP16";
        return nullptr;
    }
    if (!shafts::WindowUsable(inputs.window))
    {
        _failure = "the render window is empty, too small or does not fit the scene";
        return nullptr;
    }

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

    // The scene has to be one texture this pass can read: a single-sample 2D texture with one mip and one slice, of the size the draw's own viewport and target
    // measured. Anything else is another pass's binding, and the draw keeps the texture it was handed.
    ComPtr<ID3D11Resource> sceneResource;
    inputs.scene->GetResource(&sceneResource);
    ComPtr<ID3D11Texture2D> sceneTexture;
    if (sceneResource == nullptr || FAILED(sceneResource.As(&sceneTexture)) || sceneTexture == nullptr)
    {
        _failure = "the scene is not a texture";
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC sceneDesc {};
    sceneTexture->GetDesc(&sceneDesc);
    if (sceneDesc.MipLevels != 1 || sceneDesc.ArraySize != 1 || sceneDesc.SampleDesc.Count != 1 || sceneDesc.Width != inputs.sceneWidth ||
        sceneDesc.Height != inputs.sceneHeight)
    {
        _failure = "the scene is not the single-sample texture the draw measured";
        return nullptr;
    }
    if (!EnsureScene(device.Get(), sceneDesc))
        return nullptr;

    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
    {
        _failure = "ID3D11DeviceContext1 is not available";
        return nullptr;
    }

    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr)
    {
        _failure = "the parameter buffer could not be written";
        return nullptr;
    }
    ParamsBuffer params {};
    Set(params.mode, float(inputs.mode), inputs.manualGain, inputs.sHold, 0.0f);
    Set(params.limits, kGainMin, kGainMax, kYRelTolerance, kYAbsTolerance);
    Set(params.valid, kSMinValid, kSMaxValid, kYFactor, 0.0f);
    std::memcpy(mapped.pData, &params, sizeof params);
    context->Unmap(_params.Get(), 0);

    CollectGpuTiming(context);
    // The game's own state has to come back on every path, also when the draw throws through this fork's hooks: the guard restores it in its destructor.
    ext::core::explicit_state::Guard stateGuard(context, context1.Get(), device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1, kSpec);
    stateGuard.Neutralise(); // what the private state's defaults provided: no HS, DS, GS, stream-out target or predicate

    // The rectangle: the render window and its margin, inside the texture.
    const float left = std::max(0.0f, inputs.window.x - kWindowMargin);
    const float top = std::max(0.0f, inputs.window.y - kWindowMargin);
    const float right = std::min(float(sceneDesc.Width), inputs.window.x + inputs.window.width + kWindowMargin);
    const float bottom = std::min(float(sceneDesc.Height), inputs.window.y + inputs.window.height + kWindowMargin);
    const D3D11_VIEWPORT viewport { left, top, right - left, bottom - top, 0.0f, 1.0f };

    const float blendFactor[4] {};
    ID3D11Buffer* buffers[2] {};
    buffers[kSlotParams] = _params.Get();
    buffers[kSlotCommon] = inputs.mode == kExposureAuto ? inputs.exposureCopy : _params.Get(); // manual never reads b1; a valid buffer keeps the slot defined
    ID3D11RenderTargetView* sceneRtv = _sceneRtv.Get();
    ID3D11ShaderResourceView* views[1] {};
    views[kSlotScene] = inputs.scene;
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(_vs.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 2, buffers);
    context->RSSetState(_rasterizer.Get());
    context->RSSetViewports(1, &viewport);
    context->OMSetDepthStencilState(nullptr, 0);
    context->OMSetRenderTargets(1, &sceneRtv, nullptr);
    context->PSSetShaderResources(0, 1, views);

    // The hold draw, timed: one timestamp pair, read a run or two later.
    bool timed = false;
    if (_disjoint && _begin && _end && !_timingPending)
    {
        context->Begin(_disjoint.Get());
        context->End(_begin.Get());
        _timingPending = true;
        ++_timingBegins;
        timed = true;
    }
    context->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    context->PSSetShader(_holdShader.Get(), nullptr, 0);
    context->Draw(3, 0);
    ++_drawsLastRun;
    if (timed)
    {
        context->End(_end.Get());
        context->End(_disjoint.Get());
        ++_timingEnds;
        // `_timingPending` stays set: the pair is in the queue until CollectGpuTiming reads it on a later run.
    }

    ID3D11ShaderResourceView* none[1] {};
    context->PSSetShaderResources(0, 1, none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    return _sceneSrv.Get();
}
} // namespace ext::ffxiv::enginelight
