// Godrays.hlsl: the volumetric god rays, added to a copy of the lit scene before the engine's tone map reads it.
//
// Three pixel shaders, one compute shader and one vertex shader, all driven by one constant buffer and no sampler (every read is a Load on an exact
// texel, so the result does not depend on filter state and a test can predict it):
//   VS            (vs_5_0) one oversized triangle from SV_VertexID
//   PS_March      (ps_5_0) a quarter (or half) of the render window: the per-pixel in-scatter integral along the view ray, each step's visibility
//                 read from the engine's own cascade shadow atlas (all four cascades, each only inside its own atlas slice, the minimum), the
//                 medium from the engine's own live fog profile times a density boost, the phase a Henyey-Greenstein lobe around the shadow light
//                 - one float, S = sum sigma * phase * V * T * dt
//   CS_Stats      (cs_5_0) one group: the 99th percentile per channel of the scene's own sky pixels (a 1/16 sample) and the march term's peak
//   PS_SunColour  (ps_5_0) one texel: the sky's colour around the sun's screen position, which is the colour the march takes when the
//                 engine reports none (the radial shafts' scan-and-sky rule)
//   PS_Composite  (ps_5_0) the render window: the scene plus the march's term, depth-aware upsampled, tinted and scaled through the same
//                 white-bounded addition the radial's composite runs, the sky capped at its own ceiling
//
// This file is the march's own: it shares no math with SceneMarch.hlsl (the SSR and puddle-mirror surface-crossing march, another
// module's file) and nothing here edits the radial's LightShafts.hlsl - the per-pixel hash below is copied from it with its origin named,
// and the composite's addition is the same ShaftAdded expression ShaftsRules.h states and the radial's composite carries.
//
// The engine facts the march rests on (PS-ROADMAP-FOG.md, GODRAYS-S0-DIAG-ATLAS.md, the mask writer's own disassembly, and the 2026-11-07 RE report):
//  - the pixel's view-space position is reconstructed through the engine's inverse projection (g_CameraParameter +0x0E0), the same form
//    the mask writer and the engine's fog post use; a sky pixel (reversed Z, no far plane) has w = 0 there and only a direction;
//  - the cascade matrix maps that view-space point into the atlas' uv and depth (each cascade's v row carries its own slice offset), and
//    the compare is `shadowed when the sample's depth is at or beyond the gathered texel` - the writer's own `ge`; an air sample has no
//    surface normal, so it carries a small constant bias instead of the writer's normal offset;
//  - the engine's mask writer blends the cascades with MIN, each only effective inside its own distance band (the fade rows), so a sample
//    is looked up in EVERY cascade, each only inside its own atlas slice, and the minimum is the visibility (the first march's "first cascade
//    whose uv is inside [0,1]^2" owned far samples with a near cascade and hid the beams the far cascades cast);
//  - a disc tap's depth is pulled toward the camera by the tap's radius times the shared map row's constant (cb3[7].x * -15), which is
//    what keeps the writer's tilted disc comparing against the depths the light sees at the taps' own positions;
//  - the edge fades and the distance ramp run on the sample's view z through the cascade's own fade and distance rows, so the march
//    inherits the engine's own band edges;
//  - the density profile is the engine's own: rho(y) = d1*exp(f1*(h1-y)*0.545968) + d2*exp(f2*(h2-y)*0.545968), normalized by the
//    density at the camera's own height; the real density (0.0009 per metre there) is too thin for a visible beam, so it is boosted by the
//    density key (20 x 0.001 per metre), an artistic constant, with extinction.
//
// Rules:
//  1. The engine allocates the frame's targets at the display size, draws a smaller window inside them and starts that window at a
//     half-pixel origin, so SV_Position.xy is already the render target's pixel (k + 0.5) whatever the viewport's origin, and a
//     sub-resolution texel k of the window stands for the window's own scene pixels s*k .. s*k+s-1 (s = the resolution's scale).
//  2. Nothing is written to the engine's textures. PS_Composite reads the scene it was handed and writes this pass's own copy of it.
//  3. The sky gets light too (the air in front of it is where beams are seen), but each of its channels is capped at the scene's own sky
//     99th percentile, estimated from a 1/16 sample of the scene's sky pixels by CS_Stats; without enough sky samples the sky is left alone.
//     The engine's sSky texture is never read.
//  4. The term is a fixed function of the frame's buffers: no frame number, no random offset, no history. The march's samples are offset
//     per pixel by a hash of the pixel's own coordinate, which is still a function of the frame alone.
//  5. The added light is measured against the frame's own white point, and the composite never adds more than the pixel's remaining room
//     to that white: no intensity key can push a channel past it, so no key can burn the frame.
//  6. A sample the march cannot read (a NaN texel, a depth the view refuses) is a shadowed sample, not a lit one: a broken atlas takes
//     beams away instead of filling the frame with glow.
//
// The register slots and the constant-buffer layout are mirrored by GodraysPass.cpp; change them together.

cbuffer Params : register(b0)
{
    float4 Window;        // xy = the render window's origin in scene pixels, zw = its extent in scene pixels
    float4 RayScale;      // xy = the pixel-to-NDC scale (derived from the render window), zw = its offset
    float4 InvProjRow0;   // the inverse projection's rows (g_CameraParameter +0x0E0): the ray's reconstruction
    float4 InvProjRow1;
    float4 InvProjRow2;
    float4 InvProjRow3;
    float4 Camera;        // xyz = the inverse view's second row (the world y of a view point), w = the camera's own world height
    float4 LightDir;      // xyz = the shadow light's direction (view space, toward the sun), w = the march's far bound in view metres
    float4 FogA;          // x = density1, y = fallOff1, z = refHeight1, w = 1 / rho(cameraWorldY): the term's normalization
    float4 FogB;          // x = density2, y = fallOff2, z = refHeight2, w = the fog boost
    float4 FogC;          // x = the height lift, y = the phase boost, z = the engine's DI exponent (kept for the log), w = 0
    float4 Tuning;        // x = steps, y = softness (0 point, 1 two taps, 2 four), z = jitter (0 off / 1 on), w = the resolution's scale (2 or 4)
    float4 Colour;        // xyz = the tint (a colour of peak 1, the auto and white modes), w = the colour mode (0 tint, 1 the sun's texel, 2 enginefog)
    float4 Bound;         // x = the frame's white point in scene units, y = the held state's fade, z = intensity * the composite gain * the light's gain, w = the sky ceiling (1 on / 0 off)
    float4 SunScreen;     // xy = the sun's screen position in scene pixels (the auto colour's search centre), zw = 0
    float4 FogColour;     // xyz = the engine's own fog colour (cFogParam[0]), w = 0
    float4 FogLut;        // x = the LUT's u param A, y = its B (u = A + B / worldDist), z = the depth-to-distance A, w = its B
    float4 Cascade0Row0;  // the first cascade in the march's own order (finest first): the light-space matrix's rows
    float4 Cascade0Row1;
    float4 Cascade0Row2;
    float4 Cascade0Row3;
    float4 Cascade0Fade;  // the writer's cb3[6]: the two edge fades' bounds and scale, on the sample's view z
    float4 Cascade0Ramp;  // x = the distance row's far (cb3[4].z), y = the ramp's scale (cb3[5].z), z = the ramp's target (cb3[5].w), w = the atlas slice (0..3; 4 = none)
    float4 Cascade1Row0;  // and the same five rows for the second, third and fourth cascade
    float4 Cascade1Row1;
    float4 Cascade1Row2;
    float4 Cascade1Row3;
    float4 Cascade1Fade;
    float4 Cascade1Ramp;
    float4 Cascade2Row0;
    float4 Cascade2Row1;
    float4 Cascade2Row2;
    float4 Cascade2Row3;
    float4 Cascade2Fade;
    float4 Cascade2Ramp;
    float4 Cascade3Row0;
    float4 Cascade3Row1;
    float4 Cascade3Row2;
    float4 Cascade3Row3;
    float4 Cascade3Fade;
    float4 Cascade3Ramp;
    float4 AtlasParam;    // xy = the atlas' texel size in uv units (the writer's own resinfo arithmetic), z = the disc scale, w = the air sample's depth bias
    float4 Medium;        // x = the density key (units of 0.001 per metre), y = the Henyey-Greenstein g in effect, zw = 0
    float4 Upsample;      // x = the bilateral upsample's relative depth sigma (0 = the plain bilinear read), yzw = 0
};

Texture2D<float> Depth : register(t0);
Texture2D<float> Atlas : register(t1);
Texture2D<float4> Scene : register(t2);
Texture2D<float> Term : register(t3);
Texture2D<float4> Sky : register(t4);
Texture2D<float2> Lut : register(t5);
Texture2D<float4> SunTint : register(t6);
Texture2D<float4> SkyCeil : register(t7);
RWTexture2D<float4> Stats : register(u0);

// The engine's depth is reversed-Z: a pixel at the far plane is sky, and it is where the light comes from. A cannot-see value (a NaN, a
// read the depth view refuses) is a surface, so a broken depth adds no light instead of filling the frame with glow.
static const float kSkyDepth = 1.0e-6;

// The march term's clamp: the captured fog constants' absolute scale is unmeasured (the FOG-F9 capture owns it), and a runaway density
// read must not wash the frame. The composite's own bound below is the real guarantee; this keeps the term in the range the gain constant
// was chosen for. (GodraysRules.h kMaxTerm carries the same number for the host tests.)
static const float kMaxTerm = 4.0;

// GodraysRules.h kSigmaPerMetre, kAtlasSlices, kSkyBins, kMinSkySamples, kSkyCeilingMargin, kSkyPercentile, kFarDistance: the same numbers.
static const float kSigmaPerMetre = 0.001;
static const uint kSkyBins = 128;
static const uint kMinSkySamples = 64;
static const float kSkyCeilingMargin = 0.98;
static const float kSkyPercentile = 0.99;
static const float kFarDistance = 1.0e6;

float SkyOf(float depth)
{
    return (depth == depth && depth >= 0.0 && depth <= kSkyDepth) ? 1.0 : 0.0;
}

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

// A per-pixel fraction of a step, from the pixel's own coordinate: an integer hash, so a still frame stays still (no frame number, no
// history, no sampler state) and two runs of the same frame are one picture. Copied verbatim from LightShafts.hlsl 93-103 - the radial
// shafts' own jitter, which this march integrates the same way; the file itself is not edited, so the copy carries its origin with it.
static const float kInverse24 = 1.0 / 16777216.0;
float Hash01(float2 pixel)
{
    uint h = uint(pixel.x) * 1664525u + uint(pixel.y) * 1013904223u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return float(h & 0xFFFFFFu) * kInverse24;
}

// The window pixel a march texel stands for: the block's centre (rule 1), clamped into the window. The march and the upsample use the one function, so
// the depth the upsample weighs a texel with is the depth that texel was marched to.
float2 MarchWindowPixel(float2 texelCentre)
{
    float2 windowPixel = texelCentre * Tuning.w;
    windowPixel = max(windowPixel, Window.xy);
    return min(windowPixel, Window.xy + Window.zw - 1.0);
}

int2 DepthPixelOf(float2 windowPixel)
{
    uint depthWidth = 0;
    uint depthHeight = 0;
    Depth.GetDimensions(depthWidth, depthHeight);
    const int2 depthLast = max(int2(depthWidth, depthHeight) - int2(1, 1), int2(0, 0));
    return min(int2(windowPixel), depthLast);
}

// The linear view distance of a depth value (GodraysRules.h LinearDistanceOf): sky and degenerate values stand for a far distance, so the upsample's
// weights never mix sky with a surface.
float LinearDistance(float depth)
{
    if (!(depth > kSkyDepth))
        return kFarDistance;
    const float z = InvProjRow2.z * depth + InvProjRow2.w;
    const float w = InvProjRow3.z * depth + InvProjRow3.w;
    if (!(abs(w) > 1.0e-20))
        return kFarDistance;
    const float measured = abs(z / w);
    return measured == measured ? measured : kFarDistance;
}

// The engine's own density profile at a world height, with the height falloffs scaled by the lift key (1.0 = the engine's own profile).
// The 0.545968 literal is the engine's own; the exponent's sign convention is the fog post's own reading of its height rows.
float FogDensityAt(float worldY)
{
    const float lift = FogC.x;
    const float e1 = exp(FogA.y * (FogA.z - worldY) * 0.545968 * lift);
    const float e2 = exp(FogB.y * (FogB.z - worldY) * 0.545968 * lift);
    return FogA.x * e1 + FogB.x * e2;
}

// Henyey-Greenstein normalised to 1 at the light: (1 - g)^3 / (1 + g^2 - 2 g cos)^1.5 (GodraysRules.h HenyeyGreenstein).
float HenyeyGreenstein(float cosTheta, float g)
{
    const float c = clamp(cosTheta, -1.0, 1.0);
    const float denominator = (1.0 - g) * (1.0 - g) + 2.0 * g * (1.0 - c);
    const float cube = (1.0 - g) * (1.0 - g) * (1.0 - g);
    return cube / pow(max(denominator, 1.0e-6), 1.5);
}

// A sample belongs to a cascade only inside that cascade's slice of the atlas: v in [k/4, (k+1)/4).
bool InSlice(float v, float slice)
{
    if (!(slice >= 0.0 && slice < 4.0) || !(v == v))
        return false;
    return v >= slice * 0.25 && v < (slice + 1.0) * 0.25;
}

// One softness tap's compare, in the writer's own disc form: the tap's uv offset is its disc-unit position scaled by the atlas' texel size
// and the shared map row's constant, and the tap's depth is pulled toward the camera by the tap's radius times the same constant, then by the air
// bias. A point compare is the radius-zero tap, so one code serves all three softness settings. The read is clamped onto the atlas (a tap that reaches
// past a cascade's edge compares against the edge's own texel, the way the engine's own clamped sampler would).
float ShadowVisibility(float u, float v, float z, float2 texelSize, float discScale, float2 tap, float2 atlasSize)
{
    const float tapU = u + texelSize.x * tap.x * discScale;
    const float tapV = v + texelSize.y * tap.y * discScale;
    const float radius = sqrt(tap.x * tap.x + tap.y * tap.y);
    const float tapZ = z - radius * discScale - AtlasParam.w;
    const int2 at = int2(min(max(float2(tapU, tapV), float2(0.0, 0.0)), float2(1.0, 1.0)) * atlasSize - 0.5);
    const float gathered = Atlas.Load(int3(at, 0));
    // Shadowed when the tap's depth is at or beyond the gathered texel (the writer's own ge); a NaN texel is a shadowed sample, never a
    // lit one (rule 6).
    const bool shadowed = !(gathered == gathered) || !(tapZ == tapZ) || tapZ >= gathered;
    return shadowed ? 0.0 : 1.0;
}

// The sample's visibility against one cascade: the light-space projection, the slice containment test, the softness taps and the writer's own
// edge fades and distance ramp. A cascade that does not contain the sample (outside [0,1] across, outside its own slice, no slice, behind its
// light) says 1: it casts no shadow there, and the min over the four cascades is the visibility.
float CascadeVisibility(float3 p, float4 row0, float4 row1, float4 row2, float4 row3, float4 fade, float4 ramp, float2 texelSize,
                        float discScale, float softness, float2 atlasSize)
{
    const float4 clip = float4(dot(row0, float4(p, 1.0)), dot(row1, float4(p, 1.0)), dot(row2, float4(p, 1.0)), dot(row3, float4(p, 1.0)));
    if (!(clip.w > 1.0e-6) || !(clip.w == clip.w))
        return 1.0;
    const float3 uvz = clip.xyz / clip.w;
    if (!(uvz.x >= 0.0 && uvz.x <= 1.0 && uvz.y >= 0.0 && uvz.y <= 1.0) || !InSlice(uvz.y, ramp.w))
        return 1.0;
    float shadowed = 0.0;
    float taps = 0.0;
    if (softness < 0.5)
    {
        shadowed += 1.0 - ShadowVisibility(uvz.x, uvz.y, uvz.z, texelSize, discScale, float2(0.0, 0.0), atlasSize);
        taps += 1.0;
    }
    else if (softness < 1.5)
    {
        shadowed += 1.0 - ShadowVisibility(uvz.x, uvz.y, uvz.z, texelSize, discScale, float2(0.5, 0.5), atlasSize);
        shadowed += 1.0 - ShadowVisibility(uvz.x, uvz.y, uvz.z, texelSize, discScale, float2(-0.5, -0.5), atlasSize);
        taps += 2.0;
    }
    else
    {
        shadowed += 1.0 - ShadowVisibility(uvz.x, uvz.y, uvz.z, texelSize, discScale, float2(0.5, 0.5), atlasSize);
        shadowed += 1.0 - ShadowVisibility(uvz.x, uvz.y, uvz.z, texelSize, discScale, float2(-0.5, 0.5), atlasSize);
        shadowed += 1.0 - ShadowVisibility(uvz.x, uvz.y, uvz.z, texelSize, discScale, float2(0.5, -0.5), atlasSize);
        shadowed += 1.0 - ShadowVisibility(uvz.x, uvz.y, uvz.z, texelSize, discScale, float2(-0.5, -0.5), atlasSize);
        taps += 4.0;
    }
    const float shadow = 1.0 - shadowed / taps;
    // The writer's own tail: the two edge fades on the sample's view z, then the distance ramp that lights the region past the cascade's
    // own reach. With the captured constants these are the band edges - the march inherits them by carrying the same rows.
    const float fadeA = saturate((-p.z - fade.x) * fade.z);
    const float fadeB = saturate((p.z + fade.y) * fade.z);
    const float edge = min(fadeA, fadeB);
    const float shadowEdged = 1.0 - (1.0 - shadow) * edge;
    const float distFade = 1.0 - saturate((p.z + ramp.x) * ramp.y);
    return shadowEdged + distFade * distFade * (ramp.z - shadowEdged);
}

float PS_March(float4 position : SV_Position) : SV_Target0
{
    // The march pixel's own window pixel (rule 1). The ray: the engine's own reconstruction (the mask writer's and the fog post's own
    // inverse-projection form), from the camera's own position (the view origin) through the pixel's surface.
    const float2 windowPixel = MarchWindowPixel(position.xy);
    const float2 ndc = windowPixel * RayScale.xy + RayScale.zw;
    const float depth = Depth.Load(int3(DepthPixelOf(windowPixel), 0));
    const float4 clip = float4(ndc, depth, 1.0);
    const float4 view4 = float4(dot(InvProjRow0, clip), dot(InvProjRow1, clip), dot(InvProjRow2, clip), dot(InvProjRow3, clip));
    if (!(view4.w == view4.w))
        return 0.0; // a camera this frame cannot reconstruct: no light from it (rule 6's own answer for the ray)
    float3 dir;
    float rayLength;
    if (abs(view4.w) > 1.0e-20)
    {
        const float3 view = view4.xyz / view4.w;
        rayLength = length(view);
        if (!(rayLength > 1.0e-6) || !(rayLength == rayLength))
            return 0.0;
        dir = view / rayLength;
    }
    else
    {
        // A sky pixel of a reversed-Z projection with no far plane: w is exactly 0 and the transformed point is a direction. The atlas' own reach
        // bounds the march.
        const float directionLength = length(view4.xyz);
        if (!(directionLength > 1.0e-12) || !(directionLength == directionLength))
            return 0.0;
        dir = view4.xyz / directionLength;
        rayLength = 1.0e30;
    }
    // The march's bounds: the pixel's own surface, and the captured cascades' own shadow distance.
    const float tFar = min(rayLength, LightDir.w);
    if (!(tFar > 0.0))
        return 0.0;
    const float dt = tFar / max(Tuning.x, 1.0);
    // Every pixel starts its march at a different fraction of its own first step (the radial's own jitter, rule 4).
    const float jitter = Tuning.z > 0.5 ? Hash01(position.xy) : 0.0;
    const float phase = HenyeyGreenstein(dot(dir, LightDir.xyz), Medium.y);
    const float sigmaScale = Medium.x * kSigmaPerMetre;
    float2 atlasSize = float2(1.0, 1.0);
    Atlas.GetDimensions(atlasSize.x, atlasSize.y);
    const float2 texelSize = AtlasParam.xy;
    const float discScale = AtlasParam.z;
    float sum = 0.0;
    float tau = 0.0;
    for (int i = 0; i < int(max(Tuning.x, 1.0)); ++i)
    {
        const float t = (float(i) + jitter) * dt;
        const float3 p = dir * t;
        // The sample's world height: the inverse view's second row dotted with the view point (and its translation).
        const float worldY = dot(Camera.xyz, p) + Camera.w;
        const float rho = FogDensityAt(worldY) * FogB.w * FogA.w;
        const float sigma = sigmaScale * rho;
        // The shadow: every cascade, each only inside its own slice, the minimum - the way the engine's own mask writer blends them. A sample in
        // no cascade's slice is lit - the fail-open answer, the same one the engine's own mask gives past its cascades' reach.
        float visibility = 1.0;
        visibility = min(visibility, CascadeVisibility(p, Cascade0Row0, Cascade0Row1, Cascade0Row2, Cascade0Row3, Cascade0Fade, Cascade0Ramp,
                                                       texelSize, discScale, Tuning.y, atlasSize));
        visibility = min(visibility, CascadeVisibility(p, Cascade1Row0, Cascade1Row1, Cascade1Row2, Cascade1Row3, Cascade1Fade, Cascade1Ramp,
                                                       texelSize, discScale, Tuning.y, atlasSize));
        visibility = min(visibility, CascadeVisibility(p, Cascade2Row0, Cascade2Row1, Cascade2Row2, Cascade2Row3, Cascade2Fade, Cascade2Ramp,
                                                       texelSize, discScale, Tuning.y, atlasSize));
        visibility = min(visibility, CascadeVisibility(p, Cascade3Row0, Cascade3Row1, Cascade3Row2, Cascade3Row3, Cascade3Fade, Cascade3Ramp,
                                                       texelSize, discScale, Tuning.y, atlasSize));
        // The in-scatter integral: extinction accrues through shadowed air too.
        const float transmittance = exp(-tau);
        sum += sigma * phase * visibility * transmittance * dt;
        tau += sigma * dt;
    }
    return clamp(sum, 0.0, kMaxTerm);
}

// The stats pass: one group of 256 threads. It reads the scene's own sky pixels (every 4th pixel each way over the window), builds a 128-bin histogram
// per channel over [0, white] and writes 0.98 of the interpolated 99th percentile (or 0 with fewer than 64 samples) into texel 0, with the march term's
// peak in its alpha; texel 1 carries the sample count. GodraysRules.h HistogramPercentile / SkyCeilingOf are the host copies of the percentile.
groupshared uint gHistogram[3 * 128];
groupshared uint gSamples;
groupshared uint gPeak;

uint SkyBinOf(float value, float range)
{
    if (!(value == value) || !(value > 0.0) || !(range > 0.0))
        return 0u;
    const float scaled = value / range * float(kSkyBins);
    return scaled >= float(kSkyBins) ? kSkyBins - 1u : uint(scaled);
}

float PercentileOf(uint channel, float range)
{
    float total = 0.0;
    for (uint i = 0; i < kSkyBins; ++i)
        total += float(gHistogram[channel * kSkyBins + i]);
    if (!(total > 0.0))
        return 0.0;
    const float rank = kSkyPercentile * total;
    float before = 0.0;
    for (uint j = 0; j < kSkyBins; ++j)
    {
        const float count = float(gHistogram[channel * kSkyBins + j]);
        if (count > 0.0 && before + count >= rank)
            return (float(j) + saturate((rank - before) / count)) * range / float(kSkyBins);
        before += count;
    }
    return range;
}

[numthreads(256, 1, 1)]
void CS_Stats(uint index : SV_GroupIndex)
{
    for (uint slot = index; slot < 3 * kSkyBins; slot += 256)
        gHistogram[slot] = 0u;
    if (index == 0)
    {
        gSamples = 0u;
        gPeak = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    const float range = Bound.x;
    const int2 origin = int2(Window.xy);
    const uint nx = (uint(Window.z) + 3u) / 4u;
    const uint ny = (uint(Window.w) + 3u) / 4u;
    for (uint s = index; s < nx * ny; s += 256)
    {
        const int2 at = origin + int2(int((s % nx) * 4u + 1u), int((s / nx) * 4u + 1u));
        if (at.x >= origin.x + int(Window.z) || at.y >= origin.y + int(Window.w))
            continue;
        if (SkyOf(Depth.Load(int3(at, 0))) < 0.5)
            continue;
        const float3 colour = Scene.Load(int3(at, 0)).rgb;
        InterlockedAdd(gSamples, 1u);
        InterlockedAdd(gHistogram[0 * kSkyBins + SkyBinOf(colour.r, range)], 1u);
        InterlockedAdd(gHistogram[1 * kSkyBins + SkyBinOf(colour.g, range)], 1u);
        InterlockedAdd(gHistogram[2 * kSkyBins + SkyBinOf(colour.b, range)], 1u);
    }
    uint termWidth = 0;
    uint termHeight = 0;
    Term.GetDimensions(termWidth, termHeight);
    for (uint k = index; k < termWidth * termHeight; k += 256)
    {
        const float value = Term.Load(int3(int(k % termWidth), int(k / termWidth), 0));
        if (value == value && value > 0.0)
        {
            uint ignored;
            InterlockedMax(gPeak, asuint(value), ignored);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (index == 0)
    {
        float3 skyCeiling = float3(0.0, 0.0, 0.0);
        if (gSamples >= kMinSkySamples)
            skyCeiling = kSkyCeilingMargin * float3(PercentileOf(0u, range), PercentileOf(1u, range), PercentileOf(2u, range));
        Stats[uint2(0, 0)] = float4(skyCeiling, asfloat(gPeak));
        Stats[uint2(1, 0)] = float4(float(gSamples), 0.0, 0.0, 0.0);
    }
}

// The colour of the march: the light's own colour when the engine reports one, otherwise the sky's own colour around the sun - the radial
// shafts' scan-and-sky rule, on this pass's own depth and scene.
//
// Only texels whose depth is the far plane count, and the sampled colour is normalized to its own peak, so the march's brightness is the
// key's and the term's alone (the radial's own colour pass carries the same two rules and the reasons for them).
static const float kMinTintLuminance = 0.02;
static const float kMinTintPeak = 0.25;
static const float3 kLumaWeights = float3(0.2126390059, 0.7151686788, 0.0721923154);
static const int kSunColourRadius = 12; // scene pixels around the sun's position that the search looks at

float4 PS_SunColour(float4 position : SV_Position) : SV_Target0
{
    uint width = 0;
    uint height = 0;
    Scene.GetDimensions(width, height);
    const float2 last = float2(max(float(width) - 1.0, 0.0), max(float(height) - 1.0, 0.0));
    const int2 sunPixel = int2(clamp(SunScreen.xy, float2(0.0, 0.0), last));
    float3 sum = float3(0.0, 0.0, 0.0);
    float sky = 0.0;
    for (int dy = -kSunColourRadius; dy <= kSunColourRadius; ++dy)
        for (int dx = -kSunColourRadius; dx <= kSunColourRadius; ++dx)
        {
            const int2 at = sunPixel + int2(dx, dy);
            if (at.x < 0 || at.y < 0 || at.x >= int(width) || at.y >= int(height))
                continue;
            if (SkyOf(Depth.Load(int3(at, 0))) < 0.5)
                continue;
            sum += saturate(Scene.Load(int3(at, 0)).rgb);
            sky += 1.0;
        }
    if (!(sky > 0.0))
        return float4(0.0, 0.0, 0.0, 0.0);
    const float3 colour = sum / sky;
    if (!(dot(colour, kLumaWeights) >= kMinTintLuminance))
        return float4(0.0, 0.0, 0.0, 0.0);
    const float peak = max(max(colour.r, colour.g), colour.b);
    return float4(saturate(colour / max(peak, kMinTintPeak)), 1.0);
}

// The term at a full-resolution pixel: the bilinear read between the four march texels whose centres surround it, each weighted by how close its own
// depth is to the pixel's (a pixel keeps the term of its side of a depth edge), or the plain bilinear read when the sigma is 0 or every weight is gone.
float UpsampledTerm(float2 pixelCentre, int2 at)
{
    uint termWidth = 1;
    uint termHeight = 1;
    Term.GetDimensions(termWidth, termHeight);
    const int2 lastTerm = max(int2(termWidth, termHeight) - int2(1, 1), int2(0, 0));
    const float2 coordinate = (pixelCentre - Window.xy) / Tuning.w - 0.5;
    const int2 base = int2(floor(coordinate));
    const float2 fraction = saturate(coordinate - float2(base));
    const bool bilateral = Upsample.x > 0.0;
    const float distancePixel = bilateral ? LinearDistance(Depth.Load(int3(at, 0))) : 0.0;
    float sum = 0.0;
    float weights = 0.0;
    float plain = 0.0;
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx)
        {
            const int2 texel = clamp(base + int2(dx, dy), int2(0, 0), lastTerm);
            const float value = Term.Load(int3(texel, 0));
            const float bilinear = (dx == 1 ? fraction.x : 1.0 - fraction.x) * (dy == 1 ? fraction.y : 1.0 - fraction.y);
            plain += bilinear * value;
            float weight = bilinear;
            if (bilateral)
            {
                const float distanceTexel = LinearDistance(Depth.Load(int3(DepthPixelOf(MarchWindowPixel(float2(texel) + 0.5)), 0)));
                const float larger = max(distancePixel, distanceTexel);
                const float relative = larger > 0.0 ? abs(distancePixel - distanceTexel) / larger / Upsample.x : 0.0;
                weight *= exp(-relative * relative);
            }
            sum += weight * value;
            weights += weight;
        }
    return (bilateral && weights > 1.0e-4) ? sum / weights : plain;
}

// The composite adds the beams, not the haze (GodraysRules.h kBeamBoost, kBeamKeep, kBeamTapsPerSide, kBeamReachPixels, BeamTapStride, BeamCentreTexel, BeamTerm:
// the same numbers and the same expressions, pinned equal): term' = min(max(term - lowpass, 0) * boost + keep * term, kMaxTerm), the low-pass being the mean of
// a (2 * taps + 1)^2 grid of march texels around the pixel's own nearest march texel, reach pixels per side.
static const float kBeamBoost = 0.75;
static const float kBeamKeep = 0.10;
static const int kBeamTapsPerSide = 4;
static const float kBeamReachPixels = 48.0;

float BeamLowpass(float2 pixelCentre)
{
    uint termWidth = 1;
    uint termHeight = 1;
    Term.GetDimensions(termWidth, termHeight);
    const int2 lastTerm = max(int2(termWidth, termHeight) - int2(1, 1), int2(0, 0));
    const float2 coordinate = (pixelCentre - Window.xy) / Tuning.w - 0.5;
    const int2 centre = int2(floor(coordinate + 0.5));
    const int stride = max(int(kBeamReachPixels / Tuning.w / float(kBeamTapsPerSide) + 0.5), 1);
    float sum = 0.0;
    for (int dy = -kBeamTapsPerSide; dy <= kBeamTapsPerSide; ++dy)
        for (int dx = -kBeamTapsPerSide; dx <= kBeamTapsPerSide; ++dx)
            sum += Term.Load(int3(clamp(centre + int2(dx, dy) * stride, int2(0, 0), lastTerm), 0));
    return sum / float((2 * kBeamTapsPerSide + 1) * (2 * kBeamTapsPerSide + 1));
}

float BeamTerm(float term, float lowpass)
{
    if (!(term == term) || !(term > 0.0))
        return 0.0;
    const float structure = (lowpass == lowpass && term > lowpass) ? term - max(lowpass, 0.0) : 0.0;
    return min(structure * kBeamBoost + kBeamKeep * term, kMaxTerm);
}

float4 PS_Composite(float4 position : SV_Position) : SV_Target0
{
    // The scene's own pixel and this pass's copy's own pixel are the same one: SV_Position already holds the target pixel, so the window's
    // half-pixel origin is not added here (rule 1).
    const int2 at = int2(position.xy);
    const float4 scene = Scene.Load(int3(at, 0));
    if (!(Bound.z > 0.0))
        return scene;
    const float upsampled = max(UpsampledTerm(position.xy, at), 0.0);
    if (!(upsampled > 0.0))
        return scene;
    // Only the term's structure above its own local level, plus a small kept share (see BeamTerm): the unshadowed in-scatter is the engine's fog.
    const float term = BeamTerm(upsampled, BeamLowpass(position.xy));
    if (!(term > 0.0))
        return scene;
    const float sky = SkyOf(Depth.Load(int3(at, 0)));
    // The tint: the resolved colour (the auto mode's light colour or the white key's neutral), the sun's own texel (the auto mode's
    // scan-and-sky answer), or the engine's own fog colour rule.
    float3 tint = Colour.rgb;
    if (Colour.w > 1.5)
    {
        // The engine's own rule: colour = max(0, LUT.y * (sky(pixelUV) - fogColor) + fogColor), with the LUT's u from the pixel's own
        // world distance the way the fog post maps it. The sky's own texel is the one behind this pixel, at the sky texture's own scale.
        uint skyWidth = 1;
        uint skyHeight = 1;
        Sky.GetDimensions(skyWidth, skyHeight);
        const float2 skyAt = min((position.xy - Window.xy) / Window.zw * float2(skyWidth, skyHeight),
                                 float2(max(float(skyWidth) - 1.0, 0.0), max(float(skyHeight) - 1.0, 0.0)));
        const float3 skyColour = saturate(Sky.Load(int3(int2(skyAt), 0)).rgb);
        const float depth = Depth.Load(int3(at, 0));
        const float worldDist = FogLut.z + FogLut.w * depth;
        const float u = worldDist > 0.0 ? FogLut.x + FogLut.y / worldDist : 0.0;
        const float2 lut = Lut.Load(int3(int2(clamp(u, 0.0, 1.0) * 255.0, 0), 0));
        const float weight = lut.y == lut.y && lut.y > 0.0 ? lut.y : 0.0;
        tint = max(weight * (skyColour - FogColour.rgb) + FogColour.rgb, float3(0.0, 0.0, 0.0));
    }
    else if (Colour.w > 0.5)
    {
        const float4 sampled = SunTint.Load(int3(0, 0, 0));
        if (!(sampled.a > 0.5))
            return scene; // no sky around the sun: the frame has no sun to take a colour from
        tint = sampled.rgb;
    }
    // The light this pixel gets, in fractions of the frame's own white: the march's term times the key, the tint's own channel, the gain
    // and the held state's fade. The addition is bounded by how much room the pixel has left to the white - head = white - scene,
    // add = head*raw/(raw+head) - so the sum is always strictly below the white point: no key, no term and no fade can push a channel
    // past it, and a pixel already at white gets nothing instead of a clipped white smear. (ShaftsRules.h ShaftAdded is this expression,
    // and its tests pin the bound; the radial's composite carries the same one.)
    const float3 light = term * Bound.z * tint * Bound.y;
    const float3 head = max(Bound.x - scene.rgb, 0.0);
    const float3 raw = light * Bound.x;
    float3 add = head * raw / max(raw + head, 1.0e-6);
    // The sky is lit too, but never past its own ceiling (rule 3): the scene's sky 99th percentile as CS_Stats sampled it; a ceiling of 0 (too few sky
    // samples) lets nothing through.
    if (sky > 0.5 && Bound.w > 0.5)
    {
        const float3 skyCeiling = SkyCeil.Load(int3(0, 0, 0)).rgb;
        const float3 allowed = max(skyCeiling - scene.rgb, 0.0);
        add = min(add, allowed) * step(1.0e-30, skyCeiling);
    }
    float4 outColor = scene;
    outColor.rgb += add;
    return outColor;
}
