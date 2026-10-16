#include "pch.h"

#include "ext/reshade/ReShadeD3D11.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

using Microsoft::WRL::ComPtr;

namespace ext::reshade::d3d11
{
// ---- SEH ------------------------------------------------------------------------------------------------------------
uint32_t GuardedCall(void (*fn)(void*), void* arg)
{
    DWORD code = 0;
    __try
    {
        fn(arg);
    }
    __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
    {
        return static_cast<uint32_t>(code != 0 ? code : 0xFFFFFFFFu);
    }
    return 0;
}

// ---- formats --------------------------------------------------------------------------------------------------------
FormatFamily FamilyOf(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return { true, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB };
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return { true, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB };
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return { true, DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB };
    default:
        return {};
    }
}

bool SameFamily(DXGI_FORMAT a, DXGI_FORMAT b)
{
    if (a == b)
        return true;
    const FormatFamily fa = FamilyOf(a);
    const FormatFamily fb = FamilyOf(b);
    return fa.known && fb.known && fa.linear == fb.linear;
}

// ---- state block ----------------------------------------------------------------------------------------------------
namespace
{
template <class T, size_t N> void ReleaseAll(std::array<T*, N>& a)
{
    for (auto& p : a)
    {
        if (p != nullptr)
        {
            p->Release();
            p = nullptr;
        }
    }
}
} // namespace

void StateBlock::Release()
{
    _captured = false;
    _layout.Reset();
    _indexBuffer.Reset();
    ReleaseAll(_vertexBuffers);
    for (auto& s : _stages)
    {
        s.shader.Reset();
        ReleaseAll(s.constantBuffers);
        ReleaseAll(s.resources);
        ReleaseAll(s.samplers);
    }
    ReleaseAll(_csUavs);
    _rasterizer.Reset();
    _blend.Reset();
    _depthStencil.Reset();
    ReleaseAll(_renderTargets);
    _depthView.Reset();
}

#define STAGE_CAPTURE(P, TYPE, IDX)                                                                                    \
    {                                                                                                                  \
        TYPE* shader = nullptr;                                                                                        \
        c->P##GetShader(&shader, nullptr, nullptr);                                                                    \
        _stages[IDX].shader.Attach(shader);                                                                            \
        c->P##GetConstantBuffers(0, 14, _stages[IDX].constantBuffers.data());                                          \
        c->P##GetShaderResources(0, 128, _stages[IDX].resources.data());                                               \
        c->P##GetSamplers(0, 16, _stages[IDX].samplers.data());                                                        \
    }

#define STAGE_APPLY(P, TYPE, IDX)                                                                                      \
    {                                                                                                                  \
        ComPtr<TYPE> shader;                                                                                           \
        if (_stages[IDX].shader)                                                                                       \
            _stages[IDX].shader.As(&shader);                                                                           \
        c->P##SetShader(shader.Get(), nullptr, 0);                                                                     \
        c->P##SetConstantBuffers(0, 14, _stages[IDX].constantBuffers.data());                                          \
        c->P##SetShaderResources(0, 128, _stages[IDX].resources.data());                                               \
        c->P##SetSamplers(0, 16, _stages[IDX].samplers.data());                                                        \
    }

void StateBlock::Capture(ID3D11DeviceContext* c)
{
    Release();
    if (c == nullptr)
        return;

    ID3D11InputLayout* layout = nullptr;
    c->IAGetInputLayout(&layout);
    _layout.Attach(layout);
    c->IAGetPrimitiveTopology(&_topology);
    c->IAGetVertexBuffers(0, 16, _vertexBuffers.data(), _strides.data(), _offsets.data());
    ID3D11Buffer* index = nullptr;
    c->IAGetIndexBuffer(&index, &_indexFormat, &_indexOffset);
    _indexBuffer.Attach(index);

    STAGE_CAPTURE(VS, ID3D11VertexShader, 0)
    STAGE_CAPTURE(HS, ID3D11HullShader, 1)
    STAGE_CAPTURE(DS, ID3D11DomainShader, 2)
    STAGE_CAPTURE(GS, ID3D11GeometryShader, 3)
    STAGE_CAPTURE(PS, ID3D11PixelShader, 4)
    STAGE_CAPTURE(CS, ID3D11ComputeShader, 5)
    c->CSGetUnorderedAccessViews(0, 8, _csUavs.data());

    ID3D11RasterizerState* rasterizer = nullptr;
    c->RSGetState(&rasterizer);
    _rasterizer.Attach(rasterizer);
    _viewportCount = static_cast<UINT>(_viewports.size());
    c->RSGetViewports(&_viewportCount, _viewports.data());
    _scissorCount = static_cast<UINT>(_scissors.size());
    c->RSGetScissorRects(&_scissorCount, _scissors.data());

    ID3D11BlendState* blend = nullptr;
    c->OMGetBlendState(&blend, _blendFactor.data(), &_sampleMask);
    _blend.Attach(blend);
    ID3D11DepthStencilState* depth = nullptr;
    c->OMGetDepthStencilState(&depth, &_stencilRef);
    _depthStencil.Attach(depth);
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(8, _renderTargets.data(), &dsv);
    _depthView.Attach(dsv);
    _captured = true;
}

void StateBlock::Apply(ID3D11DeviceContext* c)
{
    if (c == nullptr || !_captured)
        return;

    c->IASetInputLayout(_layout.Get());
    c->IASetPrimitiveTopology(_topology);
    c->IASetVertexBuffers(0, 16, _vertexBuffers.data(), _strides.data(), _offsets.data());
    c->IASetIndexBuffer(_indexBuffer.Get(), _indexFormat, _indexOffset);

    STAGE_APPLY(VS, ID3D11VertexShader, 0)
    STAGE_APPLY(HS, ID3D11HullShader, 1)
    STAGE_APPLY(DS, ID3D11DomainShader, 2)
    STAGE_APPLY(GS, ID3D11GeometryShader, 3)
    STAGE_APPLY(PS, ID3D11PixelShader, 4)
    STAGE_APPLY(CS, ID3D11ComputeShader, 5)
    const UINT keep[8] = { UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1) };
    c->CSSetUnorderedAccessViews(0, 8, _csUavs.data(), keep);

    c->RSSetState(_rasterizer.Get());
    c->RSSetViewports(_viewportCount, _viewportCount != 0 ? _viewports.data() : nullptr);
    c->RSSetScissorRects(_scissorCount, _scissorCount != 0 ? _scissors.data() : nullptr);

    c->OMSetBlendState(_blend.Get(), _blendFactor.data(), _sampleMask);
    c->OMSetDepthStencilState(_depthStencil.Get(), _stencilRef);
    c->OMSetRenderTargets(8, _renderTargets.data(), _depthView.Get());
}

// ---- before/after comparison ----------------------------------------------------------------------------------------
namespace
{
float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exponent = (h >> 10) & 0x1Fu;
    const uint32_t mantissa = h & 0x3FFu;
    float value;
    if (exponent == 0)
        value = std::ldexp(float(mantissa), -24);
    else if (exponent == 31)
        value = mantissa == 0 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
    else
        value = std::ldexp(float(mantissa | 0x400u), int(exponent) - 25);
    return sign ? -value : value;
}

double NowUs()
{
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

bool TargetSnapshot::Sample(ID3D11DeviceContext* context, ID3D11Resource* resource, std::vector<float>& out, std::string& note)
{
    out.clear();
    ComPtr<ID3D11Texture2D> texture;
    if (resource == nullptr || FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))))
    {
        note = "the target is not a 2D texture";
        return false;
    }
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);

    enum class Kind { Rgba8, Bgra8, Half4, Unsupported } kind = Kind::Unsupported;
    switch (desc.Format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        kind = Kind::Rgba8;
        break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        kind = Kind::Bgra8;
        break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        kind = Kind::Half4;
        break;
    default:
        break;
    }
    if (kind == Kind::Unsupported)
    {
        note = std::format("format {} is not supported by the comparison", int(desc.Format));
        return false;
    }
    if (desc.SampleDesc.Count != 1)
    {
        note = "multisampled target";
        return false;
    }

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    staging.ArraySize = 1;
    staging.MipLevels = 1;
    ComPtr<ID3D11Texture2D> copy;
    if (device == nullptr || FAILED(device->CreateTexture2D(&staging, nullptr, &copy)))
    {
        note = "staging texture creation failed";
        return false;
    }

    context->CopyResource(copy.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    {
        note = "Map of the staging texture failed";
        return false;
    }

    for (UINT y = 0; y < desc.Height; y += kStride)
    {
        const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
        for (UINT x = 0; x < desc.Width; x += kStride)
        {
            float r, g, b;
            if (kind == Kind::Half4)
            {
                const uint16_t* p = reinterpret_cast<const uint16_t*>(row + size_t(x) * 8);
                r = HalfToFloat(p[0]) * 255.f;
                g = HalfToFloat(p[1]) * 255.f;
                b = HalfToFloat(p[2]) * 255.f;
            }
            else
            {
                const uint8_t* p = row + size_t(x) * 4;
                r = kind == Kind::Rgba8 ? p[0] : p[2];
                g = p[1];
                b = kind == Kind::Rgba8 ? p[2] : p[0];
            }
            out.push_back(r);
            out.push_back(g);
            out.push_back(b);
        }
    }
    context->Unmap(copy.Get(), 0);
    note = std::format("every {}th pixel in x and y of {}x{}, format {}", kStride, desc.Width, desc.Height, int(desc.Format));
    return true;
}

bool TargetSnapshot::Capture(ID3D11DeviceContext* context, ID3D11Resource* resource, std::string& note)
{
    Release();
    const double t0 = NowUs();
    _haveBefore = Sample(context, resource, _before, note);
    _beforeNote = note;
    _beforeReadbackUs = NowUs() - t0;
    return _haveBefore;
}

probe::PixelDiff TargetSnapshot::CaptureAndDiff(ID3D11DeviceContext* context, ID3D11Resource* resource)
{
    probe::PixelDiff diff;
    if (!_haveBefore)
    {
        diff.note = "the before-snapshot is unavailable: " + _beforeNote;
        diff.readbackUs = _beforeReadbackUs;
        Release();
        return diff;
    }

    const double t0 = NowUs();
    std::vector<float> after;
    std::string note;
    const bool ok = Sample(context, resource, after, note);
    diff.readbackUs = _beforeReadbackUs + (NowUs() - t0);
    if (!ok || after.size() != _before.size())
    {
        diff.note = ok ? "size changed between the snapshots" : note;
        Release();
        return diff;
    }

    uint64_t changed = 0;
    double sum = 0;
    float maxDelta = 0;
    const size_t pixels = after.size() / 3;
    for (size_t i = 0; i < pixels; i++)
    {
        float pixelMax = 0;
        for (size_t c = 0; c < 3; c++)
        {
            const float d = std::fabs(after[i * 3 + c] - _before[i * 3 + c]);
            sum += d;
            pixelMax = std::max(pixelMax, d);
        }
        changed += pixelMax > 0.5f ? 1 : 0;
        maxDelta = std::max(maxDelta, pixelMax);
    }
    diff.valid = pixels != 0;
    diff.sampled = pixels;
    diff.changedFraction = pixels ? double(changed) / double(pixels) : 0.0;
    diff.meanAbs = pixels ? sum / double(pixels * 3) : 0.0;
    diff.maxDelta = static_cast<uint32_t>(std::min(maxDelta, 4294967040.0f));
    diff.note = note;
    Release();
    return diff;
}

void TargetSnapshot::Release()
{
    _before.clear();
    _before.shrink_to_fit();
    _haveBefore = false;
}
} // namespace ext::reshade::d3d11
