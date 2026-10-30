#pragma once
// The mask texture of the VFX mask gate probe (ext/ffxiv/vfxmask/VfxMaskCore.h): one R8_UNORM texture the size of the colour
// input, filled once per size or mode change and handed to the mask input of every evaluate while the probe is on.
//
// A plain D3D11 texture is all the DirectX 11 bridge needs: it copies the resource like the other upscaler inputs, so nothing
// here touches the DirectX 12 side (no interop, no shader, no pass). The fill is one UpdateSubresource per change, which is why
// the cost of the probe is a texture description read and one parameter write per frame.

#include "ext/ffxiv/vfxmask/VfxMaskCore.h"

#include <d3d11.h>
#include <wrl/client.h>

namespace ext::ffxiv::vfxmask
{
// The mask the probe keeps between evaluates. It is recreated when the colour input's size, the mode or the device context
// changes; a context change also covers a recreated device, whose textures the old ones would not be valid for.
struct MaskTexture
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    // Held only so the texture's device outlives it; the texture is rebuilt when the evaluate's context changes, which also covers
    // a device the game recreated.
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    ID3D11DeviceContext* context = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    ProbeMode mode = ProbeMode::Off;

    void Reset() noexcept
    {
        texture.Reset();
        device.Reset();
        context = nullptr;
        width = 0;
        height = 0;
        mode = ProbeMode::Off;
    }
};

// What EnsureMaskTexture reports: the texture to hand over, and when it is null the reason for the caller's log line.
struct EnsureResult
{
    ID3D11Texture2D* texture = nullptr;
    const char* reason = nullptr;
    long error = 0; // the HRESULT of a failed D3D call, 0 when there was none
};

// True when the description is a mask this feature may hand over: a single-sample, single-mip, single-array 2D R8_UNORM
// texture of a size the plan accepts. Read on every evaluate, so a texture that is not what it was created as (or a device that
// replaced it) is never given to the upscaler.
bool ValidMaskDesc(const D3D11_TEXTURE2D_DESC& desc) noexcept;

// The mask for this evaluate, created and filled on `context`'s device when the cache does not already hold one of this size
// and mode. Never throws on a D3D failure: the result carries the reason instead.
EnsureResult EnsureMaskTexture(ID3D11DeviceContext* context, unsigned width, unsigned height, ProbeMode mode,
                               MaskTexture& cache) noexcept;
} // namespace ext::ffxiv::vfxmask
