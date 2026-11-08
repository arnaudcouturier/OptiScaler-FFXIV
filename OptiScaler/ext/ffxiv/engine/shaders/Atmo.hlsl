// Atmo.hlsl: atmosphere (aerial perspective with height and a sun lobe), mixed into a copy of the scene before the engine's tone map reads it.
//
// One vertex shader and one pixel shader, one constant buffer and no sampler (every read is a Load on an exact texel, so the result does not depend on
// filter state and a test can predict it):
//   VS       (vs_5_0) one oversized triangle from SV_VertexID
//   PS_Atmo  (ps_5_0) the render window: per surface pixel the view position through the engine's inverse projection (the depth's reconstruction the fog post
//            and the depth of field use), the optical depth of an exponential height medium along the view ray from a start distance to the surface, the
//            transmittance T = exp(-tau), the air's own light (the engine's fog colour as the ambient term, plus the sun's Henyey-Greenstein lobe), and
//            out = scene * T + inscatter * (1 - T) in LINEAR light, stored back as its root (the scene is stored as the square root of linear light).
//
// This file is the stage's own: it includes nothing, shares no shader source with Godrays.hlsl, SceneMarch.hlsl or Dof.hlsl, and edits no shader of the
// game's own family. The maths is AtmoRules.h's and the two files change together (atmo_pins reads their constants against each other); the references
// are named there (Henyey and Greenstein 1941, Wenzel 2006, Quilez 2010, Hillaire 2015).
//
// Rules:
//  1. The engine allocates the frame's targets at the display size, draws a smaller window inside them and starts that window at a half-pixel origin, so
//     SV_Position.xy is already the render target's pixel (k + 0.5) whatever the viewport's origin (the march's own rule).
//  2. Nothing is written to the engine's textures. PS_Atmo reads the scene it was handed and writes this pass's own copy of it; the caller seeded the copy
//     with a CopyResource of the handed-in scene, so the window's outside keeps the handed-in picture byte for byte.
//  3. A pixel that is not a surface (the far plane, a NaN depth), a pixel nearer than the start distance, a pixel whose reconstruction fails and a pixel the
//     medium does not touch (T = 1) returns the scene's texel as it was loaded - no round trip through the square, so the near field and the sky are bit-exact.
//  4. The term is a fixed function of the frame's buffers: no frame number, no random offset, no history, no noise. Two runs of one frame are one picture,
//     and a camera that moves by a jitter step moves the haze by a jitter step (the haze is smooth in the view distance).
//  5. The air's light is bounded by what it is made of (the fog colour, the frame's white): the mix is a convex combination of the scene's own light and the
//     air's, so no key can push a channel past the larger of the two.
//
// The register slots and the constant-buffer layout are mirrored by AtmoPass.cpp; change them together.

cbuffer Params : register(b0)
{
    float4 Window;      // xy = the render window's origin in scene pixels, zw = its extent in scene pixels
    float4 RayScale;    // xy = the pixel-to-NDC scale (derived from the render window), zw = its offset
    float4 InvProjRow0; // the inverse projection's rows (g_CameraParameter +0x0E0): the depth's reconstruction
    float4 InvProjRow1;
    float4 InvProjRow2;
    float4 InvProjRow3;
    float4 UpView;      // xyz = the world's up in view space (unit), w = 0
    float4 SunView;     // xyz = the direction toward the sun in view space (unit), w = the sun's gain (0 = no sun: the ambient term alone)
    float4 Medium;      // x = the density at the camera's height per metre (after the engine-fog and night scales), y = the height falloff per metre,
                        // z = the start distance in metres, w = the Henyey-Greenstein g
    float4 Light;       // x = the sky key (the ambient term's scale), y = the sun key times the frame's linear white, z = the neutral share (L2b: how much of the
                        // fog colour is replaced by a grey of its own luminance), w = 0
    float4 FogColour;   // xyz = the engine's fog colour (linear light), w = 0
    float4 SunTint;     // xyz = the sun's colour of peak 1, w = 0
};

Texture2D<float> Depth : register(t0);  // the scene depth, reversed-Z (the fog post's own t0)
Texture2D<float4> Scene : register(t1); // the scene the chain handed in (read only, never written)

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

// AtmoRules.h's constants (kSkyDepthEpsilon, kMaxDensityGain, kMaxFallExponent, kMaxOpticalDepth); neither file may drift.
static const float kSkyDepthEpsilon = 1.0e-6;
static const float kMaxDensityGain = 6.0;
static const float kMaxFallExponent = 40.0;
static const float kMaxOpticalDepth = 12.0;

float2 NdcOf(float2 windowPixel)
{
    return windowPixel * RayScale.xy + RayScale.zw;
}

// The optical depth along a ray from `start` to `distance` metres in an exponential height medium whose density at the camera's height is rho0 and whose
// density at a ray point is rho0 exp(-falloff * up * s): AtmoRules.h OpticalDepth, with a fourth-order series for the mean near zero (this file has no expm1,
// and (1 - exp(-x)) / x cancels below about 0.02 in single precision).
float OpticalDepth(float rho0, float falloff, float up, float start, float distance)
{
    if (!(rho0 > 0.0) || !(distance > start) || !(start >= 0.0) || !(falloff == falloff) || !(up == up))
        return 0.0;
    const float len = distance - start;
    const float a = falloff * up;
    const float f0 = clamp(-a * start, -kMaxFallExponent, kMaxDensityGain);
    const float x = clamp(a * len, -kMaxDensityGain, kMaxFallExponent);
    const float series = 1.0 - 0.5 * x + x * x * (1.0 / 6.0) - x * x * x * (1.0 / 24.0) + x * x * x * x * (1.0 / 120.0);
    const float mean = abs(x) < 0.02 ? series : (1.0 - exp(-x)) / x;
    const float tau = rho0 * exp(f0) * len * mean;
    if (!(tau == tau) || tau <= 0.0)
        return 0.0;
    return min(tau, kMaxOpticalDepth);
}

// The ambient term's colour (AtmoRules.h NeutralAmbient, L2b): the engine's fog colour (negative channels are no light) blended toward a grey of its own luminance by
// `neutral`. The luminance weights are the tree's own (ShaftsRules.h kLumaR / kLumaG / kLumaB); the two files change together.
float3 NeutralAmbient(float3 fogColour, float neutral)
{
    const float3 colour = max(fogColour, float3(0.0, 0.0, 0.0));
    const float grey = dot(colour, float3(0.2126390059, 0.7151686788, 0.0721923154));
    const float share = clamp(neutral, 0.0, 1.0);
    return colour * (1.0 - share) + grey * share;
}

// The Henyey-Greenstein lobe normalised to 1 at the light (AtmoRules.h SunPhase): (1 - g)^3 / ((1 - g)^2 + 2 g (1 - c))^1.5.
float SunPhase(float cosTheta, float g)
{
    const float gg = clamp(g, 0.0, 0.95);
    const float c = clamp(cosTheta, -1.0, 1.0);
    const float oneMinus = 1.0 - gg;
    const float denominator = oneMinus * oneMinus + 2.0 * gg * (1.0 - c);
    return oneMinus * oneMinus * oneMinus / pow(max(denominator, 1.0e-6), 1.5);
}

float4 PS_Atmo(float4 position : SV_Position) : SV_Target0
{
    // The scene's own pixel and this pass's copy's own pixel are the same one (rule 1).
    const int2 at = int2(position.xy);
    const float4 scene = Scene.Load(int3(at, 0));
    const float depth = Depth.Load(int3(at, 0));
    // The sky and the far plane get nothing (reversed Z: the far plane is 0), and a depth the arithmetic cannot read is not a surface (rule 3).
    if (!(depth == depth) || !(depth > kSkyDepthEpsilon))
        return scene;
    // The view position: the clip point (ndc, depth, 1) through the inverse projection, divided by w.
    const float2 ndc = NdcOf(position.xy);
    const float4 clip = float4(ndc, depth, 1.0);
    const float4 v = float4(dot(InvProjRow0, clip), dot(InvProjRow1, clip), dot(InvProjRow2, clip), dot(InvProjRow3, clip));
    if (!(abs(v.w) > 1.0e-6) || !(v.w == v.w))
        return scene;
    const float3 p = v.xyz / v.w;
    const float distance = length(p);
    if (!(distance == distance) || !(distance > Medium.z) || !(distance > 0.0))
        return scene; // nearer than the start distance (or no reconstruction): the near field stays as it was
    const float3 direction = p / distance;
    const float tau = OpticalDepth(Medium.x, Medium.y, dot(direction, UpView.xyz), Medium.z, distance);
    if (!(tau > 0.0))
        return scene;
    const float transmittance = exp(-min(tau, kMaxOpticalDepth));
    if (!(transmittance < 1.0))
        return scene;
    // The air's own light, linear: the ambient term plus the sun's lobe.
    const float phase = SunPhase(dot(direction, SunView.xyz), Medium.w);
    const float3 ambient = NeutralAmbient(FogColour.rgb, Light.z);
    const float3 air = Light.x * ambient + Light.y * SunView.w * phase * SunTint.rgb;
    // The mix, in linear light (the scene is stored as the root of it), stored back as its root. A broken source texel is left as it was (rule 3).
    if (!(scene.r == scene.r) || !(scene.g == scene.g) || !(scene.b == scene.b))
        return scene;
    const float3 stored = max(scene.rgb, float3(0.0, 0.0, 0.0));
    const float3 mixed = stored * stored * transmittance + max(air, float3(0.0, 0.0, 0.0)) * (1.0 - transmittance);
    return float4(sqrt(max(mixed, float3(0.0, 0.0, 0.0))), scene.a);
}
