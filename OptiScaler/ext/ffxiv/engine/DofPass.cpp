#include "pch.h"

#include "ext/ffxiv/engine/DofPass.h"
#include "ext/core/D3D11ExplicitState.h"

#include <Dof_Vs.h>
#include <Dof_Pack.h>
#include <Dof_Blur.h>
#include <Dof_Merge.h>
#include <Dof_PackDistance.h>
#include <Dof_Focus.h>

#include <cstring>

// Depth of field, Route B: the pass (DofPass.h). The three draws the shader's comment names, one explicit save and restore of
// the state they touch (a scope guard puts it back), one constant buffer mapped D3D11_MAP_WRITE_DISCARD per run, and one
// timestamp pair inside a disjoint query that is read without ever waiting for the GPU (the no-flush flag in the flags
// argument).
namespace ext::ffxiv::dof
{
using Microsoft::WRL::ComPtr;

namespace
{
// The register slots and the constant-buffer layout of ext/ffxiv/engine/shaders/Dof.hlsl. Change them together.
constexpr uint32_t kSlotDepth = 0;
constexpr uint32_t kSlotScene = 1;
constexpr uint32_t kSlotPack = 2;
constexpr uint32_t kSlotNear = 3;
constexpr uint32_t kSlotFar = 4;
constexpr uint32_t kSlots = 5;
// The focus state is read where the near half is read later: the focus update and the pack bind nothing at slot 3, so the explicit-state Spec below
// keeps its five slots.
constexpr uint32_t kSlotFocus = 3;

// What this pass touches on the game's context, in the Spec's fields (psSrvCount, psSamplerCount, psConstantBuffers): the IA layout and
// topology, VS and PS, pixel-shader resource slots 0..4 (kSlots) and constant buffer 0, the rasterizer state and the viewports, blend,
// depth-stencil, and the render targets (the blur binds two at once) with the UAV slots. HS, DS, GS, stream-out and predication are what
// the private device-context state used to provide as "none". No sampler is touched: the shaders only Load. Scissor rects and the other
// constant buffers are not read.
constexpr ext::core::explicit_state::Spec kSpec { 5, 0, 1 };
static_assert(kSpec.psSrvCount == kSlots, "the Spec declares exactly the pass's own pixel-shader slots");

// The pass's constant buffer, one float4 per row of the shader's own layout (Dof.hlsl's cbuffer Params; the two files
// change together).
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
    Float4 rayScale;
    Float4 invProjRow[4];
    Float4 centre;
    Float4 tuning;
    Float4 blend;
    Float4 focusMode;
};

static_assert(sizeof(ParamsBuffer) == 10 * 16, "the pass's constant buffer is ten float4s");

// The scene views: the engine's texture may be typeless, so the copy takes the texture's own format and the views take
// the FP16 the tone map reads (the march's own two-formats rule).
constexpr DXGI_FORMAT kSceneViewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// The window's centre pixel, for the auto focus' one texel: the window's own centre, clamped inside the window.
void WindowCentre(const shafts::Window& window, float& centreX, float& centreY)
{
    centreX = window.x + window.width * 0.5f;
    centreY = window.y + window.height * 0.5f;
}
} // namespace

void Pass::Reset()
{
    for (uint32_t i = 0; i < 2; ++i)
    {
        _focusSrv[i].Reset();
        _focusRtv[i].Reset();
        _focus[i].Reset();
    }
    _focusIndex = 0;
    _focusRan = false;
    _focusShader.Reset();
    _packDistanceShader.Reset();
    _farSrv.Reset();
    _farRtv.Reset();
    _far.Reset();
    _nearSrv.Reset();
    _nearRtv.Reset();
    _near.Reset();
    _packSrv.Reset();
    _packRtv.Reset();
    _pack.Reset();
    _sceneSrv.Reset();
    _sceneRtv.Reset();
    _scene.Reset();
    _params.Reset();
    _rasterizer.Reset();
    _mergeShader.Reset();
    _blurShader.Reset();
    _packShader.Reset();
    _vs.Reset();
    _device.Reset();
    _disjoint.Reset();
    _begin.Reset();
    _end.Reset();
    _packWidth = _packHeight = 0;
    _quarterWidth = _quarterHeight = 0;
    _sceneResourceFormat = _sceneWidth = _sceneHeight = 0;
    _scenesCreated = _packsCreated = _quartersCreated = 0;
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
    if (FAILED(device->CreateVertexShader(Dof_Vs_cso, sizeof Dof_Vs_cso, nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(Dof_Pack_cso, sizeof Dof_Pack_cso, nullptr, &_packShader)) ||
        FAILED(device->CreatePixelShader(Dof_Blur_cso, sizeof Dof_Blur_cso, nullptr, &_blurShader)) ||
        FAILED(device->CreatePixelShader(Dof_Merge_cso, sizeof Dof_Merge_cso, nullptr, &_mergeShader)) ||
        FAILED(device->CreatePixelShader(Dof_PackDistance_cso, sizeof Dof_PackDistance_cso, nullptr, &_packDistanceShader)) ||
        FAILED(device->CreatePixelShader(Dof_Focus_cso, sizeof Dof_Focus_cso, nullptr, &_focusShader)))
    {
        _failure = "the depth of field shaders could not be created";
        return false;
    }

    // The focus state: two one-texel textures, zero at the start (a zero focus is "nothing seen yet": the first sample is adopted whole).
    {
        D3D11_TEXTURE2D_DESC focusDesc {};
        focusDesc.Width = 1;
        focusDesc.Height = 1;
        focusDesc.MipLevels = 1;
        focusDesc.ArraySize = 1;
        focusDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
        focusDesc.SampleDesc.Count = 1;
        focusDesc.Usage = D3D11_USAGE_DEFAULT;
        focusDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        const float zero[2] { 0.0f, 0.0f };
        const D3D11_SUBRESOURCE_DATA initial { zero, sizeof zero, 0 };
        for (uint32_t i = 0; i < 2; ++i)
            if (FAILED(device->CreateTexture2D(&focusDesc, &initial, &_focus[i])) ||
                FAILED(device->CreateRenderTargetView(_focus[i].Get(), nullptr, &_focusRtv[i])) ||
                FAILED(device->CreateShaderResourceView(_focus[i].Get(), nullptr, &_focusSrv[i])))
            {
                _failure = "the focus state could not be created";
                return false;
            }
        _focusIndex = 0;
        _focusRan = false;
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
    // The pass's own GPU time: a timestamp pair inside a disjoint query, read one run later without ever waiting for the
    // GPU. No queries: the measurement is the only thing that is lost (the march's own rule).
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

bool Pass::EnsureTargets(ID3D11Device* device, D3D11_TEXTURE2D_DESC sceneDesc, uint32_t windowWidth, uint32_t windowHeight)
{
    const uint32_t quarterWidth = QuarterExtent(float(windowWidth));
    const uint32_t quarterHeight = QuarterExtent(float(windowHeight));
    if (quarterWidth == 0 || quarterHeight == 0 || windowWidth == 0 || windowHeight == 0)
    {
        _failure = "the render window's quarter extent is empty";
        return false;
    }

    // The scene copy: the march's own EnsureScene, its own bind set; the views are the FP16 the tone map reads. The
    // copy's own description: the engine's texture may carry a smaller bind-flag set than the pass needs, so the copy
    // asks for both binds of its own (its own desc rather than the input's).
    D3D11_TEXTURE2D_DESC copyDesc = sceneDesc;
    copyDesc.MipLevels = 1;
    copyDesc.ArraySize = 1;
    copyDesc.SampleDesc.Count = 1;
    copyDesc.SampleDesc.Quality = 0;
    copyDesc.Usage = D3D11_USAGE_DEFAULT;
    copyDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    copyDesc.CPUAccessFlags = 0;
    copyDesc.MiscFlags = 0;
    if (!_scene || _sceneResourceFormat != uint32_t(copyDesc.Format) || _sceneWidth != copyDesc.Width ||
        _sceneHeight != copyDesc.Height)
    {
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
        if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, &_scene)) ||
            FAILED(device->CreateRenderTargetView(_scene.Get(), &rtv, &_sceneRtv)) ||
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
    }

    // The pack: one FP16 RGBA texture at the scene's own allocation shape (the copy's own desc: the same bind set).
    if (!_pack || _packWidth != copyDesc.Width || _packHeight != copyDesc.Height)
    {
        _packSrv.Reset();
        _packRtv.Reset();
        _pack.Reset();
        _packWidth = _packHeight = 0;
        D3D11_TEXTURE2D_DESC packDesc = copyDesc;
        packDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        D3D11_RENDER_TARGET_VIEW_DESC rtv {};
        rtv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateTexture2D(&packDesc, nullptr, &_pack)) ||
            FAILED(device->CreateRenderTargetView(_pack.Get(), &rtv, &_packRtv)) ||
            FAILED(device->CreateShaderResourceView(_pack.Get(), &srv, &_packSrv)))
        {
            _packSrv.Reset();
            _packRtv.Reset();
            _pack.Reset();
            _failure = "the pack target could not be created";
            return false;
        }
        _packWidth = sceneDesc.Width;
        _packHeight = sceneDesc.Height;
        ++_packsCreated;
    }

    // The quarter pair: the near and the far half, each one FP16 RGBA texture at the quarter of the render window.
    if (!_near || !_far || _quarterWidth != quarterWidth || _quarterHeight != quarterHeight)
    {
        _nearSrv.Reset();
        _nearRtv.Reset();
        _near.Reset();
        _farSrv.Reset();
        _farRtv.Reset();
        _far.Reset();
        _quarterWidth = _quarterHeight = 0;
        D3D11_TEXTURE2D_DESC quarterDesc = copyDesc;
        quarterDesc.Width = quarterWidth;
        quarterDesc.Height = quarterHeight;
        quarterDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        D3D11_RENDER_TARGET_VIEW_DESC rtv {};
        rtv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateTexture2D(&quarterDesc, nullptr, &_near)) ||
            FAILED(device->CreateRenderTargetView(_near.Get(), &rtv, &_nearRtv)) ||
            FAILED(device->CreateShaderResourceView(_near.Get(), &srv, &_nearSrv)) ||
            FAILED(device->CreateTexture2D(&quarterDesc, nullptr, &_far)) ||
            FAILED(device->CreateRenderTargetView(_far.Get(), &rtv, &_farRtv)) ||
            FAILED(device->CreateShaderResourceView(_far.Get(), &srv, &_farSrv)))
        {
            _nearSrv.Reset();
            _nearRtv.Reset();
            _near.Reset();
            _farSrv.Reset();
            _farRtv.Reset();
            _far.Reset();
            _failure = "the quarter targets could not be created";
            return false;
        }
        _quarterWidth = quarterWidth;
        _quarterHeight = quarterHeight;
        ++_quartersCreated;
    }
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
    if (!WindowUsable(inputs.window) || !QuarterTargetsPossible(inputs.window))
    {
        _failure = "the render window is empty, too small or does not fit the scene";
        return nullptr;
    }
    if (!(inputs.ndcScale[0] > 0.0f) || !(inputs.ndcScale[1] < 0.0f))
    {
        // The mapping row's own flip: the engine's y scale is negative (the march's own read of the same row rejects a
        // non-negative one's frames too). A row this pass cannot reconstruct the window's depths through packs no blur.
        _failure = "the pixel-to-NDC row is not one this pass can reconstruct through";
        return nullptr;
    }
    if (!(inputs.ndcScale[0] == inputs.ndcScale[0]) || !(inputs.ndcScale[1] == inputs.ndcScale[1]) ||
        !(inputs.ndcOffset[0] == inputs.ndcOffset[0]) || !(inputs.ndcOffset[1] == inputs.ndcOffset[1]))
    {
        _failure = "the pixel-to-NDC row is not finite";
        return nullptr;
    }
    // The camera rows were accepted by the caller's own band rule (DofRules.h CameraDepthBand); the pass asserts that
    // the ones it walks are finite, so nothing downstream can reconstruct a NaN metres value.
    for (uint32_t i = 0; i < 16; ++i)
        if (!(inputs.inverseProjection[i] == inputs.inverseProjection[i]))
        {
            _failure = "the inverse projection is not finite";
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

    // The scene has to be one texture this pass can copy: a single-sample 2D texture with one mip and one slice, of the
    // size the draw's own viewport and target measured. Anything else is another pass's binding, and the draw keeps the
    // texture it was handed (the march's own rule).
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
    if (sceneDesc.MipLevels != 1 || sceneDesc.ArraySize != 1 || sceneDesc.SampleDesc.Count != 1 ||
        sceneDesc.Width != inputs.sceneWidth || sceneDesc.Height != inputs.sceneHeight)
    {
        _failure = "the scene is not the single-sample texture the draw measured";
        return nullptr;
    }
    if (!EnsureTargets(device.Get(), sceneDesc, uint32_t(inputs.window.width), uint32_t(inputs.window.height)))
        return nullptr;

    // The stage without something to do answers a fresh copy and no draws: strength 0 (or an aperture at the floor)
    // is today's picture at no cost, and the copy the pass makes is byte-exact (a previous run's merged answer cannot
    // stand in for the handed-in scene, so this branch re-copies rather than reusing its own texture - the identity at
    // that key is one CopyResource and nothing else). The draw path's Wanted has the same rule; this asserts it.
    if (!Wanted(true, inputs.params))
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

    // This pass's copy of the scene it was handed, then the parameters the draws read.
    context->CopyResource(_scene.Get(), sceneResource.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr)
    {
        _failure = "the parameter buffer could not be written";
        return nullptr;
    }
    float centreX = 0.0f, centreY = 0.0f;
    WindowCentre(inputs.window, centreX, centreY);
    ParamsBuffer params {};
    Set(params.window, inputs.window.x, inputs.window.y, inputs.window.width, inputs.window.height);
    Set(params.rayScale, inputs.ndcScale[0], inputs.ndcScale[1], inputs.ndcOffset[0], inputs.ndcOffset[1]);
    for (uint32_t i = 0; i < 4; ++i)
        Set(params.invProjRow[i], inputs.inverseProjection[i * 4 + 0], inputs.inverseProjection[i * 4 + 1],
            inputs.inverseProjection[i * 4 + 2], inputs.inverseProjection[i * 4 + 3]);
    // Centre.z is the time since the pass last ran and Centre.w the focus cross's tap offset in pixels (the focus update reads both; the cinematic pack
    // reads the centre alone).
    const float deltaSeconds = inputs.deltaSeconds > 0.0f ? (inputs.deltaSeconds < 3600.0f ? inputs.deltaSeconds : 3600.0f) : 0.0f; // NaN and negative: no time passed
    Set(params.centre, centreX, centreY, deltaSeconds, kFocusTapOffsetFraction * inputs.window.height);
    // The look decides the reach and the strength on the CPU, so PS_Blur and PS_Merge are the first release's code for both looks.
    Set(params.tuning, float(inputs.params.steps), ReachPixelsFor(inputs.params.mode, inputs.window.height, inputs.params.aperture), 0.0f, 0.0f);
    Set(params.blend, EffectiveStrength(inputs.params.mode, inputs.params.strength), kPackColourCap, inputs.params.focusDistance, 0.0f);
    Set(params.focusMode, inputs.params.focusMode == FocusMode::Manual ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
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

    // The pair covers all three draws: the pack, the blur and the merge are one pass.
    const bool timed = BeginGpuTiming(context);

    // 0. The distance look's focus update, one texel: the previous state (slot 3) and the depth in, the next state out; then the swap, so the pack
    //    below reads the result of this very run.
    const bool distanceLook = inputs.params.mode == Mode::Distance;
    const bool focusUpdate = inputs.params.mode == Mode::Distance && inputs.params.focusMode == FocusMode::Auto;
    if (focusUpdate)
    {
        ID3D11ShaderResourceView* focusViews[kSlots] {};
        focusViews[kSlotDepth] = inputs.depth;
        focusViews[kSlotFocus] = _focusSrv[_focusIndex].Get();
        const uint32_t next = 1u - _focusIndex;
        ID3D11RenderTargetView* focusRtv = _focusRtv[next].Get();
        const D3D11_VIEWPORT focusViewport { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
        context->OMSetRenderTargets(1, &focusRtv, nullptr);
        context->PSSetShaderResources(0, kSlots, focusViews);
        context->PSSetShader(_focusShader.Get(), nullptr, 0);
        context->RSSetViewports(1, &focusViewport);
        context->Draw(3, 0);
        ++_drawsLastRun;
        _focusIndex = next;
        _focusRan = true;
    }

    // 1. The pack, at the render window: rgb = the capped colour (the game's own min(col,10)), a = the signed CoC (the look's own pack). The render
    //    target is bound before the views: the focus update's target is read as the focus state here.
    ID3D11ShaderResourceView* packViews[kSlots] {};
    packViews[kSlotDepth] = inputs.depth;
    packViews[kSlotScene] = inputs.scene;
    if (focusUpdate)
        packViews[kSlotFocus] = _focusSrv[_focusIndex].Get();
    ID3D11RenderTargetView* packRtv = _packRtv.Get();
    const D3D11_VIEWPORT packViewport { inputs.window.x, inputs.window.y, inputs.window.width, inputs.window.height,
                                        0.0f, 1.0f };
    context->OMSetRenderTargets(1, &packRtv, nullptr);
    context->PSSetShaderResources(0, kSlots, packViews);
    context->PSSetShader(distanceLook ? _packDistanceShader.Get() : _packShader.Get(), nullptr, 0);
    context->RSSetViewports(1, &packViewport);
    context->Draw(3, 0);
    ++_drawsLastRun;

    // 2. The disc blur, at a quarter of the window: two targets bound at once (the game's own DownScale4x4 writes two
    //    640x360), the near half into the first and the far half into the second. The pack was the previous draw's
    //    render target, so the render targets switch before its view is bound - the march's own reason.
    ID3D11ShaderResourceView* blurViews[kSlots] {};
    blurViews[kSlotPack] = _packSrv.Get();
    ID3D11RenderTargetView* quarterRtvs[2] { _nearRtv.Get(), _farRtv.Get() };
    const D3D11_VIEWPORT quarterViewport { 0.0f, 0.0f, float(_quarterWidth), float(_quarterHeight), 0.0f, 1.0f };
    context->OMSetRenderTargets(2, quarterRtvs, nullptr);
    context->PSSetShaderResources(0, kSlots, blurViews);
    context->PSSetShader(_blurShader.Get(), nullptr, 0);
    context->RSSetViewports(1, &quarterViewport);
    context->Draw(3, 0);
    ++_drawsLastRun;

    // 3. The merge, over the render window, into this pass's copy of the scene: the scene it was handed is the colour
    //    source (never the pack's capped copy), the pack's alpha is the amount map, and the two quarter targets are the
    //    blur's halves. The quarter pair and the pack were previous draws' render targets, so the render target
    //    switches before their views bind again - the march's own reason.
    ID3D11ShaderResourceView* mergeViews[kSlots] {};
    mergeViews[kSlotScene] = inputs.scene;
    mergeViews[kSlotPack] = _packSrv.Get();
    mergeViews[kSlotNear] = _nearSrv.Get();
    mergeViews[kSlotFar] = _farSrv.Get();
    ID3D11RenderTargetView* sceneRtv = _sceneRtv.Get();
    context->OMSetRenderTargets(1, &sceneRtv, nullptr);
    context->RSSetViewports(1, &packViewport);
    context->PSSetShaderResources(0, kSlots, mergeViews);
    context->PSSetShader(_mergeShader.Get(), nullptr, 0);
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
} // namespace ext::ffxiv::dof
