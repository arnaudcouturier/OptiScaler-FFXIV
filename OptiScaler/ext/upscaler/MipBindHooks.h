#pragma once
// Bind-time mipmap bias: the glue to the game (docs/FEATURES.md "Automatic mipmap bias"). The decisions are in MipBind.h, the
// sampler table and the bind path in MipBindEngine.h (both unit-tested); this file attaches them to the game's D3D11 device
// and context, reads the settings and the evaluate sizes, and logs.
//
// Entry points, all reached from seams that already exist (no upstream line):
//   OnDeviceHooked  <- ext::game::OnD3D11DeviceHooked (Seams.cpp), matched FFXIV build only
//   OnUnhook        <- ext::game::OnD3D11Unhook
//   OnEvaluate      <- ext::dx11bridge::OnUpscaleBoundary, every DX11 NGX evaluate, on the context's own thread
// The older create-time module (MipBias.cpp) stands down while Owns() is true.

#include "ext/upscaler/MipBind.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct NVSDK_NGX_Parameter;

namespace ext::upscaler::mipbind
{
// A matched FFXIV build's device reached the hook seam: remember it, and attach the hooks when the setting is on.
void OnDeviceHooked(ID3D11Device* device);

// The device is going away: put the game's samplers back, detach, forget.
void OnUnhook();

// An evaluate of the DX11 upscaler: read the sizes, move the target, rebind when it changed.
void OnEvaluate(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters);

// The hooks are attached: samplers are bound with the bias, so the older module that writes MipmapBiasOverride at sampler
// creation must not also run.
bool Owns();

// [Mipmap] MipmapBiasAuto, the alias AutomaticFFXIV and the game default (on for FFXIV) combined.
bool AutoEnabled();

// The menu changed the checkbox or the offset: attach the hooks when the setting just came on.
void OnSettingChanged();

BindStatus CurrentBindStatus();
} // namespace ext::upscaler::mipbind
