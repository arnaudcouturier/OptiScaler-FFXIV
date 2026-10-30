#pragma once
// Developer probe of the UI draws (docs/HUDLESS.md, "Developer probes"), draw side. Log only, behind [HudLess] PlateDiagnostics
// (ext::hudless::plate::Enabled()). This header is included by FfxivLightingCapture.h, so it stays light: the arguments of the draw in
// progress are remembered with one atomic test, everything else is out of line in PlateDraws.cpp.

#include "ext/hudless/PlateProbe.h"

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11VertexShader;

namespace ext::ffxiv::ui
{
class Core;
}

namespace ext::ffxiv::ui::plate
{
struct DrawArgs
{
    char kind = 'I';    // D draw, I indexed, N instanced, X indexed instanced, Y indirect
    uint32_t count = 0; // indices of an indexed draw, vertices of the others
    uint32_t instances = 1;
    uint32_t start = 0;
    int32_t baseVertex = 0;
};
inline thread_local DrawArgs g_args;

namespace detail
{
void VertexShaderCreated(ID3D11VertexShader* shader, const void* code, size_t size);
} // namespace detail

// Every draw hook, before anything else: remembers what the draw was asked to do. One atomic test while both probes are off; it serves
// the UI draw probe and the draw diagnostic (ext/ffxiv/diag), which is why it tests both feature bits at once (one relaxed load either
// way).
inline void NoteDraw(char kind, uint32_t count, uint32_t instances, uint32_t start, int32_t baseVertex)
{
    if (ext::ActiveAny(ext::F::PlateProbe, ext::F::DrawDiag))
        g_args = { kind, count, instances, start, baseVertex };
}

// The vertex shader creation hook: stamps the bytecode CRC32 on the object so draws can name their vertex shader.
inline void OnVertexShaderCreated(ID3D11VertexShader* shader, const void* code, size_t size)
{
    if (ext::hudless::plate::Enabled())
        detail::VertexShaderCreated(shader, code, size);
}

// FfxivUiBoundary.cpp: a counted game draw (after Core::OnDraw), and the end of the frame (before Core::CompleteFrame).
// Both run on the same thread (the draw hooks and the Present are one thread, as the detector assumes) and share unlocked state.
void OnGameDraw(ID3D11DeviceContext* context, Core& core, uint32_t ordinal, uint8_t stage, uint32_t firstUiOrdinal);
void OnFrameEnd(uint64_t frame);
} // namespace ext::ffxiv::ui::plate
