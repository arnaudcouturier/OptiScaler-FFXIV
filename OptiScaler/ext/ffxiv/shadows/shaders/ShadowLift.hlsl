// The shared remap and tent rules (the same rule the contact combine and the CPU reference pin):
#include "ShadowLiftCommon.hlsli"

Texture2D<float4> Mask : register(t0);

cbuffer Params : register(b0)
{
    float4 Window; // xy = the render window's origin in mask pixels, zw = the mask's size
    float4 Lift;   // x = the effective lift (0 = identity), y = the tent's reach in mask pixels, zw = 0
};

struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput VS(uint id : SV_VertexID)
{
    VSOutput o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Shadow mapping (ext/ffxiv/shadows): the mask lift, the module's own pass (the copy/remap/bind that runs when
// contact shadows is not armed - ext/ffxiv/shadows/ShadowsDispatch.cpp). The contact shadows pass applies the same
// remap as a stage of its combine (ext/ffxiv/relight/shaders/ContactShadows.hlsl), so the two remap blocks are
// written the same way and it is the same test that pins them to the CPU reference.
//
// One pixel shader with a vertex shader, no sampler (every read is a Load on an exact texel, so the result does not
// depend on filter state and a test can predict it):
//   VS       (vs_5_0) one oversized triangle from SV_VertexID
//   PS_Lift  (ps_5_0) the mask's allocation -> the lifted copy the light draw samples
//
// The arithmetic (the design report 2.2, asm 649: the light draw squares the mask it samples):
//   the remap is defined on the SQUARED mask, m'^2 = m^2 + (1-m^2)*Lift, so the copy carries sqrt(m'^2) and the
//   light draw's own square lands back on m'^2. A lift of 0 (or below the rules' identity epsilon) is byte-exact:
//   the shader branches to the mask's own bytes, so the default state and every idle curve give the game's mask, and
//   the picture only moves when the module really lifts.
//   The softness tent follows the remap (it feathers the lift): each texel's value over its 5x5 neighbourhood is
//   the weights sum( max(0, 1 - d/S) ) with d the pixel distance and S the softness key. S at most one pixel reads
//   the 3x3 kernel, two the 5x5 one, and S <= 0 is the texel itself. The blur is over the REMAPPED values, so lifted
//   shadow interiors stay lifted and only the boundary texels mix levels.
//   The render window disciplines the remap: outside it, the copy is the mask's own value even at a live lift (the
//   light draw does not read those texels, and every other reader of the mask sees its own bytes).
//
// Inputs (the register slots and the constant-buffer layout are mirrored by ShadowsPass.cpp; change them together):
//   t0 Mask   Texture2D<float4> the draw's screen-space mask, as the light draw samples it (R8_UNORM this build:
//             the level is in .r; B8G8R8A8_UNORM the benchmark build: the level is in .rgb, .a is not a mask field)
//   b0 Params float4 Window     xy = the render window's origin in mask pixels, zw = the mask's size
//             float4 Lift       x = the effective lift (0 = identity), y = the tent's reach in pixels, zw = 0
//
// Outputs: SV_Target0 float4, written to the module's own copy of the mask (the typed member of the mask's format
// family, sized to the engine's allocation - ShadowsPass.cpp creates it; nothing of the game is written).

float4 PS_Lift(float4 position : SV_Position) : SV_Target0
{
    const int2 at = int2(position.xy);
    const float4 mask = Mask.Load(int3(at, 0));
    const float lift = Lift.x;

    // Identity first: the branch the default state and every idle curve take - the mask's own bytes, byte for byte.
    if (!(lift > SHADOW_LIFT_IDENTITY_EPSILON))
        return mask;

    // Outside the render window: the mask's own bytes, whatever the keys say. The light draw does not read those
    // texels, and a reader that does (a filter at the window's edge) sees exactly what it saw before.
    const float2 relative = position.xy - Window.xy;
    if (relative.x < 0.0 || relative.y < 0.0 || relative.x >= Window.z || relative.y >= Window.w)
        return mask;

    const float softness = Lift.y;
    if (!(softness > 0.0))
    {
        // The remap without a tent: one remapped value per channel.
        return float4(ShadowLiftRemappedCopy(mask.r, lift), ShadowLiftRemappedCopy(mask.g, lift), ShadowLiftRemappedCopy(mask.b, lift), mask.a);
    }

    // The tent over the remapped values, weights 5x5 with the reach. A load outside the allocation reads zero,
    // which is not a shadowed texel but nothing at all, so an out-of-range neighbour is dropped instead and the
    // weights renormalise over the texels the kernel carried.
    float sumW = 0.0, sumR = 0.0, sumG = 0.0, sumB = 0.0;
    [unroll]
    for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll]
        for (int dx = -2; dx <= 2; ++dx)
        {
            const float w = ShadowLiftTentWeight(float(dx), float(dy), softness);
            if (!(w > 0.0))
                continue;
            const int2 neighbourAt = at + int2(dx, dy);
            if (neighbourAt.x < 0 || neighbourAt.y < 0 || neighbourAt.x >= int(Window.z) || neighbourAt.y >= int(Window.w))
                continue;
            const float4 neighbour = Mask.Load(int3(neighbourAt, 0));
            sumW += w;
            sumR += w * ShadowLiftRemappedCopy(neighbour.r, lift);
            sumG += w * ShadowLiftRemappedCopy(neighbour.g, lift);
            sumB += w * ShadowLiftRemappedCopy(neighbour.b, lift);
        }
    }
    if (!(sumW > 0.0))
        return float4(ShadowLiftRemappedCopy(mask.r, lift), ShadowLiftRemappedCopy(mask.g, lift), ShadowLiftRemappedCopy(mask.b, lift), mask.a);
    return float4(sumR / sumW, sumG / sumW, sumB / sumW, mask.a);
}
