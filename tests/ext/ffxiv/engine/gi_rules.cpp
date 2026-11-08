// Screen-space light bounce: the pure rules (ext/ffxiv/engine/GiRules.h) on the host, no device.
//
// What a wrong rule would break, and what is checked:
//   the keys' clamps and defaults - a NaN reads as the default, a range outside the band clamps in, every default is today's picture;
//   the ray distribution - the basis is orthonormal for any unit normal, the hemisphere's mean cosine is 2/3, the rotational jitter is a
//   function of the pixel alone and moves a ray's angle, and a ray whose theta sits at the hemisphere's edge counts nothing;
//   the reflected radiance - a hit's radiance toward the origin is its own linear scene value: the stored texel squared, per channel, with
//   the surface's own colour left in (scene = a·b gives a²·b², NOT the b² an albedo quotient returned: that read every hit as a white
//   reflector and overstated it by 1/a²), the clamp refuses a runaway (above kMaxRadiance), and negatives and non-finite values refuse;
//   the store's data marker - the irradiance store's alpha is the number of rays an origin traced, whether or not one hit; a texel traced
//   with every ray missed is a valid zero (alpha above 0, rgb 0) and an origin that could not be traced carries alpha 0 (no data);
//   the origin's gate - the engine's own special-material bytes answer zero, the specular-albedo selector answers zero, the pack's diffuse
//   weight is one minus the share it reports, and a NaN store answers zero;
//   the estimator - the hemisphere integral of a constant radiance over K cosine rays is that radiance, pairs of a miss carry nothing, and
//   the estimate is not renormalized by the pairs that hit;
//   the smooth - the stencil is the dense 5x5 square of integer tap positions, centre and the four direct neighbours included, its weights are
//   the 1-4-6-4-1 outer product (sum 256, symmetric, falling with the distance, separable), and its response at the checkerboard frequency
//   (pi,pi) - and at (pi,0) and (0,pi) - is exactly zero: the parity classes of the checkerboard are mixed, which the old ring (every tap on
//   an even x+y) never did. The decisive proof of that on the real shader is gi_smooth_warp; this block is the second check on the rules'
//   own table;
//   the compose - the strength key scales the estimate, a zero strength is the identity, and the white-room bound itself is the radial's own
//   ShaftAdded (its tests pin it; this test only checks that the gain passes nothing at a zero);
//   the log lines - the wording of the chained and the owned declines, the card's decomposition line, and the once-per-run line.
#include "check.h"

#include "ext/ffxiv/engine/GiRules.h"

#include <cmath>
#include <cstring>
#include <string>

using namespace ext::ffxiv::engine::gi;

namespace
{
// The little three-vector the fixture maths needs, with the array the rules' functions read.
struct float3
{
    float v[3] {};
};

float3 Make3(float x, float y, float z) { return float3 { { x, y, z } }; }
} // namespace

int main()
{
    // ---- the keys ----------------------------------------------------------------------------------------------------------------
    {
        const Params params = BuildParams(99, 99, 99.0f, 99.0f, "half");
        CHECK_EQ(params.rays, uint32_t(kMaxRays));
        CHECK_EQ(params.steps, uint32_t(kMaxSteps));
        CHECK_EQ(params.range, kMaxRange);
        CHECK_EQ(params.strength, kMaxStrength);
        CHECK_EQ(params.resolution, Resolution::Half);
        const Params quarter = BuildParams(0, 0, 0.0f, 0.0f, "quarter");
        CHECK_EQ(quarter.rays, uint32_t(kMinRays));
        CHECK_EQ(quarter.steps, uint32_t(kMinSteps));
        CHECK_EQ(quarter.range, kMinRange);
        CHECK_EQ(quarter.resolution, Resolution::Quarter);
        // A NaN reads as the default, so a broken ini value never turns the stage into something the keys did not ask for.
        const Params nan = BuildParams(0, 0, std::sqrt(-1.0f), std::sqrt(-1.0f), "half");
        CHECK_EQ(nan.range, kDefaultRange);
        CHECK_EQ(nan.strength, kDefaultStrength);
        // The strength default and the pass's gain, by value (gi_pins reads every place that writes the default against these): the default 0.55
        // of the full diffuse bounce (strength 1 times kGain = 1/pi, the Lambertian normalisation), the integration's choice (2026-11-07): the lowest
        // strength that keeps the bounce visible on the bright-room frame (lamp wall +7.1 %, fireplace +4.6 %) AND the rain square's speckle excess
        // over the constant-irradiance control under its 0.01 % bar (0.0041 % at 0.55; 0.70 gave 0.0115 %, a miss); the slider keeps its whole
        // range and a strength of 0 is the identity. The gain stays 1/pi: the knob that was calibrated is the strength.
        CHECK_EQ(kDefaultStrength, 0.55f);
        CHECK(std::fabs(double(kGain) - 1.0 / 3.14159265358979323846) < 1.0e-7);
        CHECK(std::fabs(double(kGain) * double(kPi) - 1.0) < 1.0e-6);
        CHECK_EQ(kMinStrength, 0.0f);
        CHECK_EQ(kMaxStrength, 1.0f);
        CHECK_EQ(BuildParams(4, 16, 5.0f, 0.0f, "half").strength, 0.0f);
        CHECK(IsIdentity(BuildParams(4, 16, 5.0f, 0.0f, "half")));
        CHECK(!IsIdentity(BuildParams(4, 16, 5.0f, kDefaultStrength, "half")));
        CHECK_EQ(AddedLightShare(2.0f, 0.25f, BuildParams(4, 16, 5.0f, 0.0f, "half")), 0.0f); // a strength of 0 adds nothing
        CHECK_EQ(nan.rays, uint32_t(kMinRays)); // an int key cannot carry a NaN, so it clamps into the band's own low end
        CHECK_EQ(BuildParams(4, 16, 5.0f, 0.5f, "half").range, kDefaultRange);
        // The identity rules: a strength of 0 runs nothing; the pass's own resolution is a separate question from the strength's.
        CHECK(IsIdentity(BuildParams(4, 16, 5.0f, 0.0f, "half")));
        CHECK(!IsIdentity(BuildParams(4, 16, 5.0f, 0.5f, "half")));
        CHECK(Wants(BuildParams(4, 16, 5.0f, 0.5f, "half")));
        // The extent rules: an odd window extent keeps its last half pixel, and a zero extent stays zero.
        CHECK_EQ(GiExtent(96.0f, Resolution::Half), 48u);
        CHECK_EQ(GiExtent(96.0f, Resolution::Quarter), 24u);
        CHECK_EQ(GiExtent(95.0f, Resolution::Half), 48u);
        CHECK_EQ(GiExtent(1.0f, Resolution::Half), 0u);
        CHECK_EQ(GiExtent(0.0f, Resolution::Half), 0u);
        CHECK_EQ(ParseResolution("half"), Resolution::Half);
        CHECK_EQ(ParseResolution("QUARTER"), Resolution::Quarter);
        CHECK_EQ(ParseResolution("anything"), Resolution::Half);
        CHECK(std::string_view(ResolutionName(Resolution::Half)) == "half");
        CHECK(std::string_view(ResolutionName(Resolution::Quarter)) == "quarter");
    }

    // ---- the ray distribution ----------------------------------------------------------------------------------------------------
    {
        // The tangent basis is orthonormal for any unit normal: two unit rows perpendicular to the normal and to each other.
        for (const float3 normal : { Make3(0.0f, 1.0f, 0.0f), Make3(0.70710678f, 0.0f, 0.70710678f), Make3(-0.5f, 0.5f, -0.70710678f) })
        {
            float basis[3][3] {};
            CHECK(TangentBasis(normal.v, basis));
            auto dot = [](const float a[3], const float b[3]) {
                return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            };
            CHECK(std::fabs(dot(basis[0], basis[2])) < 1.0e-6f);
            CHECK(std::fabs(dot(basis[1], basis[2])) < 1.0e-6f);
            CHECK(std::fabs(dot(basis[0], basis[1])) < 1.0e-6f);
            CHECK(std::fabs(dot(basis[0], basis[0]) - 1.0f) < 1.0e-5f);
            CHECK(std::fabs(dot(basis[1], basis[1]) - 1.0f) < 1.0e-5f);
            CHECK(std::fabs(dot(basis[2], basis[2]) - 1.0f) < 1.0e-5f);
        }
        // A normal that is not a surface answers false and leaves the basis zeroed: no rays, no bounce.
        float basis[3][3] {};
        const float zero[3] { 0.0f, 0.0f, 0.0f };
        CHECK(!TangentBasis(zero, basis));
        const float nanNormal[3] { std::sqrt(-1.0f), 0.0f, 0.0f };
        CHECK(!TangentBasis(nanNormal, basis));
        // The hemisphere's mean cosine is 2/3 (a cosine pdf's own average), whatever the rotation the hash draws.
        auto dot = [](const float a[3], const float b[3]) {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        };
        float mean = 0.0f;
        const float n[3] { 0.0f, 1.0f, 0.0f };
        float one[3][3] {};
        CHECK(TangentBasis(n, one));
        constexpr int32_t kCount = 4096;
        for (int32_t i = 0; i < kCount; ++i)
        {
            const float azimuth = Hash01(float(i), 7.0f, 0);
            const float elevation = Hash01(float(i), 11.0f, kHashSaltElevation);
            float dir[3];
            const float cosine = HemisphereDirection(i, kCount, azimuth, elevation, one, dir);
            CHECK(cosine > 0.0f);
            CHECK(std::fabs(dot(dir, dir) - 1.0f) < 1.0e-5f);
            mean += cosine / float(kCount);
        }
        CHECK(std::fabs(mean - 2.0f / 3.0f) < 0.05f);
        // The jitter is a function of the pixel alone and moves the azimuth: two different pixels' hash do not agree, and the same pixel's do.
        CHECK(Hash01(3.0f, 5.0f, 0) != Hash01(4.0f, 5.0f, 0));
        CHECK_EQ(Hash01(3.0f, 5.0f, 0), Hash01(3.0f, 5.0f, 0));
        CHECK(Hash01(3.0f, 5.0f, kHashSaltElevation) != Hash01(3.0f, 5.0f, 0));
        // A ray whose elevation hash sits at the hemisphere's edge counts nothing (its own cosine is zero by construction).
        float dir[3];
        CHECK_EQ(HemisphereDirection(0, 4, 0.25f, 0.0f, one, dir), 0.0f);
        CHECK_EQ(HemisphereDirection(0, 4, std::sqrt(-1.0f), 0.5f, one, dir), 0.0f);
        CHECK_EQ(HemisphereDirection(4, 4, 0.25f, 0.5f, one, dir), 0.0f);
    }

    // ---- the reflected radiance --------------------------------------------------------------------------------------------------
    {
        // The radiance leaving a hit toward the origin is its own linear scene value: the stored texel (the square root of the composed sum)
        // squared. The surface's own colour stays IN: a scene texel written as store·sqrt(light) is store²·light, whatever the store, where the
        // old albedo quotient returned the light alone (every hit read as a white reflector).
        CHECK_EQ(BounceRadiance(0.0f), 0.0f);
        CHECK(std::fabs(BounceRadiance(0.5f) - 0.25f) < 1.0e-7f);
        CHECK(std::fabs(BounceRadiance(1.0f) - 1.0f) < 1.0e-7f);
        CHECK(std::fabs(BounceRadiance(2.0f) - 4.0f) < 1.0e-6f);
        for (const float store : { 0.3f, 0.5f, 0.8f, 1.0f })
        {
            const float light = 4.0f;
            const float scene = store * std::sqrt(light); // the engine's stored form of store²·light
            CHECK(std::fabs(BounceRadiance(scene) - store * store * light) < 1.0e-5f);
            CHECK(BounceRadiance(scene) <= light + 1.0e-5f); // never more than the light that hit it (the albedo is at most 1)
        }
        // A darker surface sends less back than a brighter one under the same light: the colour bleeding the quotient removed.
        CHECK(BounceRadiance(0.3f * 2.0f) < BounceRadiance(0.8f * 2.0f));
        // The bounds: a negative scene is not a light, a NaN and an infinity are not a number, and a runaway is refused (not scaled down).
        const float quietNan = std::sqrt(-1.0f);
        CHECK_EQ(BounceRadiance(-0.5f), 0.0f);
        CHECK_EQ(BounceRadiance(quietNan), 0.0f);
        CHECK_EQ(BounceRadiance(INFINITY), 0.0f);
        CHECK_EQ(BounceRadiance(16.0f), 0.0f); // 256 linear
        // The clamp sits at kMaxRadiance (8.0) in the LINEAR domain, so the stored edge is its square root (2.83): a hit just under it
        // counts whole, a hit just over it counts nothing.
        CHECK_EQ(kMaxRadiance, 8.0f);
        const float edge = std::sqrt(kMaxRadiance);
        CHECK(BounceRadiance(edge * 0.999f) > 7.9f && BounceRadiance(edge * 0.999f) < kMaxRadiance);
        CHECK_EQ(BounceRadiance(edge * 1.001f), 0.0f);
        // An emissive surface (a lamp) bounces its own light: stored 2.5 (linear 6.25) comes back whole, and is bounded by the clamp.
        CHECK(std::fabs(BounceRadiance(2.5f) - 6.25f) < 1.0e-5f);
        CHECK(BounceRadiance(2.5f) < kMaxRadiance);
        // The origin's own albedo multiplier is the store squared, at the byte scale.
        CHECK_EQ(AlbedoSquared(0.5f), 0.25f);
        CHECK_EQ(AlbedoSquared(0.0f), 0.0f);
    }

    // ---- the store's data marker -------------------------------------------------------------------------------------------------
    {
        // The irradiance store's alpha counts the rays an origin traced: an origin that traced none (the sky, no view position, a normal the
        // store does not carry, a zero fade) has no data, and one that traced any has it - whether or not a ray hit.
        CHECK_EQ(DataMarker(0), 0.0f);
        CHECK_EQ(DataMarker(1), 1.0f);
        CHECK_EQ(DataMarker(4), 4.0f);
        CHECK_EQ(DataMarker(uint32_t(kMaxRays)), float(kMaxRays));
        CHECK(!HasData(DataMarker(0)));
        CHECK(HasData(DataMarker(1)));
        CHECK(HasData(DataMarker(4)));
        CHECK(!HasData(0.0f));
        CHECK(!HasData(-1.0f));
        CHECK(!HasData(std::nanf("")));
        CHECK(HasData(1.0f));
        // A traced texel whose every ray missed is a VALID ZERO: the estimator answers 0 for it (a miss carries nothing) and the marker still
        // says it was traced, which is what lets the smooth average it in instead of skipping it.
        CHECK_EQ(IrradianceEstimate(0.0f, 0, 4), 0.0f);
        CHECK(HasData(DataMarker(4)));
    }

    // ---- the origin's gate -------------------------------------------------------------------------------------------------------
    {
        // The engine's own special-material bytes answer zero: the light-draw band (32..44) and the byte beside it.
        for (const float special : { 32.0f, 41.0f, 44.0f, 224.0f })
            CHECK_EQ(OriginGate(special, 0.0f, 0.0f), 0.0f);
        CHECK(IsSpecialMaterialId(32.0f));
        CHECK(IsSpecialMaterialId(40.0f));
        CHECK(IsSpecialMaterialId(44.0f));
        CHECK(IsSpecialMaterialId(224.0f));
        CHECK(!IsSpecialMaterialId(128.0f));
        CHECK(!IsSpecialMaterialId(31.0f));
        CHECK(!IsSpecialMaterialId(45.0f));
        CHECK(!IsSpecialMaterialId(255.0f));
        CHECK(!IsSpecialMaterialId(std::sqrt(-1.0f)));
        // The generic deferred surfaces are byte 128, the measured frame's own.
        const float generic = OriginGate(128.0f, 0.0f, 0.0f);
        CHECK(std::fabs(generic - 1.0f) < 1.0e-6f);
        // The pack's diffuse weight is one minus the share it reports, clamped into [0, 1].
        CHECK(std::fabs(OriginGate(128.0f, 0.0f, 0.5f) - 0.5f) < 1.0e-6f);
        CHECK_EQ(OriginGate(128.0f, 0.0f, 1.0f), 0.0f);
        CHECK_EQ(OriginGate(128.0f, 0.0f, -0.5f), 1.0f);
        // The special-colour selector (at or above 1.0) answers zero: another model owns that pixel's colour.
        CHECK_EQ(OriginGate(128.0f, 1.0f, 0.0f), 0.0f);
        CHECK_EQ(OriginGate(128.0f, 2.0f, 0.0f), 0.0f);
        // A NaN store refuses (a cleared texel the sky test missed cannot become a light source).
        const float quietNan = std::sqrt(-1.0f);
        CHECK_EQ(OriginGate(128.0f, quietNan, 0.0f), 0.0f);
        CHECK_EQ(OriginGate(128.0f, 0.0f, quietNan), 0.0f);
        CHECK_EQ(OriginGate(quietNan, 0.0f, 0.0f), 0.0f);
        // The material id the composite decodes: the store's byte, exactly the way the engine's own bytecode reads it.
        CHECK_EQ(MaterialIdOf(0.0f), 0.0f);
        CHECK_EQ(MaterialIdOf(1.0f), 255.0f);
        CHECK_EQ(MaterialIdOf(128.0f / 255.0f), 128.0f);
    }

    // ---- the estimator -----------------------------------------------------------------------------------------------------------
    {
        // The hemisphere integral of a CONSTANT radiance over K cosine rays is that radiance (the pdf's own answer): the estimator's factor is
        // the cosine pdf's own pi share, and the cosine the rays were drawn with carries the hemisphere's own weighting.
        const float n[3] { 0.0f, 1.0f, 0.0f };
        float basis[3][3] {};
        CHECK(TangentBasis(n, basis));
        const float truth = kPi * 0.5f; // a cosine-distributed sample's mean, over one half of the hemisphere's constant radiance
        float sum = 0.0f;
        constexpr uint32_t kRays = 4096;
        uint32_t pairs = 0;
        for (uint32_t i = 0; i < kRays; ++i)
        {
            const float azimuth = Hash01(float(i), 3.0f, 0);
            const float elevation = Hash01(float(i), 13.0f, kHashSaltElevation);
            float dir[3];
            const float cosine = HemisphereDirection(int32_t(i), kRays, azimuth, elevation, basis, dir);
            if (cosine > 0.0f)
            {
                // Every ray of this fixture looks at a constant radiance: the pdf and the cosine cancel into the estimator's pi share.
                sum += 0.5f;
                ++pairs;
            }
        }
        const float estimate = IrradianceEstimate(sum, pairs, kRays);
        CHECK(pairs > 0);
        CHECK(std::fabs(estimate - truth) < 0.01f * truth);
        // The miss's own answer: no pairs, no light.
        CHECK_EQ(IrradianceEstimate(123.0f, 0, kRays), 0.0f);
        CHECK_EQ(IrradianceEstimate(0.0f, 3, kRays), 0.0f);
        CHECK_EQ(IrradianceEstimate(std::sqrt(-1.0f), 3, kRays), 0.0f);
        CHECK_EQ(IrradianceEstimate(1.0f, 3, 0), 0.0f);
        // The estimate is NOT renormalized by the pairs that hit: a pixel with two of four rays hitting carries what the rays saw at the keys'
        // own count, the shade being the answer itself (not a renormalization the miss washes out).
        CHECK(std::fabs(IrradianceEstimate(1.0f, 2, 4) - kPi * 0.25f) < 1.0e-5f);
        // The view fade: a pixel's own bounce fades with its own camera-facing cosine. The origin is a view-space point, so the camera sits at
        // (0,0,0) and a pixel three metres ahead of it carries a view direction of (0,0,+1) toward the camera.
        const float facingOrigin[3] { 0.0f, 0.0f, -3.0f }; // the origin, three metres in front of the camera
        const float cameraFacing[3] { 0.0f, 0.0f, 1.0f };
        CHECK_EQ(ViewFade(facingOrigin, cameraFacing), 1.0f);
        const float tilted[3] { 0.0f, 0.9486833f, 0.31622776f }; // a floor tilted 71 degrees out of the camera's own axis
        CHECK(ViewFade(facingOrigin, tilted) > 0.3f);
        const float turnedAway[3] { 0.0f, 0.0f, -1.0f };
        CHECK_EQ(ViewFade(facingOrigin, turnedAway), 0.0f);
        const float zeroOrigin[3] { 0.0f, 0.0f, 0.0f };
        CHECK_EQ(ViewFade(zeroOrigin, cameraFacing), 0.0f);
        const float quietOrigin[3] { std::sqrt(-1.0f), 0.0f, -3.0f };
        CHECK_EQ(ViewFade(quietOrigin, cameraFacing), 0.0f);
    }

    // ---- the smooth --------------------------------------------------------------------------------------------------------------
    {
        // The stencil: the full 5x5 square (25 taps), the tap positions exact integers inside the reach, every position once.
        CHECK_EQ(kSmoothTaps, 25u);
        CHECK_EQ(double(kSmoothRadius), 2.0);
        bool seen[5][5] {};
        for (uint32_t tap = 0; tap < kSmoothTaps; ++tap)
        {
            const double dx = double(kSmoothOffsets[tap][0]);
            const double dy = double(kSmoothOffsets[tap][1]);
            CHECK(dx == std::floor(dx) && dy == std::floor(dy)); // never a fractional position (the old table's 1.5 became 1 through int())
            CHECK(std::fabs(dx) <= double(kSmoothRadius) && std::fabs(dy) <= double(kSmoothRadius));
            const int ix = int(dx) + 2, iy = int(dy) + 2;
            if (ix >= 0 && ix < 5 && iy >= 0 && iy < 5)
            {
                CHECK(!seen[iy][ix]); // no position twice
                seen[iy][ix] = true;
            }
        }
        for (int iy = 0; iy < 5; ++iy)
            for (int ix = 0; ix < 5; ++ix)
                CHECK(seen[iy][ix]); // and none missing: the centre, the four direct neighbours and the four diagonals are all in

        // The weights: positive, the outer product of the row 1-4-6-4-1 (so separable and symmetric), the centre the largest, a farther tap
        // never heavier, the total 256, and the tap past the set weighs nothing.
        const double row[3] { 6.0, 4.0, 1.0 };
        double total = 0.0, evenParity = 0.0, oddParity = 0.0, checker = 0.0, stripesX = 0.0, stripesY = 0.0;
        for (uint32_t tap = 0; tap < kSmoothTaps; ++tap)
        {
            const int dx = int(kSmoothOffsets[tap][0]), dy = int(kSmoothOffsets[tap][1]);
            const double w = SmoothWeight(tap);
            CHECK_EQ(w, row[std::abs(dx)] * row[std::abs(dy)]);
            total += w;
            (((dx + dy) & 1) == 0 ? evenParity : oddParity) += w;
            checker += w * double(((dx + dy) & 1) == 0 ? 1 : -1);
            stripesX += w * double((dx & 1) == 0 ? 1 : -1);
            stripesY += w * double((dy & 1) == 0 ? 1 : -1);
        }
        CHECK_EQ(total, 256.0);
        // The point of the whole stencil: as much weight on the odd-parity taps as on the even-parity ones, so the response at the
        // checkerboard frequency is exactly zero (and so it is for the one-texel stripes along each axis).
        CHECK_EQ(evenParity, 128.0);
        CHECK_EQ(oddParity, 128.0);
        CHECK_EQ(checker, 0.0);
        CHECK_EQ(stripesX, 0.0);
        CHECK_EQ(stripesY, 0.0);
        // Where the centre and the direct neighbours sit, by value (found by position, whatever the table's order).
        double centre = 0.0, direct = 0.0, diagonal = 0.0, outerCorner = 0.0;
        uint32_t directCount = 0, diagonalCount = 0;
        for (uint32_t tap = 0; tap < kSmoothTaps; ++tap)
        {
            const int dx = int(kSmoothOffsets[tap][0]), dy = int(kSmoothOffsets[tap][1]);
            const double w = SmoothWeight(tap);
            if (dx == 0 && dy == 0)
                centre = w;
            else if (std::abs(dx) + std::abs(dy) == 1)
            {
                direct = w;
                ++directCount;
            }
            else if (std::abs(dx) == 1 && std::abs(dy) == 1)
            {
                diagonal = w;
                ++diagonalCount;
            }
            else if (std::abs(dx) == 2 && std::abs(dy) == 2)
                outerCorner = w;
        }
        CHECK_EQ(directCount, 4u);
        CHECK_EQ(diagonalCount, 4u);
        CHECK_EQ(centre, 36.0);
        CHECK_EQ(direct, 24.0);
        CHECK_EQ(diagonal, 16.0);
        CHECK_EQ(outerCorner, 1.0);
        CHECK(centre > direct && direct > diagonal && diagonal > outerCorner); // a farther tap never weighs more
        // A tap past the set weights nothing, so a caller indexing the whole stencil gets one answer.
        CHECK_EQ(SmoothWeight(kSmoothTaps), 0.0);
    }

    // ---- the outlier control -----------------------------------------------------------------------------------------------------
    {
        // The numbers, by value (gi_pins reads the shader against them): a texel may not exceed 16 x the mean luminance of its other traced
        // neighbours plus 0.1.
        CHECK_EQ(kClampRatio, 16.0f);
        CHECK_EQ(kClampFloor, 0.1f);
        // A flat 0.02 neighbourhood: the cap is 16 x 0.02 + 0.1 = 0.42. A 50x outlier (luminance 1.0) is held to it, a texel under it is left alone,
        // one exactly on it is left alone.
        CHECK(std::fabs(ClampScale(1.0f, 0.02f) - 0.42f) < 1.0e-6f);
        CHECK_EQ(ClampScale(0.3f, 0.02f), 1.0f);
        CHECK_EQ(ClampScale(0.42f, 0.02f), 1.0f);
        // The floor: with traced zeros all around (mean 0) the cap is the floor alone, so a lone hit under it survives and one over it is held to it.
        CHECK_EQ(ClampScale(0.08f, 0.0f), 1.0f);
        CHECK(std::fabs(ClampScale(0.5f, 0.0f) - 0.2f) < 1.0e-6f);
        // The cap grows with the neighbourhood: the same texel is an outlier in a dim one and an ordinary one in a bright one.
        CHECK(ClampScale(1.0f, 0.02f) < 1.0f && ClampScale(1.0f, 0.2f) == 1.0f);
        // Nothing the rule cannot read is changed: a zero, negative or NaN luminance, a negative or NaN mean.
        CHECK_EQ(ClampScale(0.0f, 0.02f), 1.0f);
        CHECK_EQ(ClampScale(-1.0f, 0.02f), 1.0f);
        CHECK_EQ(ClampScale(std::sqrt(-1.0f), 0.02f), 1.0f);
        CHECK_EQ(ClampScale(1.0f, -0.5f), 1.0f);
        CHECK_EQ(ClampScale(1.0f, std::sqrt(-1.0f)), 1.0f);
        // The scale never raises a texel and never takes it below what the cap allows.
        for (const float luminance : { 0.05f, 0.5f, 3.0f, 100.0f })
            for (const float mean : { 0.0f, 0.01f, 0.3f })
            {
                const float scale = ClampScale(luminance, mean);
                CHECK(scale > 0.0f && scale <= 1.0f);
                CHECK(luminance * scale <= kClampRatio * mean + kClampFloor + 1.0e-5f);
            }
    }

    // ---- the edge-aware filter's numbers ------------------------------------------------------------------------------------------
    {
        CHECK_EQ(kFilterIterations, 4u);
        const int32_t expectedSteps[4] { 1, 2, 4, 8 };
        for (uint32_t i = 0; i < kFilterIterations; ++i)
            CHECK_EQ(kFilterSteps[i], expectedSteps[i]);
        for (uint32_t i = 1; i < kFilterIterations; ++i)
            CHECK_EQ(kFilterSteps[i], 2 * kFilterSteps[i - 1]); // the a-trous doubling: the footprint is 2 x (1 + 2 + 4 + 8) + 1 = 31 texels
        CHECK_EQ(kDepthSigma, 0.03f);
        // The depth weight: a tap on the centre's plane weighs 1, one sigma off the plane weighs 1/e, a step to another surface weighs nothing.
        const float centre = 0.2f;
        CHECK_EQ(DepthWeight(0.2f, 0.2f, centre), 1.0f);
        CHECK(std::fabs(DepthWeight(0.2f + 0.03f * centre, 0.2f, centre) - std::exp(-1.0f)) < 1.0e-5f);
        CHECK(std::fabs(DepthWeight(0.2f - 0.03f * centre, 0.2f, centre) - std::exp(-1.0f)) < 1.0e-5f); // either side of the plane
        CHECK(DepthWeight(0.1f, 0.2f, centre) < 1.0e-6f);                                              // a far wall behind a near one
        CHECK(DepthWeight(0.4f, 0.2f, centre) < 1.0e-6f);                                              // a near object in front of a far wall
        CHECK(DepthWeight(0.2f, 0.2f, centre) > DepthWeight(0.21f, 0.2f, centre));                      // the weight falls with the distance from the plane
        // What the rule cannot read weighs nothing, never 1: no centre, no tap, a NaN prediction.
        CHECK_EQ(DepthWeight(0.2f, 0.2f, 0.0f), 0.0f);
        CHECK_EQ(DepthWeight(0.0f, 0.2f, centre), 0.0f);
        CHECK_EQ(DepthWeight(0.2f, std::sqrt(-1.0f), centre), 0.0f);
        // The slope of the inverse depth along an axis: the smaller one-sided step where both sides exist (the other is across an edge), the one that
        // exists, or 0. A plane (equal steps) reads its own step; a silhouette on one side reads the plane's side; a silhouette on the other too.
        CHECK(std::fabs(InverseDepthStep(0.20f, 0.202f, 0.198f) - 0.002f) < 1.0e-6f);
        CHECK(std::fabs(InverseDepthStep(0.20f, 0.40f, 0.20f) - 0.0f) < 1.0e-6f);       // the plus side is an object: the minus side's step (0)
        CHECK(std::fabs(InverseDepthStep(0.20f, 0.20f, 0.40f) - 0.0f) < 1.0e-6f);       // the minus side is an object
        CHECK(std::fabs(InverseDepthStep(0.20f, 0.203f, 0.10f) - 0.003f) < 1.0e-6f);    // the minus side is a far wall (step 0.1): the plus side's 0.003
        CHECK(std::fabs(InverseDepthStep(0.20f, 0.10f, 0.197f) - 0.003f) < 1.0e-6f);    // and the other way round: the minus side's 0.003
        CHECK(std::fabs(InverseDepthStep(0.20f, 0.204f, 0.0f) - 0.004f) < 1.0e-6f);     // no minus side (the extent's edge, an untraced texel): the plus step
        CHECK(std::fabs(InverseDepthStep(0.20f, 0.0f, 0.195f) - 0.005f) < 1.0e-6f);     // no plus side: the minus step
        CHECK_EQ(InverseDepthStep(0.20f, 0.0f, 0.0f), 0.0f);                           // a lone texel: no slope
        // A slanted plane is one surface to its far end: with the measured slope the prediction 16 texels out is the plane's inverse depth, weight 1.
        {
            const float u0 = 0.2f, slope = 0.0015f;
            const float measured = InverseDepthStep(u0, u0 + slope, u0 - slope);
            for (const int32_t offset : { 1, 2, 4, 8, 16, -16 })
                CHECK(DepthWeight(u0 + slope * float(offset), u0 + measured * float(offset), u0) > 0.99f);
            // The same tap judged without the slope (a plain relative depth test) is a different surface at the wide steps.
            CHECK(DepthWeight(u0 + slope * 16.0f, u0, u0) < 0.02f);
        }
    }

    // ---- the capture frame's numbers ----------------------------------------------------------------------------------------------
    {
        // A 6x5 frame of traced texels (marker 4) at 0.02 with one 50x outlier, two untraceable texels (marker 0, a huge colour that must never be
        // read) and a filtered image of a flat 0.5. The statistics count the traced texels only, the outlier is the one clamped texel, and the mean
        // light is the filtered luminance times the strength key times the gain over the traced texels.
        constexpr uint32_t w = 6, h = 5;
        float raw[w * h * 4], filtered[w * h * 4];
        for (uint32_t i = 0; i < w * h; ++i)
        {
            raw[i * 4] = raw[i * 4 + 1] = raw[i * 4 + 2] = 0.02f;
            raw[i * 4 + 3] = 4.0f;
            filtered[i * 4] = filtered[i * 4 + 1] = filtered[i * 4 + 2] = 0.5f;
            filtered[i * 4 + 3] = 1.0f;
        }
        raw[(2 * w + 3) * 4] = raw[(2 * w + 3) * 4 + 1] = raw[(2 * w + 3) * 4 + 2] = 1.0f; // the outlier, luminance 1.0
        for (const uint32_t untraced : { 0u, 29u })
        {
            raw[untraced * 4] = raw[untraced * 4 + 1] = raw[untraced * 4 + 2] = 60000.0f;
            raw[untraced * 4 + 3] = 0.0f;
            filtered[untraced * 4] = filtered[untraced * 4 + 1] = filtered[untraced * 4 + 2] = 99.0f; // never read: untraced
            filtered[untraced * 4 + 3] = 0.0f;
        }
        const float scale = 0.7f * kGain;
        const CaptureStats stats = ComputeCaptureStats(raw, filtered, w, h, scale);
        CHECK_EQ(stats.traced, w * h - 2u);
        CHECK_EQ(stats.clamped, 1u);
        CHECK(std::fabs(stats.ClampedShare() - 1.0 / double(w * h - 2)) < 1.0e-9); // the share is over the TRACED texels, not the whole frame
        CHECK(std::fabs(stats.meanLight - 0.5 * double(scale)) < 1.0e-6);
        // The words and the line.
        const std::string words = FormatCaptureStats(stats);
        CHECK(words.find("clamped=3.57%") != std::string::npos);
        CHECK(words.find("bounce=0.11141") != std::string::npos);
        CHECK(FormatCaptureLine(stats).find("held back 3.57 %") != std::string::npos);
        CHECK(FormatCaptureLine(stats).find("28 traced texels") != std::string::npos);
        // Nothing traced, no data: zeros, never a division by zero; a null or empty frame likewise.
        float none[4 * 4] {};
        CHECK_EQ(ComputeCaptureStats(none, none, 2, 2, scale).traced, 0u);
        CHECK_EQ(ComputeCaptureStats(none, none, 2, 2, scale).ClampedShare(), 0.0);
        CHECK_EQ(ComputeCaptureStats(nullptr, filtered, w, h, scale).traced, 0u);
        CHECK_EQ(ComputeCaptureStats(raw, filtered, 0, h, scale).traced, 0u);
    }

    // ---- the compose -------------------------------------------------------------------------------------------------------------
    {
        const Params atDefault = BuildParams(4, 16, 5.0f, 0.5f, "half");
        // The share is the estimate, times the store squared, times the gain, times the strength key.
        const float share = AddedLightShare(2.0f, 0.25f, atDefault);
        CHECK(std::fabs(share - 2.0f * 0.25f * kGain * 0.5f) < 1.0e-5f);
        // A zero estimate, a zero albedo or a NaN carry nothing: the compose is not a pass the numbers book can turn into a light.
        CHECK_EQ(AddedLightShare(0.0f, 0.25f, atDefault), 0.0f);
        CHECK_EQ(AddedLightShare(2.0f, 0.0f, atDefault), 0.0f);
        CHECK_EQ(AddedLightShare(std::sqrt(-1.0f), 0.25f, atDefault), 0.0f);
        CHECK_EQ(AddedLightShare(2.0f, -0.25f, atDefault), 0.0f);
        // The strength of a NaN key reads as the default, never as "as bright as the keys say".
        CHECK_EQ(AddedLightShare(2.0f, 0.25f, BuildParams(4, 16, 5.0f, std::sqrt(-1.0f), "half")),
                 2.0f * 0.25f * kGain * kDefaultStrength);
    }

    // ---- the outcome names and the stand-down ------------------------------------------------------------------------------------
    {
        CHECK(std::string(OutcomeName(Outcome::Answered)) == "answered");
        CHECK(std::string(OutcomeName(Outcome::HoldStands)) == "declined, the chain's hold stands");
        CHECK(std::string(OutcomeName(Outcome::EngineOwn)) == "declined, the frame is the engine's own");
        CHECK(std::string(OutcomeName(Outcome::Skipped)) == "skipped");
        // The chained decline's wording names the hold; the owner's names the engine's own binding. Both are the march's own contract.
        const std::string chained = FormatDecline(Decline::Window, Outcome::HoldStands);
        const std::string owner = FormatDecline(Decline::Window, Outcome::EngineOwn);
        CHECK(chained.find("the chain's hold stands") != std::string::npos);
        CHECK(owner.find("the frame is the engine's own") != std::string::npos);
        CHECK(chained.find("empty or does not fit") != std::string::npos);
        // The stand-down's counting rule: the build's own failures count, the frame's own absences do not.
        CHECK(CountsTowardsStandDown(Decline::NoCamera));
        CHECK(CountsTowardsStandDown(Decline::RenderTarget));
        CHECK(CountsTowardsStandDown(Decline::SizeMismatch));
        CHECK(CountsTowardsStandDown(Decline::PassFailed));
        CHECK(CountsTowardsStandDown(Decline::Exception));
        CHECK(!CountsTowardsStandDown(Decline::NoGBuffer));
        CHECK(!CountsTowardsStandDown(Decline::NoDepth));
        CHECK(!CountsTowardsStandDown(Decline::NotToneMap));
        CHECK(!CountsTowardsStandDown(Decline::SceneHdr));
        CHECK(!CountsTowardsStandDown(Decline::CharaReplay));
        CHECK(!CountsTowardsStandDown(Decline::FeatureOff));
        // The card's decomposition: `skipped` and exactly one reason move together, whatever the order and however many.
        CardCounts counts;
        CountSkip(counts, SkipReason::NoGBuffer);
        CountSkip(counts, SkipReason::NoGBuffer);
        CountSkip(counts, SkipReason::NoDepth);
        CountSkip(counts, SkipReason::NoCamera);
        CHECK_EQ(counts.skipped, 4u);
        CHECK_EQ(counts.noGBuffer, 2u);
        CHECK_EQ(counts.noDepth, 1u);
        CHECK_EQ(counts.noCamera, 1u);
        // The one line a state change writes, and the card's own line.
        CHECK(FormatSkip(SkipReason::NoGBuffer).find("G-buffer") != std::string::npos);
        CHECK(FormatSkip(SkipReason::NoDepth).find("depth") != std::string::npos);
        CHECK(FormatSkip(SkipReason::NoCamera).find("camera") != std::string::npos);
        CardNumbers numbers;
        numbers.frames = 100;
        numbers.runs = 3;
        numbers.held = 1;
        numbers.declined = 2;
        numbers.skipped = 4;
        numbers.noGBuffer = 2;
        numbers.noDepth = 1;
        numbers.noCamera = 1;
        const std::string card = FormatCard(numbers);
        CHECK(card.find("100 frames") != std::string::npos);
        CHECK(card.find("3 runs") != std::string::npos);
        CHECK(card.find("4 skipped") != std::string::npos);
        // The once-per-run line carries the keys, the extent and the fade the held state drew with.
        const Params atDefault = BuildParams(4, 16, 5.0f, 0.5f, "half");
        const std::string run = FormatRun(atDefault, 1920, 1080, 960, 540, 0.85f, 0.838f);
        CHECK(run.find("960x540") != std::string::npos);
        CHECK(run.find("1920x1080") != std::string::npos);
        CHECK(run.find("4 rays") != std::string::npos);
        CHECK(run.find("16 steps") != std::string::npos);
        CHECK(run.find("0.85") != std::string::npos);
        CHECK(run.find("0.838") != std::string::npos);
        // The cost line, once the timestamp pair has landed.
        const std::string cost = FormatCost(true, 0.42, 300, 290, 10);
        CHECK(cost.find("0.4200 GPU ms") != std::string::npos);
        CHECK(cost.find("chained") != std::string::npos);
        // The view rows the camera test covers: three unit rows answer true, a half-length row answers false.
        const float good[12] { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
        CHECK(ViewRowsOk(good));
        const float broken[12] { 0.3f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
        CHECK(!ViewRowsOk(broken));
        CHECK(!ViewRowsOk(nullptr));
    }

    // ---- the stored-domain increment and the bounded add --------------------------------------------------------------------------
    {
        // Nothing to add is nothing added, and a NaN is nothing.
        CHECK_EQ(StoredIncrement(0.5f, 0.0f), 0.0f);
        CHECK_EQ(StoredIncrement(0.5f, -1.0f), 0.0f);
        CHECK_EQ(StoredIncrement(0.5f, std::nanf("")), 0.0f);
        CHECK_EQ(StoredIncrement(std::nanf(""), 1.0f), 0.0f);
        // The squared domain: stored 0.6 is linear 0.36, plus 0.28 is 0.64, stored 0.8, so the increment is 0.2.
        CHECK(std::fabs(StoredIncrement(0.6f, 0.28f) - 0.2f) < 1.0e-6f);
        // A dark pixel gains more stored value from the same linear light than a bright one (a plain addition would give both the same).
        CHECK(StoredIncrement(0.02f, 0.05f) > 4.0f * StoredIncrement(1.0f, 0.05f));
        // From black the stored value is the square root of the light.
        CHECK(std::fabs(StoredIncrement(0.0f, 0.25f) - 0.5f) < 1.0e-6f);
        CHECK(std::fabs(StoredIncrement(-1.0f, 0.25f) - 0.5f) < 1.0e-6f); // a negative scene reads as black
        // The bounded add: slope 1 at zero (a tiny increment is added whole), strictly below the white, nothing at or past the white.
        const float tiny = BoundedAdd(0.2f, 1.0e-4f, 4.0f);
        CHECK(std::fabs(tiny - StoredIncrement(0.2f, 1.0e-4f)) < 1.0e-4f * StoredIncrement(0.2f, 1.0e-4f));
        CHECK(BoundedAdd(0.2f, 1.0e6f, 4.0f) < 3.8f && 0.2f + BoundedAdd(0.2f, 1.0e6f, 4.0f) < 4.0f);
        CHECK_EQ(BoundedAdd(4.0f, 1.0f, 4.0f), 0.0f);
        CHECK_EQ(BoundedAdd(5.0f, 1.0f, 4.0f), 0.0f);
        CHECK_EQ(BoundedAdd(0.2f, 1.0f, 0.0f), 0.0f);
        CHECK(BoundedAdd(0.2f, 0.5f, 4.0f) < StoredIncrement(0.2f, 0.5f)); // the cap only ever takes away
    }

    // ---- the stage's own stand-down streak -----------------------------------------------------------------------------------------
    {
        using ext::ffxiv::engine::kConsecutiveDeclineLimit;
        using ext::ffxiv::engine::Knob;
        using ext::ffxiv::engine::Session;
        CHECK_EQ(kConsecutiveDeclineLimit, 64u);
        Session session;
        CHECK(session.Armed());
        for (uint32_t i = 1; i < kConsecutiveDeclineLimit; ++i)
            CHECK(!session.NoteDecline(Knob::Gi)); // 63 counted declines in a row: still armed
        CHECK(session.Armed());
        CHECK_EQ(session.giDeclines, kConsecutiveDeclineLimit - 1);
        session.NoteRun(Knob::Gi); // a run starts the streak again
        CHECK_EQ(session.giDeclines, 0u);
        for (uint32_t i = 1; i < kConsecutiveDeclineLimit; ++i)
            CHECK(!session.NoteDecline(Knob::Gi));
        CHECK(session.NoteDecline(Knob::Gi)); // the 64th is the one that switches the stage off
        CHECK(!session.Armed());
        CHECK(!session.NoteDecline(Knob::Gi)); // and it reports that once
        // The streak is the stage's own: the other knobs' declines never count towards it, and its declines never count towards theirs.
        Session other;
        for (uint32_t i = 0; i < kConsecutiveDeclineLimit - 1; ++i)
        {
            CHECK(!other.NoteDecline(Knob::Godrays));
            CHECK(!other.NoteDecline(Knob::LightShafts));
        }
        CHECK_EQ(other.giDeclines, 0u);
        CHECK_EQ(other.godraysDeclines, kConsecutiveDeclineLimit - 1);
        CHECK(!other.NoteDecline(Knob::Gi));
        CHECK_EQ(other.giDeclines, 1u);
        CHECK_EQ(other.godraysDeclines, kConsecutiveDeclineLimit - 1);
    }

    // ---- METER sweep S1: a refused camera read with a good camera in hand stands on it ---------------------------------------------------------------------
    {
        // nothing in hand: the refusal is a counted decline
        CHECK(!RefusedCameraUsesHold(false, 0, 1.0f));
        // a young, unfaded hold: the frame stands on it, at every age up to the hold's length
        for (uint32_t age = 0; age <= ext::ffxiv::shafts::kHoldFrames; ++age)
            CHECK(RefusedCameraUsesHold(true, age, 1.0f));
        // too old: one frame past the hold's length is a decline
        CHECK(!RefusedCameraUsesHold(true, ext::ffxiv::shafts::kHoldFrames + 1, 1.0f));
        CHECK(!RefusedCameraUsesHold(true, 100000u, 1.0f));
        // faded out: a hold with no weight left is nothing to stand on
        CHECK(!RefusedCameraUsesHold(true, 2, 0.0f));
        CHECK(!RefusedCameraUsesHold(true, 2, -0.5f));
        CHECK(RefusedCameraUsesHold(true, 2, 0.01f));
        // a NaN weight is nothing (the comparison is false)
        CHECK(!RefusedCameraUsesHold(true, 2, std::nanf("")));
        // the two lines say different things: the held one names the fallback, the plain one the refusal
        CHECK(std::string(kCameraRefusedHeld).find("stands") != std::string::npos);
        CHECK(std::string(kCameraRefused).find("stands") == std::string::npos);
    }

    return Finish("gi_rules");
}
