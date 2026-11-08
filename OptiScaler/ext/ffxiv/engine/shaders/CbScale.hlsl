// CbScale.hlsl: the float4 scale and the component set the engine pass overrides apply to one of the game's constant buffers.
//
// The dispatch reads the game's buffer as its own constant buffer (b0: the whole buffer, up to 16,384 bytes, which is under the 64 KB
// constant-buffer limit) and copies every float4 into the output byte-address buffer. Two tables address the float4s a knob changes:
//
//   the scale table carries one multiplier per component, and a component whose multiplier is exactly 1 keeps the bits it was read with;
//   the set table carries one value and one component mask per entry, and a component whose mask bit is unset keeps the bits it was read with.
//
// So a denormal, an integer-as-float or a NaN payload beside a knob never passes through a float conversion: the game's buffer arrives here as
// raw uint4 bits, only a component a knob really scales is converted, and a set value is written as the bits the caller computed.
//
// The layout is mirrored by CbPatch.cpp (ParamsBuffer) and by EnginePassRules.h (ScaleEntry, SetEntry); change them together.
cbuffer Source : register(b0)
{
    uint4 g_source[1024]; // 1,024 float4s = 16,384 bytes, as raw bits
};

cbuffer Params : register(b1)
{
    uint4 g_info;           // x = the scale entry count, y = the byte count to write (a multiple of 16), z = the set entry count, w = 0
    float4 g_multiplier[8]; // one multiplier per scale entry, applied component by component where it is not exactly 1
    uint4 g_address[8];     // one address per scale entry: x = the first byte, y = the stride, z = the count, w = 0
    uint4 g_setvalue[8];    // one value per set entry: the bits a masked component is replaced with
    uint4 g_setmask[8];     // one mask per set entry: x = bit 0..3 replace .x .. .w, an unset bit keeps the game's bits; yzw = 0
    uint4 g_setaddress[8];  // one address per set entry: x = the first byte, y = the stride, z = the count, w = 0
};

RWByteAddressBuffer g_out : register(u0);

// A multiplier of exactly 1 leaves the bits alone: the conversion and the multiply only happen for a component a knob changes, so nothing
// beside it can be altered by a denormal flush or a NaN canonicalisation on the way through.
void ScaleComponent(inout uint value, float multiplier)
{
    if (multiplier != 1.0f)
        value = asuint(asfloat(value) * multiplier);
}

// Whether the entry `address` (x = first byte, y = stride, z = count) addresses the float4 at byte `at`.
bool Addresses(uint at, uint4 address)
{
    if (address.y == 0u || at < address.x)
        return false;
    const uint delta = at - address.x;
    return delta % address.y == 0u && (delta / address.y) < address.z;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint words = g_info.y / 16u;
    const uint entries = min(g_info.x, 8u);
    const uint sets = min(g_info.z, 8u);
    if (id.x >= words)
        return; // one thread per float4: the dispatch launches exactly the groups the byte count needs, so no group redoes another's work
    const uint at = id.x * 16u;
    uint4 value = g_source[id.x];

    // The scale first, then the set: a knob's set value lands on the scaled result, and neither loop touches a component it does not address.
    for (uint e = 0u; e < entries; ++e)
    {
        if (!Addresses(at, g_address[e]))
            continue;
        ScaleComponent(value.x, g_multiplier[e].x);
        ScaleComponent(value.y, g_multiplier[e].y);
        ScaleComponent(value.z, g_multiplier[e].z);
        ScaleComponent(value.w, g_multiplier[e].w);
    }
    for (uint s = 0u; s < sets; ++s)
    {
        if (!Addresses(at, g_setaddress[s]))
            continue;
        const uint mask = g_setmask[s].x;
        if ((mask & 1u) != 0u)
            value.x = g_setvalue[s].x;
        if ((mask & 2u) != 0u)
            value.y = g_setvalue[s].y;
        if ((mask & 4u) != 0u)
            value.z = g_setvalue[s].z;
        if ((mask & 8u) != 0u)
            value.w = g_setvalue[s].w;
    }
    g_out.Store4(at, value);
}
