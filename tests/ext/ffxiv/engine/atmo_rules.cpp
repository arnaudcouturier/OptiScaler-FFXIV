// Atmosphere: the pure rules on the host (no device).
//
// What a wrong rule would break, and what is checked here:
//   the keys: every default (L2b: on by default, density 1.5, start 50, sun 0.3, neutral 0.6), every clamp (NaN reads as the default, the ends pinch), Wanted (off, no medium, no light to scatter);
//   the neutral ambient (L2b): the engine's fog colour blended toward a grey of its own luminance, 0 = the engine's colour, 1 = neutral, luminance preserved, negative channels no light;
//   the lobe's direction (L2b): the sky's visible sun disc through the inverse projection, accepted only when it agrees with the light in azimuth and is not above it;
//   the optical depth: zero nearer than the start distance and with no medium, exactly rho0 * (s1 - s0) in a homogeneous medium and on a horizontal ray,
//     equal to a numeric integral of the density along the ray for rays that look up and down, monotone in the distance, smaller with a later start,
//     continuous across the series limit, capped, NaN-proof;
//   the transmittance: exactly 1 for no medium (the caller's early-out relies on it), exp(-tau) otherwise;
//   the sun phase: 1 at the light for every g, isotropic at g = 0, forward greater than backward, monotone, the exact value at a reference point;
//   the engine-fog scale: 1 on a clear day and for a broken number, below 1 above it, monotone;
//   the night scale and the effective density: no light is a day, the moon's luminance keeps the key's share, the sun's keeps all of it;
//   FOGFIX (the user's own 2026-11-07 captures, rows in live_fog_fixtures.h): a clear-weather zone binds a correctly laid out profile with every density zero, so the engine's density
//     at the camera is exactly 0 (engine fog scale 1, the key's whole density, the night share at night), the engine's fog colour is non-black in all three captures and makes the
//     ambient term; a zone change from fogged (r2: 0.0041 per metre) to clear never lets the effective density drop to nothing, moves it monotonically and by at most exp(change of the engine's density / span) a
//     frame (2.3 % over a 60-frame fade of r2's density: the rule is Lipschitz, a fade of the engine's density is a fade of the haze), and a one-frame step is bounded by 1 / EngineFogScale (stated: the engine's own fog vanishes in that same frame);
//   CAMFIX (the camera does not depend on the sun-shaped light draw): the fold of the frame's two camera copies (the sun draw's, the light draws') - the sun draw's copy is the first choice, the
//     light draws' copy stands in when it did not land or its table is refused, a table that is not this build's camera is no camera, only the sun draw's copy refreshes the sun's stamp - and the
//     ages: a camera stands 180 frames after its refresh and not one more, the sun the same after the sun draw's copy; driven as frame sequences (a benchmark with no sun-shaped light draw in 1,000
//     frames never skips "camera" and never has a sun, both copies every frame take the sun draw's, neither source skips after 180 frames, a sun draw that vanishes while the light draws keep landing
//     keeps the stage running and the sun exactly 180 frames, a refused sun draw table is rescued by the other copy, a refused table in the only copy is a skipped frame);
//   the sun's gain and the in-scatter: no sun runs on the ambient term alone, the sun term at the light is exactly white * strength * gain * tint;
//   the surface rule: the far plane, a NaN and a speck of depth are not surfaces;
//   the view position through the capture's own inverse projection, and the stored mix: T = 1 returns the stored texel bit for bit, the mix is done in
//     linear light and stored as its root, a pixel whose own light equals the air's does not move;
//   the outcomes, the stand-down streak and the log lines.
#include "check.h"
#include "live_fog_fixtures.h"

#include "ext/ffxiv/engine/AtmoRules.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

using namespace ext::ffxiv::engine::atmo;
namespace godrays = ext::ffxiv::godrays;

namespace
{
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// The numeric integral the closed form must equal: the density at a ray point is rho0 exp(-falloff * up * s), midpoint rule.
double NumericTau(double rho0, double falloff, double up, double start, double distance)
{
    if (!(distance > start))
        return 0.0;
    const int steps = 200000;
    const double h = (distance - start) / steps;
    double sum = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        const double s = start + (i + 0.5) * h;
        sum += rho0 * std::exp(-falloff * up * s) * h;
    }
    return sum;
}

// The capture's own inverse projection (reversed Z, infinite far, near 0.1): view z = -0.1 / depth.
void CaptureInverseProjection(float out[16])
{
    std::memset(out, 0, 16 * sizeof(float));
    out[0] = 0.7307643f;  // x scale
    out[5] = 0.4110417f;  // y scale
    out[11] = -1.0f;      // z row: -1
    out[14] = 10.0f;      // w row: 10 * depth = depth / near
}

template <size_t N> void BytesOf(const uint32_t (&words)[N], unsigned char* out) { std::memcpy(out, words, N * 4); }

// A live capture's decoded fog state and its camera's world height (the inverse view's second row, +0x040: float 16..19 of the camera words).
struct LiveFog
{
    godrays::FogState fog;
    float cameraWorldY = 0.0f;
};

LiveFog LiveOf(const uint32_t (&camera)[88], const uint32_t (&fogParam)[32], const uint32_t (&profile)[16])
{
    unsigned char fogBytes[128], profileBytes[64];
    BytesOf(fogParam, fogBytes);
    BytesOf(profile, profileBytes);
    LiveFog live;
    live.fog = godrays::DecodeFog(fogBytes, profileBytes);
    float row[4];
    std::memcpy(row, camera + 16, sizeof row);
    live.cameraWorldY = godrays::CameraWorldY(row);
    return live;
}
} // namespace

int main()
{
    // ---- the keys -------------------------------------------------------------------------------------------------------------
    {
        const Params defaults;
        CHECK(Near(defaults.density, 1.5f, 0.0f) && Near(defaults.falloff, 0.01f, 0.0f) && Near(defaults.start, 50.0f, 0.0f));
        CHECK(Near(defaults.phaseG, 0.7f, 0.0f) && Near(defaults.sunStrength, 0.3f, 0.0f) && Near(defaults.skyStrength, 1.0f, 0.0f));
        CHECK(Near(defaults.nightDensity, 0.1f, 0.0f) && Near(defaults.neutral, 0.6f, 0.0f));
        // L2b: the stage is ON by default (the installer never edits the ini, so the default is what the user sees without the menu).
        CHECK(kDefaultEnabled);
        // NaN reads as the default, the ends pinch.
        CHECK(Near(ClampDensity(kNaN), kDefaultDensity, 0.0f) && Near(ClampDensity(-5.0f), kMinDensity, 0.0f) && Near(ClampDensity(1.0e9f), kMaxDensity, 0.0f));
        CHECK(Near(ClampFalloff(kNaN), kDefaultFalloff, 0.0f) && Near(ClampFalloff(-1.0f), kMinFalloff, 0.0f) && Near(ClampFalloff(5.0f), kMaxFalloff, 0.0f));
        CHECK(Near(ClampStart(kNaN), kDefaultStart, 0.0f) && Near(ClampStart(-10.0f), kMinStart, 0.0f) && Near(ClampStart(9999.0f), kMaxStart, 0.0f));
        CHECK(Near(ClampPhaseG(kNaN), kDefaultPhaseG, 0.0f) && Near(ClampPhaseG(-0.5f), kMinPhaseG, 0.0f) && Near(ClampPhaseG(1.5f), kMaxPhaseG, 0.0f));
        CHECK(Near(ClampSunStrength(kNaN), kDefaultSunStrength, 0.0f) && Near(ClampSunStrength(-1.0f), 0.0f, 0.0f) && Near(ClampSunStrength(99.0f), kMaxSunStrength, 0.0f));
        CHECK(Near(ClampSkyStrength(kNaN), kDefaultSkyStrength, 0.0f) && Near(ClampSkyStrength(-1.0f), 0.0f, 0.0f) && Near(ClampSkyStrength(99.0f), kMaxSkyStrength, 0.0f));
        CHECK(Near(ClampNightDensity(kNaN), kDefaultNightDensity, 0.0f) && Near(ClampNightDensity(-1.0f), 0.0f, 0.0f) && Near(ClampNightDensity(2.0f), 1.0f, 0.0f));
        CHECK(Near(ClampNeutral(kNaN), kDefaultNeutral, 0.0f) && Near(ClampNeutral(-1.0f), 0.0f, 0.0f) && Near(ClampNeutral(2.0f), 1.0f, 0.0f));
        // Infinities pinch too.
        CHECK(Near(ClampDensity(kInf), kMaxDensity, 0.0f) && Near(ClampDensity(-kInf), kMinDensity, 0.0f));
        const Params built = BuildParams(1000.0f, 5.0f, 9999.0f, 5.0f, 99.0f, 99.0f, 9.0f, 9.0f);
        CHECK(Near(built.density, kMaxDensity, 0.0f) && Near(built.falloff, kMaxFalloff, 0.0f) && Near(built.start, kMaxStart, 0.0f) &&
              Near(built.phaseG, kMaxPhaseG, 0.0f) && Near(built.sunStrength, kMaxSunStrength, 0.0f) && Near(built.skyStrength, kMaxSkyStrength, 0.0f) &&
              Near(built.nightDensity, kMaxNightDensity, 0.0f) && Near(built.neutral, kMaxNeutral, 0.0f));
        // The old seven-key call keeps the neutral's default (a caller that predates the key).
        CHECK(Near(BuildParams(1.0f, 0.01f, 10.0f, 0.5f, 0.5f, 1.0f, 0.5f).neutral, kDefaultNeutral, 0.0f));
        const Params nan = BuildParams(kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN);
        CHECK(Near(nan.density, kDefaultDensity, 0.0f) && Near(nan.start, kDefaultStart, 0.0f) && Near(nan.phaseG, kDefaultPhaseG, 0.0f) && Near(nan.neutral, kDefaultNeutral, 0.0f));
    }
    {
        // Wanted: the switch, a medium, some light to scatter.
        Params params;
        CHECK(Wanted(true, params));
        CHECK(!Wanted(false, params));
        params.density = 0.0f;
        CHECK(!Wanted(true, params));
        params = Params {};
        params.sunStrength = 0.0f;
        CHECK(Wanted(true, params)); // the ambient term alone is a stage
        params.skyStrength = 0.0f;
        CHECK(!Wanted(true, params)); // nothing to scatter: today's picture
    }

    // ---- the optical depth ----------------------------------------------------------------------------------------------------
    {
        const float rho0 = 0.004f;
        // Nearer than the start, at the start, no medium, bad numbers: no medium on the ray (mutant: the start distance ignored).
        CHECK(OpticalDepth(rho0, 0.01f, 0.2f, 40.0f, 30.0f) == 0.0f);
        CHECK(OpticalDepth(rho0, 0.01f, 0.2f, 40.0f, 40.0f) == 0.0f);
        CHECK(OpticalDepth(0.0f, 0.01f, 0.2f, 40.0f, 400.0f) == 0.0f);
        CHECK(OpticalDepth(-1.0f, 0.01f, 0.2f, 40.0f, 400.0f) == 0.0f);
        CHECK(OpticalDepth(rho0, kNaN, 0.2f, 40.0f, 400.0f) == 0.0f);
        CHECK(OpticalDepth(rho0, 0.01f, kNaN, 40.0f, 400.0f) == 0.0f);
        CHECK(OpticalDepth(rho0, 0.01f, 0.2f, 40.0f, kNaN) == 0.0f);
        CHECK(OpticalDepth(rho0, 0.01f, 0.2f, -5.0f, 400.0f) == 0.0f);
        CHECK(OpticalDepth(rho0, 0.01f, 0.2f, 40.0f, kInf) == 0.0f);
        // Homogeneous medium: exactly rho0 * (s1 - s0), whatever the ray's pitch; and a horizontal ray in a stratified one.
        CHECK(Near(OpticalDepth(rho0, 0.0f, 0.7f, 40.0f, 340.0f), rho0 * 300.0f, 1.0e-6f));
        CHECK(Near(OpticalDepth(rho0, 0.0f, -0.7f, 0.0f, 100.0f), rho0 * 100.0f, 1.0e-6f));
        CHECK(Near(OpticalDepth(rho0, 0.01f, 0.0f, 40.0f, 340.0f), rho0 * 300.0f, 1.0e-6f));
        // The start distance matters: a later start has less medium in front of the same surface (mutant: start ignored).
        CHECK(OpticalDepth(rho0, 0.01f, 0.0f, 40.0f, 340.0f) < OpticalDepth(rho0, 0.01f, 0.0f, 0.0f, 340.0f));
        CHECK(Near(OpticalDepth(rho0, 0.01f, 0.0f, 0.0f, 340.0f) - OpticalDepth(rho0, 0.01f, 0.0f, 40.0f, 340.0f), rho0 * 40.0f, 1.0e-6f));
        // The closed form equals the numeric integral, looking up, level and down, near and far, from the start.
        struct Case { float rho0, falloff, up, start, distance; };
        const Case cases[] {
            { 0.004f, 0.01f, 0.3f, 40.0f, 400.0f },  { 0.004f, 0.01f, -0.3f, 40.0f, 400.0f }, { 0.002f, 0.02f, 0.05f, 0.0f, 900.0f },
            { 0.002f, 0.02f, -0.05f, 10.0f, 900.0f }, { 0.01f, 0.05f, 0.5f, 60.0f, 120.0f },    { 0.001f, 0.1f, 0.9f, 0.0f, 50.0f },
            { 0.003f, 0.0005f, 0.2f, 20.0f, 2000.0f }, { 0.006f, 0.03f, -0.1f, 5.0f, 150.0f },
        };
        for (const Case& c : cases)
        {
            const double expected = NumericTau(c.rho0, c.falloff, c.up, c.start, c.distance);
            const float got = OpticalDepth(c.rho0, c.falloff, c.up, c.start, c.distance);
            CHECK(std::fabs(double(got) - expected) <= 2.0e-4 * expected + 1.0e-7);
        }
        // Monotone in the distance, and looking down (more medium) is at least looking up (less) in a stratified medium.
        float previous = 0.0f;
        for (float distance = 50.0f; distance < 3000.0f; distance += 97.0f)
        {
            const float tau = OpticalDepth(rho0, 0.01f, 0.2f, 40.0f, distance);
            CHECK(tau >= previous);
            previous = tau;
        }
        CHECK(OpticalDepth(rho0, 0.01f, -0.4f, 40.0f, 300.0f) > OpticalDepth(rho0, 0.01f, 0.0f, 40.0f, 300.0f));
        CHECK(OpticalDepth(rho0, 0.01f, 0.4f, 40.0f, 300.0f) < OpticalDepth(rho0, 0.01f, 0.0f, 40.0f, 300.0f));
        // Continuity across the series limit (a * len = 1e-3): the two forms agree to a few parts in a million.
        {
            const float up = 1.0e-3f / (0.01f * 300.0f); // a * len = falloff * up * 300 = 1e-3
            const float just = OpticalDepth(rho0, 0.01f, up * 0.999f, 0.0f, 300.0f);
            const float over = OpticalDepth(rho0, 0.01f, up * 1.001f, 0.0f, 300.0f);
            CHECK(Near(just, over, 2.0e-5f * rho0 * 300.0f + 1.0e-7f));
        }
        // The cap: a very dense medium never answers more than the cap, and the optical depth of a steep downward ray from a high camera is finite.
        CHECK(OpticalDepth(100.0f, 0.0f, 0.0f, 0.0f, 1.0e6f) == kMaxOpticalDepth);
        const float steep = OpticalDepth(0.004f, 0.1f, -1.0f, 0.0f, 5000.0f);
        CHECK(steep == steep && steep > 0.0f && steep <= kMaxOpticalDepth);
    }

    // ---- the transmittance ----------------------------------------------------------------------------------------------------
    {
        CHECK(Transmittance(0.0f) == 1.0f); // exactly 1: the caller's early-out
        CHECK(Transmittance(-1.0f) == 1.0f);
        CHECK(Transmittance(kNaN) == 1.0f);
        CHECK(Near(Transmittance(std::log(2.0f)), 0.5f, 1.0e-6f));
        CHECK(Near(Transmittance(1.0f), std::exp(-1.0f), 1.0e-6f));
        CHECK(Transmittance(0.5f) > Transmittance(1.0f));
        CHECK(Transmittance(1.0e9f) == std::exp(-kMaxOpticalDepth));
        CHECK(Transmittance(1.0e9f) > 0.0f);
    }

    // ---- the sun's lobe -------------------------------------------------------------------------------------------------------
    {
        for (float g : { 0.0f, 0.3f, 0.7f, 0.95f })
            CHECK(Near(SunPhase(1.0f, g), 1.0f, 1.0e-6f)); // normalised to 1 at the light
        for (float c : { -1.0f, -0.3f, 0.0f, 0.6f, 1.0f })
            CHECK(Near(SunPhase(c, 0.0f), 1.0f, 1.0e-6f)); // g = 0 is isotropic
        // Forward greater than backward (mutant: the sign of g flipped), strictly monotone in the cosine, never above 1.
        for (float g : { 0.3f, 0.7f, 0.95f })
        {
            CHECK(SunPhase(1.0f, g) > SunPhase(-1.0f, g));
            CHECK(SunPhase(0.5f, g) > SunPhase(-0.5f, g));
            float previous = 0.0f;
            for (float c = -1.0f; c <= 1.0f; c += 0.05f)
            {
                const float value = SunPhase(c, g);
                CHECK(value > previous && value <= 1.0f + 1.0e-6f);
                previous = value;
            }
        }
        // The exact value at g = 0.7, c = 0: 0.3^3 / (0.3^2 + 2 * 0.7)^1.5.
        CHECK(Near(SunPhase(0.0f, 0.7f), 0.027f / std::pow(1.49f, 1.5f), 1.0e-6f));
        // A wider g narrows the lobe: at 60 degrees the value falls as g rises.
        CHECK(SunPhase(0.5f, 0.9f) < SunPhase(0.5f, 0.5f));
        // Broken numbers: NaN cosine is the quarter turn, g is clamped into the key's own range.
        CHECK(Near(SunPhase(kNaN, 0.7f), SunPhase(0.0f, 0.7f), 0.0f));
        CHECK(Near(SunPhase(0.5f, 5.0f), SunPhase(0.5f, kMaxPhaseG), 0.0f));
        CHECK(Near(SunPhase(0.5f, -5.0f), SunPhase(0.5f, 0.0f), 0.0f)); // forward scattering only
    }

    // ---- the engine's own fog, the night, the effective density --------------------------------------------------------------------
    {
        // Clear weather leaves our density alone (the captured frames' engine density at the camera: 0.0009, 0.0024, 0.0010).
        CHECK(EngineFogScale(0.0009f) == 1.0f && EngineFogScale(0.0024f) == 1.0f && EngineFogScale(kEngineFogClear) == 1.0f);
        CHECK(EngineFogScale(kNaN) == 1.0f && EngineFogScale(-1.0f) == 1.0f);
        // Above it the scale falls: half at clear + span, a quarter at clear + 3 span, monotone (mutant: the scale dropped).
        CHECK(Near(EngineFogScale(kEngineFogClear + kEngineFogSpan), 0.5f, 1.0e-6f));
        CHECK(Near(EngineFogScale(kEngineFogClear + 3.0f * kEngineFogSpan), 0.25f, 1.0e-6f));
        CHECK(EngineFogScale(0.0094f) < 1.0f && EngineFogScale(0.03f) < EngineFogScale(0.0094f) && EngineFogScale(0.03f) > 0.0f);
        // The night scale: no light read is a day; the sun's luminance keeps the whole key; the moon keeps the night share.
        CHECK(NightDensityScale(0.35f, 0.0f, false) == 1.0f);
        CHECK(Near(NightDensityScale(0.35f, 1.029f, true), 1.0f, 1.0e-6f));
        CHECK(Near(NightDensityScale(0.35f, 0.04f, true), 0.35f, 1.0e-6f));
        CHECK(NightDensityScale(0.35f, 0.3f, true) > 0.35f && NightDensityScale(0.35f, 0.3f, true) < 1.0f);
        CHECK(Near(NightDensityScale(0.35f, kNaN, true), 0.35f, 1.0e-6f)); // a NaN light is no light's day
        CHECK(Near(NightDensityScale(0.0f, 0.04f, true), 0.0f, 1.0e-6f) && Near(NightDensityScale(1.0f, 0.04f, true), 1.0f, 1.0e-6f));
        // The effective density: the key per metre times both scales.
        const Params params;
        const float perMetre = params.density / 1000.0f; // the key per metre (L2b: 1.5 per km)
        CHECK(Near(EffectiveDensityPerMetre(params, 0.0009f, 1.029f, true), perMetre, 1.0e-7f));
        CHECK(Near(EffectiveDensityPerMetre(params, 0.0009f, 0.04f, true), perMetre * params.nightDensity, 1.0e-7f));
        CHECK(Near(EffectiveDensityPerMetre(params, kEngineFogClear + kEngineFogSpan, 1.029f, true), perMetre * 0.5f, 1.0e-7f));
        CHECK(EffectiveDensityPerMetre(params, 0.0094f, 1.029f, true) < perMetre);
    }

    // ---- FOGFIX: the engine with no height fog, the colour, the zone change ----------------------------------------------------------
    {
        const Params params;
        const float perMetre = params.density / 1000.0f;
        const float tint[3] { 1.0f, 0.95f, 0.9f };
        const float white = WhiteLinearOf(0.838f);
        struct Zero
        {
            const char* name;
            LiveFog live;
            float luminance; // the light draw's diffuse luminance of that capture's frame (day sun 1.0+, dawn about 0.5, night 0 = the moon / none)
            bool night;
        };
        const Zero zeros[] {
            { "day 164414-3", LiveOf(live_fog::kLiveDayCamera, live_fog::kLiveDayFogParam, live_fog::kLiveDayExpHeightFog), 1.0f, false },
            { "dawn 164337-2", LiveOf(live_fog::kLiveDawnCamera, live_fog::kLiveDawnFogParam, live_fog::kLiveDawnExpHeightFog), 0.5f, false },
            { "night 164135", LiveOf(live_fog::kLiveNightCamera, live_fog::kLiveNightFogParam, live_fog::kLiveNightExpHeightFog), 0.04f, true },
        };
        for (const Zero& zero : zeros)
        {
            CHECK(godrays::FogStateUsable(zero.live.fog));
            CHECK(godrays::EngineHasNoHeightFog(zero.live.fog));
            // The engine's density at the camera is exactly 0: the engine fog scale is 1, so the whole key is ours (and the night share at night).
            const float engineDensity = godrays::FogDensityAt(zero.live.fog, zero.live.cameraWorldY, 1.0f);
            CHECK(engineDensity == 0.0f);
            CHECK(EngineFogScale(engineDensity) == 1.0f);
            CHECK(Near(EffectiveDensityPerMetre(params, engineDensity, zero.luminance, true), perMetre * NightDensityScale(params.nightDensity, zero.luminance, true), 1.0e-9f));
            if (!zero.night)
                CHECK(Near(EffectiveDensityPerMetre(params, engineDensity, zero.luminance, true), perMetre, perMetre * 0.5f)); // a day keeps most of the key
            else
                CHECK(Near(EffectiveDensityPerMetre(params, engineDensity, zero.luminance, true), perMetre * params.nightDensity, 1.0e-9f)); // the moon keeps the night share
            CHECK(EffectiveDensityPerMetre(params, engineDensity, zero.luminance, true) > 0.0f);
            // The colour the ambient term is made of: the capture's own fog colour (non-black), neutralised at the default share; the air far away (T -> 0) is that.
            float ambient[3];
            NeutralAmbient(zero.live.fog.fogColor, params.neutral, ambient);
            for (uint32_t i = 0; i < 3; ++i)
                CHECK(ambient[i] > 0.01f);
            const Inscatter air = InscatterOf(params, zero.live.fog.fogColor, tint, white, 0.0f, 1.0f);
            for (uint32_t i = 0; i < 3; ++i)
                CHECK(Near(air.rgb[i], params.skyStrength * ambient[i], 1.0e-7f) && air.rgb[i] > 0.01f);
            // The engine's colour is the ambient's source, not a constant: the same arithmetic with a black colour is black air.
            const float black[3] { 0.0f, 0.0f, 0.0f };
            const Inscatter blackAir = InscatterOf(params, black, tint, white, 0.0f, 1.0f);
            CHECK(blackAir.rgb[0] == 0.0f && blackAir.rgb[1] == 0.0f && blackAir.rgb[2] == 0.0f);
        }

        // A fogged zone above the clear rate (r2: the lab measured 0.0041 per metre at the camera).
        const LiveFog fogged = LiveOf(live_fog::kFoggedR2Camera, live_fog::kFoggedR2FogParam, live_fog::kFoggedR2ExpHeightFog);
        CHECK(godrays::FogStateUsable(fogged.fog) && !godrays::EngineHasNoHeightFog(fogged.fog));
        const float foggedDensity = godrays::FogDensityAt(fogged.fog, fogged.cameraWorldY, 1.0f);
        CHECK(Near(foggedDensity, 0.0041f, 2.0e-4f));
        CHECK(EngineFogScale(foggedDensity) < 0.8f && EngineFogScale(foggedDensity) > 0.65f);

        // The zone change inside one scene: the engine's density at the camera runs from the fogged zone's down to zero over 60 frames (a weather or zone fade), then stays zero. The
        // effective density never drops to nothing, never falls (clearing the engine's fog gives the key back), steps by at most exp(the engine's per-frame change / span) (d ln(scale) / d density is at most 1 / span), and ends at the key's whole density.
        float previous = EffectiveDensityPerMetre(params, foggedDensity, 1.029f, true);
        CHECK(previous > 0.0f);
        float worstStep = 1.0f;
        for (uint32_t frame = 1; frame <= 90; ++frame)
        {
            const float engine = frame >= 60 ? 0.0f : foggedDensity * (1.0f - float(frame) / 60.0f);
            const float density = EffectiveDensityPerMetre(params, engine, 1.029f, true);
            CHECK(density > 0.0f);
            CHECK(density >= previous - 1.0e-12f);
            const float step = density / previous;
            if (step > worstStep)
                worstStep = step;
            previous = density;
        }
        CHECK(worstStep <= std::exp(foggedDensity / 60.0f / kEngineFogSpan) + 1.0e-5f);
        CHECK(worstStep > 1.0f);
        CHECK(Near(previous, perMetre, 1.0e-9f));
        // A one-frame step (the engine's fog gone in the frame it changes zone): the haze moves by exactly the engine fog scale it used to carry, never to nothing; the densest fog the
        // profiles on disc can make at the camera (both layers at their reference height: 0.00693 + 0.003465) is the worst case, stated here as a number.
        const float densest = 0.00693f + 0.003465f;
        const float stepRatio = EffectiveDensityPerMetre(params, 0.0f, 1.029f, true) / EffectiveDensityPerMetre(params, densest, 1.029f, true);
        CHECK(Near(stepRatio, 1.0f / EngineFogScale(densest), 1.0e-4f));
        CHECK(stepRatio > 1.0f && stepRatio < 4.0f);
        CHECK(EffectiveDensityPerMetre(params, 0.0f, 1.029f, true) > 0.0f);
        // The session's own numbers (0.0026 per metre one moment, 0 the next) sit under the clear rate on both sides: the haze does not move at all.
        CHECK(EffectiveDensityPerMetre(params, 0.0026f, 1.029f, true) == EffectiveDensityPerMetre(params, 0.0f, 1.029f, true));

        // The line a change of the engine's fog writes.
        const float colour[3] { 0.267f, 0.385f, 0.414f };
        const std::string none = FormatEngineFog(true, 0.0f, 1.0f, colour);
        const std::string some = FormatEngineFog(false, 0.0041f, 0.72f, colour);
        CHECK(Contains(none, "ZERO") && Contains(none, "0.0000/m") && Contains(none, "engine fog scale 1.00") && Contains(none, "0.267, 0.385, 0.414"));
        CHECK(Contains(some, "fogged") && !Contains(some, "ZERO") && Contains(some, "0.0041/m") && Contains(some, "0.72"));
        CHECK(none.find("Atmosphere: ") == 0 && none != some);
    }

    // ---- the sun and the in-scatter -------------------------------------------------------------------------------------------------
    {
        CHECK(SunGain(false, true, 1.0f, 0.5f) == 0.0f); // no sun, whatever else
        CHECK(Near(SunGain(true, true, 0.6f, 0.0f), 1.0f, 1.0e-6f));
        CHECK(Near(SunGain(true, true, 0.3f, 0.0f), 0.5f, 1.0e-6f));
        CHECK(SunGain(true, true, 5.0f, 0.0f) == 1.0f);
        CHECK(SunGain(true, true, -1.0f, 0.9f) == 0.0f);
        CHECK(SunGain(true, true, kNaN, 0.9f) == 1.0f); // a NaN luminance is none reported: the fog sun's elevation decides
        // No luminance reported: the fog sun's own elevation decides.
        CHECK(SunGain(true, false, 0.0f, 0.5f) == 1.0f);
        CHECK(SunGain(true, false, 0.0f, -0.5f) == 0.0f);
        CHECK(SunGain(true, false, 0.0f, 0.1f) > 0.0f && SunGain(true, false, 0.0f, 0.1f) < 1.0f);

        const float fog[3] { 0.3551f, 0.5344f, 0.5908f };
        const float tint[3] { 1.0f, 0.9f, 0.8f };
        const float white = WhiteLinearOf(0.838f);
        CHECK(Near(white, 0.702244f, 1.0e-5f));
        Params params;
        params.neutral = 0.0f; // the engine's own colour: the L2 arithmetic below is stated at neutral 0 (the neutral blend has its own block)
        // No sun runs on the ambient term alone (mutant: the stage declines without a sun is a pins matter; the numbers are here).
        const Inscatter ambientOnly = InscatterOf(params, fog, tint, white, 0.0f, 1.0f);
        for (uint32_t i = 0; i < 3; ++i)
            CHECK(Near(ambientOnly.rgb[i], params.skyStrength * fog[i], 1.0e-7f));
        // The sun term at the light is exactly white * strength * gain * tint, on top of the ambient.
        const Inscatter atSun = InscatterOf(params, fog, tint, white, 0.8f, 1.0f);
        for (uint32_t i = 0; i < 3; ++i)
            CHECK(Near(atSun.rgb[i], fog[i] + params.sunStrength * white * 0.8f * tint[i], 1.0e-6f));
        // The lobe falls away from the sun, the ambient does not.
        const Inscatter away = InscatterOf(params, fog, tint, white, 0.8f, -0.5f);
        for (uint32_t i = 0; i < 3; ++i)
            CHECK(away.rgb[i] < atSun.rgb[i] && away.rgb[i] >= fog[i] - 1.0e-7f);
        // The sky key scales the ambient, the sun key the lobe; both at zero is black air.
        params.skyStrength = 0.0f;
        params.sunStrength = 0.0f;
        const Inscatter nothing = InscatterOf(params, fog, tint, white, 1.0f, 1.0f);
        CHECK(nothing.rgb[0] == 0.0f && nothing.rgb[1] == 0.0f && nothing.rgb[2] == 0.0f);
        // A negative fog colour channel is no light.
        const float badFog[3] { -1.0f, 0.2f, 0.2f };
        params = Params {};
        params.neutral = 0.0f; // (at the default neutral the blend lends the dark channel its share of the luminance of the others: that is the neutral block's)
        CHECK(InscatterOf(params, badFog, tint, white, 0.0f, 0.0f).rgb[0] == 0.0f);
    }

    // ---- the neutral ambient (L2b) ---------------------------------------------------------------------------------------------------
    {
        const float fog[3] { 0.3551f, 0.5344f, 0.5908f }; // r1's engine fog colour: cyan
        const float luminance = ext::ffxiv::shafts::Luminance(fog); // the tree's own Rec. 709 weights
        float out[3];
        NeutralAmbient(fog, 0.0f, out); // 0 is the engine's own colour, untouched
        for (uint32_t i = 0; i < 3; ++i)
            CHECK(Near(out[i], fog[i], 1.0e-7f));
        NeutralAmbient(fog, 1.0f, out); // 1 is a pure neutral of the same luminance (Rec. 709 weights, linear light)
        for (uint32_t i = 0; i < 3; ++i)
            CHECK(Near(out[i], luminance, 1.0e-6f));
        NeutralAmbient(fog, 0.6f, out); // a blend: the luminance is kept at every share and the chroma shrinks with it
        CHECK(Near(ext::ffxiv::shafts::Luminance(out), luminance, 1.0e-6f));
        for (uint32_t i = 0; i < 3; ++i)
            CHECK(Near(out[i], fog[i] * 0.4f + luminance * 0.6f, 1.0e-6f));
        CHECK(out[2] > out[0] && (out[2] - out[0]) < (fog[2] - fog[0]) * 0.41f + 1.0e-6f); // still the same hue, 40 % of the chroma
        // A grey fog colour does not move at any share; a negative channel is no light; a NaN share reads as the default.
        const float grey[3] { 0.4f, 0.4f, 0.4f };
        for (float share : { 0.0f, 0.3f, 1.0f })
        {
            NeutralAmbient(grey, share, out);
            CHECK(Near(out[0], 0.4f, 1.0e-6f) && Near(out[1], 0.4f, 1.0e-6f) && Near(out[2], 0.4f, 1.0e-6f));
        }
        const float negative[3] { -1.0f, 0.2f, 0.2f };
        NeutralAmbient(negative, 0.0f, out);
        CHECK(out[0] == 0.0f && Near(out[1], 0.2f, 1.0e-7f));
        float viaNaN[3], viaDefault[3];
        NeutralAmbient(fog, kNaN, viaNaN);
        NeutralAmbient(fog, kDefaultNeutral, viaDefault);
        CHECK(viaNaN[0] == viaDefault[0] && viaNaN[1] == viaDefault[1] && viaNaN[2] == viaDefault[2]);
        // The in-scatter takes it: the default keys' ambient-only air is the neutral blend times the sky key, not the engine's colour.
        const float tint[3] { 1.0f, 0.9f, 0.8f };
        Params params; // the defaults (neutral 0.6, sky 1.0)
        const Inscatter ambientOnly = InscatterOf(params, fog, tint, WhiteLinearOf(0.838f), 0.0f, 1.0f);
        NeutralAmbient(fog, params.neutral, out);
        for (uint32_t i = 0; i < 3; ++i)
            CHECK(Near(ambientOnly.rgb[i], params.skyStrength * out[i], 1.0e-7f));
        params.neutral = 0.0f;
        CHECK(Near(InscatterOf(params, fog, tint, WhiteLinearOf(0.838f), 0.0f, 1.0f).rgb[2], fog[2], 1.0e-7f));
    }

    // ---- the lobe's direction: the sky's visible sun disc (L2b) -----------------------------------------------------------------------------
    {
        float invP[16];
        CaptureInverseProjection(invP);
        const float up[3] { 0.0f, 1.0f, 0.0f }; // a level camera: the world's up is the view's +y
        float dir[3];
        // The window's centre is the view's forward (-z); a UV at the top of the window looks up, at the right looks right (the UV's y runs down).
        CHECK(SunDirectionFromUv(invP, 0.5f, 0.5f, dir) && Near(dir[0], 0.0f, 1.0e-6f) && Near(dir[1], 0.0f, 1.0e-6f) && Near(dir[2], -1.0f, 1.0e-6f));
        CHECK(SunDirectionFromUv(invP, 0.5f, 0.25f, dir) && dir[1] > 0.0f && Near(dir[0], 0.0f, 1.0e-6f));
        CHECK(SunDirectionFromUv(invP, 0.75f, 0.5f, dir) && dir[0] > 0.0f && Near(dir[1], 0.0f, 1.0e-6f));
        CHECK(SunDirectionFromUv(invP, 0.0f, 0.0f, dir) && dir[0] < 0.0f && dir[1] > 0.0f);
        // The direction is a unit vector that depends on nothing but the UV and the projection: r1's disc at (0.533, 0.2004) through r1's own projection is the
        // direction the lab measured, (0.0467, 0.2387, -0.97).
        float r1Inv[16] {};
        r1Inv[0] = 0.7307643f;
        r1Inv[5] = 0.41105491f;
        r1Inv[11] = -1.0f;
        r1Inv[14] = 10.0f;
        CHECK(SunDirectionFromUv(r1Inv, 0.5330276f, 0.2004416f, dir));
        CHECK(Near(std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]), 1.0f, 1.0e-5f));
        CHECK(Near(dir[0], 0.0467f, 5.0e-4f) && Near(dir[1], 0.2387f, 5.0e-4f) && Near(dir[2], -0.97f, 5.0e-4f));
        // Unusable numbers give no direction.
        CHECK(!SunDirectionFromUv(invP, kNaN, 0.5f, dir) && !SunDirectionFromUv(invP, 0.5f, kInf, dir));
        float degenerate[16] {};
        CHECK(!SunDirectionFromUv(degenerate, 0.5f, 0.5f, dir));

        // The choice: r1's own pair. The light draw's direction (0.047, 0.509, -0.860) sits 16.8 degrees above the disc (0.0467, 0.2387, -0.97): the engine clamps the
        // light's elevation. A fresh, agreeing disc is what the lobe is centred on.
        const float light[3] { 0.047f, 0.50869f, -0.859665f };
        float disc[3];
        CHECK(SunDirectionFromUv(r1Inv, 0.5330276f, 0.2004416f, disc));
        SunChoice choice = ChooseSunDirection(light, true, disc, true, up);
        CHECK(choice.disc && Near(choice.dir[0], disc[0], 1.0e-6f) && Near(choice.dir[1], disc[1], 1.0e-6f) && Near(choice.dir[2], disc[2], 1.0e-6f));
        // Not fresh / no UV: the light's direction (the L2 behaviour), whatever the disc says (mutant: the lobe direction ignored is the converse).
        choice = ChooseSunDirection(light, true, disc, false, up);
        CHECK(!choice.disc && Near(choice.dir[0], light[0], 1.0e-6f) && Near(choice.dir[1], light[1], 1.0e-6f) && Near(choice.dir[2], light[2], 1.0e-6f));
        // No light at all: nothing to agree with, so no disc either (the stage runs on its ambient term).
        choice = ChooseSunDirection(light, false, disc, true, up);
        CHECK(!choice.disc);
        // The sun behind the camera mirrors its UV to the front: the disc then points the other way in azimuth (here 180 degrees) and is refused.
        const float behind[3] { -0.047f, 0.50869f, 0.859665f };
        choice = ChooseSunDirection(behind, true, disc, true, up);
        CHECK(!choice.disc && Near(choice.dir[2], behind[2], 1.0e-6f));
        // A disc 40 degrees to the side of the light in azimuth is refused; 10 degrees is accepted (the clamp moves elevation, not azimuth).
        auto rotated = [&](float degrees, float elevationSine, float out[3]) {
            const float radians = degrees * 3.14159265f / 180.0f;
            const float horizontal = std::sqrt(1.0f - elevationSine * elevationSine);
            out[0] = horizontal * std::sin(radians);
            out[1] = elevationSine;
            out[2] = -horizontal * std::cos(radians);
        };
        float sideLight[3], sideDisc[3];
        rotated(0.0f, 0.5f, sideLight);
        rotated(40.0f, 0.2f, sideDisc);
        CHECK(!ChooseSunDirection(sideLight, true, sideDisc, true, up).disc);
        rotated(10.0f, 0.2f, sideDisc);
        CHECK(ChooseSunDirection(sideLight, true, sideDisc, true, up).disc);
        // A disc ABOVE the light is not the clamp (the clamp only lifts the light): refused. Level with it is accepted (a high sun's two agree).
        rotated(0.0f, 0.2f, sideLight);
        rotated(0.0f, 0.6f, sideDisc);
        CHECK(!ChooseSunDirection(sideLight, true, sideDisc, true, up).disc);
        rotated(0.0f, 0.6f, sideLight);
        rotated(2.0f, 0.6f, sideDisc);
        CHECK(ChooseSunDirection(sideLight, true, sideDisc, true, up).disc);
        // Near the zenith the azimuth means nothing: the total angle decides (35 degrees).
        const float zenithLight[3] { 0.0f, 1.0f, 0.0f };
        float zenithDisc[3];
        rotated(70.0f, 0.9f, zenithDisc); // 25.8 degrees from the zenith: accepted
        CHECK(ChooseSunDirection(zenithLight, true, zenithDisc, true, up).disc);
        rotated(70.0f, 0.5f, zenithDisc); // 60 degrees from the zenith: refused
        CHECK(!ChooseSunDirection(zenithLight, true, zenithDisc, true, up).disc);
        // A tilted camera: the world's up is not the view's y, and the gate follows it (a level gate would call this azimuth-off).
        const float tilt = 30.0f * 3.14159265f / 180.0f;
        const float tiltedUp[3] { std::sin(tilt), std::cos(tilt), 0.0f };
        const float tiltedLight[3] { std::sin(tilt) * 0.5f, std::cos(tilt) * 0.5f, -0.8660254f }; // sine 0.5 above the horizon, straight ahead
        const float tiltedDisc[3] { std::sin(tilt) * 0.2f, std::cos(tilt) * 0.2f, -0.9797959f };  // sine 0.2, the same azimuth
        CHECK(ChooseSunDirection(tiltedLight, true, tiltedDisc, true, tiltedUp).disc);
        // A broken disc or light is not chosen.
        const float notNumber[3] { kNaN, 0.0f, -1.0f };
        CHECK(!ChooseSunDirection(light, true, notNumber, true, up).disc);
        const float zero[3] { 0.0f, 0.0f, 0.0f };
        CHECK(!ChooseSunDirection(zero, true, disc, true, up).disc);
        CHECK(Contains(FormatSunDisc(0.5330276f, 0.2004416f, 16.8f), "visible sun"));
    }

    // ---- the surface rule -----------------------------------------------------------------------------------------------------------
    {
        CHECK(!IsAtmosphereSurface(0.0f));      // the far plane (reversed Z): the sky gets nothing (mutant: the exclusion dropped)
        CHECK(!IsAtmosphereSurface(kNaN));
        CHECK(!IsAtmosphereSurface(-0.5f));
        CHECK(!IsAtmosphereSurface(1.0e-7f));
        CHECK(IsAtmosphereSurface(1.0e-3f) && IsAtmosphereSurface(0.5f) && IsAtmosphereSurface(1.0f));
    }

    // ---- the view position, through the capture's own inverse projection ---------------------------------------------------------
    {
        float invP[16];
        CaptureInverseProjection(invP);
        float p[3];
        // Reversed Z, near 0.1: depth 0.01 is 10 m straight ahead at the centre.
        CHECK(ViewPositionOf(invP, 0.0f, 0.0f, 0.01f, p));
        CHECK(Near(p[0], 0.0f, 1.0e-5f) && Near(p[1], 0.0f, 1.0e-5f) && Near(p[2], -10.0f, 1.0e-4f));
        // An edge pixel: x = 0.7307643 / 0.1 = 7.3076 m at 10 m ahead, so 12.38 m away.
        CHECK(ViewPositionOf(invP, 1.0f, 0.0f, 0.01f, p));
        CHECK(Near(p[0], 7.307643f, 1.0e-3f) && Near(p[2], -10.0f, 1.0e-4f));
        CHECK(Near(std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]), std::sqrt(7.307643f * 7.307643f + 100.0f), 1.0e-3f));
        // Farther is smaller depth: 0.001 is 100 m.
        CHECK(ViewPositionOf(invP, 0.0f, 0.0f, 0.001f, p) && Near(p[2], -100.0f, 1.0e-2f));
        // The sky (depth 0) has no position (w = 0), and a NaN depth none either.
        CHECK(!ViewPositionOf(invP, 0.0f, 0.0f, 0.0f, p));
        CHECK(!ViewPositionOf(invP, 0.0f, 0.0f, kNaN, p));
    }

    // ---- the stored mix -------------------------------------------------------------------------------------------------------------
    {
        // T = 1: the stored texel itself, bit for bit, for values the square would not give back (mutant: key off / near field not a passthrough).
        for (float stored : { 0.0f, 0.1234567f, 0.5f, 0.838f, 1.7f, 3.3333333f })
        {
            const float out = ComposeStored(stored, 1.0f, 0.7f);
            CHECK(std::memcmp(&out, &stored, sizeof stored) == 0);
        }
        CHECK(Near(ComposeStored(0.5f, 0.0f, 0.36f), 0.6f, 1.0e-6f));                                    // all air: the root of its light
        CHECK(Near(ComposeStored(0.5f, 0.5f, 0.36f), std::sqrt(0.5f * 0.25f + 0.5f * 0.36f), 1.0e-6f)); // linear mix, stored as its root
        // A pixel whose own light equals the air's does not move, at any T.
        for (float transmittance : { 0.0f, 0.3f, 0.9f })
            CHECK(Near(ComposeStored(0.6f, transmittance, 0.36f), 0.6f, 1.0e-6f));
        // Applying the transmittance twice is a different (wrong) picture: the pass mixes once.
        const float once = ComposeStored(0.9f, 0.5f, 0.1f);
        const float twice = ComposeStored(once, 0.5f, 0.1f);
        CHECK(!Near(once, twice, 1.0e-3f));
        CHECK(once < 0.9f && once > std::sqrt(0.1f));
        // The domain: mixing the stored values directly (the wrong domain) differs from mixing their light.
        const float storedDomain = 0.5f * 0.9f + 0.5f * std::sqrt(0.1f);
        CHECK(!Near(once, storedDomain, 1.0e-3f));
        // Broken numbers: a negative air or scene is no light.
        CHECK(ComposeStored(0.5f, 0.5f, -1.0f) >= 0.0f && ComposeStored(-0.5f, 0.5f, 0.2f) >= 0.0f);
        CHECK(ComposeStored(0.5f, kNaN, 0.2f) == 0.5f); // not a transmittance below 1: left as it was
    }

    // ---- outcomes, the stand-down and the log lines ---------------------------------------------------------------------------------
    {
        CHECK(CountsTowardsStandDown(Decline::RenderTarget) && CountsTowardsStandDown(Decline::PassFailed) && CountsTowardsStandDown(Decline::Window));
        CHECK(!CountsTowardsStandDown(Decline::NoFog) && !CountsTowardsStandDown(Decline::NoCamera) && !CountsTowardsStandDown(Decline::NoDepth));
        CHECK(!CountsTowardsStandDown(Decline::NotToneMap) && !CountsTowardsStandDown(Decline::SceneHdr) && !CountsTowardsStandDown(Decline::FeatureOff));
        CHECK(SkipOf(Decline::NoFog) == SkipReason::NoFog && SkipOf(Decline::NoDepth) == SkipReason::NoDepth && SkipOf(Decline::NoCamera) == SkipReason::NoCamera);
        CHECK(SkipOf(Decline::PassFailed) == SkipReason::None);
        // The streak: 64 refusals in a row switch the stage off, a run starts it over.
        Streak streak;
        for (uint32_t i = 0; i + 1 < kConsecutiveDeclineLimit; ++i)
            CHECK(!streak.NoteDecline());
        streak.NoteRun();
        for (uint32_t i = 0; i + 1 < kConsecutiveDeclineLimit; ++i)
            CHECK(!streak.NoteDecline());
        CHECK(streak.NoteDecline() && streak.off && !streak.Armed());
        CHECK(!streak.NoteDecline());
        // The skips.
        SkipCounts counts;
        CountSkip(counts, SkipReason::NoFog);
        CountSkip(counts, SkipReason::NoDepth);
        CountSkip(counts, SkipReason::None);
        CHECK(counts.skipped == 2 && counts.noFog == 1 && counts.noDepth == 1 && counts.noCamera == 0);
        // The two hold wordings.
        CHECK(Contains(FormatDecline(Decline::NoFog, Outcome::HoldStands), "the chain's hold stands"));
        CHECK(Contains(FormatDecline(Decline::NoFog, Outcome::EngineOwn), "the frame is the engine's own"));
        CHECK(Contains(FormatSkip(SkipReason::NoCamera), "no atmosphere in it"));
        // The run line, the card, the cost line, the stage values.
        RunFacts run;
        run.windowWidth = 1920;
        run.windowHeight = 1080;
        run.haveSun = true;
        run.haveLight = true;
        const std::string runLine = FormatRun(run);
        CHECK(Contains(runLine, "1920x1080") && Contains(runLine, "sun and light colour") && Contains(runLine, "start 50 m") && Contains(runLine, "neutral 0.60"));
        CHECK(Contains(FormatRun(run), "lobe on the light"));
        run.lobeOnDisc = true;
        CHECK(Contains(FormatRun(run), "lobe on the visible disc"));
        run.lobeOnDisc = false;
        run.haveLight = false;
        CHECK(Contains(FormatRun(run), "fog sun"));
        run.haveSun = false;
        CHECK(Contains(FormatRun(run), "no sun"));
        CardCounts card;
        card.armed = false;
        CHECK(Contains(FormatCard(card), "off for this session"));
        card.armed = true;
        CHECK(!Contains(FormatCard(card), "off for this session") && Contains(FormatCard(card), "density 1.50/km") && Contains(FormatCard(card), "neutral 0.60"));
        CHECK(Contains(FormatCost(true, 0.1234, 100, 90, 10), "0.1234 ms"));
        CHECK(Contains(FormatCost(false, 0.0, 100, 90, 10), "not measured yet"));
        CHECK(FormatStageValues(Params {}) == std::string("density=1.50 falloff=0.0100 start=50.0 g=0.70 sun=0.30 sky=1.00 night=0.10 neutral=0.60"));
        CHECK(Contains(kSceneHdrInactive, "scene HDR"));
    }

    // ---- CAMFIX: the camera the frame stands on --------------------------------------------------------------------------------
    {
        const CameraCopy none {};
        const CameraCopy landedRefused { true, false };
        const CameraCopy ok { true, true };
        const HoldStamps empty {};

        // The truth table of one frame's fold, from an empty hold.
        CHECK(FoldCameraSources(empty, none, none, 50).source == CameraSource::None);
        CHECK(FoldCameraSources(empty, ok, none, 50).source == CameraSource::SunDraw);
        CHECK(FoldCameraSources(empty, none, ok, 50).source == CameraSource::LightDraw); // the benchmark's frames: no sun-shaped light draw, a camera elsewhere
        CHECK(FoldCameraSources(empty, ok, ok, 50).source == CameraSource::SunDraw);     // both: the sun draw's copy stays the first choice
        CHECK(FoldCameraSources(empty, landedRefused, ok, 50).source == CameraSource::LightDraw); // the sun draw's table was refused: the other copy stands in
        CHECK(FoldCameraSources(empty, ok, landedRefused, 50).source == CameraSource::SunDraw);
        CHECK(FoldCameraSources(empty, none, landedRefused, 50).source == CameraSource::None);          // a table that is not this build's camera is no camera
        CHECK(FoldCameraSources(empty, landedRefused, landedRefused, 50).source == CameraSource::None);
        CHECK(FoldCameraSources(empty, CameraCopy { false, true }, CameraCopy { false, true }, 50).source == CameraSource::None); // "verified" without "landed" is nothing

        // What each source refreshes: both the camera's stamp; only the sun draw's copy the sun's.
        {
            const CameraFold sun = FoldCameraSources(empty, ok, ok, 50);
            CHECK(sun.stamps.haveCamera && sun.stamps.cameraFrame == 50 && sun.stamps.haveSunDraw && sun.stamps.sunDrawFrame == 50);
            const CameraFold light = FoldCameraSources(empty, none, ok, 50);
            CHECK(light.stamps.haveCamera && light.stamps.cameraFrame == 50 && !light.stamps.haveSunDraw && light.stamps.sunDrawFrame == 0);
            // A light draws' copy leaves an earlier sun stamp where it was.
            const CameraFold later = FoldCameraSources(sun.stamps, none, ok, 90);
            CHECK(later.source == CameraSource::LightDraw && later.stamps.cameraFrame == 90 && later.stamps.haveSunDraw && later.stamps.sunDrawFrame == 50);
            // Nothing landed: the stamps are untouched.
            const CameraFold quiet = FoldCameraSources(sun.stamps, none, none, 91);
            CHECK(quiet.source == CameraSource::None && quiet.stamps.cameraFrame == 50 && quiet.stamps.sunDrawFrame == 50);
        }

        // The ages. A camera stands for kMaxCameraAge frames after its refresh, not one more; a clock that went backwards (a reset) stands for nothing; no camera ever is no camera.
        {
            HoldStamps stamps;
            stamps.haveCamera = true;
            stamps.cameraFrame = 100;
            CHECK(CameraStands(stamps, 100) && CameraStands(stamps, 100 + kMaxCameraAge) && !CameraStands(stamps, 100 + kMaxCameraAge + 1));
            CHECK(!CameraStands(stamps, 99));
            CHECK(!CameraStands(HoldStamps {}, 5));
            stamps.haveSunDraw = true;
            stamps.sunDrawFrame = 100;
            CHECK(SunStands(stamps, 100) && SunStands(stamps, 100 + kMaxCameraAge) && !SunStands(stamps, 100 + kMaxCameraAge + 1) && !SunStands(stamps, 99));
            CHECK(!SunStands(HoldStamps {}, 5));
            CHECK(kMaxCameraAge == 180);
        }

        // The frame sequences, driven the way the stage drives them: Readback folds the two reads, the tone map asks CameraStands (false = skip "camera"), the run asks SunStands.
        struct Outcome
        {
            uint32_t skippedCamera = 0;
            uint32_t ran = 0;
            uint32_t sunFrames = 0;
            uint32_t fromSunDraw = 0;
            uint32_t fromLightDraw = 0;
        };
        const auto drive = [](uint64_t firstFrame, uint64_t frames, auto copiesOf) {
            Outcome outcome;
            HoldStamps stamps;
            for (uint64_t frame = firstFrame; frame < firstFrame + frames; ++frame)
            {
                CameraCopy sunDraw, lightDraw;
                copiesOf(frame, sunDraw, lightDraw);
                const CameraFold fold = FoldCameraSources(stamps, sunDraw, lightDraw, frame);
                stamps = fold.stamps;
                outcome.fromSunDraw += fold.source == CameraSource::SunDraw;
                outcome.fromLightDraw += fold.source == CameraSource::LightDraw;
                if (!CameraStands(stamps, frame))
                {
                    ++outcome.skippedCamera;
                    continue;
                }
                ++outcome.ran;
                outcome.sunFrames += SunStands(stamps, frame);
            }
            return outcome;
        };
        // (a) The benchmark: no sun-shaped light draw in 1,000 frames, a camera from the light draws every frame (frame 1 is the first the ring can read): runs every frame, never a sun.
        {
            const Outcome outcome = drive(1, 1000, [](uint64_t, CameraCopy&, CameraCopy& light) { light = CameraCopy { true, true }; });
            CHECK(outcome.skippedCamera == 0 && outcome.ran == 1000 && outcome.sunFrames == 0 && outcome.fromLightDraw == 1000 && outcome.fromSunDraw == 0);
        }
        // (b) Both every frame: the sun draw's copy every frame, the sun every frame (nothing about a frame that has the sun draw changes).
        {
            const Outcome outcome = drive(1, 1000, [](uint64_t, CameraCopy& sun, CameraCopy& light) { sun = light = CameraCopy { true, true }; });
            CHECK(outcome.skippedCamera == 0 && outcome.ran == 1000 && outcome.sunFrames == 1000 && outcome.fromSunDraw == 1000 && outcome.fromLightDraw == 0);
        }
        // (c) Neither source for 200 frames after a first read at frame 10: the camera stands 180 frames and is then skipped, as before (the stage never draws a stale up vector).
        {
            const Outcome outcome = drive(10, 400, [](uint64_t frame, CameraCopy& sun, CameraCopy&) { sun = CameraCopy { frame == 10, true }; });
            CHECK(outcome.ran == 181 && outcome.skippedCamera == 400 - 181 && outcome.sunFrames == 181);
            const Outcome never = drive(1, 300, [](uint64_t, CameraCopy&, CameraCopy&) {});
            CHECK(never.ran == 0 && never.skippedCamera == 300);
        }
        // (d) The sun draw is there for 300 frames, then gone for 400 while the light draws keep landing: the stage keeps running on the fresh camera, the sun stands exactly 180 frames
        //     after its last copy and no more (no lobe from a zone the player has left), then the ambient term alone.
        {
            const Outcome outcome = drive(1, 700, [](uint64_t frame, CameraCopy& sun, CameraCopy& light) {
                light = CameraCopy { true, true };
                if (frame <= 300)
                    sun = CameraCopy { true, true };
            });
            CHECK(outcome.skippedCamera == 0 && outcome.ran == 700);
            CHECK(outcome.sunFrames == 300 + kMaxCameraAge);
            CHECK(outcome.fromSunDraw == 300 && outcome.fromLightDraw == 400);
        }
        // (e) The sun draw lands with a table that is refused for 100 frames while the light draws' copy is good: the camera never lapses, the sun lapses 180 frames after the last good copy.
        {
            const Outcome outcome = drive(1, 400, [](uint64_t frame, CameraCopy& sun, CameraCopy& light) {
                light = CameraCopy { true, true };
                sun = CameraCopy { true, frame <= 50 };
            });
            CHECK(outcome.skippedCamera == 0 && outcome.sunFrames == 50 + kMaxCameraAge && outcome.fromSunDraw == 50 && outcome.fromLightDraw == 350);
        }
        // (f) A fallback that lands a refused table and no sun draw ever: the camera never stands. (A broken build's bytes must not light the stage.)
        {
            const Outcome outcome = drive(1, 300, [](uint64_t, CameraCopy&, CameraCopy& light) { light = CameraCopy { true, false }; });
            CHECK(outcome.ran == 0 && outcome.skippedCamera == 300 && outcome.fromLightDraw == 0);
        }
        // (g) The zone-transition shape of the 2026-11-07 session: the sun draw's copy stops, the light draws' copy lands through the stretch; no frame is skipped for "camera".
        {
            const Outcome outcome = drive(1, 1200, [](uint64_t frame, CameraCopy& sun, CameraCopy& light) {
                light = CameraCopy { true, true };
                if (frame < 400 || frame > 800)
                    sun = CameraCopy { true, true };
            });
            CHECK(outcome.skippedCamera == 0 && outcome.ran == 1200);
        }
        CHECK(Contains(FormatCameraSource(CameraSource::LightDraw), "light draws' copy") && Contains(FormatCameraSource(CameraSource::SunDraw), "sun draw's copy"));
        CHECK(std::string(CameraSourceName(CameraSource::None)) == "no camera");
    }

    return Finish("atmo_rules");
}
