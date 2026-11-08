#pragma once

#include <array>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ext::ffxiv::engine::ssr::native_context
{
using Microsoft::WRL::ComPtr;
inline constexpr GUID ReShadeUnwrappedObject {0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};

// Match FfxivLightingScan's bounded ReShade 6.8 unwrapping: canonical IUnknown
// identity at each hop, and fail closed for broken, cyclic, or excessive chains.
inline ComPtr<IUnknown> Identity(IUnknown* object)
{
    if (!object) return {};
    ComPtr<IUnknown> current = object;
    std::array<ComPtr<IUnknown>, 8> seen;
    for (size_t i = 0; i < seen.size(); ++i)
    {
        auto& identity = seen[i];
        if (FAILED(current.As(&identity))) return {};
        for (size_t j = 0; j < i; ++j)
            if (seen[j] == identity) return {};
        ComPtr<IUnknown> original;
        const HRESULT hr = current->QueryInterface(ReShadeUnwrappedObject, reinterpret_cast<void**>(original.GetAddressOf()));
        if (hr == E_NOINTERFACE) return identity;
        if (FAILED(hr) || !original) return {};
        current = std::move(original);
    }
    return {};
}

inline bool Same(IUnknown* a, IUnknown* b)
{
    if (!a || !b) return false;
    const auto x = Identity(a), y = Identity(b);
    return x && y && x == y;
}

inline bool VerifiedRoute(ID3D11DeviceContext* incoming, ID3D11DeviceContext* candidate, ID3D11Device* device,
                          ID3D11DeviceChild* scene, ID3D11DeviceChild* planar, ID3D11DeviceChild* vposition,
                          ID3D11DeviceChild* camera)
{
    if (!incoming || !candidate || !device || !scene || !planar ||
        incoming->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || candidate->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        !Same(incoming, candidate))
        return false;
    ComPtr<ID3D11Device> incomingDevice, candidateDevice;
    incoming->GetDevice(&incomingDevice);
    candidate->GetDevice(&candidateDevice);
    if (!Same(incomingDevice.Get(), device) || !Same(candidateDevice.Get(), device)) return false;
    const auto childUsesDevice = [device](ID3D11DeviceChild* child) {
        if (!child) return true;
        ComPtr<ID3D11Device> childDevice;
        child->GetDevice(&childDevice);
        return Same(childDevice.Get(), device);
    };
    return childUsesDevice(scene) && childUsesDevice(planar) && childUsesDevice(vposition) && childUsesDevice(camera);
}
}
