#include "pch.h"

#include "ext/ffxiv/engine/GiPass.h"
#include "ext/core/D3D11ExplicitState.h"

#include <Gi_Apply.h>
#include <Gi_Gi.h>
#include <Gi_Smooth.h>
#include <Gi_Vs.h>

#include <cstring>

namespace ext::ffxiv::engine::gi
{
using Microsoft::WRL::ComPtr;

namespace
{
// The register slots and the constant-buffer layout of ext/ffxiv/engine/shaders/Gi.hlsl. Change them together.
constexpr uint32_t kSlotDepth = 0;
constexpr uint32_t kSlotViewPos = 1;
constexpr uint32_t kSlotNormal = 2;
constexpr uint32_t kSlotAlbedo = 3;
constexpr uint32_t kSlotScene = 4;
constexpr uint32_t kSlotMaterial = 5;
constexpr uint32_t kSlotIrradiance = 6;
constexpr uint32_t kSlots = 7;

// What this pass touches on the game's context, in the Spec's fields (psSrvCount, psSamplerCount, psConstantBuffers): the IA layout and
// topology, VS and PS, pixel-shader resource slots 0..6 (kSlots) and constant buffer 0, the rasterizer state and the viewports, blend,
// depth-stencil, and the render targets with the UAV slots. HS, DS, GS, stream-out and predication are what the private device-context
// state used to provide as "none". No sampler is touched: the shaders only Load. Scissor rects and the other constant buffers are not read.
constexpr ext::core::explicit_state::Spec kSpec { 7, 0, 1 };
static_assert(kSpec.psSrvCount == kSlots, "the Spec declares exactly the pass's own pixel-shader slots");

// The pass's constant buffer, one float4 per row of the shader's own layout (Gi.hlsl's cbuffer Params; the two files change together).
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
    Float4 window;
    Float4 tunings;
    Float4 range;
    Float4 bound;
    Float4 viewRow[3];
    Float4 projRow[4];
};

static_assert(sizeof(ParamsBuffer) == 11 * 16, "the pass's constant buffer is eleven float4s");

// The scene's views: the engine's texture may be typeless, so the copy takes the texture's own format and the views take the FP16 the tone
// map reads (the march's own arrangement).
constexpr DXGI_FORMAT kSceneViewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// The irradiance and the smooth store rgba: the estimate's own channels and, in the first target, the rays the origin marched (the data marker).
constexpr DXGI_FORMAT kStageFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
} // namespace

void Pass::Reset()
{
    _smoothSrv.Reset();
    _smoothRtv.Reset();
    _smooth.Reset();
    _irradianceSrv.Reset();
    _irradianceRtv.Reset();
    _irradiance.Reset();
    _sceneSrv.Reset();
    _sceneRtv.Reset();
    _scene.Reset();
    _applyShader.Reset();
    _smoothShader.Reset();
    _giShader.Reset();
    _vs.Reset();
    _params.Reset();
    _rasterizer.Reset();
    _device.Reset();
    _disjoint.Reset();
    _begin.Reset();
    _end.Reset();
    _sceneResourceFormat = _sceneWidth = _sceneHeight = 0;
    _irradianceWidth = _irradianceHeight = 0;
    _scenesCreated = _irradianceCreated = 0;
    _timingPending = false;
    _timingBegins = _timingEnds = 0;
    _haveGpuMs = false;
    _gpuMs = 0.0;
    _failure = nullptr;
}

bool Pass::Init(ID3D11Device* device)
{
    if (device == nullptr)
        return false;
    if (FAILED(device->CreateVertexShader(Gi_Vs_cso, sizeof Gi_Vs_cso, nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(Gi_Gi_cso, sizeof Gi_Gi_cso, nullptr, &_giShader)) ||
        FAILED(device->CreatePixelShader(Gi_Smooth_cso, sizeof Gi_Smooth_cso, nullptr, &_smoothShader)) ||
        FAILED(device->CreatePixelShader(Gi_Apply_cso, sizeof Gi_Apply_cso, nullptr, &_applyShader)))
    {
        _failure = "the light bounce's shaders could not be created";
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

    // No private pipeline state: the game's own is captured and put back explicitly around the draws (ext/core/D3D11ExplicitState.h).
    // The pass's own GPU time: a timestamp pair inside a disjoint query, read one run later without ever waiting for the GPU.
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

bool Pass::EnsureScene(ID3D11Device* device, uint32_t format, uint32_t width, uint32_t height)
{
    if (_scene && _sceneResourceFormat == format && _sceneWidth == width && _sceneHeight == height)
        return true;
    _sceneSrv.Reset();
    _sceneRtv.Reset();
    _scene.Reset();
    _sceneResourceFormat = _sceneWidth = _sceneHeight = 0;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = static_cast<DXGI_FORMAT>(format); // the engine's own, so CopyResource accepts the copy
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC rtv {};
    rtv.Format = kSceneViewFormat;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = kSceneViewFormat;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &_scene)) || FAILED(device->CreateRenderTargetView(_scene.Get(), &rtv, &_sceneRtv)) ||
        FAILED(device->CreateShaderResourceView(_scene.Get(), &srv, &_sceneSrv)))
    {
        _sceneSrv.Reset();
        _sceneRtv.Reset();
        _scene.Reset();
        _failure = "this pass's copy of the scene could not be created";
        return false;
    }
    _sceneResourceFormat = format;
    _sceneWidth = width;
    _sceneHeight = height;
    ++_scenesCreated;
    return true;
}

bool Pass::EnsureStageTargets(ID3D11Device* device, uint32_t width, uint32_t height)
{
    if (_irradiance && _irradianceWidth == width && _irradianceHeight == height && _smooth)
        return true;
    _irradianceSrv.Reset();
    _irradianceRtv.Reset();
    _irradiance.Reset();
    _smoothSrv.Reset();
    _smoothRtv.Reset();
    _smooth.Reset();
    _irradianceWidth = _irradianceHeight = 0;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = kStageFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC rtv {};
    rtv.Format = kStageFormat;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = kStageFormat;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &_irradiance)) ||
        FAILED(device->CreateRenderTargetView(_irradiance.Get(), &rtv, &_irradianceRtv)) ||
        FAILED(device->CreateShaderResourceView(_irradiance.Get(), &srv, &_irradianceSrv)))
    {
        _irradianceSrv.Reset();
        _irradianceRtv.Reset();
        _irradiance.Reset();
        _smoothSrv.Reset();
        _smoothRtv.Reset();
        _smooth.Reset();
        _failure = "the irradiance target could not be created";
        return false;
    }
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &_smooth)) ||
        FAILED(device->CreateRenderTargetView(_smooth.Get(), &rtv, &_smoothRtv)) ||
        FAILED(device->CreateShaderResourceView(_smooth.Get(), &srv, &_smoothSrv)))
    {
        _smoothSrv.Reset();
        _smoothRtv.Reset();
        _smooth.Reset();
        _failure = "the smooth target could not be created";
        return false;
    }
    _irradianceWidth = width;
    _irradianceHeight = height;
    ++_irradianceCreated;
    return true;
}

void Pass::CollectGpuTiming(ID3D11DeviceContext* context)
{
    if (!_timingPending || !_disjoint || !_begin || !_end)
        return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint {};
    // The read never waits and never flushes: a query that is not finished is looked at again on the next run.
    if (context->GetData(_disjoint.Get(), &disjoint, sizeof disjoint, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return; // not finished: looked at again on the next run, never waited for
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

ID3D11ShaderResourceView* Pass::Run(ID3D11DeviceContext* context, const Inputs& inputs)
{
    _failure = nullptr;
    if (context == nullptr || inputs.scene == nullptr || inputs.depth == nullptr || inputs.viewPos == nullptr ||
        inputs.normal == nullptr || inputs.albedo == nullptr || inputs.material == nullptr)
    {
        _failure = "the draw bound no scene, no depth, no view positions or no G-buffer views";
        return nullptr;
    }
    if (!shafts::IsSceneFormat(inputs.sceneFormat))
    {
        _failure = "the scene is not FP16";
        return nullptr;
    }
    if (!shafts::IsDepthFormat(inputs.depthFormat))
    {
        _failure = "the depth is not a format this pass reads";
        return nullptr;
    }
    if (!shafts::WindowUsable(inputs.window))
    {
        _failure = "the render window is empty or does not fit the scene";
        return nullptr;
    }
    const uint32_t giWidth = GiExtent(inputs.window.width, inputs.params.resolution);
    const uint32_t giHeight = GiExtent(inputs.window.height, inputs.params.resolution);
    if (giWidth == 0 || giHeight == 0)
    {
        _failure = "the render window's pass extent is empty";
        return nullptr;
    }
    if (!(inputs.params.rays > 0) || !(inputs.params.steps > 0) || !(inputs.params.range > 0.0f) ||
        !(inputs.fade > 0.0f) || !(inputs.whitePoint == inputs.whitePoint))
    {
        _failure = "the pass's state is not one it can evaluate";
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

    // The scene has to be one texture this pass can copy: a single-sample 2D texture with one mip and one slice, of the size the draw's own
    // viewport and target measured. Anything else is another pass's binding, and the draw keeps the texture it was handed.
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
    if (!EnsureScene(device.Get(), uint32_t(sceneDesc.Format), sceneDesc.Width, sceneDesc.Height) ||
        !EnsureStageTargets(device.Get(), giWidth, giHeight))
        return nullptr;

    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
    {
        _failure = "ID3D11DeviceContext1 is not available";
        return nullptr;
    }

    // This pass's copy of the scene it was handed, then the parameters the draws read.
    context->CopyResource(_scene.Get(), sceneResource.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr)
    {
        _failure = "the parameter buffer could not be written";
        return nullptr;
    }
    ParamsBuffer params {};
    Set(params.window, inputs.window.x, inputs.window.y, inputs.window.width, inputs.window.height);
    Set(params.tunings, float(inputs.params.rays), float(inputs.params.steps), 0.0f,
        ClampStrength(inputs.params.strength) * kGain);
    Set(params.range, inputs.params.range,
        inputs.params.resolution == Resolution::Half ? 2.0f : 4.0f, 0.0f, 0.0f);
    Set(params.bound, inputs.whitePoint, inputs.fade, 0.0f, 0.0f);
    for (uint32_t row = 0; row < 3; ++row)
        Set(params.viewRow[row], inputs.view[row * 4 + 0], inputs.view[row * 4 + 1], inputs.view[row * 4 + 2],
            inputs.view[row * 4 + 3]);
    for (uint32_t row = 0; row < 4; ++row)
        Set(params.projRow[row], inputs.projection[row * 4 + 0], inputs.projection[row * 4 + 1], inputs.projection[row * 4 + 2],
            inputs.projection[row * 4 + 3]);
    std::memcpy(mapped.pData, &params, sizeof params);
    context->Unmap(_params.Get(), 0);

    CollectGpuTiming(context);
    // The game's own state has to come back on every path, also when one of the draws throws through this fork's hooks: the guard restores it in
    // its destructor.
    ext::core::explicit_state::Guard stateGuard(context, context1.Get(), device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1, kSpec);
    stateGuard.Neutralise(); // what the private state's defaults provided: no HS, DS, GS, stream-out target or predicate

    const float blendFactor[4] {};
    ID3D11Buffer* buffers[1] { _params.Get() };
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(_vs.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, buffers);
    context->RSSetState(_rasterizer.Get());
    context->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);

    // The pair covers all three draws: the irradiance, the smooth and the apply are one pass.
    const bool timed = BeginGpuTiming(context);

    // 1. The irradiance, at the pass's own resolution of the render window.
    ID3D11ShaderResourceView* giViews[kSlots] {};
    giViews[kSlotDepth] = inputs.depth;
    giViews[kSlotViewPos] = inputs.viewPos;
    giViews[kSlotNormal] = inputs.normal;
    giViews[kSlotAlbedo] = inputs.albedo;
    giViews[kSlotScene] = inputs.scene;
    ID3D11RenderTargetView* irradianceRtv = _irradianceRtv.Get();
    const D3D11_VIEWPORT giViewport { 0.0f, 0.0f, float(giWidth), float(giHeight), 0.0f, 1.0f };
    context->PSSetShaderResources(0, kSlots, giViews);
    context->PSSetShader(_giShader.Get(), nullptr, 0);
    context->OMSetRenderTargets(1, &irradianceRtv, nullptr);
    context->RSSetViewports(1, &giViewport);
    context->Draw(3, 0);

    // 2. The smooth, over the same extent: the irradiance's dense 5x5 binomial stencil (it binds the irradiance alone: no depth or normal
    //    weight, so it averages across a depth step as the old ring did).
    ID3D11ShaderResourceView* smoothViews[kSlots] {};
    smoothViews[kSlotIrradiance] = _irradianceSrv.Get();
    ID3D11RenderTargetView* smoothRtv = _smoothRtv.Get();
    // The target changes BEFORE the irradiance is bound for reading: while the irradiance is still the bound render target, binding it as a
    // shader resource makes the runtime drop the read (a hazard) and the smooth would sample nothing.
    context->OMSetRenderTargets(1, &smoothRtv, nullptr);
    context->PSSetShaderResources(0, kSlots, smoothViews);
    context->PSSetShader(_smoothShader.Get(), nullptr, 0);
    context->Draw(3, 0);

    // 3. The apply, over the render window, into this pass's copy of the scene. The scene it was handed is the shader resource here (read
    //    only), and the render target is this pass's copy - the two are different textures, so the copy is written while the handed scene is
    //    read (the march's own arrangement).
    ID3D11ShaderResourceView* applyViews[kSlots] {};
    applyViews[kSlotDepth] = inputs.depth;
    applyViews[kSlotAlbedo] = inputs.albedo;
    applyViews[kSlotScene] = inputs.scene;
    applyViews[kSlotMaterial] = inputs.material;
    applyViews[kSlotIrradiance] = _smoothSrv.Get();
    ID3D11RenderTargetView* sceneRtv = _sceneRtv.Get();
    const D3D11_VIEWPORT windowViewport { inputs.window.x, inputs.window.y, inputs.window.width, inputs.window.height, 0.0f, 1.0f };
    context->OMSetRenderTargets(1, &sceneRtv, nullptr);
    context->RSSetViewports(1, &windowViewport);
    context->PSSetShaderResources(0, kSlots, applyViews);
    context->PSSetShader(_applyShader.Get(), nullptr, 0);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* none[kSlots] {};
    context->PSSetShaderResources(0, kSlots, none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    if (timed && _end && _disjoint)
    {
        context->End(_end.Get());
        context->End(_disjoint.Get());
        ++_timingEnds;
        // `_timingPending` stays set: the pair is in the queue until CollectGpuTiming reads it on a later run.
    }
    return _sceneSrv.Get();
}
} // namespace ext::ffxiv::engine::gi
