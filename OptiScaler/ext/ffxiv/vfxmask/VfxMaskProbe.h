#pragma once
// The VFX mask gate probe ([FFXIV] VfxMaskProbe = off | left | right, ini only, off by default). One entry point, called from
// ext::dx11bridge::OnUpscaleBoundary (ext/Seams.cpp) on the DirectX 11 bridge's evaluate path, before the parameters go to the
// upscaler. What the probe is for and what each mode does: ext/ffxiv/vfxmask/VfxMaskCore.h.
//
// With the key off the cost is one relaxed load of the armed word. With it on, each evaluate reads the colour input's size,
// creates (once per size or mode change) an R8_UNORM mask filled with the chosen half, and writes the mask key of that
// evaluate's parameter block. Nothing else is touched: no game memory, no shader, no pass, no DirectX 12 resource.
//
// Fail open: a missing colour input, a size that cannot be split, a texture the device refuses, or a mask description that is
// not what was created - each one writes nothing at all, so the game's own NULL for that key reaches the upscaler exactly as
// without this feature. One log line per change of what happened (never one per frame).

struct ID3D11DeviceContext;
struct ID3D11Resource;
struct NVSDK_NGX_Parameter;

namespace ext::ffxiv::vfxmask
{
// One relaxed load of the armed word; false while the key is off.
bool Armed() noexcept;

// End of every config load, with the text of [FFXIV] VfxMaskProbe (ext/core/ExtConfig.cpp hands it over through
// FfxivConfig::PostLoad). An unknown value reads as off.
void OnConfigLoaded(const char* text) noexcept;

// The seam, on the evaluate thread. `color` is the game's own colour input of this evaluate (the bridge's restore value, null
// when the game did not pass one: the probe then reads the parameter block for it).
void OnUpscaleBoundary(ID3D11DeviceContext* context, ID3D11Resource* color,
                       NVSDK_NGX_Parameter* parameters) noexcept;
} // namespace ext::ffxiv::vfxmask
