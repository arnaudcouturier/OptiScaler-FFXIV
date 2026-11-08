#include "pch.h"

#include "ext/ffxiv/engine/PuddleTonePass.h"

#include <PuddleReflection_Ps.h>
#include <PuddleReflection_Vs.h>

#include <cstring>

// Puddles at the tone map: the device side of the pass (ext/ffxiv/engine/PuddleTonePass.h). One draw on the game's own immediate context inside a
// private device-context state, two owned textures (the copy of the scene the tone map reads, and - only while a capture is armed - the
// per-texel statistics) and a timestamp pair that is read a run or two later and never waited for.
namespace ext::ffxiv::engine::gloss
{
using Microsoft::WRL::ComPtr;

namespace
{
// The register slots of ext/ffxiv/engine/shaders/PuddleReflection.hlsl. Change them together.
constexpr uint32_t kSlotScene = 0;
constexpr uint32_t kSlotVPosition = 1;
constexpr uint32_t kSlotNormal = 2;
constexpr uint32_t kSlotCamera = 3;
constexpr uint32_t kSlotWetness = 4;
constexpr uint32_t kSlotCount = 5;

// The statistics target's format: two flags per texel, so the pass writes a value a capture can count and a frame that is not captured never
// pays for the second target (it is not bound, and an unbound render target costs nothing).
constexpr uint32_t kStatsFormat = 34; // DXGI_FORMAT_R16G16_FLOAT
constexpr uint16_t kHalfOne = 0x3C00;  // the half the shader writes for a flag that is set: the count compares against it, so 1.0 needs no conversion

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

D3D11_BOX RegionBox(const Region& region)
{
    D3D11_BOX box {};
    box.left = region.x;
    box.top = region.y;
    box.front = 0;
    box.right = region.Right();
    box.bottom = region.Bottom();
    box.back = 1;
    return box;
}
} // namespace

void TonePass::Reset()
{
    _statsStaging.Reset();
    _statsRtv.Reset();
    _statsTarget.Reset();
    _outSrv.Reset();
    _outRtv.Reset();
    _out.Reset();
    _cameraView.Reset();
    _camera.Reset();
    _params.Reset();
    _rasterizer.Reset();
    _ps.Reset();
    _vs.Reset();
    _state.Reset();
    _device.Reset();
    _disjoint.Reset();
    _begin.Reset();
    _end.Reset();
    _width = _height = 0;
    _format = 0;
    _statsStagingWidth = _statsStagingHeight = 0;
    _statsCopyPending = false;
    _outValid = false; // the output is gone with the pass: the tone stage has no hold to answer a skipped frame with
    _stats = ToneStats {};
    _statsKnown = false;
    _clearPending = true;
    _targetsCreated = 0;
    _failure = nullptr;
    _timingPending = false;
    _timingBegins = _timingEnds = 0;
    _haveGpuMs = false;
    _gpuMs = 0.0;
}

bool TonePass::Init(ID3D11Device* device)
{
    ComPtr<ID3D11Device1> device1;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))))
    {
        _failure = "ID3D11Device1 is not available";
        return false;
    }
    if (FAILED(device->CreateVertexShader(PuddleReflection_Vs_cso, sizeof PuddleReflection_Vs_cso, nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(PuddleReflection_Ps_cso, sizeof PuddleReflection_Ps_cso, nullptr, &_ps)))
    {
        _failure = "the puddle reflection shaders could not be created";
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
    params.ByteWidth = sizeof(ToneConstants);
    params.Usage = D3D11_USAGE_DYNAMIC;
    params.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    params.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&params, nullptr, &_params)))
    {
        _failure = "the parameter buffer could not be created";
        return false;
    }

    // The camera table, GPU side: a destination this pass reads through a shader resource view rather than a constant buffer, the same shape the
    // water reflections use (a partial-box copy into a constant buffer is not something every driver accepts).
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

    // The private pipeline state: the game's own is captured and put back after the draw.
    const D3D_FEATURE_LEVEL levels[] { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL chosen {};
    const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    if (FAILED(device1->CreateDeviceContextState(flags, levels, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), &chosen, &_state)) ||
        _state == nullptr)
    {
        _failure = "the private device-context state could not be created";
        return false;
    }

    // The draw's own GPU time: a timestamp pair inside a disjoint query, read one run later without ever waiting for the GPU.
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

bool TonePass::EnsureTargets(ID3D11Device* device, const ToneFacts& facts)
{
    if (_out && _width == facts.sceneWidth && _height == facts.sceneHeight && _format == facts.sceneFormat)
        return true;
    _outSrv.Reset();
    _outRtv.Reset();
    _out.Reset();
    _width = _height = 0;
    _format = 0;
    _clearPending = true;
    _outValid = false; // a size or format change drops the pass's last answer with the texture itself

    // The copy of the scene the tone map will read, at the scene's own size and format: the picture is the part of it the render window covers.
    // Its view is only there to clear it once (Run does that on the first copy), so a copy that does not land leaves black rather than the
    // previous frame's picture.
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = facts.sceneWidth;
    desc.Height = facts.sceneHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT(facts.sceneFormat);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc {};
    rtvDesc.Format = desc.Format;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &_out)) || FAILED(device->CreateRenderTargetView(_out.Get(), &rtvDesc, &_outRtv)) ||
        FAILED(device->CreateShaderResourceView(_out.Get(), &srvDesc, &_outSrv)))
    {
        _outSrv.Reset();
        _outRtv.Reset();
        _out.Reset();
        _failure = "the scene copy could not be created";
        return false;
    }
    _width = facts.sceneWidth;
    _height = facts.sceneHeight;
    _format = facts.sceneFormat;
    // A statistics target belongs to one scene size: the pair a capture writes is rebuilt with it.
    _statsTarget.Reset();
    _statsRtv.Reset();
    _statsStaging.Reset();
    _statsStagingWidth = _statsStagingHeight = 0;
    _statsCopyPending = false;
    ++_targetsCreated;
    return true;
}

bool TonePass::EnsureStatsTarget(ID3D11Device* device, const ToneFacts& facts)
{
    if (_statsTarget && _statsRtv && _statsStaging && _statsStagingWidth == facts.sceneWidth && _statsStagingHeight == facts.sceneHeight)
        return true;
    _statsStaging.Reset();
    _statsRtv.Reset();
    _statsTarget.Reset();
    _statsStagingWidth = _statsStagingHeight = 0;
    _statsCopyPending = false; // the copy that was in the queue belonged to the target that is gone

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = facts.sceneWidth;
    desc.Height = facts.sceneHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT(kStatsFormat);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc {};
    rtvDesc.Format = desc.Format;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

    // The copy a later run maps without waiting: the whole allocation, because the render window's origin and size belong to the frame that is
    // reading it, and a staging texture that changes size would have to be rebuilt on every resize of that window.
    D3D11_TEXTURE2D_DESC staging {};
    staging.Width = facts.sceneWidth;
    staging.Height = facts.sceneHeight;
    staging.MipLevels = 1;
    staging.ArraySize = 1;
    staging.Format = DXGI_FORMAT(kStatsFormat);
    staging.SampleDesc.Count = 1;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &_statsTarget)) || FAILED(device->CreateRenderTargetView(_statsTarget.Get(), &rtvDesc, &_statsRtv)) ||
        FAILED(device->CreateTexture2D(&staging, nullptr, &_statsStaging)))
    {
        _statsStaging.Reset();
        _statsRtv.Reset();
        _statsTarget.Reset();
        _failure = "the statistics target could not be created";
        return false;
    }
    _statsStagingWidth = facts.sceneWidth;
    _statsStagingHeight = facts.sceneHeight;
    return true;
}

void TonePass::CollectStats(ID3D11DeviceContext* context, const ToneFacts& facts)
{
    // The copy an earlier run took: read without waiting, and a map that has not landed keeps the previous answer. It is a capture's own path, so
    // a run that is not being captured neither copies nor maps anything.
    if (!_statsCopyPending || !_statsStaging)
        return;
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (context->Map(_statsStaging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK || mapped.pData == nullptr)
        return; // the GPU is behind: the stats are looked at on the next capture frame
    _statsCopyPending = false;
    const Region region = WholeTexels(facts.window[0], facts.window[1], facts.window[2], facts.window[3], facts.sceneWidth, facts.sceneHeight);
    ToneStats stats;
    stats.texels = region.width * region.height;
    for (uint32_t y = region.y; y < region.Bottom(); ++y)
    {
        // R16G16_FLOAT: two halves in one 32-bit word. The shader writes only 0.0 and 1.0, so a half at or above 0.5 is that flag and the exact
        // value needs no conversion; the comparison is on the half's own bit pattern for that reason.
        const auto* halves = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch);
        for (uint32_t x = region.x; x < region.Right(); ++x)
        {
            if (halves[size_t(x) * 2 + 0] >= kHalfOne)
                ++stats.poolTexels;
            if (halves[size_t(x) * 2 + 1] >= kHalfOne)
                ++stats.hitTexels;
        }
    }
    _stats = stats;
    _statsKnown = true;
    context->Unmap(_statsStaging.Get(), 0);
}

void TonePass::CollectGpuTiming(ID3D11DeviceContext* context)
{
    if (!_timingPending || !_disjoint || !_begin || !_end)
        return;
    // D3D11_ASYNC_GETDATA_DONOTFLUSH on every read, the same idiom as AoPass/CbPatch/SsrPass: a GetData without it can
    // flush the game's own command queue to resolve the query, and this runs once a run on the immediate context.
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

bool TonePass::BeginGpuTiming(ID3D11DeviceContext* context)
{
    if (!_disjoint || !_begin || !_end || _timingPending)
        return false; // the previous pair is still in the queue: this run is not timed, and it must not end that pair either
    context->Begin(_disjoint.Get());
    context->End(_begin.Get());
    _timingPending = true;
    ++_timingBegins;
    return true;
}

ID3D11ShaderResourceView* TonePass::Run(ID3D11DeviceContext* context, const TonePassInputs& inputs)
{
    // Every run starts with no reason: the caller logs whatever this run left behind, and a reason from an earlier failure would name the wrong
    // fault.
    _failure = nullptr;
    if (context == nullptr || inputs.scene == nullptr || inputs.sceneTexture == nullptr || inputs.viewPosition == nullptr ||
        inputs.normal == nullptr || inputs.camera == nullptr || inputs.wetness == nullptr)
        return nullptr;
    // The pass runs on the shape the pure rules accepted; a caller that skipped them gets the game's own scene back.
    if (ClassifyTone(inputs.facts) != ToneDecline::None)
        return nullptr;
    const Region region = WholeTexels(inputs.facts.window[0], inputs.facts.window[1], inputs.facts.window[2], inputs.facts.window[3],
                                      inputs.facts.sceneWidth, inputs.facts.sceneHeight);
    if (region.Empty())
    {
        _failure = "the render window covers no whole texel of the scene";
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
        _failure = "the camera table is smaller than the matrices the shader reads";
        return nullptr;
    }
    if (!EnsureTargets(device.Get(), inputs.facts))
        return nullptr;
    const bool stats = inputs.stats && EnsureStatsTarget(device.Get(), inputs.facts);
    if (inputs.stats && !stats && !_statsTarget)
        return nullptr; // a capture asked for the shares and the target could not be built: this run is a counted failure

    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
    {
        _failure = "ID3D11DeviceContext1 is not available";
        return nullptr;
    }

    // The camera table, GPU side: one small copy, no map, no readback.
    const D3D11_BOX cameraBox = ByteBox(0, kCameraBytes);
    context->CopySubresourceRegion(_camera.Get(), 0, 0, 0, 0, inputs.camera, 0, &cameraBox);

    // A fresh copy of the scene starts cleared, so a copy that does not land reads as black rather than as the previous frame's picture.
    if (_clearPending && _outRtv)
    {
        const float nothing[4] { 0.0f, 0.0f, 0.0f, 0.0f };
        context->ClearRenderTargetView(_outRtv.Get(), nothing);
        _clearPending = false;
    }

    // The game's own texels, so every texel outside the region below is the game's answer and not an uninitialised target. Only the window's
    // one-texel ring is copied: the draw below covers every texel inside the window, and the tone map's own filter is what reads the ring. The copy
    // is taken with the game's render targets unbound: a copy source may be bound, but the shape is one a runtime or a driver may treat as
    // undefined. The cost is one state query, two binds, a few thousand texels and the released references, once a frame.
    const RegionBands border = ToneBorderOf(inputs.facts);
    if (ToneCopyOf(inputs.facts).Empty())
    {
        _failure = "the render window covers no texel of the scene copy";
        return nullptr;
    }
    // A window that covers the whole allocation has no ring: the draw writes every texel the tone map can read, so nothing is copied.
    if (border.count != 0)
    {
        ID3D11RenderTargetView* gameViews[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] {};
        ID3D11DepthStencilView* gameDepth = nullptr;
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, gameViews, &gameDepth);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        // The hold's flag drops while this frame's copy is in flight: a post-copy failure (the scalar Map, the state swap)
        // on a pass that succeeded before must not serve THIS frame's half-copied scene through the hold (the review's N2).
        _outValid = false;
        for (uint32_t i = 0; i < border.count; ++i)
        {
            const D3D11_BOX bandBox = RegionBox(border.band[i]);
            context->CopySubresourceRegion(_out.Get(), 0, border.band[i].x, border.band[i].y, 0, inputs.sceneTexture, 0, &bandBox);
        }
        context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, gameViews, gameDepth);
        for (ID3D11RenderTargetView* view : gameViews)
            if (view != nullptr)
                view->Release();
        if (gameDepth != nullptr)
            gameDepth->Release();
    }

    // The scalars, one dynamic buffer.
    D3D11_MAPPED_SUBRESOURCE params {};
    if (FAILED(context->Map(_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &params)) || params.pData == nullptr)
    {
        _failure = "the parameter buffer could not be mapped";
        return nullptr;
    }
    const ToneConstants constants = ToneConstantsOf(inputs.facts, inputs.params, inputs.timeSeconds, inputs.rain);
    std::memcpy(params.pData, &constants, sizeof constants);
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
    // The game's own device-context state has to come back, also when the draw throws through this fork's hooks.
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
    // The draw runs over the region only: the game's own scene is the picture in a session that renders the picture smaller than the allocation,
    // so the texels outside it keep the copy taken above.
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

    ID3D11ShaderResourceView* views[kSlotCount] {};
    views[kSlotScene] = inputs.scene;
    views[kSlotVPosition] = inputs.viewPosition;
    views[kSlotNormal] = inputs.normal;
    views[kSlotCamera] = _cameraView.Get();
    views[kSlotWetness] = inputs.wetness;
    ID3D11RenderTargetView* targets[2] { _outRtv.Get(), stats ? _statsRtv.Get() : nullptr };
    // A capture's statistics start at zero, and only a capture binds the second target: the shader's write to it is discarded otherwise, which
    // costs a session nothing.
    if (stats && _statsRtv)
    {
        const float nothing[4] { 0.0f, 0.0f, 0.0f, 0.0f };
        context->ClearRenderTargetView(_statsRtv.Get(), nothing);
        CollectStats(context, inputs.facts); // the earlier frame's copy, before this frame's is queued
    }
    const bool timed = BeginGpuTiming(context);
    context->OMSetRenderTargets(stats ? 2u : 1u, targets, nullptr);
    context->PSSetShaderResources(0, kSlotCount, views);
    context->PSSetShader(_ps.Get(), nullptr, 0);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* none[kSlotCount] {};
    context->PSSetShaderResources(0, kSlotCount, none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    if (timed && _end && _disjoint)
    {
        context->End(_end.Get());
        context->End(_disjoint.Get());
        ++_timingEnds;
        // `_timingPending` stays set: the pair is in the queue until CollectGpuTiming reads it on a later run.
    }

    // The statistics of this frame, queued for a later run's no-wait map. It is taken after the draw and with no target bound, which is what the
    // copy of a render target requires.
    if (stats && _statsTarget && _statsStaging)
    {
        context->CopyResource(_statsStaging.Get(), _statsTarget.Get());
        _statsCopyPending = true;
    }

    _failure = nullptr;
    // The draw wrote every texel inside the window and the copy took the ring around it: the pass's output is a whole
    // picture from here on, the answer a later skipped frame keeps at t0 (the hold, instead of the engine's scene).
    _outValid = true;
    return _outSrv.Get();
}
} // namespace ext::ffxiv::engine::gloss
