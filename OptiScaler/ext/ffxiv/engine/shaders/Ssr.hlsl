// Screen-space reflections beside the engine's planar reflection map: this fork's own reflection of the scene, marched in screen space from the
// engine's view-position buffer, written into the map the water draws sample WHERE THE ENGINE'S OWN MAP HAS NO HIT, in the engine's convention.
//
// Three entry points, one vertex shader, no sampler (every read is a Load on an exact texel, so the result does not depend on filter state and a
// host test can predict it):
//   VS         (vs_5_0) one oversized triangle from SV_VertexID
//   PS_Ssr     (ps_5_0) draw 1: the planar map's texels -> this module's TERM (premultiplied rgb and alpha, plus the hit's distance along its ray)
//   PS_SsrFinal (ps_5_0) draw 2: the term filtered over its neighbours, or the engine's own texel
//
// Inputs:
//   t0 Planar       Texture2D<float4>  the reader's g_SamplerReflectionMap (the engine's map: draw 2 keeps its hits and its texels where there is no term)
//   t1 VPosition    Texture2D<float4>  the reader's g_SamplerVPosition: the view-space position of every pixel (the camera is the origin)
//   t2 Scene        Texture2D<float4>  this pass's copy of the scene the frame had rendered when the first water draw arrived
//   t3 Camera       Buffer<float4>     g_CameraParameter: the projection matrix at register 18
//   t4 Raw          Texture2D<float4>  draw 2: draw 1's term (R16G16B16A16_FLOAT)
//   t5 Guide        Texture2D<float>   draw 2: draw 1's hit distance along the ray, in metres (R16_FLOAT; 0 = no term)
//
// REPLACE, NOT ADD. The water shader ADDS the map's decoded rgb under its Fresnel term and uses the map's alpha only to dim its probe reflection and
// the sun glint, so a colour written with an alpha of 0 is light on top of the full probe reflection (a double reflection). The engine's own map
// stores its colour PREMULTIPLIED by its alpha (rgb is 0 wherever alpha is 0, and rgb <= alpha in 99 % of its hit texels), and this pass writes the
// same convention: a hit is alpha = blend x confidence and rgb = alpha x the reflected colour, the colour being the scene's own (the scene target
// holds the water shader's output encoding s x sqrt(C), the one the shader decodes with (rgb / cb1[3].x)^2). Where the engine's own alpha is above
// zero the texel stays the engine's, all four channels; where the march finds nothing it stays the engine's too.

// Rules:
//  1. The planar map is a fraction of the scene allocation and every reader samples it with the allocation-relative screen coordinate, so a texel
//     of ours stands for the scene texels at the same fraction of the scene allocation: `Mapping` is that ratio (2 at the session's 1920x1080 map
//     over a 3840x2160 allocation) and there is no origin. The engine's own producer fills only the part of the map that lies inside the render
//     window, so this pass writes only `Region` and its target arrives holding the engine's own texels everywhere else.
//  2. The geometry is the view-position buffer the water shader itself samples, not a depth: it is exact at every pixel and needs no matrix to
//     become a view point. The reflecting surface is the water itself, and the map this pass replaces is a mirror about the water plane, so the
//     normal is the world's up transformed into view space (the view matrix's own second column) at every texel - the water's own normal, not a
//     normal derived from neighbouring pixels. Where the march needs a surface at all it compares view-space distances (the thickness rule); it
//     never needs a normal at the hit.
//  3. The march starts a small distance along the normal (a ray that starts on the surface reads its own pixel) and steps outward geometrically,
//     so the near field - where a reflection is sharpest - gets most of the taps. A step that leaves the render window ends the march with no
//     hit: the texel keeps the engine's own reflection.
//  4. A hit is a step whose sampled surface lies behind the ray point along the ray, and not farther behind it than the thickness: a wall the ray
//     passed a metre ago is another object, not the surface the ray crossed. The interval the crossing sits in is halved a few times, which turns
//     a step into the surface's own texel; the texel the ray crosses is where the reflected colour comes from - weighted at the crossing's own
//     fraction, so a sub-texel slide of the crossing (the upscaler's jitter inside the projection) moves the four weights instead of flipping whole
//     texels. A hit that lands within a texel or two of its own origin at a short distance is the ray grazing the surface it left (the view-position
//     buffer holds the ground under the water, so the ray starts inside it): the engine's own texel answers instead.
//  5. The confidence is the hit's distance to the edge of the picture divided by `fade`: a reflection that ends at the screen's edge fades out
//     instead of stopping at a hard line. Below the threshold the texel is the engine's own, so a march that found nothing never smears.
//  6. The term is a fixed function of the frame's buffers: no frame number, no random offset, no history. A still scene gives the same reflection
//     every frame, which matters because the upscaler is not a denoiser.

Texture2D<float4> Planar : register(t0);
Texture2D<float4> VPosition : register(t1);
Texture2D<float4> Scene : register(t2);
Buffer<float4> Camera : register(t3);
Texture2D<float4> Raw : register(t4);
Texture2D<float> Guide : register(t5);

cbuffer Params : register(b0)
{
    float4 Window;  // xy = the render window's origin in scene texels, zw = its size in scene texels
    float4 Mapping; // xy = scene texels per planar texel, zw = the planar target's size in texels
    float4 Sizes;   // xy = the scene allocation's size in texels, z = the steps, w = the thickness in metres
    float4 Tuning;  // x = blend, y = fade, z = the confidence threshold, w = the self-graze distance in metres (0 = off)
    float4 Region;  // xy = the planar texel this pass writes from, zw = how many texels of it in each direction
};

// A host test of one frame compiles this file with SSR_MARCH_DIAG defined and reads the red channel of the result back: 1..6 are the inputs
// that leave the texel without a term before the march starts, 10 is a hit, 11..14 are the march's own reasons for ending without one, 15 is a
// hit the confidence threshold dropped, and 16 is a hit the self-graze rejection dropped; the green channel carries the hit's distance along the ray
// over kMaxDistance, which is the number that says whether a ray ends at once. In that build draw 2 hands the term to the map as it is. The reasons and the distance are written inside the shared march
// (SceneMarch.hlsl), which is why the two macros they use are defined here, before that file is included. The product's build never defines
// SSR_MARCH_DIAG (ssr.shaders.props), so every macro below expands to the statement beside it and the shader compiled for the game is this file with
// the diagnostics removed.
#ifdef SSR_MARCH_DIAG
#define SSR_ENGINE(code) return float4(float(code) / 255.0, 0.0, 0.0, 1.0)
#define SSR_HIT(colour, distance) return float4(10.0 / 255.0, saturate(distance / kMaxDistance), 0.0, 1.0)
#define SSR_REASON(code) reason = code
#define SSR_DIAG_DISTANCE(value) hitDistance = value
#else
#define SSR_ENGINE(code) return float4(0.0, 0.0, 0.0, 0.0) // no term: draw 2 gives the texel the engine's own
#define SSR_HIT(colour, distance) { guide = (distance); return (colour); }
#define SSR_REASON(code)
#define SSR_DIAG_DISTANCE(value)
#endif

// g_CameraParameter's register numbers come from the one header that states the layout (ext/ffxiv/relight/shaders/CameraRegisters.h, the
// reflected layout of the engine's own shaders), so this pass and the contact shadow pass cannot drift apart. The build passes that folder with
// -I (ssr.shaders.props).
#include "CameraRegisters.h"

// The march is shared with the puddles' mirror at the tone map (SceneMarch.hlsl): one implementation and one set of step rules, so a fix to one
// reflection cannot leave the other marching differently. The step count is still this pass's own key.
#include "SceneMarch.hlsl"

static const float kMinNormalLength = 1e-12; // a camera table without a view matrix: no plane to reflect in
static const int kSelfGrazeTexels = 2;       // the self-graze texel radius, pinned to SsrRules.h's kSelfGrazeTexels by ssr_frame's code table

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

float4x4 LoadProjection()
{
    return float4x4(Camera[CS_CAMERA_PROJECTION_REGISTER + 0], Camera[CS_CAMERA_PROJECTION_REGISTER + 1],
                    Camera[CS_CAMERA_PROJECTION_REGISTER + 2], Camera[CS_CAMERA_PROJECTION_REGISTER + 3]);
}

// The term of draw 1: premultiplied rgb and alpha in SV_Target0, the hit's distance along its ray in SV_Target1 (0 where there is no term). It is a
// function of the frame's buffers alone - it does not look at the engine's map, so the filter of draw 2 can use the term of a texel the engine
// already answers for as one of its taps.
float4 PS_Ssr(float4 position : SV_Position, out float guide : SV_Target1) : SV_Target0
{
    guide = 0.0;
    // The scene texel this planar texel stands for: the map is a fraction of the scene allocation and the readers sample it by the same fraction,
    // so the mapping is the ratio of the two sizes and the texel's own index is where its block starts (rule 1). The position carries the pixel's
    // centre (+0.5) and that half must not reach the ratio: at scale 2 a texel of ours stands for scene texel 2t, and scaling the centre would
    // read 2t+1, the block's second texel, which is not what the rules, the reference or this comment say.
    const int2 own = int2(position.xy);
    const int2 regionLow = int2(Region.x, Region.y);
    const int2 regionHigh = int2(Region.x + Region.z, Region.y + Region.w);
    if (own.x < regionLow.x || own.y < regionLow.y || own.x >= regionHigh.x || own.y >= regionHigh.y)
        SSR_ENGINE(1); // not the part the engine's own producer fills: no term, the engine's texel

    const int2 at = int2(float2(own) * Mapping.xy);
    if (float(at.x) >= Sizes.x || float(at.y) >= Sizes.y)
        SSR_ENGINE(2);

    const float3 P = VPosition.Load(int3(at, 0)).xyz;
    if (!(dot(P, P) > 1e-6))
        SSR_ENGINE(3); // the cleared value of a pixel with no geometry (the sky): nothing to reflect from

    // The water plane's normal: the world's up turned into view space by the view matrix's own second column (rule 2). It is the surface the planar
    // map is a mirror about, and it is the same at every texel, so the reflection of a still scene is a mirror image and not a screen-space guess.
    float3 N = float3(Camera[CS_CAMERA_VIEW_REGISTER + 0].y, Camera[CS_CAMERA_VIEW_REGISTER + 1].y, Camera[CS_CAMERA_VIEW_REGISTER + 2].y);
    if (!(dot(N, N) > kMinNormalLength))
        SSR_ENGINE(4); // a camera table without a view matrix: no plane to reflect in
    N = normalize(N);
    if (dot(N, P) > 0.0)
        N = -N; // the visible side faces the camera, which is the origin in view space

    const float3 V = normalize(P);
    const float3 R = reflect(V, N);
    if (any(isnan(R)))
        SSR_ENGINE(5);

    const int steps = clamp(int(Sizes.z), 1, kMaxSteps);
    const float thickness = Sizes.w;
    if (!(thickness > 0.0) || !(Window.z > 0.0) || !(Window.w > 0.0))
        SSR_ENGINE(6);

    int2 hitTexel = int2(-1, -1);
    float2 hitWindow = float2(0.0, 0.0);
    // The march's exit reason and the hit's distance along the ray: the diagnostic output reads them, and the product's build never does (the
    // macros expand to nothing, so the march's stores to them are dead).
    int reason = 13;
    float hitDistance = 0.0;
    if (!MarchScene(VPosition, P, R, LoadProjection(), Window, Sizes.xy, steps, thickness, hitTexel, hitWindow, reason, hitDistance))
        SSR_ENGINE(reason); // the march missed: no term, the engine's own texel

    // The self-graze rejection: a hit this close that lands a texel or two from its own origin is the ray grazing the surface it left (the
    // view-position buffer holds the ground under the water, so the ray starts inside that surface). About half of the march's hits sat in this
    // class in the captured frame; they anchor the reflection to the origin's own neighbourhood, which the upscaler reads as noise.
    if (hitDistance < Tuning.w && max(abs(hitTexel.x - at.x), abs(hitTexel.y - at.y)) <= kSelfGrazeTexels)
        SSR_ENGINE(16);

    // The confidence fades the hit out towards the edge of the picture (rule 5), then the hit's colour replaces the engine's by the blend.
    const float2 inWindow = (float2(hitTexel) + 0.5 - Window.xy) / Window.zw;
    const float edge = min(min(inWindow.x, 1.0 - inWindow.x), min(inWindow.y, 1.0 - inWindow.y));
    const float confidence = saturate(edge / max(Tuning.y, 1e-4));
    if (!(confidence > Tuning.z))
        SSR_ENGINE(15);

    // The hit's colour at the crossing's own fraction (rule 4): the four texels the crossing sits between, so the per-frame jitter inside the
    // projection slides the weights instead of flipping whole texels - the re-quantisation that re-rolled about a third of the reflection every
    // frame. The term is the engine's convention: alpha = blend x confidence says how much of the water's probe reflection and glint this hit
    // replaces, and rgb = alpha x the reflected colour (premultiplied). The hit's distance goes to the guide target for the filter.
    const float3 hitColour = saturate(SceneMarchColour(Scene, hitWindow, Window));
    const float factor = saturate(Tuning.x * confidence);
    SSR_HIT(float4(hitColour * factor, factor), hitDistance);
}

// ---- draw 2: the filter and the stand-down --------------------------------------------------------------------------------------------------------
// The constants are pinned to SsrRules.h's by ssr_warp's code table: change them together.
static const float kEngineHitAlpha = 0.0;    // an engine alpha above this is the engine's own hit
static const int kFilterRadius = 2;          // the filter reads (2 R + 1)^2 taps of the raw term
static const float kGuideTolerance = 0.15;   // a hit this much (relative) farther or nearer along its ray is another object: weight 0
static const float kGuideFloor = 0.25;       // metres: below this distance the tolerance is not made any tighter

float GuideWeight(float centre, float tap)
{
    const float scale = max(max(centre, tap), kGuideFloor) * kGuideTolerance;
    return saturate(1.0 - abs(centre - tap) / scale);
}

// The map's texel: the engine's where the engine has a hit or this pass has no term; otherwise the term, filtered over the neighbours that have a
// term of their own (inside the region: draw 1 wrote nothing outside it), weighted by their distance on the map and by how close their hit is to this
// texel's hit along the ray. Premultiplied rgb and alpha are filtered together, so rgb = alpha x colour holds for the mean as it does for each tap.
float4 PS_SsrFinal(float4 position : SV_Position) : SV_Target0
{
    const int2 own = int2(position.xy);
    const float4 centre = Raw.Load(int3(own, 0));
#ifdef SSR_MARCH_DIAG
    return centre; // the diagnostic build's term is the march's exit code: it reaches the map as it is
#else
    const float4 engine = Planar.Load(int3(own, 0));
    if (engine.a > kEngineHitAlpha)
        return engine; // the engine's own hit stands
    if (!(centre.a > 0.0))
        return engine; // this pass found nothing here
    const float guideCentre = Guide.Load(int3(own, 0));
    const int2 regionLow = int2(Region.x, Region.y);
    const int2 regionHigh = int2(Region.x + Region.z, Region.y + Region.w);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float weight = 0.0;
    [unroll]
    for (int dy = -kFilterRadius; dy <= kFilterRadius; ++dy)
    {
        [unroll]
        for (int dx = -kFilterRadius; dx <= kFilterRadius; ++dx)
        {
            const int2 tap = own + int2(dx, dy);
            if (tap.x < regionLow.x || tap.y < regionLow.y || tap.x >= regionHigh.x || tap.y >= regionHigh.y)
                continue;
            const float4 term = Raw.Load(int3(tap, 0));
            if (!(term.a > 0.0))
                continue; // a neighbour without a term has nothing to give: it neither fades this texel nor spreads
            const float w = (1.0 / (1.0 + float(dx * dx + dy * dy))) * GuideWeight(guideCentre, Guide.Load(int3(tap, 0)));
            sum += term * w;
            weight += w;
        }
    }
    return sum / weight;
#endif
}
