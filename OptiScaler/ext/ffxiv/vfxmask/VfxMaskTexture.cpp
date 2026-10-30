#include "pch.h"

#include "ext/ffxiv/vfxmask/VfxMaskTexture.h"

#include <vector>

namespace ext::ffxiv::vfxmask
{
bool ValidMaskDesc(const D3D11_TEXTURE2D_DESC& desc) noexcept
{
    return desc.Width >= kMinMaskEdge && desc.Width <= kMaxMaskEdge && desc.Height >= 1 &&
           desc.Height <= kMaxMaskEdge && desc.MipLevels == 1 && desc.ArraySize == 1 &&
           desc.Format == DXGI_FORMAT_R8_UNORM && desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 &&
           desc.Usage == D3D11_USAGE_DEFAULT;
}

EnsureResult EnsureMaskTexture(ID3D11DeviceContext* context, unsigned width, unsigned height, ProbeMode mode,
                               MaskTexture& cache) noexcept
{
    EnsureResult result;

    if (context == nullptr)
    {
        result.reason = "the evaluate has no device context";
        return result;
    }

    const MaskPlan plan = PlanMask(width, height, mode);
    if (!plan.ok)
    {
        result.reason = "the colour input's size cannot be split into two halves";
        return result;
    }

    // Same context, same size, same half: the texture from the last frame is still the mask this evaluate needs, and its
    // content cannot have changed in between (nothing writes it after the fill).
    if (cache.texture != nullptr && cache.context == context && cache.width == plan.width &&
        cache.height == plan.height && cache.mode == mode)
    {
        D3D11_TEXTURE2D_DESC current = {};
        cache.texture->GetDesc(&current);
        if (ValidMaskDesc(current))
        {
            result.texture = cache.texture.Get();
            return result;
        }
    }

    // Anything else: build a new one. Releasing first keeps at most one mask alive, whatever the game does with its device.
    cache.Reset();

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (device == nullptr)
    {
        result.reason = "the device context has no device";
        return result;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = plan.width;
    desc.Height = plan.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    const HRESULT created = device->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(created) || texture == nullptr)
    {
        result.reason = "the mask texture could not be created";
        result.error = created;
        return result;
    }

    // The whole mask in one upload: the two halves are the same in every row, and this runs once per size or mode change.
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(plan.width) * plan.height);
    for (unsigned y = 0; y < plan.height; ++y)
        FillRow(pixels.data() + static_cast<std::size_t>(y) * plan.width, plan.width, mode);
    context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), plan.width, 0);

    D3D11_TEXTURE2D_DESC built = {};
    texture->GetDesc(&built);
    if (!ValidMaskDesc(built))
    {
        result.reason = "the created texture is not an R8_UNORM mask";
        return result;
    }

    cache.texture = texture;
    cache.device = device;
    cache.context = context;
    cache.width = plan.width;
    cache.height = plan.height;
    cache.mode = mode;

    result.texture = cache.texture.Get();
    return result;
}
} // namespace ext::ffxiv::vfxmask
