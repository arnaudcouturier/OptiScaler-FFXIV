#include "pch.h"

#include "ext/ffxiv/engine/AtmoPass.h"
#include "ext/core/D3D11ExplicitState.h"

#include <Atmo_Vs.h>
#include <Atmo_Ps.h>

#include <cstring>

// Atmosphere: the pass (AtmoPass.h). The draw the shader's comment names, one explicit save and restore of the state it touches (a scope guard puts it
// back), one constant buffer mapped D3D11_MAP_WRITE_DISCARD per run, and one timestamp pair inside a disjoint query that is read without ever waiting for the
// GPU (the no-flush flag in the flags argument).
namespace ext::ffxiv::engine::atmo
{
using Microsoft::WRL::ComPtr;

namespace
{
// The register slots and the constant-buffer layout of ext/ffxiv/engine/shaders/Atmo.hlsl. Change them together.
constexpr uint32_t kSlotDepth = 0;
constexpr uint32_t kSlotScene = 1;
constexpr uint32_t kSlots = 2;

// What this pass touches on the game's context, in the Spec's fields (psSrvCount, psSamplerCount, psConstantBuffers): the IA layout and topology, VS and PS,
// pixel-shader resource slots 0..1 (kSlots) and constant buffer 0, the rasterizer state and the viewports, blend, depth-stencil and the render target. No
// sampler is touched: the shader only Loads.
constexpr ext::core::explicit_state::Spec kSpec { 2, 0, 1 };
static_assert(kSpec.psSrvCount == kSlots, "the Spec declares exactly the pass's own pixel-shader slots");

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

// One float4 per row of the shader's own layout (Atmo.hlsl's cbuffer Params; the two files change together).
struct ParamsBuffer
{
    Float4 window;
    Float4 rayScale;
    Float4 invProjRow[4];
    Float4 upView;
    Float4 sunView;
    Float4 medium;
    Float4 light;
    Float4 fogColour;
    Float4 sunTint;
};

static_assert(sizeof(ParamsBuffer) == 12 * 16, "the pass's constant buffer is twelve float4s");

// The scene views: the engine's texture may be typeless, so the copy takes the texture's own format and the views take the FP16 the tone map reads (the
// march's own two-formats rule).
constexpr DXGI_FORMAT kSceneViewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

bool Finite(float value) { return value == value && value > -1.0e30f && value < 1.0e30f; }
} // namespace

void Pass::Reset()
{
    _sceneSrv.Reset();
    _sceneRtv.Reset();
    _scene.Reset();
    _params.Reset();
    _rasterizer.Reset();
    _shader.Reset();
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
    if (FAILED(device->CreateVertexShader(Atmo_Vs_cso, sizeof Atmo_Vs_cso, nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(Atmo_Ps_cso, sizeof Atmo_Ps_cso, nullptr, &_shader)))
    {
        _failure = "the atmosphere shaders could not be created";
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

    // No private pipeline state: the game's own is captured and put back explicitly around the draw (ext/core/D3D11ExplicitState.h).
    // The pass's own GPU time: a timestamp pair inside a disjoint query, read one run later without ever waiting for the GPU. No queries: the measurement is
    // the only thing that is lost (the march's own rule).
    D3D11_QUERY_DESC disjointDesc { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    D3D11_QUERY_DESC stampDesc { D3D11_QUERY_TIMESTAMP, 0 };
    if (FAILED(device->CreateQuery(&disjointDesc, &_disjoint)) || FAILED(device->CreateQuery(&stampDesc, &_begin)) ||
        FAILED(device->CreateQuery(&stampDesc, &_end)))
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
    // The scene copy: the engine's texture may carry a smaller bind-flag set than the pass needs, so the copy asks for both binds of its own.
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

bool Pass::BeginGpuTiming(ID3D11DeviceContext* context)
{
    if (!_disjoint || !_begin || !_end || _timingPending)
        return false; // the previous pair is still in the queue: this run is not timed, and it must not end that pair
    context->Begin(_disjoint.Get());
    context->End(_begin.Get());
    _timingPending = true;
    ++_timingBegins;
    return true;
}

ID3D11ShaderResourceView* Pass::Run(ID3D11DeviceContext* context, const Inputs& inputs)
{
    _failure = nullptr;
    _drawsLastRun = 0;
    if (context == nullptr || inputs.scene == nullptr || inputs.depth == nullptr)
    {
        _failure = "the draw bound no scene or no depth";
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
    if (!shafts::WindowUsable(inputs.window) || inputs.window.x + inputs.window.width > float(inputs.sceneWidth) + 1.0f ||
        inputs.window.y + inputs.window.height > float(inputs.sceneHeight) + 1.0f)
    {
        _failure = "the render window is empty, too small or does not fit the scene";
        return nullptr;
    }
    if (!(inputs.ndcScale[0] > 0.0f) || !(inputs.ndcScale[1] < 0.0f))
    {
        // The mapping row's own flip: the engine's y scale is negative (the march's own read of the same row rejects a non-negative one's frames too).
        _failure = "the pixel-to-NDC row is not one this pass can reconstruct through";
        return nullptr;
    }
    if (!Finite(inputs.ndcScale[0]) || !Finite(inputs.ndcScale[1]) || !Finite(inputs.ndcOffset[0]) || !Finite(inputs.ndcOffset[1]))
    {
        _failure = "the pixel-to-NDC row is not finite";
        return nullptr;
    }
    // The pass asserts that every number it walks is finite, so nothing downstream can reconstruct a NaN position.
    for (uint32_t i = 0; i < 16; ++i)
        if (!Finite(inputs.inverseProjection[i]))
        {
            _failure = "the inverse projection is not finite";
            return nullptr;
        }
    for (uint32_t i = 0; i < 3; ++i)
        if (!Finite(inputs.upView[i]) || !Finite(inputs.sunView[i]) || !Finite(inputs.fogColour[i]) || !Finite(inputs.sunTint[i]))
        {
            _failure = "a direction or a colour the pass was handed is not finite";
            return nullptr;
        }
    if (!Finite(inputs.densityPerMetre) || !Finite(inputs.whiteLinear) || !Finite(inputs.sunGain))
    {
        _failure = "the medium's density, the white or the sun's gain is not finite";
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

    // The scene has to be one texture this pass can copy: a single-sample 2D texture with one mip and one slice, of the size the draw's own viewport and
    // target measured. Anything else is another pass's binding, and the draw keeps the texture it was handed (the march's own rule).
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

    // The stage without something to do answers a fresh copy and no draw: no medium or no light is today's picture at no cost, and the copy the pass makes
    // is byte-exact (a previous run's mixed answer cannot stand in for the handed-in scene, so this branch re-copies rather than reusing its own texture).
    if (!Wanted(true, inputs.params) || !(inputs.densityPerMetre > 0.0f))
    {
        context->CopyResource(_scene.Get(), sceneResource.Get());
        return _sceneSrv.Get();
    }

    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
    {
        _failure = "ID3D11DeviceContext1 is not available";
        return nullptr;
    }

    // This pass's copy of the scene it was handed, then the parameters the draw reads.
    context->CopyResource(_scene.Get(), sceneResource.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr)
    {
        _failure = "the parameter buffer could not be written";
        return nullptr;
    }
    ParamsBuffer params {};
    Set(params.window, inputs.window.x, inputs.window.y, inputs.window.width, inputs.window.height);
    Set(params.rayScale, inputs.ndcScale[0], inputs.ndcScale[1], inputs.ndcOffset[0], inputs.ndcOffset[1]);
    for (uint32_t i = 0; i < 4; ++i)
        Set(params.invProjRow[i], inputs.inverseProjection[i * 4 + 0], inputs.inverseProjection[i * 4 + 1], inputs.inverseProjection[i * 4 + 2],
            inputs.inverseProjection[i * 4 + 3]);
    Set(params.upView, inputs.upView[0], inputs.upView[1], inputs.upView[2], 0.0f);
    Set(params.sunView, inputs.sunView[0], inputs.sunView[1], inputs.sunView[2], inputs.sunGain);
    Set(params.medium, inputs.densityPerMetre, inputs.params.falloff, inputs.params.start, inputs.params.phaseG);
    Set(params.light, inputs.params.skyStrength, inputs.params.sunStrength * inputs.whiteLinear, inputs.params.neutral, 0.0f);
    Set(params.fogColour, inputs.fogColour[0], inputs.fogColour[1], inputs.fogColour[2], 0.0f);
    Set(params.sunTint, inputs.sunTint[0], inputs.sunTint[1], inputs.sunTint[2], 0.0f);
    std::memcpy(mapped.pData, &params, sizeof params);
    context->Unmap(_params.Get(), 0);

    CollectGpuTiming(context);
    // The game's own state has to come back on every path, also when the draw throws through this fork's hooks: the guard restores it in its destructor.
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

    const bool timed = BeginGpuTiming(context);

    // The draw, over the render window, into this pass's copy of the scene: the scene it was handed is the colour source, the depth the fog post bound is
    // the geometry. The window's outside keeps the handed-in picture through the CopyResource above.
    ID3D11ShaderResourceView* views[kSlots] {};
    views[kSlotDepth] = inputs.depth;
    views[kSlotScene] = inputs.scene;
    ID3D11RenderTargetView* rtv = _sceneRtv.Get();
    const D3D11_VIEWPORT viewport { inputs.window.x, inputs.window.y, inputs.window.width, inputs.window.height, 0.0f, 1.0f };
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->PSSetShaderResources(0, kSlots, views);
    context->PSSetShader(_shader.Get(), nullptr, 0);
    context->RSSetViewports(1, &viewport);
    context->Draw(3, 0);
    ++_drawsLastRun;

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
} // namespace ext::ffxiv::engine::atmo
