#include "pch.h"

#include "ext/core/D3D11ExplicitState.h"
#include "ext/ffxiv/chara/CharaEnhancePass.h"

#include <Chara_Fxaa_PS.h>
#include <Chara_Fxaa_PSTint.h>
#include <Chara_Fxaa_VS.h>

namespace ext::ffxiv::chara
{
using Microsoft::WRL::ComPtr;

// What the pass touches, and so what the guard saves and puts back: pixel-shader resource slot 0 (the image), sampler slot 0, no constant buffer.
constexpr ext::core::explicit_state::Spec kSpec { /*psSrvCount*/ 1, /*psSamplerCount*/ 1, /*psConstantBuffers*/ 0 };

void FxaaPass::Reset()
{
    _outputSrv.Reset();
    _outputRtv.Reset();
    _output.Reset();
    _rasterizer.Reset();
    _sampler.Reset();
    _ps.Reset();
    _psTint.Reset();
    _vs.Reset();
    _device.Reset();
    _outWidth = _outHeight = 0;
    _outSrgb = false;
    _failure = nullptr;
}

bool FxaaPass::Init(ID3D11Device* device)
{
    if (FAILED(device->CreateVertexShader(Chara_Fxaa_VS_cso, sizeof(Chara_Fxaa_VS_cso), nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(Chara_Fxaa_PS_cso, sizeof(Chara_Fxaa_PS_cso), nullptr, &_ps)) ||
        FAILED(device->CreatePixelShader(Chara_Fxaa_PSTint_cso, sizeof(Chara_Fxaa_PSTint_cso), nullptr, &_psTint)))
    {
        _failure = "the filter shaders could not be created";
        return false;
    }

    D3D11_SAMPLER_DESC sampler {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampler, &_sampler)))
    {
        _failure = "the sampler could not be created";
        return false;
    }

    // The full-screen triangle must not be culled whatever its winding.
    D3D11_RASTERIZER_DESC raster {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&raster, &_rasterizer)))
    {
        _failure = "the rasterizer state could not be created";
        return false;
    }

    _device = device;
    return true;
}

bool FxaaPass::EnsureOutput(ID3D11Device* device, UINT width, UINT height, bool srgb)
{
    if (_output && _outWidth == width && _outHeight == height && _outSrgb == srgb)
        return true;

    _outputSrv.Reset();
    _outputRtv.Reset();
    _output.Reset();

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    const DXGI_FORMAT view = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D11_RENDER_TARGET_VIEW_DESC rtv {};
    rtv.Format = view;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = view;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;

    if (FAILED(device->CreateTexture2D(&desc, nullptr, &_output)) ||
        FAILED(device->CreateRenderTargetView(_output.Get(), &rtv, &_outputRtv)) ||
        FAILED(device->CreateShaderResourceView(_output.Get(), &srv, &_outputSrv)))
    {
        _outputSrv.Reset();
        _outputRtv.Reset();
        _output.Reset();
        _outWidth = _outHeight = 0;
        return false; // not permanent: a later, smaller or less memory-starved request may work
    }

    _outWidth = width;
    _outHeight = height;
    _outSrgb = srgb;
    return true;
}

bool FxaaPass::Process(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, UINT srcW, UINT srcH, bool srgb,
                       UINT scale, ID3D11ShaderResourceView** out, DrawFn draw, bool tint)
{
    if (out != nullptr)
        *out = nullptr;
    if (_failure != nullptr || context == nullptr || source == nullptr || out == nullptr || srcW == 0 || srcH == 0 ||
        (scale != 1 && scale != 2))
        return false;

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

    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
    {
        _failure = "ID3D11DeviceContext1 is not available";
        return false;
    }

    const UINT width = srcW * scale;
    const UINT height = srcH * scale;
    if (!EnsureOutput(device.Get(), width, height, srgb))
        return false;

    // No private pipeline state: the game's own is captured here and put back explicitly when the function ends (ext/core/D3D11ExplicitState.h), also
    // when the draw callback throws through this fork's hooks. The swap into a private state it replaces does nothing when the draw hook is nested.
    ext::core::explicit_state::Guard stateGuard(context, context1.Get(), device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1, kSpec);
    stateGuard.Neutralise(); // what the private state's defaults provided: no HS, DS, GS, stream-out target or predicate

    const D3D11_VIEWPORT viewport { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    const float blendFactor[4] {};
    ID3D11RenderTargetView* rtv = _outputRtv.Get();
    ID3D11SamplerState* sampler = _sampler.Get();

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(_vs.Get(), nullptr, 0);
    context->PSSetShader(tint ? _psTint.Get() : _ps.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, &sampler);
    context->PSSetShaderResources(0, 1, &source);
    context->RSSetState(_rasterizer.Get());
    context->RSSetViewports(1, &viewport);
    context->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);
    context->OMSetRenderTargets(1, &rtv, nullptr);

    if (draw != nullptr)
        draw(context);
    else
        context->Draw(3, 0);

    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    context->OMSetRenderTargets(0, nullptr, nullptr);

    *out = _outputSrv.Get();
    return true;
}
} // namespace ext::ffxiv::chara
