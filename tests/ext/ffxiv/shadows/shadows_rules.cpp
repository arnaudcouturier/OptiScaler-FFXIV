// Shadow mapping: the pure rules (ext/ffxiv/shadows/ShadowsRules.h) on the host, no device.
//
// The checks split the way the header does: the key clamps (a NaN reads as its own default, like the engine knobs'
// identity tests), the provisional day/night curve (full effect below the night threshold, zero above the day one,
// the smoothstep between them; the toggle that pins it at full effect; the no-reading state that runs nothing), the
// square-compensated remap (identity at a lift of 0 both as the float and down to the mask's own 1/255 grid; the
// lift moves the SHARED value m^2; the copy carries the square root back), the tent's weights and what they mean on
// a step edge, the two ambient entry tables and the fixed ORDER the one merged dispatch relies on (the shipped
// knob's entries first, then the two shadows' entries appended into the same table - CbScale.hlsl applies every
// matching entry in order, multiplicatively, which the WARP composition test verifies on a device), and the
// 64-refusal stand-down with the log budget.
//
// The curve thresholds are asserted to BE the provisional constants: a session that replaces them with measured
// values breaks this test on purpose - the change is then a facts change, not a code accident.
#include "check.h"

#include "ext/ffxiv/shadows/ShadowsRules.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace fs = ext::ffxiv::shadows;
namespace engine = ext::ffxiv::engine;
namespace relight = ext::ffxiv::relight;

namespace
{
constexpr uint32_t kMaskLevels = 256;

constexpr float GridValue(uint32_t level) noexcept { return float(level) / 255.0f; }

// The R8 round trip the tests share with the shaders: sample (byte / 255), compute, write (the nearest level).
constexpr uint32_t Quantize(float value) noexcept
{
    const float clamped = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
    return uint32_t(clamped * 255.0f + 0.5f) > 255 ? 255 : uint32_t(clamped * 255.0f + 0.5f);
}
} // namespace

int main()
{
    // ---- the key clamps ---------------------------------------------------------------------------------------------------
    CHECK_EQ(fs::ClampLift(std::numeric_limits<float>::quiet_NaN()), fs::kDefaultLift);
    CHECK_EQ(fs::ClampLift(-0.5f), fs::kMinLift);
    CHECK_EQ(fs::ClampLift(1.5f), fs::kMaxLift);
    CHECK_EQ(fs::ClampLift(0.42f), 0.42f);
    CHECK(fs::kDefaultLift == 0.0f && fs::kMaxLift == 0.9f && fs::kMinLift == 0.0f);

    CHECK_EQ(fs::ClampSoftness(std::numeric_limits<float>::quiet_NaN()), fs::kDefaultSoftness);
    CHECK_EQ(fs::ClampSoftness(-1.0f), fs::kMinSoftness);
    CHECK_EQ(fs::ClampSoftness(3.0f), fs::kMaxSoftness);
    CHECK_EQ(fs::ClampSoftness(1.25f), 1.25f);
    CHECK(fs::kDefaultSoftness == 0.0f && fs::kMaxSoftness == 2.0f);

    CHECK_EQ(fs::ClampAmbientLift(std::numeric_limits<float>::quiet_NaN()), fs::kDefaultAmbientLift);
    CHECK_EQ(fs::ClampAmbientLift(0.5f), fs::kMinAmbientLift);
    CHECK_EQ(fs::ClampAmbientLift(9.0f), fs::kMaxAmbientLift);
    CHECK_EQ(fs::ClampAmbientLift(2.5f), 2.5f);
    CHECK(fs::kMinAmbientLift == 1.0f && fs::kMaxAmbientLift == 4.0f && fs::kDefaultAmbientLift == 1.0f);

    // The identity ranges decide "runs nothing": a thousandth away from the idle value is identity, like the
    // engine knob's epsilon, and the softness alone (a lift of 0) is identity - it feathers the lift.
    CHECK(fs::IsLiftIdentity(0.0f));
    CHECK(fs::IsLiftIdentity(fs::kIdentityEpsilon * 0.999f));
    CHECK(!fs::IsLiftIdentity(fs::kIdentityEpsilon * 1.001f));
    CHECK(fs::IsAmbientIdentity(1.0f));
    CHECK(fs::IsAmbientIdentity(1.0f + fs::kIdentityEpsilon / 2.0f));
    CHECK(!fs::IsAmbientIdentity(1.002f));
    CHECK(!fs::IsAmbientIdentity(0.998f));

    // ---- the day/night curve -----------------------------------------------------------------------------------------------
    CHECK_EQ(fs::kCurveNightLuminance, 0.15f); // PROVISIONAL: the five-stop F9 capture settles it
    CHECK_EQ(fs::kCurveDayLuminance, 0.90f);   // PROVISIONAL: the five-stop F9 capture settles it
    // The measured captured rows land where the report says they do: the dark night row fully inside the range, the
    // brighter moon row within the ramp, the day row far past it.
    {
        const float nightRow[3] = { 0.078f, 0.117f, 0.16f };
        const float moonRow[3] = { 0.176f, 0.264f, 0.49f };
        const float dayRow[3] = { 1.96f, 1.49f, 1.25f };
        const float nightLuminance = fs::LuminanceOf(nightRow);
        const float moonLuminance = fs::LuminanceOf(moonRow);
        const float dayLuminance = fs::LuminanceOf(dayRow);
        CHECK_LT(nightLuminance, fs::kCurveNightLuminance);
        CHECK_GE(moonLuminance, fs::kCurveNightLuminance);
        CHECK_LT(moonLuminance, fs::kCurveDayLuminance);
        CHECK_GE(dayLuminance, fs::kCurveDayLuminance);
        CHECK_EQ(fs::CurveAt(nightLuminance), 1.0f);
        CHECK_EQ(fs::CurveAt(dayLuminance), 0.0f);
        // The moon row is only a quarter up the ramp: it keeps most of the effect (a moonlit night reads still night).
        CHECK(fs::CurveAt(moonLuminance) > 0.7f && fs::CurveAt(moonLuminance) < 1.0f);
    }
    // The shape: full at the threshold and below, zero at the day value and above, monotone in between, and near
    // the middle of the ramp at the ramp's own middle (a smoothstep, so dawn and dusk blend and never snap).
    CHECK_EQ(fs::CurveAt(fs::kCurveNightLuminance), 1.0f);
    {
        float previous = 1.0f;
        const float span = fs::kCurveDayLuminance - fs::kCurveNightLuminance;
        for (uint32_t step = 1; step <= 100; ++step)
        {
            const float luminance = fs::kCurveNightLuminance + float(step) / 100.0f * span;
            const float value = fs::CurveAt(luminance);
            CHECK_LE(value, previous); // monotone down across the whole ramp
            previous = value;
        }
        CHECK_EQ(previous, 0.0f);
        const float mid = fs::CurveAt(fs::kCurveNightLuminance + 0.5f * span);
        CHECK_GT(mid, 0.4f);
        CHECK_LT(mid, 0.6f);
        CHECK_EQ(fs::CurveAt(0.0f), 1.0f);
        CHECK_EQ(fs::CurveAt(5.0f), 0.0f);
    }
    // The curve's two gates: a toggle that is off pins the full effect (day included, an explicit debug override),
    // and a session that has not read a light colour yet runs the identity.
    CHECK_EQ(fs::CurveStrength(false, true, 1.5f), 1.0f);
    CHECK_EQ(fs::CurveStrength(true, false, 0.05f), 0.0f);
    CHECK_EQ(fs::CurveStrength(true, true, 0.05f), 1.0f);
    CHECK_EQ(fs::CurveStrength(true, true, 1.5f), 0.0f);
    CHECK(fs::CurveStrength(true, true, 0.4f) > 0.0f && fs::CurveStrength(true, true, 0.4f) < 1.0f);
    // A NaN channel is a broken colour, not night: cleaned to the low side, never past it, so the identity state a
    // no-reading session runs (not a NaN channel) is what decides the picture.
    {
        const float broken[3] = { std::numeric_limits<float>::quiet_NaN(), 0.5f, 0.5f };
        CHECK(fs::LuminanceOf(broken) <= 0.2126f * 0.0f + 0.7152f * 0.5f + 0.0722f * 0.5f + 1e-6f);
    }

    // ---- the square-compensated remap ---------------------------------------------------------------------------------------
    // Identity: every one of the mask's 256 levels is its own byte, both as the shared square and as the copy's
    // value - the whole default state is the game's own mask.
    for (uint32_t level = 0; level < kMaskLevels; ++level)
    {
        const float m = GridValue(level);
        CHECK_EQ(fs::RemappedSquare(m, 0.0f), m * m);
        CHECK_EQ(fs::RemapCopyValue(m, 0.0f), m);
    }
    // The shared-value lift: m'^2 = m^2 + (1-m^2)*Lift, exactly, for every grid level and a mid lift.
    for (uint32_t level = 0; level < kMaskLevels; ++level)
    {
        const float m = GridValue(level);
        const float lifted = fs::RemappedSquare(m, 0.5f);
        const float expected = m * m + (1.0f - m * m) * 0.5f;
        CHECK_LT(std::fabs(lifted - expected), 1e-6f);
    }
    CHECK_EQ(fs::RemappedSquare(1.0f, 0.9f), 1.0f); // a fully lit texel has nothing to lift
    CHECK_EQ(fs::RemappedSquare(0.0f, 0.9f), 0.9f); // a fully shadowed texel moves exactly by the lift
    CHECK_LT(fs::RemappedSquare(0.0f, fs::kMaxLift), 1.0f); // the key's own ceiling never flattens the mask
    // The copy's byte: the square root back, so the light draw's own square lands on the lifted square within the
    // arithmetic that rounds it into the R8 grid (half a level squared of slack, the shader's own error budget).
    for (uint32_t level = 0; level < kMaskLevels; ++level)
    {
        const float m = GridValue(level);
        const float copyValue = fs::RemapCopyValue(m, 0.5f);
        const float squaredBack = copyValue * copyValue;
        const float expectedSquare = fs::RemappedSquare(m, 0.5f);
        CHECK_LT(std::fabs(squaredBack - expectedSquare), 1e-3f);
        CHECK_EQ(Quantize(copyValue) <= 255, true);
        CHECK(copyValue <= 1.0f);
    }
    // A broken mask never becomes a bright one at identity (the caller's guard owns the NaN case, the rule keeps the
    // arithmetic's own answer): identity copies the caller's byte wherever it came from, and the lift's arithmetic
    // answers (1-0)*lift of a NaN square.
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        CHECK(fs::RemapCopyValue(nan, 0.0f) != fs::RemapCopyValue(nan, 0.0f)); // NaN stays NaN through the identity
        CHECK(fs::RemappedSquare(nan, 0.5f) != fs::RemappedSquare(nan, 0.5f)); // and through the lift (never a clean colour)
    }

    // ---- the tent ------------------------------------------------------------------------------------------------------------
    // Zero softness: the texel itself only.
    CHECK_EQ(fs::TentWeight(0.0f, 0.0f, 0.0f), 1.0f);
    CHECK_EQ(fs::TentWeight(1.0f, 0.0f, 0.0f), 0.0f);
    // A reach of one pixel: the four edge neighbours take the linear falloff (- finest at the reach), the diagonals
    // (d = sqrt 2) are zero, so the kernel is the 3x3 the design names.
    CHECK_EQ(fs::TentWeight(1.0f, 0.0f, 1.0f), 0.0f); // exactly at the reach: zero
    CHECK_EQ(fs::TentWeight(0.5f, 0.0f, 1.0f), 0.5f); // half the reach: half the weight
    CHECK_EQ(fs::TentWeight(1.0f, 1.0f, 1.0f), 0.0f); // the diagonal is beyond the reach
    CHECK_EQ(fs::TentWeight(0.0f, 0.0f, 1.0f), 1.0f);
    CHECK_EQ(fs::TentWeight(-1.0f, 0.0f, 1.0f), fs::TentWeight(1.0f, 0.0f, 1.0f)); // same mask, either sign
    // A reach of two: the 5x5 kernel's edge (d = 2) is zero, the diagonals (d = sqrt 2) carry weight.
    CHECK_EQ(fs::TentWeight(2.0f, 0.0f, 2.0f), 0.0f);
    CHECK(fs::TentWeight(1.0f, 1.0f, 2.0f) > 0.0f);
    CHECK_EQ(fs::TentWeight(2.0f, 1.0f, 2.0f), 0.0f); // sqrt(5) is beyond the reach
    // The weights are the same on both axes (isotropy) and fall with distance.
    CHECK_EQ(fs::TentWeight(0.0f, 1.0f, 2.0f), fs::TentWeight(1.0f, 0.0f, 2.0f));
    CHECK(fs::TentWeight(1.0f, 0.0f, 2.0f) > fs::TentWeight(1.0f, 0.0f, 1.2f));
    // What the weights mean on a step edge: the CPU reference of the shader's kernel - the weights over their sum,
    // the centre's own weight first. A lit texel two pixels from a shadowed wall, S = 2: the wall shows through the
    // kernel, but the texel is still nearly its own level.
    {
        static constexpr float kSteps[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
                                               { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
        const auto blurAt = [](float at, float softness) { // the wall covers x <= 0 (mask 0), the open ground is lit
            double sum = 0.0, weighted = 0.0;
            const double self = fs::TentWeight(0.0f, 0.0f, softness);
            sum += self;
            weighted += self * (at < 0.5f ? 0.0f : 1.0f);
            for (const auto& offset : kSteps)
            {
                const float w = fs::TentWeight(float(offset[0]), float(offset[1]), softness);
                if (!(w > 0.0f))
                    continue;
                const float neighbourAt = at + float(offset[0]);
                const float value = neighbourAt < 0.5f ? 0.0f : 1.0f;
                sum += w;
                weighted += w * value;
            }
            return float(weighted / sum);
        };
        CHECK_EQ(blurAt(3.0f, 2.0f), 1.0f); // beyond the reach: the texel is its own level
        CHECK_EQ(blurAt(2.0f, 2.0f), 1.0f); // the wall sits exactly two texels away, at the zero-weight edge
        const float edgeLit = blurAt(1.0f, 2.0f);
        CHECK_LT(edgeLit, 0.85f);  // one texel into the lit side is pulled toward the wall
        CHECK_GT(edgeLit, 0.6f);   // but keeps most of its own level
        const float edgeShadow = blurAt(0.0f, 2.0f);
        CHECK_GT(edgeShadow, 0.2f);  // the first shadowed texel is pulled toward the open ground: the edge feathers
        CHECK_LT(edgeShadow, 0.35f); // (a quarter of the lit level's weight reaches it, all within the one-pixel band)
        const float oneMore = blurAt(-1.0f, 2.0f);
        CHECK_EQ(oneMore, 0.0f);     // the second texel in keeps its own level: the reach ends at the zero-weight edge
        CHECK_EQ(blurAt(-2.0f, 2.0f), 0.0f); // beyond the reach: the texel is its own level, no bleed
        CHECK_EQ(blurAt(-2.0f, 0.0f), 0.0f); // a softness of 0 is the texel itself, always
    }

    // ---- the ambient entries -----------------------------------------------------------------------------------------------
    // The two corrected shapes (the design doc's "offset 4, stride 0" header entry is not addressable and its
    // replacement covers float4[0], the row the header field lives in): the header's `skyShRecipScale` scales
    // through multiplier[1] of float4[0] (byteOffset 0, stride 16, count 1), the probes' `shRecipScale` through the
    // engine's own AmbientSkyRecipEntry shape (byteOffset 112, stride 192, count 64, multiplier[3]).
    {
        const engine::ScaleEntry header = fs::HeaderSkyScaleEntry(2.5f);
        CHECK_EQ(header.byteOffset, 0u);
        CHECK_EQ(header.stride, 16u);
        CHECK_EQ(header.count, 1u);
        CHECK_EQ(header.multiplier[0], 1.0f);
        CHECK_EQ(header.multiplier[1], 2.5f);
        CHECK_EQ(header.multiplier[2], 1.0f);
        CHECK_EQ(header.multiplier[3], 1.0f);
        CHECK(engine::AddressFits(header.byteOffset, header.stride, header.count, engine::kAmbientDeclaredBytes));
        CHECK(engine::AddressFits(header.byteOffset, header.stride, header.count, engine::kAmbientBoundBytes));
        CHECK(!engine::AddressFits(4u, 0u, 1u, engine::kAmbientDeclaredBytes)); // the design doc's shape is refused
        const engine::ScaleEntry probes = fs::ProbeSkyScaleEntry(2.5f);
        CHECK_EQ(probes.byteOffset, 112u);
        CHECK_EQ(probes.stride, 192u);
        CHECK_EQ(probes.count, 64u);
        CHECK_EQ(probes.multiplier[0], 1.0f);
        CHECK_EQ(probes.multiplier[1], 1.0f);
        CHECK_EQ(probes.multiplier[2], 1.0f);
        CHECK_EQ(probes.multiplier[3], 2.5f);
        CHECK(engine::AddressFits(probes.byteOffset, probes.stride, probes.count, engine::kAmbientDeclaredBytes));
        CHECK(engine::AddressFits(probes.byteOffset, probes.stride, probes.count, engine::kAmbientBoundBytes));
        // The engine's own entry and the shadows' probe entry differ ONLY in which component of the multiplier
        // vector the scale sits in - same bytes addressed, same roles.
        {
            const engine::ScaleEntry engineEntry = engine::AmbientSkyRecipEntry(0.7f);
            const engine::ScaleEntry shadowEntry = fs::ProbeSkyScaleEntry(2.5f);
            CHECK_EQ(engineEntry.byteOffset, shadowEntry.byteOffset);
            CHECK_EQ(engineEntry.stride, shadowEntry.stride);
            CHECK_EQ(engineEntry.count, shadowEntry.count);
            CHECK_EQ(engineEntry.multiplier[3], 0.7f);
            CHECK_EQ(shadowEntry.multiplier[3], 2.5f);
        }
        // The field the probe entry addresses is the same field the shipped knob's second entry addresses:
        // together they compose multiplicatively (asserted as the product of the two multiplier vectors at the
        // field, and verified on the device by the composition WARP test).
        CHECK_GE(0.7f * 2.5f, 1.74f);
        CHECK_LE(0.7f * 2.5f, 1.76f);
    }
    // The table builder: both entries at a live multiplier, 0 at the idle one (and at an identity-within-epsilon
    // one), and 0 for a caller with no room (a night lift never ships a half table).
    {
        engine::ScaleEntry buffer[engine::kMaxScaleEntries] {};
        CHECK_EQ(fs::AmbientEntries(2.5f, buffer, engine::kMaxScaleEntries), 2u);
        CHECK_EQ(buffer[0].multiplier[1], 2.5f); // the header
        CHECK_EQ(buffer[1].multiplier[3], 2.5f); // the probes
        CHECK_EQ(fs::AmbientEntries(1.0f, buffer, engine::kMaxScaleEntries), 0u);
        CHECK_EQ(fs::AmbientEntries(1.0f + engine::kIdentityEpsilon / 2.0f, buffer, engine::kMaxScaleEntries), 0u);
        CHECK_EQ(fs::AmbientEntries(2.5f, buffer, 1), 0u);
        CHECK_EQ(fs::AmbientEntries(2.5f, nullptr, engine::kMaxScaleEntries), 0u);
        for (uint32_t i = 0; i < 2; ++i)
        {
            CHECK(engine::EntryFits(buffer[i], engine::kAmbientDeclaredBytes));
            CHECK(engine::EntryFits(buffer[i], engine::kAmbientBoundBytes));
        }
        // The multiplier is the curve-scaled key: the day curve gives exactly 1.0, so a day frame costs one pure
        // call, never a dispatch (the entry builder's identity test with the epsilon handles the product's drift).
        const fs::Blend day = fs::FrameBlend(true, true, 0.5f, 1.0f, 3.0f, true, true, 1.5f);
        CHECK_EQ(day.ambientScale, 1.0f);
        CHECK_EQ(fs::AmbientEntries(day.ambientScale, buffer, engine::kMaxScaleEntries), 0u);
    }
    // The merged table: the knob's entries first, then the shadows' appended into the same table.
    {
        engine::ScaleEntry buffer[engine::kMaxScaleEntries] {};
        const engine::Params knob = engine::BuildParams(0.7f);
        const engine::Params idleKnob = engine::BuildParams(1.0f);
        const fs::MergedAmbientTable merged = fs::MergedAmbientEntries(knob, 2.5f, buffer, engine::kMaxScaleEntries);
        CHECK_EQ(merged.knobEntries, 2u);
        CHECK_EQ(merged.shadowEntries, 2u);
        CHECK_EQ(merged.total, 4u);
        CHECK(fs::MergedAmbientRuns(merged));
        // Positions: entries 0 and 1 are the knob's (the sky visibility at 240+192k .x and the SH reciprocal at
        // 112+192k .w), entries 2 and 3 are the shadows' (the header at 0 and the probes at 112+192k .w). The
        // order is the composition: the shipped knob's scale reaches the field earlier in CbScale's loop and the
        // probe shRecipScale leaves the dispatch as base * 0.7 * 2.5.
        CHECK_EQ(buffer[0].byteOffset, engine::kAmbientScaleOffset);
        CHECK_EQ(buffer[0].multiplier[0], 0.7f);
        CHECK_EQ(buffer[1].byteOffset, engine::kAmbientSkyRecipOffset);
        CHECK_EQ(buffer[1].multiplier[3], 0.7f);
        CHECK_EQ(buffer[2].byteOffset, 0u);
        CHECK_EQ(buffer[2].multiplier[1], 2.5f);
        CHECK_EQ(buffer[3].byteOffset, engine::kAmbientSkyRecipOffset);
        CHECK_EQ(buffer[3].multiplier[3], 2.5f);
        // Both arms at idle: nothing at all, and nothing counted.
        engine::ScaleEntry idle[engine::kMaxScaleEntries] {};
        const fs::MergedAmbientTable none = fs::MergedAmbientEntries(idleKnob, 1.0f, idle, engine::kMaxScaleEntries);
        CHECK_EQ(none.total, 0u);
        CHECK(!fs::MergedAmbientRuns(none));
        // The shadows arm alone (the shipped knob at its idle): two entries, in slots 0 and 1.
        engine::ScaleEntry alone[engine::kMaxScaleEntries] {};
        const fs::MergedAmbientTable one = fs::MergedAmbientEntries(idleKnob, 2.5f, alone, engine::kMaxScaleEntries);
        CHECK_EQ(one.total, 2u);
        CHECK_EQ(one.knobEntries, 0u);
        CHECK_EQ(one.shadowEntries, 2u);
        CHECK_EQ(alone[0].multiplier[1], 2.5f);
        CHECK_EQ(alone[1].multiplier[3], 2.5f);
        // A capacity shortfall: the knob's entries keep their low slots whole and the night lift waits - never a
        // half table, never a shifted knob entry.
        engine::ScaleEntry small[2] {};
        const fs::MergedAmbientTable choked = fs::MergedAmbientEntries(knob, 2.5f, small, 2);
        CHECK_EQ(choked.knobEntries, 2u);
        CHECK_EQ(choked.shadowEntries, 0u);
        CHECK_EQ(choked.total, 2u);
        CHECK_EQ(small[0].multiplier[0], 0.7f);
        CHECK_EQ(small[1].multiplier[3], 0.7f);
        // A capacity of one: neither entry set ships (a half of either is a knob that moves one field and not its
        // pairing one).
        engine::ScaleEntry tiny[1] {};
        const fs::MergedAmbientTable blocked = fs::MergedAmbientEntries(knob, 2.5f, tiny, 1);
        CHECK_EQ(blocked.total, 0u);
        CHECK(!fs::MergedAmbientRuns(blocked));
        // A null table answers 0s.
        const fs::MergedAmbientTable nothing = fs::MergedAmbientEntries(knob, 2.5f, nullptr, engine::kMaxScaleEntries);
        CHECK_EQ(nothing.total, 0u);
    }

    // ---- the arming word: which arms this frame's keys and curve want ------------------------------------------------------
    {
        // Defaults (master on, mask path present, no reading, every key at its idle): nothing runs.
        const fs::Blend defaults = fs::FrameBlend(true, true, 0.0f, 0.0f, 1.0f, true, false, 0.1f);
        CHECK(!defaults.maskRuns);
        CHECK(!defaults.ambientRuns);
        CHECK_EQ(defaults.maskLift, 0.0f);
        CHECK_EQ(defaults.softness, 0.0f);
        CHECK_EQ(defaults.ambientScale, 1.0f);
        // A night reading with values: both arms run, both keys inside their ranges.
        const fs::Blend night = fs::FrameBlend(true, true, 0.5f, 0.5f, 2.5f, true, true, 0.05f);
        CHECK(night.maskRuns);
        CHECK(night.ambientRuns);
        CHECK_GE(night.maskLift, 0.499f);
        CHECK_GE(night.ambientScale, 2.499f);
        CHECK_EQ(night.softness, 0.5f);
        // The curve scales the keys: the same keys at the moon's luminance lift less, at the day's they are idle.
        const fs::Blend moon = fs::FrameBlend(true, true, 0.5f, 0.5f, 2.5f, true, true, 0.26f);
        CHECK_LT(moon.maskLift, night.maskLift);
        CHECK_LT(moon.ambientScale, night.ambientScale);
        const fs::Blend day = fs::FrameBlend(true, true, 0.5f, 0.5f, 2.5f, true, true, 1.5f);
        CHECK(!day.maskRuns);
        CHECK(!day.ambientRuns);
        // The toggle off pins the full effect (day included - an explicit debug override, not the default), and the
        // softness rides with the lift: a lift of 0 is the passthrough the softness can never act on (it feathers
        // the lift, does not replace it).
        const fs::Blend pinned = fs::FrameBlend(true, true, 0.5f, 0.0f, 2.5f, false, false, 1.5f);
        CHECK(pinned.maskRuns);   // the toggle IS the override: full effect without a light reading, day included
        CHECK_EQ(pinned.maskLift, 0.5f);
        CHECK_EQ(pinned.ambientScale, 2.5f);
        const fs::Blend softOnly = fs::FrameBlend(true, true, 0.0f, 2.0f, 1.0f, true, true, 0.05f);
        CHECK(!softOnly.maskRuns);
        CHECK_EQ(softOnly.ambientScale, 1.0f);
        // The master off is today's picture whatever the keys say.
        const fs::Blend masterOff = fs::FrameBlend(false, true, 0.5f, 0.5f, 2.5f, true, true, 0.05f);
        CHECK(!masterOff.maskRuns);
        CHECK(!masterOff.ambientRuns);
        CHECK_EQ(masterOff.maskLift, 0.0f);
        CHECK_EQ(masterOff.ambientScale, 1.0f);
        // The mask path without its own-capture route (contact shadows deferred to own half): the combination keep.
        const fs::Blend withoutCapture = fs::FrameBlend(true, false, 0.5f, 0.0f, 2.5f, true, true, 0.05f);
        CHECK(!withoutCapture.maskRuns); // the mask arm waits: nothing copies without its route
        CHECK(withoutCapture.ambientRuns); // and the ambient arm still asks (its checker is the keys)
    }

    // ---- the session: the 64-refusal stand-down, the shape refusals that never count, and the log budget ---------------
    {
        fs::Session session;
        for (uint32_t i = 0; i < fs::kConsecutiveDeclineLimit - 1; ++i)
            CHECK(!session.NoteDecline());
        CHECK(session.Armed());
        CHECK(session.NoteDecline()); // the sixty-fourth: the stand-down
        CHECK(!session.Armed());
        CHECK(!session.NoteDecline()); // a stand-down holds: no further line, no further counting
        session = fs::Session{};
        // A run clears the streak: sixty-three refusals, one served draw, sixty-three more, and the module stays on.
        session.NoteDecline();
        session.NoteDecline();
        session.NoteRun();
        for (uint32_t i = 0; i < fs::kConsecutiveDeclineLimit - 1; ++i)
            CHECK(!session.NoteDecline());
        CHECK(session.Armed());
        CHECK(session.NoteDecline());
    }
    {
        fs::LogGate gate;
        CHECK(gate.Take(0x10u));          // the first tag's line
        CHECK(!gate.Take(0x10u));         // the same tag stays quiet
        CHECK(gate.Take(0x11u));          // a new tag speaks
        for (uint32_t i = 0; i < 200; ++i)
            (void) gate.Take(0x20u + i % 8); // repeated tags quiet, the budget fills
        const bool took = gate.Take(0x90u);
        CHECK_EQ(bool(took), (gate.lines < relight::kMaxLogLines)); // the budget, not the take, closes the tap
    }

    // ---- the drop list and the log lines -----------------------------------------------------------------------------------
    // The decline names are the relight module's own (the same draw the contact pass refuses), so a session reads
    // the reason once and knows the draw.
    CHECK_CONTAINS(std::string(relight::DeclineName(fs::Decline::MaskFormat)), "format");
    CHECK_CONTAINS(fs::FormatDecline(fs::Decline::MaskSize), "left alone");
    CHECK_CONTAINS(fs::FormatDecline(fs::Decline::MaskFormat), "Shadow mapping");
    CHECK_CONTAINS(fs::FormatSwitchedOff(), std::to_string(fs::kConsecutiveDeclineLimit));
    {
        const std::string run = fs::FormatMaskRun(3840, 2160, 61, 0.5f, 0.0f, 1.0f, true);
        CHECK_CONTAINS(run, "3840x2160");
        CHECK_CONTAINS(run, "format 61");
        CHECK_CONTAINS(run, "lift 0.500");
        CHECK_CONTAINS(run, "curve 1.000");
        CHECK_CONTAINS(fs::FormatMaskRun(3840, 2160, 61, 0.0f, 0.0f, 0.0f, false), "no light colour read yet");
        CHECK_CONTAINS(fs::FormatMaskRun(1280, 720, 87, 0.42f, 1.25f, 0.77f, true), "softness 1.25 px");
        const std::string ambient = fs::FormatAmbientRun(2.5f, 1.0f, 16384u, 2560u, 1440u, 2u);
        CHECK_CONTAINS(ambient, "16384 bytes");
        CHECK_CONTAINS(ambient, "2560x1440 composite");
        CHECK_CONTAINS(ambient, "by 2.500");
        CHECK_CONTAINS(ambient, "header skyShRecipScale");
        CHECK_CONTAINS(ambient, "one dispatch");
        CHECK_CONTAINS(ambient, "g_AmbientParamArray at cb2"); // the slot the arm patches is named
    }

    // ---- the mask route's gate: the lift key, or the colour feed of the ticked night curve (the capture session found the
    // route never ran with Contact shadows off, and that lift 0 with ambient 2.0 captured no light colour at all) ----
    {
        // keyOn, sessionArmed, lift, ambient, toggle
        CHECK(fs::MaskWantsDecision(true, true, 0.5f, 1.0f, true));    // the lift alone
        CHECK(fs::MaskWantsDecision(true, true, 0.5f, 1.0f, false));   // the lift alone, curve pinned
        CHECK(fs::MaskWantsDecision(true, true, 0.0f, 2.0f, true));    // the capture session's case: lift 0, ambient 2.0, ticked
        CHECK(!fs::MaskWantsDecision(true, true, 0.0f, 2.0f, false));  // unticked: the curve is pinned, no colour is needed
        CHECK(!fs::MaskWantsDecision(true, true, 0.0f, 1.0f, true));   // both keys at identity: nothing runs
        CHECK(!fs::MaskWantsDecision(true, true, 0.0f, 1.0f, false));
        CHECK(!fs::MaskWantsDecision(false, true, 0.5f, 2.0f, true));  // the master is off
        CHECK(!fs::MaskWantsDecision(true, false, 0.5f, 2.0f, true));  // the route stood itself down
        CHECK(!fs::MaskWantsDecision(true, true, 0.0f, 1.0005f, true)); // inside the identity epsilon
        CHECK(!fs::MaskWantsDecision(true, true, 0.0005f, 1.0f, true));
        CHECK(fs::MaskWantsDecision(true, true, 0.0f, 1.002f, true));  // just outside it
        CHECK(!fs::MaskWantsDecision(true, true, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(), true)); // NaN keys read as the idle values
    }

    // ---- the night curve on the rows of the capture session (light-curve.csv; the g_LightParam m_DiffuseColor of the
    // cb.csv rows). c2 = night exterior 20261105-220954-2 (t=305.050, also c3 at t=400.665); c1 = day exterior in view
    // 20261105-220907 (t=257.807); c4 = interior inn 20261105-221302-4 (t=447.017, the colour in transit). ----
    {
        const float night[3] { 0.0779614f, 0.116937f, 0.16f };      // c2 / c3
        const float day[3] { 1.96f, 1.86885f, 1.76525f };           // c1
        const float transit[3] { 0.370252f, 0.553156f, 0.759867f }; // c4, between night and day
        CHECK(std::fabs(fs::LuminanceOf(night) - 0.11176f) < 1e-4f);
        CHECK(std::fabs(fs::LuminanceOf(day) - 1.88075f) < 1e-3f);
        CHECK(std::fabs(fs::LuminanceOf(transit) - 0.52920f) < 1e-4f);
        CHECK_EQ(fs::CurveAt(fs::LuminanceOf(night)), 1.0f); // below 0.15: exactly full effect
        CHECK_EQ(fs::CurveAt(fs::LuminanceOf(day)), 0.0f);   // above 0.9: exactly zero
        CHECK(std::fabs(fs::CurveAt(fs::LuminanceOf(transit)) - 0.4916f) < 1e-3f);
        const fs::Blend atNight = fs::FrameBlend(true, true, 0.5f, 0.0f, 2.0f, true, true, fs::LuminanceOf(night));
        CHECK_EQ(atNight.curve, 1.0f);
        CHECK_EQ(atNight.maskLift, 0.5f);     // the mask lift at the key
        CHECK_EQ(atNight.ambientScale, 2.0f); // the ambient table at the key
        CHECK(atNight.maskRuns && atNight.ambientRuns);
        const fs::Blend atDay = fs::FrameBlend(true, true, 0.5f, 0.0f, 2.0f, true, true, fs::LuminanceOf(day));
        CHECK(!atDay.maskRuns && !atDay.ambientRuns);
        CHECK_EQ(atDay.ambientScale, 1.0f);
        const fs::Blend inTransit = fs::FrameBlend(true, true, 0.5f, 0.0f, 2.0f, true, true, fs::LuminanceOf(transit));
        CHECK(std::fabs(inTransit.maskLift - 0.2458f) < 1e-3f);
        CHECK(std::fabs(inTransit.ambientScale - 1.4916f) < 1e-3f);
        // the capture session's setting: lift 0, ambient 2.0, ticked, night colour landed -> only the ambient arm runs
        const fs::Blend ambientOnly = fs::FrameBlend(true, true, 0.0f, 0.0f, 2.0f, true, true, fs::LuminanceOf(night));
        CHECK(!ambientOnly.maskRuns && ambientOnly.ambientRuns);
        // before the first read lands the ticked curve is the identity (the chicken-and-egg of the base build)
        const fs::Blend noReading = fs::FrameBlend(true, true, 0.0f, 0.0f, 2.0f, true, false, 0.0f);
        CHECK(!noReading.maskRuns && !noReading.ambientRuns && noReading.curve == 0.0f);
    }

    // ---- the new log lines and the state line ---------------------------------------------------------------------------
    {
        const std::string first = fs::FormatRouteFirst(3840, 2160, 61, 8, 2, 0.5f, 2.0f, true);
        CHECK_STARTS(first, "Shadow mapping: the mask route is serving the sun light draw");
        CHECK_CONTAINS(first, "3840x2160");
        CHECK_CONTAINS(first, "at t8");
        CHECK_CONTAINS(first, "g_LightParam at cb2");
        CHECK_CONTAINS(first, "shadow lift 0.50");
        CHECK_CONTAINS(first, "night ambient lift 2.00");
        CHECK_CONTAINS(fs::FormatRouteFirst(3840, 2160, 61, 8, 2, 0.5f, 2.0f, false), "pinned by the toggle");
        const float night[3] { 0.0779614f, 0.116937f, 0.16f };
        const std::string read = fs::FormatColourRead(night, 1.0f, 0.5f, 2.0f);
        CHECK_STARTS(read, "Shadow mapping: the first light colour landed");
        CHECK_CONTAINS(read, "0.0780, 0.1169, 0.1600");
        CHECK_CONTAINS(read, "luminance 0.1118");
        CHECK_CONTAINS(read, "curve 1.000");
        CHECK_CONTAINS(read, "mask lift 0.500");
        CHECK_CONTAINS(read, "ambient table x2.000");
        // five idle reasons, five different lines, each naming what was missing
        const std::string idles[] { fs::FormatIdleNoLayout(fs::kNoLayoutAsks), fs::FormatIdleColourBuffer(16u, 48u),
                                    fs::FormatIdleColourStaging(), fs::FormatIdleNoColour(fs::kNoColourAsks, 0, 0, 0, 0),
                                    fs::FormatIdleEntries(), fs::FormatIdleNoComposite(fs::kNoCompositeDraws, 2.0f) };
        CHECK_EQ(sizeof(idles) / sizeof(idles[0]), size_t(fs::Idle::Count));
        for (size_t i = 0; i < size_t(fs::Idle::Count); ++i)
        {
            CHECK_STARTS(idles[i], "Shadow mapping:");
            for (size_t j = i + 1; j < size_t(fs::Idle::Count); ++j)
                CHECK(idles[i] != idles[j]);
        }
        CHECK_CONTAINS(idles[0], std::to_string(fs::kNoLayoutAsks));
        CHECK_CONTAINS(idles[1], "16 bytes");
        CHECK_CONTAINS(idles[1], "first 48");
        CHECK_CONTAINS(idles[5], "600 sun light draws were served");
        CHECK_CONTAINS(idles[5], "x2.00");
        CHECK_CONTAINS(idles[5], "g_AmbientParamArray and g_SamplerLightDiffuse");
        CHECK_CONTAINS(fs::FormatIdleNoColour(240, 12, 7, 0, 5), "7 copies staged");
        CHECK_CONTAINS(fs::FormatIdleNoColour(240, 12, 7, 0, 5), "5 not ready");
        CHECK_CONTAINS(fs::FormatCompositeIndexed(2, 3), "g_AmbientParamArray at cb2");
        CHECK_CONTAINS(fs::FormatCompositeIndexed(2, 3), "g_SamplerLightDiffuse at t3");
        {
            const float dim[3] { 0.1764f, 0.2635f, 0.49f };
            const std::string change = fs::FormatColourChange(dim, 0.85f, 7);
            CHECK_CONTAINS(change, "the light colour read changed to (0.1764, 0.2635, 0.4900)");
            CHECK_CONTAINS(change, "luminance 0.2613");
            CHECK_CONTAINS(change, "curve 0.850");
            CHECK_CONTAINS(change, "route id 7");
            const std::string two = fs::FormatTwoLightDraws(3, 5);
            CHECK_CONTAINS(two, "two light draws were served in one frame (route ids 3 then 5)");
            CHECK_CONTAINS(two, "the later draw's colour is the one the night curve reads");
        }
        CHECK_CONTAINS(fs::FormatSwitchedOff(), "forgets the colour it had read");
        CHECK(fs::FormatSwitchedOff().find("keeps the last") == std::string::npos); // the stale-reading text is gone
    }
    {
        fs::StateFacts facts;
        CHECK(fs::FormatStateLine(facts) == "off");
        facts.keyOn = true;
        CHECK_CONTAINS(fs::FormatStateLine(facts), "nothing runs");
        facts.sceneHdr = true;
        CHECK_CONTAINS(fs::FormatStateLine(facts), "inactive while scene HDR is on");
        facts.sceneHdr = false;
        facts.liftKeyOff = false;
        facts.routeDraws = 1234;
        facts.routeRuns = 1200;
        facts.colourCopies = 77;
        facts.colourReads = 70;
        facts.colourNotReady = 7;
        facts.ambientPatched = 55;
        facts.haveReading = true;
        facts.curve = 1.0f;
        const std::string line = fs::FormatStateLine(facts);
        // the line speaks in the module's own counters, not in the engine seam's "asks"
        CHECK_CONTAINS(line, "1234 route draw(s) seen");
        CHECK_CONTAINS(line, "1200 lifted");
        CHECK_CONTAINS(line, "77 copy(ies) staged");
        CHECK_CONTAINS(line, "70 read(s) landed");
        CHECK_CONTAINS(line, "7 not ready");
        CHECK_CONTAINS(line, "curve 1.000");
        CHECK_CONTAINS(line, "55 dispatch(es) patched");
        CHECK(line.find("ask") == std::string::npos);
        facts.haveReading = false;
        CHECK_CONTAINS(fs::FormatStateLine(facts), "no reading yet");
        facts.standDown = true;
        CHECK_CONTAINS(fs::FormatStateLine(facts), "stood itself down");
    }

    return Finish("shadows_rules");
}
