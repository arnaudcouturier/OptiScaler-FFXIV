// Helper passes of the isolated character-preview DLSS path (CharaGuides.cpp). One source, four entry points:
//   VS      full-screen triangle from the vertex id (vs_5_0)
//   Copy    native BGRA image -> RGBA8 colour input
//   Guides  depth copy + signed motion with the engine's camera offset removed (two render targets)
//   Alpha   upscaled colour + native alpha, shifted by the render jitter
// Resource slots and constant-buffer layouts are fixed; CharaGuides.cpp binds them by these registers.
//
// Written from a description of the behaviour of Nickle's depth-jitter-20261013 build (docs/RECONSTRUCTION-NICKLE.md).

Texture2D<float4> Source : register(t0);   // Copy: native image; Alpha: the upscaler output
Texture2D<float4> Motion : register(t1);   // Guides: signed motion target (RGBA16F, xy used)
Texture2D<float> Depth : register(t2);     // Guides: the preview's depth, read as a normalised value
Texture2D<float4> Original : register(t3); // Alpha: the native image (its alpha is kept)
SamplerState Linear : register(s0);        // bilinear, clamp

cbuffer Settings : register(b0)
{
    float2 Jitter;  // render jitter in pixels
    float2 InvSize; // 1 / render width, 1 / render height
};

cbuffer NativeCamera : register(b1)
{
    float4 Camera[59]; // the game's own per-pass camera block; only element 58 is read
};

struct VsOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut VS(uint id : SV_VertexID)
{
    VsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 Copy(VsOut i) : SV_Target
{
    return Source.SampleLevel(Linear, i.uv, 0);
}

struct GuidesOut
{
    float depth : SV_Target0;  // R32_FLOAT
    float2 motion : SV_Target1; // R16G16_FLOAT
};

bool IsFinite(float x)
{
    return (asuint(x) & 0x7fffffff) < 0x7f800000;
}

GuidesOut Guides(VsOut i)
{
    const int3 pixel = int3(int2(i.position.xy), 0);

    GuidesOut o;
    o.depth = Depth.Load(pixel);

    float2 motion = Motion.Load(pixel).xy - Camera[58].zw;
    if (!IsFinite(motion.x) || !IsFinite(motion.y))
        motion = float2(0.0, 0.0);
    o.motion = clamp(motion, float2(-2.0, -2.0), float2(2.0, 2.0));
    return o;
}

float4 Alpha(VsOut i) : SV_Target
{
    const float3 colour = Source.SampleLevel(Linear, i.uv, 0).rgb;
    const float alpha = Original.SampleLevel(Linear, i.uv + Jitter * InvSize, 0).a;
    return float4(colour, alpha);
}
