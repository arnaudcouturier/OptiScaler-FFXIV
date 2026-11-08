// Meter.hlsl: this fork's effect meter. One vertex shader, one pixel shader, one constant buffer, no sampler (every read is a Load on an exact texel).
//
// The pixel shader draws over a small target of float4 texels. Each target texel is one BLOCK of the measured box (Geom.x texels on a side) and reads
// Geom.y x Geom.y stratified, jittered taps of it (every texel of the block when the block is 4 or smaller), loads the stage's input (t0) and output (t1) at
// each tap and writes (taps in the region, visible taps in the region, the sum of the change over the region taps, taps in the box). The CPU adds the texels
// in doubles (MeterRules.h Reduce). Only pixel-shader resources are used, so the explicit-state guard covers everything the draw touches.
//
// The maths is MeterRules.h (the host reference the tests run beside this shader; change them together):
//   level(x) = saturate(x / white) * 255   (Mode.z = 0, the tone-map chain's stored colour)  |  saturate(x) * 255   (Mode.z = 1, a UNORM texture)
//   change   = max over the channels in Mode.x of |level(out) - level(in)|        (a NaN in a measured channel: no change, not in the region, not counted)
//   region   = Mode.y: 0 window | 1 occluded: min level < 254.5 | 2 lit: max level >= 7.5 | 3 shaded: min level < 247.5 | 4 reflecting: max level >= 0.5
//   visible  = change > Meas.y (8)
// The tap hash (MeterRules.h TapHash) is repeated bit for bit in unsigned 32-bit arithmetic.
//
// This file is the meter's own: it includes nothing and shares no shader source with the stages. The register slots and the constant-buffer layout are
// mirrored by MeterPass.cpp; change them together.

cbuffer Params : register(b0)
{
    uint4 Box;   // x, y, width, height of the measured box, in texels of both textures
    uint4 Geom;  // x = block (texels on a side), y = taps on a side, z = stratum size (block / taps on a side), w = the sample's seed
    uint4 Mode;  // x = channel mask (1 r, 2 g, 4 b, 8 a), y = region kind, z = unit (0 display, 1 unorm), w = 0
    float4 Meas; // x = the frame's white (display unit only), y = the visible threshold in levels, zw = 0
};

Texture2D<float4> In : register(t0);  // the stage's input (read only)
Texture2D<float4> Out : register(t1); // the stage's output (read only)

float4 VS(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 Level4(float4 v)
{
    const float4 scaled = (Mode.z == 0u) ? v / Meas.x : v;
    return saturate(scaled) * 255.0;
}

float4 PS(float4 position : SV_Position) : SV_Target
{
    const uint2 bin = uint2(position.xy);
    const float4 mask = float4(Mode.x & 1u, (Mode.x >> 1) & 1u, (Mode.x >> 2) & 1u, (Mode.x >> 3) & 1u);
    const uint taps = Geom.y * Geom.y;
    float region = 0.0, visible = 0.0, sum = 0.0, inside = 0.0;
    [loop] for (uint k = 0; k < taps; ++k)
    {
        const uint cx = k % Geom.y;
        const uint cy = k / Geom.y;
        uint h = (bin.x * 73856093u) ^ (bin.y * 19349663u) ^ (k * 83492791u) ^ (Geom.w * 2654435761u);
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        uint ox = cx * Geom.z;
        uint oy = cy * Geom.z;
        if (Geom.z > 1u)
        {
            ox += (h & 0xFFFFu) % Geom.z;
            oy += (h >> 16) % Geom.z;
        }
        const uint2 rel = bin * Geom.x + uint2(ox, oy);
        if (rel.x >= Box.z || rel.y >= Box.w)
            continue;
        const int3 p = int3(int2(Box.xy + rel), 0);
        const float4 a = In.Load(p);
        const float4 b = Out.Load(p);
        inside += 1.0;
        if (any((mask > 0.5) && (isnan(a) || isnan(b))))
            continue;
        const float4 la = Level4(a);
        const float4 lb = Level4(b);
        const float4 delta = (mask > 0.5) ? abs(lb - la) : float4(0.0, 0.0, 0.0, 0.0);
        const float change = max(max(delta.x, delta.y), max(delta.z, delta.w));
        const float4 lo = (mask > 0.5) ? min(la, lb) : float4(1.0e9, 1.0e9, 1.0e9, 1.0e9);
        const float4 hi = (mask > 0.5) ? max(la, lb) : float4(-1.0e9, -1.0e9, -1.0e9, -1.0e9);
        const float minLevel = min(min(lo.x, lo.y), min(lo.z, lo.w));
        const float maxLevel = max(max(hi.x, hi.y), max(hi.z, hi.w));
        bool member = true;
        if (Mode.y == 1u)
            member = minLevel < 254.5;
        else if (Mode.y == 2u)
            member = maxLevel >= 7.5;
        else if (Mode.y == 3u)
            member = minLevel < 247.5;
        else if (Mode.y == 4u)
            member = maxLevel >= 0.5;
        if (!member)
            continue;
        region += 1.0;
        sum += change;
        if (change > Meas.y)
            visible += 1.0;
    }
    return float4(region, visible, sum, inside);
}
