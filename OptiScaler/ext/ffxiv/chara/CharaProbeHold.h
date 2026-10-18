#pragma once
// A reference to a texture for the probe's derived set (CharaProbeRules.h, Derived::hold). Separate from the probe so a test can link it.

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <memory>

namespace ext::ffxiv::chara::probe
{
// True when the resource is a buffer of a swapchain (DXGI marks it with DXGI_USAGE_BACK_BUFFER). A reference held to one makes the game's
// IDXGISwapChain::ResizeBuffers fail.
inline bool IsSwapchainBuffer(ID3D11Resource* resource)
{
    Microsoft::WRL::ComPtr<IDXGIResource> dxgi;
    if (resource == nullptr || FAILED(resource->QueryInterface(IID_PPV_ARGS(&dxgi))) || dxgi == nullptr)
        return false;
    DXGI_USAGE usage = 0;
    return SUCCEEDED(dxgi->GetUsage(&usage)) && (usage & DXGI_USAGE_BACK_BUFFER) != 0;
}

// A reference to a derived texture for the derived set, or an empty one when it must not be held: a swapchain buffer. A buffer is
// recognised by being the backbuffer the boundary module reports (`isBackbuffer`) and by DXGI's own back-buffer usage flag (the other
// buffers of a flip-model swapchain).
inline std::shared_ptr<void> HoldOf(ID3D11Resource* resource, bool isBackbuffer)
{
    if (resource == nullptr || isBackbuffer || IsSwapchainBuffer(resource))
        return {};
    resource->AddRef();
    return std::shared_ptr<void>(resource, [](void* p) { static_cast<ID3D11Resource*>(p)->Release(); });
}
} // namespace ext::ffxiv::chara::probe
