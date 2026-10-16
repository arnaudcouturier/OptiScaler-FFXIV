#pragma once
// S10 package A: FFXIV boundary detector (docs/HUDLESS.md section 10). Implements ext::hudless::IHudlessSource and is the
// consumer of the existing lighting draw hooks (ext/ffxiv/lighting/FfxivLightingCapture.h): no Detour of its own.
//
// This header is included by FfxivLightingCapture.h, so it stays light: forward declarations and the arming test only.

#include "ext/ffxiv/ui/UiConfig.h"

#include <cstddef>

struct ID3D11DeviceContext;
struct ID3D11PixelShader;

namespace ext::hudless
{
struct IHudlessSource;
}

namespace ext::ffxiv::ui
{
// The provider object assigned to ext::game::Game::hudless.
ext::hudless::IHudlessSource& Provider();

// Draw hook (FfxivLightingCapture::Around, before its early-outs). `latched` = the draw is on the immediate context the
// lighting hooks latched onto. Never throws.
void OnDraw(ID3D11DeviceContext* context, bool latched);

// Pixel shader creation hook (FfxivLightingCapture::CreateShader): registers and stamps the shader. Never throws.
void TagShader(ID3D11PixelShader* shader, const void* code, size_t size);

// Present entry (ext::dx11bridge::PresentScope constructor): refreshes the arming cache, runs the per-frame bookkeeping.
void OnPresentBegin();
} // namespace ext::ffxiv::ui
