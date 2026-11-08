#include "pch.h"

#include "ext/core/D3D11ExplicitState.h"
#include "ext/ffxiv/chara/CharaGuides.h"

namespace ext::ffxiv::chara::guides
{
using Microsoft::WRL::ComPtr;

// What the passes touch, and so what the guard saves and puts back: pixel-shader resource slots 0-3 (t0 source, t1 motion, t2 depth, t3 original),
// sampler slot 0, constant-buffer slots 0 (settings) and 1 (the game's camera block, bound with its exact range).
constexpr ext::core::explicit_state::Spec kSpec { /*psSrvCount*/ 4, /*psSamplerCount*/ 1, /*psConstantBuffers*/ 2 };

bool Passes::Init(ID3D11Device* device, const Shaders& shaders, DrawTriangleFn drawTriangle)
{
    Release();
    if (device == nullptr || drawTriangle == nullptr)
        return false;

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> copy, guides, alpha, alphaTint, alphaTintNr;
    if (FAILED(device->CreateVertexShader(shaders.vs.data, shaders.vs.size, nullptr, &vs)) ||
        FAILED(device->CreatePixelShader(shaders.copy.data, shaders.copy.size, nullptr, &copy)) ||
        FAILED(device->CreatePixelShader(shaders.guides.data, shaders.guides.size, nullptr, &guides)) ||
        FAILED(device->CreatePixelShader(shaders.alpha.data, shaders.alpha.size, nullptr, &alpha)))
    {
        LOG_ERROR("CharaView DLSS: helper shaders could not be created");
        return false;
    }
    if (shaders.alphaTint.data != nullptr && shaders.alphaTint.size != 0 &&
        FAILED(device->CreatePixelShader(shaders.alphaTint.data, shaders.alphaTint.size, nullptr, &alphaTint)))
        alphaTint.Reset(); // the debug variants are optional
    if (shaders.alphaTintNr.data != nullptr && shaders.alphaTintNr.size != 0 &&
        FAILED(device->CreatePixelShader(shaders.alphaTintNr.data, shaders.alphaTintNr.size, nullptr, &alphaTintNr)))
        alphaTintNr.Reset();

    D3D11_SAMPLER_DESC sd {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    if (FAILED(device->CreateSamplerState(&sd, &sampler)))
        return false;

    D3D11_BUFFER_DESC bd {};
    bd.ByteWidth = sizeof(Settings);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> settings;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &settings)))
        return false;

    _vs = vs;
    _copy = copy;
    _guides = guides;
    _alpha = alpha;
    _alphaTint = alphaTint;
    _alphaTintNr = alphaTintNr;
    _sampler = sampler;
    _settings = settings;
    _drawTriangle = drawTriangle;
    _fl111 = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1;
    _ready = true;
    return true;
}

void Passes::Release()
{
    _ready = false;
    _vs.Reset();
    _copy.Reset();
    _guides.Reset();
    _alpha.Reset();
    _alphaTint.Reset();
    _alphaTintNr.Reset();
    _sampler.Reset();
    _settings.Reset();
    _drawTriangle = nullptr;
}

Passes::Scope::Scope(Passes& passes, ID3D11DeviceContext* context, const Settings& settings, ID3D11Buffer* camera, UINT first,
                     UINT count)
    : _passes(passes), _context(context)
{
    if (!passes.Ready() || context == nullptr || FAILED(context->QueryInterface(IID_PPV_ARGS(&_context1))))
        return;

    // The settings buffer is not part of the pipeline state: write it before the guard (the order the swap into a private state had).
    context->UpdateSubresource(passes._settings.Get(), 0, nullptr, &settings, 0, 0);

    // No private pipeline state: the game's own is captured here and put back explicitly when the scope ends (ext/core/D3D11ExplicitState.h), on every
    // exit of the caller. The swap into a private state it replaces does nothing when the draw hook is nested.
    _guard.emplace(context, _context1.Get(), passes._fl111, kSpec);
    _guard->Neutralise(); // what the private state's defaults provided: no HS, DS, GS, stream-out target or predicate

    ID3D11Buffer* settingsBuffer = passes._settings.Get();
    _context->PSSetConstantBuffers(0, 1, &settingsBuffer);
    if (camera != nullptr)
    {
        // A range bound with an offset keeps its range (the runtime wants offsets and counts in multiples of 16
        // constants, which the game's own binding satisfied). A block that starts at the beginning of the buffer is bound
        // whole: the buffer is only as large as the block, so a rounded-up count would not fit.
        if (first == 0)
            _context->PSSetConstantBuffers(1, 1, &camera);
        else
            _context1->PSSetConstantBuffers1(1, 1, &camera, &first, &count);
    }
    ID3D11SamplerState* sampler = passes._sampler.Get();
    _context->PSSetSamplers(0, 1, &sampler);
    _context->IASetInputLayout(nullptr);
    _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    _context->VSSetShader(passes._vs.Get(), nullptr, 0);
    // The rest of what the private state's defaults provided and this scope relied on without setting it: the default rasterizer state (solid,
    // back-face culling, scissor off), no blending (all channels, sample mask all ones) and no depth-stencil test.
    _context->RSSetState(nullptr);
    _context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    _context->OMSetDepthStencilState(nullptr, 0);
    _ok = true;
}

Passes::Scope::~Scope()
{
    _guard.reset(); // puts the game's state back
}

ext::core::explicit_state::FullStateSnapshot Passes::Scope::KeepState()
{
    // Built in place for the caller (guaranteed copy elision); inert when the scope never got going and has no context interface.
    return ext::core::explicit_state::FullStateSnapshot(_context, _context1.Get(), _passes._fl111, _ok);
}

bool Passes::Scope::Draw(ID3D11PixelShader* ps, ID3D11ShaderResourceView* const (&srvs)[4], ID3D11RenderTargetView* const* targets,
                         UINT targetCount, uint32_t width, uint32_t height)
{
    if (!_ok || width == 0 || height == 0)
        return false;

    const D3D11_VIEWPORT viewport { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    _context->RSSetViewports(1, &viewport);
    _context->OMSetRenderTargets(targetCount, targets, nullptr);
    _context->PSSetShaderResources(0, 4, srvs);
    _context->PSSetShader(ps, nullptr, 0);
    _passes._drawTriangle(_context);

    // Unbind so the next pass can use these resources on the other side.
    ID3D11ShaderResourceView* none[4] {};
    _context->PSSetShaderResources(0, 4, none);
    _context->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

bool Passes::Scope::Copy(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, uint32_t width, uint32_t height)
{
    if (source == nullptr || target == nullptr)
        return false;
    ID3D11ShaderResourceView* const srvs[4] { source, nullptr, nullptr, nullptr };
    return Draw(_passes._copy.Get(), srvs, &target, 1, width, height);
}

bool Passes::Scope::Guides(ID3D11ShaderResourceView* motion, ID3D11ShaderResourceView* depth, ID3D11RenderTargetView* depthTarget,
                           ID3D11RenderTargetView* motionTarget, uint32_t width, uint32_t height)
{
    if (motion == nullptr || depth == nullptr || depthTarget == nullptr || motionTarget == nullptr)
        return false;
    ID3D11ShaderResourceView* const srvs[4] { nullptr, motion, depth, nullptr };
    ID3D11RenderTargetView* const targets[2] { depthTarget, motionTarget };
    return Draw(_passes._guides.Get(), srvs, targets, 2, width, height);
}

bool Passes::Scope::Alpha(ID3D11ShaderResourceView* upscaled, ID3D11ShaderResourceView* original,
                          ID3D11RenderTargetView* target, uint32_t width, uint32_t height, Tint tint)
{
    if (upscaled == nullptr || original == nullptr || target == nullptr)
        return false;
    ID3D11ShaderResourceView* const srvs[4] { upscaled, nullptr, nullptr, original };
    ID3D11PixelShader* ps = _passes._alpha.Get();
    if (tint == Tint::Sr && _passes._alphaTint != nullptr)
        ps = _passes._alphaTint.Get();
    else if (tint == Tint::Nr && _passes._alphaTintNr != nullptr)
        ps = _passes._alphaTintNr.Get();
    return Draw(ps, srvs, &target, 1, width, height);
}

bool Passes::Scope::TintResult(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, uint32_t width, uint32_t height)
{
    // The tinted shader is required here: without it there would be nothing to tell the result from the untinted image, and the caller
    // would count a tint that never happened.
    if (_passes._alphaTintNr == nullptr)
        return false;
    return Alpha(source, source, target, width, height, Tint::Nr);
}
} // namespace ext::ffxiv::chara::guides
