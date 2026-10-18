#pragma once
// S10 package A: FFXIV boundary detector (docs/HUDLESS.md section 10). Implements ext::hudless::IHudlessSource and is the
// consumer of the existing lighting draw hooks (ext/ffxiv/lighting/FfxivLightingCapture.h): no Detour of its own.
//
// This header is included by FfxivLightingCapture.h, so it stays light: forward declarations and the arming test only.

#include "ext/ffxiv/ui/UiConfig.h"

#include <cstddef>
#include <cstdint>

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

// Pixel shader creation hook (FfxivLightingCapture::CreateShader), called for every DXBC pixel shader with the CRC32 of its bytecode
// (computed once by the hook). Stamps the catalog stages ALWAYS (a Mode switched on later needs them), and runs the D1 registry only
// while Armed(). Never throws; does nothing unless the game module is a matched FFXIV build.
void OnShaderCreated(ID3D11PixelShader* shader, uint32_t crc, uint32_t bytes);

// Identity of the swapchain buffer the bridge copies from: the pointer value (never dereference it), its size and DXGI format.
// Looks the buffer up through the swapchain on every call and keeps no reference (a retained reference would make ResizeBuffers
// fail), so callers cache the answer; it is meant for log lines and debug probes, never for per-draw use. False when unknown.
bool BackbufferIdentity(uintptr_t* texture, uint32_t* width, uint32_t* height, uint32_t* format);

// Present entry (ext::dx11bridge::PresentScope constructor): refreshes the arming cache, runs the per-frame bookkeeping.
void OnPresentBegin();
} // namespace ext::ffxiv::ui
