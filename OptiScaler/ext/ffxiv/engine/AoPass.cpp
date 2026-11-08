#include "pch.h"

#include "ext/ffxiv/engine/AoPass.h"

#include <Ao_Ao.h>
#include <Ao_Final.h>
#include <Ao_Vs.h>

#include <cstring>

// Ambient occlusion: the device side of the pass (ext/ffxiv/engine/AoPass.h). Two draws on the game's own immediate context inside a private
// device-context state, two owned targets of the occlusion texture's size and format, one owned copy of the camera table, and a timestamp
// pair that is read a run or two later and never waited for.
namespace ext::ffxiv::engine::ao
{
using Microsoft::WRL::ComPtr;

namespace
{
// The register slots of ext/ffxiv/engine/shaders/Ao.hlsl. Change them together.
constexpr uint32_t kSlotDepth = 0;
constexpr uint32_t kSlotNormal = 1;
constexpr uint32_t kSlotCamera = 2;
constexpr uint32_t kSlotRaw = 3;
constexpr uint32_t kSlotOcclusion = 4;

// A byte range of a constant buffer as the box a partial copy takes: for a buffer the box is in bytes, one row, one slice.
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
    _outSrv.Reset();
    _outRtv.Reset();
    _out.Reset();
    _rawSrv.Reset();
    _rawRtv.Reset();
    _raw.Reset();
    _cameraView.Reset();
    _camera.Reset();
    _params.Reset();
    _rasterizer.Reset();
    _final.Reset();
    _ao.Reset();
    _vs.Reset();
    _state.Reset();
    _device.Reset();
    _disjoint.Reset();
    _begin.Reset();
    _end.Reset();
    _width = _height = 0;
    _targetsCreated = 0;
    _failure = nullptr;
    _timingPending = false;
    _timingBegins = _timingEnds = 0;
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
    if (FAILED(device->CreateVertexShader(Ao_Vs_cso, sizeof Ao_Vs_cso, nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(Ao_Ao_cso, sizeof Ao_Ao_cso, nullptr, &_ao)) ||
        FAILED(device->CreatePixelShader(Ao_Final_cso, sizeof Ao_Final_cso, nullptr, &_final)))
    {
        _failure = "the ambient occlusion shaders could not be created";
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
    params.ByteWidth = sizeof(Constants);
    params.Usage = D3D11_USAGE_DYNAMIC;
    params.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    params.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&params, nullptr, &_params)))
    {
        _failure = "the parameter buffer could not be created";
        return false;
    }

    // The camera table, GPU side. A destination this pass reads through a shader resource view rather than a constant buffer: a partial-box
    // copy into a constant buffer is not something every driver accepts.
    D3D11_BUFFER_DESC camera {};
    camera.ByteWidth = kCameraBytes;
    camera.Usage = D3D11_USAGE_DEFAULT;
    camera.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC cameraView {};
    cameraView.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    cameraView.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    cameraView.Buffer.FirstElement = 0;
    cameraView.Buffer.NumElements = kCameraBytes / 16;
    if (FAILED(device->CreateBuffer(&camera, nullptr, &_camera)) ||
        FAILED(device->CreateShaderResourceView(_camera.Get(), &cameraView, &_cameraView)))
    {
        _failure = "the camera table's copy could not be created";
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
        _end.Reset(); // no timings, the pass still runs: the measurement is the only thing that is lost
    }
    _device = device;
    return true;
}

bool Pass::EnsureTargets(ID3D11Device* device, uint32_t width, uint32_t height)
{
    if (_out && _raw && _width == width && _height == height)
        return true;
    _outSrv.Reset();
    _outRtv.Reset();
    _out.Reset();
    _rawSrv.Reset();
    _rawRtv.Reset();
    _raw.Reset();
    _width = _height = 0;

    // The search's own target: one 16-bit channel is plenty for a term that ends in 8 bits, and half the memory of a float4.
    D3D11_TEXTURE2D_DESC rawDesc {};
    rawDesc.Width = width;
    rawDesc.Height = height;
    rawDesc.MipLevels = 1;
    rawDesc.ArraySize = 1;
    rawDesc.Format = DXGI_FORMAT_R16_FLOAT;
    rawDesc.SampleDesc.Count = 1;
    rawDesc.Usage = D3D11_USAGE_DEFAULT;
    rawDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC rawRtv {};
    rawRtv.Format = DXGI_FORMAT_R16_FLOAT;
    rawRtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC rawSrv {};
    rawSrv.Format = DXGI_FORMAT_R16_FLOAT;
    rawSrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    rawSrv.Texture2D.MipLevels = 1;

    // What the reader draws bind: the engine's own format and size, so a shader that samples it (with its own sampler state, its own
    // addressing and any bilinear filter) sees the shape it expects.
    D3D11_TEXTURE2D_DESC outDesc {};
    outDesc.Width = width;
    outDesc.Height = height;
    outDesc.MipLevels = 1;
    outDesc.ArraySize = 1;
    outDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    outDesc.SampleDesc.Count = 1;
    outDesc.Usage = D3D11_USAGE_DEFAULT;
    outDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC outRtvDesc {};
    outRtvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    outRtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC outSrvDesc {};
    outSrvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    outSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    outSrvDesc.Texture2D.MipLevels = 1;

    if (FAILED(device->CreateTexture2D(&rawDesc, nullptr, &_raw)) || FAILED(device->CreateRenderTargetView(_raw.Get(), &rawRtv, &_rawRtv)) ||
        FAILED(device->CreateShaderResourceView(_raw.Get(), &rawSrv, &_rawSrv)) ||
        FAILED(device->CreateTexture2D(&outDesc, nullptr, &_out)) || FAILED(device->CreateRenderTargetView(_out.Get(), &outRtvDesc, &_outRtv)) ||
        FAILED(device->CreateShaderResourceView(_out.Get(), &outSrvDesc, &_outSrv)))
    {
        _outSrv.Reset();
        _outRtv.Reset();
        _out.Reset();
        _rawSrv.Reset();
        _rawRtv.Reset();
        _raw.Reset();
        _failure = "the occlusion targets could not be created";
        return false;
    }
    _width = width;
    _height = height;
    ++_targetsCreated;
    return true;
}

void Pass::CollectGpuTiming(ID3D11DeviceContext* context)
{
    if (!_timingPending || !_disjoint || !_begin || !_end)
        return;
    // D3D11_ASYNC_GETDATA_DONOTFLUSH on every read: a GetData without it can flush the game's own command queue to
    // resolve the query, and this runs once a frame on the immediate context - the periodic hitch a session reads
    // as a lag. Not finished is the expected answer most frames: the pair is read again on a later run.
    constexpr UINT kGetDataFlags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint {};
    if (context->GetData(_disjoint.Get(), &disjoint, sizeof disjoint, kGetDataFlags) != S_OK)
        return; // not finished: looked at again on the next run, never waited for
    UINT64 begin = 0, end = 0;
    const HRESULT haveBegin = context->GetData(_begin.Get(), &begin, sizeof begin, kGetDataFlags);
    const HRESULT haveEnd = context->GetData(_end.Get(), &end, sizeof end, kGetDataFlags);
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

ID3D11ShaderResourceView* Pass::Run(ID3D11DeviceContext* context, const PassInputs& inputs)
{
    if (context == nullptr || inputs.occlusion == nullptr || inputs.occlusionTexture == nullptr || inputs.depth == nullptr ||
        inputs.normal == nullptr || inputs.camera == nullptr)
        return nullptr;
    // The pass runs on the shape the pure rules accepted; a caller that skipped them gets the game's texture back.
    if (Classify(inputs.facts) != Decline::None)
        return nullptr;
    // The part of the target the engine's own producer fills, and the only part this pass writes.
    const Region region = RegionOf(inputs.facts);
    if (region.Empty())
    {
        _failure = "the render window covers no whole texel of the occlusion target";
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

    D3D11_BUFFER_DESC cameraDesc {};
    inputs.camera->GetDesc(&cameraDesc);
    if (cameraDesc.ByteWidth < kCameraBytes)
    {
        _failure = "the camera table is smaller than the matrices the shaders read";
        return nullptr;
    }
    if (!EnsureTargets(device.Get(), inputs.facts.occlusionWidth, inputs.facts.occlusionHeight))
        return nullptr;

    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
    {
        _failure = "ID3D11DeviceContext1 is not available";
        return nullptr;
    }

    // The camera table, GPU side: one small copy, no map, no readback.
    const D3D11_BOX cameraBox = ByteBox(0, kCameraBytes);
    context->CopySubresourceRegion(_camera.Get(), 0, 0, 0, 0, inputs.camera, 0, &cameraBox);

    // The engine's own texels, so every texel outside the region below is the engine's answer and not an uninitialised target: a reader that
    // samples the whole texture (bilinear taps at the region's edge included) sees the picture it saw before.
    context->CopySubresourceRegion(_out.Get(), 0, 0, 0, 0, inputs.occlusionTexture, 0, nullptr);

    // The scalars, one dynamic buffer.
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr)
        return nullptr;
    const Constants constants = ConstantsOf(inputs.facts, inputs.params);
    std::memcpy(mapped.pData, &constants, sizeof constants);
    context->Unmap(_params.Get(), 0);

    CollectGpuTiming(context);
    ComPtr<ID3DDeviceContextState> previous;
    context1->SwapDeviceContextState(_state.Get(), &previous);
    if (previous == nullptr)
    {
        context1->SwapDeviceContextState(_state.Get(), nullptr); // best effort, as the fork's other passes do
        _failure = "the previous device-context state was not returned";
        return nullptr;
    }
    // The game's own device-context state has to come back, also when one of the two draws throws through this fork's hooks.
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
    // Both draws run over the region only: the engine's own occlusion is a half-resolution quadrant of the render window in a session that
    // renders the picture smaller than the allocation, so the texels outside it keep the copy taken above.
    const D3D11_VIEWPORT viewport { float(region.x), float(region.y), float(region.width), float(region.height), 0.0f, 1.0f };

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(_vs.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, buffers);
    context->PSSetSamplers(0, 1, samplers);
    context->RSSetState(_rasterizer.Get());
    context->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);
    context->RSSetViewports(1, &viewport);

    // The search: the raw target is the render target here, so it must not also be bound as a shader resource.
    ID3D11ShaderResourceView* searchViews[5] {};
    searchViews[kSlotDepth] = inputs.depth;
    searchViews[kSlotNormal] = inputs.normal;
    searchViews[kSlotCamera] = _cameraView.Get();
    ID3D11RenderTargetView* rawRtv = _rawRtv.Get();
    const bool timed = BeginGpuTiming(context);
    context->OMSetRenderTargets(1, &rawRtv, nullptr);
    context->PSSetShaderResources(0, 5, searchViews);
    context->PSSetShader(_ao.Get(), nullptr, 0);
    context->Draw(3, 0);

    // The final draw: the raw target is switched out of the render-target slot first, because D3D11 drops a shader-resource view of the
    // current render target and the denoise would then read nothing at all.
    ID3D11RenderTargetView* outRtv = _outRtv.Get();
    context->OMSetRenderTargets(1, &outRtv, nullptr);
    ID3D11ShaderResourceView* finalViews[5] {};
    finalViews[kSlotRaw] = _rawSrv.Get();
    finalViews[kSlotOcclusion] = inputs.occlusion;
    context->PSSetShaderResources(0, 5, finalViews);
    context->PSSetShader(_final.Get(), nullptr, 0);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* none[5] {};
    context->PSSetShaderResources(0, 5, none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    if (timed && _end && _disjoint)
    {
        context->End(_end.Get());
        context->End(_disjoint.Get());
        ++_timingEnds;
        // `_timingPending` stays set: the pair is in the queue until CollectGpuTiming reads it on a later run.
    }

    _failure = nullptr;
    return _outSrv.Get();
}
} // namespace ext::ffxiv::engine::ao
