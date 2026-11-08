#pragma once
// Engine light controls, the lamp arm (ENG2): scales the point-light volumes' contribution through the BLEND FACTOR, with no knowledge of the constant buffers.
//
// What the capture shows (draw-diag 20261107-164135, frame 3, 34 draws: 17 per pass, ordinals 547..595 and 695..743): every point-light volume draw has the blend state
// enable / src ONE / dst ONE / op ADD / srcA ZERO / dstA ONE / opA ADD / mask 15 on two R11G11B10_FLOAT targets (the diffuse and specular light buffers) and a pixel
// shader that declares g_LightParam. The directional lights (the moon, its water copy) are not blended; the shadow-mask readers write one R8 target with MAX. So the
// shape is "PS declares g_LightParam" (the seam's kShaftsLightBit) + "additive ONE/ONE/ADD on the bound targets" + "two R11G11B10 targets"; no draw number, no CRC.
//
// The route: the draw's own blend state with SrcBlend ONE replaced by BLEND_FACTOR, the factor (k,k,k,1) set for that one draw. out = src * k + dst. The game's blend
// state, factor and sample mask are put back explicitly after the draw, on every exit (also a throwing draw). A factor of exactly 1 touches NOTHING: no wrapper at all.
// Both light passes (the main one and the water's light pass, which re-draws the same 17 lights into the water light buffers) get the same factor: a lamp is x3
// brighter for the water's body shading as it is for the stone beside it (the brief says "main pass"; this keeps one rule, and says so in the report).
//
// How it composes with the rest of the seam: the arm does not own the draw. EnginePass.cpp substitutes THIS wrapper for the game's draw function before any other
// module sees it, so the ambient occlusion and puddles reader swaps, the screen-space reflections and the pass-through all call the wrapped draw, and the blend state
// is set and restored around the game's one real call (the private passes those modules run before it build their own state).

#include "ext/ffxiv/engine/EnginePassRules.h" // Anchors, IsShaftsLightAnchors, ShaftsSlotOf
#include "ext/ffxiv/enginelight/EngineLightRules.h"

#include <cstdint>
#include <string>

struct ID3D11DeviceContext;

namespace ext::ffxiv::enginelight
{
using DrawFn = void (*)(void*);

// D3D11 values the shape test compares (the header stays free of d3d11.h so the rules tests build without a device).
inline constexpr int kBlendOne = 2;         // D3D11_BLEND_ONE
inline constexpr int kBlendFactor = 14;     // D3D11_BLEND_BLEND_FACTOR
inline constexpr int kBlendOpAdd = 1;       // D3D11_BLEND_OP_ADD
inline constexpr uint32_t kFormatR11G11B10Float = 26; // DXGI_FORMAT_R11G11B10_FLOAT: the light buffers' format

struct BlendRt
{
    bool enable = false;
    int src = 0, dst = 0, op = 0;
};

// One target of the blend state is "additive" when it adds the source at weight one: ONE / ONE / ADD.
constexpr bool IsAdditive(const BlendRt& rt) noexcept { return rt.enable && rt.src == kBlendOne && rt.dst == kBlendOne && rt.op == kBlendOpAdd; }

// The volume shape from what the draw has bound: the blend state (render target 0 decides, render target 1 too when the state is independent) and the two targets' formats.
constexpr bool IsVolumeShape(const BlendRt& rt0, const BlendRt& rt1, bool independent, uint32_t format0, uint32_t format1) noexcept
{
    if (!IsAdditive(rt0))
        return false;
    if (independent && !IsAdditive(rt1))
        return false;
    return format0 == kFormatR11G11B10Float && format1 == kFormatR11G11B10Float;
}

// The wrapper the draw path substitutes for the game's draw function. Lives on the caller's stack for one draw.
struct LampDraw
{
    ID3D11DeviceContext* context = nullptr;
    DrawFn draw = nullptr;
    void* user = nullptr;
    float factor = 1.0f;
    uint32_t lightSlot = 0; // the slot the draw's g_LightParam sits at (the seam's slot field), for the capture frame's dump

    static void Invoke(void* opaque);
};

// The arm's factor now (the applied weight and the keys folded: LampFactor). 1 = the engine's own value.
void SetLampFactor(float factor);
float LampFactorNow();
bool LampsWanted();

// Substitutes the wrapper for `draw`/`user` when this is a light draw and the factor is not 1; false (nothing changed) otherwise. One relaxed load and a bit test on every
// other draw of the frame. `out` must outlive the draw.
bool PrepareLampDraw(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn& draw, void*& user, LampDraw& out);

struct LampStatus
{
    float factor = 1.0f;
    uint64_t scaled = 0;       // volume draws the factor was set for
    uint64_t considered = 0;   // light draws that reached the shape test
    uint64_t notVolume = 0;    // ... and were left alone (the moon, the readers, a one-target draw)
    uint32_t statesCreated = 0;
    bool failed = false;       // a derived blend state could not be created
};
LampStatus CurrentLampStatus();

// Device-side release (the engine seam's Release chain).
void ReleaseLamps();

// Test seam: the capture-frame row writer (null in the product until the draw diagnostic installs it).
using LampRowFn = void (*)(const std::string& csvRow);
void SetLampRowSinkForTest(LampRowFn sink);
std::string LampRowHeader();
} // namespace ext::ffxiv::enginelight
