// Puddles at the tone map: the pool's lit colour darkened, and a marched mirror of the scene added to it, before the engine's tone map reads the
// scene.
//
// Two entry points, one vertex shader, no sampler (every read is a Load on an exact texel, so the result does not depend on filter state and a
// host test can predict it):
//   VS         (vs_5_0) one oversized triangle from SV_VertexID
//   PS_Puddle  (ps_5_0) the scene's texels -> the same texels with a pool's colour, and the per-texel statistics of a capture
//
// Inputs:
//   t0 Scene    Texture2D<float4>  the tone map's own t0: the scene the frame's composite wrote, sqrt-encoded and not clamped
//   t1 VPosition Texture2D<float4> the frame's g_SamplerViewPosition: the view-space point of every pixel (the camera is the origin)
//   t2 Normal   Texture2D<float4>  the frame's g_SamplerGBuffer: the primary world-space normal as n * 0.5 + 0.5 (zero in the sky)
//   t3 Camera   Buffer<float4>     g_CameraParameter: the view matrix's three rows at register 0 and the inverse view's at 3
//   t4 Wetness  Buffer<float4>     this module's 32-byte wetness amount: .x is what the gloss pass gated on, so both stages read one value
//   t5 Gate     Texture2D<float4>  the engine's material pack (G-buffer 1) of THIS frame, as the frame's composite bound it: its alpha is the engine's own
//                                  gloss, which PuddleGate.hlsli turns into the exposure that scales the pool. Null when the frame has none: it reads 0 and
//                                  no pool is drawn
//   b0 Params   five float4s, the layout of GlossPuddlesRules.h's ToneConstants
//
// Rules:
//  1. One texel of the scene stands for the view position and the normal of the same index: the engine writes all three at the display
//     allocation's size, and this pass runs over the part of it that lies inside the tone map's render window (`Region`). Every texel outside
//     that region keeps the engine's own bytes, and the target arrives holding them: the caller copies the scene before this draw.
//  2. A texel is shaded only when it has a surface (a view position that is not the cleared sky), a decoded world-space normal of unit length
//     that faces up (n.y above the cosine in Control.w: FFXIV is Y-up) and an engine wetness above zero. Every other texel returns the scene's
//     own texel, which is what makes a dry frame a pure copy and keeps a wall, a roof and a character exactly as the engine drew them.
//  3. The pool is the gloss pass's own field: two octaves of a value noise on a lattice anchored to the world, thresholded at
//     `1 - coverage * wetness` with the key's softness as the edge's half-width. Both stages therefore gate on the same shape and the same
//     wetness buffer, and neither has a timer or any history.
//  4. The pool's colour is the scene's own times `1 - mask * darkening`, and the mirror is added on top: the march of SceneMarch.hlsl from the
//     view-position buffer, whose direction is the flat floor's normal (the world's up) tilted by a static gradient of the same noise field, and
//     whose colour is added with water's Fresnel and the key's strength. Where the march misses, nothing is added - the darkened scene already
//     carries the engine's own probe reflection of that ground, so a miss is never black and never a hole.
//  4b. When the caller passes a clock (Sizes.w), the pool's normal also tilts by the splash rings of drops landing on it: a two-layer lattice of
//     rings anchored to the world, each cell's drop starting at its own phase. The rings are the only thing in this pass that moves with real
//     time, and they are a function of that clock and the world position alone - no history, no per-frame random. Sizes.w = 0 keeps a still
//     pool, which is what a deterministic host test and every reference frame are built with.
//  4c. The pool is scaled by the gloss stage's own exposure (PuddleGate.hlsli, the one copy both shaders include): a texel the engine renders dry
//     (its material pack's alpha is far below what wet ground shows at this wetness) gets no darkening, no mirror and no ring, and the edge between
//     dry and wet ground is soft. The gate is read before the march, so a dry texel costs none of it.
//  5. The statistics (SV_Target1) are two flags per texel: whether it is pool, and whether the march found a surface there. The caller binds that
//     target only while the draw diagnostics capture a frame, so a session pays nothing for them.
//  6. Everything but the raindrops' phase is a fixed function of the frame's buffers: no frame number, no random offset, no history. A still scene
//     gives the same picture every frame, and a moving one cannot ghost - which matters because the upscaler and the frame generator take this as
//     scene colour. The splash rings keep their world anchor and their phase per cell, so a camera that turns sees the same drops in the same
//     place, and only the clock moves them.

Texture2D<float4> Scene : register(t0);
Texture2D<float4> VPosition : register(t1);
Texture2D<float4> Normal : register(t2);
Buffer<float4> Camera : register(t3);
Buffer<float4> Wetness : register(t4);
Texture2D<float4> Gate : register(t5);

cbuffer Params : register(b0)
{
    float4 Window;  // xy = the render window's origin in scene texels, zw = its size in scene texels
    float4 Sizes;   // xy = the scene allocation's size in texels, z = the march's steps, w = the raindrops' clock in seconds (0 = a still pool)
    float4 Tuning;  // x = coverage, y = metres per noise cell, z = the edge's half-width, w = the darkening
    float4 Control; // x = the reflection's strength (0 = no mirror), y = the ripple, z = the wetness floor, w = the up-facing cosine
    float4 Region;  // xy = the scene texel this pass writes from, zw = how many texels of it in each direction
    float4 Rain;    // x = how strongly it is raining on the pools now, 0..1: the splash rings' amplitude, faded in and out by the caller
    float4 Reach;   // x = the pool's reach in metres (0 = no limit): past it the stage keeps the engine's texel, yzw unused
};

// g_CameraParameter's register numbers come from the one header that states the layout (ext/ffxiv/relight/shaders/CameraRegisters.h), so this
// pass, the contact shadow pass and the screen-space reflections cannot state different layouts. The build passes that folder with -I
// (gloss-puddles.shaders.props).
#include "CameraRegisters.h"

// The march is the water reflections' own (SceneMarch.hlsl): one implementation for both, so the two reflections cannot drift apart.
#include "SceneMarch.hlsl"

// The exposure gate, the one copy the gloss stage includes too.
#include "PuddleGate.hlsli"

// The noise's shape. It is a constant of the algorithm, not a key: the same lattice in GlossPuddlesRules.h, so the CPU reference and the shader
// agree. #include guards would be nicer than a copy, but the gloss pass's shader is a separate compilation with its own entry points and the
// lattice is eleven lines: the two copies are pinned against each other by the noise test of GlossPuddlesRules.h.
static const uint kNoisePeriod = 64;
static const float kFirstOctave = 0.65;
static const float kSecondOctave = 0.35;
static const float kNormalLengthLow = 0.5; // a decoded normal shorter than this (squared) is not a unit vector: not a surface
static const float kNormalLengthHigh = 1.5;
static const float kMinCell = 0.05;

// The ripple: a fixed cell of about a metre and a fixed slope, so the wobble is the same size on a pool of any scale key. The step is the
// smaller distance the slope is read over; both are in GlossPuddlesRules.h for the host reference.
static const float kRippleCell = 1.0;
static const float kRippleStep = 0.05;
static const float kRippleSlope = 0.2;
static const float kWaterF0 = 0.02;

// The raindrops: a cell of kRainCell metres per drop (small enough that a pool a few metres wide holds several rings, large enough that the
// shader's two layers do not read as a pattern), a ring that expands to kRainRingRadius of a cell over one phase, and a wave whose period is a
// hand's width on the water. kRainCentreSpread is the fraction of the cell a drop's centre may be offset by, so the rings do not sit on a
// lattice; kRainRate is how many drops land in one cell per second.
//
// The three lengths an image reads at distance are the ring's radius (0.23 m), the wave's period (2*pi/kRainWaveFrequency of a cell, 13 cm) and
// the band the crest lives in (1/kRainRingBand of a cell, 7 cm). Much finer than that - a 6 cm period in a 3 cm band - is under two pixels per
// crest at the ten to thirty metres a pool is usually seen from: the rings alias into shimmer and read as noise on the water rather than as a
// drop. kRainCentreFade lets a ring outlive its own cell (a third of a cell, 18 cm) so successive drops overlap as they do on real water.
static const float kRainCell = 0.55;
static const float kRainCentreSpread = 0.55;
static const float kRainRingRadius = 0.42;
static const float kRainWaveFrequency = 26.0;
static const float kRainRingBand = 8.0;
static const float kRainCentreFade = 4.0;
static const float kRainRate = 1.6;
static const float kRainSlopeGain = 1.8;
static const float kRainGlint = 0.12;

// The march's surface tolerance, in metres: a surface farther behind the crossing than this is another object, not the one the ray crossed. The
// puddle is a flat plane, so the tolerance only has to absorb the stone's own relief.
static const float kMarchThickness = 0.3;

struct VSOutput
{
    float4 position : SV_Position;
};

struct PSOutput
{
    float4 colour : SV_Target0;
    float4 stats : SV_Target1; // x = this texel is pool, y = the march found a surface there
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

float LatticeValue(int x, int y) { return float(HashOf(x, y)) * (1.0 / 16777216.0); }

// One octave of the value noise at a lattice coordinate `x`,`y` in cells: the four corners around it, interpolated with the smoothstep weights.
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
    return lerp(lerp(v00, v10, wx), lerp(v01, v11, wx), wy);
}

// The field at a world position, in metres: one cell of `metersPerCell` for the first octave, half of it for the second. The lattice is folded
// into the period first, which is exact for a power-of-two period and keeps the smoothstep weights precise at a large zone coordinate.
float Noise(float worldX, float worldZ, float metersPerCell)
{
    const float period = float(kNoisePeriod);
    const float cell = max(metersPerCell, kMinCell);
    const float px = worldX / cell;
    const float pz = worldZ / cell;
    const float fx = px - period * floor(px / period);
    const float fz = pz - period * floor(pz / period);
    return kFirstOctave * ValueNoise(fx, fz, kNoisePeriod) + kSecondOctave * ValueNoise(fx * 2.0, fz * 2.0, kNoisePeriod * 2u);
}

// The world position of a view-space point: the inverse view's three stored rows, each dotted with the point as a column vector.
float3 WorldPositionOf(float3 view)
{
    const float4 view4 = float4(view, 1.0);
    return float3(dot(Camera[CS_CAMERA_INVERSE_VIEW_REGISTER + 0], view4), dot(Camera[CS_CAMERA_INVERSE_VIEW_REGISTER + 1], view4),
                  dot(Camera[CS_CAMERA_INVERSE_VIEW_REGISTER + 2], view4));
}

float4x4 LoadProjection()
{
    return float4x4(Camera[CS_CAMERA_PROJECTION_REGISTER + 0], Camera[CS_CAMERA_PROJECTION_REGISTER + 1],
                    Camera[CS_CAMERA_PROJECTION_REGISTER + 2], Camera[CS_CAMERA_PROJECTION_REGISTER + 3]);
}

// A world direction as view space sees it: the view matrix's three rows dotted with it (its second column, which is the world's up in view
// space, is the same expression with (0,1,0) as the vector).
float3 ToView(float3 world)
{
    return float3(dot(Camera[CS_CAMERA_VIEW_REGISTER + 0].xyz, world), dot(Camera[CS_CAMERA_VIEW_REGISTER + 1].xyz, world),
                  dot(Camera[CS_CAMERA_VIEW_REGISTER + 2].xyz, world));
}

// Three independent hashes of one lattice cell and one layer: the drop's centre (xy, in 0..1 of the cell) and its phase (z). Each is the same
// hash the noise uses with a different salt, so the drops are anchored to the world and a camera that moves does not re-roll them.
float3 RainHash(int2 cell, int layer)
{
    const uint a = HashOf(cell.x + layer * 131, cell.y + layer * 257);
    const uint b = HashOf(cell.y + layer * 613, cell.x + layer * 977);
    const uint c = HashOf(cell.x * 17 + cell.y + layer * 719, cell.y - cell.x + layer * 421);
    return float3(float(a), float(b), float(c)) * (1.0 / 16777216.0);
}

// The splash rings of rain landing on the pool, at a world position and a clock in seconds: the slope the rings add to the pool's normal (xy, in
// world axes) and the strongest crest there (0..1), which the caller adds as the water's own glint. Two layers of cells, the second offset half a
// cell so the rings overlap instead of tiling; within a cell one drop lands per kRainRate seconds, its ring expanding and fading with the phase.
void RainRipples(float2 world, float time, out float2 slope, out float crest)
{
    slope = float2(0.0, 0.0);
    crest = 0.0;
    [unroll]
    for (int layer = 0; layer < 2; ++layer)
    {
        const float2 p = world / kRainCell + float2(0.27, 0.31) * float(layer);
        const float2 cell = floor(p);
        const float2 local = frac(p) - 0.5;
        const float3 drop = RainHash(int2(cell), layer);
        const float2 centre = (drop.xy - 0.5) * kRainCentreSpread;
        const float phase = frac(time * kRainRate + drop.z);
        const float2 d = local - centre;
        const float r = length(d);
        const float waveRadius = phase * kRainRingRadius;
        // The ring's envelope: it fades from the drop's own centre outwards (the water is flat where the drop has not reached), loses strength as
        // it travels, and starts at zero at the centre so a texel on the drop's own position is never a spike.
        const float env = (1.0 - phase) * (1.0 - phase) * exp(-kRainCentreFade * r) * smoothstep(0.0, 0.04, r);
        const float w = (r - waveRadius) * kRainWaveFrequency;
        const float ring = sin(w) * exp(-kRainRingBand * abs(r - waveRadius)) * env;
        // The slope is the ring's own radial derivative, so the mirror tilts along the ring rather than across it.
        const float2 outward = d / max(r, 1.0e-4);
        slope += outward * (cos(w) * kRainSlopeGain * env);
        crest = max(crest, ring);
    }
}

// The mirror's normal in view space: the flat floor's (the world's up) unless the ripple key tilts it, by a slope read from a fixed-cell noise
// anchored to the world plus, when the caller passes a clock, the splash rings of the rain. The noise half is static on purpose: an animated
// perturbed direction is what a temporal upscaler turns into noise, and the rings are the one wobble that is meant to move. `crest` answers the
// strongest ring crest at this texel, for the glint.
float3 MirrorNormal(float3 world, out float crest)
{
    crest = 0.0;
    float3 worldNormal = float3(0.0, 1.0, 0.0);
    if (Control.y > 0.0)
    {
        const float centre = Noise(world.x, world.z, kRippleCell);
        float2 slope = float2(Noise(world.x + kRippleStep, world.z, kRippleCell) - centre,
                              Noise(world.x, world.z + kRippleStep, kRippleCell) - centre) / kRippleStep;
        if (Sizes.w > 0.0)
        {
            float2 rainSlope = float2(0.0, 0.0);
            RainRipples(world.xz, Sizes.w, rainSlope, crest);
            slope += rainSlope * Rain.x;
            crest *= Rain.x;
        }
        worldNormal = normalize(float3(-slope.x * kRippleSlope * Control.y, 1.0, -slope.y * kRippleSlope * Control.y));
    }
    return normalize(ToView(worldNormal));
}

PSOutput PS_Puddle(float4 position : SV_Position)
{
    PSOutput output;
    output.stats = float4(0.0, 0.0, 0.0, 0.0);
    const int2 at = int2(position.xy);
    const float4 scene = Scene.Load(int3(at, 0));
    output.colour = scene; // every early answer is the scene's own texel, to the bit

    // The region this pass writes. The viewport keeps the draw inside it, so this is the belt to that braces.
    const int2 regionLow = int2(Region.x, Region.y);
    const int2 regionHigh = int2(Region.x + Region.z, Region.y + Region.w);
    if (at.x < regionLow.x || at.y < regionLow.y || at.x >= regionHigh.x || at.y >= regionHigh.y)
        return output;

    // Dry: the scene's own texel to the bit (rule 2).
    const float wetness = Wetness[0].x;
    if (!(wetness > Control.z))
        return output;

    const float3 P = VPosition.Load(int3(at, 0)).xyz;
    if (!(dot(P, P) > 1e-6) || !(-P.z > 0.05 && -P.z < 500.0))
        return output; // the sky or the far plane: nothing there can hold standing water

    // The pool's reach: the G-buffer beneath a forward draw - the ocean, the lakes, the rivers - is the up-facing ground under the water, so past
    // the reach the stage keeps the engine's own texel instead of painting pools onto the water the engine drew over that ground. The reach key's
    // band and its default are GlossPuddlesRules.h's (WithinReach, the CPU reference of this gate).
    if (Reach.x > 0.0 && !(-P.z < Reach.x))
        return output;

    const float3 normal = Normal.Load(int3(at, 0)).xyz * 2.0 - 1.0;
    const float lengthSquared = dot(normal, normal);
    if (!(lengthSquared > kNormalLengthLow) || !(lengthSquared < kNormalLengthHigh))
        return output; // not a surface: a cleared or half-written G-buffer texel
    if (!(normal.y > Control.w))
        return output; // not up-facing: a wall, a roof, a character

    const float3 worldPosition = WorldPositionOf(P);
    if (any(isnan(worldPosition)))
        return output;

    // The gloss pass's own field and threshold (rule 3). The wetness multiplies the factor so a dry frame is a pure copy even at the noise
    // field's top.
    const float field = Noise(worldPosition.x, worldPosition.z, Tuning.y);
    const float level = 1.0 - saturate(Tuning.x) * saturate(wetness);
    const float width = max(Tuning.z, 0.0005);
    const float puddle = smoothstep(level - width, level + width, field);
    float mask = saturate(wetness) * saturate(puddle);
    if (!(mask > 0.0))
        return output;

    // The gate (rule 4c): the engine's own gloss at this texel, from the frame's material pack. A texel it renders dry keeps the scene's own texel.
    mask *= PuddleExposure(Gate.Load(int3(at, 0)).w, wetness);
    if (!(mask > 0.0))
        return output;

    // The darkening (rule 4): what makes the ground read as water rather than varnish, and the part that has to work before the mirror does.
    float3 colour = scene.rgb * (1.0 - saturate(mask) * saturate(Tuning.w));

    // The mirror: the shared march from this texel, in the direction a flat pool reflects the eye ray. The normal faces the camera's side of the
    // surface, the same rule the water reflection applies to its plane.
    const float3 V = normalize(P);
    float rainCrest = 0.0f;
    float3 N = MirrorNormal(worldPosition, rainCrest);
    if (dot(N, P) > 0.0)
        N = -N;
    const float cosine = saturate(dot(N, -V));
    bool found = false;
    if (Control.x > 0.0)
    {
        const float3 R = reflect(V, N);
        if (!any(isnan(R)))
        {
            const int steps = clamp(int(Sizes.z), 1, kMaxSteps);
            int2 hitTexel = int2(-1, -1);
            float2 hitWindow = float2(0.0, 0.0);
            int marchReason = 0; // the march's own exit code: the water reflection's diagnostic reads it, this pass never does
            float marchDistance = 0.0;
            found = MarchScene(VPosition, P, R, LoadProjection(), Window, Sizes.xy, steps, kMarchThickness, hitTexel, hitWindow, marchReason,
                               marchDistance);
            if (found)
            {
                // Water's Fresnel (rule 4): 2 % straight on, most of it at a grazing angle, which is what makes a low camera a mirror and a high
                // one only a sheen. The hit's colour is read at the crossing's own fraction (SceneMarchColour): the per-frame jitter inside the
                // projection slides the four weights instead of flipping whole texels, the same re-quantisation the water's reflection dropped.
                const float m = 1.0 - cosine;
                const float m2 = m * m;
                const float gain = saturate(mask) * saturate(Control.x) * (kWaterF0 + (1.0 - kWaterF0) * m2 * m2 * m);
                colour += SceneMarchColour(Scene, hitWindow, Window) * gain;
            }
        }
    }

    // The raindrops' own glint: the crest of a splash ring catches the light even where the mirror has nothing to reflect (a wall, a shadowed
    // corner), which is what makes a drop read as striking the water instead of as a wobble in the reflection. It is scaled by the same mask and
    // ripple keys the normal is, so a pool with the ripple at zero and a dry frame are both untouched, and by the clock, so a still frame adds nothing.
    colour += scene.rgb * (kRainGlint * saturate(rainCrest) * saturate(Control.y) * saturate(wetness) * saturate(mask));

    output.colour = float4(colour, scene.a);
    output.stats = float4(mask >= 0.5 ? 1.0 : 0.0, found ? 1.0 : 0.0, 0.0, 0.0);
    return output;
}
