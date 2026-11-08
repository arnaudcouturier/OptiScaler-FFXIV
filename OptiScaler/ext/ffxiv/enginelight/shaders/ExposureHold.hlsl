// ExposureHold.hlsl: this fork's exposure hold (engine light controls, ENG2), chained at the tone map in the slot the retired night grade had.
//
// One vertex shader, one pixel shader, two constant buffers, no sampler (every read is a Load on an exact texel, so the result does not depend on filter state and a
// test can predict it).
//
// What it does. The engine stores the scene as sqrt(C) * s, where `s` is its own exposure (g_CommonParameter byte 48, m_Misc2.x), measures the stored scene AFTER that
// multiply (the luminance measure runs after the composite) and raises `s` when the lights are darkened, which would undo the darkening. This pass rescales the stored
// scene by a gain at the tone map's input:
//   mode 1 (auto hold)  gain = clamp(s_hold / s_now, gainMin, gainMax), with s_now read ON THE GPU from this frame's own copy of g_CommonParameter (b1, copied at the
//                       composite), so a frame whose exposure the CPU has not seen yet is still exact: stored' = sqrt(C) * s_now * s_hold / s_now = sqrt(C) * s_hold.
//                       The copy must pass the fingerprint y = max(1, 2.04 s^2) (the same rule as EngineLightRules.h ExposureValid); anything else is gain 1.
//   mode 2 (manual)     gain = the factor the CPU eased with the night weight (Params.y).
// The alpha channel is passed through. The maths is EngineLightRules.h (HoldGain, ExposureValid, StoredGain): change them together.
//
// This file is this stage's own: it includes nothing. The register slots and the constant-buffer layout are mirrored by EngineLightPass.cpp.

cbuffer Params : register(b0)
{
    float4 Mode;   // x = mode (1 auto hold, 2 manual), y = the manual gain (mode 2), z = s_hold (mode 1), w = 0
    float4 Limits; // x = gain min, y = gain max, z = the fingerprint's relative tolerance, w = its absolute tolerance
    float4 Valid;  // x = the smallest valid s, y = the largest valid s, z = the fingerprint's factor (2.04), w = 0
};

cbuffer Common : register(b1)
{
    float4 CommonRow0;
    float4 CommonRow1;
    float4 CommonRow2;
    float4 Misc2; // g_CommonParameter.m_Misc2 = (s, y = max(1, 2.04 s^2), FLT_MAX, 0)
};

Texture2D<float4> Scene : register(t0); // the scene the chain handed in (read only, never written)

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

float GainNow()
{
    if (Mode.x > 1.5)
        return Mode.y; // manual
    const float s = Misc2.x;
    const float y = Misc2.y;
    const float expectedY = max(1.0, Valid.z * s * s);
    // a NaN s fails every comparison: gain 1
    const bool ok = s >= Valid.x && s <= Valid.y && abs(y - expectedY) <= Limits.z * expectedY + Limits.w;
    if (!ok || !(Mode.z > 0.0))
        return 1.0;
    return clamp(Mode.z / s, Limits.x, Limits.y);
}

float4 PS_Hold(VSOutput i) : SV_Target
{
    const float4 scene = Scene.Load(int3(int2(i.position.xy), 0));
    return float4(scene.rgb * GainNow(), scene.a);
}
