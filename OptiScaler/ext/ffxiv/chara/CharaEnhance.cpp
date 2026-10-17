#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/chara/CharaEnhance.h"
#include "ext/ffxiv/chara/CharaEnhancePass.h"
#include "ext/ffxiv/chara/CharaStats.h"

namespace ext::ffxiv::chara::enhance
{
using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT kMaxOutput = 4096;

// Never destroyed: the process may unload this module under the loader lock, where releasing D3D objects is not safe.
FxaaPass& Pass()
{
    static auto* pass = new FxaaPass;
    return *pass;
}

void DrawTriangle(ID3D11DeviceContext* context) { DrawFullscreenTriangle(context); }

// The formats the equipment image is known to come in. sRGB views make the pass work on linear values; the stored bytes
// stay sRGB because the output views use the matching format.
bool SupportedFormat(DXGI_FORMAT format, bool& srgb)
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        srgb = false;
        return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        srgb = true;
        return true;
    default:
        return false;
    }
}

bool g_loggedMatch = false;
bool g_loggedFormat = false;
bool g_loggedFailure = false;
} // namespace

bool Around(const DrawView& view, DrawCall draw)
{
    ID3D11DeviceContext* context = view.context;
    if (context == nullptr || view.table == nullptr)
        return false;

    ++g_stats.equipShaderSeen;

    ComPtr<ID3D11ShaderResourceView> source;
    context->PSGetShaderResources(0, 1, &source);
    if (source == nullptr)
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
    source->GetDesc(&viewDesc);
    if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
        return false;

    ComPtr<ID3D11Resource> resource;
    source->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (resource == nullptr || FAILED(resource.As(&texture)))
        return false;

    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1)
        return false;

    // The preview renderer's equipment output is table entry 0.
    if (table::Lookup(*view.table, reinterpret_cast<uintptr_t>(texture.Get()), desc.Width, desc.Height) != 0)
        return false;

    ++g_stats.equipMatched;
    if (!g_loggedMatch)
    {
        g_loggedMatch = true;
        LOG_INFO("CharaView: native equipment output matched, {}x{}, deferred {}", desc.Width, desc.Height,
                 g_stats.deferredSeen.load());
    }

    const DXGI_FORMAT format = viewDesc.Format != DXGI_FORMAT_UNKNOWN ? viewDesc.Format : desc.Format;
    bool srgb = false;
    const UINT scale = ext::Cfg().ffxiv.chara.previewUpscale.value_or_default() ? 2u : 1u;
    if (!SupportedFormat(format, srgb) || desc.Width * scale > kMaxOutput || desc.Height * scale > kMaxOutput)
    {
        ++g_stats.aaSkipped;
        if (!g_loggedFormat)
        {
            g_loggedFormat = true;
            LOG_WARN("CharaView: spatial AA skipped, equipment image {}x{} format {} (scale {}) is not supported", desc.Width,
                     desc.Height, static_cast<int>(format), scale);
        }
        return false;
    }

    ID3D11ShaderResourceView* filtered = nullptr;
    if (!Pass().Process(context, source.Get(), desc.Width, desc.Height, srgb, scale, &filtered, DrawTriangle) || filtered == nullptr)
    {
        ++g_stats.aaSkipped;
        if (!g_loggedFailure)
        {
            g_loggedFailure = true;
            LOG_WARN("CharaView: spatial AA could not run ({}); native preview retained",
                     Pass().Failure() != nullptr ? Pass().Failure() : "output texture not available");
        }
        return false;
    }

    // Show the filtered image to this one draw, then give the draw its own image back.
    WithSource0(context, filtered, [&] { draw(); });

    const uint64_t n = ++g_stats.aaProcessed;
    if (n == 1 || n % 600 == 0)
        LOG_INFO("CharaView spatial AA: {}x{} -> {}x{}, processed draws {}", desc.Width, desc.Height, Pass().OutputWidth(),
                 Pass().OutputHeight(), n);
    return true;
}

void Detach()
{
    Pass().Reset();
    g_loggedFailure = false;
}
} // namespace ext::ffxiv::chara::enhance
