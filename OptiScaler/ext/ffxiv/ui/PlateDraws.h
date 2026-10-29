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

// The packet record of the draw in progress on this thread (PlatePackets.cpp). `Pending`: the game bound a draw command's state and no draw
// has taken the record yet. `Has`: the draw in progress has its packet outcome decided; the next draw clears it.
inline thread_local bool g_packetPending = false;
inline thread_local bool g_packetHas = false;

namespace detail
{
void VertexShaderCreated(ID3D11VertexShader* shader, const void* code, size_t size);
void InterceptedDraw(ID3D11DeviceContext* context);
void AssignPacket(); // moves the record bound to this thread to the draw in progress (checked against g_args), drops the previous draw's
} // namespace detail

// Every draw hook, before anything else: remembers what the draw was asked to do, and hands the packet record bound to this thread (if
// any) to this draw, so a record can never end up on a later draw. Two relaxed loads while the probe is off or has nothing bound.
inline void NoteDraw(char kind, uint32_t count, uint32_t instances, uint32_t start, int32_t baseVertex)
{
    if (ext::hudless::plate::Enabled())
    {
        g_args = { kind, count, instances, start, baseVertex };
        if (g_packetPending || g_packetHas)
            detail::AssignPacket();
    }
}

// The vertex shader creation hook: stamps the bytecode CRC32 on the object so draws can name their vertex shader.
inline void OnVertexShaderCreated(ID3D11VertexShader* shader, const void* code, size_t size)
{
    if (ext::hudless::plate::Enabled())
        detail::VertexShaderCreated(shader, code, size);
}

// A draw the nameplate visibility layer took over (it never reaches the boundary detector): recorded with the state of the context after
// the layer drew, marked as intercepted.
inline void OnInterceptedDraw(ID3D11DeviceContext* context)
{
    if (ext::hudless::plate::Enabled())
        detail::InterceptedDraw(context);
}

// FfxivUiBoundary.cpp: a counted game draw (after Core::OnDraw), and the end of the frame (before Core::CompleteFrame).
// Both run on the same thread (the draw hooks and the Present are one thread, as the detector assumes) and share unlocked state.
void OnGameDraw(ID3D11DeviceContext* context, Core& core, uint32_t ordinal, uint8_t stage, uint32_t firstUiOrdinal);
void OnFrameEnd(uint64_t frame);
} // namespace ext::ffxiv::ui::plate
