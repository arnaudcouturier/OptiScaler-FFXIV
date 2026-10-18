#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/chara/CharaEnhance.h"
#include "ext/ffxiv/chara/CharaEnhancePass.h"
#include "ext/ffxiv/chara/CharaProbeHooks.h"
#include "ext/ffxiv/chara/CharaStats.h"

#include <array>
#include <string>

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

// Why an equipment display draw was left native: each reason is written once (the draw repeats every frame), so a log that
// shows the shader was seen but the AA never ran also says what the draw looked like.
enum class Why : uint8_t
{
    NoSource,
    NotTexture2D,
    NotATexture,
    Multisampled,
    NotEntryZero,
    Count,
};
std::array<bool, static_cast<size_t>(Why::Count)> g_loggedWhy {};

bool FirstTime(Why why)
{
    bool& logged = g_loggedWhy[static_cast<size_t>(why)];
    const bool first = !logged;
    logged = true;
    return first;
}

// The pixel-shader slots (of the first eight) that hold a preview renderer image, as "t<slot>=entry <n>" text.
std::string PreviewSlots(ID3D11DeviceContext* context, const table::Snapshot& snap)
{
    ID3D11ShaderResourceView* views[8] {};
    context->PSGetShaderResources(0, 8, views);
    std::string text;
    for (UINT i = 0; i < 8; ++i)
    {
        if (views[i] == nullptr)
            continue;
        ComPtr<ID3D11Resource> resource;
        views[i]->GetResource(&resource);
        const int entry = table::IndexOfPointer(snap, reinterpret_cast<uintptr_t>(resource.Get()));
        if (entry >= 0)
            text += (text.empty() ? "t" : ", t") + std::to_string(i) + "=entry " + std::to_string(entry);
        views[i]->Release();
    }
    return text.empty() ? std::string("none") : text;
}
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
    {
        if (FirstTime(Why::NoSource))
            LOG_INFO("CharaView spatial AA: the equipment display draw ({}) has nothing bound at t0; preview images bound: {}",
                     DrawApiName(view.api), PreviewSlots(context, *view.table));
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
    source->GetDesc(&viewDesc);
    if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
    {
        if (FirstTime(Why::NotTexture2D))
            LOG_INFO("CharaView spatial AA: t0 of the equipment display draw ({}) is not a plain 2D texture (view dimension {})",
                     DrawApiName(view.api), static_cast<int>(viewDesc.ViewDimension));
        return false;
    }

    ComPtr<ID3D11Resource> resource;
    source->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (resource == nullptr || FAILED(resource.As(&texture)))
    {
        if (FirstTime(Why::NotATexture))
            LOG_INFO("CharaView spatial AA: t0 of the equipment display draw ({}) is not a 2D texture resource", DrawApiName(view.api));
        return false;
    }

    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1)
    {
        if (FirstTime(Why::Multisampled))
            LOG_INFO("CharaView spatial AA: t0 of the equipment display draw ({}) has {} samples", DrawApiName(view.api),
                     desc.SampleDesc.Count);
        return false;
    }

    // The preview renderer's equipment output is table entry 0.
    const int entry = table::Lookup(*view.table, reinterpret_cast<uintptr_t>(texture.Get()), desc.Width, desc.Height);
    if (entry != 0)
    {
        if (FirstTime(Why::NotEntryZero))
            LOG_INFO("CharaView spatial AA: t0 of the equipment display draw ({}) is {}x{} format {}, table entry {} (entry 0 is "
                     "needed); preview images bound: {}",
                     DrawApiName(view.api), desc.Width, desc.Height, static_cast<int>(desc.Format),
                     entry >= 0 ? std::to_string(entry) : std::string("none"), PreviewSlots(context, *view.table));
        return false;
    }

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
    const bool tinted = ext::Cfg().ffxiv.chara.previewDebugTint.value_or_default();
    if (!Pass().Process(context, source.Get(), desc.Width, desc.Height, srgb, scale, &filtered, DrawTriangle, tinted) ||
        filtered == nullptr)
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

    if (tinted)
        ++g_stats.tintApplied;

    // Show the filtered image to this one draw, then give the draw its own image back.
    probe::SwapTrace trace(context, probe::SwapKind::Aa, 0, filtered);
    WithSource0(context, filtered,
                [&]
                {
                    trace.Applied();
                    draw();
                });
    trace.Restored();

    const uint64_t n = ++g_stats.aaProcessed;
    if (n == 1)
        LOG_INFO("CharaView spatial AA: first draw processed, via {}, {}x{} format {} -> {}x{}, processed draws 1",
                 DrawApiName(view.api), desc.Width, desc.Height, static_cast<int>(format), Pass().OutputWidth(),
                 Pass().OutputHeight());
    else if (n % 600 == 0)
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
