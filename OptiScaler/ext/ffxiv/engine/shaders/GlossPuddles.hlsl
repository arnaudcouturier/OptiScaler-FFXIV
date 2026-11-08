// Puddles before the engine's light passes: the engine's own G-buffer target, copied into a texture of this module's and shaped into puddles
// on the up-facing ground.
//
// Two entry points, one vertex shader, no sampler (every read is a Load on an exact texel, so the result does not depend on filter state and a
// host test can predict it):
//   VS  (vs_5_0) one oversized triangle from SV_VertexID
//   PS  (ps_5_0) the target's texels -> the engine's own texel, with the gloss channel replaced where a puddle is
//
// Inputs:
//   t0 Depth    Texture2D<float>   the reader's g_SamplerDepth (reversed-Z: 1 at the near plane, 0 at the far plane)
//   t1 Normal   Texture2D<float4>  the reader's g_SamplerGBuffer: the primary world-space normal as n * 0.5 + 0.5 in rgb
//   t2 Target   Texture2D<float4>  the engine's G-buffer target this module shapes (the one the target key names)
//   t3 Camera   Buffer<float4>     g_CameraParameter: the inverse view's three rows at register 3, the inverse projection's four at 14
//   t4 Wetness  Buffer<float4>     the engine's g_WetnessParameter: .x is the wetness amount (1 rain .. 0 dry)
//   b0 Params   five float4s, the layout of GlossPuddlesRules.h's Constants
//
// Rules:
//  1. One texel of the target stands for the depth and the normal texel of the same index: the engine writes all three at the display
//     allocation's size, and this pass runs over the part of it that lies inside the draw's render window (`Region`). Every texel outside that
//     region keeps the engine's own bytes, and the target arrives holding them: the caller copies the engine's texture before this draw.
//  2. A texel is shaped only when it has a surface (a depth above the far plane), a decoded world-space normal of unit length that faces up
//     (n.y above the cosine in Control.y: FFXIV is Y-up), and an engine wetness above zero. Every other texel returns the engine's own value,
//     which is what makes a dry frame a pure copy.
//  3. The puddle field is two octaves of a value noise anchored to the world position of the texel, reconstructed from the depth and the
//     camera's inverse projection and inverse view: the same world point gets the same value in every frame and from every camera. The field
//     is thresholded at `1 - coverage * wetness`, so the puddles fill up as the engine's own wetness rises and dry out as it falls - this pass
//     has no timer and no history of its own.
//  4. The channel key selects the one channel that is replaced. The other three are the engine's own bits, untouched, so a channel whose
//     meaning has not been measured cannot be corrupted by this pass.
//  5. The formula is a fixed function of the frame's buffers: no frame number, no random offset, no history. The host test compares this
//     shader's output with a CPU reference of the same code (GlossPuddlesRules.h), which is what makes the two implementations agree.

Texture2D<float> Depth : register(t0);
Texture2D<float4> Normal : register(t1);
Texture2D<float4> Target : register(t2);
Buffer<float4> Camera : register(t3);
Buffer<float4> Wetness : register(t4);

cbuffer Params : register(b0)
{
    float4 Window;  // xy = the render window's origin in target texels, zw = its size in target texels
    float4 Sizes;   // xy = the target's size in texels, zw = unused
    float4 Tuning;  // x = coverage, y = metres per noise cell, z = the edge's half-width, w = the wet value
    float4 Control; // x = the channel index (0..3), y = the up-facing cosine, z = the wetness floor, w = unused
    float4 Region;  // xy = the target texel this pass writes from, zw = how many texels of it in each direction
};

// g_CameraParameter's register numbers come from the one header that states the layout (ext/ffxiv/relight/shaders/CameraRegisters.h, the
// reflected layout of the engine's own shaders), so this pass and the contact shadow pass cannot drift apart. The build passes that folder
// with -I (gloss-puddles.shaders.props).
#include "CameraRegisters.h"

// The noise's shape. It is a constant of the algorithm, not a key: the same lattice in GlossPuddlesRules.h, so the CPU reference and the
// shader agree.
static const uint kNoisePeriod = 64;
static const float kFirstOctave = 0.65;
static const float kSecondOctave = 0.35;
static const float kNormalLengthLow = 0.5; // a decoded normal shorter than this (squared) is not a unit vector: not a surface
static const float kNormalLengthHigh = 1.5;

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

// The lattice hash: two multiplies, two shifts and a third multiply of 32-bit integers, then the top 24 bits as a number in 0..1. 24 bits is
// exactly the mantissa of a float, so this converts without a rounding argument. The order is the host reference's, bit for bit.
uint HashOf(int x, int y)
{
    uint h = (uint(x) * 0x9E3779B1u) ^ (uint(y) * 0x85EBCA77u);
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return h >> 8;
}

float LatticeValue(int x, int y)
{
    return float(HashOf(x, y)) * (1.0 / 16777216.0);
}

// One octave of the value noise at a lattice coordinate `x`,`y` in cells: the four corners around it, interpolated with the smoothstep weights.
// `period` (a power of two) is the octave's own lattice period, so the higher octave wraps twice as fast and the sum is periodic.
float ValueNoise(float x, float y, uint period)
{
    const int mask = int(period) - 1;
    const float fx = x - floor(x);
    const float fy = y - floor(y);
    const int ix = int(floor(x));
    const int iy = int(floor(y));
    const float wx = fx * fx * (3.0 - 2.0 * fx);
    const float wy = fy * fy * (3.0 - 2.0 * fy);
    const float v00 = LatticeValue(ix & mask, iy & mask);
    const float v10 = LatticeValue((ix + 1) & mask, iy & mask);
    const float v01 = LatticeValue(ix & mask, (iy + 1) & mask);
    const float v11 = LatticeValue((ix + 1) & mask, (iy + 1) & mask);
    const float low = lerp(v00, v10, wx);
    const float high = lerp(v01, v11, wx);
    return lerp(low, high, wy);
}

// The field at a world position, in metres: one cell of `metersPerCell` for the first octave, half of it for the second. The lattice is folded
// into the period first, which is exact for a power-of-two period and keeps the smoothstep weights precise at a large zone coordinate (a few
// thousand metres of a playable area in a 32-bit float).
float Noise(float worldX, float worldZ, float metersPerCell)
{
    const float period = float(kNoisePeriod);
    const float cell = max(metersPerCell, 0.05);
    const float px = worldX / cell;
    const float pz = worldZ / cell;
    const float fx = px - period * floor(px / period);
    const float fz = pz - period * floor(pz / period);
    return kFirstOctave * ValueNoise(fx, fz, kNoisePeriod) + kSecondOctave * ValueNoise(fx * 2.0, fz * 2.0, kNoisePeriod * 2u);
}

// The view-space position of a target texel: the inverse projection takes the normalized device coordinate and the reversed-Z depth back to the
// point it came from. The device coordinate spans the draw's render window, not the target's allocation (the engine renders a smaller window
// inside an allocation at the display size), so the ndc is computed against the window.
float3 ViewPositionOf(float2 centre, float depth)
{
    const float2 ndc = float2((centre.x - Window.x) / Window.z * 2.0 - 1.0, 1.0 - (centre.y - Window.y) / Window.w * 2.0);
    const float4x4 inverseProjection = float4x4(Camera[CS_CAMERA_INVERSE_PROJECTION_REGISTER + 0],
                                                Camera[CS_CAMERA_INVERSE_PROJECTION_REGISTER + 1],
                                                Camera[CS_CAMERA_INVERSE_PROJECTION_REGISTER + 2],
                                                Camera[CS_CAMERA_INVERSE_PROJECTION_REGISTER + 3]);
    const float4 clip = mul(inverseProjection, float4(ndc, depth, 1.0));
    return clip.w != 0.0 ? clip.xyz / clip.w : float3(0.0, 0.0, 0.0);
}

// The world position of a view-space point: the inverse view's three stored rows, each dotted with the point as a column vector.
float3 WorldPositionOf(float3 view)
{
    const float4 view4 = float4(view, 1.0);
    return float3(dot(Camera[CS_CAMERA_INVERSE_VIEW_REGISTER + 0], view4), dot(Camera[CS_CAMERA_INVERSE_VIEW_REGISTER + 1], view4),
                  dot(Camera[CS_CAMERA_INVERSE_VIEW_REGISTER + 2], view4));
}

float4 PS(float4 position : SV_Position) : SV_Target0
{
    const int2 at = int2(position.xy);
    const float4 engine = Target.Load(int3(at, 0));

    // The region this pass writes. The viewport keeps the draw inside it, so this is the belt to that braces: a texel outside is the engine's.
    const int2 regionLow = int2(Region.x, Region.y);
    const int2 regionHigh = int2(Region.x + Region.z, Region.y + Region.w);
    if (at.x < regionLow.x || at.y < regionLow.y || at.x >= regionHigh.x || at.y >= regionHigh.y)
        return engine;

    // Dry: the engine's own texel to the bit (rule 2).
    const float wetness = Wetness[0].x;
    if (!(wetness > Control.z))
        return engine;

    const float depth = Depth.Load(int3(at, 0));
    if (!(depth > 0.0))
        return engine; // the far plane, a cleared depth or a NaN: nothing there can hold water

    const float3 normal = Normal.Load(int3(at, 0)).xyz * 2.0 - 1.0;
    const float lengthSquared = dot(normal, normal);
    if (!(lengthSquared > kNormalLengthLow) || !(lengthSquared < kNormalLengthHigh))
        return engine; // not a surface: a cleared or half-written G-buffer texel
    if (!(normal.y > Control.y))
        return engine; // not up-facing: a wall, a roof, a character

    const float3 view = ViewPositionOf(float2(at) + 0.5, depth);
    if (any(isnan(view)))
        return engine;
    const float3 worldPosition = WorldPositionOf(view);
    if (any(isnan(worldPosition)))
        return engine;

    // The puddle field and the threshold the engine's own wetness sets (rule 3): full rain with a coverage of 0.5 puts half the up-facing ground
    // above the level, and a wetness of zero puts all of it below.
    const float field = Noise(worldPosition.x, worldPosition.z, Tuning.y);
    const float level = 1.0 - saturate(Tuning.x) * saturate(wetness);
    const float width = max(Tuning.z, 0.0005);
    const float puddle = smoothstep(level - width, level + width, field);
    const float amount = saturate(wetness) * saturate(puddle);

    // One channel, chosen by the key; the other three start from the engine's own values (rule 4). The if-chain is the dynamic index written out:
    // a float4 indexed by a value read from a constant buffer is a select on some drivers, and this is exact everywhere.
    float4 result = engine;
    const int channel = clamp(int(Control.x), 0, 3);
    if (channel == 0)
        result.x = engine.x + (Tuning.w - engine.x) * amount;
    else if (channel == 1)
        result.y = engine.y + (Tuning.w - engine.y) * amount;
    else if (channel == 2)
        result.z = engine.z + (Tuning.w - engine.z) * amount;
    else
        result.w = engine.w + (Tuning.w - engine.w) * amount;
    return result;
}
