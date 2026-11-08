// The screen-space light bounce's own march, against the game's scene depth and view positions.
//
// One function, no sampler and no state of its own: every read is a Load on an exact texel of a view-position buffer, so the result does not
// depend on filter state, a host test can predict it, and the pass composes rays from it without a second march file. The projection, the
// window and the allocation are the caller's: the march lives in the caller's own shaders and the pass is the only product caller.
//
// This file is the light bounce's own: it shares no math with SceneMarch.hlsl (the water reflections' and the puddles' mirror's surface
// march, another module's file - this pass never edits it and never includes it), and what the two share is the shape every march of this
// family takes: a quadratic step ramp so the near field gets the taps, a crossing between two steps, and a thickness rejection so a wall the
// ray passed a metre ago is never the surface it crossed.
//
// What it does, in the order the pass states it:
//  1. the ray starts a small distance from its origin texel and steps outward on a quadratic ramp over the range key; a step that leaves the
//     render window, or that runs past the passed-in range, ends the march with no hit;
//  2. the scene is a depth surface: a step is a hit when its ray point lies BEHIND the visible surface at its texel, measured along the
//     camera's own line (the distance from the camera, which is the view space's origin), by more than the bias that keeps a surface from
//     shadowing itself and by no more than the thickness above that bias. The interval the crossing sits in is halved four times, and the
//     refined texel is where the bounce's radiance is read from. A ray that stays in front of the visible surface never hits it, so a flat
//     surface cannot light itself;
//  3. the answer is that texel's view position and its window coordinate, or `found = false`. A missed ray is the caller's decision: the
//     irradiance estimator simply adds a zero for it, and the picture loses one sample instead of one texel;
//  4. `found` and the refined view position are the whole answer: the march writes nothing, projects nothing of its own and declines nothing.
//
// One source, two mirrors: the rules header (ext/ffxiv/engine/GiRules.h) carries the same constants under the same names, and the host test
// computes the reference march from its own copy of the same fixture, so a shader side that drifted from the rules cannot pass the tests.

// The march's fixed shape. The step count is the caller's key; these are not, so the picture cannot drift with a number an ini does not name.
// (GiRules.h carries the same numbers for the host tests.)
static const float kSurfaceBias = 0.02;     // metres a ray point must lie behind a surface before it has crossed it
static const float kDepthBias = 0.02;       // the bias grows by this share of the surface's own distance (a far surface is sampled coarser)
static const float kThickness = 0.30;       // metres of surface tolerance behind a crossing, above the bias
static const int kMaxRefine = 4;            // halvings of the interval the crossing sits in

// The window-space scene coordinate a view-space point projects to (texels, not floored): the march's texel test reads the integer part of
// it. `inside` says whether the point projects into the render window at all (behind the camera and outside the picture both answer false).
float2 GiProject(float3 viewPoint, float4x4 projection, float4 window, out bool inside)
{
    inside = false;
    const float4 clip = mul(projection, float4(viewPoint, 1.0));
    if (!(clip.w > 0.0))
        return float2(0.0, 0.0); // behind the camera
    const float2 ndc = clip.xy / clip.w;
    const float2 projected = float2(window.x + (ndc.x * 0.5 + 0.5) * window.z, window.y + (0.5 - ndc.y * 0.5) * window.w);
    if (!(projected.x >= window.x) || !(projected.y >= window.y) || !(projected.x < window.x + window.z) ||
        !(projected.y < window.y + window.w))
        return projected; // the ray left the picture: the coordinate stands, `inside` does not
    inside = true;
    return projected;
}

// The scene texel a view-space point would be drawn at, or a negative index when it is outside the render window (the march's texel test,
// with this pass's own name so the two files never share a symbol).
int2 GiTexel(float3 viewPoint, float4x4 projection, float4 window, out bool inside)
{
    inside = false;
    float2 projected = float2(0.0, 0.0);
    projected = GiProject(viewPoint, projection, window, inside);
    if (!inside)
        return int2(-1, -1);
    const int2 texel = int2(floor(projected));
    if (texel.x < 0 || texel.y < 0)
        return int2(-1, -1);
    inside = true;
    return texel;
}

// How far a ray point lies behind the visible surface sampled at its texel, along the camera's own line, less the bias that keeps a surface
// from crossing itself: positive is a crossing, negative is still in front. A texel with no surface (the sky) is never behind anything.
float GiBehind(float3 rayPoint, float3 surface)
{
    if (!(dot(surface, surface) > 1.0e-6))
        return -1.0;
    const float surfaceDistance = length(surface);
    return length(rayPoint) - surfaceDistance - (kSurfaceBias + kDepthBias * surfaceDistance);
}

// The march itself: `origin` is the bounce point in view space (the origin texel's own view position), `direction` the unit ray, `steps` the
// caller's key, `range` the reach in view metres (`ViewPositions` is the composite's own view-position buffer). The first confirmed surface
// along the ray comes back through `hitTexel` (the texel the radiance is read on) and `hitView` (the refined view position); `found` is the
// hit or nothing, and a miss is the caller's answer, not a failure of the march.
bool GiMarchFrom(Texture2D<float4> ViewPositions, float3 origin, float3 direction, float4x4 projection, float4 window,
                 int steps, float range, out int2 hitTexel, out float3 hitView)
{
    bool found = false;
    hitTexel = int2(-1, -1);
    hitView = float3(0.0, 0.0, 0.0);
    if (!(steps > 0))
        return false;

    // The quadratic ramp over the range, the march family's own shape: the first steps are short (a cosine ray leaves its texel narrow) and
    // the last step is the range itself.
    float previous = 0.0; // the last step that was still in front of the surface, in metres along the ray (0 = the origin itself)
    [loop]
    for (int k = 0; k < steps; ++k)
    {
        const float u = float(k + 1) / float(steps);
        const float next = kSurfaceBias + (range - kSurfaceBias) * u * u;
        if (next > range)
            break; // the range's own bound: a ray that reaches past it ends there, the estimator adds a zero for it
        bool inside = false;
        const int2 texel = GiTexel(origin + direction * next, projection, window, inside);
        if (!inside)
            return false; // the ray left the picture: no hit
        const float3 Q = ViewPositions.Load(int3(texel, 0)).xyz;
        if (!(GiBehind(origin + direction * next, Q) > 0.0))
        {
            previous = next;
            continue; // still in front of the surface (or over the sky): keep looking
        }
        // A crossing, between the last step that was in front and this one: halve the interval until the step turns into the surface's own
        // texel, then the thickness decides whether that texel is the surface the ray crossed or another object's wall.
        float loEdge = previous;
        float hiEdge = next;
        int2 refined = texel;
        [unroll]
        for (int r = 0; r < kMaxRefine; ++r)
        {
            const float mid = 0.5 * (loEdge + hiEdge);
            bool midInside = false;
            const int2 midTexel = GiTexel(origin + direction * mid, projection, window, midInside);
            if (!midInside)
                break;
            if (GiBehind(origin + direction * mid, ViewPositions.Load(int3(midTexel, 0)).xyz) > 0.0)
            {
                hiEdge = mid;
                refined = midTexel;
            }
            else
            {
                loEdge = mid;
            }
        }
        // The thickness at the refined point: a surface this far in front of the ray point is another object's wall, not the one crossed.
        hitTexel = refined;
        hitView = ViewPositions.Load(int3(refined, 0)).xyz;
        const float behind = GiBehind(origin + direction * hiEdge, hitView);
        found = behind > 0.0 && behind <= kThickness;
        return found;
    }
    return found;
}
