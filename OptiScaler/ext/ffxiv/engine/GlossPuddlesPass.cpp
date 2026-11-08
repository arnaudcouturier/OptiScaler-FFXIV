#include "pch.h"

#include "ext/ffxiv/engine/GlossPuddlesPass.h"
#include "ext/core/D3D11ExplicitState.h"

#include "ext/ffxiv/relight/ContactShadowsRules.h" // CameraLayoutOk: the one-time camera layout check

#include <GlossPuddles_Ps.h>
#include <GlossPuddles_Vs.h>

#include <chrono>
#include <cstring>

// Puddles: the device side of the pass (ext/ffxiv/engine/GlossPuddlesPass.h). One draw on the game's own immediate context between an explicit
// save and restore of the state it touches (ext/core/D3D11ExplicitState.h), one owned target of the engine's own size and format, one owned copy each of the camera table and the engine's wetness
// amount, and two readbacks that never wait for the GPU: the camera table's, once a session for the layout check, and the engine's wetness
// amount, once a frame for the run line.
namespace ext::ffxiv::engine::gloss
{
using Microsoft::WRL::ComPtr;

namespace
{
// The register slots of ext/ffxiv/engine/shaders/GlossPuddles.hlsl. Change them together.
constexpr uint32_t kSlotDepth = 0;
constexpr uint32_t kSlotNormal = 1;
constexpr uint32_t kSlotTarget = 2;
constexpr uint32_t kSlotCamera = 3;
constexpr uint32_t kSlotWetness = 4;
constexpr uint32_t kBoundSlots = 5;

// What this pass touches on the game's context: the IA layout and topology, VS and PS, PS resource slots 0..4 and constant buffer 0, the
// rasterizer state and the viewports, blend, depth-stencil, and the render targets with the UAV slots. HS, DS, GS, stream-out and predication
// are what the private device-context state used to provide as "none". No sampler is touched: the shader only Loads.
constexpr ext::core::explicit_state::Spec kSpec { /*psSrvCount*/ kBoundSlots, /*psSamplerCount*/ 0, /*psConstantBuffers*/ 1 };

// The gate's measurement target: two flags per texel (a pool before the gate, a pool after it), written by the shader's second output only on the runs
// that bind it. 8 bits a flag is enough because the shader writes exactly 0.0 and 1.0.
constexpr uint32_t kGateFlagsFormat = 49; // DXGI_FORMAT_R8G8_UNORM
constexpr uint8_t kGateFlagSet = 128;     // a UNORM byte at or above this is the shader's 1.0

// A refused camera table is looked at again after this long instead of switching the module off: the first light draws of a session can carry a
// loading camera, and the check is the thing that tells a patch from a loading screen. The check never waits for the GPU (the copy it reads is a
// frame or more old), so this is a back-off for a copy that keeps being refused, not a stall budget.
constexpr double kCameraCheckSeconds = 5.0;

double SteadySeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// One staging buffer's rows, or false when the GPU has not finished the copy that fills it. The map never waits: a map that would block is a map of
// a copy the GPU has not reached, and the pass then runs on a later frame instead of draining the queue inside one of the game's draws.
bool MapStagingRows(ID3D11DeviceContext* context, ID3D11Buffer* staging, float* rows, uint32_t bytes)
{
    if (context == nullptr || staging == nullptr || rows == nullptr || bytes == 0)
        return false;
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (context->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK || mapped.pData == nullptr)
        return false;
    std::memcpy(rows, mapped.pData, bytes);
    context->Unmap(staging, 0);
    return true;
}

// A read-back staging buffer of `bytes` for one of the pass's small copies (the camera table's rows, the engine's wetness amount).
bool EnsureStaging(ID3D11Device* device, Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer, uint32_t bytes)
{
    if (buffer)
        return true;
    if (device == nullptr || bytes == 0)
        return false;
    D3D11_BUFFER_DESC desc {};
    desc.ByteWidth = bytes;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(device->CreateBuffer(&desc, nullptr, &buffer)) && buffer != nullptr;
}

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
    // The frame's answers are the pass's own too: a torn-down pass carries no latch, no shaped frame and no hold (the
    // pointers would name a texture this pass is about to drop), so the frame's readers go back to the engine's target
    // and the tone-map stage's pairing has nothing to answer with until a new pass shapes one.
    _wetLatch = false;
    _shapeToken = 0;
    _shapeOnFrame = false;
    _holdEngine = nullptr;
    _holdEngineView = nullptr;
    _outValid = false;
    _gateStaging.Reset();
    _gateRtv.Reset();
    _gateTarget.Reset();
    _gateWidth = _gateHeight = 0;
    _gateCopyPending = false;
    _gateStats = GateStats {};
    _gateStatsKnown = false;
    _outSrv.Reset();
    _outRtv.Reset();
    _out.Reset();
    _wetnessView.Reset();
    _wetness.Reset();
    _cameraView.Reset();
    _camera.Reset();
    _params.Reset();
    _rasterizer.Reset();
    _ps.Reset();
    _vs.Reset();
    _device.Reset();
    _wetnessStaging[0].Reset();
    _wetnessStaging[1].Reset();
    _wetnessPending = false;
    _wetnessPendingSlot = 0;
    _engineWetness = 0.0f;
    _engineWetnessKnown = false;
    _cameraStaging[0].Reset();
    _cameraStaging[1].Reset();
    _width = _height = 0;
    _format = 0;
    _targetsCreated = 0;
    _cameraChecked = false;
    _cameraRejected = false;
    _cameraNotReady = false;
    _cameraNotReadyCount = 0;
    _cameraPending = false;
    _cameraPendingSlot = 0;
    _cameraRetryAt = 0.0;
    _failure = nullptr;
}

bool Pass::Init(ID3D11Device* device)
{
    if (FAILED(device->CreateVertexShader(GlossPuddles_Vs_cso, sizeof GlossPuddles_Vs_cso, nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(GlossPuddles_Ps_cso, sizeof GlossPuddles_Ps_cso, nullptr, &_ps)))
    {
        _failure = "the puddles shaders could not be created";
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

    // The camera table, GPU side. A destination this pass reads through a shader resource view rather than a constant buffer: a partial-box copy
    // into a constant buffer is not something every driver accepts.
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

    // The engine's wetness amount, GPU side. It starts at zero, which is the formula's identity: a frame whose material draws have not handed
    // the buffer over yet shapes nothing, and the pass is then a pure copy of the engine's target.
    const float wetnessZero[kWetnessBytes / 4] {};
    D3D11_BUFFER_DESC wetness {};
    wetness.ByteWidth = kWetnessBytes;
    wetness.Usage = D3D11_USAGE_DEFAULT;
    wetness.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA wetnessData { wetnessZero, 0, 0 };
    D3D11_SHADER_RESOURCE_VIEW_DESC wetnessView {};
    wetnessView.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    wetnessView.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    wetnessView.Buffer.FirstElement = 0;
    wetnessView.Buffer.NumElements = kWetnessBytes / 16;
    if (FAILED(device->CreateBuffer(&wetness, &wetnessData, &_wetness)) ||
        FAILED(device->CreateShaderResourceView(_wetness.Get(), &wetnessView, &_wetnessView)))
    {
        _failure = "the engine's wetness copy could not be created";
        return false;
    }

    // No private pipeline state: the game's own is captured and put back explicitly around the draw (ext/core/D3D11ExplicitState.h).
    _device = device;
    return true;
}

bool Pass::EnsureTarget(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& engineDesc)
{
    if (_out && _width == engineDesc.Width && _height == engineDesc.Height && _format == uint32_t(engineDesc.Format))
        return true;
    _outSrv.Reset();
    _outRtv.Reset();
    _out.Reset();
    _width = _height = 0;
    _format = 0;
    // A fresh texture holds no answer yet: the full copy in Run is what makes it one (a size change invalidates every
    // shape the pass had, including the hold).
    _outValid = false;

    // The engine's own size and format, so a reader that samples it (with its own sampler state, its own addressing and any bilinear filter)
    // sees the shape it expects. The views are typed: a typeless engine texture is copied here as it is, and the shader's four channels are the
    // 8-bit ones the key names.
    D3D11_TEXTURE2D_DESC outDesc = engineDesc;
    outDesc.MipLevels = 1;
    outDesc.ArraySize = 1;
    outDesc.SampleDesc.Count = 1;
    outDesc.Usage = D3D11_USAGE_DEFAULT;
    outDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    outDesc.CPUAccessFlags = 0;
    outDesc.MiscFlags = 0;
    D3D11_RENDER_TARGET_VIEW_DESC outRtv {};
    outRtv.Format = DXGI_FORMAT(kTargetFormat);
    outRtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC outSrv {};
    outSrv.Format = DXGI_FORMAT(kTargetFormat);
    outSrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    outSrv.Texture2D.MipLevels = 1;

    if (FAILED(device->CreateTexture2D(&outDesc, nullptr, &_out)) || FAILED(device->CreateRenderTargetView(_out.Get(), &outRtv, &_outRtv)) ||
        FAILED(device->CreateShaderResourceView(_out.Get(), &outSrv, &_outSrv)))
    {
        _outSrv.Reset();
        _outRtv.Reset();
        _out.Reset();
        _failure = "the puddles target could not be created";
        return false;
    }
    _width = outDesc.Width;
    _height = outDesc.Height;
    _format = uint32_t(engineDesc.Format);
    ++_targetsCreated;
    return true;
}

bool Pass::EnsureGateStatsTarget(ID3D11Device* device)
{
    if (_gateTarget && _gateRtv && _gateStaging && _gateWidth == _width && _gateHeight == _height)
        return true;
    _gateStaging.Reset();
    _gateRtv.Reset();
    _gateTarget.Reset();
    _gateWidth = _gateHeight = 0;
    _gateCopyPending = false; // the copy that was in the queue belonged to the target that is gone

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = _width;
    desc.Height = _height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT(kGateFlagsFormat);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc {};
    rtvDesc.Format = desc.Format;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    // The copy a later run maps without waiting: the whole allocation, because the render window belongs to the run that reads it.
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &_gateTarget)) || FAILED(device->CreateRenderTargetView(_gateTarget.Get(), &rtvDesc, &_gateRtv)) ||
        FAILED(device->CreateTexture2D(&staging, nullptr, &_gateStaging)))
    {
        _gateStaging.Reset();
        _gateRtv.Reset();
        _gateTarget.Reset();
        return false; // the measurement is a diagnostic: the pass runs without it
    }
    _gateWidth = _width;
    _gateHeight = _height;
    return true;
}

void Pass::CollectGateStats(ID3D11DeviceContext* context, const Region& region)
{
    // The copy an earlier run took, read without waiting: a map that has not landed leaves the previous answer in place and is asked again on the
    // next run. A run that was not measured neither copies nor maps anything.
    if (!_gateCopyPending || !_gateStaging)
        return;
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (context->Map(_gateStaging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK || mapped.pData == nullptr)
        return;
    _gateCopyPending = false;
    // A strided read: every kGateStatsStride-th texel in each direction over the region the pass wrote, which is as many samples as a log line
    // needs and keeps the count under a millisecond at 4K.
    GateStats stats;
    for (uint32_t y = region.y; y < region.Bottom() && y < _gateHeight; y += kGateStatsStride)
    {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
        for (uint32_t x = region.x; x < region.Right() && x < _gateWidth; x += kGateStatsStride)
        {
            if (row[size_t(x) * 2 + 0] >= kGateFlagSet)
                ++stats.poolTexels;
            if (row[size_t(x) * 2 + 1] >= kGateFlagSet)
                ++stats.keptTexels;
        }
    }
    _gateStats = stats;
    _gateStatsKnown = true;
    context->Unmap(_gateStaging.Get(), 0);
}

bool Pass::NoteWetness(ID3D11DeviceContext* context, ID3D11Buffer* game)
{
    if (context == nullptr || game == nullptr)
        return false;
    // The engine's wetness buffer is bound on the material draws, which come before the light draw the pass runs at: the first frame's copy is
    // taken before the pass has ever run, so the device objects are created here as well as in Run.
    if (!_wetness)
    {
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (device == nullptr)
            return false;
        if (device.Get() != _device.Get())
        {
            Reset();
            if (!Init(device.Get()))
                return false;
        }
    }
    D3D11_BUFFER_DESC desc {};
    game->GetDesc(&desc);
    if (desc.ByteWidth < kWetnessBytes || (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0)
        return false; // another buffer under the engine's wetness name: not the value the material shaders read
    const D3D11_BOX box = ByteBox(0, kWetnessBytes);
    context->CopySubresourceRegion(_wetness.Get(), 0, 0, 0, 0, game, 0, &box);

    // The engine's own amount, for the run line only: the same 32 bytes into a staging buffer, whose previous frame's copy is mapped without
    // waiting here. The value is a frame behind by construction and never reaches a draw, so a copy the GPU has not finished costs nothing.
    if (_wetnessPending)
    {
        float values[kWetnessBytes / sizeof(float)] {};
        if (MapStagingRows(context, _wetnessStaging[_wetnessPendingSlot].Get(), values, kWetnessBytes))
        {
            _wetnessPending = false;
            if (values[0] == values[0]) // a NaN copy is not an amount
            {
                _engineWetness = values[0];
                _engineWetnessKnown = true;
            }
        }
    }
    // This frame's copy, into the buffer the read above did not take, so a copy is never written over one that is still in the GPU's queue.
    const uint32_t slot = _wetnessPending ? 1u - _wetnessPendingSlot : 0u;
    if (EnsureStaging(_device.Get(), _wetnessStaging[slot], kWetnessBytes))
    {
        context->CopySubresourceRegion(_wetnessStaging[slot].Get(), 0, 0, 0, 0, game, 0, &box);
        _wetnessPending = true;
        _wetnessPendingSlot = slot;
    }
    return true;
}

bool Pass::SetWetness(ID3D11DeviceContext* context, float amount)
{
    if (context == nullptr)
        return false;
    // The wetness buffer starts up in Init; a knob that is armed from the first frame reaches it before any material draw has handed over the
    // game's buffer, so the device objects are created here as well as in NoteWetness and Run.
    if (!_wetness)
    {
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (device == nullptr)
            return false;
        if (device.Get() != _device.Get())
        {
            Reset();
            if (!Init(device.Get()))
                return false;
        }
    }
    // The two components the engine's own wetness entry writes (EnginePassRules.h's WetnessEntry): the amount and the dryness blend that goes with
    // it. The rest of the buffer is zero here because the puddles shader reads .x only, and nothing else samples this copy.
    const float values[kWetnessBytes / sizeof(float)] { amount, 1.0f - amount * 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    context->UpdateSubresource(_wetness.Get(), 0, nullptr, values, 0, 0);
    return true;
}

bool Pass::CheckCamera(ID3D11DeviceContext* context, ID3D11Buffer* game)
{
    _cameraNotReady = false;
    if (_cameraChecked)
        return true;
    if (context == nullptr || game == nullptr || !_device)
        return false;
    if (_cameraRejected && SteadySeconds() < _cameraRetryAt)
        return false;
    // The copy a previous frame took, read without waiting. A copy the GPU has not finished is a frame of delay rather than a refusal, and the
    // check simply asks again on the next frame with the newest table it has.
    if (_cameraPending)
    {
        float rows[kCameraBytes / sizeof(float)] {};
        if (MapStagingRows(context, _cameraStaging[_cameraPendingSlot].Get(), rows, kCameraBytes))
        {
            _cameraPending = false;
            // The one-time layout check: the same one the contact shadow pass makes on its own camera table, so a patch that moves a matrix is
            // refused once instead of shading with nonsense.
            if (!ext::ffxiv::relight::CameraLayoutOk(rows, kCameraBytes / sizeof(float)))
            {
                _failure = "the camera table is not the layout the shaders read";
                _cameraRejected = true;
                _cameraRetryAt = SteadySeconds() + kCameraCheckSeconds;
                return false;
            }
            _cameraChecked = true;
            _cameraRejected = false;
            return true;
        }
        _cameraNotReady = true;
        ++_cameraNotReadyCount;
    }
    // This frame's copy, into the buffer whose earlier copy the read above did not take, so a copy is never written over one that is still in the
    // GPU's queue.
    const uint32_t slot = _cameraPending ? 1u - _cameraPendingSlot : 0u;
    if (!EnsureStaging(_device.Get(), _cameraStaging[slot], kCameraBytes))
    {
        _failure = "the camera check's staging buffer could not be created";
        return false;
    }
    const D3D11_BOX box = ByteBox(0, kCameraBytes);
    context->CopySubresourceRegion(_cameraStaging[slot].Get(), 0, 0, 0, 0, game, 0, &box);
    _cameraPending = true;
    _cameraPendingSlot = slot;
    _cameraNotReady = true; // no answer yet: the copy is in the GPU's queue
    return false;
}

ID3D11ShaderResourceView* Pass::Run(ID3D11DeviceContext* context, const PassInputs& inputs)
{
    _cameraNotReady = false; // the caller reads it after a null return, so it says this run and not an earlier one
    if (context == nullptr || inputs.target == nullptr || inputs.targetTexture == nullptr || inputs.depth == nullptr || inputs.normal == nullptr ||
        inputs.camera == nullptr)
        return nullptr;
    // The pass runs on the shape the pure rules accepted; a caller that skipped them gets the engine's texture back.
    if (Classify(inputs.facts) != Decline::None)
        return nullptr;
    // The part of the target the engine's own geometry pass wrote, and the only part this pass writes.
    const Region region = RegionOf(inputs.facts);
    if (region.Empty())
    {
        _failure = "the render window covers no whole texel of the target";
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

    // The engine's own texture: its size and format are what this module's copy has to be, and the copy is what keeps every texel outside the
    // region the engine's own.
    D3D11_TEXTURE2D_DESC engineDesc {};
    inputs.targetTexture->GetDesc(&engineDesc);
    if (uint32_t(engineDesc.Format) != kTargetFormat && uint32_t(engineDesc.Format) != 90u /* B8G8R8A8_TYPELESS */)
    {
        _failure = "the G-buffer target's format is not one this pass can copy and write";
        return nullptr;
    }
    if (!EnsureTarget(device.Get(), engineDesc))
        return nullptr;
    // The gate's measurement: an earlier run's copy is read first (no wait), and this run keeps its own flags only when it was asked to.
    CollectGateStats(context, _gateRegion);
    const bool gate = inputs.gateStats && EnsureGateStatsTarget(device.Get());

    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
    {
        _failure = "ID3D11DeviceContext1 is not available";
        return nullptr;
    }

    // The camera table and the engine's wetness amount, GPU side: two small copies, no map and no readback.
    const D3D11_BOX cameraBox = ByteBox(0, kCameraBytes);
    context->CopySubresourceRegion(_camera.Get(), 0, 0, 0, 0, inputs.camera, 0, &cameraBox);
    if (!CheckCamera(context, inputs.camera))
        return nullptr;

    // The engine's own texels, so every texel outside the region below is the engine's answer and not an uninitialised target: a reader that
    // samples the whole target (bilinear taps at the region's edge included) sees the picture it saw before. The copy alone does NOT make the
    // output a whole picture of THIS frame's shape - the shape draw below can still fail (a Map on a device-removed frame, a thrown swap) - so
    // the flag that lets a later frame HOLD this output waits for the draw's success at the return. It is also CLEARED here, before the copy:
    // a run that already succeeded once leaves the flag up, and a post-copy failure on a later frame must not promote that frame's UNSHAPED
    // copy through the hold gates (the review's N1 - the F1 black frame, self-healing trigger).
    _outValid = false;
    context->CopySubresourceRegion(_out.Get(), 0, 0, 0, 0, inputs.targetTexture, 0, nullptr);

    // The scalars, one dynamic buffer.
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr)
        return nullptr;
    const Constants constants = ConstantsOf(inputs.facts, inputs.params);
    std::memcpy(mapped.pData, &constants, sizeof constants);
    context->Unmap(_params.Get(), 0);

    // The game's own state is captured here and has to come back, also when the draw throws through this fork's hooks: the guard restores in its
    // destructor, on every path.
    ext::core::explicit_state::Guard stateGuard(context, context1.Get(), device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1, kSpec);
    stateGuard.Neutralise(); // what the private state's defaults provided: no HS, DS, GS, stream-out target or predicate

    const float blendFactor[4] {};
    ID3D11Buffer* buffers[1] { _params.Get() };
    // The draw runs over the region only: the engine's own geometry pass filled that part of the allocation and no other, so the texels outside
    // it keep the copy taken above.
    const D3D11_VIEWPORT viewport { float(region.x), float(region.y), float(region.width), float(region.height), 0.0f, 1.0f };

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(_vs.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, buffers);
    context->RSSetState(_rasterizer.Get());
    context->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);
    context->RSSetViewports(1, &viewport);

    ID3D11ShaderResourceView* views[kBoundSlots] {};
    views[kSlotDepth] = inputs.depth;
    views[kSlotNormal] = inputs.normal;
    views[kSlotTarget] = inputs.target;
    views[kSlotCamera] = _cameraView.Get();
    views[kSlotWetness] = _wetnessView.Get();
    ID3D11RenderTargetView* targets[2] { _outRtv.Get(), gate ? _gateRtv.Get() : nullptr };
    // The flags start at zero, and only a measuring run binds the second target: the shader's write to it is discarded otherwise, which costs a
    // session nothing.
    if (gate)
    {
        const float nothing[4] { 0.0f, 0.0f, 0.0f, 0.0f };
        context->ClearRenderTargetView(_gateRtv.Get(), nothing);
    }
    context->OMSetRenderTargets(gate ? 2u : 1u, targets, nullptr);
    context->PSSetShaderResources(0, kBoundSlots, views);
    context->PSSetShader(_ps.Get(), nullptr, 0);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* none[kBoundSlots] {};
    context->PSSetShaderResources(0, kBoundSlots, none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    // The flags of this run, queued for a later run's no-wait map. Taken after the draw and with no target bound, which is what the copy of a
    // render target requires.
    if (gate && _gateTarget && _gateStaging)
    {
        context->CopyResource(_gateStaging.Get(), _gateTarget.Get());
        _gateCopyPending = true;
        _gateRegion = region;
    }

    _failure = nullptr;
    // The shape draw wrote the window and the copy took the rest: the pass's output is a whole picture of this frame's
    // shape from here on, the answer a later skipped frame keeps (the hold). Set at the success return only - every
    // post-copy failure above leaves the flag down, so a hold can never answer with an unshaped picture.
    _outValid = true;
    return _outSrv.Get();
}
} // namespace ext::ffxiv::engine::gloss
