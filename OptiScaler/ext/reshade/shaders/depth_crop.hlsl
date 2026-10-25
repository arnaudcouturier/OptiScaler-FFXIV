// Automatic ReShade scene depth (ReShadeDepthCopy.cpp): the game's depth, cropped by the subrect origin and mapped nearest to the
// size of the effect runtime's back buffer, into an R32_FLOAT target. One source, two entry points:
//   VS  full-screen triangle from the vertex id (vs_5_0)
//   PS  the crop and remap (ps_5_0)
// Resource slots and the constant-buffer layout are fixed; ReShadeDepthCopy.h mirrors them.

Texture2D<float> Depth : register(t0); // typed view of the game's depth (D32 / D24 / D16 families), read as a normalised value

cbuffer Crop : register(b0)
{
    uint4 Packed; // x: origin, y: render size, z: output size (each: x in the low 16 bits, y in the high 16 bits), w: unused
};

uint2 Unpack(uint v)
{
    return uint2(v & 0xFFFFu, v >> 16);
}

float4 VS(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// Exact integer maths. SV_Position is the pixel centre (i + 0.5), so truncating it gives the pixel index i: nothing is added to
// it, and no float floor is used (it can land on the wrong side where (i + 0.5) * render / output is exactly an integer, e.g. at
// 2:3). The source texel is the one whose span holds the pixel centre mapped to the render size, kept inside the render region.
// The largest intermediate, (2 * 0x3FFF + 1) * 0x4000, fits in 32 bits.
float PS(float4 position : SV_Position) : SV_Target
{
    const uint2 origin = Unpack(Packed.x);
    const uint2 render = Unpack(Packed.y);
    const uint2 output = Unpack(Packed.z);
    const uint2 i = uint2(position.xy);
    const uint2 src = origin + min((i * 2u + 1u) * render / (output * 2u), render - 1u);
    return Depth.Load(int3(int2(src), 0));
}
