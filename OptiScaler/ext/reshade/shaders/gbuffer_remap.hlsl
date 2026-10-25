// Automatic ReShade G-buffers (ReShadeGBufferCopy.cpp): the render window of an add-on's output-sized G-buffer (normals, decals,
// motion vectors, material), stretched by exact-integer nearest sampling to the whole output, into a target of the same format. One
// source, two entry points:
//   VS  full-screen triangle from the vertex id (vs_5_0)
//   PS  the remap (ps_5_0)
// Resource slots and the constant-buffer layout are fixed; ReShadeGBufferCopy.h mirrors them.

Texture2D<float4> Src : register(t0); // the add-on's own view, any float / UNORM / SNORM format, read as four floats

cbuffer Remap : register(b0)
{
    uint2 Packed; // x: render size, y: output size (each: x in the low 16 bits, y in the high 16 bits)
    float2 Mul;   // multiplier of the first two channels: exactly 1.0 for everything but scaled motion vectors
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

// The same exact integer maths as depth_crop.hlsl with origin 0,0: SV_Position is the pixel centre (i + 0.5), so truncating it gives
// the pixel index i, nothing is added to it and no float floor is used (it can land on the wrong side where (i + 0.5) * render /
// output is exactly an integer, e.g. at 2:3). The source texel is the one whose span holds the pixel centre mapped to the render
// size, kept inside the render window. The largest intermediate, (2 * 0x3FFF + 1) * 0x4000, fits in 32 bits.
float4 PS(float4 position : SV_Position) : SV_Target
{
    const uint2 render = Unpack(Packed.x);
    const uint2 output = Unpack(Packed.y);
    const uint2 i = uint2(position.xy);
    const uint2 src = min((i * 2u + 1u) * render / (output * 2u), render - 1u);
    float4 v = Src.Load(int3(int2(src), 0));
    v.xy *= Mul.xy; // a multiplier of exactly 1.0 changes no value
    return v;
}
