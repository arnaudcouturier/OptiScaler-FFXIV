#include "pch.h"

#include "ext/ffxiv/engine/ShaftsPass.h"

#include <LightShafts_Blur.h>
#include <LightShafts_Composite.h>
#include <LightShafts_Mask.h>
#include <LightShafts_SunColour.h>
#include <LightShafts_Vs.h>

#include <cstring>

namespace ext::ffxiv::shafts
{
using Microsoft::WRL::ComPtr;

namespace
{
// The register slots and the constant-buffer layout of ext/ffxiv/engine/shaders/LightShafts.hlsl. Change them together.
constexpr uint32_t kSlotDepth = 0;
constexpr uint32_t kSlotMask = 1;
constexpr uint32_t kSlotScene = 2;
constexpr uint32_t kSlotTerm = 3;
constexpr uint32_t kSlotSunTint = 4;
constexpr uint32_t kSlots = 5;

struct ParamsBuffer
{
    float window[4]; // xy = the render window's origin in scene pixels, zw = its extent in scene pixels
    float sun[4];    // xy = the sun's position in scene pixels, z = intensity, w = decay
    float tuning[4]; // x = samples, y = density, z = 0 the light's colour / 1 the sky's colour at the sun, w = the shafts' gain against the
                     // frame's white
    float tint[4];   // rgb = the light's colour (peak 1)
    float white[4];  // x = the frame's white point in the scene's units, y = the held sun's fade (1 = this frame's own read)
};
static_assert(sizeof(ParamsBuffer) == 80, "the shaders' constant buffer is five float4s");

// The scene's views: the engine's texture may be typeless, so the copy takes the texture's own format and the views take the FP16 the tone
// map reads.
constexpr DXGI_FORMAT kSceneViewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
} // namespace

void Pass::Reset()
{
    _sunColourSrv.Reset();
    _sunColourRtv.Reset();
    _sunColour.Reset();
    _termSrv.Reset();
    _termRtv.Reset();
    _term.Reset();
    _maskSrv.Reset();
    _maskRtv.Reset();
    _mask.Reset();
    _sceneSrv.Reset();
    _sceneRtv.Reset();
    _scene.Reset();
    _params.Reset();
    _rasterizer.Reset();
    _compositeShader.Reset();
    _sunColourShader.Reset();
    _blurShader.Reset();
    _maskShader.Reset();
    _vs.Reset();
    _state.Reset();
    _device.Reset();
    _disjoint.Reset();
    _begin.Reset();
    _end.Reset();
    _sceneResourceFormat = _sceneWidth = _sceneHeight = 0;
    _halfWidth = _halfHeight = 0;
    _scenesCreated = _halfCreated = _sunColourCreated = 0;
    _timingPending = false;
    _haveGpuMs = false;
    _gpuMs = 0.0;
    _failure = nullptr;
}

bool Pass::Init(ID3D11Device* device)
{
    if (device == nullptr)
        return false;
    ComPtr<ID3D11Device1> device1;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))))
    {
        _failure = "ID3D11Device1 is not available";
        return false;
    }
    if (FAILED(device->CreateVertexShader(LightShafts_Vs_cso, sizeof LightShafts_Vs_cso, nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(LightShafts_Mask_cso, sizeof LightShafts_Mask_cso, nullptr, &_maskShader)) ||
        FAILED(device->CreatePixelShader(LightShafts_Blur_cso, sizeof LightShafts_Blur_cso, nullptr, &_blurShader)) ||
        FAILED(device->CreatePixelShader(LightShafts_SunColour_cso, sizeof LightShafts_SunColour_cso, nullptr, &_sunColourShader)) ||
        FAILED(device->CreatePixelShader(LightShafts_Composite_cso, sizeof LightShafts_Composite_cso, nullptr, &_compositeShader)))
    {
        _failure = "the light shaft shaders could not be created";
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

    // The private pipeline state: the game's own is captured and put back after the three draws.
    const D3D_FEATURE_LEVEL levels[] { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL chosen {};
    const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    if (FAILED(device1->CreateDeviceContextState(flags, levels, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), &chosen, &_state)) ||
        _state == nullptr)
    {
        _failure = "the private device-context state could not be created";
        return false;
    }

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

bool Pass::EnsureHalf(ID3D11Device* device, uint32_t width, uint32_t height)
{
    if (_mask && _halfWidth == width && _halfHeight == height)
        return true;
    _termSrv.Reset();
    _termRtv.Reset();
    _term.Reset();
    _maskSrv.Reset();
    _maskRtv.Reset();
    _mask.Reset();
    _halfWidth = _halfHeight = 0;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    desc.Format = DXGI_FORMAT_R8_UNORM;
    D3D11_RENDER_TARGET_VIEW_DESC maskRtv {};
    maskRtv.Format = DXGI_FORMAT_R8_UNORM;
    maskRtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC maskSrv {};
    maskSrv.Format = DXGI_FORMAT_R8_UNORM;
    maskSrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    maskSrv.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &_mask)) || FAILED(device->CreateRenderTargetView(_mask.Get(), &maskRtv, &_maskRtv)) ||
        FAILED(device->CreateShaderResourceView(_mask.Get(), &maskSrv, &_maskSrv)))
    {
        _maskSrv.Reset();
        _maskRtv.Reset();
        _mask.Reset();
        _failure = "the sky mask could not be created";
        return false;
    }

    desc.Format = DXGI_FORMAT_R16_FLOAT;
    D3D11_RENDER_TARGET_VIEW_DESC termRtv {};
    termRtv.Format = DXGI_FORMAT_R16_FLOAT;
    termRtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC termSrv {};
    termSrv.Format = DXGI_FORMAT_R16_FLOAT;
    termSrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    termSrv.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &_term)) || FAILED(device->CreateRenderTargetView(_term.Get(), &termRtv, &_termRtv)) ||
        FAILED(device->CreateShaderResourceView(_term.Get(), &termSrv, &_termSrv)))
    {
        _termSrv.Reset();
        _termRtv.Reset();
        _term.Reset();
        _failure = "the blurred term could not be created";
        return false;
    }

    _halfWidth = width;
    _halfHeight = height;
    ++_halfCreated;
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

void Pass::CollectGpuTiming(ID3D11DeviceContext* context)
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
    if (context == nullptr || inputs.scene == nullptr || inputs.depth == nullptr)
    {
        _failure = "the draw bound no scene or no depth";
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
    const uint32_t halfWidth = HalfExtent(inputs.window.width);
    const uint32_t halfHeight = HalfExtent(inputs.window.height);
    if (halfWidth == 0 || halfHeight == 0)
    {
        _failure = "the render window's half is empty";
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
    // viewport and target measured. Anything else is another pass's binding, and the draw keeps the engine's own resource.
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
        !EnsureHalf(device.Get(), halfWidth, halfHeight) || !EnsureSunColour(device.Get()))
        return nullptr;

    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
    {
        _failure = "ID3D11DeviceContext1 is not available";
        return nullptr;
    }

    // This pass's copy of the engine's scene, then the parameters the three draws read.
    context->CopyResource(_scene.Get(), sceneResource.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr)
    {
        _failure = "the parameter buffer could not be written";
        return nullptr;
    }
    ParamsBuffer params {};
    params.window[0] = inputs.window.x;
    params.window[1] = inputs.window.y;
    params.window[2] = inputs.window.width;
    params.window[3] = inputs.window.height;
    params.sun[0] = inputs.sun.screenX;
    params.sun[1] = inputs.sun.screenY;
    params.sun[2] = inputs.params.intensity;
    params.sun[3] = inputs.params.decay;
    params.tuning[0] = float(inputs.params.samples);
    params.tuning[1] = inputs.params.density;
    params.tuning[2] = inputs.tint.fromSky ? 1.0f : 0.0f;
    params.tuning[3] = kShaftGain;
    params.tint[0] = inputs.tint.rgb[0];
    params.tint[1] = inputs.tint.rgb[1];
    params.tint[2] = inputs.tint.rgb[2];
    params.white[0] = inputs.whitePoint;
    params.white[1] = inputs.fade;
    std::memcpy(mapped.pData, &params, sizeof params);
    context->Unmap(_params.Get(), 0);

    CollectGpuTiming(context);
    ComPtr<ID3DDeviceContextState> previous;
    context1->SwapDeviceContextState(_state.Get(), &previous);
    if (previous == nullptr)
    {
        context1->SwapDeviceContextState(_state.Get(), nullptr); // best effort, as the other fork passes do
        _failure = "the previous device-context state was not returned";
        return nullptr;
    }
    // The game's own device-context state has to come back on every path, also when one of the three draws throws through this fork's hooks.
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
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(_vs.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, buffers);
    context->PSSetSamplers(0, 0, nullptr);
    context->RSSetState(_rasterizer.Get());
    context->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);

    // The pair covers all three draws: the mask, the blur and the composite are one pass.
    const bool timed = BeginGpuTiming(context);

    // 1. The sky mask, at half the render window.
    ID3D11ShaderResourceView* maskViews[kSlots] {};
    maskViews[kSlotDepth] = inputs.depth;
    ID3D11RenderTargetView* maskRtv = _maskRtv.Get();
    const D3D11_VIEWPORT halfViewport { 0.0f, 0.0f, float(halfWidth), float(halfHeight), 0.0f, 1.0f };
    context->PSSetShaderResources(0, kSlots, maskViews);
    context->PSSetShader(_maskShader.Get(), nullptr, 0);
    context->OMSetRenderTargets(1, &maskRtv, nullptr);
    context->RSSetViewports(1, &halfViewport);
    context->Draw(3, 0);

    // 2. The radial blur, at half the render window. The render target is switched first: while the mask is still a render target, D3D11 drops
    // a shader-resource view of it, and the blur would then read zero instead of the mask's sky.
    ID3D11ShaderResourceView* blurViews[kSlots] {};
    blurViews[kSlotMask] = _maskSrv.Get();
    ID3D11RenderTargetView* termRtv = _termRtv.Get();
    context->OMSetRenderTargets(1, &termRtv, nullptr);
    context->RSSetViewports(1, &halfViewport);
    context->PSSetShaderResources(0, kSlots, blurViews);
    context->PSSetShader(_blurShader.Get(), nullptr, 0);
    context->Draw(3, 0);

    // 3. The sun's colour, into one texel: the sky's own colour around the sun. It is a separate draw because the search is a square of the
    //    scene around one position, and doing it per output pixel of the composite would cost hundreds of reads a pixel. With the colour key set
    //    to the neutral shaft the light's own colour is used, so the search is not needed and the draw is skipped.
    if (inputs.tint.fromSky)
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

    // 4. The composite, over the render window, into this pass's copy of the scene. The engine's scene is the shader resource here, and the
    // render target is switched before its views are bound for the same reason as the blur's: the term and the sun's colour were the previous
    // passes' render targets.
    ID3D11ShaderResourceView* compositeViews[kSlots] {};
    compositeViews[kSlotDepth] = inputs.depth;
    compositeViews[kSlotScene] = inputs.scene;
    compositeViews[kSlotTerm] = _termSrv.Get();
    compositeViews[kSlotSunTint] = _sunColourSrv.Get();
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
} // namespace ext::ffxiv::shafts
