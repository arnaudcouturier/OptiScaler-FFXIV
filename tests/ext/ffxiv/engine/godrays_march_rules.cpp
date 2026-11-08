// God rays, the beams that follow the engine's own shadows: the pure rules of the second march (ext/ffxiv/engine/GodraysRules.h), on the host.
//
// What this file pins, and what a wrong rule would break:
//   the Henyey-Greenstein lobe is 1 at the light and 0.1 at 24.6 degrees for g 0.8 - a pow(cos, 16) lobe (the first march's) removed 92 % of the
//     term, a lobe that is not normalised to its peak changes the whole effect's strength;
//   the in-scatter integral accrues optical depth also through shadowed air, and a ray that stays lit saturates at the phase value 1 - exp(-sigma L);
//   a cascade's atlas slice is read from its own matrix and fade row, on all sixteen matrices of the four rc6 frames, whatever number of cascades
//     landed - a first-cascade-wins pick (or a slice from the cascade's rank) hides the beams the farther cascades cast;
//   the sky ceiling is the sampled 99th percentile of the scene's own sky, interpolated inside its bin, and a frame without sky samples leaves the sky alone;
//   the light gates: the shadow light's axis is the cascades' axis to 0.05 degrees on the four frames (3 degrees is the limit), the luminance gain is
//     0 at 0.05 and 1 at 0.6, the nearest pixel ray to the light is 8.3 / 65.7 / 0 / 117.5 degrees on the four frames (the RE report's numbers), and a
//     light beyond 72.5 degrees of every pixel ray (cosine under 0.3) casts no beams;
//   the stage values string carries the keys and the last completed run's measured numbers (the window opens before the chain runs).
#include "check.h"
#include "godrays_capture_rows.h"

#include "ext/ffxiv/engine/GodraysRules.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace ext::ffxiv::godrays;
using ext::ffxiv::godrays::Cascade;
using ext::ffxiv::godrays::Params;

namespace
{
constexpr float kDegrees = 57.29577951308232f;
float QuietNan() { return std::numeric_limits<float>::quiet_NaN(); }
bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

struct CaptureRows
{
    const float (*cascades)[32];
    const float* inverseProjection;
    const float* view;
    const float* lightDirection;
    const float* lightDiffuse;
    const float* fogSun;
};

CaptureRows Capture(int n)
{
    using namespace capture_rows;
    switch (n)
    {
    case 1: return { kC1Cascades, kC1InverseProjection, kC1View, kC1LightDirection, kC1LightDiffuse, kC1FogSun };
    case 2: return { kC2Cascades, kC2InverseProjection, kC2View, kC2LightDirection, kC2LightDiffuse, kC2FogSun };
    case 3: return { kC3Cascades, kC3InverseProjection, kC3View, kC3LightDirection, kC3LightDiffuse, kC3FogSun };
    default: return { kC4Cascades, kC4InverseProjection, kC4View, kC4LightDirection, kC4LightDiffuse, kC4FogSun };
    }
}

// A cascade as the draw path decodes a 128-byte instance (CascadeReader::Read): the matrix, the fade row, the distance row's far, the ramp.
Cascade CascadeOf(const float row[32])
{
    Cascade c;
    std::memcpy(c.matrix, row, sizeof c.matrix);
    std::memcpy(c.fade, row + 24, sizeof c.fade);
    c.distanceFar = row[18];
    c.rampScale = row[22];
    c.rampTarget = row[23];
    c.mapRowX = row[28];
    c.shaped = ext::ffxiv::shafts::ShadowMatrixShape(row);
    return c;
}

float ElevationDegrees(const CaptureRows& c)
{
    return std::asin(ext::ffxiv::shafts::WorldElevationOf(c.view, c.lightDirection)) * kDegrees;
}
} // namespace

int main()
{
    // ---- the keys: the new ones, the new defaults, and the first march's calls still compile -----------------------------------------
    {
        CHECK_EQ(kDefaultSteps, 48);
        CHECK_EQ(kDefaultDensity, 20.0f);
        CHECK_EQ(kDefaultAnisotropy, 0.8f);
        CHECK_EQ(kMarchGain, 0.70f);
        CHECK_EQ(kSigmaPerMetre, 0.001f);
        CHECK_EQ(kAirBias, 0.002f);
        CHECK_EQ(ClampDensity(QuietNan()), kDefaultDensity);
        CHECK_EQ(ClampDensity(-3.0f), kMinDensity);
        CHECK_EQ(ClampDensity(1.0e9f), kMaxDensity);
        CHECK_EQ(ClampDensity(35.0f), 35.0f);
        CHECK_EQ(ClampAnisotropy(QuietNan()), kDefaultAnisotropy);
        CHECK_EQ(ClampAnisotropy(-0.5f), kMinAnisotropy);
        CHECK_EQ(ClampAnisotropy(3.0f), kMaxAnisotropy);
        CHECK(kMaxAnisotropy < 1.0f); // g = 1 is a delta lobe: the normalisation divides by zero
        const Params defaults {};
        CHECK_EQ(defaults.steps, 48u);
        CHECK_EQ(defaults.density, 20.0f);
        CHECK_EQ(defaults.anisotropy, 0.8f);
        // The first march's nine-argument call still builds (the lab driver and the old tests use it) and takes the new keys' defaults.
        const Params old = BuildParams(32, "quarter", 1.0f, 1.0f, 1.0f, "auto", 0, true, 1.0f);
        CHECK_EQ(old.density, kDefaultDensity);
        CHECK_EQ(old.anisotropy, kDefaultAnisotropy);
        const Params keyed = BuildParams(48, "quarter", 1.0f, 1.0f, 1.0f, "auto", 0, true, 1.0f, 60.0f, 0.5f);
        CHECK_EQ(keyed.density, 60.0f);
        CHECK_EQ(keyed.anisotropy, 0.5f);
        const Params broken = BuildParams(48, "quarter", 1.0f, 1.0f, 1.0f, "auto", 0, true, 1.0f, QuietNan(), QuietNan());
        CHECK_EQ(broken.density, kDefaultDensity);
        CHECK_EQ(broken.anisotropy, kDefaultAnisotropy);
    }

    // ---- the phase: Henyey-Greenstein normalised to 1 at the light -----------------------------------------------------------------
    {
        for (const float g : { 0.0f, 0.3f, 0.65f, 0.8f, 0.95f })
            CHECK(Near(HenyeyGreenstein(1.0f, g), 1.0f, 1.0e-5f));
        for (const float c : { -1.0f, -0.4f, 0.0f, 0.7f, 1.0f })
            CHECK(Near(HenyeyGreenstein(c, 0.0f), 1.0f, 1.0e-5f)); // g 0 is isotropic
        // The RE report's number: 10 % of the peak at 24.6 degrees for g 0.8.
        CHECK(Near(HenyeyGreenstein(std::cos(24.6f / kDegrees), 0.8f), 0.1f, 0.006f));
        CHECK(Near(LobeAngleDegrees(0.8f, 0.1f), 24.6f, 0.3f));
        // The two far frames: capture 2's nearest ray (65.7 degrees) keeps 0.8 % and capture 4's (117.5) keeps 0.2 %.
        CHECK(Near(HenyeyGreenstein(0.4116f, 0.8f), 0.0082f, 4.0e-4f));
        CHECK(Near(HenyeyGreenstein(-0.4622f, 0.8f), 0.0022f, 2.0e-4f));
        // Monotone away from the light.
        float last = 2.0f;
        for (int degrees = 0; degrees <= 180; degrees += 5)
        {
            const float value = HenyeyGreenstein(std::cos(float(degrees) / kDegrees), 0.8f);
            CHECK(value < last);
            last = value;
        }
        // A rounder lobe (smaller g) is wider: at 30 degrees g 0.5 keeps more than g 0.8.
        CHECK(HenyeyGreenstein(std::cos(30.0f / kDegrees), 0.5f) > HenyeyGreenstein(std::cos(30.0f / kDegrees), 0.8f));
        // GodraysPhaseBoost keeps its meaning (1 neutral, higher concentrates, lower spreads) by scaling the lobe's narrowness 1 - g.
        CHECK(Near(EffectiveAnisotropy(0.8f, 1.0f), 0.8f, 1.0e-6f));
        CHECK(Near(EffectiveAnisotropy(0.8f, 2.0f), 0.9f, 1.0e-6f));
        CHECK(Near(EffectiveAnisotropy(0.8f, 0.5f), 0.6f, 1.0e-6f));
        CHECK_EQ(EffectiveAnisotropy(0.8f, 0.0f), 0.0f);          // a boost of zero spreads the lobe fully: isotropic
        CHECK_EQ(EffectiveAnisotropy(0.8f, 100.0f), kMaxAnisotropy); // never past the normalisation's limit
        CHECK(Near(EffectiveAnisotropy(0.8f, QuietNan()), 0.8f, 1.0e-6f)); // a broken key reads as neutral
    }

    // ---- the in-scatter integral: S = sum sigma ph V T dt, optical depth accruing also through shadow ------------------------------
    {
        constexpr int n = 48;
        const float length = 100.0f;
        const float dt = length / float(n);
        const float sigma = 0.02f;
        const float phase = 0.37f;
        std::vector<float> sigmas(n, sigma), lit(n, 1.0f), dark(n, 0.0f), middleDark(n, 1.0f);
        for (int i = 16; i < 32; ++i)
            middleDark[size_t(i)] = 0.0f;
        // Uniform medium, never shadowed: the discrete geometric sum, q = exp(-sigma dt), S = phase sigma dt (1 - q^n) / (1 - q).
        const float q = std::exp(-sigma * dt);
        const float closed = phase * sigma * dt * (1.0f - std::pow(q, float(n))) / (1.0f - q);
        CHECK(Near(InscatterIntegral(sigmas.data(), lit.data(), n, dt, phase, true), closed, 1.0e-5f));
        // The integral saturates: a longer ray adds less than a shorter one did (the phase value is the ceiling, up to the left-Riemann sum's
        // overshoot of sigma dt / (1 - exp(-sigma dt)) = 1.02 at this step).
        CHECK(closed < phase);
        std::vector<float> longer(2 * n, sigma), litLonger(2 * n, 1.0f);
        const float twice = InscatterIntegral(longer.data(), litLonger.data(), 2 * n, dt, phase, true);
        CHECK(twice > closed && twice < 1.03f * phase && twice - closed < closed);
        // Without extinction (the first march's mean) the same ray is phase sigma L: a different number, and the mutant this separates.
        CHECK(Near(InscatterIntegral(sigmas.data(), lit.data(), n, dt, phase, false), phase * sigma * length, 1.0e-5f));
        CHECK(InscatterIntegral(sigmas.data(), lit.data(), n, dt, phase, false) > closed * 1.2f);
        // Shadow: nothing lit, nothing added; the shadowed middle third removes its own samples and still extinguishes what follows.
        CHECK_EQ(InscatterIntegral(sigmas.data(), dark.data(), n, dt, phase, true), 0.0f);
        float expected = 0.0f;
        for (int i = 0; i < n; ++i)
            if (i < 16 || i >= 32)
                expected += phase * sigma * dt * std::pow(q, float(i));
        CHECK(Near(InscatterIntegral(sigmas.data(), middleDark.data(), n, dt, phase, true), expected, 1.0e-5f));
        // The step function is the integral's own body (the shader and the WARP reference run the same expression).
        InscatterState state;
        for (int i = 0; i < n; ++i)
            InscatterStep(state, sigma, phase, 1.0f, dt, true);
        CHECK(Near(state.sum, closed, 1.0e-5f));
        CHECK(Near(state.tau, sigma * length, 1.0e-4f));
        // The medium: density 20 at the camera's own height is 0.02 per metre.
        CHECK(Near(MediumSigma(20.0f, 1.0f), 0.02f, 1.0e-7f));
        CHECK(Near(MediumSigma(20.0f, 0.5f), 0.01f, 1.0e-7f));
    }

    // ---- the atlas slice of a cascade, from its own matrix and fade row: the sixteen matrices of the four frames --------------------
    {
        for (int n = 1; n <= 4; ++n)
        {
            const CaptureRows c = Capture(n);
            for (uint32_t k = 0; k < 4; ++k)
            {
                const float* row = c.cascades[k];
                CHECK_EQ(CascadeSlice(row, row + 24), k);
                // The margin to the slice's edge: the band's middle is well inside its slice (>= 0.05 of the slice's 0.25).
                const float v = CascadeBandMidV(row, row + 24);
                CHECK(v - 0.25f * float(k) > 0.05f && 0.25f * float(k + 1) - v > 0.05f);
            }
        }
        // A set of three: every remaining cascade keeps its slice (nothing is read from the rank or the count).
        const CaptureRows c1 = Capture(1);
        const uint32_t kept[3] { 0, 2, 3 };
        for (const uint32_t k : kept)
            CHECK_EQ(CascadeSlice(c1.cascades[k], c1.cascades[k] + 24), k);
        // Unreadable rows have no slice: a zero fade scale, a NaN matrix, a v that leaves the atlas.
        float fade[4] { -2.9f, 12.0f, 0.0f, 0.0f };
        CHECK_EQ(CascadeSlice(c1.cascades[0], fade), kNoSlice);
        float matrix[16] {};
        std::memcpy(matrix, c1.cascades[1], sizeof matrix);
        matrix[5] = QuietNan();
        CHECK_EQ(CascadeSlice(matrix, c1.cascades[1] + 24), kNoSlice);
        std::memcpy(matrix, c1.cascades[1], sizeof matrix);
        matrix[7] += 5.0f; // v + 5: the band's middle projects far outside the atlas
        CHECK_EQ(CascadeSlice(matrix, c1.cascades[1] + 24), kNoSlice);
        // The slice window: [k/4, (k+1)/4), nothing for no slice.
        CHECK(SliceContains(0.249f, 0) && !SliceContains(0.25f, 0) && SliceContains(0.25f, 1));
        CHECK(SliceContains(0.9999f, 3) && !SliceContains(1.0f, 3) && !SliceContains(-0.001f, 0));
        CHECK(!SliceContains(0.5f, kNoSlice) && !SliceContains(0.5f, 4));
        CHECK(!SliceContains(QuietNan(), 0));
    }

    // ---- the sky ceiling: the interpolated 99th percentile of a 128-bin histogram, 0.98 of it ---------------------------------------
    {
        const float range = 0.838f;
        const float binWidth = range / float(kSkyBins);
        CHECK_EQ(SkyBinOf(0.0f, range, kSkyBins), 0u);
        CHECK_EQ(SkyBinOf(range * 2.0f, range, kSkyBins), kSkyBins - 1); // clamped into the last bin
        CHECK_EQ(SkyBinOf(-1.0f, range, kSkyBins), 0u);
        CHECK_EQ(SkyBinOf(QuietNan(), range, kSkyBins), 0u);
        CHECK_EQ(SkyBinOf(binWidth * 10.5f, range, kSkyBins), 10u);
        // Ten thousand samples spread evenly over [0.3, 0.8]: the exact 99th percentile is 0.795.
        std::vector<uint32_t> bins(kSkyBins, 0);
        uint32_t samples = 0;
        for (int i = 0; i < 10000; ++i)
        {
            const float value = 0.3f + 0.5f * (float(i) + 0.5f) / 10000.0f;
            ++bins[SkyBinOf(value, range, kSkyBins)];
            ++samples;
        }
        const float p99 = HistogramPercentile(bins.data(), kSkyBins, range, 0.99f);
        CHECK(Near(p99, 0.795f, 0.5f * binWidth)); // interpolated inside the bin: within half a bin of the exact value
        CHECK(Near(SkyCeilingOf(bins.data(), kSkyBins, range, samples), 0.98f * p99, 1.0e-6f));
        // Fewer than kMinSkySamples samples: no ceiling (0), so the sky is left alone.
        CHECK_EQ(SkyCeilingOf(bins.data(), kSkyBins, range, kMinSkySamples - 1), 0.0f);
        CHECK(SkyCeilingOf(bins.data(), kSkyBins, range, kMinSkySamples) > 0.0f);
        // All samples in one bin: the percentile stays inside that bin.
        std::vector<uint32_t> one(kSkyBins, 0);
        one[80] = 500;
        const float inBin = HistogramPercentile(one.data(), kSkyBins, range, 0.99f);
        CHECK(inBin >= binWidth * 80.0f && inBin <= binWidth * 81.0f);
        // The clamp: add never takes a channel past the ceiling, and a channel already past it gets nothing.
        CHECK(Near(SkyLimited(0.5f, 0.30f, 0.70f), 0.40f, 1.0e-6f));
        CHECK_EQ(SkyLimited(0.5f, 0.75f, 0.70f), 0.0f);
        CHECK(Near(SkyLimited(0.05f, 0.30f, 0.70f), 0.05f, 1.0e-6f));
        CHECK_EQ(SkyLimited(0.5f, 0.30f, 0.0f), 0.0f);
        // Property: whatever the add, scene + SkyLimited never exceeds the ceiling, and the ceiling stays under the exact 99th percentile
        // of a sky whose values sit in the upper part of the range (the capture's skies are 0.7-0.8 of the white).
        uint32_t seed = 12345u;
        for (int trial = 0; trial < 200; ++trial)
        {
            std::vector<uint32_t> hist(kSkyBins, 0);
            std::vector<float> values;
            const float centre = 0.55f + 0.2f * float(trial % 10) / 10.0f;
            for (int i = 0; i < 4000; ++i)
            {
                seed = seed * 1664525u + 1013904223u;
                const float value = centre + (float((seed >> 8) & 0xFFFF) / 65536.0f - 0.5f) * 0.12f;
                values.push_back(value);
                ++hist[SkyBinOf(value, range, kSkyBins)];
            }
            std::vector<float> sorted = values;
            std::sort(sorted.begin(), sorted.end());
            const float exact99 = sorted[size_t(0.99 * double(sorted.size()))];
            const float ceiling = SkyCeilingOf(hist.data(), kSkyBins, range, 4000);
            CHECK(ceiling <= exact99);
            for (size_t i = 0; i < values.size(); i += 97)
                CHECK(values[i] + SkyLimited(10.0f, values[i], ceiling) <= std::max(values[i], ceiling) + 1.0e-6f);
        }
    }

    // ---- the light class: luminance gain and the nearest pixel ray ----------------------------------------------------------------------
    {
        CHECK_EQ(LightLuminanceGain(0.0f), 0.0f);
        CHECK_EQ(LightLuminanceGain(0.05f), 0.0f);
        CHECK_EQ(LightLuminanceGain(0.6f), 1.0f);
        CHECK_EQ(LightLuminanceGain(5.0f), 1.0f);
        CHECK_EQ(LightLuminanceGain(QuietNan()), 0.0f);
        CHECK(Near(LightLuminanceGain(0.325f), 0.5f, 1.0e-4f));
        float last = -1.0f;
        for (int i = 0; i <= 80; ++i)
        {
            const float gain = LightLuminanceGain(0.01f * float(i));
            CHECK(gain >= last);
            last = gain;
        }
        // The four frames: the sun at 1.57 runs at full strength, the dim blue lights at 0.26 / 0.11 / 0.11 do not.
        CHECK_EQ(LightLuminanceGain(ext::ffxiv::shafts::Luminance(Capture(1).lightDiffuse)), 1.0f);
        CHECK(Near(LightLuminanceGain(ext::ffxiv::shafts::Luminance(Capture(2).lightDiffuse)), 0.33f, 0.03f));
        CHECK(LightLuminanceGain(ext::ffxiv::shafts::Luminance(Capture(3).lightDiffuse)) < 0.05f);
        CHECK(LightLuminanceGain(ext::ffxiv::shafts::Luminance(Capture(4).lightDiffuse)) < 0.05f);

        // The nearest pixel ray, the RE report's numbers: capture 1 8.3 degrees (the light is above the frame), capture 2 65.7, capture 3
        // inside the frame (0), capture 4 117.5 (behind the camera).
        const float expectedDegrees[4] { 8.3f, 65.7f, 0.0f, 117.5f };
        for (int n = 1; n <= 4; ++n)
        {
            const CaptureRows c = Capture(n);
            const float cosine = NearestRayCosine(c.inverseProjection, c.lightDirection);
            CHECK(Near(std::acos(std::fmax(-1.0f, std::fmin(1.0f, cosine))) * kDegrees, expectedDegrees[n - 1], 0.5f));
        }
        // The gate: cosine under 0.3 (72.5 degrees). Capture 2 at 65.7 degrees is NOT gated (it runs at the luminance gain's 0.33); capture 4 is.
        CHECK(!LightAside(NearestRayCosine(Capture(1).inverseProjection, Capture(1).lightDirection)));
        CHECK(!LightAside(NearestRayCosine(Capture(2).inverseProjection, Capture(2).lightDirection)));
        CHECK(!LightAside(NearestRayCosine(Capture(3).inverseProjection, Capture(3).lightDirection)));
        CHECK(LightAside(NearestRayCosine(Capture(4).inverseProjection, Capture(4).lightDirection)));
        CHECK(LightAside(QuietNan())); // an unreadable cosine is gated, not run
        // Two synthetic lights either side of the threshold, in the horizontal plane beyond the frame's right edge (the edge ray is the
        // nearest): 80 degrees away (cosine 0.17, gated) and 70 degrees away (cosine 0.34, runs). A threshold of 0 would run both.
        const CaptureRows c4 = Capture(4);
        const float edge = std::atan(c4.inverseProjection[0]); // the right edge ray's angle from the view axis
        for (const float away : { 80.0f, 70.0f })
        {
            const float theta = edge + away / kDegrees;
            const float light[3] { std::sin(theta), 0.0f, -std::cos(theta) };
            const float cosine = NearestRayCosine(c4.inverseProjection, light);
            CHECK(Near(std::acos(cosine) * kDegrees, away, 0.5f));
            CHECK(LightAside(cosine) == (away > 72.5f));
        }
    }

    // ---- the cascade axis: the shadow light is -normalize(row 2), on all sixteen matrices -------------------------------------------------
    {
        for (int n = 1; n <= 4; ++n)
        {
            const CaptureRows c = Capture(n);
            for (uint32_t k = 0; k < 4; ++k)
            {
                CHECK(AxisAgreementDegrees(c.cascades[k], c.lightDirection) <= 0.05f);
                float axis[3] {};
                CHECK(CascadeAxisOf(c.cascades[k], axis));
                CHECK(Near(axis[0], c.lightDirection[0], 1.0e-3f) && Near(axis[1], c.lightDirection[1], 1.0e-3f) &&
                      Near(axis[2], c.lightDirection[2], 1.0e-3f));
            }
        }
        // A light yawed by 2 degrees still agrees (3 is the limit), by 5 it does not, and the sign matters (the unnegated row is 180 away).
        const CaptureRows c = Capture(1);
        for (const float yaw : { 2.0f, 5.0f })
        {
            const float a = yaw / kDegrees;
            const float* d = c.lightDirection;
            const float yawed[3] { d[0] * std::cos(a) + d[2] * std::sin(a), d[1], -d[0] * std::sin(a) + d[2] * std::cos(a) };
            CHECK(AxisAgrees(c.cascades[0], yawed) == (yaw < kAxisAgreementDegrees));
        }
        const float opposite[3] { -c.lightDirection[0], -c.lightDirection[1], -c.lightDirection[2] };
        CHECK(!AxisAgrees(c.cascades[0], opposite));
        CHECK(!AxisAgrees(nullptr, c.lightDirection));
        float zero[16] {};
        CHECK(!AxisAgrees(zero, c.lightDirection));
        // The finest cascade comes first whatever order the copies landed in (CascadeOrder), and its axis is the march's direction.
        Cascade shuffled[kCascadeCount] { CascadeOf(c.cascades[2]), CascadeOf(c.cascades[0]), CascadeOf(c.cascades[3]), CascadeOf(c.cascades[1]) };
        uint32_t order[kCascadeCount] {};
        CHECK_EQ(CascadeOrder(shuffled, order, kCascadeCount), 4u);
        CHECK(std::memcmp(shuffled[order[0]].matrix, c.cascades[0], sizeof(float) * 16) == 0);
    }

    // ---- the verdict: one function from the light, the camera and the finest cascade ---------------------------------------------------
    {
        const float elevation[4] { 25.9f, 41.2f, 15.4f, 11.1f }; // the RE report's world elevations of the four lights
        const LightGate expected[4] { LightGate::Run, LightGate::Run, LightGate::Run, LightGate::Aside };
        for (int n = 1; n <= 4; ++n)
        {
            const CaptureRows c = Capture(n);
            CHECK(Near(ElevationDegrees(c), elevation[n - 1], 0.3f));
            const LightJudgement j = JudgeLight(c.lightDirection, c.lightDiffuse, std::sin(ElevationDegrees(c) / kDegrees), c.inverseProjection,
                                                c.cascades[0]);
            CHECK(j.gate == expected[n - 1]);
            CHECK(j.axisDegrees >= 0.0f && j.axisDegrees <= 0.05f);
            CHECK(Near(j.elevationDegrees, elevation[n - 1], 0.3f));
            CHECK(Near(j.luminance, ext::ffxiv::shafts::Luminance(c.lightDiffuse), 1.0e-5f));
            CHECK(Near(j.gain, LightLuminanceGain(j.luminance), 1.0e-6f));
        }
        // Capture 1 runs at full strength, capture 2 runs at its low gain (65.7 degrees is inside the 72.5 gate), capture 4 is gated.
        const CaptureRows c1 = Capture(1), c2 = Capture(2), c4 = Capture(4);
        CHECK_EQ(JudgeLight(c1.lightDirection, c1.lightDiffuse, 0.44f, c1.inverseProjection, c1.cascades[0]).gain, 1.0f);
        const LightJudgement low = JudgeLight(c2.lightDirection, c2.lightDiffuse, 0.66f, c2.inverseProjection, c2.cascades[0]);
        CHECK(low.gate == LightGate::Run && low.gain > 0.25f && low.gain < 0.4f);
        // An axis that disagrees refuses the light before anything else: capture 1's light against capture 4's cascades.
        CHECK(JudgeLight(c1.lightDirection, c1.lightDiffuse, 0.44f, c1.inverseProjection, c4.cascades[0]).gate == LightGate::AxisMismatch);
        // No cascade to judge against.
        CHECK(JudgeLight(c1.lightDirection, c1.lightDiffuse, 0.44f, c1.inverseProjection, nullptr).gate == LightGate::NoCascade);
        // The names and the once-per-change lines: each gate has its own wording, and the key moves with the gate.
        const LightJudgement run = JudgeLight(c1.lightDirection, c1.lightDiffuse, 0.44f, c1.inverseProjection, c1.cascades[0]);
        const LightJudgement aside = JudgeLight(c4.lightDirection, c4.lightDiffuse, 0.19f, c4.inverseProjection, c4.cascades[0]);
        CHECK(LightGateKey(run) != LightGateKey(aside));
        const std::string runLine = FormatLightJudgement(run);
        CHECK(runLine.find("luminance 1.572") != std::string::npos && runLine.find("elevation") != std::string::npos &&
              runLine.find("8.") != std::string::npos);
        const std::string asideLine = FormatLightJudgement(aside);
        CHECK(asideLine.find("behind or beside") != std::string::npos && asideLine.find("117.") != std::string::npos);
        const std::string mismatch = FormatLightJudgement(JudgeLight(c1.lightDirection, c1.lightDiffuse, 0.44f, c1.inverseProjection, c4.cascades[0]));
        CHECK(mismatch.find("shadow cascades") != std::string::npos);
        CHECK(std::string(LightGateName(LightGate::Run)) == "run" && std::string(LightGateName(LightGate::Aside)) == "aside");
    }

    // ---- the fog sun: cFogParam[2].xyz read from the 128-byte copy, the profile not needed ------------------------------------------------
    {
        const float expectedY[4] { 0.183f, -0.1086f, -0.0582f, -0.0119f };
        for (int n = 1; n <= 4; ++n)
        {
            const CaptureRows c = Capture(n);
            // A 128-byte cFogParam with only the sun row filled: row 2 (+0x20) holds the sun, the rest is not read by this rule.
            unsigned char bytes[kFogParamBytes] {};
            std::memcpy(bytes + kFogSunDirAt, c.fogSun, sizeof(float) * 3);
            float sun[3] {};
            CHECK(FogSunOf(bytes, sun));
            CHECK(Near(sun[1], expectedY[n - 1], 0.001f));
            CHECK(Near(std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]), 1.0f, 0.02f));
        }
        unsigned char bad[kFogParamBytes] {};
        float sun[3] {};
        CHECK(!FogSunOf(nullptr, sun));
        CHECK(!FogSunOf(bad, sun)); // the zero vector is not a direction
        const float nanSun[3] { 0.0f, QuietNan(), 1.0f };
        std::memcpy(bad + kFogSunDirAt, nanSun, sizeof nanSun);
        CHECK(!FogSunOf(bad, sun));
        const float shortSun[3] { 0.0f, 0.3f, 0.3f };
        std::memcpy(bad + kFogSunDirAt, shortSun, sizeof shortSun);
        CHECK(!FogSunOf(bad, sun)); // not a unit vector: not the engine's sun
    }

    // ---- the stage values string: the keys and the last completed run's numbers -----------------------------------------------------------
    {
        Params params {};
        StageMeasure measured;
        measured.have = true;
        measured.termPeak = 0.1234f;
        measured.whitePoint = 0.838f;
        measured.fade = 1.0f;
        measured.sunScreen[0] = 1032.5f;
        measured.sunScreen[1] = -237.1f;
        measured.tint[0] = 1.0f;
        measured.tint[1] = 0.76f;
        measured.tint[2] = 0.64f;
        measured.luminance = 1.572f;
        measured.elevationDegrees = 25.9f;
        measured.nearestDegrees = 8.3f;
        measured.gate = LightGate::Run;
        // The frame's white, fade, sun pixel and tint are NOT in this snapshot (2026-11-07 integration): they reach the line once, as this frame's own,
        // through the late hook (StageNoteFrame after the run); this string is read when the capture window opens, one run old.
        CHECK(FormatStageValues(params, measured) ==
              "steps=48 intensity=1.00 fogBoost=1.00 heightLift=1.00 softness=0 jitter=1 phaseBoost=1.00 density=20.00 anisotropy=0.80 "
              "termPeak=0.123 lightLum=1.572 elev=25.9 angle=8.3 gate=run");
        StageMeasure none;
        const std::string before = FormatStageValues(params, none);
        CHECK(before.find("density=20.00") != std::string::npos && before.find("measured=0") != std::string::npos &&
              before.find("termPeak") == std::string::npos);
        // The run line carries the measured peak (the first march passed a literal 0 here).
        const float direction[3] { 0.0f, 1.0f, 0.0f };
        const std::string run = FormatRun(params, 1920, 1080, 480, 270, direction, 1.0f, 0.838f, 0.75f);
        CHECK(run.find("term peak 0.750") != std::string::npos);
        CHECK(run.find("48 steps") != std::string::npos && run.find("density 20.00") != std::string::npos &&
              run.find("anisotropy 0.80") != std::string::npos);
    }

    // ---- the pair of in-scatter integrals (Brief R4): I_1 is the same loop with the visibility fixed at 1, one step weight for both ---------------
    {
        constexpr int n = 48;
        const float dt = 100.0f / float(n);
        const float sigma = 0.02f;
        const float phase = 0.37f;
        std::vector<float> sigmas(n, sigma), lit(n, 1.0f), dark(n, 0.0f), middleDark(n, 1.0f), half(n, 0.5f);
        for (int i = 16; i < 32; ++i)
            middleDark[size_t(i)] = 0.0f;
        const auto run = [&](const std::vector<float>& visibility, bool extinction) {
            InscatterPairState state;
            for (int i = 0; i < n; ++i)
                InscatterPairStep(state, sigma, phase, visibility[size_t(i)], dt, extinction);
            return state;
        };
        const InscatterPairState allLit = run(lit, true);
        // Lit air: the two channels are bit-identical (the no-shadow control's modulation is exactly 1), and I_1 is the closed form of the v2 integral.
        CHECK(allLit.shadowed == allLit.unshadowed);
        CHECK(allLit.unshadowed > 0.0f);
        CHECK(Near(allLit.unshadowed, InscatterIntegral(sigmas.data(), lit.data(), n, dt, phase, true), 1.0e-6f));
        // Shadow takes the first channel's samples and none of the second's, and the extinction accrues through shadow in both.
        const InscatterPairState middle = run(middleDark, true);
        CHECK(middle.unshadowed == allLit.unshadowed);                      // I_1 does not see the atlas
        CHECK(middle.shadowed < middle.unshadowed - 1.0e-4f);
        CHECK(Near(middle.shadowed, InscatterIntegral(sigmas.data(), middleDark.data(), n, dt, phase, true), 1.0e-6f)); // I_V is v2's term
        const InscatterPairState black = run(dark, true);
        CHECK(black.shadowed == 0.0f && black.unshadowed == allLit.unshadowed);
        CHECK(Near(black.tau, allLit.tau, 1.0e-7f) && Near(allLit.tau, sigma * 100.0f, 1.0e-4f));
        const InscatterPairState grey = run(half, true);
        CHECK(Near(grey.shadowed, 0.5f * grey.unshadowed, 1.0e-6f));        // a visibility of one half halves I_V and leaves I_1
        // Without extinction both channels lose the transmittance (the first march's mean): I_1 grows, and stays a channel of its own.
        const InscatterPairState noExt = run(middleDark, false);
        CHECK(noExt.unshadowed > allLit.unshadowed * 1.2f && Near(noExt.unshadowed, phase * sigma * 100.0f, 1.0e-5f));
        CHECK(noExt.shadowed < noExt.unshadowed);
    }

    // ---- the shadow modulation m = I_V / I_1 (Brief R4): guarded, clamped, never inverted --------------------------------------------------------
    {
        CHECK(kModulationBoost > 0.0f && kModulationEpsilon > 0.0f && kModulationEpsilon < 1.0e-3f);
        CHECK_EQ(ShadowModulation(0.5f, 0.5f), 1.0f);                       // unshadowed air: exactly 1
        CHECK_EQ(ShadowModulation(0.0037f, 0.0037f), 1.0f);
        CHECK(Near(ShadowModulation(0.25f, 1.0f), 0.25f, 1.0e-7f));         // not 4 (inverted), not 1
        CHECK(Near(ShadowModulation(0.3f, 0.6f), 0.5f, 1.0e-7f));
        CHECK_EQ(ShadowModulation(0.0f, 1.0f), 0.0f);                       // fully shadowed: 0
        CHECK_EQ(ShadowModulation(3.0f, 1.0f), 1.0f);                       // more shadowed light than unshadowed light is rounding noise: clamped to 1
        CHECK_EQ(ShadowModulation(-0.5f, 1.0f), 0.0f);                      // and a negative one to 0
        CHECK_EQ(ShadowModulation(0.0f, 0.0f), 1.0f);                       // I_1 carries nothing: 1, never a NaN from 0 / 0
        CHECK_EQ(ShadowModulation(0.00005f, kModulationEpsilon * 0.5f), 1.0f);
        CHECK_EQ(ShadowModulation(0.5f, -1.0f), 1.0f);
        CHECK_EQ(ShadowModulation(QuietNan(), 1.0f), 1.0f);                 // a broken channel is no modulation
        CHECK_EQ(ShadowModulation(0.5f, QuietNan()), 1.0f);
        CHECK(Near(ShadowModulation(0.5f * 2.0e-4f, 2.0e-4f), 0.5f, 1.0e-6f)); // just above the guard, the ratio stands
    }

    // ---- the added term I_1 * max(m - level, 0) * boost (Brief R4) -------------------------------------------------------------------------------
    {
        // The no-shadow control: m = 1 and so is the level, whatever I_1 and whatever the grid's sums are: exactly nothing is added.
        for (const float v : { 1.0e-3f, 0.02f, 0.37f, 1.0f, 3.9f, 4.0f })
        {
            CHECK_EQ(ModulatedTerm(v, v, v * 81.0f, v * 81.0f), 0.0f);
            CHECK_EQ(ModulatedTerm(v, v, 0.0f, 0.0f), 0.0f);                // a level that carries nothing is 1
        }
        const float i1 = 0.2f;
        // A lit pixel in a half-shadowed neighbourhood gets the lit side of the edge: I_1 * (1 - 0.5) * boost.
        CHECK(Near(ModulatedTerm(i1, i1, 0.5f * 81.0f, 1.0f * 81.0f), i1 * 0.5f * kModulationBoost, 1.0e-6f));
        // m above the level by 0.5: structure 0.5.
        CHECK(Near(ModulatedTerm(0.9f * i1, i1, 0.4f, 1.0f), i1 * 0.5f * kModulationBoost, 1.0e-6f));
        // A shadowed pixel adds nothing, whatever the level; so does one below its neighbourhood's level (the structure is clamped at 0, never negative light).
        CHECK_EQ(ModulatedTerm(0.0f, i1, 0.5f, 1.0f), 0.0f);
        CHECK_EQ(ModulatedTerm(0.3f * i1, i1, 0.8f, 1.0f), 0.0f);
        CHECK(ModulatedTerm(0.9f * i1, i1, 0.8f, 1.0f) >= 0.0f);
        // The magnitude is I_1 (not I_V): the same ratio at twice the I_1 gives twice the term, and the same I_V at another I_1 does not.
        CHECK(Near(ModulatedTerm(0.05f, 0.1f, 0.2f, 1.0f), 0.1f * 0.3f * kModulationBoost, 1.0e-6f));
        CHECK(Near(ModulatedTerm(0.10f, 0.2f, 0.2f, 1.0f), 0.2f * 0.3f * kModulationBoost, 1.0e-6f));
        // I_V = 0.05 over I_1 = 0.2 is m = 0.25, 0.05 above the level of 0.2: the term is I_1 * 0.05 * boost, not I_V * 0.05 * boost.
        CHECK(Near(ModulatedTerm(0.05f, 0.2f, 0.2f, 1.0f), 0.2f * 0.05f * kModulationBoost, 1.0e-6f));
        CHECK(!Near(ModulatedTerm(0.05f, 0.2f, 0.2f, 1.0f), 0.05f * 0.05f * kModulationBoost, 1.0e-4f));
        // The modulation is clamped inside the term: more shadowed than unshadowed light (rounding noise) at a level of 0.2 is m = 1, structure 0.8 ...
        CHECK(Near(ModulatedTerm(3.0f, 1.0f, 0.2f, 1.0f), 1.0f * 0.8f * kModulationBoost, 1.0e-6f));
        // ... and in a flat unshadowed neighbourhood (level 1) it adds nothing.
        CHECK_EQ(ModulatedTerm(3.0f, 1.0f, 1.0f, 1.0f), 0.0f);
        // The bound.
        CHECK_EQ(ModulatedTerm(4.0f, 4.0f, 0.0f, 1.0f), kMaxTerm);
        CHECK(ModulatedTerm(1.5f, 1.5f, 0.0f, 1.0f) <= kMaxTerm);
        // I_1 at or under the guard: nothing.
        CHECK_EQ(ModulatedTerm(0.00005f, 0.00005f, 0.0f, 1.0f), 0.0f);
        // Broken numbers add nothing (a NaN channel, a NaN level, a negative I_1).
        CHECK_EQ(ModulatedTerm(QuietNan(), 0.5f, 0.0f, 1.0f), 0.0f);
        CHECK_EQ(ModulatedTerm(0.5f, QuietNan(), 0.0f, 1.0f), 0.0f);
        CHECK_EQ(ModulatedTerm(0.5f, 0.5f, QuietNan(), 1.0f), 0.0f);
        CHECK_EQ(ModulatedTerm(0.5f, 0.5f, 0.0f, QuietNan()), 0.0f);
        CHECK_EQ(ModulatedTerm(0.5f, -0.5f, 0.0f, 1.0f), 0.0f);
        // A ramp of m across a step: only the part above the level counts, and it rises with m.
        float last = -1.0f;
        for (int k = 0; k <= 10; ++k)
        {
            const float m = float(k) / 10.0f;
            const float term = ModulatedTerm(m * i1, i1, 0.3f, 1.0f);
            CHECK(term >= last);
            CHECK((m <= 0.3f) == (term == 0.0f) || m == 0.3f);
            last = term;
        }
    }

    // ---- the grid the level is read on (Brief R4, v3's sparse grid): stride from the resolution, centre = the nearest texel --------------------------------
    {
        CHECK_EQ(kBeamTapsPerSide, 4);
        CHECK(kBeamReachPixels == 48.0f);
        CHECK_EQ(BeamTapStride(4.0f), 3);    // the quarter resolution: 48 pixels / 4 taps / 4 pixels per texel
        CHECK_EQ(BeamTapStride(2.0f), 6);    // the half resolution
        CHECK_EQ(BeamTapStride(1.0f), 12);
        CHECK_EQ(BeamTapStride(1000.0f), 1); // never below one texel
        CHECK_EQ(BeamTapStride(0.0f), 3);    // a broken scale reads as the quarter
        CHECK_EQ(BeamTapStride(QuietNan()), 3);
        CHECK_EQ(BeamCentreTexel(0.0f), 0);
        CHECK_EQ(BeamCentreTexel(0.49f), 0);
        CHECK_EQ(BeamCentreTexel(0.5f), 1);  // the nearest texel, not the floor
        CHECK_EQ(BeamCentreTexel(2.5f), 3);
        CHECK_EQ(BeamCentreTexel(3.49f), 3);
        CHECK_EQ(BeamCentreTexel(-0.49f), 0);
        CHECK_EQ(BeamCentreTexel(-0.51f), -1);
        CHECK_EQ(BeamCentreTexel(-1.6f), -2);
    }

    // ---- the stats texture's layout, shared by the compute shader, the pass and the reader ---------------------------------------------------
    {
        StatsRead stats;
        CHECK(!stats.landed);
        CHECK_EQ(kStatsTexels, 2u);
        const float raw[8] { 0.5f, 0.6f, 0.7f, 0.9f, 1234.0f, 0.0f, 0.0f, 0.0f };
        DecodeStats(raw, stats);
        CHECK(Near(stats.skyCeiling[0], 0.5f, 1.0e-6f) && Near(stats.skyCeiling[2], 0.7f, 1.0e-6f));
        CHECK(Near(stats.termPeak, 0.9f, 1.0e-6f));
        CHECK_EQ(stats.skySamples, 1234u);
        const float nan[8] { QuietNan(), 0.6f, 0.7f, QuietNan(), 1234.0f, 0.0f, 0.0f, 0.0f };
        DecodeStats(nan, stats);
        CHECK(stats.skyCeiling[0] == 0.0f && stats.termPeak == 0.0f); // a broken read is no ceiling and no peak, never a NaN
    }

    return Finish("godrays_march_rules");
}
