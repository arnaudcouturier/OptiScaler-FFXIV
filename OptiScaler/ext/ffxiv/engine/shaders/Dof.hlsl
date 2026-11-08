// Dof.hlsl: this fork's own depth of field, Route B, chained at the tone-map owner as the last stage before the tone map.
//
// Five pixel shaders and one vertex shader, one constant buffer, no sampler (every read is a Load on an exact texel, the
// march's own rule, so the result does not depend on filter state and a test can predict it). Two looks share PS_Blur and PS_Merge
// and differ in the pack (the CoC) and in where the focus comes from:
//   PS_Pack  (ps_5_0) the CINEMATIC look, the first release's code untouched: the render window, the scene's colour with the pack's cap
//            (the game's own Weight2 keeps min(col,10)) in rgb, the analytic signed CoC in alpha - CoC from the pixel's view depth
//            against the focus distance, whose auto mode is the view depth under the screen centre (the depth texel at the window's
//            own centre pixel, this frame's), shaped: a sharp focus band around the focus plane and the near side capped (CoCSigned);
//   PS_PackDistance (ps_5_0) the DISTANCE look (the default): the same pack, but the CoC is the far side only (DistanceCoC below: nothing before a
//            start distance, then the thin-lens difference to a far limit), and the auto focus is the smoothed state PS_Focus keeps;
//   PS_Focus (ps_5_0) one texel: the distance look's auto focus, a cross of depth taps around the window's centre (the sky ignored, the lower
//            median of the rest) moved toward by an exponential step in seconds (a far outlier slowly); R = the smoothed focus, G = this
//            run's raw sample, both in view metres;
//   PS_Blur  (ps_5_0) a quarter of the window, two targets at once (the game's own DownScale4x4 writes two 640x360): the
//            disc taps - golden-angle, sqrt-ramped over the aperture's reach - split near and far, each tap weighted by
//            the texel's own CoC the pack carries, each target energy-normalized over the taps that reached it;
//   PS_Merge (ps_5_0) the render window: the two quarter targets read back bilinearly and mixed into an owned copy of the
//            scene through the full-resolution CoC the pack carries at that very pixel - the game's own Merge2 reads the
//            pack beside the quarter targets for the same reason, and the mix's strength is the DofStrength key.
//
// This file is this stage's own: it includes nothing (the march's own rule holds here too), shares no shader source with
// SceneMarch.hlsl or Godrays.hlsl, and edits no shader of the game's own family. The min(col,10) cap and the CoC-in-alpha
// habit are the captured weight shader's own (its body saturates the colour to 10 and writes the CoC to the output alpha); the analytic
// CoC (a sharp focus band, a capped near side) replaces the engine's CoC LUT (the LUT belongs to shaping the game's own chain, not to this stage).
//
// Rules:
//  1. The engine allocates the frame's targets at the display size, draws a smaller window inside them and starts that
//     window at a half-pixel origin, so SV_Position.xy is already the render target's pixel (k + 0.5) whatever the
//     viewport's origin, and a sub-resolution texel k of the window stands for the window's own scene pixels s*k..s*k+s-1
//     (s = the resolution's scale) - the march's own rule, mirrored here: the quarter's block centre is Window.xy +
//     4*(quarter + 0.5), and the merge's readback is that inverse.
//  2. Nothing is written to the engine's textures. PS_Merge reads the scene it was handed and writes this pass's own copy
//     of it; the pack and the two quarter targets are the pass's own textures throughout.
//  3. A pixel the pack cannot reconstruct (a NaN depth, a broken read) focuses nothing: the CoC is 0, so the pixel mixes
//     nothing and keeps the scene's own value. A broken depth never fills the frame with blur (the march's rule 6).
//  4. Nothing here adds light or clips it: the blur is a mix of the scene's own texels, so the merged value lies between
//     the scene's value and the neighbours the disc read - no key can push a channel past the picture's own values.
//  5. The taps are a fixed function of the frame: no frame number, no history, no random offset. Two runs of one frame
//     are one picture.
//
// The register slots and the constant-buffer layout are mirrored by DofPass.cpp; change them together.

cbuffer Params : register(b0)
{
    float4 Window;      // xy = the render window's origin in scene pixels, zw = its extent in scene pixels
    float4 RayScale;    // xy = the pixel-to-NDC scale (derived from the render window), zw = its offset
    float4 InvProjRow0; // the inverse projection's rows (g_CameraParameter +0x0E0): the depth's reconstruction
    float4 InvProjRow1;
    float4 InvProjRow2;
    float4 InvProjRow3;
    float4 Centre;      // xy = the window's centre pixel (scene pixels, for the auto focus' one texel), z = seconds since the pass last ran
                        // (PS_Focus only), w = the focus cross's tap offset in pixels (PS_Focus only)
    float4 Tuning;      // x = the disc's taps, y = the reach in window pixels, w = 0
    float4 Blend;       // x = strength (already the look's effective strength), y = the pack's colour cap (10), z = the manual focus in view metres, w = 0
    float4 FocusMode;   // x = 0 auto (the centre's own depth decides, or the smoothed state in the distance look), 1 manual (Blend.z), w = 0
};

Texture2D<float> Depth : register(t0);     // the scene depth, reversed-Z (the fog post's own t0)
Texture2D<float4> Scene : register(t1);    // the scene the chain handed in (read only, never written)
Texture2D<float4> Pack : register(t2);     // this pass's own pack: rgb = the capped colour, a = the signed CoC
Texture2D<float4> NearTex : register(t3);  // the near half's blurred colour, own target; the focus update and the distance pack read the
                                           // one-texel focus state here instead (a slot nothing else binds at those draws: the explicit-state
                                           // Spec stays five slots)
Texture2D<float4> FarTex : register(t4);   // the far half's blurred colour, own target
#define FocusState NearTex

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

// The pixel-to-NDC mapping the march's rays walk (derived from the render window), and the reconstruction the
// engine's own inverse projection answers. A texel this matrix cannot reconstruct answers a negative one, and nothing
// downstream trusts it (rule 3).
float2 NdcOf(float2 windowPixel)
{
    return windowPixel * RayScale.xy + RayScale.zw;
}

float ViewDistance(float2 ndc, float depth)
{
    const float4 clip = float4(ndc, depth, 1.0);
    const float4 view4 = float4(dot(InvProjRow0, clip), dot(InvProjRow1, clip), dot(InvProjRow2, clip),
                                dot(InvProjRow3, clip));
    if (!(abs(view4.w) > 1.0e-6) || !(view4.w == view4.w))
        return -1.0; // the caller's guard: no reconstruction, no CoC
    const float viewZ = -view4.z / view4.w;
    if (!(viewZ == viewZ) || !(viewZ > 0.0))
        return -1.0;
    return viewZ;
}

// The focus band and the near cap, the rules' own numbers (DofRules.h kDeadZone and kNearCap; neither file may drift): a pixel
// whose raw CoC is within kDeadZone of the focus plane is sharp, and the near side (toward the camera) never blurs by more than
// kNearCap of the full CoC. Without them a far auto focus (the camera looking across a plaza) put everything nearer than about
// a third of the focus distance at a raw CoC of 0.5 or more, a strong near blur over the lower third of the frame.
static const float kDeadZone = 0.12;
static const float kNearCap = 0.25;

// The signed CoC, the rules' own formula (DofRules.h CoCSigned): 0 on the focus plane and inside the focus band, positive toward
// the camera, negative away from it, the pack's alpha's own unit circle. The raw CoC (focus - z) / (z + focus) is clamped to
// [-1, 1] and then shaped: the magnitude loses the dead zone and is remapped linearly, (|c| - kDeadZone) / (1 - kDeadZone), so the
// blur starts continuously at the band's edge (a hard cut would start a 0.12 CoC, a 6.5 px blur at the default reach, at once);
// the near side is capped at kNearCap and the far side is not. The focus plane and the far half of the band answer -0.0 (the form
// below negates a zero), which every reader here takes as 0. A broken focus or depth answers 0, so the pixel blurs nothing (rule 3).
float CoCSigned(float viewZ, float focus)
{
    if (!(viewZ > 0.0) || !(focus > 0.0))
        return 0.0;
    const float c = (focus - viewZ) / (viewZ + focus);
    if (!(c == c))
        return 0.0;
    const float raw = clamp(c, -1.0, 1.0);
    const float magnitude = max(abs(raw) - kDeadZone, 0.0) / (1.0 - kDeadZone);
    return raw > 0.0 ? min(magnitude, kNearCap) : -magnitude;
}

// The disc: tap i's angle on the golden-angle sweep and its radius on the sqrt ramp (DofRules.h DiscTap).
void DiscTap(float index, float steps, float reach, out float angle, out float radius)
{
    angle = index * 2.39996333f;
    radius = sqrt((index + 0.5) / steps) * reach;
}

// A tap's weight, from the tap's own CoC in the pack's alpha (DofRules.h TapWeight).
float TapWeight(float tapCoc)
{
    if (!(tapCoc == tapCoc))
        return 0.0;
    return min(abs(tapCoc), 1.0);
}

// ---- the distance look and the auto focus' smoothing: the rules' own numbers and operations (DofRules.h; neither file may drift) -----------
static const float kStartFocusMultiple = 2.0;
static const float kStartFloorMetres = 20.0;
static const float kFarOverStart = 8.0;
static const float kFocusFarRatio = 3.0;
static const float kFocusTimeConstant = 0.4;
static const float kFocusFarTimeConstant = 1.6;
static const float kFocusStaleSeconds = 1.0;

// Nothing nearer than the start is touched: the larger of a multiple of the focus and a floor in metres. A focus that is missing (zero) gives the floor.
float DistanceStart(float focus)
{
    return max(kStartFocusMultiple * focus, kStartFloorMetres);
}

// The CoC magnitude in [0, 1]: 0 up to and at the start, then the thin-lens difference anchored at the start, (1 - start/z) / (1 - start/far) with
// far = kFarOverStart x start, clamped. A broken depth (zero, negative or NaN) is no blur.
float DistanceCoC(float viewZ, float focus)
{
    if (!(viewZ > 0.0))
        return 0.0;
    const float start = DistanceStart(focus);
    if (!(viewZ > start))
        return 0.0;
    const float coc = (1.0 - start / viewZ) / (1.0 - 1.0 / kFarOverStart);
    return saturate(coc);
}

// The weight of one smoothing step, 1 - exp(-dt / tau): nothing for no time, everything for a stale gap or no time constant.
float FocusAlpha(float dt, float tau)
{
    if (!(dt > 0.0))
        return 0.0;
    if (dt >= kFocusStaleSeconds || !(tau > 0.0))
        return 1.0;
    return 1.0 - exp(-dt / tau);
}

// One step of the focus toward what the cross saw: a missing reading holds the focus, the first reading is adopted whole, a far outlier (more than
// kFocusFarRatio times the focus) is approached with the slow constant.
float FocusStep(float previous, float seen, float dt)
{
    if (!(seen > 0.0) || !isfinite(seen))
        return previous > 0.0 ? previous : 0.0;
    if (!(previous > 0.0))
        return seen;
    const float tau = seen > previous * kFocusFarRatio ? kFocusFarTimeConstant : kFocusTimeConstant;
    return previous + (seen - previous) * FocusAlpha(dt, tau);
}

// The lower median of the taps that read a depth (0 when none did).
float FocusSample(float taps[5])
{
    float valid[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
    int kept = 0;
    for (int i = 0; i < 5; ++i)
        if (taps[i] > 0.0 && isfinite(taps[i]))
        {
            valid[kept] = taps[i];
            kept += 1;
        }
    if (kept == 0)
        return 0.0;
    for (int a = 1; a < kept; ++a)
    {
        const float key = valid[a];
        int b = a;
        while (b > 0 && valid[b - 1] > key)
        {
            valid[b] = valid[b - 1];
            b -= 1;
        }
        valid[b] = key;
    }
    return valid[(kept - 1) >> 1];
}

// ---- PS_Pack: the weight+CoC pack, at the render window --------------------------------------------------------------------

float4 PS_Pack(float4 position : SV_Position) : SV_Target0
{
    // The window's absolute pixel: SV_Position is the render target's pixel (rule 1), and the pack target is the scene's
    // own allocation shape, so the pack's texel at is the scene's texel at - the depth read beside it needs no mapping.
    const int2 at = int2(position.xy);
    float4 scene = Scene.Load(int3(at, 0));
    if (!(scene.r == scene.r) || !(scene.g == scene.g) || !(scene.b == scene.b))
        scene.rgb = float3(0.0, 0.0, 0.0); // a broken source texel carries a black base into the pack, not a NaN
    // The pixel's view depth: the same reconstruction the march's rays start from.
    const float depth = Depth.Load(int3(at, 0));
    if (!(depth == depth) || !(depth >= 0.0))
        return float4(min(scene.rgb, Blend.y), 0.0); // a broken depth packs the colour and no blur at all (rule 3)
    const float viewZ = ViewDistance(NdcOf(position.xy), depth);
    // The focus: manual when the key asks for it, the window's centre texel otherwise (the auto rule).
    // A centre texel the reconstruction refuses focuses at the far plane the reconstruction reaches - an unusable focus
    // is not a broken pack, it is the far-focus edge the manual key is there for.
    float focus = Blend.z;
    if (FocusMode.x < 0.5)
    {
        const int2 last = int2(Window.xy + Window.zw) - int2(1, 1);
        const int2 centreTexel = max(min(int2(Centre.xy), last), int2(Window.xy));
        const float centreDepth = Depth.Load(int3(centreTexel, 0));
        const float centreZ = ViewDistance(NdcOf(Centre.xy), centreDepth);
        if (centreZ > 0.0)
            focus = centreZ;
    }
    return float4(min(scene.rgb, Blend.y), CoCSigned(viewZ, focus));
}

// ---- PS_PackDistance: the distance look's pack ----------------------------------------------------------------------------------

float4 PS_PackDistance(float4 position : SV_Position) : SV_Target0
{
    const int2 at = int2(position.xy);
    float4 scene = Scene.Load(int3(at, 0));
    if (!(scene.r == scene.r) || !(scene.g == scene.g) || !(scene.b == scene.b))
        scene.rgb = float3(0.0, 0.0, 0.0); // a broken source texel carries a black base into the pack, not a NaN
    const float depth = Depth.Load(int3(at, 0));
    if (!(depth == depth) || !(depth >= 0.0))
        return float4(min(scene.rgb, Blend.y), 0.0); // a broken depth packs the colour and no blur at all
    const float viewZ = ViewDistance(NdcOf(position.xy), depth);
    // The focus: the key's metres (manual), or the smoothed state PS_Focus wrote in this run (auto; zero = nothing seen yet, the floor start).
    float focus = Blend.z;
    if (FocusMode.x < 0.5)
        focus = FocusState.Load(int3(0, 0, 0)).x;
    return float4(min(scene.rgb, Blend.y), -DistanceCoC(viewZ, focus));
}

// ---- PS_Focus: the distance look's auto focus, one texel ---------------------------------------------------------------------------

float4 PS_Focus(float4 position : SV_Position) : SV_Target0
{
    const float2 state = FocusState.Load(int3(0, 0, 0)).xy;
    const int2 last = int2(Window.xy + Window.zw) - int2(1, 1);
    const float2 offsets[5] = { float2(0.0, 0.0), float2(Centre.w, 0.0), float2(-Centre.w, 0.0), float2(0.0, Centre.w), float2(0.0, -Centre.w) };
    float taps[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
    for (int i = 0; i < 5; ++i)
    {
        const float2 pixel = Centre.xy + offsets[i];
        const int2 texel = max(min(int2(pixel), last), int2(Window.xy));
        const float depth = Depth.Load(int3(texel, 0));
        if (depth == depth && depth >= 0.0)
            taps[i] = ViewDistance(NdcOf(pixel), depth); // -1 where the depth cannot be reconstructed (the sky): not a reading
    }
    const float seen = FocusSample(taps);
    const float focus = FocusStep(state.x, seen, Centre.z);
    return float4(focus, seen, 0.0, 0.0);
}

// ---- PS_Blur: the disc blur, split near and far, at a quarter of the window ------------------------------------------------

void PS_Blur(float4 position : SV_Position, out float4 nearOut : SV_Target0, out float4 farOut : SV_Target1)
{
    // The quarter pixel's window block, at its centre (rule 1: the texel k stands for the scene pixels 4k through 4k+3,
    // whose centre is 4*(k + 0.5)); the odd window's last block is clamped inside it, the march's own rule.
    const float scale = 4.0;
    float2 texel = Window.xy + position.xy * scale;
    texel = max(texel, Window.xy);
    texel = min(texel, Window.xy + Window.zw - 1.0);
    const float4 own = Pack.Load(int3(int2(texel), 0));
    // The disc taps: a fixed-function ramp of the window's own aperture, one tap one texel (rule 5), each tap included
    // when its own CoC (the pack's alpha at the tap's texel) reaches far enough to cover this pixel, weighted by that
    // same alpha, split by its sign into the near and the far class.
    const float steps = Tuning.x;
    const float reach = Tuning.y;
    if (!(reach > 0.0) || !(steps >= 2.0))
    {
        nearOut = float4(own.rgb, 0.0);
        farOut = float4(own.rgb, 0.0);
        return;
    }
    float3 nearSum = float3(0.0, 0.0, 0.0);
    float nearWeight = 0.0;
    float3 farSum = float3(0.0, 0.0, 0.0);
    float farWeight = 0.0;
    for (float i = 0.0; i < steps; i += 1.0)
    {
        float angle = 0.0;
        float radius = 0.0;
        DiscTap(i, steps, reach, angle, radius);
        const float2 tapTexel = min(max(texel + radius * float2(cos(angle), sin(angle)), Window.xy),
                                    Window.xy + Window.zw - 1.0);
        const float4 tap = Pack.Load(int3(int2(tapTexel), 0));
        const float tapCoc = tap.a;
        const float weight = TapWeight(tapCoc);
        if (!(weight > 0.0))
            continue;
        // The tap's own defocus circle: its radius in quarter pixels, at the tap's CoC times the aperture's reach. A tap
        // whose own reach stops short of the tap's distance on the disc does not reach this pixel (DofRules.h
        // TapReaches: the distance is the pre-snap radius, the same float the reference runs).
        if (!(radius <= abs(tapCoc) * reach))
            continue;
        if (tapCoc > 0.0)
        {
            nearSum += weight * tap.rgb;
            nearWeight += weight;
        }
        else
        {
            farSum += weight * tap.rgb;
            farWeight += weight;
        }
    }
    // Energy-normalized answers, or the colour this pixel's own pack texel carries when no tap of the class reached it
    // (the merge's own amount map decides what that answer is mixed as, by the pack's alpha at the window pixel).
    nearOut.rgb = nearWeight > 0.0 ? nearSum / nearWeight : own.rgb;
    farOut.rgb = farWeight > 0.0 ? farSum / farWeight : own.rgb;
    nearOut.a = nearWeight;
    farOut.a = farWeight;
}

// ---- PS_Merge: the quarter targets' bilinear readback, mixed through the pack's own alpha ----------------------------------

// The four-texel bilinear read of a quarter target at a window pixel's own coordinate (the march's own four-load read,
// at the quarter's scale). Garbage is the caller's to guard: a NaN texel read here is returned as it was.
float3 ReadQuarter(Texture2D<float4> target, float2 windowPixel)
{
    uint width = 1, height = 1;
    target.GetDimensions(width, height);
    const int2 last = int2(max(width, uint(1)), max(height, uint(1))) - int2(1, 1);
    const float2 coordinate = (windowPixel - Window.xy) / max(Window.zw, float2(1.0, 1.0)) * float2(width, height) - 0.5;
    const int2 base = int2(floor(coordinate));
    const float2 fraction = saturate(coordinate - float2(base));
    const float3 topA = target.Load(int3(clamp(base, int2(0, 0), last), 0)).rgb;
    const float3 topB = target.Load(int3(clamp(base + int2(1, 0), int2(0, 0), last), 0)).rgb;
    const float3 bottomA = target.Load(int3(clamp(base + int2(0, 1), int2(0, 0), last), 0)).rgb;
    const float3 bottomB = target.Load(int3(clamp(base + int2(1, 1), int2(0, 0), last), 0)).rgb;
    const float3 top = lerp(topA, topB, fraction.x);
    const float3 bottom = lerp(bottomA, bottomB, fraction.x);
    return lerp(top, bottom, fraction.y);
}

float4 PS_Merge(float4 position : SV_Position) : SV_Target0
{
    // The scene's own texel: the handed-in scene is the source here, never the pack's capped copy (rule 2, and the
    // in-focus colour stays byte-exact whatever the pack's cap does).
    const int2 at = int2(position.xy);
    const float4 scene = Scene.Load(int3(at, 0));
    uint packWidth = 1, packHeight = 1;
    Pack.GetDimensions(packWidth, packHeight);
    const float4 pack = Pack.Load(int3(min(at, int2(max(packWidth, uint(1)), max(packHeight, uint(1))) - int2(1, 1)), 0));
    const float cocSigned = pack.a;
    // The mix amounts, from the pack's full-resolution alpha: exactly one is ever non-zero, and Strength 0 answers both
    // at 0 (DofRules.h MergeAmounts) - the stage does not run at all at that key, and the pass holds the byte-exact copy.
    float nearAmount = 0.0;
    float farAmount = 0.0;
    const float strength = min(max(Blend.x, 0.0), 1.0);
    if (cocSigned == cocSigned)
    {
        nearAmount = max(cocSigned, 0.0) * strength;
        farAmount = max(-cocSigned, 0.0) * strength;
    }
    float3 blurred = scene.rgb;
    float amount = 0.0;
    if (nearAmount > 0.0)
    {
        amount = nearAmount;
        const float3 hit = ReadQuarter(NearTex, position.xy);
        if (hit.x == hit.x && hit.y == hit.y && hit.z == hit.z)
            blurred = hit;
    }
    else if (farAmount > 0.0)
    {
        amount = farAmount;
        const float3 hit = ReadQuarter(FarTex, position.xy);
        if (hit.x == hit.x && hit.y == hit.y && hit.z == hit.z)
            blurred = hit;
    }
    // The mix's own algebra, guarded once more at the stage's seam: no blur outside the amounts, no NaN into the merged
    // pixel (the rules' DofMix states it; the shader mirrors it with the guards above).
    const float3 merged = scene.rgb + (blurred - scene.rgb) * amount;
    return float4(merged, scene.a);
}
