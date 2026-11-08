// The screen-space march the water reflections and the puddles' mirror share.
//
// One function, no sampler and no state of its own: every read is a Load on an exact texel of a view-position buffer, so the result does not
// depend on filter state, a host test can predict it, and two passes that call it march the same way. The projection, the window and the
// allocation are the caller's: the water map is a fraction of the scene allocation and the puddles' mirror is the scene itself, and both march the
// same buffer.
//
// What it does, in the order both callers state it:
//  1. the ray starts a small distance from the surface (a ray that starts on it reads its own pixel) and steps outward on a quadratic ramp over
//     the range, so the near field - where a reflection is sharpest - gets most of the taps while the far steps stay short enough for the
//     thickness test below to decide them. A step that leaves the render window ends the march with no hit.
//  2. a hit is a step whose sampled surface lies behind the ray point along the ray, and not farther behind it than `thickness`: a wall the ray
//     passed a metre ago is another object, not the surface the ray crossed. The interval the crossing sits in is halved six times, which turns
//     a step into the surface's own texel; the texel the ray crosses is where the reflected colour comes from.
//  3. the answer is that texel, or `found = false`. What a miss means is the caller's: the water's map keeps the engine's own reflection, and the
//     puddle keeps its darkened lit colour - both fail open, and neither can turn into a black hole.
//  4. the reason the march ended and the distance along the ray a hit sits at come back as two out parameters. The host test that reads them
//     compiles the caller with SSR_MARCH_DIAG defined and defines the two macros the march writes the REASON with (below); the product's build
//     never defines that name, so the reason costs the product nothing - but the distance is written unconditionally now, because the callers read
//     it (the water reflection's self-graze rule).

// The march's own exit reasons, written by the macros the host test defines: 11 the ray left the picture, 12 the ray reached the distance limit,
// 13 the steps ran out without a crossing, 14 a crossing whose surface is farther behind the ray than the thickness. The test that reads them
// (tests/ext/ffxiv/engine/prepare_ssr_frame.ps1) compiles the water reflection with SSR_MARCH_DIAG defined, and its pixel shader defines the two
// macros before this file is included. The product's build never defines that name (ssr.shaders.props), so the two macros expand to nothing and
// the march compiled for the game is this function with the diagnostics removed - the reason and the distance are out parameters the caller does
// not read, which is why they cost the product nothing.
#ifndef SSR_REASON
#define SSR_REASON(code)
#endif
#ifndef SSR_DIAG_DISTANCE
#define SSR_DIAG_DISTANCE(value)
#endif

// The march's fixed shape. The step count is the caller's key; these are not, so the picture cannot drift with a number an ini does not name.
//
// The distance schedule is quadratic in the step index: `next = kSurfaceBias + range * u * u` with `u = (k + 1) / steps`. A geometric ramp grew a
// 1.45x per step, which at 48 steps made the first thirty taps shorter than five centimetres (thirty reads of the starting pixel, nine wasted) and
// the last steps 15 to 31 metres long - a step a 0.30 m thickness can never confirm after four halvings, which is why the reflection flickered
// as the camera turned. The quadratic ramp at the same count ends at 4.1 m and refines to 6.4 cm, and the near forty centimetres still get a tap.
static const float kSurfaceBias = 0.02;   // metres along the normal before the first tap
static const float kMaxDistance = 100.0;  // metres: the march stops here even if the ray has not left the picture
static const int kMaxSteps = 128;         // the hard bound the loop unrolls to at the highest step count
static const int kMaxRefine = 6;          // halvings of the interval the crossing sits in: a 4.1 m step becomes 6.4 cm

// The window-space scene coordinate a view-space point projects to (texels, not floored): the march's texel test
// reads the integer part of it, and the bilinear colour read weights the fraction. `inside` says whether the point
// projects into the render window at all (behind the camera and outside the picture both answer false).
float2 SceneMarchProject(float3 viewPoint, float4x4 projection, float4 window, out bool inside)
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

// The scene texel a view-space point would be drawn at, as the sampled texel's index, or a negative index when it is outside the render window.
// `inside` says whether the answer is a texel of the picture at all.
int2 SceneMarchTexel(Texture2D<float4> positions, float3 viewPoint, float4x4 projection, float4 window, float2 allocation, out bool inside)
{
    inside = false;
    const float2 projected = SceneMarchProject(viewPoint, projection, window, inside);
    if (!inside)
        return int2(-1, -1); // behind the camera or the ray left the picture
    const int2 texel = int2(floor(projected));
    if (texel.x < 0 || texel.y < 0 || float(texel.x) >= allocation.x || float(texel.y) >= allocation.y)
        return int2(-1, -1);
    inside = true;
    return texel;
}

// The scene's colour at the march's crossing, at the crossing's own fraction: the four texels the point sits between,
// weighted by the fractional part (a manual bilinear of exact Loads, so the answer does not depend on filter state
// and a host test can predict it). A per-frame sub-texel slide of the crossing - the upscaler's jitter inside the
// projection - then moves these weights continuously instead of flipping whole texels, which is what made the water's
// reflection re-roll about a third of its texels every frame. The taps stay inside the render window: the scene copy
// holds the window's own texels only.
float3 SceneMarchColour(Texture2D<float4> scene, float2 hitWindow, float4 window)
{
    const float2 at = hitWindow - 0.5;
    const float2 base = floor(at);
    const float2 weight = at - base;
    const int2 low = int2(window.xy);
    const int2 high = low + int2(window.zw) - 1; // the window's last texel index
    const int2 i0 = clamp(int2(base), low, high);
    const int2 i1 = min(i0 + 1, high);
    const float w00 = (1.0 - weight.x) * (1.0 - weight.y);
    const float w10 = weight.x * (1.0 - weight.y);
    const float w01 = (1.0 - weight.x) * weight.y;
    const float w11 = weight.x * weight.y;
    return w00 * Scene.Load(int3(i0, 0)).rgb + w10 * Scene.Load(int3(int2(i1.x, i0.y), 0)).rgb +
           w01 * Scene.Load(int3(int2(i0.x, i1.y), 0)).rgb + w11 * Scene.Load(int3(i1, 0)).rgb;
}

// The march itself: `origin` is the reflecting point in view space, `direction` the unit direction the ray leaves it in, `steps` the caller's
// count and `thickness` the surface tolerance in metres. `hitTexel` is the texel the ray crossed, or (-1, -1) when it found nothing, which is
// what the return value says as well. `reason` is the code above that says how the march ended, and `hitDistance` the distance along the ray the
// hit sits at; both are the diagnostic output's, and the product's build drops them.
bool MarchScene(Texture2D<float4> positions, float3 origin, float3 direction, float4x4 projection, float4 window, float2 allocation,
                int steps, float thickness, out int2 hitTexel, out float2 hitWindow, out int reason, out float hitDistance)
{
    bool found = false;
    hitTexel = int2(-1, -1);
    hitWindow = float2(0.0, 0.0);
    reason = 13; // the loop ran its steps out without a crossing; the breaks below name their own
    hitDistance = 0.0;

    // The quadratic ramp over the range: the first step is a fraction of the whole range squared, the last one is the range itself. `t` is the
    // previous step, so the interval the crossing sits in is always the two taps the loop just took.
    const float range = kMaxDistance - kSurfaceBias;

    float t = kSurfaceBias;
    float previous = 0.0; // the last step that had not crossed anything yet, in metres along the ray
    bool haveFront = false;
    [loop]
    for (int k = 0; k < steps; ++k)
    {
        const float u = float(k + 1) / float(steps);
        const float next = kSurfaceBias + range * u * u;
        if (next > kMaxDistance)
        {
            SSR_REASON(12);
            break;
        }
        bool inside = false;
        const int2 texel = SceneMarchTexel(positions, origin + direction * next, projection, window, allocation, inside);
        if (!inside)
        {
            SSR_REASON(11); // the ray left the picture: no hit
            break;
        }
        const float3 Q = positions.Load(int3(texel, 0)).xyz;
        if (!(dot(Q, Q) > 1e-6))
        {
            previous = next;
            haveFront = false;
            t = next;
            continue; // sky along the ray: nothing crossed yet
        }
        const float crossed = dot(Q - origin, direction); // how far along the ray the sampled surface is
        if (crossed <= 0.0)
        {
            previous = next;
            haveFront = false;
            t = next;
            continue; // nothing of the scene in front of the ray point here: keep looking
        }
        if (crossed >= next)
        {
            previous = next;
            haveFront = true;
            t = next;
            continue; // the surface is farther along than this step: the ray is still in front of it
        }
        // A crossing, between the last step that had not crossed anything and this one. Halving the interval six times turns a step into the
        // surface's own texel; a step longer than the thickness heuristic's tolerance is why the crossing is the test and the refined gap is the
        // thickness's.
        float loEdge = haveFront && previous > 0.0 ? previous : max(t, 0.0);
        float hiEdge = next;
        int2 refined = texel;
        [unroll]
        for (int r = 0; r < kMaxRefine; ++r)
        {
            const float mid = 0.5 * (loEdge + hiEdge);
            bool midInside = false;
            const int2 midTexel = SceneMarchTexel(positions, origin + direction * mid, projection, window, allocation, midInside);
            if (!midInside)
                break;
            const float midCrossed = dot(positions.Load(int3(midTexel, 0)).xyz - origin, direction);
            if (midCrossed > 0.0 && midCrossed <= mid)
            {
                hiEdge = mid;
                refined = midTexel;
            }
            else
            {
                loEdge = mid;
            }
        }
        // The thickness heuristic at the refined point: a surface farther behind the crossing than this is another object, not the one the ray
        // crossed. The crossing's own window coordinate goes back with the texel either way: the caller's colour read weights the fraction, so a
        // sub-texel slide of the crossing moves the weights instead of flipping the texel.
        bool hitInside = false;
        const float2 hitProjected = SceneMarchProject(origin + direction * hiEdge, projection, window, hitInside);
        hitWindow = hitInside ? hitProjected : float2(refined) + 0.5; // the refine's last inside texel's centre, when the crossing left the picture
        const float gap = hiEdge - dot(positions.Load(int3(refined, 0)).xyz - origin, direction);
        if (gap <= thickness)
        {
            hitTexel = refined;
            hitDistance = hiEdge; // the product's caller reads it (the self-graze rule); the diagnostic's green channel carries the same value
            found = true;
        }
        else
        {
            SSR_REASON(14); // a crossing whose surface is farther behind the ray than the thickness: another object
        }
        break;
    }
    return found;
}
