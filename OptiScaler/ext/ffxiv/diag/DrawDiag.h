#pragma once
// Developer draw diagnostic, hook side. Log only, behind [FFXIV] DrawDiagnostics (ext::F::DrawDiag, off by default, ini only).
//
// This header is included by FfxivLightingCapture.h and by ext/Seams.cpp, so it stays light: the key test is ONE relaxed atomic
// load and the branch is not taken in a normal session. Everything else - the D3D state reads, the reflected names, the CSV
// text and the files - is out of line in FfxivDrawDiag.cpp.
//
// Threads: the draw, the frame end and the constant-buffer reads run on the render thread (the draw hooks, the Present entry and
// the upscaler evaluate are that thread). Shader creation may run on any thread and only fills the reflection table.

#include "ext/ExtFeatures.h"

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11PixelShader;

namespace ext::ffxiv::diag
{
// A capture is running: the key is `capture` and the requested frames have not been written yet.
inline bool Capturing() { return ext::Active(ext::F::DrawDiag); }

// Draw hook (FfxivLightingCapture::Around, before its early-outs). `api` is the name of the draw call the hook is in.
void OnDraw(ID3D11DeviceContext* context, const char* api);

// Pixel shader creation hook (FfxivLightingCapture::CreateShader): stamps the bytecode CRC on the shader object and remembers the
// names and slots the bytecode declares, once per shader.
void OnPixelShaderCreated(ID3D11PixelShader* shader, const void* code, size_t size, uint32_t crc);

// The upscaler evaluate of the frame (ext::dx11bridge::OnUpscaleBoundary): marks the point between two draws.
void OnUpscaleBoundary();

// Present entry (ext::ffxiv::ui::OnPresentBegin): closes the frame the draw hooks collected and writes the files at the end.
void OnPresent();

// End of config::Load (ext::FfxivConfig::PostLoad): arms a capture, or disarms it. A capture happens once per session.
void OnConfigLoaded(bool capture);
} // namespace ext::ffxiv::diag
