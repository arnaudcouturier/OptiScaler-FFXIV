#include "pch.h"

#include "ext/ffxiv/chara/CharaFlood.h"

#include "ext/ffxiv/chara/CharaProbeHooks.h"
#include "ext/ffxiv/chara/CharaProbeRules.h"
#include "ext/ffxiv/chara/CharaStats.h"

namespace ext::ffxiv::chara::flood
{
using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT kSlots = 8;               // pixel-shader slots the generic swap looks at
constexpr UINT kSide = 8;                // the magenta texture is kSide x kSide
constexpr uint64_t kPollMs = 100;        // the swap counter is read this often

struct State
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> unorm;
    ComPtr<ID3D11ShaderResourceView> srgb;
    bool failed = false; // creation failed on `device`: not retried until Release

    bool on = false; // the key as last seen by Update
    uint64_t lastPoll = 0;
    uint64_t lastSwaps = 0; // g_stats.floodSwaps at the last poll
    probe::ThrottledSwapCounter counter;
};

// Never destroyed: the process may unload this module under the loader lock, where releasing D3D objects is not safe.
State& S()
{
    static auto* state = new State;
    return *state;
}

bool Create(ID3D11Device* device)
{
    State& s = S();
    uint32_t pixels[kSide * kSide];
    for (uint32_t& p : pixels)
        p = 0xFFFF00FFu; // R 255, G 0, B 255, A 255 in R8G8B8A8 byte order (little endian: A B G R)

    D3D11_TEXTURE2D_DESC td {};
    td.Width = kSide;
    td.Height = kSide;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data { pixels, kSide * 4, 0 };

    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> unorm;
    ComPtr<ID3D11ShaderResourceView> srgb;
    HRESULT hr = device->CreateTexture2D(&td, &data, &texture);
    if (SUCCEEDED(hr))
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd {};
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = device->CreateShaderResourceView(texture.Get(), &vd, &unorm);
        if (SUCCEEDED(hr))
        {
            vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            hr = device->CreateShaderResourceView(texture.Get(), &vd, &srgb);
        }
    }
    if (FAILED(hr))
    {
        LOG_WARN("CharaView probe: the flood test could not create its magenta texture (HRESULT {:#x}); it does nothing this session",
                 static_cast<uint32_t>(hr));
        s.failed = true;
        return false;
    }
    s.device = device;
    s.texture = std::move(texture);
    s.unorm = std::move(unorm);
    s.srgb = std::move(srgb);
    return true;
}
} // namespace

bool SamplesAsFloat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_UNKNOWN:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
        return false;
    default:
        return true;
    }
}

bool CanReplace(const D3D11_SHADER_RESOURCE_VIEW_DESC& desc)
{
    return desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && SamplesAsFloat(desc.Format);
}

ID3D11ShaderResourceView* View(ID3D11DeviceContext* context, DXGI_FORMAT replacedFormat)
{
    if (context == nullptr)
        return nullptr;
    State& s = S();
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (device == nullptr)
        return nullptr;
    if (s.texture != nullptr && s.device.Get() != device.Get())
        Release(); // another device: the texture cannot be used with it
    if (s.texture == nullptr)
    {
        if (s.failed || !Create(device.Get()))
            return nullptr;
    }
    return IsSrgbFamily(replacedFormat) ? s.srgb.Get() : s.unorm.Get();
}

void Release()
{
    State& s = S();
    s.unorm.Reset();
    s.srgb.Reset();
    s.texture.Reset();
    s.device.Reset();
    s.failed = false;
}

bool Created() { return S().texture != nullptr; }

bool SwapAndDraw(ID3D11DeviceContext* context, uint32_t slotMask, DrawCall draw)
{
    if (context == nullptr || slotMask == 0 || draw.function == nullptr)
        return false;

    ID3D11ShaderResourceView* original[kSlots] {};
    context->PSGetShaderResources(0, kSlots, original);

    ID3D11ShaderResourceView* replaced[kSlots] {};
    int first = -1;
    for (UINT i = 0; i < kSlots; ++i)
    {
        replaced[i] = original[i];
        if (original[i] == nullptr || (slotMask & (1u << i)) == 0)
            continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd {};
        original[i]->GetDesc(&vd);
        if (!CanReplace(vd))
            continue;
        ID3D11ShaderResourceView* magenta = View(context, vd.Format);
        if (magenta == nullptr)
            continue;
        replaced[i] = magenta;
        if (first < 0)
            first = static_cast<int>(i);
    }

    bool ran = false;
    if (first >= 0)
    {
        probe::SwapTrace trace(context, probe::SwapKind::Flood, static_cast<uint32_t>(first), replaced[first]);
        context->PSSetShaderResources(0, kSlots, replaced);
        trace.Applied();
        draw();
        context->PSSetShaderResources(0, kSlots, original);
        trace.Restored();
        ++g_stats.floodSwaps;
        ran = true;
    }
    for (ID3D11ShaderResourceView* v : original)
    {
        if (v != nullptr)
            v->Release();
    }
    return ran;
}

void Update(uint64_t now)
{
    State& s = S();
    const bool want = On();
    if (want != s.on)
    {
        s.on = want;
        s.counter.Clear();
        s.lastPoll = now;
        s.lastSwaps = g_stats.floodSwaps.load(std::memory_order_relaxed);
        if (want)
            LOG_INFO("CharaView probe: flood on; draws that sample the preview image (or a copy of it that was followed) get a solid "
                     "magenta image instead. A draw that reads the image with texel loads instead of sampling shows black outside the "
                     "8x8 corner");
        else
        {
            LOG_INFO("CharaView probe: flood off (flood swaps so far {})", g_stats.floodSwaps.load(std::memory_order_relaxed));
            Release();
        }
        return;
    }
    if (!s.on || (now >= s.lastPoll && now - s.lastPoll < kPollMs))
        return;
    s.lastPoll = now;

    const uint64_t swaps = g_stats.floodSwaps.load(std::memory_order_relaxed);
    const uint64_t delta = swaps >= s.lastSwaps ? swaps - s.lastSwaps : 0;
    s.lastSwaps = swaps;
    s.counter.Add(now, static_cast<uint32_t>(delta > UINT32_MAX ? UINT32_MAX : delta));
    probe::ThrottledSwapCounter::Line line;
    while (s.counter.TakeLine(now, line))
        LOG_INFO("CharaView probe: flood swaps {} in the last second (total {})", line.perSecond, line.total);
}
} // namespace ext::ffxiv::chara::flood
