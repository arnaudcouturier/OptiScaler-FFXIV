#pragma once
// Atmosphere, the tone-map chain's stage between the light bounce and the depth of field (ext/ffxiv/engine): the pure rules. No pch, no Windows, D3D or
// shader call, so every decision here is covered by tests/ext/ffxiv/engine on the host.
//
// What the stage does, in one line: at the tone-map draw, on a copy of the chain's current output, every surface pixel is mixed toward the air in front
// of it - aerial perspective in an exponential height medium with a Henyey-Greenstein lobe around the sun: out = scene * T + inscatter * (1 - T), where
// T = exp(-tau) and tau is the analytic optical depth along the view ray from a start distance to the surface. The far town and trees sink into a bright,
// sun-tinted haze, the lobe is the glow around the sun, and the near field stays clear. The tone map's t0 then reads that picture for the one draw.
//
// Where it sits: after the engine's luminance measure (the tone map is the draw after MeasureLum / AdaptLum), so eye adaptation does not undo it. The chain
// order is the tree's own: puddle -> god rays march -> light bounce -> THIS -> depth of field (the brief's position; the plan document's longer order names
// stages that do not exist yet).
//
// The maths (primary sources; REFERENCES.md has no atmosphere entry, so these are cited here):
//  - Henyey, L. G. and Greenstein, J. L., "Diffuse radiation in the Galaxy", Astrophysical Journal 93 (1941) 70-83: the phase function
//    p(c, g) = (1 - g^2) / (4 pi (1 + g^2 - 2 g c)^1.5); this stage uses it normalised to 1 at the light (c = 1), like the march's own HenyeyGreenstein,
//    because the sun strength key says how bright the glow is at the sun and the lobe says how fast it falls;
//  - Wenzel, C., "Real-time atmospheric effects in games", SIGGRAPH 2006 (Crytek): fog with a height falloff and the in-scatter toward the sun;
//  - Quilez, I., "Better fog" (2010), iquilezles.org/articles/fog: the closed form of the optical depth of an exponential height medium along a ray
//    (rho(h) = rho0 exp(-k h), tau = rho0 exp(-k h0) (1 - exp(-k dy len)) / (k dy), with the limit rho0 len exp(-k h0) when k dy -> 0);
//  - Hillaire, S., "Physically based and unified volumetric rendering in Frostbite", SIGGRAPH 2015: the compositing out = scene * T + L * (1 - T) (the
//    in-scatter of a homogeneous slab saturates at its source term, L * (1 - T)).
// The reference height is the camera's own height, so only the ray's world-up component matters (no world heights are needed per pixel).
//
// The scene is stored as the SQUARE ROOT of linear light (the light bounce adds `sqrt(s^2 + light)`; the sky texture and the fog colour are linear): the
// mix is done in linear light and stored back as its root. A pixel nearer than the start distance, a sky pixel and a stage that is off are returned as the
// stored texel itself (no round trip through the square: FP16 does not give back what it was given).
//
// Exclusions: sky and far-plane pixels get nothing (the sky is already the sky); the engine's own height fog (cFogParam / cExpHeightFogParam, which the
// god rays' fog capture also reads) already fogs the scene, so our density is scaled down by the engine's own density at the camera (EngineFogScale) and
// foggy weather is not fogged twice; a night light's low luminance scales the density down too (NightDensityScale), which is the "low density key" of the
// moon.
//
// Nothing needs a sun: the sun's direction is the fog post's own sun row (world space, present whenever the fog post runs) and its colour the light draw's
// diffuse when a read lands. No sun means the ambient term alone; the stage never declines for a sun that is behind, aside or missing.
//
// L2b (the default look): the stage is on by default; the ambient term is the engine's fog colour blended toward a neutral grey of its own luminance (the
// reference look's far haze is a bright near-neutral white, the engine's fog colour is the cyan of its sky); and the lobe is centred on the sky's VISIBLE sun
// disc (cSunParam's UV through the camera's inverse projection) when that disc agrees with the light draw's direction, because the light the engine's shadow
// cascades are built from is clamped in elevation (r1: 16.8 degrees above the disc) and a lobe on it lands in the sky, not on the land under the sun.

#include "ext/ffxiv/engine/GodraysRules.h" // FogState, FogDensityAt, kSkyDepthEpsilon, LightLuminanceGain
#include "ext/ffxiv/engine/ShaftsRules.h"  // Window, ToneMapFacts, ClassifyToneMap, ResolveSunTint, ViewDirectionOf, Luminance

#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>

namespace ext::ffxiv::engine::atmo
{
// ---- the keys -----------------------------------------------------------------------------------------------------------------

// L2b: the switch's default. ON: the installer never edits the ini, so what the user sees without the menu is this.
inline constexpr bool kDefaultEnabled = true;

// The medium's extinction at the camera's own height, in 1/km (the card shows it so; the pass takes it per metre). 0 is no medium (the stage is off).
// L2b: 3.0 -> 1.5 (on a level ray a surface 200 m away keeps 80 % of its own light at the default start, 500 m 51 %, 1 km 24 %; see l2\REPORT.md "L2b").
inline constexpr float kMinDensity = 0.0f;
inline constexpr float kMaxDensity = 60.0f;
inline constexpr float kDefaultDensity = 1.5f;
inline constexpr float kDensityEpsilon = 0.001f;
inline constexpr float kMetresPerKm = 1000.0f;

// The height falloff k in 1/m: the density at a ray point is rho0 exp(-k (h - h_camera)), so 0.01 is a scale height of 100 m. 0 is a homogeneous medium.
inline constexpr float kMinFalloff = 0.0f;
inline constexpr float kMaxFalloff = 0.1f;
inline constexpr float kDefaultFalloff = 0.01f;

// The start distance in metres: nothing nearer than this is fogged, so the first metres stay clear whatever the density is.
inline constexpr float kMinStart = 0.0f;
inline constexpr float kMaxStart = 500.0f;
inline constexpr float kDefaultStart = 50.0f; // L2b: 40 -> 50 m

// The Henyey-Greenstein anisotropy of the sun's lobe: 0 is isotropic, 0.95 a narrow forward lobe. Forward scattering only (a negative g would be a
// back-scattering lobe, which an aerosol haze does not have).
inline constexpr float kMinPhaseG = 0.0f;
inline constexpr float kMaxPhaseG = 0.95f;
inline constexpr float kDefaultPhaseG = 0.7f;

// The sun's in-scatter at the light (cos = 1), as a share of the frame's own white (linear), and the ambient term's scale on the fog colour (1 = the
// engine's own fog colour, which matches the horizon sky). 0 switches the term off.
inline constexpr float kMinSunStrength = 0.0f;
inline constexpr float kMaxSunStrength = 4.0f;
inline constexpr float kDefaultSunStrength = 0.3f; // L2b: 0.5 -> 0.3 (the lobe is on the visible disc now, and a stronger one lifts the far sea above the sky it meets: see the seam table)
inline constexpr float kMinSkyStrength = 0.0f;
inline constexpr float kMaxSkyStrength = 4.0f;
inline constexpr float kDefaultSkyStrength = 1.0f;

// The share of the density that stays at night (the light draw's own luminance is the day weight): the moon's "low density key".
inline constexpr float kMinNightDensity = 0.0f;
inline constexpr float kMaxNightDensity = 1.0f;
inline constexpr float kDefaultNightDensity = 0.1f;

// L2b: how much of the ambient term's colour is replaced by a neutral grey of the same luminance: 0 is the engine's own fog colour (cyan under a clear sky), 1 a
// pure neutral. The reference look's far haze has OKLab chroma 0.007-0.010; the engine's colour has 0.035-0.039 on the lab's frames.
inline constexpr float kMinNeutral = 0.0f;
inline constexpr float kMaxNeutral = 1.0f;
inline constexpr float kDefaultNeutral = 0.6f;

constexpr float ClampTo(float value, float low, float high, float fallback) noexcept
{
    if (!(value == value))
        return fallback; // a NaN reads as the default, so a broken value never changes the picture
    return value < low ? low : (value > high ? high : value);
}

constexpr float ClampDensity(float value) noexcept { return ClampTo(value, kMinDensity, kMaxDensity, kDefaultDensity); }
constexpr float ClampFalloff(float value) noexcept { return ClampTo(value, kMinFalloff, kMaxFalloff, kDefaultFalloff); }
constexpr float ClampStart(float value) noexcept { return ClampTo(value, kMinStart, kMaxStart, kDefaultStart); }
constexpr float ClampPhaseG(float value) noexcept { return ClampTo(value, kMinPhaseG, kMaxPhaseG, kDefaultPhaseG); }
constexpr float ClampSunStrength(float value) noexcept { return ClampTo(value, kMinSunStrength, kMaxSunStrength, kDefaultSunStrength); }
constexpr float ClampSkyStrength(float value) noexcept { return ClampTo(value, kMinSkyStrength, kMaxSkyStrength, kDefaultSkyStrength); }
constexpr float ClampNightDensity(float value) noexcept { return ClampTo(value, kMinNightDensity, kMaxNightDensity, kDefaultNightDensity); }
constexpr float ClampNeutral(float value) noexcept { return ClampTo(value, kMinNeutral, kMaxNeutral, kDefaultNeutral); }

struct Params
{
    float density = kDefaultDensity;      // 1/km at the camera's height
    float falloff = kDefaultFalloff;      // 1/m
    float start = kDefaultStart;          // m
    float phaseG = kDefaultPhaseG;
    float sunStrength = kDefaultSunStrength;
    float skyStrength = kDefaultSkyStrength;
    float nightDensity = kDefaultNightDensity;
    float neutral = kDefaultNeutral;
};

constexpr Params BuildParams(float density, float falloff, float start, float phaseG, float sunStrength, float skyStrength, float nightDensity,
                             float neutral = kDefaultNeutral) noexcept
{
    Params params;
    params.density = ClampDensity(density);
    params.falloff = ClampFalloff(falloff);
    params.start = ClampStart(start);
    params.phaseG = ClampPhaseG(phaseG);
    params.sunStrength = ClampSunStrength(sunStrength);
    params.skyStrength = ClampSkyStrength(skyStrength);
    params.nightDensity = ClampNightDensity(nightDensity);
    params.neutral = ClampNeutral(neutral);
    return params;
}

// Whether the stage wants a draw: the switch is on, there is a medium, and there is some light for it to scatter. A key set to "nothing to do" is
// today's picture at no cost, so the draw path costs one relaxed load and the scene handed in stays the scene the game's tone map reads.
constexpr bool Wanted(bool enabled, const Params& params) noexcept
{
    return enabled && params.density > kDensityEpsilon && (params.sunStrength > kDensityEpsilon || params.skyStrength > kDensityEpsilon);
}

// ---- the medium ---------------------------------------------------------------------------------------------------------------

// The exponent bounds the closed form keeps: a ray that looks down into a valley sees the density grow as exp(-k dy s), and one that looks up sees it
// die away. Both are clamped so a steep ray from a high camera never answers an infinity, and the optical depth is capped so T never underflows.
inline constexpr float kMaxDensityGain = 6.0f;    // exp(6) = 403 times the camera-height density, at most
inline constexpr float kMaxFallExponent = 40.0f;  // beyond this the density is gone
inline constexpr float kMaxOpticalDepth = 12.0f;  // T >= 6e-6

// The optical depth along a view ray from `start` to `distance` metres, in an exponential height medium whose density at the camera's height is
// `rho0` (1/m) and whose density at a point of the ray is rho0 exp(-falloff * up * s) (the reference height is the camera's, so only the ray's world-up
// component `up` matters). Closed form with the series limit near zero; zero when the surface is nearer than the start, when there is no medium, or when
// anything is not a number. Quilez 2010: tau = rho0 exp(-a s0) (1 - exp(-a len)) / a, with a = falloff * up.
inline float OpticalDepth(float rho0, float falloff, float up, float start, float distance) noexcept
{
    if (!(rho0 > 0.0f) || !(distance > start) || !(start >= 0.0f) || !(falloff == falloff) || !(up == up) || !(distance < 1.0e8f))
        return 0.0f;
    const float len = distance - start;
    const float a = falloff * up; // 1/m: the density's change rate along the ray
    const float f0 = ClampTo(-a * start, -kMaxFallExponent, kMaxDensityGain, 0.0f); // the exponent at the segment's start
    const float x = ClampTo(a * len, -kMaxDensityGain, kMaxFallExponent, 0.0f);      // the exponent's total change over the segment
    // The mean of exp(-a (s - s0)) over the segment: (1 - exp(-x)) / x, with its series near zero.
    const float mean = std::fabs(x) < 1.0e-3f ? 1.0f - 0.5f * x + x * x / 6.0f : -std::expm1(-x) / x;
    const float tau = rho0 * std::exp(f0) * len * mean;
    if (!(tau == tau) || tau <= 0.0f)
        return 0.0f;
    return tau > kMaxOpticalDepth ? kMaxOpticalDepth : tau;
}

// The transmittance of an optical depth: exactly 1 for none (the caller's early-out relies on it: no medium on the ray, no change to the texel).
inline float Transmittance(float tau) noexcept
{
    if (!(tau > 0.0f))
        return 1.0f;
    return std::exp(-(tau > kMaxOpticalDepth ? kMaxOpticalDepth : tau));
}

// The engine's own fog: its density at the camera's height (FogDensityAt with the profile's own lift of 1) fogs the scene already, so the density this stage
// uses is scaled down as the engine's rises above what a clear day has. Clear weather leaves our density alone (the captured frames' engine density at the
// camera is 0.0009-0.0024 per metre, and its fog starts 300-400 m out); above kEngineFogClear the scale falls as 1 / (1 + excess / kEngineFogSpan), so a
// weather that fogs the air at ten times the clear rate leaves a quarter of ours, and the air is not fogged twice.
inline constexpr float kEngineFogClear = 0.003f; // 1/m
inline constexpr float kEngineFogSpan = 0.003f;  // 1/m

constexpr float EngineFogScale(float engineDensity) noexcept
{
    if (!(engineDensity == engineDensity) || !(engineDensity > kEngineFogClear))
        return 1.0f;
    return 1.0f / (1.0f + (engineDensity - kEngineFogClear) / kEngineFogSpan);
}

// The day weight of the light the frame's light draw reported: 0 at the moon's luminance, 1 at the sun's (the march's own smoothstep between 0.05 and
// 0.6). A frame with no light read at all is a day (the weight 1): nothing says it is dark.
inline float NightDensityScale(float nightDensity, float luminance, bool haveLight) noexcept
{
    if (!haveLight)
        return 1.0f;
    const float day = godrays::LightLuminanceGain(luminance);
    const float night = ClampNightDensity(nightDensity);
    return night + (1.0f - night) * day;
}

// The density the pass uses, per metre: the key (1/km) times the engine-fog scale times the night scale.
inline float EffectiveDensityPerMetre(const Params& params, float engineDensity, float luminance, bool haveLight) noexcept
{
    return params.density / kMetresPerKm * EngineFogScale(engineDensity) * NightDensityScale(params.nightDensity, luminance, haveLight);
}

// ---- the light ----------------------------------------------------------------------------------------------------------------

// The Henyey-Greenstein lobe, normalised to 1 at the light: (1 - g)^3 / (1 + g^2 - 2 g c)^1.5, written with (1 - g)^2 + 2 g (1 - c) so that c = 1 is
// exact. g is this stage's own key (0 .. 0.95); c is the cosine between the view ray (from the camera toward the surface) and the direction TOWARD the sun,
// so c = 1 looks into the sun and the lobe is its glow.
inline float SunPhase(float cosTheta, float g) noexcept
{
    const float gg = ClampPhaseG(g);
    const float c = cosTheta != cosTheta ? 0.0f : (cosTheta < -1.0f ? -1.0f : (cosTheta > 1.0f ? 1.0f : cosTheta));
    const float oneMinus = 1.0f - gg;
    const float denominator = oneMinus * oneMinus + 2.0f * gg * (1.0f - c);
    return oneMinus * oneMinus * oneMinus / std::pow(denominator > 1.0e-6f ? denominator : 1.0e-6f, 1.5f);
}

// The sun's share of its own strength, from what the frame knows of it: the light draw's diffuse luminance when it reported one (0 at 0, 1 at the sun's
// 0.6), else the fog sun's elevation sine (the sun's own rise, 0 at the horizon, 1 from 0.25 up). No sun at all is 0: the stage runs on its ambient term.
inline float SunGain(bool haveSun, bool haveLuminance, float luminance, float elevationSine) noexcept
{
    if (!haveSun)
        return 0.0f;
    if (haveLuminance && luminance == luminance)
        return ClampTo(luminance / godrays::kLightFullLuminance, 0.0f, 1.0f, 0.0f);
    return ClampTo((elevationSine + 0.02f) / 0.27f, 0.0f, 1.0f, 0.0f);
}

// The ambient term's colour (L2b): the engine's fog colour (negative channels are no light) blended toward a grey of its own luminance (Rec. 709 weights, linear
// light) by `neutral`: 0 is the engine's colour, 1 a pure neutral, and the luminance is the same at every share.
inline void NeutralAmbient(const float fogColour[3], float neutral, float out[3]) noexcept
{
    const float share = ClampNeutral(neutral);
    float colour[3];
    for (uint32_t i = 0; i < 3; ++i)
        colour[i] = fogColour[i] > 0.0f ? fogColour[i] : 0.0f;
    const float grey = shafts::Luminance(colour);
    for (uint32_t i = 0; i < 3; ++i)
        out[i] = colour[i] * (1.0f - share) + grey * share;
}

// The air's own light along one ray, in LINEAR light units of the scene: the ambient term (the engine's fog colour, neutralised by the key, times the sky key)
// plus the sun's lobe (the frame's linear white times the sun key, the light's colour of peak 1, the sun's gain and the phase at this pixel).
struct Inscatter
{
    float rgb[3] { 0.0f, 0.0f, 0.0f };
};

inline Inscatter InscatterOf(const Params& params, const float fogColour[3], const float sunTint[3], float whiteLinear, float sunGain, float cosTheta) noexcept
{
    Inscatter out;
    const float phase = SunPhase(cosTheta, params.phaseG);
    float ambient[3];
    NeutralAmbient(fogColour, params.neutral, ambient);
    for (uint32_t i = 0; i < 3; ++i)
        out.rgb[i] = params.skyStrength * ambient[i] + params.sunStrength * whiteLinear * sunGain * phase * sunTint[i];
    return out;
}

// The linear white of a frame, from the stored white the chain reads (the scene is stored as the root of linear light: 0.838 stored is 0.702 linear).
constexpr float WhiteLinearOf(float storedWhite) noexcept { return storedWhite * storedWhite; }

inline bool ViewPositionOf(const float inverseProjection[16], float ndcX, float ndcY, float depth, float out[3]) noexcept; // defined with the surface rules below

// ---- the lobe's direction (L2b) ----------------------------------------------------------------------------------------------------

// The sky draws its sun sprite at the UV cSunParam row 0 holds (the tone map's UV: 0 at the window's top-left, y down). Through the camera's inverse projection the
// UV is a ray in view space; the ray's direction does not depend on the depth used (the camera is the origin), so any positive depth gives it. False when the
// UV is not a number or the projection is degenerate.
inline bool SunDirectionFromUv(const float inverseProjection[16], float uvX, float uvY, float out[3]) noexcept
{
    if (!(uvX == uvX) || !(uvY == uvY) || !(std::fabs(uvX) < 1.0e6f) || !(std::fabs(uvY) < 1.0e6f))
        return false;
    float position[3];
    if (!ViewPositionOf(inverseProjection, uvX * 2.0f - 1.0f, 1.0f - uvY * 2.0f, 0.5f, position))
        return false;
    const float length = std::sqrt(position[0] * position[0] + position[1] * position[1] + position[2] * position[2]);
    if (!(length > 1.0e-6f) || !(length < 1.0e30f))
        return false;
    for (uint32_t i = 0; i < 3; ++i)
        out[i] = position[i] / length;
    return true;
}

// The engine clamps the shadow-casting light's ELEVATION (so its cascades do not stretch to infinity), never its azimuth: the visible disc is below the light and
// in the same azimuth. So the disc is taken for the lobe's direction only when it agrees with the light in azimuth (25 degrees), is not above it (the clamp only
// lifts the light; a small slack for the two readings' own noise), and is fresh. Near the zenith the azimuth means nothing, and the total angle decides
// (35 degrees). A sun behind the camera mirrors its UV to the front: its azimuth is the opposite of the light's and it is refused.
inline constexpr float kDiscAzimuthCosine = 0.9063f;   // cos 25 degrees
inline constexpr float kDiscZenithAngleCosine = 0.8192f; // cos 35 degrees, where the azimuth is undefined
inline constexpr float kDiscAboveLightSlack = 0.05f;   // in the sine of the elevation
inline constexpr float kDiscNoAzimuth = 0.04f;         // the squared horizontal part below which the azimuth is undefined (elevation sine above 0.98)

struct SunChoice
{
    float dir[3] { 0.0f, 1.0f, 0.0f };
    bool disc = false; // the lobe follows the visible disc
};

inline SunChoice ChooseSunDirection(const float light[3], bool haveLight, const float disc[3], bool discFresh, const float upView[3]) noexcept
{
    SunChoice choice;
    if (!haveLight || light == nullptr)
        return choice; // no light: the stage runs on its ambient term; there is nothing for a disc to agree with
    for (uint32_t i = 0; i < 3; ++i)
        choice.dir[i] = light[i];
    if (!discFresh || disc == nullptr || upView == nullptr)
        return choice;
    float lightHorizontal[3], discHorizontal[3];
    float lightLength = 0.0f, discLength = 0.0f;
    float lightUp = 0.0f, discUp = 0.0f, between = 0.0f;
    for (uint32_t i = 0; i < 3; ++i)
    {
        lightLength += light[i] * light[i];
        discLength += disc[i] * disc[i];
        lightUp += light[i] * upView[i];
        discUp += disc[i] * upView[i];
        between += light[i] * disc[i];
    }
    if (!(lightLength > 0.9f && lightLength < 1.1f) || !(discLength > 0.9f && discLength < 1.1f) || !(lightUp == lightUp) || !(discUp == discUp) || !(between == between))
        return choice; // not unit vectors (a broken or missing read)
    if (discUp > lightUp + kDiscAboveLightSlack)
        return choice; // the clamp only lifts the light: a disc above it is not that disc
    float horizontalDot = 0.0f, lightH2 = 0.0f, discH2 = 0.0f;
    for (uint32_t i = 0; i < 3; ++i)
    {
        lightHorizontal[i] = light[i] - upView[i] * lightUp;
        discHorizontal[i] = disc[i] - upView[i] * discUp;
        horizontalDot += lightHorizontal[i] * discHorizontal[i];
        lightH2 += lightHorizontal[i] * lightHorizontal[i];
        discH2 += discHorizontal[i] * discHorizontal[i];
    }
    bool agrees;
    if (lightH2 < kDiscNoAzimuth || discH2 < kDiscNoAzimuth)
        agrees = between >= kDiscZenithAngleCosine;
    else
        agrees = horizontalDot >= kDiscAzimuthCosine * std::sqrt(lightH2) * std::sqrt(discH2);
    if (!agrees)
        return choice;
    for (uint32_t i = 0; i < 3; ++i)
        choice.dir[i] = disc[i];
    choice.disc = true;
    return choice;
}

// ---- the camera the frame stands on (CAMFIX) -----------------------------------------------------------------------------------------

// The stage needs a camera (the inverse projection the depth is reconstructed through, the view rows the up vector and the sun come from, the camera's world height);
// it does NOT need a sun. Two copies of the engine's one camera table can supply it:
//   the SUN DRAW's copy - taken at the frame's first light draw that has the sun draw's shape; it carries the light too, so it is the only copy that can feed the sun;
//   the LIGHT DRAW's copy - taken at the frame's first light draw of ANY shape (depth of field's own source, which is why the depth of field and the light bounce run in
//     lighting layouts with no sun-shaped light draw at all, e.g. the benchmark's, and in the zone transitions that have none); it carries the camera alone.
// Both are the same buffer object (the light draws and the deferred composite bind one 1024-byte g_CameraParameter in every live capture), decoded by the same reader and
// verified by the same rule (ProjectionOk and ViewProjectionOk). The sun draw's copy stays the first choice where it exists.

// A camera that has not been refreshed for this many rendered frames is not this frame's (a view that turned since): the frames after it are skipped until the next read
// lands, never drawn with a stale up vector. The sun the sun draw's copy carried stands for the same time after that copy, and no longer: a fresh camera from the light
// draws' copy must never keep drawing a sun from a zone the player has left.
inline constexpr uint64_t kMaxCameraAge = 180;

enum class CameraSource : uint8_t
{
    None = 0,
    SunDraw,   // the sun draw's copy (camera, light and sun)
    LightDraw, // the light draws' copy (the camera alone)
};

constexpr const char* CameraSourceName(CameraSource source) noexcept
{
    switch (source)
    {
    case CameraSource::None: return "no camera";
    case CameraSource::SunDraw: return "the sun draw's copy";
    case CameraSource::LightDraw: return "the light draws' copy (no sun-shaped light draw this frame)";
    }
    return "unknown";
}

// What one ring read of the frame came to, as far as the camera goes: the copy was mapped, and its camera bytes are this build's camera.
struct CameraCopy
{
    bool landed = false;
    bool verified = false;
};

// What the hold stands on: the rendered frame the camera was last refreshed at, and the rendered frame the sun draw's copy last landed verified at.
struct HoldStamps
{
    bool haveCamera = false;
    uint64_t cameraFrame = 0;
    bool haveSunDraw = false;
    uint64_t sunDrawFrame = 0;
};

struct CameraFold
{
    CameraSource source = CameraSource::None;
    HoldStamps stamps;
};

// The frame's two reads folded into the hold's stamps. The sun draw's copy wins when it landed with a camera this build reads (and only it refreshes the sun's stamp);
// else the light draws' copy when it landed with one; else nothing is refreshed and the stamps age. A copy that landed and is not this build's camera is no camera.
constexpr CameraFold FoldCameraSources(const HoldStamps& held, CameraCopy sunDraw, CameraCopy lightDraw, uint64_t frames) noexcept
{
    CameraFold fold;
    fold.stamps = held;
    if (sunDraw.landed && sunDraw.verified)
    {
        fold.source = CameraSource::SunDraw;
        fold.stamps.haveCamera = true;
        fold.stamps.cameraFrame = frames;
        fold.stamps.haveSunDraw = true;
        fold.stamps.sunDrawFrame = frames;
    }
    else if (lightDraw.landed && lightDraw.verified)
    {
        fold.source = CameraSource::LightDraw;
        fold.stamps.haveCamera = true;
        fold.stamps.cameraFrame = frames;
    }
    return fold;
}

constexpr bool StampStands(bool have, uint64_t stamp, uint64_t frames) noexcept { return have && frames >= stamp && frames - stamp <= kMaxCameraAge; }

// Whether the hold's camera is young enough to draw with (false: the frame is skipped, "camera").
constexpr bool CameraStands(const HoldStamps& stamps, uint64_t frames) noexcept { return StampStands(stamps.haveCamera, stamps.cameraFrame, frames); }

// Whether the sun the hold carries is young enough to draw with (false: no sun lobe and no tint from it, the ambient term alone). It does NOT age the day/night weight of
// the density: that follows the light's luminance, which is the time of day and not the zone, so the last luminance read keeps standing.
constexpr bool SunStands(const HoldStamps& stamps, uint64_t frames) noexcept { return StampStands(stamps.haveSunDraw, stamps.sunDrawFrame, frames); }

// The one line a change of camera source writes (rate-limited by the caller): which copy the frame's camera comes from now.
inline std::string FormatCameraSource(CameraSource source)
{
    return std::format("Atmosphere: the frame's camera comes from {} now", CameraSourceName(source));
}

// ---- the surface ---------------------------------------------------------------------------------------------------------------

// A sky or far-plane pixel (reversed Z: the far plane is 0) and anything the depth cannot say is not a surface: it gets nothing.
constexpr bool IsAtmosphereSurface(float depth) noexcept { return depth == depth && depth > godrays::kSkyDepthEpsilon; }

// The view-space position a depth reconstructs through the engine's inverse projection (the fog post's own form, the same reconstruction DoF walks): the
// clip point (ndc.x, ndc.y, depth, 1) through the four rows, divided by w. False when the point is not finite or w is degenerate.
inline bool ViewPositionOf(const float inverseProjection[16], float ndcX, float ndcY, float depth, float out[3]) noexcept
{
    float v[4];
    for (uint32_t row = 0; row < 4; ++row)
        v[row] = inverseProjection[row * 4 + 0] * ndcX + inverseProjection[row * 4 + 1] * ndcY + inverseProjection[row * 4 + 2] * depth + inverseProjection[row * 4 + 3];
    if (!(std::fabs(v[3]) > 1.0e-6f) || !(v[3] == v[3]))
        return false;
    for (uint32_t i = 0; i < 3; ++i)
        out[i] = v[i] / v[3];
    return out[0] == out[0] && out[1] == out[1] && out[2] == out[2];
}

// The stored texel's mix, one channel: linear mix of the scene's own light and the air's, stored back as its root. T = 1 returns the stored texel itself.
inline float ComposeStored(float stored, float transmittance, float inscatterLinear) noexcept
{
    if (!(transmittance < 1.0f))
        return stored;
    const float scene = stored > 0.0f ? stored * stored : 0.0f;
    const float mixed = scene * transmittance + (inscatterLinear > 0.0f ? inscatterLinear : 0.0f) * (1.0f - transmittance);
    return std::sqrt(mixed > 0.0f ? mixed : 0.0f);
}

// ---- outcomes, declines and skips (the chain's hold semantics) ------------------------------------------------------------------

enum class Decline : uint8_t
{
    None = 0,
    FeatureOff,   // the key is off, or the medium is empty, or the stage stood down for the session
    NotToneMap,   // the draw is not the tone map's shape
    RenderTarget, // the tone map's render target is not one post-tone texture
    TargetFormat, // the tone map's target is not FP16
    SceneFormat,  // the scene is not FP16
    SizeMismatch, // the scene, the depth and the render window do not belong to one frame
    Window,       // the render window is empty, too small or does not fit the scene
    CharaReplay,  // the draw is a character-preview replay
    SceneHdr,     // the scene HDR path replaces the tone chain this stage draws before
    NoDepth,      // the fog post has not given this module a depth of the scene's own size yet
    NoCamera,     // the frame's camera rows never landed or are not this build's camera
    NoFog,        // the fog post's fog state (the engine's fog colour and density) has not landed or is not usable
    PassFailed,   // the pass could not build its textures or run its draw
    Exception,    // a call threw
};

constexpr const char* DeclineName(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::None: return "run";
    case Decline::FeatureOff: return "the key is off, or the medium is empty";
    case Decline::NotToneMap: return "the draw is not the tone map";
    case Decline::RenderTarget: return "the tone map's render target is not one post-tone texture";
    case Decline::TargetFormat: return "the tone map's target is not FP16";
    case Decline::SceneFormat: return "the scene is not FP16";
    case Decline::SizeMismatch: return "the scene, the depth and the render window do not belong to one frame";
    case Decline::Window: return "the render window is empty or does not fit the scene";
    case Decline::CharaReplay: return "the draw is a character-preview replay";
    case Decline::SceneHdr: return "the engine's scene HDR path is on, which replaces this stage's tone chain";
    case Decline::NoDepth: return "no depth of the scene's own size was secured by the fog post";
    case Decline::NoCamera: return "no camera this build can read has landed";
    case Decline::NoFog: return "the fog post's fog state has not landed or is not usable";
    case Decline::PassFailed: return "the pass could not build its textures, shaders or draw";
    case Decline::Exception: return "a call threw";
    }
    return "unknown";
}

// What the chain came to (the DoF stage's own outcomes): a chained decline leaves the caller's texture at t0 (the hold stands), an owner decline leaves the
// game's own binding and draws nothing (the caller draws the game's call once).
enum class Outcome : uint8_t
{
    Answered = 0,
    HoldStands,
    EngineOwn,
    Skipped,
};

// Which declines count toward the stand-down: a shape the stage cannot serve is a build mismatch or a capture that stopped landing; "the draw is not the
// tone map's" is not a failure, and neither is a frame that has no medium, no depth yet, no camera yet or no fog yet (they are skips).
constexpr bool CountsTowardsStandDown(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::RenderTarget:
    case Decline::TargetFormat:
    case Decline::SceneFormat:
    case Decline::SizeMismatch:
    case Decline::Window:
    case Decline::PassFailed:
        return true;
    case Decline::None:
    case Decline::FeatureOff:
    case Decline::NotToneMap:
    case Decline::CharaReplay:
    case Decline::SceneHdr:
    case Decline::NoDepth:
    case Decline::NoCamera:
    case Decline::NoFog:
    case Decline::Exception:
        return false;
    }
    return false;
}

inline constexpr uint32_t kConsecutiveDeclineLimit = 64;

// The stage's own refusal streak: 64 counted refusals in a row switch the stage off for the session (the other stages' rule; a run starts the streak over).
struct Streak
{
    uint32_t declines = 0;
    bool off = false;

    // True when this decline is the one that switched the stage off.
    bool NoteDecline() noexcept
    {
        if (off)
            return false;
        if (++declines >= kConsecutiveDeclineLimit)
        {
            off = true;
            return true;
        }
        return false;
    }
    void NoteRun() noexcept { declines = 0; }
    bool Armed() const noexcept { return !off; }
};

enum class SkipReason : uint8_t
{
    None = 0,
    NoDepth,
    NoCamera,
    NoFog,
};

constexpr SkipReason SkipOf(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::NoDepth: return SkipReason::NoDepth;
    case Decline::NoCamera: return SkipReason::NoCamera;
    case Decline::NoFog: return SkipReason::NoFog;
    default: return SkipReason::None;
    }
}

constexpr const char* SkipName(SkipReason reason) noexcept
{
    switch (reason)
    {
    case SkipReason::None: return "nothing to skip";
    case SkipReason::NoDepth: return "the fog post's depth has not been secured yet";
    case SkipReason::NoCamera: return "the frame's camera rows never landed or have stopped landing";
    case SkipReason::NoFog: return "the fog post's fog state never landed or is not usable";
    }
    return "unknown";
}

struct SkipCounts
{
    uint32_t skipped = 0;
    uint32_t noDepth = 0;
    uint32_t noCamera = 0;
    uint32_t noFog = 0;
};

constexpr void CountSkip(SkipCounts& counts, SkipReason reason) noexcept
{
    switch (reason)
    {
    case SkipReason::NoDepth: ++counts.noDepth; break;
    case SkipReason::NoCamera: ++counts.noCamera; break;
    case SkipReason::NoFog: ++counts.noFog; break;
    case SkipReason::None: break;
    }
    if (reason != SkipReason::None)
        ++counts.skipped;
}

inline std::string FormatDecline(Decline reason, Outcome outcome)
{
    if (outcome == Outcome::HoldStands)
        return std::format("Atmosphere: the draw was left alone, so the chain's hold stands ({})", DeclineName(reason));
    return std::format("Atmosphere: the draw was left alone, so the frame is the engine's own ({})", DeclineName(reason));
}

inline std::string FormatSkip(SkipReason reason) { return std::format("Atmosphere: the frame has no atmosphere in it ({})", SkipName(reason)); }

// What one run measured, for the run line and the stage dump: the numbers a session needs to see what the medium did.
struct RunFacts
{
    Params params;
    float engineDensity = 0.0f;   // the engine's own fog density at the camera (1/m)
    float engineScale = 1.0f;     // EngineFogScale of it
    float dayScale = 1.0f;        // NightDensityScale
    float densityPerMetre = 0.0f; // what the pass used
    bool haveSun = false;
    bool haveLight = false;
    float sunGain = 0.0f;
    bool lobeOnDisc = false; // L2b: the lobe follows the sky's visible sun disc (else the light draw's own direction)
    float elevationSine = 0.0f;
    float whiteStored = 0.0f;
    uint32_t windowWidth = 0, windowHeight = 0;
};

inline std::string FormatRun(const RunFacts& facts)
{
    return std::format("Atmosphere: mixed the window's {}x{} toward the air: density {:.2f}/km x engine fog scale {:.2f} (engine {:.4f}/m) x night scale {:.2f} = {:.5f}/m, "
                       "falloff {:.4f}/m, start {:.0f} m, phase g {:.2f}, sun {:.2f} ({}, gain {:.2f}, elevation sine {:.2f}, lobe on the {}), sky {:.2f}, neutral {:.2f}, white {:.3f}",
                       facts.windowWidth, facts.windowHeight, double(facts.params.density), double(facts.engineScale), double(facts.engineDensity),
                       double(facts.dayScale), double(facts.densityPerMetre), double(facts.params.falloff), double(facts.params.start),
                       double(facts.params.phaseG), double(facts.params.sunStrength), facts.haveSun ? (facts.haveLight ? "sun and light colour" : "fog sun") : "no sun",
                       double(facts.sunGain), double(facts.elevationSine), facts.lobeOnDisc ? "visible disc" : "light", double(facts.params.skyStrength),
                       double(facts.params.neutral), double(facts.whiteStored));
}

// The line a change of the engine's height fog writes (FOGFIX): the engine's profile went from fogged to none or back. A zone with no height fog (both layers' densities zero,
// which clear weather binds correctly laid out) leaves our density whole (engine fog scale 1) and the air's ambient term is the engine's own fog colour in both states.
inline std::string FormatEngineFog(bool none, float densityAtCamera, float scale, const float colour[3])
{
    return std::format("Atmosphere: the engine's height fog profile is {} now: density at the camera {:.4f}/m, engine fog scale {:.2f}, fog colour ({:.3f}, {:.3f}, {:.3f}) feeds the air's ambient term",
                       none ? "ZERO (no height fog in this zone)" : "fogged", double(densityAtCamera), double(scale), double(colour[0]), double(colour[1]), double(colour[2]));
}

// The one line the first frame whose lobe follows the visible disc writes: where the sky draws its sun and how far that is from the light draw's own direction.
inline std::string FormatSunDisc(float uvX, float uvY, float degreesFromLight)
{
    return std::format("Atmosphere: the sky draws its sun at uv ({:.4f}, {:.4f}), so the lobe follows the visible sun, {:.1f} degrees from where the shadow-casting light points", double(uvX),
                       double(uvY), double(degreesFromLight));
}

inline std::string FormatCost(bool haveGpuMilliseconds, double gpuMilliseconds, uint64_t frames, uint32_t runs, uint32_t declined)
{
    const std::string gpu = haveGpuMilliseconds ? std::format(", the pass {:.4f} ms on the GPU", gpuMilliseconds) : std::string(", its GPU time not measured yet");
    return std::format("Atmosphere: started on the tone-map chain ({} runs, {} declined over {} frames{})", runs, declined, frames, gpu);
}

struct CardCounts
{
    uint64_t frames = 0;
    uint32_t runs = 0;
    uint32_t declined = 0;
    SkipCounts skips;
    bool armed = false;
    Params params;
};

inline std::string FormatCard(const CardCounts& counts)
{
    return std::format("Atmosphere: {}{} frames, {} runs, {} declined, {} skipped (depth {}, camera {}, fog {}); density {:.2f}/km, falloff {:.4f}/m, start {:.0f} m, g {:.2f}, sun {:.2f}, sky {:.2f}, night {:.2f}, neutral {:.2f}",
                       counts.armed ? "" : "off for this session: ", counts.frames, counts.runs, counts.declined, counts.skips.skipped, counts.skips.noDepth,
                       counts.skips.noCamera, counts.skips.noFog, double(counts.params.density), double(counts.params.falloff), double(counts.params.start),
                       double(counts.params.phaseG), double(counts.params.sunStrength), double(counts.params.skyStrength), double(counts.params.nightDensity),
                       double(counts.params.neutral));
}

inline constexpr const char* kSceneHdrInactive =
    "Atmosphere: inactive while the engine's scene HDR is on, because that path replaces the tone map this stage draws before";

// The stage dump's values string, one line.
inline std::string FormatStageValues(const Params& params)
{
    return std::format("density={:.2f} falloff={:.4f} start={:.1f} g={:.2f} sun={:.2f} sky={:.2f} night={:.2f} neutral={:.2f}", double(params.density), double(params.falloff),
                       double(params.start), double(params.phaseG), double(params.sunStrength), double(params.skyStrength), double(params.nightDensity),
                       double(params.neutral));
}
} // namespace ext::ffxiv::engine::atmo
