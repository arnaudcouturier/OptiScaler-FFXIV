// Ambient occlusion before the engine's light passes: our own occlusion from the game's depth and G-buffer normal, in place of the
// engine's own screen-space occlusion texture.
//
// Three entry points, one vertex shader, no sampler (every read is a Load on an exact texel, so the result does not depend on filter state
// and a host test can predict it):
//   VS        (vs_5_0) one oversized triangle from SV_VertexID
//   PS_Ao     (ps_5_0) the occlusion target's texels -> this module's raw occlusion, 1 = unoccluded
//   PS_Final  (ps_5_0) the same texels -> the 4x4 denoise, the strength, the blend with the engine's texture, 8-bit encoded
//
// Inputs of PS_Ao:
//   t0 Depth    Texture2D<float>    the reader's g_SamplerDepth (reversed-Z: 1 at the near plane, 0 at the far plane)
//   t1 Normal   Texture2D<float4>   the reader's g_SamplerGBuffer: the primary world-space normal as n * 0.5 + 0.5 in rgb
//   t2 Camera   Buffer<float4>      g_CameraParameter: the view matrix's three rows at register 0, the view-projection's at 6, the
//                                   inverse view-projection's at 10, the inverse projection's at 14 and the projection's at 18
// Inputs of PS_Final:
//   t0 Depth (unused here, kept for one bind list), t3 Raw (the march's own target), t4 Occlusion (the engine's texture, for the blend)
//
// Rules:
//  1. The occlusion target is a fraction of the depth allocation and every reader samples it with the allocation-relative screen coordinate,
//     so a texel of ours stands for the depth texels at the same fraction of the depth texture: `Mapping` is that ratio (2 at the session's
//     1920x1080 occlusion texture over a 3840x2160 allocation) and there is no origin. The engine's own producer fills only the part of the
//     texture that lies inside the render window, so this pass writes only `Region` and its target arrives holding the engine's own texels
//     everywhere else.
//  2. The normal comes from the G-buffer, not from the depth's screen-space derivatives: the engine has already written it, it is exact at
//     a silhouette, and it is the surface's own world-space normal turned into view space by the view matrix. A texel whose stored normal
//     does not decode to a unit vector is not a surface (a cleared or half-written G-buffer texel) and keeps 1.0.
//  3. Nothing is dark where nothing is: a pixel without usable depth (the far plane, a NaN, a zero) keeps 1.0, so a pixel that fails
//     validation is never darkened - the failure direction is "no occlusion", never "black".
//  4. The search is two horizon slices a direction, in the tangent plane of the pixel's normal, rotated per pixel by a 4x4 pattern (a
//     banding breaker, not a random number: it is a function of the pixel only, so a still scene gives the same picture every frame - the
//     upscaler is not a denoiser and a term that differs between two frames of a still scene arrives as noise).
//  5. A tap is an occluder only when it is in front of the ray point (the thickness heuristic: farther in front than the search radius is
//     another object, not this one) and rises above the pixel's tangent plane by more than the bias. A surface at the same height as the
//     pixel (a flat floor) therefore never shadows itself, at any depth quantisation.
//  6. The occlusion value is the mean of the two slices' horizons, clipped to the tangent plane, so 1 is a pixel that sees open sky in
//     every direction it searched, and the strength scales how much of it is applied (0 gives 1.0 everywhere).
//  7. The engine's occlusion value is its texture's red channel, and 1 is unoccluded: every reader whose bytecode was captured samples that
//     one channel and uses it as `1 + (x - 1) * k`. This pass therefore writes its value to red and returns the engine's own green, blue and
//     alpha untouched, so a channel whose meaning this module has not measured cannot be corrupted by it. The two values are visibility
//     factors in the same convention, so they compose by multiplication with the blend taking the product part of the way: 1.0 applies both,
//     0 keeps the engine's texel, and a term of 1.0 is the identity at every blend.
//  8. The term is a fixed function of the frame's buffers: no frame number, no random offset, no history.

Texture2D<float> Depth : register(t0);
Texture2D<float4> Normal : register(t1);
Buffer<float4> Camera : register(t2);
Texture2D<float> Raw : register(t3);
Texture2D<float4> Occlusion : register(t4);

cbuffer Params : register(b0)
{
    float4 Window;  // xy = the render window's origin in depth texels, zw = its size in depth texels
    float4 Mapping; // xy = depth texels per occlusion texel, zw = the occlusion target's size in texels
    float4 Sizes;   // xy = the depth texture's size in texels, z = steps a direction, w = the search radius in metres
    float4 Tuning;  // x = strength, y = blend, z = the horizon's cosine bias, w = unused
    float4 Region;  // xy = the occlusion texel this pass writes from, zw = how many texels of it in each direction
};

// g_CameraParameter's register numbers come from the one header that states the layout (ext/ffxiv/relight/shaders/CameraRegisters.h, the
// reflected layout of the engine's own shaders), so this pass and the contact shadow pass cannot drift apart: the view matrix at register 0, the
// view-projection's at 6, the inverse projection's at 14 and the projection's at 18. The build passes that folder with -I (ao.shaders.props).
#include "CameraRegisters.h"

// The search's shape. The steps are the key's (4, 8 or 16); the rest is fixed, so the picture cannot drift with a number the ini does not
// name.
static const float kMaxRadiusWindowFraction = 0.25; // the radius' cap on the screen, as a share of the window's height (2026-11-07)
static const float kStepGrowth = 1.45;     // each step of a slice is this much longer than the one before it
static const float kStepStart = 0.06;      // the first step covers this fraction of the radius, a few pixels at a metre
static const int kMaxTaps = 32;            // the hard bound the loop unrolls to at the highest quality
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

float3x3 LoadView()
{
    return float3x3(Camera[CS_CAMERA_VIEW_REGISTER + 0].xyz, Camera[CS_CAMERA_VIEW_REGISTER + 1].xyz,
                    Camera[CS_CAMERA_VIEW_REGISTER + 2].xyz);
}

float4x4 LoadProjection()
{
    return float4x4(Camera[CS_CAMERA_PROJECTION_REGISTER + 0], Camera[CS_CAMERA_PROJECTION_REGISTER + 1],
                    Camera[CS_CAMERA_PROJECTION_REGISTER + 2], Camera[CS_CAMERA_PROJECTION_REGISTER + 3]);
}

float4x4 LoadInverseProjection()
{
    return float4x4(Camera[CS_CAMERA_INVERSE_PROJECTION_REGISTER + 0], Camera[CS_CAMERA_INVERSE_PROJECTION_REGISTER + 1],
                    Camera[CS_CAMERA_INVERSE_PROJECTION_REGISTER + 2], Camera[CS_CAMERA_INVERSE_PROJECTION_REGISTER + 3]);
}

// The view-space position of a depth texel. `centre` is the texel's centre in depth-texel units, not its integer index: the mapping from
// an occlusion texel lands between two depth texels, and rounding it here would put a half-texel error on every tap.
float3 ViewPositionOf(float2 centre, float depth)
{
    const float2 ndc = float2((centre.x - Window.x) / Window.z * 2.0 - 1.0, 1.0 - (centre.y - Window.y) / Window.w * 2.0);
    const float4 viewPoint = mul(LoadInverseProjection(), float4(ndc, depth, 1.0));
    return viewPoint.w != 0.0 ? viewPoint.xyz / viewPoint.w : float3(0.0, 0.0, 0.0);
}

// A view-space position taken back to the depth texel it would be drawn at, or a negative index when it left the render window. The
// centre it answers with is the texel's own: the sampled depth belongs to that texel, and a position reconstructed from a fractional
// coordinate and a neighbour's depth is a point metres off the surface it belongs to (which the search would read as an occluder).
bool ProjectToTexel(float3 viewPoint, out float2 centre)
{
    const float4 clip = mul(LoadProjection(), float4(viewPoint, 1.0));
    centre = float2(-1.0, -1.0);
    if (!(clip.w > 0.0))
        return false; // behind the camera
    const float2 ndc = clip.xy / clip.w;
    const float2 projected = float2(Window.x + (ndc.x * 0.5 + 0.5) * Window.z, Window.y + (0.5 - ndc.y * 0.5) * Window.w);
    if (!(projected.x >= Window.x) || !(projected.y >= Window.y) || !(projected.x < Window.x + Window.z) ||
        !(projected.y < Window.y + Window.w))
        return false; // the ray left the picture
    centre = floor(projected) + 0.5;
    const float2 size = Sizes.xy;
    return centre.x >= 0.0 && centre.y >= 0.0 && centre.x < size.x && centre.y < size.y;
}

// The surface's own normal, from the depth (2026-11-07). The G-buffer normal carries the materials' normal maps: on flat ground in the user's
// frames it is tilted a median 5 (a room) to 13 (paving) degrees from the real surface, and a search plane taken from it let the real surface
// rise above the plane - 11-19 % of occlusion on flat ground, multiplied into the engine's own. The search plane is therefore the depth's own
// surface: the central difference where the two neighbours of an axis agree, the smaller step at a silhouette (the same surface there). False when an axis has no neighbour with a depth; the caller then keeps the G-buffer normal.
// Two one-sided steps belong to the same surface when they agree within half of the larger one, or are both below a small share of the
// depth (a quantised depth makes neighbours on one surface step by 0 and then by one quantum): the plane is then the central difference.
// Only where they disagree (a silhouette) does the smaller step pick the side.
static const float kStepAgreeRelative = 0.5;
static const float kStepFloorRelative = 0.002;
bool GeometricNormal(int2 at, float3 P, float2 size, out float3 N)
{
    N = float3(0.0, 0.0, 0.0);
    float3 axes[2] = { float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0) };
    const float floorStep = kStepFloorRelative * abs(P.z);
    [unroll]
    for (int axis = 0; axis < 2; ++axis)
    {
        float3 neighbour[2] = { float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0) };
        bool has[2] = { false, false };
        [unroll]
        for (int s = 0; s < 2; ++s)
        {
            const int side = s == 0 ? 1 : -1;
            const int2 tap = at + (axis == 0 ? int2(side, 0) : int2(0, side));
            if (tap.x < 0 || tap.y < 0 || float(tap.x) >= size.x || float(tap.y) >= size.y)
                continue;
            const float tapDepth = Depth.Load(int3(tap, 0));
            if (!(tapDepth > 0.0))
                continue;
            const float3 Q = ViewPositionOf(float2(tap) + 0.5, tapDepth);
            if (any(isnan(Q)))
                continue;
            neighbour[s] = Q;
            has[s] = true;
        }
        if (has[0] && has[1])
        {
            const float stepPlus = abs(neighbour[0].z - P.z);
            const float stepMinus = abs(neighbour[1].z - P.z);
            if (abs(stepPlus - stepMinus) <= max(kStepAgreeRelative * max(stepPlus, stepMinus), floorStep))
                axes[axis] = neighbour[0] - neighbour[1];
            else
                axes[axis] = stepPlus < stepMinus ? neighbour[0] - P : P - neighbour[1];
        }
        else if (has[0])
            axes[axis] = neighbour[0] - P;
        else if (has[1])
            axes[axis] = P - neighbour[1];
        else
            return false;
    }
    // Normalised here, with a scale-free degeneracy test: the cross product of two one-texel differences is tiny up close (its length is the
    // square of the texel's size in metres), so an absolute length test would refuse every texel nearer than tens of metres.
    N = cross(axes[0], axes[1]);
    const float lengthN = length(N);
    if (!(lengthN > 1e-3 * length(axes[0]) * length(axes[1])))
        return false;
    N /= lengthN;
    return true;
}

// The 4x4 pattern that rotates the two slices per pixel: a function of the pixel only (rule 4).
float SliceRotation(int2 at)
{
    return (float(((at.x & 3) + (at.y & 3) * 4)) + 0.5) / 16.0 * 1.5707963267948966; // 0..90 degrees
}

float PS_Ao(float4 position : SV_Position) : SV_Target0
{
    // The depth texel this occlusion texel stands for: the occlusion target is a fraction of the depth allocation and the readers sample it
    // by the same fraction, so the mapping is the ratio of the two sizes and the texel's own index is where its block starts (rule 1). The
    // position carries the pixel's centre (+0.5) and that half must not reach the ratio: at scale 2 a texel of ours stands for depth texel 2t,
    // and scaling the centre would read 2t+1, the block's second texel, which is not what the rules, the reference or this comment say.
    const int2 own = int2(position.xy);
    const int2 at = int2(float2(own) * Mapping.xy);
    const float2 size = Sizes.xy;
    if (at.x < 0 || at.y < 0 || float(at.x) >= size.x || float(at.y) >= size.y)
        return 1.0;

    const float depth = Depth.Load(int3(at, 0));
    if (!(depth > 0.0))
        return 1.0; // the far plane, a cleared depth or a NaN: nothing there can be occluded (rule 3)

    // The depth belongs to the texel, so the position is the texel's centre and not the fractional mapping (see ProjectToTexel).
    const float2 centre = float2(at) + 0.5;
    const float3 P = ViewPositionOf(centre, depth);
    if (any(isnan(P)))
        return 1.0;

    // The search plane: the depth's own surface (GeometricNormal); the G-buffer normal (world space, as the engine wrote it, turned into view
    // space, rule 2) only where an axis has no neighbour with a depth.
    float3 N = float3(0.0, 0.0, 0.0);
    if (!GeometricNormal(at, P, size, N)) // a unit normal when it answers true
    {
        const float3 world = Normal.Load(int3(at, 0)).xyz * 2.0 - 1.0;
        const float lengthSquared = dot(world, world);
        if (!(lengthSquared > kNormalLengthLow) || !(lengthSquared < kNormalLengthHigh))
            return 1.0;
        N = mul(LoadView(), world);
        const float viewLengthSquared = dot(N, N);
        if (!(viewLengthSquared > 1e-6))
            return 1.0;
        N *= rsqrt(viewLengthSquared);
    }
    // The camera is the origin in view space, so a normal pointing away from P faces away from the viewer.
    if (dot(N, P) > 0.0)
        N = -N;

    const int steps = clamp(int(Sizes.z), 1, kMaxTaps);
    if (!(Sizes.w > 0.0) || !(Window.z > 0.0) || !(Window.w > 0.0))
        return 1.0;
    // The search radius in metres, capped on the screen at kMaxRadiusWindowFraction of the window's height (2026-11-07): a 1 m radius at 0.7 m
    // from the camera spans twice the screen, and a close-up (a cutscene's face and a sheet of paper in the benchmark) darkened whole surfaces
    // behind the nearest object in banded steps. At gameplay distances (1 m at 10 m is about 0.14 of the height) the cap changes nothing; a 0.7 m close-up is capped from twice the screen to a quarter of it.
    const float4x4 projectionForRadius = LoadProjection();
    const float texelMetres = projectionForRadius[1][1] > 0.0 ? 2.0 * abs(P.z) / (projectionForRadius[1][1] * Window.w) : 0.0;
    const float radius = texelMetres > 0.0 ? min(Sizes.w, kMaxRadiusWindowFraction * Window.w * texelMetres) : Sizes.w;

    // The tangent plane's two directions, one slice along each, rotated per pixel so the pattern of the two slices does not align with the
    // screen's grid.
    const float3 helper = abs(N.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    const float3 B1 = normalize(cross(N, helper));
    const float3 B2 = cross(N, B1);
    // The rotation is a function of the texel of this target, so the same occlusion texel gives the same answer whatever the depth allocation
    // around it is.
    const float phi = SliceRotation(own);
    const float3 direction0 = B1 * cos(phi) + B2 * sin(phi);
    const float3 direction1 = -B1 * sin(phi) + B2 * cos(phi);

    // A step of a slice is longer than the one before it (a geometric series over the radius), so the near field gets most of the taps where
    // occlusion is small and sharp, and the last tap still reaches the whole radius.
    const float growth = pow(kStepGrowth, float(steps)) - 1.0;
    const float base = radius * (kStepGrowth - 1.0) / max(growth, 1e-6);
    const float bias = Tuning.z;

    float occlusion = 0.0;
    [unroll]
    for (int slice = 0; slice < 2; ++slice)
    {
        const float3 direction = slice == 0 ? direction0 : direction1;
        float horizon = 0.0;
        float t = 0.0;
        [loop]
        for (int k = 0; k < steps; ++k)
        {
            t += base * pow(kStepGrowth, float(k));
            const float3 S = P + direction * t;
            float2 tapCentre = float2(0.0, 0.0);
            if (!ProjectToTexel(S, tapCentre))
                break; // the ray left the picture: nothing more can be occluded along it
            const int2 tap = int2(floor(tapCentre));
            const float tapDepth = Depth.Load(int3(tap, 0));
            if (!(tapDepth > 0.0))
                continue; // sky: not an occluder
            const float3 Q = ViewPositionOf(tapCentre, tapDepth);
            if (any(isnan(Q)))
                continue;
            const float3 D = Q - P;
            const float distance3d = length(D);
            if (!(distance3d > 1e-5) || !(distance3d <= radius))
                continue;
            // The thickness heuristic (rule 5): the sampled surface has to be in front of the ray point, and not farther in front than the
            // search radius - a wall a metre nearer the camera is another object, not the occluder this ray is looking for.
            const float inFront = (-S.z) - (-Q.z);
            if (!(inFront > 0.0) || !(inFront <= radius))
                continue;
            // The height of the occluder above the pixel's tangent plane, clipped at the plane itself.
            horizon = max(horizon, dot(D / distance3d, N));
        }
        occlusion += saturate(horizon - bias) * 0.5;
    }
    return saturate(1.0 - occlusion);
}

float4 PS_Final(float4 position : SV_Position) : SV_Target0
{
    const int2 at = int2(position.xy);
    const int2 regionLow = int2(Region.x, Region.y);
    const int2 regionHigh = int2(Region.x + Region.z, Region.y + Region.w);

    // The 4x4 denoise: the search's two slices leave a little structure at the highest quality, and a 4x4 box is what the engine's own
    // occlusion is smooth enough for. Taps outside the region are skipped rather than clamped: the search never wrote them, and a clamped edge
    // tap would darken a border.
    float sum = 0.0;
    float taps = 0.0;
    [unroll]
    for (int dy = 0; dy < 4; ++dy)
    {
        [unroll]
        for (int dx = 0; dx < 4; ++dx)
        {
            const int2 tap = at + int2(dx, dy) - 1;
            if (tap.x < regionLow.x || tap.y < regionLow.y || tap.x >= regionHigh.x || tap.y >= regionHigh.y)
                continue;
            sum += Raw.Load(int3(tap, 0));
            taps += 1.0;
        }
    }
    const float blurred = taps > 0.0 ? sum / taps : 1.0;
    const float ours = saturate(1.0 - Tuning.x * (1.0 - blurred));

    // The engine's own texel, at the same size as this target. Only its red channel is replaced: two visibility factors compose by
    // multiplication, and the blend takes that product part of the way (1.0 applies the engine's occlusion and this module's, multiplied; 0.0
    // keeps the engine's texel; a term of 1.0 is the identity at every blend, so a search that finds nothing cannot delete the engine's
    // occlusion). Its other three channels go back as they came (rule 7).
    const float4 engine = Occlusion.Load(int3(at, 0));
    const float red = engine.r * (1.0 + (ours - 1.0) * Tuning.y);
    return float4(red, engine.g, engine.b, engine.a);
}
