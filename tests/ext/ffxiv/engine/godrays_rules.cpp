// God rays: the volumetric march's pure rules, on the host (ext/ffxiv/engine/GodraysRules.h).
//
// The engine facts the rules encode, and what a wrong encoding would break:
//   the fog decode reads the byte offsets the engine's own fog post reads - a wrong offset puts another buffer's float in the march's
//   density, and the beams take whatever shape that float gives them;
//   the density profile is the engine's own two-exponential formula, with the 0.545968 literal the disassembly carries in three places -
//   a march that invented its own profile would not be "the engine's own density" at boost 1.0;
//   the compare, the fades and the cascade order are the writer's own bytecode's tail - a wrong compare direction lights the shadows
//   instead of the beams;
//   the keys' clamps and defaults are the card's contract: every default is today's picture, and a NaN ini value must never turn the
//   march into something the keys did not ask for.
#include "check.h"

#include "ext/ffxiv/engine/GodraysRules.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

using namespace ext::ffxiv::godrays;
// The radial's own rules are qualified explicitly (ext::ffxiv::shafts::) to avoid the same-named ambiguity with the march's types.
// The using-declarations below beat the using-namespace ambiguity: the march's own types win over the radial's same-named ones.
using ext::ffxiv::godrays::Decline;
using ext::ffxiv::godrays::Outcome;
using ext::ffxiv::godrays::SkipReason;
using ext::ffxiv::godrays::CardCounts;
using ext::ffxiv::godrays::CardNumbers;
using ext::ffxiv::godrays::Params;
using ext::ffxiv::godrays::FogState;
using ext::ffxiv::godrays::Cascade;

namespace
{
float QuietNan() { return std::numeric_limits<float>::quiet_NaN(); }

// A fog state with values the tests can reason about: layer 1 dense low fog (falloff positive, so the density grows as y drops below
// the reference height), layer 2 a thin haze above.
FogState TestFog()
{
    FogState fog;
    fog.fogColor[0] = 0.30f;
    fog.fogColor[1] = 0.40f;
    fog.fogColor[2] = 0.50f;
    fog.sunDir[0] = 0.0f;
    fog.sunDir[1] = 0.7071f;
    fog.sunDir[2] = -0.7071f;
    fog.volumeFlag = 1.0f;
    fog.lutA = 0.10f;
    fog.lutB = 0.05f;
    fog.cameraPos[0] = 1.0f;
    fog.cameraPos[1] = 2.0f;
    fog.cameraPos[2] = 3.0f;
    fog.distA = 1.0f;
    fog.distB = 100.0f;
    fog.density1 = 0.5f;
    fog.fallOff1 = 0.25f;
    fog.refHeight1 = 10.0f;
    fog.density2 = 0.2f;
    fog.fallOff2 = 0.05f;
    fog.refHeight2 = 40.0f;
    fog.minOpacity = 0.02f;
    fog.mixScale = 1.0f;
    fog.diIntensity = 0.8f;
    fog.diExponent = 4.0f;
    return fog;
}

// The engine's own formula, written straight from the disassembly (the asm's two mad/exp pairs and the 0.545968 literal), as the
// independent reference the decode and the evaluation are checked against. The march's own copy lives in GodraysRules.h; this one is
// the test's, so a bug in either is visible as a disagreement.
float EngineDensityAt(const FogState& fog, float worldY, float lift)
{
    const double e1 = std::exp(double(fog.fallOff1) * (double(fog.refHeight1) - double(worldY)) * 0.545968 * double(lift));
    const double e2 = std::exp(double(fog.fallOff2) * (double(fog.refHeight2) - double(worldY)) * 0.545968 * double(lift));
    return float(double(fog.density1) * e1 + double(fog.density2) * e2);
}

// The captured pose's two cascade instances, as cbdump.csv of session-render-4 carries them (the march's fixture for the fade bands and
// the shared map row): instance ...b320 (an assembly draw's) and instance ...29220 (the sun draw's re-bind of one of the four).
constexpr float kInstanceB320[32] {
    0.018665f, 0.015461f, -0.000000f, 0.500257f,
    0.000505f, -0.000609f, 0.009411f, 0.250899f,
    0.016293f, -0.019669f, -0.002147f, 0.446442f,
    0.000000f, 0.000000f, 0.000000f, 1.000000f,
    1.000000f, 0.004000f, 190.000000f, 0.000000f,
    0.000488f, 0.000122f, 0.052632f, 1.000000f,
    -2.900000f, 24.998795f, 0.666667f, 0.000000f,
    -0.000264f, -39.014683f, -30.291693f, 1.000000f,
};
constexpr float kInstance29220[32] {
    0.037330f, 0.030923f, -0.000000f, 0.000027f,
    -0.004037f, 0.004873f, -0.075288f, -1.006702f,
    0.016293f, -0.019669f, -0.002147f, 0.446442f,
    0.000000f, 0.000000f, 0.000000f, 1.000000f,
    1.000000f, 0.002000f, 190.000000f, 0.000000f,
    0.000488f, 0.000488f, 0.052632f, 1.000000f,
    0.100000f, 190.000000f, 100000.000000f, 0.000000f,
    -0.000264f, -39.014683f, -30.291693f, 1.000000f,
};

Cascade CascadeOf(const float* instance)
{
    Cascade cascade;
    std::memcpy(cascade.matrix, instance, sizeof cascade.matrix);
    std::memcpy(cascade.fade, instance + 24, sizeof cascade.fade);
    cascade.distanceFar = instance[18];
    cascade.rampScale = instance[22];
    cascade.rampTarget = instance[23];
    cascade.shaped = ext::ffxiv::shafts::ShadowMatrixShape(instance);
    return cascade;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: godrays_rules <repo>\n");
        return 2;
    }
    (void)argc;
    (void)argv;

    // ---- the fog decode: every field, from the offsets the engine's own fog post reads -------------------------------------------
    {
        // cFogParam: eight float4s with a recognizable value per float, so a swapped offset is visible as a swapped value.
        unsigned char fog[kFogParamBytes] {};
        for (uint32_t i = 0; i < kFogParamBytes / 4; ++i)
        {
            const float value = float(i + 1) * 0.25f;
            std::memcpy(fog + i * 4, &value, 4);
        }
        unsigned char exp[kExpHeightFogBytes] {};
        for (uint32_t i = 0; i < kExpHeightFogBytes / 4; ++i)
        {
            const float value = float(i + 1) * 0.5f;
            std::memcpy(exp + i * 4, &value, 4);
        }
        const FogState decoded = DecodeFog(fog, exp);
        CHECK(decoded.fogColor[0] == 0.25f);
        CHECK(decoded.fogColor[1] == 0.50f);
        CHECK(decoded.fogColor[2] == 0.75f);
        CHECK(decoded.sunDir[0] == 2.25f);  // row 2's xyz: the fog state's own sun (floats 8..10)
        CHECK(decoded.sunDir[1] == 2.50f);
        CHECK(decoded.sunDir[2] == 2.75f);
        CHECK(decoded.volumeFlag == 3.00f); // row 2's .w: the 3-D volume branch's gate (float 11)
        CHECK(decoded.lutA == 4.00f);       // row 3's .w
        CHECK(decoded.cameraPos[0] == 4.25f); // row 4's xyz (floats 16..18)
        CHECK(decoded.cameraPos[1] == 4.50f);
        CHECK(decoded.cameraPos[2] == 4.75f);
        CHECK(decoded.lutB == 5.00f); // row 4's .w (float 19)
        CHECK(decoded.distA == 5.25f); // row 5's .x (float 20)
        CHECK(decoded.distB == 5.50f); // row 5's .y (float 21)
        CHECK(decoded.minOpacity == 2.5f);  // exp row 1's .x: the opacity floor (float 4)
        CHECK(decoded.density1 == 1.5f);    // exp row 0's .z
        CHECK(decoded.fallOff1 == 1.0f);    // exp row 0's .y
        CHECK(decoded.refHeight1 == 2.0f);  // exp row 0's .w
        CHECK(decoded.density2 == 3.5f);    // exp row 1's .z
        CHECK(decoded.fallOff2 == 3.0f);    // exp row 1's .y
        CHECK(decoded.refHeight2 == 4.0f);  // exp row 1's .w
        CHECK(decoded.mixScale == 4.5f);    // exp row 2's .x (float 8)
        CHECK(decoded.diIntensity == 5.0f); // exp row 2's .y (float 9)
        CHECK(decoded.diExponent == 5.5f);  // exp row 2's .z (float 10)
        CHECK(FogStateUsable(decoded));
        // A null half answers the default state, which the gate refuses: a capture that landed half a pair is a frame without fog, not
        // a frame with zeros the march divides by.
        const FogState half = DecodeFog(fog, nullptr);
        CHECK(!FogStateUsable(half));
        CHECK(!FogStateUsable(DecodeFog(nullptr, exp)));
        // A NaN anywhere the gate reads is refused.
        FogState nanFog = TestFog();
        nanFog.density1 = QuietNan();
        CHECK(!FogStateUsable(nanFog));
        FogState negative = TestFog();
        negative.density2 = -0.1f;
        CHECK(!FogStateUsable(negative));
        FogState backward = TestFog();
        backward.distB = -1.0f;
        CHECK(!FogStateUsable(backward));
    }

    // ---- the density profile: the engine's own formula, the two-exp shape, the height lift ----------------------------------------
    {
        const FogState fog = TestFog();
        // Identity with the independent reference at lift 1 (and any y): the march's density is the engine's own at boost 1.0.
        for (float y = -20.0f; y <= 120.0f; y += 7.5f)
            CHECK(std::fabs(FogDensityAt(fog, y, 1.0f) - EngineDensityAt(fog, y, 1.0f)) < 1.0e-5f);
        // The two-exp shape: at the first layer's reference height the first exponential is exactly 1, so the density is density1 plus
        // the second layer's contribution there.
        const float atHeight1 = FogDensityAt(fog, fog.refHeight1, 1.0f);
        const float secondOnly = fog.density2 * std::exp(fog.fallOff2 * (fog.refHeight2 - fog.refHeight1) * 0.545968f);
        CHECK(std::fabs(atHeight1 - (fog.density1 + secondOnly)) < 1.0e-5f);
        // Below the reference height the density grows (the captured convention: denser low), above it shrinks.
        CHECK(FogDensityAt(fog, fog.refHeight1 - 10.0f, 1.0f) > FogDensityAt(fog, fog.refHeight1, 1.0f));
        CHECK(FogDensityAt(fog, fog.refHeight1 + 10.0f, 1.0f) < FogDensityAt(fog, fog.refHeight1, 1.0f));
        // The height lift: 0 flattens to the sum of the two densities, 2 doubles the height gradient's exponent, 1 is the engine's own.
        CHECK(std::fabs(FogDensityAt(fog, 0.0f, 0.0f) - (fog.density1 + fog.density2)) < 1.0e-5f);
        CHECK(FogDensityAt(fog, 0.0f, 2.0f) > FogDensityAt(fog, 0.0f, 1.0f));
        // At the first layer's reference height the first exponential is exactly 1 whatever the lift; the second layer still responds.
        CHECK(FogDensityAt(fog, fog.refHeight1, 2.0f) > FogDensityAt(fog, fog.refHeight1, 1.0f));
        // The profile is never negative whatever the lift.
        for (float lift = 0.0f; lift <= 2.0f; lift += 0.5f)
            CHECK(FogDensityAt(fog, -100.0f, lift) >= 0.0f);
    }

    // ---- the phase: the engine's own exponent, clamped to its own floor of 1 ------------------------------------------------------
    {
        const FogState fog = TestFog(); // diExponent 4.0
        CHECK(std::fabs(PhaseExponent(fog, 1.0f) - 4.0f) < 1.0e-6f);
        CHECK(std::fabs(PhaseExponent(fog, 2.0f) - 8.0f) < 1.0e-6f);
        CHECK(std::fabs(PhaseExponent(fog, 0.5f) - 2.0f) < 1.0e-6f);
        FogState small = TestFog();
        small.diExponent = 0.5f; // below the engine's own floor: the shaders clamp it to 1
        CHECK(std::fabs(PhaseExponent(small, 1.0f) - 1.0f) < 1.0e-6f);
        FogState nan = TestFog();
        nan.diExponent = QuietNan();
        CHECK(std::fabs(PhaseExponent(nan, 1.0f) - 1.0f) < 1.0e-6f); // a broken exponent reads as the floor, not as a NaN weight
        CHECK(std::fabs(PhaseExponent(fog, QuietNan()) - 4.0f) < 1.0e-6f); // and a broken key reads as its default
    }

    // ---- the march's bounds: the far band, the camera's world height --------------------------------------------------------------
    {
        const float fars[4] { 190.0f, 120.0f, 190.0f, 60.0f };
        CHECK(std::fabs(MarchFarOf(fars, 4) - 190.0f) < 1.0e-6f); // the largest of the captured cascades' own distance rows
        CHECK(std::fabs(MarchFarOf(nullptr, 4) - kMaxMarchFar) < 1.0e-6f); // nothing readable: the band's far end, still a bound
        const float nans[2] { QuietNan(), 90.0f };
        CHECK(std::fabs(MarchFarOf(nans, 2) - 90.0f) < 1.0e-6f); // a NaN row is skipped, not propagated
        const float tiny[1] { 1.0f };
        CHECK(std::fabs(MarchFarOf(tiny, 1) - kMinMarchFar) < 1.0e-6f); // clamped into the band a sane build can produce
        const float huge[1] { 5000.0f };
        CHECK(std::fabs(MarchFarOf(huge, 1) - kMaxMarchFar) < 1.0e-6f);
        // The camera's own world height is the inverse view's second row's .w (the row dotted with the view origin).
        const float row1[4] { 0.0f, 1.0f, 0.0f, 42.5f };
        CHECK(std::fabs(CameraWorldY(row1) - 42.5f) < 1.0e-6f);
        CHECK(CameraWorldY(nullptr) == 0.0f);
    }

    // ---- the cascades: the shape, the footprint order, the usability --------------------------------------------------------------
    {
        const Cascade b320 = CascadeOf(kInstanceB320);
        const Cascade suns = CascadeOf(kInstance29220);
        CHECK(b320.shaped && suns.shaped); // both captured instances carry the shadow matrix's shape
        // The shared third row: the two captured instances carry byte-identical light-axis rows, so the light-space z mapping is
        // cascade-independent - which is what makes the march's selection a footprint question and not a view-z one.
        for (uint32_t i = 0; i < 4; ++i)
            CHECK(kInstanceB320[8 + i] == kInstance29220[8 + i]);
        // The footprints: the sun draw's instance spans the smaller world extent (the larger uv-per-world scale), so it is the finer
        // cascade and comes first in the march's order.
        const float b320Footprint = CascadeFootprint(b320);
        const float sunsFootprint = CascadeFootprint(suns);
        CHECK(sunsFootprint > 0.0f && b320Footprint > 0.0f);
        CHECK(sunsFootprint < b320Footprint);
        Cascade cascades[kCascadeCount] { b320, suns, Cascade {}, b320 };
        cascades[2].shaped = false;
        uint32_t order[kCascadeCount] {};
        const uint32_t count = CascadeOrder(cascades, order, kCascadeCount);
        CHECK(count == 3); // the unusable one is left out
        CHECK(order[0] == 1); // the finest first
        CHECK(order[1] == 0 || order[1] == 3);
        CHECK(order[2] == 3 || order[1] == 3);
        CHECK(CascadeUsable(b320));
        Cascade broken = b320;
        broken.fade[1] = QuietNan();
        CHECK(!CascadeUsable(broken)); // a NaN fade row is not one the march may project with
        Cascade unshaped = b320;
        unshaped.shaped = false;
        CHECK(!CascadeUsable(unshaped));
    }

    // ---- the compare: the atlas texel size, the disc scale, the shadowed rule -----------------------------------------------------
    {
        float texel[2] {};
        AtlasTexelSize(2048, 8192, texel);
        CHECK(std::fabs(texel[0] - 1.0f) < 1.0e-6f);
        CHECK(std::fabs(texel[1] - 0.25f) < 1.0e-6f); // the writer's own resinfo arithmetic: min(w,h) over each
        AtlasTexelSize(0, 8192, texel);
        CHECK(texel[0] == 0.0f && texel[1] == 0.0f);
        // The disc scale from the shared map row's first float: -0.000264 * -15 = +0.00396, the captured instances' own value.
        CHECK(std::fabs(DiscScale(-0.000264f) - 0.00396f) < 1.0e-8f);
        CHECK(DiscScale(QuietNan()) == 0.0f);
        // The tap's depth correction: the sample's z pulled toward the camera by the tap's radius times the scale.
        CHECK(std::fabs(TapDepth(0.5f, 0.7f, 0.00396f) - (0.5f - 0.7f * 0.00396f)) < 1.0e-8f);
        // The compare: shadowed when the tap's depth is at or beyond the gathered texel (the writer's own ge), and a broken read is a
        // shadowed sample - a broken atlas takes beams away instead of filling the frame with glow.
        CHECK(ShadowedAt(0.5f, 0.5f));
        CHECK(ShadowedAt(0.7f, 0.5f));
        CHECK(!ShadowedAt(0.3f, 0.5f));
        CHECK(ShadowedAt(0.5f, QuietNan()));
        CHECK(ShadowedAt(QuietNan(), 0.5f));
    }

    // ---- the fades and the distance ramp: the captured instances' own band edges --------------------------------------------------
    {
        const Cascade b320 = CascadeOf(kInstanceB320);
        const Cascade suns = CascadeOf(kInstance29220);
        const float shadow = 0.25f;
        // Instance ...b320: the fades are inert inside the window's depth range and the distance ramp lights everything (its band ends
        // near the camera), so the visibility is the compare's own answer - the march's beams then agree with that cascade's rows.
        for (float viewZ = -1.0f; viewZ >= -20.0f; viewZ -= 1.0f)
        {
            const float visibility = CascadeVisibility(shadow, viewZ, b320.fade, b320.distanceFar, b320.rampScale, b320.rampTarget);
            CHECK(std::fabs(visibility - shadow) < 1.0e-5f);
        }
        // Beyond its band's edge (~25 m) the cascade lights everything: the march inherits the engine's own band edge.
        CHECK(CascadeVisibility(shadow, -30.0f, b320.fade, b320.distanceFar, b320.rampScale, b320.rampTarget) == 1.0f);
        // The band's edge ramp starts near -24 m (fadeB = sat((viewZ + 25) * 0.6667) drops below 1 approaching -25).
        CHECK(CascadeVisibility(shadow, -24.0f, b320.fade, b320.distanceFar, b320.rampScale, b320.rampTarget) > shadow);
        // Instance ...29220: authoritative across the whole shadow distance, lit inside its near edge (0.1 m) and beyond 190 m.
        CHECK(CascadeVisibility(shadow, -0.05f, suns.fade, suns.distanceFar, suns.rampScale, suns.rampTarget) == 1.0f);
        CHECK(std::fabs(CascadeVisibility(shadow, -50.0f, suns.fade, suns.distanceFar, suns.rampScale, suns.rampTarget) - shadow) < 1.0e-5f);
        CHECK(CascadeVisibility(shadow, -195.0f, suns.fade, suns.distanceFar, suns.rampScale, suns.rampTarget) == 1.0f);
        // A broken compare is a shadowed sample, never a lit one.
        CHECK(CascadeVisibility(QuietNan(), -50.0f, suns.fade, suns.distanceFar, suns.rampScale, suns.rampTarget) == 0.0f);
    }

    // ---- the keys: the clamps, the NaN answers, the defaults ---------------------------------------------------------------------
    {
        CHECK(ClampSteps(0) == uint32_t(kMinSteps));
        CHECK(ClampSteps(1000) == uint32_t(kMaxSteps));
        CHECK(ClampSteps(32) == 32u);
        CHECK(ClampSoftness(-3) == 0u);
        CHECK(ClampSoftness(9) == 2u);
        CHECK(ClampBoost(QuietNan()) == kDefaultBoost);
        CHECK(ClampBoost(-0.5f) == kMinBoost);
        CHECK(ClampBoost(5.0f) == kMaxBoost);
        CHECK(ClampIntensity(QuietNan()) == kDefaultIntensity);
        CHECK(ParseResolution("half") == Resolution::Half);
        CHECK(ParseResolution("quarter") == Resolution::Quarter);
        CHECK(ParseResolution("") == Resolution::Quarter); // anything else reads as the default
        CHECK(ParseColour("enginefog") == Colour::EngineFog);
        CHECK(ParseColour("white") == Colour::White);
        CHECK(ParseColour("auto") == Colour::Auto);
        CHECK(ParseColour("nonsense") == Colour::Auto);
        // Every default is today's picture: the march off, the engine's own density and phase, the point compare, the jitter on.
        const Params defaults = BuildParams(32, "quarter", 1.0f, 1.0f, 1.0f, "auto", 0, true, 1.0f);
        CHECK(defaults.steps == 32u && defaults.resolution == Resolution::Quarter && defaults.fogBoost == 1.0f &&
              defaults.heightLift == 1.0f && defaults.intensity == 1.0f && defaults.colour == Colour::Auto &&
              defaults.softness == 0u && defaults.jitter && defaults.phaseBoost == 1.0f);
        // A NaN anywhere reads as the default, so a broken ini value never turns the march into something the keys did not ask for.
        const Params nans = BuildParams(kDefaultSteps, "", QuietNan(), QuietNan(), QuietNan(), "", 0, true, QuietNan());
        CHECK(nans.steps == uint32_t(kDefaultSteps) && nans.resolution == Resolution::Quarter && nans.fogBoost == kDefaultBoost &&
              nans.heightLift == kDefaultBoost && nans.intensity == kDefaultIntensity && nans.colour == Colour::Auto &&
              nans.softness == uint32_t(kDefaultSoftness) && nans.phaseBoost == kDefaultBoost);
        // The feature-bit rule: the master switch and an intensity above zero - intensity 0 is today's picture and runs nothing.
        CHECK(Wanted(true, defaults));
        Params off = defaults;
        off.intensity = 0.0f;
        CHECK(!Wanted(true, off));
        CHECK(!Wanted(false, defaults));
    }

    // ---- the march target's extent: a quarter or a half of the window ------------------------------------------------------------
    {
        CHECK(MarchExtent(1920.0f, Resolution::Quarter) == 480u);
        CHECK(MarchExtent(1080.0f, Resolution::Quarter) == 270u);
        CHECK(MarchExtent(1920.0f, Resolution::Half) == 960u);
        CHECK(MarchExtent(1080.0f, Resolution::Half) == 540u);
        CHECK(MarchExtent(9.0f, Resolution::Quarter) == 3u); // an odd extent keeps its last fraction
        CHECK(MarchExtent(3.0f, Resolution::Quarter) == 0u); // below a quarter's minimum: no one-texel pass
        CHECK(MarchExtent(3.0f, Resolution::Half) == 2u);    // half keeps its last half pixel (the radial's HalfExtent rule)
        CHECK(MarchExtent(1.0f, Resolution::Half) == 0u);
        CHECK(MarchExtent(0.0f, Resolution::Half) == 0u);
    }

    // ---- the composite's bound: the march adds through the radial's own ShaftAdded, and no key can push a channel past the white --
    {
        const float white = 0.838f;
        // A fully lit ray at the default keys adds the same share of the white the radial's full mask does.
        const float light = TermClamped(1.0f) * 1.0f * kMarchGain;
        const float added = ext::ffxiv::shafts::ShaftAdded(0.0f, light, white);
        CHECK(added > 0.0f && added < white);
        // ShaftAdded(0, light, white): head = white, raw = light*white, add = head*raw/(raw+head) = light*white^2/(light*white + white).
        CHECK(std::fabs(added - (light * white * white / (light * white + white))) < 1.0e-6f);
        // No term, no key and no white can push a channel past the white: the bound is the pixel's own room to it.
        for (float term = 0.0f; term <= 8.0f; term += 0.5f)
            for (float scene = 0.0f; scene <= white; scene += 0.1f)
            {
                const float value = ext::ffxiv::shafts::ShaftAdded(scene, TermClamped(term) * 2.0f * kMarchGain, white);
                CHECK(value >= 0.0f);
                CHECK(scene + value <= white + 1.0e-5f);
            }
        // A NaN term is no light, and a term past the clamp is the clamp.
        CHECK(TermClamped(QuietNan()) == 0.0f);
        CHECK(TermClamped(-1.0f) == 0.0f);
        CHECK(std::fabs(TermClamped(9.0f) - kMaxTerm) < 1.0e-6f);
    }

    // ---- the engine fog colour: the LUT u, the sky lerp, the sky pixel's zeroed weight -------------------------------------------
    {
        const FogState fog = TestFog();
        // The LUT's u coordinate: A + B / worldDist, the post's own mapping.
        CHECK(std::fabs(EngineFogLutU(fog, 10.0f) - (0.10f + 0.05f / 10.0f)) < 1.0e-6f);
        CHECK(EngineFogLutU(fog, 0.0f) == 0.0f); // a distance at or below zero is the sky: the weight is zeroed there
        CHECK(EngineFogLutU(fog, -1.0f) == 0.0f);
        // The sky lerp: max(0, LUT.y * (sky - fogColor) + fogColor) per channel, the post's own expression.
        const float sky[3] { 0.60f, 0.70f, 0.80f };
        float out[3] {};
        EngineFogColour(fog, 0.5f, sky, out);
        CHECK(std::fabs(out[0] - (0.5f * (0.60f - 0.30f) + 0.30f)) < 1.0e-6f);
        CHECK(std::fabs(out[2] - (0.5f * (0.80f - 0.50f) + 0.50f)) < 1.0e-6f);
        // A weight of 1 is the sky's own colour, 0 the fog's own, and a negative result clamps at 0.
        EngineFogColour(fog, 1.0f, sky, out);
        CHECK(std::fabs(out[0] - 0.60f) < 1.0e-6f);
        EngineFogColour(fog, 0.0f, sky, out);
        CHECK(std::fabs(out[0] - 0.30f) < 1.0e-6f);
        const float dark[3] { 0.01f, 0.01f, 0.01f };
        EngineFogColour(fog, 1.0f, dark, out);
        CHECK(std::fabs(out[0] - 0.01f) < 1.0e-6f); // weight 1: the result is the sky itself (the lerp stays in [sky, fogColor])
        EngineFogColour(fog, QuietNan(), sky, out);
        CHECK(std::fabs(out[0] - 0.30f) < 1.0e-6f); // a broken weight reads as no sky lerp, not as a NaN colour
    }

    // ---- the outcomes and the log lines: the hold semantics' wordings, the skip decomposition, the stand-down --------------------
    {
        // The chained decline and the owner decline name their outcomes apart: a session that reads "the hold stands" knows the earlier
        // stages' work is still on screen, and "the frame is the engine's own" knows nothing of the chain drew at all.
        const std::string chained = FormatDecline(ext::ffxiv::godrays::Decline::PassFailed, Outcome::HoldStands);
        const std::string owner = FormatDecline(ext::ffxiv::godrays::Decline::PassFailed, Outcome::EngineOwn);
        CHECK(chained.find("the chain's hold stands") != std::string::npos);
        CHECK(chained.find("could not build") != std::string::npos);
        CHECK(owner.find("the frame is the engine's own") != std::string::npos);
        CHECK(owner.find("the chain's hold stands") == std::string::npos);
        CHECK(FormatDecline(ext::ffxiv::godrays::Decline::NoAtlas, Outcome::Skipped).find("no cascade shadow atlas") != std::string::npos);
        // The skip lines name their reasons apart.
        CHECK(FormatSkip(SkipReason::NoAtlas).find("no cascade shadow atlas") != std::string::npos);
        CHECK(FormatSkip(SkipReason::NoFogState).find("no fog state") != std::string::npos);
        CHECK(FormatSkip(SkipReason::NoLight).find("no shadow light") != std::string::npos);
        // The skip decomposition: `skipped` and exactly one reason move together.
        CardCounts counts;
        CountSkip(counts, SkipReason::NoAtlas);
        CountSkip(counts, SkipReason::NoAtlas);
        CountSkip(counts, SkipReason::NoFogState);
        CountSkip(counts, SkipReason::NoLight);
        CHECK(counts.skipped == 4u && counts.noAtlas == 2u && counts.noFog == 1u && counts.noLight == 1u);
        CHECK(counts.noAtlas + counts.noFog + counts.noLight == counts.skipped);
        // Which declines count toward the stand-down: a build this module cannot serve does, a frame without the march in it does not.
        CHECK(CountsTowardsStandDown(ext::ffxiv::godrays::Decline::PassFailed));
        CHECK(CountsTowardsStandDown(ext::ffxiv::godrays::Decline::NoCamera));
        CHECK(CountsTowardsStandDown(ext::ffxiv::godrays::Decline::SizeMismatch));
        CHECK(CountsTowardsStandDown(ext::ffxiv::godrays::Decline::Exception));
        CHECK(!CountsTowardsStandDown(ext::ffxiv::godrays::Decline::NoAtlas));
        CHECK(!CountsTowardsStandDown(ext::ffxiv::godrays::Decline::NoFogState));
        CHECK(!CountsTowardsStandDown(ext::ffxiv::godrays::Decline::NoLight));
        CHECK(!CountsTowardsStandDown(ext::ffxiv::godrays::Decline::SceneHdr));
        CHECK(!CountsTowardsStandDown(ext::ffxiv::godrays::Decline::NotToneMap));
        // The card's line carries the whole decomposition, so a session can add the reasons up and read where every skipped frame went.
        CardNumbers numbers;
        numbers.intensity = 1.0f;
        numbers.frames = 100;
        numbers.runs = 7;
        numbers.held = 2;
        numbers.declined = 3;
        numbers.skipped = 4;
        numbers.noAtlas = 2;
        numbers.noFog = 1;
        numbers.noLight = 1;
        numbers.reads = 9;
        numbers.notReady = 1;
        numbers.refused = 0;
        numbers.marchWidth = 480;
        numbers.marchHeight = 270;
        numbers.windowWidth = 1920;
        numbers.windowHeight = 1080;
        const std::string card = FormatCard(numbers);
        CHECK(card.find("7 with the march") != std::string::npos);
        CHECK(card.find("no atlas: 2") != std::string::npos);
        CHECK(card.find("no fog state: 1") != std::string::npos);
        CHECK(card.find("no shadow light: 1") != std::string::npos);
        CHECK(card.find("480x270") != std::string::npos);
        // The run line names the keys, so a session reads what the march did with the frame.
        const Params defaults = BuildParams(32, "quarter", 1.0f, 1.0f, 1.0f, "auto", 0, true, 1.0f);
        const std::string run = FormatRun(defaults, 1920, 1080, 480, 270, TestFog().sunDir, 1.0f, 0.838f, 0.75f);
        CHECK(run.find("480x270") != std::string::npos && run.find("quarter") != std::string::npos &&
              run.find("softness 0") != std::string::npos && run.find("jitter on") != std::string::npos &&
              run.find("colour auto") != std::string::npos);
    }

    // ---- the 64-refusal stand-down: the march's own knob, the shared session rule -------------------------------------------------
    {
        ext::ffxiv::engine::Session session;
        for (uint32_t i = 0; i < 63; ++i)
            CHECK(!session.NoteDecline(ext::ffxiv::engine::Knob::Godrays));
        CHECK(session.Armed());
        CHECK(session.NoteDecline(ext::ffxiv::engine::Knob::Godrays)); // the 64th is the one that switches the module off
        CHECK(!session.Armed());
        // The streak is the march's own: the shafts' refusals never move it, and the march's runs clear only it.
        ext::ffxiv::engine::Session other;
        other.NoteDecline(ext::ffxiv::engine::Knob::LightShafts);
        other.NoteDecline(ext::ffxiv::engine::Knob::Godrays);
        other.NoteRun(ext::ffxiv::engine::Knob::Godrays);
        CHECK(!other.NoteDecline(ext::ffxiv::engine::Knob::Godrays)); // one run cleared the march's streak, not the shafts'
    }
    return Finish("godrays_rules");
}
