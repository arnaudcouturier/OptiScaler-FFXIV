// Gi.hlsl: the screen-space light bounce, composed onto a copy of the lit scene before the engine's tone map reads it - the tone-map chain's
// stage after the volumetric march.
//
// Three pixel shaders and one vertex shader, all driven by one constant buffer and no sampler (every read is a Load on an exact texel, so the
// result does not depend on filter state and a host test can predict it):
//   VS        (vs_5_0) one oversized triangle from SV_VertexID
//   PS_Gi     (ps_5_0) a half (or quarter) of the render window: the bounce irradiance of the frame - K cosine rays per pixel out of the
//             G-buffer normal's tangent basis, each ray marched against the composite's own view positions (this pass's own march file,
//             shaders/GiMarch.hlsl), a hit's radiance toward the origin read as the hit texel's own linear scene value (the scene's stored
//             texel squared: the engine stores the square root of its composed sum, the surface's own colour stays in), and the accumulated
//             estimate into an own RGBA16F target
//             (rgb = the irradiance estimate, a = the rays marched: above 0 for every origin that was traced, whether or not a ray hit, 0 for
//             an origin that could not be traced - the sky, a texel with no view position or no usable normal, a zero fade)
//   PS_Smooth (ps_5_0) the same extent: the irradiance's dense 5x5 binomial stencil (the 1-4-6-4-1 outer product, 25 taps on exact integer
//             positions, the texel's own and its eight nearest neighbours included), averaged over the taps that were traced (a traced texel
//             whose rays all missed is a valid zero and is averaged in; an untraceable one is skipped), a = 1 when any was
//   PS_Apply  (ps_5_0) the render window: the scene (this pass's own copy of what it was handed) plus the smoothed estimate multiplied by the
//             origin pixel's own albedo multiplier ((gb2.rgb)^2, the material pack's diffuse weight, the engine's special-row exclusion) times
//             the strength key - through the radial shafts' own white-bounded addition, so no key pushes a channel past the white the pass
//             reads
//
// Rules:
//  1. The engine allocates the frame's targets at the display size, draws a smaller window inside them, so the window - not the allocation -
//     is the extent this pass composes over, and the depth, the G-buffers, the view positions and the scene share one index space over it.
//  2. Nothing is written to the engine's textures. Every read of the scene is this pass's own copy; the game's own resources are read through
//     the views that were bound when the frame drew them.
//  3. A pixel that is sky keeps the scene's own value: bounce light lands on surfaces, never on the sky it comes from.
//  4. The term is a fixed function of the frame's buffers: no frame number, no random offset beyond the pixel's own hash, no history.
//  5. The added light is bounded by the frame's own white point: no key pushes a channel past it (the radial shafts' own addition, the same
//     expression their composite carries).
//  6. A ray that finds no hit, or whose hit this pass cannot read (a scene value that is not finite, or over the clamp), adds nothing: the
//     estimate divides by the rays the keys asked for, and the origin is still traced (a valid zero). An origin that cannot be traced at all
//     (the sky, no view position, no usable normal, a zero fade) carries no data, and the pixel keeps the scene's own value instead of a dark
//     or a bright one.
//
// The register slots and the constant-buffer layout are mirrored by GiPass.cpp; change them together. The rules header
// (ext/ffxiv/engine/GiRules.h) carries the same fixed numbers under the same names for the host tests: neither file may drift.

// The rules' own origin floor, the same constant the tests pin.
static const float kViewFadeOriginFloor = 1.0e-6;

#include "GiMarch.hlsl"

cbuffer Params : register(b0)
{
    float4 Window;   // xy = the render window's origin in scene pixels, zw = its extent in scene pixels
    float4 Tuning;   // x = rays, y = steps, w = the strength key times the pass's own gain (z unused)
    float4 Range;    // x = the march's reach in metres, y = the irradiance target's window scale (2 at half res, 4 at a quarter)
    float4 Bound;    // x = the frame's white point in the scene's units, y = the held state's fade
    float4 ViewRow0; // the engine's view matrix (g_CameraParameter +0x000): the world-space normal's view-space rows
    float4 ViewRow1;
    float4 ViewRow2;
    float4 ProjRow0; // the engine's projection (g_CameraParameter +0x120): the march's rays are projected through it
    float4 ProjRow1;
    float4 ProjRow2;
    float4 ProjRow3;
};

Texture2D<float> Depth : register(t0);       // the reversed-Z scene depth (1 at the far plane; the fog post's own capture of this frame's)
Texture2D<float4> ViewPos : register(t1);    // the composite's own view-position buffer (rgb = the view-space position; a zero on the sky)
Texture2D<float4> Normal : register(t2);     // G-buffer 0: the primary world-space normal as n * 0.5 + 0.5 in rgb (the albedo id byte in .a)
Texture2D<float4> Albedo : register(t3);     // G-buffer 2: the albedo carrier, gamma-2 encoded (the engine's own composite squares it); the apply reads it
Texture2D<float4> Scene : register(t4);      // this pass's own copy of the chain's current output (read only)
Texture2D<float4> Material : register(t5);   // G-buffer 1: the material pack (x the selector, z the diffuse weight)
Texture2D<float4> Irradiance : register(t6); // the pass's own irradiance, raw in the smooth, smoothed in the apply

// The engine's depth is reversed-Z: a pixel at the far plane is sky. A NaN or a negative value is not a depth this pass reads, and is a
// surface with no data - which for an origin pixel means no bounce from it (rule 6's own answer), never a glow.
static const float kSkyDepth = 1.0e-6;

float SkyOf(float depth)
{
    return (depth == depth && depth >= 0.0 && depth <= kSkyDepth) ? 1.0 : 0.0;
}

// The rules' fixed numbers, mirrored under the same names (neither file's value is re-derived; the host tests pin the pair):
static const float kMaxRadiance = 8.0;    // the largest linear radiance a hit may carry (above it the sample is refused)
static const float kNormalLengthLow = 0.5;    // a decoded length squared outside this band is not a surface (the occlusion pass's limits)
static const float kNormalLengthHigh = 1.5;
static const float kMaterialSpecularSelector = 1.0; // the material pack's own selector gate (at or above it another model owns the colour)
// The smooth's stencil: every position of the 5x5 square once, row by row, as exact integer offsets (an int2 table: a tap cannot land between
// texels, and nothing truncates it). The weight of a tap is the 1-4-6-4-1 row at its |dx| times the row at its |dy|, so the texel's own tap and
// its four direct and four diagonal neighbours are all in, and half of the total weight (128 of 256) sits on the taps with an odd x+y. That
// balance is the point: the stencil's response at the checkerboard frequency (and at the one-texel stripes) is exactly zero, so the two
// checkerboard parity classes of the irradiance are mixed and never filtered as two independent fields. (A stencil whose taps all have an
// even x+y does the opposite and prints a diamond lattice on every surface: the first version's did, its 1.5 offsets truncated to 1.)
// The rules header carries the same two tables under the same names; neither file may drift, and the host tests read both.
static const int kSmoothTaps = 25;
static const int2 kSmoothOffsets[kSmoothTaps] = {
    int2(-2, -2), int2(-1, -2), int2(0, -2), int2(1, -2), int2(2, -2),
    int2(-2, -1), int2(-1, -1), int2(0, -1), int2(1, -1), int2(2, -1),
    int2(-2, 0),  int2(-1, 0),  int2(0, 0),  int2(1, 0),  int2(2, 0),
    int2(-2, 1),  int2(-1, 1),  int2(0, 1),  int2(1, 1),  int2(2, 1),
    int2(-2, 2),  int2(-1, 2),  int2(0, 2),  int2(1, 2),  int2(2, 2),
};
static const float kSmoothWeights[kSmoothTaps] = {
     1.0,  4.0,  6.0,  4.0,  1.0,
     4.0, 16.0, 24.0, 16.0,  4.0,
     6.0, 24.0, 36.0, 24.0,  6.0,
     4.0, 16.0, 24.0, 16.0,  4.0,
     1.0,  4.0,  6.0,  4.0,  1.0,
};

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

// A per-pixel fraction, salted: the azimuth and the elevation of a pixel's rays are two different functions of the same pixel, so two
// neighbours' lattices cannot align. No frame number, no history, no sampler state - the march's own per-pixel hash, one salt in.
static const float kInverse24 = 1.0 / 16777216.0;
static const uint kHashSaltElevation = 0x9E3779B9u;

float Hash01(float2 pixel, uint salt)
{
    uint h = uint(pixel.x) * 1664525u + uint(pixel.y) * 1013904223u;
    h ^= salt;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return float(h & 0xFFFFFFu) * kInverse24;
}

// One hit's radiance toward the origin, one channel: the hit texel's own linear scene value - the scene's stored texel squared (the engine
// writes the square root of its composed sum and squares it back on read). What a surface sends away is its albedo times the light that reached
// it, which is already in that value, so nothing is divided out: a warm floor sends warm light, a dark one little, a lamp its own light.
// Negative, non-finite, or over the clamp: the hit carries nothing (the rules' BounceRadiance inlined on the same constant).
float GiBounceRadiance(float sceneStored)
{
    if (!(sceneStored >= 0.0) || sceneStored != sceneStored)
        return 0.0;
    const float radiance = sceneStored * sceneStored;
    if (!(radiance == radiance) || radiance <= 0.0 || radiance > kMaxRadiance)
        return 0.0;
    return radiance;
}

// The bounce irradiance of one window texel, in the scene's own light units: the rays, the march, the per-hit radiance, and the estimator's
// own hemisphere factor at the end. A texel that is not a surface (a zero view position, a normal the store does not carry) contributes
// nothing: no bounce from a texel the frame did not draw. `pairs` comes back as the rays this origin marched, found or missed (0 when it
// marched none), so the store's alpha marks where the estimate has data behind it: a traced texel with no hit carries rgb 0 and an alpha above
// 0 (a valid zero the smooth averages in), an untraceable origin carries alpha 0 (no data, skipped).
float3 GiIrradianceOf(int2 at, uint rays, uint steps, float range, float fade, out float pairs)
{
    pairs = 0.0; // every return path writes the out: an origin that is not traced carries 0 (the store's own "no data")
    const float3 originView = ViewPos.Load(int3(at, 0)).xyz;
    if (!(dot(originView, originView) > kViewFadeOriginFloor) || !(dot(originView, originView) == dot(originView, originView)))
        return float3(0.0, 0.0, 0.0);
    // The world normal, turned to view space: the engine wrote it world-side, and everything this pass runs is view space (the view
    // positions' own grid).
    const float3 world = Normal.Load(int3(at, 0)).xyz * 2.0 - 1.0;
    const float worldLengthSquared = dot(world, world);
    if (!(worldLengthSquared > kNormalLengthLow) || !(worldLengthSquared < kNormalLengthHigh))
        return float3(0.0, 0.0, 0.0);
    float3 n = mul(float3x3(ViewRow0.xyz, ViewRow1.xyz, ViewRow2.xyz), world);
    const float viewLengthSquared = dot(n, n);
    if (!(viewLengthSquared > 1.0e-6))
        return float3(0.0, 0.0, 0.0);
    n *= rsqrt(viewLengthSquared);
    // The camera is the view space's own origin: a normal pointing away from a view point faces away from the viewer.
    if (dot(n, originView) > 0.0)
        n = -n;
    // The tangent basis: the occlusion pass's own construction (the helper axis the normal is least aligned with, the cross with the normal,
    // their cross again, each normalized), so the two passes build one basis one way.
    const float3 helper = abs(n.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    const float3 b1 = normalize(cross(n, helper));
    const float3 b2 = cross(n, b1);
    const float3x3 basis3 = float3x3(b1, b2, n);
    // The view fade: a grazing pixel's bounce fades with its own camera-facing cosine (the rules' ViewFade). The `fade` the caller handed over
    // is the held state's own (the frame's); this one multiplies beside it.
    if (!(fade > 0.0))
        return float3(0.0, 0.0, 0.0);
    const float originLengthSquared = dot(originView, originView);
    const float viewFade = saturate(dot(n, -normalize(originView)));
    const float2 pixel = float2(at);
    const float azimuthHash = Hash01(pixel, 0);
    const float elevationHash = Hash01(pixel, kHashSaltElevation);
    float3 sum = float3(0.0, 0.0, 0.0);
    float tracedCount = 0.0;
    [loop]
    for (uint ray = 0; ray < max(rays, 1); ++ray)
    {
        // The direction, cosine-distributed, rotationally jittered per pixel (the rules' HemisphereDirection's own inline form): the hash
        // draws the elevation, the ray's own index plus the azimuth hash sets the angle around the basis.
        const float elevation = sqrt(saturate(elevationHash));
        if (!(elevation > 0.0))
            continue; // along the tangent plane: its own cosine is zero and the ray carries nothing by construction
        const float horizontal = sqrt(1.0 - elevation * elevation);
        const float angle = (float(ray) + azimuthHash) / float(max(rays, 1)) * 6.28318530717958647692;
        const float along1 = horizontal * cos(angle);
        const float along2 = horizontal * sin(angle);
        // The direction is the linear combination, stated explicitly: a mul(basis, v) would read the basis's ROWS as the components' own
        // dots (the ray's own axes sideways), which is what the compare against the reference march catches.
        const float3 direction = normalize(basis3[0] * along1 + basis3[1] * along2 + basis3[2] * elevation);
        const float cosine = dot(direction, basis3[2]);
        if (!(cosine > 0.0))
            continue;
        tracedCount += 1.0; // this ray is marched from here on, whether or not it finds a hit: the origin is traced
        bool found = false;
        int2 hitTexel = int2(0, 0);
        float3 hitView = float3(0.0, 0.0, 0.0);
        found = GiMarchFrom(ViewPos, originView, direction, float4x4(ProjRow0, ProjRow1, ProjRow2, ProjRow3), Window, int(steps), range,
                            hitTexel, hitView);
        if (!found || hitTexel.x < 0)
            continue; // a ray that missed carries nothing: it was traced, and it adds a zero to the average
        const float4 sceneHit = Scene.Load(int3(hitTexel, 0));
        float3 radiance = float3(0.0, 0.0, 0.0);
        radiance.x = GiBounceRadiance(sceneHit.x);
        radiance.y = GiBounceRadiance(sceneHit.y);
        radiance.z = GiBounceRadiance(sceneHit.z);
        // The estimator's per-ray term: the hit's radiance (a hit whose value was refused adds zero: the ray was traced and carries nothing). The rays are cosine-distributed, so their pdf is cos/pi, and the pdf's own w
        // against this term's cos is what folds into the pi factor the estimator applies at the end (the rules' IrradianceEstimate's own
        // bookkeeping; none of it is done twice here).
        sum += radiance;
    }
    if (!(tracedCount > 0.0))
        return float3(0.0, 0.0, 0.0); // no ray was marched: pairs is still 0.0, the store's "no data"
    // The hemisphere integral: the cosine pdf's own pi over the rays the keys asked for (not over the rays that hit it - a corner's shade
    // is the estimator's own answer, not a renormalization a miss washes out), with the pixel's own view fade and the frame's own beside it.
    // A sum of 0 (every ray missed) is a valid zero: the alpha says the origin was traced.
    pairs = tracedCount;
    return sum * (3.14159265358979323846 / float(max(rays, 1))) * viewFade * fade; // both writes agree
}


float4 PS_Gi(float4 position : SV_Position) : SV_Target0
{
    // The irradiance texel's own window pixel: the block of scene pixels this sub-resolution texel stands for, at its centre (the march's
    // own rule). An odd window extent makes the last texel's block reach past the window, and the read is clamped onto it.
    const float scale = Range.y > 0.0 ? Range.y : 2.0;
    float2 windowPixel = position.xy * scale;
    windowPixel = max(windowPixel, Window.xy);
    windowPixel = min(windowPixel, Window.xy + Window.zw - 1.0);
    const int2 at = int2(windowPixel);
    // The sky keeps nothing: the sky itself gets no bounce light (rule 3), and a sky pixel's rays would march nothing but its own sky.
    const float depth = Depth.Load(int3(at, 0));
    if (SkyOf(depth) > 0.5)
        return float4(0.0, 0.0, 0.0, 0.0);
    float pairs = 0.0;
    const float3 irradiance = GiIrradianceOf(at, uint(max(Tuning.x, 1.0)), uint(max(Tuning.y, 1.0)), max(Range.x, 0.0), Bound.y, pairs);
    return float4(irradiance, pairs); // the alpha is the rays marched (the data marker: above 0 for a traced origin, 0 for an untraceable one)
}

float4 PS_Smooth(float4 position : SV_Position) : SV_Target0
{
    const int2 at = int2(position.xy);
    float2 targetSize = float2(1.0, 1.0);
    Irradiance.GetDimensions(targetSize.x, targetSize.y);
    const int2 last = int2(max(targetSize.x - 1.0, 0.0), max(targetSize.y - 1.0, 0.0));
    // The stencil: the texel's own tap and its 24 neighbours, each at an exact integer offset from the table. A tap that has no data (the
    // store's alpha is the data marker: 0 for an origin that could not be traced) is skipped and the sum is divided by the weights that were
    // present; a traced texel whose rays all missed has an alpha above 0 and a colour of 0, and is averaged in as the zero it is. A tap outside
    // the extent is skipped as well, never clamped onto the edge (the occlusion pass's own edge rule).
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    [unroll]
    for (int tap = 0; tap < kSmoothTaps; ++tap)
    {
        const int2 tapAt = at + kSmoothOffsets[tap];
        if (tapAt.x < 0 || tapAt.y < 0 || tapAt.x > last.x || tapAt.y > last.y)
            continue; // outside the extent: skipped, never clamped (the occlusion pass's own edge rule)
        const float4 value = Irradiance.Load(int3(tapAt, 0));
        if (!(value.a > 0.0) || value.a != value.a)
            continue;
        const float w = kSmoothWeights[tap];
        sum += value.rgb * w;
        weight += w;
    }
    if (!(weight > 0.0))
        return float4(0.0, 0.0, 0.0, 0.0);
    return float4(sum / weight, 1.0);
}

float4 PS_Apply(float4 position : SV_Position) : SV_Target0
{
    const int2 at = int2(position.xy);
    const float4 scene = Scene.Load(int3(at, 0));
    // The sky keeps its own value (rule 3).
    const float depth = Depth.Load(int3(at, 0));
    if (SkyOf(depth) > 0.5)
        return scene;
    if (!(Tuning.w > 0.0))
        return scene; // the strength key is 0: this frame is its own copy, byte for byte (the march's identity rule)
    // The origin's own albedo multiplier and its gates: the (gb2.rgb)^2 the engine's own composite multiplies, times the material pack's
    // diffuse share, refused on the engine's special rows (the id bytes the light-draw band test names) and on the special-colour selector.
    const float4 albedo = Albedo.Load(int3(at, 0));
    const float4 material = Material.Load(int3(at, 0));
    const float materialId = floor(Normal.Load(int3(at, 0)).a * 255.0 + 0.5);
    float gate = 0.0;
    const bool plain = !(materialId >= 32.0 && materialId <= 44.0) && materialId != 224.0 && material.x == material.x &&
                       material.z == material.z && material.x < kMaterialSpecularSelector;
    if (plain)
    {
        const float diffuseShare = saturate(1.0 - material.z);
        if (diffuseShare > 0.0)
            gate = diffuseShare;
    }
    const float3 albedoMultipliers = albedo.rgb * albedo.rgb * gate;
    if (dot(albedoMultipliers, albedoMultipliers) <= 0.0 || !(albedo.x == albedo.x))
        return scene;
    // The smoothed estimate, read bilinearly between the four texels whose centres surround this pixel's sub-resolution coordinate (the
    // march's own four-load read, at this pass's own scale).
    float2 targetSize = float2(1.0, 1.0);
    Irradiance.GetDimensions(targetSize.x, targetSize.y);
    const int2 lastTerm = int2(max(targetSize.x - 1.0, 0.0), max(targetSize.y - 1.0, 0.0));
    const float scale = Range.y > 0.0 ? Range.y : 2.0;
    const float2 coordinate = (position.xy - Window.xy) / scale - 0.5;
    const int2 base = int2(floor(coordinate));
    const float2 fraction = saturate(coordinate - float2(base));
    const float4 topLeft = Irradiance.Load(int3(clamp(base, int2(0, 0), lastTerm), 0));
    const float4 topRight = Irradiance.Load(int3(clamp(base + int2(1, 0), int2(0, 0), lastTerm), 0));
    const float4 bottomLeft = Irradiance.Load(int3(clamp(base + int2(0, 1), int2(0, 0), lastTerm), 0));
    const float4 bottomRight = Irradiance.Load(int3(clamp(base + int2(1, 1), int2(0, 0), lastTerm), 0));
    const float3 top = lerp(topLeft.rgb, topRight.rgb, fraction.x);
    const float3 bottom = lerp(bottomLeft.rgb, bottomRight.rgb, fraction.x);
    const float3 estimate = max(lerp(top, bottom, fraction.y), float3(0.0, 0.0, 0.0));
    if (dot(estimate, estimate) <= 0.0)
        return scene;
    // The light this pixel gets, in fractions of the frame's white: the smoothed estimate, the pixel's own albedo multiplier, the strength
    // key times the pass's own gain, and the held state's fade. The addition is bounded by how much room the pixel has left to the white
    // (head = white - scene; add = head*raw/(raw+head), where raw = light * white), so the sum is always strictly below the white point -
    // no key, no estimate and no fade can push a channel past it, and a pixel already at white gets nothing (the radial's own addition,
    // whose tests pin the bound).
    // The light is LINEAR (the composition's squared domain) and the scene is stored as its square root, so the stored value the sum gives is
    // sqrt(scene^2 + light) (the rules' StoredIncrement); that increment goes through the bounded addition.
    const float3 light = estimate * albedoMultipliers * Tuning.w;
    const float3 sceneStored = max(scene.rgb, float3(0.0, 0.0, 0.0));
    const float3 increment = sqrt(sceneStored * sceneStored + light) - sceneStored;
    const float3 head = max(Bound.x.xxx - sceneStored, float3(0.0, 0.0, 0.0));
    const float3 added = head * increment / max(increment + head, float3(1.0e-6, 1.0e-6, 1.0e-6));
    float4 outColor = scene;
    outColor.rgb += added;
    return outColor;
}
