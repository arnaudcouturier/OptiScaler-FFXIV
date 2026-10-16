#include "pch.h"

#include "ext/hudless/SharedSlot.h"

namespace ext::hudless
{
void SharedTexture::Reset()
{
    if (handle != nullptr)
    {
        CloseHandle(handle);
        handle = nullptr;
    }
    resource.Reset();
    texture.Reset();
}

HRESULT CreateSharedTexture(ID3D11Device* device11, ID3D12Device* device12, UINT width, UINT height, DXGI_FORMAT format,
                            SharedTexture& out)
{
    out.Reset();
    if (device11 == nullptr || device12 == nullptr || width == 0 || height == 0)
        return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; // without it D3D12 sees DENY_SHADER_RESOURCE
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    HRESULT hr = device11->CreateTexture2D(&desc, nullptr, &out.texture);
    Microsoft::WRL::ComPtr<IDXGIResource1> shared;
    if (SUCCEEDED(hr))
        hr = out.texture.As(&shared);
    if (SUCCEEDED(hr))
        hr = shared->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &out.handle);
    if (SUCCEEDED(hr))
        hr = device12->OpenSharedHandle(out.handle, IID_PPV_ARGS(&out.resource));

    if (SUCCEEDED(hr) && (out.resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0)
        hr = E_FAIL;

    if (FAILED(hr))
        out.Reset();
    return hr;
}
} // namespace ext::hudless
