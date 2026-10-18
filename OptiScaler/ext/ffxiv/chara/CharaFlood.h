#pragma once
// The flood test of the character preview feature (config key [FFXIV] PreviewDebugFlood, debug aid, off by default): every draw that
// samples the preview image gets a solid magenta image instead, so the maintainer can see at once whether the swap reaches the
// pixels on screen. It does not depend on PreviewAA, PreviewDLSS or PreviewProbe, and it does not depend on whether the AA or the
// DLSS path could produce anything.
//
// This unit owns the magenta texture (8x8, created once per device, released when the key goes off and by the probe's Detach), the
// choice of the view that matches the replaced view's format family, the swap of the pixel-shader resources of a draw that no module
// handled, and the "swaps per second" log line. The AA module (CharaEnhance.cpp) and the DLSS consumer (CharaDlss.cpp) take the view
// from here, so all three paths bind the same image. Draw thread only (the menu reads g_stats.floodSwaps).

#include <cstdint>
#include <d3d11.h>

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDraw.h"

namespace ext::ffxiv::chara::flood
{
// [FFXIV] PreviewDebugFlood.
inline bool On() { return ext::Cfg().ffxiv.chara.previewDebugFlood.value_or_default(); }

// Views whose format family is sRGB get the sRGB view of the magenta texture, every other view the plain one.
constexpr bool IsSrgbFamily(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

// False for formats a float-sampling shader cannot read as colour (integer formats, unknown): the flood leaves those views alone
// instead of binding an image the shader would read as garbage.
bool SamplesAsFloat(DXGI_FORMAT format);

// True when a view can be replaced by the flood image: a plain 2D texture view (not an array, not multisampled) of a colour format.
bool CanReplace(const D3D11_SHADER_RESOURCE_VIEW_DESC& desc);

// The magenta view for a view of `replacedFormat`; created on first use on the device of `context`. Null when it cannot be created
// (one WARN line). The pointer is owned by this unit and valid until Release.
ID3D11ShaderResourceView* View(ID3D11DeviceContext* context, DXGI_FORMAT replacedFormat);

// The format of the view View() hands out for a replaced view of `replacedFormat` (UNORM or UNORM_SRGB).
constexpr DXGI_FORMAT ViewFormat(DXGI_FORMAT replacedFormat)
{
    return IsSrgbFamily(replacedFormat) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
}

// Releases the texture and the views.
void Release();
bool Created();

// A draw that no module handled: replaces the pixel-shader resources of the first eight slots named by `slotMask` that hold a
// plain 2D colour view by the magenta image, runs `draw` once, then binds the game's own views again. Reads the bound views at call
// time (the mask is only a hint). Returns false without running `draw` when nothing could be replaced; the caller then runs the
// draw itself. Counts the swap (g_stats.floodSwaps) and reports it to the probe's swap trace as kind Flood.
bool SwapAndDraw(ID3D11DeviceContext* context, uint32_t slotMask, DrawCall draw);

// Once per dispatched draw (cheap): logs the switch on and off, releases the texture when the key goes off, and writes the swaps per
// second line (the first 30 seconds with swaps every second, then every 10th, at most 60 lines).
void Update(uint64_t now);
} // namespace ext::ffxiv::chara::flood
