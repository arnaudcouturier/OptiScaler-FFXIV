#include "pch.h"

#include "ext/ffxiv/engine/GodraysPass.h"
#include "ext/core/D3D11ExplicitState.h"

#include <Godrays_Composite.h>
#include <Godrays_March.h>
#include <Godrays_Stats.h>
#include <Godrays_SunColour.h>
#include <Godrays_Vs.h>

#include <cstring>

namespace ext::ffxiv::godrays
{
using Microsoft::WRL::ComPtr;

namespace
{
// The register slots and the constant-buffer layout of ext/ffxiv/engine/shaders/Godrays.hlsl. Change them together.
constexpr uint32_t kSlotDepth = 0;
constexpr uint32_t kSlotAtlas = 1;
constexpr uint32_t kSlotScene = 2;
constexpr uint32_t kSlotTerm = 3;
constexpr uint32_t kSlotSky = 4;
constexpr uint32_t kSlotLut = 5;
constexpr uint32_t kSlotSunTint = 6;
constexpr uint32_t kSlotSkyCeil = 7;
constexpr uint32_t kSlots = 8;

// What this pass touches on the game's context, in the Spec's fields (psSrvCount, psSamplerCount, psConstantBuffers): the IA layout and
// topology, VS and PS, pixel-shader resource slots 0..7 (kSlots) and constant buffer 0, the rasterizer state and the viewports, blend,
// depth-stencil, and the render targets with the UAV slots. HS, DS, GS, stream-out and predication are what the private device-context
// state used to provide as "none". No sampler is touched: the shaders only Load. Scissor rects and the other constant buffers are not read.
// The compute stage is not in the Guard's reach: the stats dispatch saves and restores its own (ComputeStateRestore below).
constexpr ext::core::explicit_state::Spec kSpec { 8, 0, 1 };
static_assert(kSpec.psSrvCount == kSlots, "the Spec declares exactly the pass's own pixel-shader slots");

// The march's constant buffer, one float4 per row of the shader's own layout (Godrays.hlsl's cbuffer Params; the two files change
// together). The cascades' rows are written in the march's own order (finest first), so the shader's cascade loop is the order.
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
    Float4 camera;
    Float4 lightDir;
    Float4 fogA;
    Float4 fogB;
    Float4 fogC;
    Float4 tuning;
    Float4 colour;
    Float4 bound;
    Float4 sunScreen;
    Float4 fogColour;
    Float4 fogLut;
    struct Cascade
    {
        Float4 row[4];
        Float4 fade;
        Float4 ramp;
    } cascades[kCascadeCount];
    Float4 atlasParam;
    Float4 medium;
    Float4 upsample;
};

static_assert(sizeof(ParamsBuffer) == 44 * 16, "the march's constant buffer is forty-four float4s");

// The compute state the stats dispatch changes, put back when the guard goes out of scope: the compute shader (with its class instances), constant
// buffer 0, shader-resource slots 0..3 and unordered-access slot 0. The explicit-state Guard does not read or write the compute stage.
class ComputeStateRestore
{
  public:
    explicit ComputeStateRestore(ID3D11DeviceContext* context) : _context(context)
    {
        _context->CSGetConstantBuffers(0, kBuffers, _buffers);
        _context->CSGetShaderResources(0, kViews, _views);
        _context->CSGetUnorderedAccessViews(0, kUavs, _uavs);
        _count = kMaxInstances;
        _context->CSGetShader(&_shader, _instances, &_count);
        if (_count > kMaxInstances)
            _count = kMaxInstances;
    }

    ~ComputeStateRestore()
    {
        _context->CSSetShader(_shader.Get(), _count > 0 ? _instances : nullptr, _count);
        _context->CSSetConstantBuffers(0, kBuffers, _buffers);
        _context->CSSetShaderResources(0, kViews, _views);
        _context->CSSetUnorderedAccessViews(0, kUavs, _uavs, nullptr);
        for (uint32_t i = 0; i < _count; ++i)
            if (_instances[i] != nullptr)
                _instances[i]->Release();
        for (ID3D11Buffer* buffer : _buffers)
            if (buffer != nullptr)
                buffer->Release();
        for (ID3D11ShaderResourceView* view : _views)
            if (view != nullptr)
                view->Release();
        for (ID3D11UnorderedAccessView* view : _uavs)
            if (view != nullptr)
                view->Release();
    }

    ComputeStateRestore(const ComputeStateRestore&) = delete;
    ComputeStateRestore& operator=(const ComputeStateRestore&) = delete;

  private:
    static constexpr uint32_t kBuffers = 1;
    static constexpr uint32_t kViews = 4;
    static constexpr uint32_t kUavs = 1;
    static constexpr uint32_t kMaxInstances = 64;
    ID3D11DeviceContext* _context = nullptr;
    ComPtr<ID3D11ComputeShader> _shader;
    ID3D11ClassInstance* _instances[kMaxInstances] {};
    uint32_t _count = 0;
    ID3D11Buffer* _buffers[kBuffers] {};
    ID3D11ShaderResourceView* _views[kViews] {};
    ID3D11UnorderedAccessView* _uavs[kUavs] {};
};

// The scene's views: the engine's texture may be typeless, so the copy takes the texture's own format and the views take the FP16 the
// tone map reads.
constexpr DXGI_FORMAT kSceneViewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
} // namespace

void Pass::Reset()
{
    _statsSrv.Reset();
    _statsUav.Reset();
    _stats.Reset();
    _statsShader.Reset();
    _sunColourSrv.Reset();
    _sunColourRtv.Reset();
    _sunColour.Reset();
    _termSrv.Reset();
    _termRtv.Reset();
    _term.Reset();
    _sceneSrv.Reset();
    _sceneRtv.Reset();
    _scene.Reset();
    _params.Reset();
    _rasterizer.Reset();
    _compositeShader.Reset();
    _sunColourShader.Reset();
    _marchShader.Reset();
    _vs.Reset();
    _device.Reset();
    _disjoint.Reset();
    _begin.Reset();
    _end.Reset();
    _sceneResourceFormat = _sceneWidth = _sceneHeight = 0;
    _marchWidth = _marchHeight = 0;
    _scenesCreated = _marchCreated = _sunColourCreated = 0;
    _timingPending = false;
    _haveGpuMs = false;
    _gpuMs = 0.0;
    _failure = nullptr;
}

bool Pass::Init(ID3D11Device* device)
{
    if (device == nullptr)
        return false;
    if (FAILED(device->CreateVertexShader(Godrays_Vs_cso, sizeof Godrays_Vs_cso, nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(Godrays_March_cso, sizeof Godrays_March_cso, nullptr, &_marchShader)) ||
        FAILED(device->CreatePixelShader(Godrays_SunColour_cso, sizeof Godrays_SunColour_cso, nullptr, &_sunColourShader)) ||
        FAILED(device->CreatePixelShader(Godrays_Composite_cso, sizeof Godrays_Composite_cso, nullptr, &_compositeShader)) ||
        FAILED(device->CreateComputeShader(Godrays_Stats_cso, sizeof Godrays_Stats_cso, nullptr, &_statsShader)))
    {
        _failure = "the god rays shaders could not be created";
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

bool Pass::EnsureMarch(ID3D11Device* device, uint32_t width, uint32_t height)
{
    if (_term && _marchWidth == width && _marchHeight == height)
        return true;
    _termSrv.Reset();
    _termRtv.Reset();
    _term.Reset();
    _marchWidth = _marchHeight = 0;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC rtv {};
    rtv.Format = DXGI_FORMAT_R16_FLOAT;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = DXGI_FORMAT_R16_FLOAT;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &_term)) || FAILED(device->CreateRenderTargetView(_term.Get(), &rtv, &_termRtv)) ||
        FAILED(device->CreateShaderResourceView(_term.Get(), &srv, &_termSrv)))
    {
        _termSrv.Reset();
        _termRtv.Reset();
        _term.Reset();
        _failure = "the march's term target could not be created";
        return false;
    }
    _marchWidth = width;
    _marchHeight = height;
    ++_marchCreated;
    return true;
}

// The sun's colour lives in one texel, so it is created once and never re-created: the search radius is a constant of the shader, not a size.
bool Pass::EnsureSunColour(ID3D11Device* device)
{
    if (_sunColour)
        return true;
    _sunColourSrv.Reset();
    _sunColourRtv.Reset();
    _sunColour.Reset();

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC rtv {};
    rtv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &_sunColour)) ||
        FAILED(device->CreateRenderTargetView(_sunColour.Get(), &rtv, &_sunColourRtv)) ||
        FAILED(device->CreateShaderResourceView(_sunColour.Get(), &srv, &_sunColourSrv)))
    {
        _sunColourSrv.Reset();
        _sunColourRtv.Reset();
        _sunColour.Reset();
        _failure = "the sun's colour target could not be created";
        return false;
    }
    ++_sunColourCreated;
    return true;
}

// The stats texture: two RGBA32F texels, written by the compute pass and read by the composite and the stats reader. Created once.
bool Pass::EnsureStats(ID3D11Device* device)
{
    if (_stats)
        return true;
    _statsSrv.Reset();
    _statsUav.Reset();
    _stats.Reset();

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = kStatsTexels;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &_stats)) || FAILED(device->CreateUnorderedAccessView(_stats.Get(), nullptr, &_statsUav)) ||
        FAILED(device->CreateShaderResourceView(_stats.Get(), nullptr, &_statsSrv)))
    {
        _statsSrv.Reset();
        _statsUav.Reset();
        _stats.Reset();
        _failure = "the stats texture could not be created";
        return false;
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
    if (context == nullptr || inputs.scene == nullptr || inputs.depth == nullptr || inputs.atlas == nullptr)
    {
        _failure = "the draw bound no scene, no depth or no atlas";
        return nullptr;
    }
    if (!IsSceneFormat(inputs.sceneFormat))
    {
        _failure = "the scene is not FP16";
        return nullptr;
    }
    if (!IsDepthFormat(inputs.depthFormat))
    {
        _failure = "the depth is not a format this pass reads";
        return nullptr;
    }
    if (!WindowUsable(inputs.window))
    {
        _failure = "the render window is empty or does not fit the scene";
        return nullptr;
    }
    const uint32_t marchWidth = MarchExtent(inputs.window.width, inputs.params.resolution);
    const uint32_t marchHeight = MarchExtent(inputs.window.height, inputs.params.resolution);
    if (marchWidth == 0 || marchHeight == 0)
    {
        _failure = "the render window's march extent is empty";
        return nullptr;
    }
    if (inputs.state.cascadeCount == 0 || !(inputs.state.marchFar > 0.0f) || !(inputs.state.invRhoCamera > 0.0f) ||
        !FogStateUsable(inputs.state.fog))
    {
        _failure = "the march's state is not one this pass can evaluate";
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

    // The scene has to be one texture this pass can copy: a single-sample 2D texture with one mip and one slice, of the size the draw's
    // own viewport and target measured. Anything else is another pass's binding, and the draw keeps the texture it was handed.
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
        !EnsureMarch(device.Get(), marchWidth, marchHeight) || !EnsureSunColour(device.Get()) || !EnsureStats(device.Get()))
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
    Set(params.rayScale, inputs.state.ndcScale[0], inputs.state.ndcScale[1], inputs.state.ndcOffset[0], inputs.state.ndcOffset[1]);
    for (uint32_t i = 0; i < 4; ++i)
        Set(params.invProjRow[i], inputs.state.inverseProjection[i * 4 + 0], inputs.state.inverseProjection[i * 4 + 1],
            inputs.state.inverseProjection[i * 4 + 2], inputs.state.inverseProjection[i * 4 + 3]);
    Set(params.camera, inputs.state.inverseViewRow1[0], inputs.state.inverseViewRow1[1], inputs.state.inverseViewRow1[2],
        inputs.state.cameraWorldY);
    Set(params.lightDir, inputs.state.sunDirection[0], inputs.state.sunDirection[1], inputs.state.sunDirection[2], inputs.state.marchFar);
    Set(params.fogA, inputs.state.fog.density1, inputs.state.fog.fallOff1, inputs.state.fog.refHeight1, inputs.state.invRhoCamera);
    Set(params.fogB, inputs.state.fog.density2, inputs.state.fog.fallOff2, inputs.state.fog.refHeight2, inputs.params.fogBoost);
    Set(params.fogC, inputs.params.heightLift, inputs.params.phaseBoost, PhaseExponent(inputs.state.fog, inputs.params.phaseBoost), 0.0f); // z: the engine's DI exponent, kept for the log
    Set(params.tuning, float(inputs.params.steps), float(inputs.params.softness), inputs.params.jitter ? 1.0f : 0.0f,
        inputs.params.resolution == Resolution::Half ? 2.0f : 4.0f);
    Set(params.colour, inputs.tintRgb[0], inputs.tintRgb[1], inputs.tintRgb[2], float(uint32_t(inputs.params.colour)));
    Set(params.bound, inputs.whitePoint, inputs.fade, inputs.params.intensity * kMarchGain * inputs.lightGain, inputs.skyCeiling ? 1.0f : 0.0f);
    Set(params.sunScreen, inputs.sunScreenX, inputs.sunScreenY, 0.0f, 0.0f);
    Set(params.fogColour, inputs.state.fog.fogColor[0], inputs.state.fog.fogColor[1], inputs.state.fog.fogColor[2], 0.0f);
    Set(params.fogLut, inputs.state.fog.lutA, inputs.state.fog.lutB, inputs.state.fog.distA, inputs.state.fog.distB);
    for (uint32_t i = 0; i < kCascadeCount; ++i)
    {
        const Cascade& cascade = i < inputs.state.cascadeCount ? inputs.state.cascades[i] : Cascade {};
        ParamsBuffer::Cascade& out = params.cascades[i];
        for (uint32_t row = 0; row < 4; ++row)
            Set(out.row[row], cascade.matrix[row * 4 + 0], cascade.matrix[row * 4 + 1], cascade.matrix[row * 4 + 2],
                cascade.matrix[row * 4 + 3]);
        Set(out.fade, cascade.fade[0], cascade.fade[1], cascade.fade[2], cascade.fade[3]);
        // The cascade's own atlas slice, from its matrix and fade row (a cascade that names none, or a slot past the landed ones, gets 4: it never shadows).
        const uint32_t slice = i < inputs.state.cascadeCount ? CascadeSlice(cascade.matrix, cascade.fade) : kNoSlice;
        Set(out.ramp, cascade.distanceFar, cascade.rampScale, cascade.rampTarget, slice == kNoSlice ? 4.0f : float(slice));
    }
    float texelSize[2] {};
    {
        // The atlas' own dimensions, the writer's own resinfo arithmetic: the march reads them from the SRV the caller secured, and the
        // same numbers go to the shader so the taps and the read agree with the writer's own.
        ComPtr<ID3D11Resource> atlasResource;
        inputs.atlas->GetResource(&atlasResource);
        ComPtr<ID3D11Texture2D> atlasTexture;
        if (atlasResource != nullptr && SUCCEEDED(atlasResource.As(&atlasTexture)) && atlasTexture != nullptr)
        {
            D3D11_TEXTURE2D_DESC atlasDesc {};
            atlasTexture->GetDesc(&atlasDesc);
            AtlasTexelSize(atlasDesc.Width, atlasDesc.Height, texelSize);
        }
    }
    Set(params.atlasParam, texelSize[0], texelSize[1], inputs.state.discScale, kAirBias);
    Set(params.medium, inputs.params.density, EffectiveAnisotropy(inputs.params.anisotropy, inputs.params.phaseBoost), 0.0f, 0.0f);
    Set(params.upsample, inputs.bilateral ? kUpsampleDepthSigma : 0.0f, 0.0f, 0.0f, 0.0f);
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

    // The pair covers all three draws: the march, the sun's colour and the composite are one pass.
    const bool timed = BeginGpuTiming(context);

    // 1. The march, at the march's own resolution of the render window.
    ID3D11ShaderResourceView* marchViews[kSlots] {};
    marchViews[kSlotDepth] = inputs.depth;
    marchViews[kSlotAtlas] = inputs.atlas;
    ID3D11RenderTargetView* termRtv = _termRtv.Get();
    const D3D11_VIEWPORT marchViewport { 0.0f, 0.0f, float(marchWidth), float(marchHeight), 0.0f, 1.0f };
    context->PSSetShaderResources(0, kSlots, marchViews);
    context->PSSetShader(_marchShader.Get(), nullptr, 0);
    context->OMSetRenderTargets(1, &termRtv, nullptr);
    context->RSSetViewports(1, &marchViewport);
    context->Draw(3, 0);

    // 1b. The stats: one group of 256 threads over the scene's sky pixels and the term. The term is a render target of the draw above, so the targets
    //     come off first (a resource bound as a target and as a view is a hazard the runtime resolves by unbinding the view). The compute state is the
    //     game's until the scope ends, then it is put back.
    context->OMSetRenderTargets(0, nullptr, nullptr);
    {
        ComputeStateRestore computeGuard(context);
        ID3D11Buffer* computeBuffers[1] { _params.Get() };
        ID3D11ShaderResourceView* computeViews[4] { inputs.depth, nullptr, inputs.scene, _termSrv.Get() };
        ID3D11UnorderedAccessView* computeUavs[1] { _statsUav.Get() };
        context->CSSetShader(_statsShader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, computeBuffers);
        context->CSSetShaderResources(0, 4, computeViews);
        context->CSSetUnorderedAccessViews(0, 1, computeUavs, nullptr);
        context->Dispatch(1, 1, 1);
    }

    // 2. The sun's colour, into one texel: the sky's own colour around the sun. It is a separate draw because the search is a square of
    //    the scene around one position, and doing it per output pixel of the composite would cost hundreds of reads a pixel. With the
    //    colour key not asking for the sky's colour the draw is skipped.
    if (inputs.params.colour == Colour::Auto && inputs.tintFromSky)
    {
        ID3D11ShaderResourceView* colourViews[kSlots] {};
        colourViews[kSlotDepth] = inputs.depth;
        colourViews[kSlotScene] = inputs.scene;
        ID3D11RenderTargetView* sunColourRtv = _sunColourRtv.Get();
        const D3D11_VIEWPORT colourViewport { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
        context->OMSetRenderTargets(1, &sunColourRtv, nullptr);
        context->RSSetViewports(1, &colourViewport);
        context->PSSetShaderResources(0, kSlots, colourViews);
        context->PSSetShader(_sunColourShader.Get(), nullptr, 0);
        context->Draw(3, 0);
    }

    // 3. The composite, over the render window, into this pass's copy of the scene. The scene it was handed is the shader resource here,
    //    and the render target is switched before its views are bound for the same reason as the radial's: the term and the sun's colour
    //    were the previous passes' render targets.
    ID3D11ShaderResourceView* compositeViews[kSlots] {};
    compositeViews[kSlotDepth] = inputs.depth;
    compositeViews[kSlotAtlas] = inputs.atlas;
    compositeViews[kSlotScene] = inputs.scene;
    compositeViews[kSlotTerm] = _termSrv.Get();
    compositeViews[kSlotSky] = inputs.sky;
    compositeViews[kSlotLut] = inputs.lut;
    compositeViews[kSlotSunTint] = _sunColourSrv.Get();
    compositeViews[kSlotSkyCeil] = _statsSrv.Get();
    ID3D11RenderTargetView* sceneRtv = _sceneRtv.Get();
    const D3D11_VIEWPORT windowViewport { inputs.window.x, inputs.window.y, inputs.window.width, inputs.window.height, 0.0f, 1.0f };
    context->OMSetRenderTargets(1, &sceneRtv, nullptr);
    context->RSSetViewports(1, &windowViewport);
    context->PSSetShaderResources(0, kSlots, compositeViews);
    context->PSSetShader(_compositeShader.Get(), nullptr, 0);
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
} // namespace ext::ffxiv::godrays
