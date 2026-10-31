// Contact shadows before the sun light pass: a short march along the sun direction at half the render window, combined into a copy of
// the engine's screen-space sun mask.
//
// Two pixel shaders, one vertex shader, no sampler (every read is a Load on an exact texel, so the result does not depend on filter
// state and a test can predict it):
//   VS         (vs_5_0) one oversized triangle from SV_VertexID
//   PS_March   (ps_5_0) half the render window -> the contact visibility term, 1 = unoccluded
//   PS_Combine (ps_5_0) the mask's allocation -> the copy the light draw samples, mask * term
//
// Inputs of PS_March:
//   t0 Depth     Texture2D<float>   the draw's g_SamplerDepth (reversed-Z, 0 = the far plane)
//   t2 Camera    Buffer<float4>     g_CameraParameter: the view matrix's three rows at register 0, the view-projection's at register 6,
//                                   the inverse projection's at register 14 and the projection's at register 18 (CameraRegisters.h, the
//                                   reflected layout of the engine's own light shader)
//   t3 Sun       Buffer<float4>     g_LightParam +0x10: the sun direction, as the game reports it
// Inputs of PS_Combine:
//   t4 Mask      Texture2D<float4>  the draw's g_SamplerShadow (the screen-space mask, B8G8R8A8_UNORM, 0..1 visibility)
//   t5 Term      Texture2D<float>   the march's output, half the render window
// and the march's t0 (the depth) again, for the depth-aware upsample. One bind list serves both passes.
//
// Rules:
//  1. Nothing is assumed about the G-buffer, and no view-position buffer is required: the sun light draw does not bind one (measured on
//     the shipped build), so the view-space position of a pixel is reconstructed from its depth with g_CameraParameter's inverse
//     projection, and the normal comes from that position's screen-space derivatives.
//  2. Nothing is dark where nothing is: a pixel without the marched point visible nowhere (sky, no depth, NaN) keeps 1.0, so a pixel
//     that fails validation is never darkened - the failure direction is "no shadow", never "black".
//  3. The march starts off the surface (along its normal and a few pixels along the ray, so the receiver cannot shadow itself), grows
//     exponentially, stops at its first hit and fades out toward the end of its length. A hit needs the sampled surface to be nearer to
//     the camera than the ray point AND the ray point to be within a thickness of it in 3D: a depth difference alone counts a surface
//     that is metres to the side as an occluder, which is what makes a depth-only test draw the object's own outline instead of the
//     contact (measured on the benchmark, round 4).
//  4. The thickness is measured in screen pixels' worth of world size at the receiver's depth: one depth buffer texel is as wide as the
//     world it covers, so a thickness that follows the view depth means the same thing at two metres and at twenty. Near the terminator
//     the term is faded out instead of added: the engine's own cosine and shadow map already darken a surface that faces away from the
//     sun, and a ray traced from such a surface grazes it.
//  5. The term grows to the mask's 8 bits, so a low strength lands on whole levels (0.5 = 128) - by design, because the thing that
//     consumes it is an 8-bit mask.
//  6. The term is a fixed function of the frame's buffers: no frame number, no random offset, no history. DLSS Super Resolution is not
//     a denoiser, and a term that differs between two frames of a still scene would arrive at the upscaler as noise.

#include "CameraRegisters.h"

Texture2D<float> Depth : register(t0);
Buffer<float4> Camera : register(t2);
Buffer<float4> Sun : register(t3);
Texture2D<float4> Mask : register(t4);
Texture2D<float> Term : register(t5);

cbuffer Params : register(b0)
{
    float4 Window; // xy = the render window's origin in mask pixels, zw = its extent in mask pixels
    float4 March;  // x = length in metres, y = steps, z = strength, w = 0 the sun is in world space / 1 it is already in view space
    float4 Sign;   // x = +1 the direction points toward the sun / -1 it is the way the light travels, yz = the mask's size
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VS(uint id : SV_VertexID)
{
    VSOutput o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// The game stores the matrices row-major in the constant buffer, so one row of the buffer is one row of the matrix and a column-vector
// multiply is mul(matrix, vector). The view matrix is three rows (3x4): the register after it is the inverse view's first row, not a
// fourth row of the view.
float3x3 LoadView()
{
    return float3x3(Camera[CS_CAMERA_VIEW_REGISTER + 0].xyz, Camera[CS_CAMERA_VIEW_REGISTER + 1].xyz,
                    Camera[CS_CAMERA_VIEW_REGISTER + 2].xyz);
}
float4x4 LoadViewProjection()
{
    return float4x4(Camera[CS_CAMERA_VIEW_PROJECTION_REGISTER + 0], Camera[CS_CAMERA_VIEW_PROJECTION_REGISTER + 1],
                    Camera[CS_CAMERA_VIEW_PROJECTION_REGISTER + 2], Camera[CS_CAMERA_VIEW_PROJECTION_REGISTER + 3]);
}
float4x4 LoadInverseProjection()
{
    return float4x4(Camera[CS_CAMERA_INVERSE_PROJECTION_REGISTER + 0], Camera[CS_CAMERA_INVERSE_PROJECTION_REGISTER + 1],
                    Camera[CS_CAMERA_INVERSE_PROJECTION_REGISTER + 2], Camera[CS_CAMERA_INVERSE_PROJECTION_REGISTER + 3]);
}
float4x4 LoadProjection()
{
    return float4x4(Camera[CS_CAMERA_PROJECTION_REGISTER + 0], Camera[CS_CAMERA_PROJECTION_REGISTER + 1],
                    Camera[CS_CAMERA_PROJECTION_REGISTER + 2], Camera[CS_CAMERA_PROJECTION_REGISTER + 3]);
}

// The view-space position of a pixel, from its depth: the inverse projection takes a normalized device coordinate and the depth back to
// the point it came from. The normalized device coordinate spans the draw's render window, not its allocation (dynamic resolution renders
// a smaller window inside a mask of the allocation's size), so the window is what a pixel's position is measured against. A depth of 0 or a
// NaN gives a point at infinity, which the callers reject before using it.
float3 ViewPositionOf(int2 at, float depth)
{
    const float2 ndc = float2(((float(at.x) + 0.5) - Window.x) / Window.z * 2.0 - 1.0,
                              1.0 - ((float(at.y) + 0.5) - Window.y) / Window.w * 2.0);
    const float4 viewPoint = mul(LoadInverseProjection(), float4(ndc, depth, 1.0));
    return viewPoint.w != 0.0 ? viewPoint.xyz / viewPoint.w : float3(0.0, 0.0, 0.0);
}


// The march's shape, in the units a pixel has at the receiver's depth (`pixel` is the world size of one depth texel there):
static const int kMaxSteps = 24;
static const float kStepGrowth = 1.35;
static const float kNormalTexels = 1.5;     // the start's offset along the surface normal, so the ray begins above its own pixel
static const float kStartTexels = 2.5;      // hits closer than this along the ray are the receiver's own surface, not contact
static const float kThicknessTexels = 12.0; // how far behind the ray a sampled surface may be and still count as the occluder
static const float kMinThickness = 0.005;   // metres: a floor for a very near surface
static const float kMaxThickness = 0.25;    // and a ceiling, so a long march does not shadow through whole objects
static const float kDepthBias = 0.002;      // metres the sampled surface must be in front of the ray point before it is an occluder
static const float kTerminatorStart = 0.02; // dot(N, L) below this is not lit at all: the engine's shading owns it
static const float kTerminatorFull = 0.25;  // and above this the term is at full strength
static const float kFadeStart = 0.7;        // the term fades to 1.0 over the last 30 % of the march, so its end cannot cut
static const float kUpsampleDepthRelative = 0.02; // the combine's depth-aware upsample: a 2 % depth difference is another surface
static const float kNormalDepthRelative = 0.02;   // a normal's neighbour belongs to the same surface within this depth difference

// The geometric normal of a pixel, from its own depth neighbourhood. A neighbour takes part only when its depth belongs to the same
// surface as the pixel's; the ones across a silhouette are replaced by the pixel itself, so the difference stays one-sided and the normal
// still points out of the pixel's own surface. (The 2x2 quad derivatives ddx/ddy cannot do that: at a silhouette the quad mixes two
// surfaces, the normal points somewhere between them, and a grazing "lit" answer drew the term along the object's outline.)
float3 NormalOf(int2 at, float depth, float3 P, float2 size)
{
    const float tolerance = kNormalDepthRelative * depth;
    const bool2 insideLow = bool2(at.x > 0, at.y > 0);
    const bool2 insideHigh = bool2(float(at.x) + 1.0 < size.x, float(at.y) + 1.0 < size.y);
    const int2 left = int2(at.x - (insideLow.x ? 1 : 0), at.y);
    const int2 right = int2(at.x + (insideHigh.x ? 1 : 0), at.y);
    const int2 up = int2(at.x, at.y - (insideLow.y ? 1 : 0));
    const int2 down = int2(at.x, at.y + (insideHigh.y ? 1 : 0));
    const float4 neighbours = float4(Depth.Load(int3(left, 0)), Depth.Load(int3(right, 0)), Depth.Load(int3(up, 0)),
                                      Depth.Load(int3(down, 0)));
    const float3 PLeft = insideLow.x && abs(neighbours.x - depth) <= tolerance ? ViewPositionOf(left, neighbours.x) : P;
    const float3 PRight = insideHigh.x && abs(neighbours.y - depth) <= tolerance ? ViewPositionOf(right, neighbours.y) : P;
    const float3 PUp = insideLow.y && abs(neighbours.z - depth) <= tolerance ? ViewPositionOf(up, neighbours.z) : P;
    const float3 PDown = insideHigh.y && abs(neighbours.w - depth) <= tolerance ? ViewPositionOf(down, neighbours.w) : P;
    return cross(PRight - PLeft, PDown - PUp);
}

bool2 Inside(int2 at, float2 size)
{
    return bool2(at.x >= 0 && at.y >= 0, float(at.x) < size.x && float(at.y) < size.y);
}

// The sun direction in view space, pointing the way the ray is traced.
float3 SunDirection()
{
    float3 d = Sun[0].xyz;
    if (March.w < 0.5)
        d = mul(LoadView(), d);
    d *= Sign.x;
    const float lengthSquared = dot(d, d);
    // A direction the shader cannot trust must not shade anything: 0 makes PS_March return 1.0.
    return lengthSquared > 1e-6 ? d * rsqrt(lengthSquared) : float3(0.0, 0.0, 0.0);
}

float PS_March(float4 position : SV_Position) : SV_Target0
{
    const float2 size = Sign.yz;
    // The 2x2 full-resolution block this half-resolution pixel stands for: texel k of the window covers the window's pixels 2k and 2k+1, so
    // its centre is 2k + 1 (a viewport-relative position of k + 0.5).
    const int2 at = int2(Window.xy + position.xy * 2.0);
    if (!all(Inside(at, size)))
        return 1.0;

    const float depth = Depth.Load(int3(at, 0));
    if (!(depth > 0.0))
        return 1.0; // the far plane, a cleared depth or a NaN: nothing is there to cast a contact shadow

    const float3 P = ViewPositionOf(at, depth);
    if (any(isnan(P)))
        return 1.0;

    // The geometric normal from the pixel's own depth neighbourhood: no G-buffer layout is assumed.
    const float3 rawNormal = NormalOf(at, depth, P, size);
    const float normalLengthSquared = dot(rawNormal, rawNormal);
    if (!(normalLengthSquared > 1e-12))
        return 1.0; // a degenerate patch of the view position (a hole, a NaN neighbour): never shade it
    float3 N = rawNormal * rsqrt(normalLengthSquared);
    // The camera is the origin in view space, so a normal pointing away from P faces away from the viewer.
    if (dot(N, P) > 0.0)
        N = -N;

    const float3 L = SunDirection();
    if (dot(L, L) < 0.5)
        return 1.0;
    // A surface that faces away from the sun is already dark: adding a contact term there would double-darken it.
    if (dot(N, L) <= 0.0)
        return 1.0;

    // The ray points are in view space, so it is the projection that takes them to the clip volume - not the view-projection, which
    // expects a world-space point and would apply the view matrix a second time (the scene's rotation then throws every tap off screen).
    const float4x4 projection = LoadProjection();
    // The world size of one depth texel at this pixel's depth: half the frustum's height there, over the pixel rows it spans. This is what
    // makes the offsets and the thickness follow the view depth - one number, three uses, no second magic constant.
    const float viewDepth = -P.z;
    if (!(viewDepth > 0.0) || !(projection[1][1] > 0.0) || !(Window.w > 0.0))
        return 1.0; // a matrix or a window the shader cannot measure a pixel with
    const float pixel = 2.0 * viewDepth / (projection[1][1] * Window.w);
    const float thickness = clamp(kThicknessTexels * pixel, kMinThickness, kMaxThickness);
    const float start = kStartTexels * pixel + kDepthBias;
    // Off the surface along its own normal, so the first taps cannot land on the pixel's surface where it is seen edge-on.
    const float3 origin = P + N * (kNormalTexels * pixel);
    // The part of a surface the term can still be about: one that faces the sun only just is already dark in the engine's own shading, and a
    // ray traced from such a surface grazes it.
    const float facing = smoothstep(kTerminatorStart, kTerminatorFull, dot(N, L));
    if (!(facing > 0.0))
        return 1.0;

    const int steps = clamp(int(March.y), 1, kMaxSteps);
    const float growth = pow(kStepGrowth, float(steps)) - 1.0;
    const float base = March.x * (kStepGrowth - 1.0) / growth; // the geometric series sums to March.x

    float t = 0.0;
    [loop]
    for (int k = 0; k < steps; ++k)
    {
        const float stepLength = base * pow(kStepGrowth, float(k));
        t += stepLength;
        if (t < start)
            continue; // the receiver's own surface, a few texels out: never an occluder of itself
        const float3 S = origin + L * t;
        const float4 clip = mul(projection, float4(S, 1.0));
        if (clip.w <= 0.0)
            break; // behind the camera
        const float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
        const int2 tap = int2(Window.xy + uv * Window.zw);
        if (!all(Inside(tap, size)))
            break; // the ray left the picture: nothing more can be occluded

        const float tapDepth = Depth.Load(int3(tap, 0));
        if (!(tapDepth > 0.0))
            continue; // sky behind the point: not an occluder
        const float3 Q = ViewPositionOf(tap, tapDepth);
        if (any(isnan(Q)))
            continue;

        // Three tests, and all three are needed: the ray has gone behind the sampled surface (by more than the bias), it is at least the
        // start's distance away from the receiver itself measured in 3D (a pixel's own surface cannot shadow it), and the ray point is
        // still within a thickness of the surface. The depth difference alone would count a surface a metre to the side.
        const float delta = (-S.z) - (-Q.z);
        const float tolerance = max(thickness, 0.6 * stepLength);
        if (delta > kDepthBias && distance(P, Q) >= start && distance(S, Q) < tolerance)
        {
            // And the tap has to face the receiver: an occluder's surface has the receiver on its outside, while the object's own surface
            // at a silhouette curves away from the receiver, which sits inside its tangent plane. That is the self-shadow of the old
            // test, and the two extra depth loads are only made for a candidate hit.
            const int2 tapRight = tap + int2(1, 0);
            const int2 tapDown = tap + int2(0, 1);
            bool faces = true;
            if (all(Inside(tapRight, size)) && all(Inside(tapDown, size)))
            {
                const float rightDepth = Depth.Load(int3(tapRight, 0));
                const float downDepth = Depth.Load(int3(tapDown, 0));
                if (rightDepth > 0.0 && downDepth > 0.0)
                {
                    float3 tapNormal = cross(ViewPositionOf(tapRight, rightDepth) - Q, ViewPositionOf(tapDown, downDepth) - Q);
                    const float tapLengthSquared = dot(tapNormal, tapNormal);
                    if (tapLengthSquared > 1e-12)
                    {
                        tapNormal *= rsqrt(tapLengthSquared);
                        if (dot(tapNormal, Q) > 0.0)
                            tapNormal = -tapNormal; // the camera is the origin: a normal pointing away from it faces away
                        faces = dot(tapNormal, P - Q) > 0.0;
                    }
                }
            }
            if (faces)
            {
                const float fade = 1.0 - smoothstep(kFadeStart * March.x, March.x, t);
                return saturate(1.0 - March.z * facing * fade);
            }
        }
    }
    return 1.0;
}

float4 PS_Combine(float4 position : SV_Position) : SV_Target0
{
    const int2 at = int2(position.xy);
    const float4 mask = Mask.Load(int3(at, 0));
    const float2 size = Sign.yz;

    // Outside the render window the copy is the engine's mask, unchanged: the light draw does not touch those pixels, and a reader that
    // does (a filter at the window's edge) sees exactly what it saw before.
    const float2 relative = position.xy - Window.xy;
    if (relative.x < 0.0 || relative.y < 0.0 || relative.x >= Window.z || relative.y >= Window.w)
        return float4(mask.rgb, mask.a);

    // The four half-resolution texels around this pixel, weighted by how well their depth agrees with the pixel's own: a term computed
    // for a surface two metres away must not bleed onto this one.
    const float2 centre = relative * 0.5 - 0.5;
    const int2 first = int2(floor(centre));
    const float2 fraction = centre - float2(first);
    const float depth = Depth.Load(int3(at, 0));
    // The term texture is the allocation's size, but only the render window's half is written (its own viewport), so the window-relative
    // tap index is what the march wrote; the origin turns it into the texture's own texel.
    const int2 termOrigin = int2(floor(Window.xy * 0.5));
    const int2 termExtent = int2(floor(Window.zw * 0.5));

    float sum = 0.0;
    float weightSum = 0.0;
    [unroll]
    for (int dy = 0; dy < 2; ++dy)
    {
        [unroll]
        for (int dx = 0; dx < 2; ++dx)
        {
            const int2 tap = first + int2(dx, dy);
            if (!all(Inside(tap, float2(termExtent))))
                continue; // outside the march's own target: no term there
            const float2 weights = float2(dx == 0 ? 1.0 - fraction.x : fraction.x, dy == 0 ? 1.0 - fraction.y : fraction.y);
            float weight = weights.x * weights.y;
            if (weight <= 0.0)
                continue;
            // The tap's own depth, so a neighbour across an edge is dropped instead of blurred in. The threshold is relative to the pixel's
            // own depth: a reversed-Z depth difference of two per cent is about two per cent of a metre at any distance, where a fixed
            // threshold is a fraction of a millimetre near the camera and metres far away (measured on the benchmark).
            const float2 tapPixel = Window.xy + (float2(tap) + 0.5) * 2.0;
            const float tapDepth = Depth.Load(int3(int2(tapPixel), 0));
            if (abs(tapDepth - depth) > kUpsampleDepthRelative * max(depth, tapDepth))
                continue;
            sum += Term.Load(int3(termOrigin + tap, 0)) * weight;
            weightSum += weight;
        }
    }
    // No usable neighbour (an edge, or a term the march never wrote): the engine's mask goes through untouched.
    const float term = weightSum > 1e-5 ? saturate(sum / weightSum) : 1.0;
    return float4(mask.rgb * term, mask.a);
}
