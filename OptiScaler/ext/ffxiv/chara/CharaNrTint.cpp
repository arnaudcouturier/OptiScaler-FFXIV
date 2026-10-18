#include "pch.h"

#include "ext/ffxiv/chara/CharaNrPlumbing.h"
#include "ext/ffxiv/chara/CharaNrTint.h"

namespace ext::ffxiv::chara::nr
{
using Microsoft::WRL::ComPtr;

ComPtr<ID3D11ShaderResourceView> TintTargets::Tinted(guides::Passes& passes, ID3D11Device* device, ID3D11DeviceContext* context,
                                                     uintptr_t key, ID3D11ShaderResourceView* source, uint64_t serial, uint64_t now,
                                                     bool* drawn)
{
    if (drawn != nullptr)
        *drawn = false;
    if (key == 0 || source == nullptr || device == nullptr || context == nullptr || serial == 0)
        return nullptr;

    Entry* entry = nullptr;
    Entry* oldest = &_entries[0];
    for (Entry& e : _entries)
    {
        if (e.key == key)
        {
            entry = &e;
            break;
        }
        if (e.key == 0 || (oldest->key != 0 && e.lastUse < oldest->lastUse))
            oldest = &e;
    }
    if (entry != nullptr)
    {
        entry->lastUse = now;
        // The target already shows this result: no second pass for the next UI draw that samples the same image.
        if (entry->srv != nullptr && !ResultTintPassDue(true, ConsumerSource::NrResult, entry->serial, serial))
            return entry->srv;
    }

    ComPtr<ID3D11Resource> resource;
    source->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc {};
    if (resource == nullptr || FAILED(resource.As(&texture)))
        return nullptr;
    texture->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0)
        return nullptr;

    if (entry == nullptr)
    {
        entry = oldest; // a free entry, else the one asked for longest ago
        *entry = Entry {};
        entry->key = key;
    }
    if (entry->texture == nullptr || entry->width != desc.Width || entry->height != desc.Height || entry->format != desc.Format)
    {
        entry->texture.Reset();
        entry->srv.Reset();
        entry->rtv.Reset();
        entry->serial = 0;
        D3D11_TEXTURE2D_DESC td {};
        td.Width = desc.Width;
        td.Height = desc.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = desc.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &entry->texture)) ||
            FAILED(device->CreateShaderResourceView(entry->texture.Get(), nullptr, &entry->srv)) ||
            FAILED(device->CreateRenderTargetView(entry->texture.Get(), nullptr, &entry->rtv)))
        {
            *entry = Entry {};
            return nullptr;
        }
        entry->width = desc.Width;
        entry->height = desc.Height;
        entry->format = desc.Format;
    }
    entry->key = key;
    entry->lastUse = now;

    guides::Settings settings; // zero jitter: the alpha is read where it is
    settings.invWidth = 1.0f / static_cast<float>(desc.Width);
    settings.invHeight = 1.0f / static_cast<float>(desc.Height);
    guides::Passes::Scope scope(passes, context, settings, nullptr, 0, 0);
    if (!scope.Ok() || !scope.TintResult(source, entry->rtv.Get(), desc.Width, desc.Height))
    {
        entry->serial = 0; // the target holds nothing usable
        return nullptr;
    }
    entry->serial = serial;
    if (drawn != nullptr)
        *drawn = true;
    return entry->srv;
}

void TintTargets::Release(uint64_t now, uint64_t unusedMs)
{
    for (Entry& e : _entries)
    {
        if (e.key != 0 && (now < e.lastUse || now - e.lastUse > unusedMs))
            e = Entry {};
    }
}

void TintTargets::Clear()
{
    for (Entry& e : _entries)
        e = Entry {};
}

size_t TintTargets::Count() const
{
    size_t n = 0;
    for (const Entry& e : _entries)
        n += e.key != 0 ? 1 : 0;
    return n;
}
} // namespace ext::ffxiv::chara::nr
