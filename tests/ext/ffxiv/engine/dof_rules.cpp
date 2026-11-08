// Depth of field, Route B: the pure rules on the host (no device).
//
// What a wrong rule would break, and what is checked here:
//   the analytic CoC: 0 exactly on the focus plane, positive toward the camera, negative away, monotone across the depth axis,
//   and a broken input answers 0 not a full-frame blur;
//   the shaped CoC (the focus band, the near cap): |c| <= 0.12 is exactly 0, the magnitude above it is remapped linearly (continuous at
//   the edge), the near side stops at 0.25 and the far side reaches -1, the sign survives, NaN and infinities answer in range;
//   the near/far split: the two weights are the CoC's own halves, one always zero, both zero on the focus plane;
//   the focus modes: manual/manual case-insensitivity, unknown text auto, NaN distance default;
//   every clamp: NaN -> default, the band's edges pinch, steps' range;
//   Wanted: strength 0 (or an aperture at the floor) is no run at all, which is what the identity at that key is;
//   the view depth, through the same inverse projection the march's fixture carries: the near and far plane round trip,
//   and the middle depths land between them;
//   the camera band: the capture's own planes in band, a wrong layout (a translated z row, out-of-range planes) refused;
//   the quarter extent, the reach in both scales, the disc's tap table at 16/24/48 (the exact radii and angles);
//   the tap weight and the reach rule; the merge amounts and the mix (one branch always zero, strength scaling, NaN guards);
//   the outcomes and the log lines: the two hold wordings, the skip decomposition, the card, the session cost line;
//   the keys: every default the schema and the card state.
#include "check.h"

#include "ext/ffxiv/engine/DofRules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace ext::ffxiv::dof;
using ext::ffxiv::shafts::Window;

namespace
{
// The march's fixture camera: the inverse projection whose near is 0.1 m and whose far is 100.0 m (the exact table the
// volumetric march's WARP test reconstructs rays through and its report pins as the build's own camera layout).
constexpr float kNear = 0.1f;
constexpr float kFar = 100.0f;

void FillInverseProjection(float out[16])
{
    const float a = kNear / (kFar - kNear);
    const float b = kNear * kFar / (kFar - kNear);
    const float tanFov = 1.0f;
    out[0] = b / tanFov;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 0.0f;
    out[4] = 0.0f;
    out[5] = b / tanFov;
    out[6] = 0.0f;
    out[7] = 0.0f;
    out[8] = 0.0f;
    out[9] = 0.0f;
    out[10] = 0.0f;
    out[11] = -b;
    out[12] = 0.0f;
    out[13] = 0.0f;
    out[14] = 1.0f;
    out[15] = a;
}

void FillProjection(float out[16])
{
    const float a = kNear / (kFar - kNear);
    const float b = kNear * kFar / (kFar - kNear);
    out[0] = 1.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 0.0f;
    out[4] = 0.0f;
    out[5] = 1.0f;
    out[6] = 0.0f;
    out[7] = 0.0f;
    out[8] = 0.0f;
    out[9] = 0.0f;
    out[10] = a;
    out[11] = b;
    out[12] = 0.0f;
    out[13] = 0.0f;
    out[14] = 0.0f - 1.0f;
    out[15] = 0.0f;
}
} // namespace

int main()
{
    // ---- the analytic CoC: the rules' own formula, shaped (a sharp focus band, a capped near side) -------------------------------
    {
        const float focus = 20.0f;
        CHECK_EQ(CoCSigned(focus, focus), 0.0f); // the focus plane is exactly no blur
        // Beyond the focus band the near side is positive and the far side negative. (Inside the band both are 0: one metre
        // off a 20 m focus is a raw CoC of 0.026, well inside the band's 0.12.)
        CHECK(CoCSigned(focus - 6.0f, focus) > 0.0f); // 14 m: raw 0.176
        CHECK(CoCSigned(focus + 6.0f, focus) < 0.0f); // 26 m: raw -0.130
        CHECK_EQ(CoCSigned(focus - 1.0f, focus), 0.0f);
        CHECK_EQ(CoCSigned(focus + 1.0f, focus), 0.0f);
        // The camera's own plane is a raw CoC of almost +1, which the near cap holds at kNearCap; infinity is almost a full
        // far CoC and the far side is not capped.
        CHECK_EQ(CoCSigned(1.0f, focus), kNearCap);
        CHECK(CoCSigned(2000.0f, focus) < -0.9f);
        // Monotone across the depth axis: a farther depth is never a larger CoC, and strictly smaller wherever the CoC is
        // neither inside the band (0) nor held at the near cap.
        float previous = 2.0f;
        for (float z = focus - 5.0f; z <= focus + 20.0f; z += 0.5f)
        {
            const float c = CoCSigned(z, focus);
            CHECK(c <= previous);
            if (c != 0.0f && c != kNearCap && previous != 0.0f && previous != kNearCap)
                CHECK(c < previous);
            previous = c;
        }
        // The shaped CoC's ends: the near side stops at the cap, the far side reaches -1, never past it.
        CHECK(CoCSigned(0.001f, 20.0f) <= kNearCap);
        CHECK(CoCSigned(1.0e6f, 20.0f) >= -1.0f);
        // A broken read is no blur, not a broken pack (rule 6's own answer for this stage).
        CHECK_EQ(CoCSigned(0.0f, focus), 0.0f);
        CHECK_EQ(CoCSigned(-5.0f, focus), 0.0f);
        CHECK_EQ(CoCSigned(std::nanf(""), focus), 0.0f);
        CHECK_EQ(CoCSigned(focus, std::nanf("")), 0.0f);
        CHECK_EQ(CoCSigned(focus, 0.0f), 0.0f);
        // The one pair that gets past both positivity guards and still divides to NaN (inf - inf over inf + inf).
        CHECK_EQ(CoCSigned(INFINITY, INFINITY), 0.0f);
        // The near/far split: the CoC's own halves, one always zero.
        const float nearCoc = CoCSigned(10.0f, 20.0f);
        const float farCoc = CoCSigned(400.0f, 20.0f);
        CHECK(NearWeight(nearCoc) > 0.0f);
        CHECK_EQ(FarWeight(nearCoc), 0.0f);
        CHECK_EQ(NearWeight(farCoc), 0.0f);
        CHECK(FarWeight(farCoc) > 0.0f);
        CHECK_EQ(NearWeight(0.0f), 0.0f);
        CHECK_EQ(FarWeight(0.0f), 0.0f);
    }

    // ---- the shaped CoC: a sharp focus band, a linear remap above it, the near side capped, the far side not ----------------------
    {
        // The two constants the shader mirrors (dof_pins reads the HLSL's numbers against these).
        CHECK_EQ(kDeadZone, 0.12f);
        CHECK_EQ(kNearCap, 0.25f);
        CHECK(kDeadZone > 0.0f && kDeadZone < 1.0f);
        CHECK(kNearCap > 0.0f && kNearCap < 1.0f);

        // Inside the band, and at the band's edge: exactly no blur, on both sides.
        for (const float raw : { 0.0f, 0.01f, -0.01f, 0.05f, -0.05f, 0.119f, -0.119f, kDeadZone, -kDeadZone })
            CHECK_EQ(ShapeCoC(raw), 0.0f);

        // Above the band the magnitude is remapped linearly, (|c| - dead zone) / (1 - dead zone): continuous at the edge (a hard
        // cut would start a 0.12 CoC, a 6.5 px blur at the default reach, at once).
        CHECK(std::fabs(ShapeCoC(-0.56f) - (-0.5f)) < 1.0e-6f);   // (0.56 - 0.12) / 0.88
        CHECK(std::fabs(ShapeCoC(-0.2f) - (-0.08f / 0.88f)) < 1.0e-6f);
        CHECK(std::fabs(ShapeCoC(0.2f) - (0.08f / 0.88f)) < 1.0e-6f); // near side below the cap: the same remap
        CHECK(std::fabs(ShapeCoC(-0.3f) - (-0.18f / 0.88f)) < 1.0e-6f);
        CHECK_EQ(ShapeCoC(-1.0f), -1.0f); // the far side's end is unchanged
        CHECK(std::fabs(ShapeCoC(-kDeadZone - 1.0e-4f)) < 2.0e-4f);   // continuity at the edge, far
        CHECK(std::fabs(ShapeCoC(kDeadZone + 1.0e-4f)) < 2.0e-4f);    // and near
        CHECK(ShapeCoC(-kDeadZone - 1.0e-4f) < 0.0f && ShapeCoC(kDeadZone + 1.0e-4f) > 0.0f);

        // The near side is capped at kNearCap and the cap is reached continuously: raw = dead zone + cap * (1 - dead zone).
        const float capRaw = kDeadZone + kNearCap * (1.0f - kDeadZone); // 0.34
        CHECK(std::fabs(ShapeCoC(capRaw) - kNearCap) < 1.0e-6f);
        CHECK(std::fabs(ShapeCoC(capRaw - 1.0e-3f) - kNearCap) < 1.5e-3f);
        CHECK(ShapeCoC(capRaw - 1.0e-3f) < kNearCap);
        CHECK_EQ(ShapeCoC(capRaw + 0.01f), kNearCap);
        CHECK_EQ(ShapeCoC(0.5f), kNearCap);
        CHECK_EQ(ShapeCoC(1.0f), kNearCap);
        // The far side with the same magnitudes is not capped.
        CHECK(ShapeCoC(-0.5f) < -kNearCap);
        CHECK(std::fabs(ShapeCoC(-0.5f) - (-0.38f / 0.88f)) < 1.0e-6f);
        CHECK(ShapeCoC(-capRaw - 0.01f) < -kNearCap + 1.0e-6f);

        // The sign survives: positive in, positive out (near), negative in, negative out (far).
        for (float raw = kDeadZone + 0.01f; raw <= 1.0f; raw += 0.01f)
        {
            CHECK(ShapeCoC(raw) > 0.0f);
            CHECK(ShapeCoC(-raw) < 0.0f);
        }
        // Out-of-range inputs clamp first: the shaped CoC lies in [-1, kNearCap] whatever comes in.
        CHECK_EQ(ShapeCoC(7.0f), kNearCap);
        CHECK_EQ(ShapeCoC(-7.0f), -1.0f);
        CHECK_EQ(ShapeCoC(INFINITY), kNearCap);
        CHECK_EQ(ShapeCoC(-INFINITY), -1.0f);
        // A broken read is no blur.
        CHECK_EQ(ShapeCoC(std::nanf("")), 0.0f);

        // Against an independent double-precision evaluation of the same formula, across and beyond the whole range, and
        // monotone (never decreasing in the raw CoC), bounded to [-1, kNearCap].
        double previous = -2.0;
        for (double raw = -1.2; raw <= 1.2; raw += 0.003)
        {
            const double c = raw < -1.0 ? -1.0 : (raw > 1.0 ? 1.0 : raw);
            const double magnitude = (std::fabs(c) - 0.12 > 0.0 ? std::fabs(c) - 0.12 : 0.0) / 0.88;
            const double expected = c > 0.0 ? (magnitude < 0.25 ? magnitude : 0.25) : -magnitude;
            const double got = double(ShapeCoC(float(raw)));
            CHECK(std::fabs(got - expected) < 2.0e-6);
            CHECK(got >= previous - 1.0e-9);
            CHECK(got >= -1.0 && got <= double(kNearCap) + 1.0e-9);
            previous = got;
        }

        // The pieces the pass reads: the near weight never passes the cap, the far weight reaches 1; the merge's near amount at
        // the cap is cap x strength and no more.
        float nearMost = 0.0f, farMost = 0.0f;
        for (float z = 0.2f; z < 5000.0f; z *= 1.07f)
        {
            nearMost = std::max(nearMost, NearWeight(CoCSigned(z, 20.0f)));
            farMost = std::max(farMost, FarWeight(CoCSigned(z, 20.0f)));
        }
        CHECK_EQ(nearMost, kNearCap);
        CHECK(farMost > 0.97f && farMost <= 1.0f);
        float nearAmount = 0.0f, farAmount = 0.0f;
        MergeAmounts(CoCSigned(0.5f, 20.0f), 0.8f, nearAmount, farAmount); // a surface half a metre from the camera: the capped near side
        CHECK(std::fabs(nearAmount - kNearCap * 0.8f) < 1.0e-6f);
        CHECK_EQ(farAmount, 0.0f);
        // A focus on a far centre (100 m) and the ground 7 m in front: raw 0.87, the capped near side, a quarter of the full CoC.
        CHECK_EQ(CoCSigned(7.0f, 100.0f), kNearCap);
        // The band's own metres: at a 20 m focus, 15.7 m to 25.4 m is sharp (raw |c| <= 0.12).
        CHECK_EQ(CoCSigned(15.8f, 20.0f), 0.0f);
        CHECK_EQ(CoCSigned(25.0f, 20.0f), 0.0f);
        CHECK(CoCSigned(15.0f, 20.0f) > 0.0f);
        CHECK(CoCSigned(26.0f, 20.0f) < 0.0f);
    }

    // ---- the view depth, through the march's fixture camera ------------------------------------------------------------------------
    {
        float inverse[16];
        FillInverseProjection(inverse);
        // The near and far plane reconstruct to their own distances; a middle depth lands between them.
        CHECK(std::fabs(ViewDistanceAtDepth(inverse, 0.0f, 0.0f, 1.0f) - kNear) < 1.0e-3f);
        CHECK(std::fabs(ViewDistanceAtDepth(inverse, 0.0f, 0.0f, 0.0f) - kFar) < 1.0e-3f);
        const float middle = ViewDistanceAtDepth(inverse, 0.25f, -0.5f, 0.5f);
        CHECK(middle > kNear && middle < kFar);
        // A depth the matrix refuses (a NaN, a negative) answers 0, and CoCSigned refuses that.
        CHECK_EQ(ViewDistanceAtDepth(inverse, 0.0f, 0.0f, std::nanf("")), 0.0f);
        CHECK_EQ(ViewDistanceAtDepth(nullptr, 0.0f, 0.0f, 0.5f), 0.0f);
    }

    // ---- the camera's band: the accepted plane pair, the refused layouts ----------------------------------------------------------
    {
        float inverse[16], projection[16];
        FillInverseProjection(inverse);
        FillProjection(projection);
        const DepthBand band = CameraDepthBand(inverse, projection);
        CHECK(band.usable);
        CHECK(std::fabs(band.nearPlane - kNear) < 1.0e-3f);
        CHECK(std::fabs(band.farPlane - kFar) < 1.0e-3f);
        // A projection whose w row is not the engine's is not this build's camera.
        projection[14] = -2.0f;
        CHECK(!CameraDepthBand(inverse, projection).usable);
        FillProjection(projection);
        // A translated z row (the inverse's own row carries a sideways push) changes the metres-per-depth form.
        float push[16];
        std::memcpy(push, inverse, sizeof push);
        push[9] = 0.25f;
        CHECK(!CameraDepthBand(push, projection).usable);
        // Planes outside the band a real camera can carry are another buffer's numbers.
        float tiny[16];
        std::memcpy(tiny, inverse, sizeof tiny);
        tiny[11] = -1.0e-7f; // the planes collapse (far below the band's floor)
        CHECK(!CameraDepthBand(tiny, projection).usable);
    }

    // ---- the keys: the clamps, the focus modes, the defaults ---------------------------------------------------------------------
    {
        const Params params = BuildParams(1.5f, 0.5f, 200, "MANUAL", 1.0e6f);
        CHECK_EQ(params.strength, kMaxStrength);
        CHECK_EQ(params.aperture, kMaxAperture);
        CHECK_EQ(params.steps, kMaxSteps);
        CHECK(params.focusMode == FocusMode::Manual);
        CHECK_EQ(params.focusDistance, kMaxFocusDistance);
        const Params defaults = BuildParams(std::nanf(""), std::nanf(""), 0, "quirk", std::nanf(""));
        CHECK_EQ(defaults.strength, kDefaultStrength);
        CHECK_EQ(defaults.aperture, kDefaultAperture);
        // The integer steps key has no NaN: a below-floor value reads as the floor, and the net default is the key's
        // own (the march's own ClampSteps rule; the ini's auto default is applied at the config stage).
        CHECK_EQ(defaults.steps, uint32_t(kMinSteps));
        CHECK(defaults.focusMode == FocusMode::Auto);
        CHECK_EQ(defaults.focusDistance, kDefaultFocusDistance);
        const Params below = BuildParams(-1.0f, -0.01f, -5, "manual", 0.1f);
        CHECK_EQ(below.strength, kMinStrength);
        CHECK_EQ(below.aperture, kMinAperture);
        CHECK_EQ(below.steps, uint32_t(kMinSteps));
        CHECK_EQ(below.focusDistance, kMinFocusDistance);
        CHECK(ParseFocusMode("manual") == FocusMode::Manual);
        CHECK(ParseFocusMode("Manual") == FocusMode::Manual);
        CHECK(ParseFocusMode("MANUAL") == FocusMode::Manual);
        CHECK(ParseFocusMode("auto") == FocusMode::Auto);
        CHECK(ParseFocusMode("anything") == FocusMode::Auto);
        CHECK(ParseFocusMode("") == FocusMode::Auto);
    }

    // ---- Wanted: the two keys' zero is no run at all, which is what the identity is ----------------------------------------------
    {
        const Params run = BuildParams(0.8f, 0.05f, 24, "auto", 3.0f);
        CHECK(Wanted(true, run));
        const Params nullStrength = BuildParams(0.0f, 0.05f, 24, "auto", 3.0f);
        CHECK(!Wanted(true, nullStrength)); // today's picture, at no cost
        const Params nullAperture = BuildParams(0.8f, 0.0f, 24, "auto", 3.0f, "cinematic");
        CHECK(!Wanted(true, nullAperture));
        // The distance look does not use the reach key: an aperture at 0 does not switch it off, only strength 0 or the master do.
        CHECK(Wanted(true, BuildParams(0.8f, 0.0f, 24, "auto", 3.0f, "distance")));
        CHECK(Wanted(true, BuildParams(0.8f, 0.0f, 24, "auto", 3.0f)));
        CHECK(!Wanted(true, BuildParams(0.0f, 0.05f, 24, "auto", 3.0f, "distance")));
        CHECK(!Wanted(false, run)); // the master switch has the last word
        CHECK(!Wanted(true, BuildParams(kStrengthEpsilon * 0.5f, 0.05f, 24, "auto", 3.0f)));
        CHECK(Wanted(true, BuildParams(kStrengthEpsilon * 4.0f, 0.05f, 24, "auto", 3.0f)));
    }

    // ---- the disc pattern: the quarter extent, the reach, the exact tap table ----------------------------------------------------
    {
        CHECK_EQ(QuarterExtent(0.0f), 0u);
        CHECK_EQ(QuarterExtent(1.0f), 0u);
        CHECK_EQ(QuarterExtent(2.0f), 1u);
        CHECK_EQ(QuarterExtent(8.0f), 2u);
        CHECK_EQ(QuarterExtent(9.0f), 3u); // an odd extent keeps its last half pixel (the march's own rule)
        CHECK_EQ(QuarterExtent(1440.0f), 360u);
        CHECK(ReachPixels(1440.0f, 0.05f) > 71.9f && ReachPixels(1440.0f, 0.05f) < 72.1f);
        CHECK_EQ(ReachPixels(0.0f, 0.05f), 0.0f);
        CHECK_EQ(ReachPixels(1440.0f, std::nanf("")), 0.0f);
        // The disc's taps: every radius inside the reach, angles monotonically stepping by the golden angle, the
        // area-uniform sqrt ramp the shader and the CPU reference agree on.
        for (const uint32_t steps : { 16u, 24u, 48u })
        {
            const float reach = ReachPixels(1440.0f, 0.05f);
            float previousAngle = -1.0f;
            for (uint32_t i = 0; i < steps; ++i)
            {
                float angle = 0.0f, radius = 0.0f;
                DiscTap(i, steps, reach, angle, radius);
                CHECK(radius > 0.0f && radius <= reach);
                const float expectedRadius = std::sqrt((float(i) + 0.5f) / float(steps)) * reach;
                CHECK(std::fabs(radius - expectedRadius) < 1.0e-6f);
                if (i > 0)
                {
                    CHECK(angle > previousAngle);
                    CHECK(std::fabs(angle - previousAngle - kGoldenAngle) < 1.0e-5f);
                }
                previousAngle = angle;
            }
            // The greatest tap radius is the reach itself, to one step's fraction (the sqrt ramp's own end).
            float lastAngle = 0.0f, lastRadius = 0.0f;
            DiscTap(steps - 1, steps, reach, lastAngle, lastRadius);
            CHECK(lastRadius > reach * 0.97f);
        }
        // Out-of-range indices answer zeros, and zero reach answers nothing.
        float angle = 1.0f, radius = 1.0f;
        DiscTap(24, 16, 10.0f, angle, radius);
        CHECK_EQ(angle, 0.0f);
        CHECK_EQ(radius, 0.0f);
        DiscTap(0, 24, 0.0f, angle, radius);
        CHECK_EQ(radius, 0.0f);
    }

    // ---- the tap weight and the reach rule ----------------------------------------------------------------------------------------
    {
        CHECK_EQ(TapWeight(0.0f), 0.0f); // a focused texel blurs nothing
        CHECK_EQ(TapWeight(0.4f), 0.4f);
        CHECK_EQ(TapWeight(-0.3f), 0.3f); // the class is the sign's own, the weight the amount's
        CHECK_EQ(TapWeight(2.0f), 1.0f);  // clamped to the pack's own unit circle
        CHECK_EQ(TapWeight(std::nanf("")), 0.0f);
        CHECK(TapReaches(0.0f, 5.0f));
        CHECK(TapReaches(5.0f, 5.0f));
        CHECK(!TapReaches(5.0f + 1.0e-6f, 5.0f)); // not past the tap's own coverage
        CHECK(!TapReaches(std::nanf(""), 5.0f));
        CHECK(!TapReaches(1.0f, std::nanf("")));
    }

    // ---- the merge: the amounts and the mix ----------------------------------------------------------------------------------------
    {
        float nearAmount = 0.0f, farAmount = 0.0f;
        MergeAmounts(0.25f, 0.8f, nearAmount, farAmount);
        CHECK_EQ(nearAmount, 0.2f); // strength scales the CoC, never above it
        CHECK_EQ(farAmount, 0.0f);  // exactly one branch is ever non-zero
        MergeAmounts(-0.5f, 0.8f, nearAmount, farAmount);
        CHECK_EQ(nearAmount, 0.0f);
        CHECK_EQ(farAmount, 0.4f);
        MergeAmounts(0.0f, 0.8f, nearAmount, farAmount);
        CHECK_EQ(nearAmount, 0.0f);
        CHECK_EQ(farAmount, 0.0f); // the focus plane mixes nothing
        MergeAmounts(1.0f, 0.0f, nearAmount, farAmount);
        CHECK_EQ(nearAmount, 0.0f);
        CHECK_EQ(farAmount, 0.0f); // strength 0, no mix at all
        // The mix: scene + amount*(blurred - scene), byte for the in-focus case.
        const float scene[3] { 0.25f, 0.5f, 0.75f };
        const float blurred[3] { 0.5f, 0.5f, 0.5f };
        float out[3] {};
        DofMix(scene, blurred, 0.0f, out);
        CHECK_EQ(out[0], scene[0]);
        CHECK_EQ(out[1], scene[1]);
        CHECK_EQ(out[2], scene[2]);
        DofMix(scene, blurred, 0.5f, out);
        CHECK(std::fabs(out[0] - 0.375f) < 1.0e-6f);
        const float broken[3] { std::nanf(""), 0.5f, 0.5f };
        DofMix(scene, broken, 0.5f, out);
        CHECK_EQ(out[0], scene[0]); // a garbage tap's answer is no blur, not a burnt pixel
    }

    // ---- the quarter targets' own belt, the tone-map shape's needs -----------------------------------------------------------------
    {
        const Window ok { 0.0f, 0.0f, 1280.0f, 720.0f };
        CHECK(QuarterTargetsPossible(ok));
        const Window empty { 0.0f, 0.0f, 0.0f, 0.0f };
        CHECK(!QuarterTargetsPossible(empty));
        const Window tiny { 0.0f, 0.0f, 3.0f, 3.0f };
        CHECK(!QuarterTargetsPossible(tiny)); // a quarter of this is a one-texel pass (the march's own rule)
    }

    // ---- the declines, the outcomes and the wordings ------------------------------------------------------------------------------
    {
        CHECK_EQ(DeclineName(Decline::None), std::string("run"));
        CHECK_EQ(OutcomeName(Outcome::HoldStands), std::string("the chain's hold stands"));
        CHECK_EQ(OutcomeName(Outcome::EngineOwn), std::string("the frame is the engine's own"));
        // The chain's hold semantics name the decline's own outcome; the two wordings are what the log says.
        const std::string chained = FormatDecline(Decline::SceneFormat, Outcome::HoldStands);
        CHECK(chained.find("the chain's hold stands") != std::string::npos);
        const std::string owned = FormatDecline(Decline::SceneFormat, Outcome::EngineOwn);
        CHECK(owned.find("the frame is the engine's own") != std::string::npos);
        // The stand-down's counting rule: a capture that stopped landing counts, "not the tone map" and no DoF does not.
        CHECK(CountsTowardsStandDown(Decline::NoCamera));
        CHECK(CountsTowardsStandDown(Decline::SceneFormat));
        CHECK(CountsTowardsStandDown(Decline::NoDepth));
        CHECK(CountsTowardsStandDown(Decline::SizeMismatch));
        CHECK(CountsTowardsStandDown(Decline::PassFailed));
        CHECK(!CountsTowardsStandDown(Decline::NotToneMap));
        CHECK(!CountsTowardsStandDown(Decline::FeatureOff));
        CHECK(!CountsTowardsStandDown(Decline::CharaReplay));
        CHECK(!CountsTowardsStandDown(Decline::SceneHdr));
        // The skip decomposition: each reason moves exactly its own count and the shared skipped count, once.
        SkipCounts skips;
        CountSkip(skips, SkipReason::NoDepth);
        CountSkip(skips, SkipReason::NoDepth);
        CountSkip(skips, SkipReason::NoCamera);
        CHECK_EQ(skips.skipped, 3u);
        CHECK_EQ(skips.noDepth, 2u);
        CHECK_EQ(skips.noCamera, 1u);
        CountSkip(skips, SkipReason::None);
        CHECK_EQ(skips.skipped, 3u);
        CHECK_EQ(SkipName(SkipReason::NoDepth), std::string("the fog post's depth or mapping row has not been secured yet"));
        const std::string skipLine = FormatSkip(SkipReason::NoCamera);
        CHECK(skipLine.find("no depth of field in it") != std::string::npos);
    }

    // ---- the card and the run lines are pure rules --------------------------------------------------------------------------------
    {
        CardCounts counts;
        counts.frames = 120;
        counts.runs = 90;
        counts.declined = 18;
        counts.skipped = 12;
        counts.noDepth = 8;
        counts.noCamera = 4;
        counts.refusals = 2;
        counts.armed = true;
        counts.strength = 0.8f;
        counts.aperture = 0.05f;
        counts.steps = 24;
        counts.focusMode = FocusMode::Auto;
        const std::string card = FormatCard(counts);
        CHECK(card.find("120 frames") != std::string::npos);
        CHECK(card.find("90 runs") != std::string::npos);
        CHECK(card.find("focus mode auto") != std::string::npos);
        const std::string sessionOff = FormatCard([] {
            CardCounts off;
            off.armed = false;
            off.frames = 4;
            return off;
        }());
        CHECK(sessionOff.find("off for this session") != std::string::npos);
        RunFacts runAuto;
        runAuto.windowWidth = 1920;
        runAuto.windowHeight = 1080;
        runAuto.quarterWidth = 480;
        runAuto.quarterHeight = 270;
        const std::string runLine = FormatRun(runAuto);
        CHECK(runLine.find("the view depth under the screen centre") != std::string::npos);
        RunFacts runManual = runAuto;
        runManual.focusMode = FocusModeName(FocusMode::Manual);
        runManual.focus = 12.0f;
        const std::string manualLine = FormatRun(runManual);
        CHECK(manualLine.find("12.0 m") != std::string::npos);
        const std::string cost = FormatCost(true, 0.25, 100, 80, 20);
        CHECK(cost.find("0.2500 ms on the GPU") != std::string::npos);
        CHECK(FormatCost(false, 0.0, 100, 80, 20).find("not measured yet") != std::string::npos);
    }

    // ---- the scene-HDR and camera lines exist as wording, once a session ----------------------------------------------------------
    {
        CHECK(std::string(kSceneHdrInactive).find("scene HDR") != std::string::npos);
        CHECK(std::string(kCameraRefused).find("build mismatch") != std::string::npos);
    }


    // ---- the look: the key's parse and the defaults ------------------------------------------------------------------------------
    {
        CHECK(ParseMode("") == Mode::Distance); // an unknown or empty text reads as the default look, never as the old one
        CHECK(ParseMode("distance") == Mode::Distance);
        CHECK(ParseMode("Distance") == Mode::Distance);
        CHECK(ParseMode("auto") == Mode::Distance);
        CHECK(ParseMode("quirk") == Mode::Distance);
        CHECK(ParseMode("cinematic") == Mode::Cinematic);
        CHECK(ParseMode("Cinematic") == Mode::Cinematic);
        CHECK(ParseMode("CINEMATIC") == Mode::Cinematic);
        CHECK_EQ(std::string(ModeName(Mode::Distance)), std::string("distance"));
        CHECK_EQ(std::string(ModeName(Mode::Cinematic)), std::string("cinematic"));
        CHECK(Params{}.mode == Mode::Distance);
        CHECK(BuildParams(0.8f, 0.05f, 24, "auto", 3.0f).mode == Mode::Distance); // the five-argument form predates the key
        CHECK(BuildParams(0.8f, 0.05f, 24, "auto", 3.0f, "cinematic").mode == Mode::Cinematic);
        CHECK(BuildParams(0.8f, 0.05f, 24, "auto", 3.0f, "nonsense").mode == Mode::Distance);
        // The other keys survive the overload unchanged.
        const Params cinematic = BuildParams(0.5f, 0.07f, 32, "manual", 12.0f, "cinematic");
        CHECK_EQ(cinematic.strength, 0.5f);
        CHECK_EQ(cinematic.aperture, 0.07f);
        CHECK_EQ(cinematic.steps, 32u);
        CHECK(cinematic.focusMode == FocusMode::Manual);
        CHECK_EQ(cinematic.focusDistance, 12.0f);
    }

    // ---- the distance look: the start distance, the far limit, the CoC -----------------------------------------------------------------
    {
        // The constants (the shader mirrors them; dof_pins reads the HLSL's numbers against these).
        CHECK_EQ(kStartFocusMultiple, 2.0f);
        CHECK_EQ(kStartFloorMetres, 20.0f);
        CHECK_EQ(kFarOverStart, 8.0f);
        CHECK(kDistanceMaxRadius >= 0.005f && kDistanceMaxRadius <= 0.01f); // 0.5 % to 1 % of the window height
        CHECK(kDistanceMixAtLimit > 0.0f && kDistanceMixAtLimit < 1.0f);

        // The start: the larger of a multiple of the focus and a floor in metres.
        CHECK_EQ(DistanceStart(8.4f), 20.0f);   // the character at 8.4 m: the floor decides
        CHECK_EQ(DistanceStart(3.0f), 20.0f);
        CHECK_EQ(DistanceStart(10.0f), 20.0f);  // both sides equal
        CHECK_EQ(DistanceStart(30.0f), 60.0f);  // the multiple decides
        CHECK_EQ(DistanceStart(150.0f), 300.0f);
        CHECK_EQ(DistanceStart(0.0f), kStartFloorMetres);       // a broken or missing focus never moves the start under the floor
        CHECK_EQ(DistanceStart(-4.0f), kStartFloorMetres);
        CHECK_EQ(DistanceStart(std::nanf("")), kStartFloorMetres);
        CHECK_EQ(DistanceStart(INFINITY), kStartFloorMetres);
        CHECK_EQ(DistanceFarLimit(20.0f), 160.0f);
        CHECK_EQ(DistanceFarLimit(60.0f), 480.0f);
        CHECK_EQ(DistanceFarLimit(0.0f), 0.0f);

        // No blur at or before the start, whatever the focus; exact zero, not "small".
        for (const float focus : { 1.0f, 3.0f, 8.4f, 10.0f, 15.0f, 30.0f, 100.0f })
        {
            const float start = DistanceStart(focus);
            for (float z = 0.1f; z <= start; z *= 1.03f)
            {
                CHECK_EQ(DistanceCoCMagnitude(z, focus), 0.0f);
                CHECK_EQ(DistanceCoCSigned(z, focus) == 0.0f, true);
            }
            CHECK_EQ(DistanceCoCMagnitude(start, focus), 0.0f);
            CHECK(DistanceCoCMagnitude(start * 1.05f, focus) > 0.0f); // and it starts right after the start (continuous, not a step)
            CHECK(DistanceCoCMagnitude(start * 1.0001f, focus) < 0.001f);
        }

        // The thin-lens form: |1/start - 1/z| over |1/start - 1/far|, against an independent double evaluation, clamped to [0, 1].
        for (const float focus : { 3.0f, 8.4f, 30.0f })
        {
            const double start = double(DistanceStart(focus));
            const double farLimit = start * 8.0;
            double previous = -1.0;
            for (double z = start; z < farLimit * 4.0; z *= 1.07)
            {
                const double expected = std::fmin(1.0, std::fmax(0.0, (1.0 / start - 1.0 / z) / (1.0 / start - 1.0 / farLimit)));
                const double got = double(DistanceCoCMagnitude(float(z), focus));
                CHECK(std::fabs(got - expected) < 2.0e-5);
                CHECK(got >= previous - 1.0e-9); // never decreasing with distance
                CHECK(got >= 0.0 && got <= 1.0);
                previous = got;
            }
        }
        // The saturation: at the far limit and beyond it, the full CoC and not more (the radius is capped through it).
        CHECK(std::fabs(DistanceCoCMagnitude(160.0f, 8.4f) - 1.0f) < 1.0e-6f);
        CHECK_EQ(DistanceCoCMagnitude(1700.0f, 8.4f), 1.0f);
        CHECK_EQ(DistanceCoCMagnitude(1.0e6f, 8.4f), 1.0f);
        CHECK(DistanceCoCMagnitude(INFINITY, 8.4f) <= 1.0f);
        float most = 0.0f;
        for (float z = 0.05f; z < 1.0e7f; z *= 1.11f)
            most = std::fmax(most, DistanceCoCMagnitude(z, 8.4f));
        CHECK_EQ(most, 1.0f);
        // Half way in thin-lens terms: 1/z midway between 1/start and 1/far is a CoC of one half.
        {
            const double start = 20.0, farLimit = 160.0;
            const double zHalf = 1.0 / (0.5 * (1.0 / start + 1.0 / farLimit));
            CHECK(std::fabs(double(DistanceCoCMagnitude(float(zHalf), 8.4f)) - 0.5) < 1.0e-4);
        }

        // The near side never blurs: the signed CoC (the pack's alpha, negative = far) is never positive, so the near weight is always 0.
        for (const float focus : { 1.0f, 8.4f, 40.0f })
            for (float z = 0.01f; z < 1.0e6f; z *= 1.09f)
            {
                const float signedCoc = DistanceCoCSigned(z, focus);
                CHECK(signedCoc <= 0.0f);
                CHECK_EQ(NearWeight(signedCoc), 0.0f);
                CHECK_EQ(FarWeight(signedCoc), DistanceCoCMagnitude(z, focus));
                if (z < focus)
                    CHECK_EQ(signedCoc == 0.0f, true);
            }
        // A broken read is no blur.
        CHECK_EQ(DistanceCoCMagnitude(0.0f, 8.4f), 0.0f);
        CHECK_EQ(DistanceCoCMagnitude(-5.0f, 8.4f), 0.0f);
        CHECK_EQ(DistanceCoCMagnitude(std::nanf(""), 8.4f), 0.0f);
        CHECK_EQ(DistanceCoCMagnitude(100.0f, std::nanf("")), DistanceCoCMagnitude(100.0f, 0.0f)); // a missing focus uses the floor start
        CHECK(DistanceCoCMagnitude(100.0f, std::nanf("")) > 0.0f);
        // The user's frames: the character wall at 8.4 m and the 15 m walls behind it are sharp, the castle (160 m) and the sea (175-1700 m) full.
        CHECK_EQ(DistanceCoCMagnitude(8.4f, 8.4f), 0.0f);
        CHECK_EQ(DistanceCoCMagnitude(15.0f, 8.4f), 0.0f);
        CHECK(DistanceCoCMagnitude(160.0f, 8.4f) >= 0.999f);

        // The radius: a fixed fraction of the window height, never more, whatever the CoC or the reach key.
        CHECK(std::fabs(DistanceReachPixels(1080.0f) - kDistanceMaxRadius * 1080.0f) < 1.0e-4f);
        CHECK(std::fabs(DistanceReachPixels(2160.0f) - 2.0f * DistanceReachPixels(1080.0f)) < 1.0e-3f);
        CHECK_EQ(DistanceReachPixels(0.0f), 0.0f);
        CHECK_EQ(DistanceReachPixels(std::nanf("")), 0.0f);
        CHECK_EQ(DistanceReachPixels(-5.0f), 0.0f);
        CHECK(ReachPixelsFor(Mode::Distance, 1080.0f, 0.12f) <= 0.01f * 1080.0f); // the aperture key does not widen the distance look
        CHECK_EQ(ReachPixelsFor(Mode::Distance, 1080.0f, 0.12f), ReachPixelsFor(Mode::Distance, 1080.0f, 0.01f));
        CHECK_EQ(ReachPixelsFor(Mode::Cinematic, 1080.0f, 0.05f), ReachPixels(1080.0f, 0.05f)); // the old look: aperture x window height
        CHECK(ReachPixelsFor(Mode::Cinematic, 1080.0f, 0.05f) > 53.9f && ReachPixelsFor(Mode::Cinematic, 1080.0f, 0.05f) < 54.1f);
        // The largest blur radius of a frame: the CoC (<= 1) times the reach; for the distance look that is the reach itself.
        CHECK(DistanceCoCMagnitude(1.0e6f, 8.4f) * DistanceReachPixels(1080.0f) <= 0.01f * 1080.0f);

        // The mix at the far limit: a share of the blurred picture, so the far side is softer and not destroyed.
        CHECK_EQ(EffectiveStrength(Mode::Cinematic, 0.8f), 0.8f);
        CHECK_EQ(EffectiveStrength(Mode::Cinematic, 2.0f), kMaxStrength);
        CHECK(std::fabs(EffectiveStrength(Mode::Distance, 1.0f) - kDistanceMixAtLimit) < 1.0e-7f);
        CHECK(std::fabs(EffectiveStrength(Mode::Distance, 0.8f) - 0.8f * kDistanceMixAtLimit) < 1.0e-7f);
        CHECK_EQ(EffectiveStrength(Mode::Distance, 0.0f), 0.0f);
        CHECK_EQ(EffectiveStrength(Mode::Distance, std::nanf("")), kDefaultStrength * kDistanceMixAtLimit);
    }

    // ---- the auto focus: the sample, the smoothing ----------------------------------------------------------------------------------
    {
        CHECK_EQ(kFocusTimeConstant, 0.4f);
        CHECK_EQ(kFocusFarTimeConstant, 1.6f);
        CHECK(kFocusFarTimeConstant > kFocusTimeConstant);
        CHECK_EQ(kFocusFarRatio, 3.0f);
        CHECK_EQ(kFocusStaleSeconds, 1.0f);
        CHECK_EQ(kFocusTaps, 5u);
        CHECK(kFocusTapOffsetFraction > 0.0f && kFocusTapOffsetFraction < 0.05f);

        // The weight of one step: 1 - exp(-dt / tau); nothing for no time, the whole way for a stale gap.
        CHECK_EQ(FocusAlpha(0.0f, 0.4f), 0.0f);
        CHECK(std::fabs(FocusAlpha(0.4f, 0.4f) - (1.0f - std::exp(-1.0f))) < 1.0e-6f); // one time constant: 63.2 %
        CHECK(std::fabs(FocusAlpha(0.2f, 0.4f) - (1.0f - std::exp(-0.5f))) < 1.0e-6f);
        CHECK(FocusAlpha(0.5f, 0.4f) > FocusAlpha(0.1f, 0.4f));
        CHECK(FocusAlpha(0.1f, 1.6f) < FocusAlpha(0.1f, 0.4f)); // a longer constant moves less
        CHECK_EQ(FocusAlpha(kFocusStaleSeconds, 0.4f), 1.0f);   // a gap this long: the focus is stale and the sample is adopted
        CHECK_EQ(FocusAlpha(30.0f, 1.6f), 1.0f);
        CHECK_EQ(FocusAlpha(-1.0f, 0.4f), 0.0f);
        CHECK_EQ(FocusAlpha(std::nanf(""), 0.4f), 0.0f);
        CHECK_EQ(FocusAlpha(0.1f, 0.0f), 1.0f);                 // no time constant at all is no smoothing (the degenerate end)

        // The step: the first sample is adopted whole, a missing sample leaves the focus alone, an elapsed time moves it exponentially.
        CHECK_EQ(FocusStep(0.0f, 8.4f, 0.016f), 8.4f);           // nothing before: snap
        CHECK_EQ(FocusStep(-1.0f, 8.4f, 0.0f), 8.4f);
        CHECK_EQ(FocusStep(std::nanf(""), 8.4f, 0.016f), 8.4f);
        CHECK_EQ(FocusStep(8.4f, 0.0f, 0.016f), 8.4f);           // the sky (no sample): the focus holds
        CHECK_EQ(FocusStep(8.4f, -3.0f, 0.5f), 8.4f);
        CHECK_EQ(FocusStep(8.4f, std::nanf(""), 0.5f), 8.4f);
        CHECK_EQ(FocusStep(8.4f, INFINITY, 0.5f), 8.4f);
        CHECK_EQ(FocusStep(0.0f, 0.0f, 0.016f), 0.0f);           // nothing and nothing stays nothing
        CHECK_EQ(FocusStep(8.4f, 12.0f, 0.0f), 8.4f);            // no time passed: no move
        {
            const float moved = FocusStep(8.0f, 12.0f, 0.4f);    // ratio 1.5: the fast constant; one constant = 63.2 % of the way
            CHECK(std::fabs(moved - (8.0f + 4.0f * (1.0f - std::exp(-1.0f)))) < 1.0e-5f);
            const float closer = FocusStep(12.0f, 8.0f, 0.4f);   // toward the camera: the same fast constant
            CHECK(std::fabs(closer - (12.0f - 4.0f * (1.0f - std::exp(-1.0f)))) < 1.0e-5f);
            // A far outlier (the sample past 3x the focus) is taken with the slow constant: one fast constant of time moves it a fraction.
            const float outlier = FocusStep(8.0f, 80.0f, 0.4f);
            CHECK(std::fabs(outlier - (8.0f + 72.0f * (1.0f - std::exp(-0.25f)))) < 1.0e-4f);
            CHECK(outlier < FocusStep(8.0f, 80.0f, 0.4f) + 1.0f);
            CHECK(outlier - 8.0f < 0.3f * 72.0f); // well under what the fast constant would have taken (63 %)
            // The ratio's edge: exactly 3x is still the fast constant, a hair beyond it the slow one.
            CHECK(std::fabs(FocusStep(10.0f, 30.0f, 0.4f) - (10.0f + 20.0f * (1.0f - std::exp(-1.0f)))) < 1.0e-4f);
            CHECK(FocusStep(10.0f, 30.01f, 0.4f) < 10.0f + 20.0f * 0.4f);
            // A stale gap adopts the sample whole, outlier or not.
            CHECK_EQ(FocusStep(8.0f, 80.0f, 1.5f), 80.0f);
            CHECK_EQ(FocusStep(8.0f, 12.0f, kFocusStaleSeconds), 12.0f);
        }
        // Sixty frames of a sixtieth of a second: monotone toward the sample, never past it, and about 1 - e^-2.5 of the way after a second.
        {
            float focus = 8.0f;
            float previous = focus;
            for (int frame = 0; frame < 60; ++frame)
            {
                focus = FocusStep(focus, 12.0f, 1.0f / 60.0f);
                CHECK(focus >= previous && focus <= 12.0f);
                previous = focus;
            }
            CHECK(std::fabs(focus - (12.0f - 4.0f * std::exp(-2.5f))) < 2.0e-3f);
            CHECK(focus > 8.0f + 4.0f * 0.9f && focus < 12.0f); // converged for play, not jumped
        }
        // A one-frame glitch (the centre sample jumps past the character for a single frame) does not move the focus visibly.
        {
            const float glitch = FocusStep(8.0f, 160.0f, 1.0f / 60.0f); // a far outlier for one frame
            CHECK((glitch - 8.0f) / (160.0f - 8.0f) < 0.011f); // about one percent of the gap in one frame (alpha 0.0104)
        }

        // The sample from the cross of taps: sky and broken taps are ignored, the lower median of the rest.
        {
            const float none[5] { 0.0f, 0.0f, std::nanf(""), -1.0f, INFINITY };
            CHECK_EQ(FocusSample(none, 5), 0.0f);
            const float one[5] { 0.0f, 0.0f, 8.4f, 0.0f, 0.0f };
            CHECK_EQ(FocusSample(one, 5), 8.4f);
            const float five[5] { 8.0f, 9.0f, 300.0f, 8.5f, 10.0f }; // sorted 8, 8.5, 9, 10, 300: the middle one
            CHECK_EQ(FocusSample(five, 5), 9.0f);
            const float four[5] { 8.0f, 9.0f, 300.0f, 0.0f, 10.0f }; // valid 8, 9, 10, 300: the lower of the two middle ones
            CHECK_EQ(FocusSample(four, 5), 9.0f);
            const float three[5] { 0.0f, 12.0f, 11.0f, 400.0f, 0.0f }; // valid 11, 12, 400: 12
            CHECK_EQ(FocusSample(three, 5), 12.0f);
            const float two[5] { 40.0f, 0.0f, 0.0f, 7.0f, 0.0f };     // valid 7, 40: the nearer
            CHECK_EQ(FocusSample(two, 5), 7.0f);
            CHECK_EQ(FocusSample(five, 0), 0.0f);
            CHECK_EQ(FocusSample(nullptr, 5), 0.0f);
            CHECK_EQ(FocusSample(five, 3), 9.0f); // only the first three are read: 8, 9, 300 -> 9
            // Order does not matter.
            const float shuffled[5] { 10.0f, 300.0f, 8.5f, 9.0f, 8.0f };
            CHECK_EQ(FocusSample(shuffled, 5), 9.0f);
        }
    }

    // ---- the engine's own depth of field: which draw counts, and the decline ----------------------------------------------------------
    {
        CHECK(IsEngineDofLayout(true, true));   // the weight pass: the parameter buffer and the picture it reads
        CHECK(!IsEngineDofLayout(true, false)); // the 256x1 CoC LUT update declares the buffer alone and writes no picture
        CHECK(!IsEngineDofLayout(false, true)); // any other pass that reads an sInput
        CHECK(!IsEngineDofLayout(false, false));
        CHECK(IsSceneSized(3840, 2160, 3840, 2160)); // Weight2 on the benchmark writes the scene's own allocation
        CHECK(!IsSceneSized(1920, 1080, 3840, 2160)); // the family's quarter-size passes are not the picture
        CHECK(!IsSceneSized(3840, 2160, 3840, 2159));
        CHECK(!IsSceneSized(0, 0, 0, 0));
        CHECK(EngineDofRanThisFrame(true, 77, 77));
        CHECK(!EngineDofRanThisFrame(true, 76, 77)); // the flag is this frame's own, so it needs no reset
        CHECK(!EngineDofRanThisFrame(true, 78, 77));
        CHECK(!EngineDofRanThisFrame(false, 77, 77));
        CHECK(!EngineDofRanThisFrame(false, 0, 0)); // a counter value of 0 is not "ran in frame 0"

        const std::string name = DeclineName(Decline::EngineDof);
        CHECK(name.find("engine's own depth of field") != std::string::npos);
        CHECK(!CountsTowardsStandDown(Decline::EngineDof)); // a frame the engine blurs is not a failure of this stage
        CHECK(SkipOf(Decline::EngineDof) == SkipReason::None);
        const std::string line = FormatDecline(Decline::EngineDof, Outcome::HoldStands);
        CHECK(line.find("engine's own depth of field") != std::string::npos);
        CHECK(line.find("the chain's hold stands") != std::string::npos);
        CHECK(FormatDecline(Decline::EngineDof, Outcome::EngineOwn).find("the frame is the engine's own") != std::string::npos);
    }

    // ---- what the capture frame measured: the stage line's numbers --------------------------------------------------------------------
    {
        Measured m;
        m.mode = Mode::Distance;
        m.autoFocus = true;
        m.focus = 8.43f;
        m.sample = 8.4f;
        m.windowHeight = 1080.0f;
        CHECK_EQ(FormatMeasured(m), std::string("focusM=8.43 centreM=8.40 start=20.00 farM=160.0 maxPx=8.1 engineDof=0"));
        m.focus = 30.0f;
        CHECK_EQ(FormatMeasured(m), std::string("focusM=30.00 centreM=8.40 start=60.00 farM=480.0 maxPx=8.1 engineDof=0")); // the start follows the focus
        m.engineDof = true;
        CHECK_EQ(FormatMeasured(m), std::string("focusM=30.00 centreM=8.40 start=60.00 farM=480.0 maxPx=8.1 engineDof=1"));
        m.engineDof = false;
        m.sample = 0.0f; // no sample this frame (sky at the centre)
        CHECK_EQ(FormatMeasured(m), std::string("focusM=30.00 centreM=n/a start=60.00 farM=480.0 maxPx=8.1 engineDof=0"));
        m.focus = 0.0f; // the state never held a focus
        CHECK_EQ(FormatMeasured(m), std::string("focusM=n/a centreM=n/a start=20.00 farM=160.0 maxPx=8.1 engineDof=0"));
        Measured manual;
        manual.autoFocus = false;
        manual.focus = 3.0f;
        manual.windowHeight = 1080.0f;
        CHECK_EQ(FormatMeasured(manual), std::string("focusM=3.00 centreM=n/a start=20.00 farM=160.0 maxPx=8.1 engineDof=0"));
        Measured cinematicAuto;
        cinematicAuto.mode = Mode::Cinematic;
        cinematicAuto.windowHeight = 1080.0f;
        CHECK_EQ(FormatMeasured(cinematicAuto), std::string("focusM=n/a centreM=n/a start=n/a farM=n/a maxPx=54.0 engineDof=0"));
        Measured cinematicManual = cinematicAuto;
        cinematicManual.autoFocus = false;
        cinematicManual.focus = 12.0f;
        cinematicManual.aperture = 0.03f;
        CHECK_EQ(FormatMeasured(cinematicManual), std::string("focusM=12.00 centreM=n/a start=n/a farM=n/a maxPx=32.4 engineDof=0"));
        // The run line and the card name the look.
        RunFacts run;
        run.mode = Mode::Cinematic;
        run.windowWidth = 1920;
        run.windowHeight = 1080;
        run.quarterWidth = 480;
        run.quarterHeight = 270;
        CHECK(FormatRun(run).find("mode cinematic") != std::string::npos);
        run.mode = Mode::Distance;
        CHECK(FormatRun(run).find("mode distance") != std::string::npos);
        CardCounts counts;
        counts.armed = true;
        counts.mode = Mode::Cinematic;
        CHECK(FormatCard(counts).find("mode cinematic") != std::string::npos);
    }

    return Finish("dof_rules");
}
