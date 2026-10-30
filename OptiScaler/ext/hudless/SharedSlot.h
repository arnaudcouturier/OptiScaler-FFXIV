#pragma once
// S10 package B: one shared D3D11 -> D3D12 texture of the hud-less slot ring (docs/HUDLESS.md section 6). Split out of the
// engine so the WARP test can create it without a bridge.

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace ext::hudless
{
struct SharedTexture
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture; // on the game's device, written by its immediate context
    HANDLE handle = nullptr;                         // the NT handle both devices opened it through
    Microsoft::WRL::ComPtr<ID3D12Resource> resource; // the same memory on the bridge's D3D12 device

    // Closes the handle (the resources keep the memory alive) and drops both references
    void Reset();
};

// Creates a default-usage, single-sample D3D11 texture of `format` (a typed format) that D3D12 can sample (a shader
// resource) and open it on `device12`. Fails (and leaves `out` empty) when the opened resource denies shader resource
// access, which is what the mask and paste shaders need. `renderTarget` also makes the texture bindable as a render
// target, so a caller can draw into the shared memory through a D3D11 render target view instead of copying into it; it
// then also fails when D3D12 sees the opened resource deny render target access. Off by default: the hud-less slot ring
// copies into its textures and needs no render target flag.
HRESULT CreateSharedTexture(ID3D11Device* device11, ID3D12Device* device12, UINT width, UINT height, DXGI_FORMAT format,
                            SharedTexture& out, bool renderTarget = false);
} // namespace ext::hudless
