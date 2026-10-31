#pragma once
// S10 package A: D3D11 helpers of the boundary detector (docs/HUDLESS.md section 10): which render target is bound, and a
// copy of it into a reused texture. No OptiScaler dependencies (no pch, no config, no log), so the WARP test in
// tests/ext/ffxiv/ui builds it standalone.
//
// Rules: never keep a reference to a swapchain backbuffer or to the render target of a draw beyond the call (a retained
// reference makes ResizeBuffers fail). `Bound` holds COM references only for the duration of a capture on the stack.

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace ext::ffxiv::ui::capture
{
using Microsoft::WRL::ComPtr;

// How a copy is issued. The game integration passes the lighting hook's un-hooked trampoline so the copy is not seen by
// OnCopy (FfxivLightingScan::ColorCopy / FfxivSceneHdr::Copy would treat it as a game copy); tests pass nullptr.
using CopyFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource* dest, ID3D11Resource* source);

struct Bound
{
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc {};
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN; // format of the render target view (typed even when the texture is typeless)
};

enum class Result : uint8_t
{
    Ok,
    NoRenderTarget,
    NotTexture2D,
    Multisampled,
    ArrayOrMips,
    SizeMismatch, // the target is not the size the caller expects (the swapchain backbuffer)
    CreateFailed,
};

inline const char* ResultText(Result r)
{
    switch (r)
    {
    case Result::Ok: return "ok";
    case Result::NoRenderTarget: return "no render target bound";
    case Result::NotTexture2D: return "render target is not a 2D texture";
    case Result::Multisampled: return "render target is multisampled";
    case Result::ArrayOrMips: return "render target is an array or has mips";
    case Result::SizeMismatch: return "render target size differs from the backbuffer";
    default: return "could not create the snapshot texture";
    }
}

// The colour render target bound at slot 0 right now.
inline Result GetBound(ID3D11DeviceContext* c, Bound& out)
{
    ComPtr<ID3D11RenderTargetView> rtv;
    c->OMGetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    if (!rtv)
        return Result::NoRenderTarget;

    D3D11_RENDER_TARGET_VIEW_DESC vd {};
    rtv->GetDesc(&vd);
    ComPtr<ID3D11Resource> resource;
    rtv->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture)))
        return Result::NotTexture2D;

    out.texture = texture;
    texture->GetDesc(&out.desc);
    out.viewFormat = vd.Format;
    if (out.desc.SampleDesc.Count != 1)
        return Result::Multisampled;
    if (out.desc.ArraySize != 1 || out.desc.MipLevels != 1)
        return Result::ArrayOrMips;
    return Result::Ok;
}

// The checks of GetBound for a texture named by the caller instead of the bound target (the early anchor copies the swapchain
// backbuffer while another target is bound): no view, so the copy is described by the texture format.
inline Result Describe(ID3D11Texture2D* texture, Bound& out)
{
    if (!texture)
        return Result::NotTexture2D;
    out.texture = texture;
    texture->GetDesc(&out.desc);
    out.viewFormat = DXGI_FORMAT_UNKNOWN;
    if (out.desc.SampleDesc.Count != 1)
        return Result::Multisampled;
    if (out.desc.ArraySize != 1 || out.desc.MipLevels != 1)
        return Result::ArrayOrMips;
    return Result::Ok;
}

// Typed (non typeless, non sRGB-view) format to describe the copy with: the view format when it is typed, else the
// texture format reduced to its UNORM / FLOAT member.
inline DXGI_FORMAT TypedFormat(const Bound& b)
{
    DXGI_FORMAT f = b.viewFormat != DXGI_FORMAT_UNKNOWN ? b.viewFormat : b.desc.Format;
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
    }
}

// Makes `dst` a single-sample, non-array texture of the same size and format as `src` (recreating it when it differs).
// staging = CPU readable copy target; otherwise a default-usage texture (the v1 delivery texture).
inline bool EnsureTexture(ID3D11Device* device, const Bound& src, bool staging, ComPtr<ID3D11Texture2D>& dst)
{
    if (dst)
    {
        D3D11_TEXTURE2D_DESC d {};
        dst->GetDesc(&d);
        const bool wantStaging = d.Usage == D3D11_USAGE_STAGING;
        if (d.Width == src.desc.Width && d.Height == src.desc.Height && d.Format == src.desc.Format && wantStaging == staging)
            return true;
        dst.Reset();
    }

    D3D11_TEXTURE2D_DESC d {};
    d.Width = src.desc.Width;
    d.Height = src.desc.Height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = src.desc.Format;
    d.SampleDesc.Count = 1;
    if (staging)
    {
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    }
    else
    {
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    }
    return SUCCEEDED(device->CreateTexture2D(&d, nullptr, dst.ReleaseAndGetAddressOf()));
}

// Copies the bound colour target into `dst` (reused across calls). `expectWidth/Height` = 0 skips the size check.
// This is the boundary capture: it runs BEFORE the boundary draw, on the immediate context, in command order.
inline Result CopyBound(ID3D11Device* device, ID3D11DeviceContext* c, bool staging, uint32_t expectWidth,
                        uint32_t expectHeight, ComPtr<ID3D11Texture2D>& dst, CopyFn copy, Bound& boundOut)
{
    const Result r = GetBound(c, boundOut);
    if (r != Result::Ok)
        return r;
    if ((expectWidth && boundOut.desc.Width != expectWidth) || (expectHeight && boundOut.desc.Height != expectHeight))
        return Result::SizeMismatch;
    if (!EnsureTexture(device, boundOut, staging, dst))
        return Result::CreateFailed;
    if (copy)
        copy(c, dst.Get(), boundOut.texture.Get());
    else
        c->CopyResource(dst.Get(), boundOut.texture.Get());
    return Result::Ok;
}
} // namespace ext::ffxiv::ui::capture
