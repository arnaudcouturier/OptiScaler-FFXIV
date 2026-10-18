// Spatial anti-aliasing of the equipment preview image (character preview feature).
//
// A luma-based, alpha-preserving edge filter in the style of FXAA 3.11, written for one job: smooth the staircase edges
// of a small off-screen image before the UI shows it, optionally while enlarging it 2x.
//
// Entry points: VS (vs_5_0), PS and PSTint (ps_5_0; PSTint is PS plus a debug tint). One input, one sampler, no constant buffer:
//   t0 Source      Texture2D<float4>, the preview image
//   s0 LinearClamp bilinear, clamp
// The texel size comes from the SOURCE dimensions, also when the render target is larger than the source.
//
// Rules:
//  1. Alpha: if the smallest alpha of the centre and its four diagonal neighbours is below 0.98 the centre sample is
//     returned unchanged. The filter never blends across cut-outs and never changes alpha.
//  2. Flat areas: if the luma range of the five samples is below max(0.0312, 0.125 * maximum) the centre sample is returned.
//  3. Otherwise the edge direction is estimated from the four diagonal lumas, normalised and clamped to +-8 texels, and the
//     image is sampled along it (two taps at +-1/6 for candidate A, two more at +-1/2 mixed with A for candidate B). B is
//     used unless its luma leaves the neighbourhood's range, then A. Alpha stays the centre sample's alpha.

Texture2D<float4> Source : register(t0);
SamplerState LinearClamp : register(s0);

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// One oversized triangle generated from the vertex id: the visible part covers the viewport, uv runs 0..1 with the origin
// at the top left.
VSOutput VS(uint id : SV_VertexID)
{
    VSOutput o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

static const float kAlphaOpaque = 0.98;
static const float kContrastAbsolute = 0.0312;
static const float kContrastRelative = 0.125;
static const float kReduceMultiplier = 0.03125;  // 0.25 / 8
static const float kReduceMinimum = 0.0078125;   // 1 / 128
static const float kSpanMax = 8.0;

float Luma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

float4 Filter(VSOutput i)
{
    float width, height;
    Source.GetDimensions(width, height);
    const float2 texel = 1.0 / float2(width, height);

    const float4 centre = Source.SampleLevel(LinearClamp, i.uv, 0);
    const float4 nw = Source.SampleLevel(LinearClamp, i.uv + float2(-1.0, -1.0) * texel, 0);
    const float4 ne = Source.SampleLevel(LinearClamp, i.uv + float2(1.0, -1.0) * texel, 0);
    const float4 sw = Source.SampleLevel(LinearClamp, i.uv + float2(-1.0, 1.0) * texel, 0);
    const float4 se = Source.SampleLevel(LinearClamp, i.uv + float2(1.0, 1.0) * texel, 0);

    if (min(centre.a, min(min(nw.a, ne.a), min(sw.a, se.a))) < kAlphaOpaque)
        return centre;

    const float lumaM = Luma(centre.rgb);
    const float lumaNW = Luma(nw.rgb);
    const float lumaNE = Luma(ne.rgb);
    const float lumaSW = Luma(sw.rgb);
    const float lumaSE = Luma(se.rgb);

    const float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    const float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    if (lumaMax - lumaMin < max(kContrastAbsolute, lumaMax * kContrastRelative))
        return centre;

    float2 direction;
    direction.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    direction.y = (lumaNW + lumaSW) - (lumaNE + lumaSE);

    const float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * kReduceMultiplier, kReduceMinimum);
    const float scale = 1.0 / (min(abs(direction.x), abs(direction.y)) + reduce);
    direction = clamp(direction * scale, -kSpanMax, kSpanMax) * texel;

    const float3 rgbA = 0.5 * (Source.SampleLevel(LinearClamp, i.uv + direction * (1.0 / 3.0 - 0.5), 0).rgb +
                               Source.SampleLevel(LinearClamp, i.uv + direction * (2.0 / 3.0 - 0.5), 0).rgb);
    const float3 rgbB = rgbA * 0.5 + 0.25 * (Source.SampleLevel(LinearClamp, i.uv + direction * -0.5, 0).rgb +
                                             Source.SampleLevel(LinearClamp, i.uv + direction * 0.5, 0).rgb);

    const float lumaB = Luma(rgbB);
    const float3 rgb = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
    return float4(rgb, centre.a);
}

float4 PS(VSOutput i) : SV_Target
{
    return Filter(i);
}

// Debug aid (config key PreviewDebugTint): the same filter, with a mild magenta mix so the processed image can be told from the
// native one on screen. Alpha is untouched.
float4 PSTint(VSOutput i) : SV_Target
{
    const float4 c = Filter(i);
    return float4(lerp(c.rgb, float3(1.0, 0.0, 1.0), 0.35), c.a);
}
