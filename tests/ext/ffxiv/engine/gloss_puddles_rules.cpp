// Puddles rules (ext/ffxiv/engine/GlossPuddlesRules.h): every decision the pass makes without a device.
//
// What is pinned here is what a wrong rule would break in a session: the keys and their clamps (a broken value never turns the pass on), the
// identity rule that the switch off, a coverage of zero, a target this pass cannot shape and the material pack's selector channel are all today's
// picture, the packed word a shader is stamped with (a missing slot is 0xFF, and the word is 0 only when nothing was read), the channel and target
// keys, the wetness the gate follows (the wetness knob's forced amount while it is armed, the engine's own amount otherwise), the noise
// (deterministic, inside 0..1, anchored to the world, and periodic at its lattice), the formula (a dry frame is the engine's value to the bit, a
// wall is never shaped, and the puddle factor really does follow the engine's own wetness ramp), the region of the target that lies inside the
// render window (the engine's half-pixel origin, an offset window, a window at the allocation's size, a window smaller than one texel), the
// validation one reason at a time, the per-frame memory (one attempt a frame, the engine's texture matched by resource, the next frame forgetting
// it), the sixty-four-refusal switch-off with its own streak, the log gate's budget, and the shader's 80-byte constant block.
#include "check.h"

#include "ext/ffxiv/engine/GlossPuddlesRules.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace ext::ffxiv::engine::gloss;
namespace engine = ext::ffxiv::engine; // the shared word's own types and limits, next to the module's

namespace
{
float NaN() { return std::numeric_limits<float>::quiet_NaN(); }

// The facts of a reader that can be served, which every validation case below breaks one field at a time.
ReaderFacts GoodFacts()
{
    ReaderFacts facts;
    facts.hasTarget = true;
    facts.hasDepth = true;
    facts.hasNormal = true;
    facts.hasCamera = true;
    facts.targetFormat = kTargetFormat;
    facts.targetWidth = 3840;
    facts.targetHeight = 2160;
    facts.targetSamples = 1;
    facts.depthFormat = 46; // DXGI_FORMAT_R24_UNORM_X8_TYPELESS
    facts.depthWidth = 3840;
    facts.depthHeight = 2160;
    facts.depthSamples = 1;
    facts.normalWidth = 3840;
    facts.normalHeight = 2160;
    facts.normalSamples = 1;
    facts.cameraBytes = kCameraBytes;
    facts.window[0] = 0.5f;
    facts.window[1] = 0.5f;
    facts.window[2] = 1920.0f;
    facts.window[3] = 1080.0f;
    return facts;
}
} // namespace

// The slot parse, on a real reflection table and on the tables a capture cannot show: only a texture binding is a slot this module reads, and a
// name that only a sampler carries is not a reader at all.
namespace
{
// The reflection of the pixel shader one captured exterior frame reads its G-buffer with first (the cascade draw that resolves the sun's
// screen-space mask): the effect compiler writes the four samplers before the four textures, so every name that matters appears twice, first at
// the sampler's slot. The blob is the game's own bytecode, kept as a test fixture.
const char* kReaderFixture = "/tests/ext/ffxiv/engine/fixtures/reader_0x1257B24B.dxbc";

std::string ReadFixture(const std::string& repo, const char* relative)
{
    std::ifstream file(repo + relative, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// The value of `static const float <name> = <number>;` in a shader's text, or NaN when it is not declared there.
float ShaderConstant(const std::string& text, const char* name)
{
    const std::string key = std::string("static const float ") + name + " =";
    const size_t at = text.find(key);
    if (at == std::string::npos)
        return std::numeric_limits<float>::quiet_NaN();
    return std::strtof(text.c_str() + at + key.size(), nullptr);
}

// One entry of a hand-built reflection table, with the name's length taken from the literal so a typo in a count cannot pass.
ext::ffxiv::diag::ShaderBinding Bound(const char* name, uint32_t type, uint32_t slot)
{
    return ext::ffxiv::diag::ShaderBinding { name, uint32_t(std::string_view(name).size()), type, slot, 1 };
}
} // namespace

int main(int argc, char** argv)
{
    // ---- the names: the four G-buffer rows of the shared table, and the anchors bit the index sets for a reader
    {
        CHECK_EQ(engine::IndexOfName("g_SamplerGBuffer1"), 7);
        CHECK_EQ(engine::IndexOfName("g_SamplerGBuffer2"), 8);
        CHECK_EQ(engine::IndexOfName("g_SamplerGBuffer3"), 9);
        CHECK_EQ(engine::IndexOfName("g_SamplerGBuffer"), 10); // G-buffer 0: the primary normal, whose name carries no number
        CHECK(engine::kNames[7].kind == engine::NameKind::Texture && engine::kNames[10].kind == engine::NameKind::Texture);
        CHECK_EQ(engine::IndexOfName("g_SamplerGBuffer0"), -1);
        CHECK(IsGlossAnchors(engine::kGlossReaderBit));
        CHECK(!IsGlossAnchors(engine::kOcclusionReaderBit));
        CHECK(!IsGlossAnchors(0));
        // The map's own fields stay disjoint with the puddles bit in it.
        CHECK(engine::AnchorsMapIsSound());
        CHECK_EQ(engine::kGlossReaderBit & engine::kOcclusionReaderSlotMask, engine::Anchors(0));
        // The shared word carries the reader bit alone: a reader declares up to four targets, which one slot field cannot hold.
        engine::PassLayout layout;
        layout.found = 1u << 7;
        layout.slots[7] = 4;
        const engine::Anchors anchors = engine::DrawAnchors(layout);
        CHECK_EQ(anchors & engine::kGlossReaderBit, engine::kGlossReaderBit);
        CHECK_EQ(anchors & engine::kGlossReaderSlotMask, engine::Anchors(0));
        CHECK_EQ(engine::DrawAnchors(engine::PassLayout {}), engine::Anchors(0));
    }

    // ---- the stamp: slot + 1 per field, a missing slot is 0xFF, and the word of an unstamped shader is 0
    {
        Inputs inputs;
        CHECK(!inputs.HasDepth() && !inputs.HasCamera() && !inputs.HasTarget(0) && !inputs.Complete(0));
        CHECK_EQ(PackInputs(inputs), 0ull);

        inputs.depth = 0;
        inputs.camera = 1;
        inputs.targets[0] = 4;
        inputs.targets[1] = 5;
        inputs.targets[2] = 6;
        inputs.targets[3] = 7;
        const uint64_t packed = PackInputs(inputs);
        CHECK(packed != 0);
        const Inputs back = UnpackInputs(packed);
        CHECK_EQ(uint32_t(back.depth), 0u);
        CHECK_EQ(uint32_t(back.camera), 1u);
        CHECK_EQ(uint32_t(back.targets[0]), 4u);
        CHECK_EQ(uint32_t(back.targets[3]), 7u);
        // Every field stays in its own byte: a slot of 127 does not reach the next one.
        Inputs wide;
        wide.depth = 127;
        wide.camera = 13;
        wide.targets[0] = 127;
        wide.targets[1] = 127;
        wide.targets[2] = 127;
        wide.targets[3] = 127;
        const Inputs wideBack = UnpackInputs(PackInputs(wide));
        CHECK_EQ(uint32_t(wideBack.depth), 127u);
        CHECK_EQ(uint32_t(wideBack.camera), 13u);
        for (uint32_t t = 0; t < kTargetCount; ++t)
            CHECK_EQ(uint32_t(wideBack.targets[t]), 127u);
        // A camera slot past the 14 D3D11 exposes is not a camera table this shader can bind.
        Inputs highCamera;
        highCamera.camera = 14;
        CHECK(!highCamera.HasCamera());
        // Complete: the target the key names, the normal (target 4), the depth and the camera, all four.
        Inputs partial = inputs;
        partial.depth = kSlotMissing;
        CHECK(!partial.Complete(0));
        partial = inputs;
        partial.camera = kSlotMissing;
        CHECK(!partial.Complete(0));
        partial = inputs;
        partial.targets[3] = kSlotMissing;
        CHECK(!partial.Complete(0));
        partial = inputs;
        partial.targets[0] = kSlotMissing;
        CHECK(!partial.Complete(0));
        CHECK(partial.Complete(1)); // the target key names another one, and that one is declared
        CHECK(!partial.Complete(7)); // an index outside the four
    }

    // ---- the keys: their defaults, their ranges, and the broken value that must not turn the pass on
    {
        const Params defaults = BuildParams(true);
        CHECK(defaults.on);
        // The three shape defaults are the values the field was measured at, over one full 64-cell period of it: about a third of the up-facing
        // ground in pools, a median blob of 4.3 m and a 0.81 m edge (the numbers are in the note).
        CHECK(std::fabs(defaults.coverage - 0.42f) < 1e-6f);
        CHECK(std::fabs(defaults.scale - 6.0f) < 1e-6f);
        CHECK(kMaxScale >= 16.0f); // the cap the session hit was five metres, and the reference's largest pool is wider than that
        CHECK(std::fabs(defaults.softness - 0.03f) < 1e-6f);
        CHECK_EQ(defaults.target, 1u);
        CHECK(defaults.channel == Channel::A); // the material pack's gloss, the channel the engine's composite reads
        CHECK(std::fabs(defaults.wet - 1.0f) < 1e-6f);
        // The struct's member defaults are a second place the default lives, and an aggregate built without naming a channel reads them.
        const Params aggregate {};
        CHECK(aggregate.channel == Channel::A);
        CHECK_EQ(aggregate.target, kTargetDefault);
        CHECK(aggregate.coverage > 0.0f); // a value built this way still has a coverage, which is why the switch is the gate
        CHECK(IsIdentity(aggregate));     // and it is today's picture until the switch says otherwise

        CHECK(std::fabs(BuildParams(true, 4.0f).coverage - kMaxCoverage) < 1e-6f);
        CHECK(std::fabs(BuildParams(true, -3.0f).coverage - kMinCoverage) < 1e-6f);
        // A NaN coverage is today's picture, not the default: a broken ini value never turns the feature on.
        CHECK(std::fabs(BuildParams(true, NaN()).coverage - kMinCoverage) < 1e-6f);
        CHECK(IsIdentity(BuildParams(true, NaN())));
        CHECK(std::fabs(BuildParams(true, 0.5f, 0.0f).scale - kMinScale) < 1e-6f);
        CHECK(std::fabs(BuildParams(true, 0.5f, 99.0f).scale - kMaxScale) < 1e-6f);
        CHECK(std::fabs(BuildParams(true, 0.5f, NaN()).scale - kScaleDefault) < 1e-6f);
        CHECK(std::fabs(BuildParams(true, 0.5f, 1.0f, 9.0f).softness - kMaxSoftness) < 1e-6f);
        CHECK(std::fabs(BuildParams(true, 0.5f, 1.0f, NaN()).softness - kSoftnessDefault) < 1e-6f);
        CHECK(std::fabs(BuildParams(true, 0.5f, 1.0f, 0.1f, 1, Channel::R, NaN()).wet - kWetDefault) < 1e-6f);

        // The target key: 1..4, and anything else reads as the default rather than as a target the pass would have to guess.
        CHECK_EQ(ClampTarget(1), 1u);
        CHECK_EQ(ClampTarget(4), 4u);
        CHECK_EQ(ClampTarget(0), kTargetDefault);
        CHECK_EQ(ClampTarget(5), kTargetDefault);
        CHECK_EQ(ClampTarget(-2), kTargetDefault);
        CHECK_EQ(TargetIndex(BuildParams(true, 0.5f, 1.0f, 0.1f, 1)), 0u);
        CHECK_EQ(TargetIndex(BuildParams(true, 0.5f, 1.0f, 0.1f, 4)), 3u);
        CHECK_EQ(TargetIndex(BuildParams(true, 0.5f, 1.0f, 0.1f, 0)), 0u); // the key's own clamp already made it the default

        // The channel key: r, g, b, a, and anything else reads as the default (`a`), so a typo in the ini shapes the channel the pass ships with
        // rather than a value the pass would have to refuse (the material pack's red is that one refused pair).
        CHECK(ParseChannel("r") == Channel::R);
        CHECK(ParseChannel("g") == Channel::G);
        CHECK(ParseChannel("b") == Channel::B);
        CHECK(ParseChannel("a") == Channel::A);
        CHECK(ParseChannel("") == Channel::A);
        CHECK(ParseChannel("red") == Channel::A);
        CHECK(ParseChannel("R") == Channel::A); // the ini's spelling is lower case; an upper case letter is not one of the four
        CHECK(ParseChannel("x") == Channel::A);
        CHECK(std::string(ChannelName(Channel::R)) == "r");
        CHECK(std::string(ChannelName(Channel::G)) == "g");
        CHECK(std::string(ChannelName(Channel::B)) == "b");
        CHECK(std::string(ChannelName(Channel::A)) == "a");

        // The feature bit: off is off, and the coverage decides while the switch is on.
        CHECK(!Wants(BuildParams(false)));
        CHECK(!Wants(BuildParams(false, 0.9f)));
        CHECK(Wants(BuildParams(true, 0.9f)));
        CHECK(!Wants(BuildParams(true, 0.0f)));
        CHECK(IsIdentity(BuildParams(true, 0.0f)));
        CHECK(!IsIdentity(BuildParams(true, 0.001f)));

        // One number per tuning, never 0, and every key moves it: the frame memory clears when a slider moves.
        const uint32_t base = EpochOf(BuildParams(true));
        CHECK(base != 0);
        CHECK_EQ(EpochOf(BuildParams(true)), base);
        CHECK(EpochOf(BuildParams(true, 0.6f)) != base);
        CHECK(EpochOf(BuildParams(true, 0.5f, 1.5f)) != base);
        CHECK(EpochOf(BuildParams(true, 0.5f, 1.0f, 0.2f)) != base);
        CHECK(EpochOf(BuildParams(true, 0.5f, 1.0f, 0.1f, 2)) != base);
        CHECK(EpochOf(BuildParams(true, 0.5f, 1.0f, 0.1f, 1, Channel::G)) != base);
        CHECK(EpochOf(BuildParams(true, 0.5f, 1.0f, 0.1f, 1, Channel::R, 0.4f)) != base);
        CHECK(EpochOf(BuildParams(false)) != base);
    }

    // ---- the noise: deterministic, inside 0..1, anchored to the world, and periodic at its lattice
    {
        CHECK_EQ(HashOf(0, 0), HashOf(0, 0));
        CHECK(HashOf(1, 0) != HashOf(0, 0));
        CHECK(HashOf(0, 1) != HashOf(0, 0));
        for (int32_t x = -70; x <= 70; x += 7)
            for (int32_t y = -70; y <= 70; y += 7)
            {
                const float value = LatticeValue(x, y);
                CHECK(value >= 0.0f && value <= 1.0f);
            }
        // The value noise interpolates between the lattice values, so it stays inside 0..1 and a lattice point answers its own value.
        CHECK(std::fabs(ValueNoise(3.0f, 5.0f, kNoisePeriod) - LatticeValue(3, 5)) < 1e-6f);
        CHECK(std::fabs(ValueNoise(0.0f, 0.0f, kNoisePeriod) - LatticeValue(0, 0)) < 1e-6f);
        // The floor is the shader's floor, negatives included.
        CHECK(std::fabs(FloorOf(2.75f) - 2.0f) < 1e-6f);
        CHECK(std::fabs(FloorOf(-2.25f) + 3.0f) < 1e-6f);
        CHECK(std::fabs(FloorOf(-3.0f) + 3.0f) < 1e-6f);
        // The field: the same world point always gives the same number, and the scale only changes which points are neighbours.
        for (float x = -12.5f; x <= 12.5f; x += 2.5f)
            for (float z = -12.5f; z <= 12.5f; z += 2.5f)
            {
                const float value = Noise(x, z, 1.0f);
                CHECK(value >= 0.0f && value <= 1.0f);
                CHECK_EQ(Noise(x, z, 1.0f), value);
            }
        CHECK(Noise(0.25f, 0.75f, 1.0f) != Noise(0.25f, 0.75f, 2.0f));
        // The lattice wraps: one period of cells at scale 1 is the same field again.
        CHECK(std::fabs(Noise(3.7f, 11.3f, 1.0f) - Noise(3.7f + float(kNoisePeriod), 11.3f, 1.0f)) < 1e-3f);
        // A scale below the key's floor is read at the floor rather than dividing by zero.
        CHECK_EQ(Noise(1.0f, 2.0f, 0.0f), Noise(1.0f, 2.0f, kMinScale));
        // The smoothstep, both ends and the middle, and the width the key is given.
        CHECK_EQ(Smoothstep(0.4f, 0.6f, 0.3f), 0.0f);
        CHECK_EQ(Smoothstep(0.4f, 0.6f, 0.7f), 1.0f);
        CHECK(std::fabs(Smoothstep(0.4f, 0.6f, 0.5f) - 0.5f) < 1e-6f);
        CHECK(std::fabs(EdgeWidth(0.0f) - kSoftnessFloor) < 1e-9f);
        CHECK(std::fabs(EdgeWidth(0.2f) - 0.2f) < 1e-6f);
    }

    // ---- the formula: the threshold follows the engine's wetness, and a dry frame is the engine's value to the bit
    {
        CHECK(std::fabs(Threshold(0.5f, 1.0f) - 0.5f) < 1e-6f);
        CHECK(std::fabs(Threshold(0.5f, 0.0f) - 1.0f) < 1e-6f);
        CHECK(std::fabs(Threshold(0.0f, 1.0f) - 1.0f) < 1e-6f);
        CHECK(std::fabs(Threshold(1.0f, 1.0f) - 0.0f) < 1e-6f);
        // A factor of 1 above the edge, 0 below it, and the middle of the smoothstep between.
        CHECK_EQ(PuddleFactor(0.9f, 0.5f, 1.0f, 0.1f), 1.0f);
        CHECK_EQ(PuddleFactor(0.1f, 0.5f, 1.0f, 0.1f), 0.0f);
        CHECK(std::fabs(PuddleFactor(0.5f, 0.5f, 1.0f, 0.1f) - 0.5f) < 1e-6f);
        // At full rain with a coverage of 1 the whole field is puddle; with a coverage of 0 none of it is (the level sits at 1, above every
        // value the field can take).
        CHECK_EQ(PuddleFactor(0.5f, 1.0f, 1.0f, 0.1f), 1.0f);
        CHECK_EQ(PuddleFactor(0.0f, 0.0f, 1.0f, 0.1f), 0.0f);
        // A softness of zero is a hard edge, not a NaN: a value on the level is exactly half way.
        CHECK(std::fabs(PuddleFactor(0.5f, 0.5f, 1.0f, 0.0f) - 0.5f) < 1e-4f);

        // The shaped channel: the engine's own value when nothing applies, the wet value when everything does.
        CHECK_EQ(GlossValue(0.25f, 1.0f, 0.0f, 1.0f), 0.25f);  // dry: the engine's value to the bit
        CHECK_EQ(GlossValue(0.25f, 0.0f, 1.0f, 1.0f), 0.25f);  // no puddle here
        CHECK_EQ(GlossValue(0.25f, 1.0f, 1.0f, 1.0f), 1.0f);   // a puddle at full rain: the wet value
        CHECK(std::fabs(GlossValue(0.25f, 1.0f, 0.5f, 1.0f) - 0.625f) < 1e-6f); // half the ramp
        CHECK_EQ(GlossValue(0.75f, 1.0f, 1.0f, 0.0f), 0.0f);   // the other way round: the roughness is the channel
        // A NaN wetness reads as no wetness, and a factor outside 0..1 is clipped.
        CHECK_EQ(GlossValue(0.25f, 1.0f, NaN(), 1.0f), 0.25f);
        CHECK_EQ(GlossValue(0.25f, 3.0f, 1.0f, 1.0f), 1.0f);

        // The wetness the gate follows: the knob's forced amount whenever the knob asks for one (0.0 and up), the engine's own amount while the
        // knob is off (below zero). The knob is what the engine's own draws read while it is armed, so this is the whole reason a forced-wet frame
        // shapes at all.
        CHECK_EQ(WetnessFor(0.0f, 1.0f), 1.0f);
        CHECK_EQ(WetnessFor(0.0f, 0.0f), 0.0f);
        CHECK_EQ(WetnessFor(0.83f, -1.0f), 0.83f);
        CHECK_EQ(WetnessFor(0.4f, -1.0f), 0.4f);
        CHECK_EQ(WetnessFor(0.4f, 0.6f), 0.6f);
        CHECK_EQ(WetnessFor(0.4f, NaN()), 0.4f); // a NaN knob is not an armed knob
        // The wetness knob only applies while the engine pass overrides switch is on; with that switch off it reads as off (-1.0).
        CHECK_EQ(WetnessKnobFor(false, 1.0f), -1.0f);
        CHECK_EQ(WetnessKnobFor(false, 0.0f), -1.0f);
        CHECK_EQ(WetnessKnobFor(true, 1.0f), 1.0f);
        CHECK_EQ(WetnessKnobFor(true, -1.0f), -1.0f);
        CHECK_EQ(WetnessKnobFor(true, NaN()), -1.0f);
        // The dry-frame skip rule shared by both stages: only a known wetness above the floor runs the pass.
        CHECK(!WetEnough(false, 1.0f));
        CHECK(!WetEnough(true, 0.0f));
        CHECK(!WetEnough(true, kWetnessFloor));
        CHECK(WetEnough(true, kWetnessFloor + 1e-4f));
        CHECK(WetEnough(true, 1.0f));

        // The dwell: once a reading lands above the floor the gate stays armed until an exact 0.0, so a stale-low
        // read-back at the ramp's crossing cannot flip the stage off for one frame and back on the next.
        CHECK(!WetEnoughDwell(false, false, 0.5f));       // nothing read yet: the fresh answer is the gate's own
        CHECK(!WetEnoughDwell(false, true, 0.0f));        // a dry floor
        CHECK(WetEnoughDwell(false, true, kWetnessFloor + 1e-4f)); // the fresh answer agrees with WetEnough
        CHECK(WetEnoughDwell(true, true, kWetnessFloor)); // latched: below the floor but above zero stays armed
        CHECK(WetEnoughDwell(true, false, 0.5f));         // latched: a frame without a reading stays armed
        CHECK(!WetEnoughDwell(true, true, 0.0f));          // latched: an exact 0.0 unlatches (a fresh dry)
        CHECK(!WetEnoughDwell(true, false, 0.0f));        // and so does an exact zero on a frame without a reading
        CHECK(!WetEnoughDwell(true, true, std::nanf(""))); // a broken reading fails open: the engine's picture, not a held gate

        // ---- the frame's two answers (the pairing and the hold): what a ramp skip shows, and what a refusal keeps ----
        // The tone-map stage's own rule. A wet frame runs the pass when the frame's composite carries the gloss stage's
        // shape, holds the last good answer when it does not, and answers nothing only when it has no last answer -
        // which is the ramp's first frame, whose composite the gloss stage did not shape either (both stages' misses
        // are the same clean answer, and the pools' first look is not a darkened engine picture). A gloss side that
        // can no longer shape at all (an unservable target/channel key, the card disarmed by its own identity) ends
        // the pairing: the answer is the engine's picture from that frame on, never the frozen pre-change look.
        CHECK(ToneAnswerFor(false, false, true, false) == ToneAnswer::None);
        CHECK(ToneAnswerFor(false, true, true, true) == ToneAnswer::None);   // the wetness gate answers None for a dry frame regardless
        CHECK(ToneAnswerFor(true, true, true, false) == ToneAnswer::Run);    // the frame's own shape: the pass runs
        CHECK(ToneAnswerFor(true, true, true, true) == ToneAnswer::Run);     // the shape outranks the hold
        CHECK(ToneAnswerFor(true, false, true, true) == ToneAnswer::Hold);   // a mid-ramp skip holds the last answer instead of the engine's picture
        CHECK(ToneAnswerFor(true, false, true, false) == ToneAnswer::None);  // the ramp's first frame answers nothing (clean, like the composite's own)
        CHECK(ToneAnswerFor(true, false, false, true) == ToneAnswer::None);  // a gloss side that cannot shape any more: the engine's picture, not the frozen hold
        CHECK(ToneAnswerFor(true, true, false, false) == ToneAnswer::None);  // ...shape or not: a dead gloss side ends the pairing
        // The shape stamp's own semantics (the pure rule the pass's ShapePresent forwards): a stamp answers for its own
        // frame only, so a token from a previous frame - a skipped attempt between stamps - reads as absent.
        CHECK(ShapePresentOnFrame(true, 7, 7));   // the composite of frame 7 reads the frame-7 shape
        CHECK(!ShapePresentOnFrame(true, 7, 8));  // a frame-7 stamp says nothing about frame 8: the attempt was skipped
        CHECK(!ShapePresentOnFrame(false, 7, 7)); // and a stamp that says "the attempt did not shape" is absent for its own frame too
        // The hold's binding test: the object the shape was computed for, by its view first and its resource beside that.
        const int engineA = 0, viewA = 1, engineB = 2, viewB = 3;
        CHECK(AnswersSameBinding(&engineA, &viewA, nullptr, &viewA));    // the same view object answers
        CHECK(AnswersSameBinding(&engineA, &viewA, &engineA, &viewB));   // a second view of the same resource answers
        CHECK(AnswersSameBinding(&engineA, nullptr, &engineA, &viewB));  // a hold with no view recorded (a resource holds)
        CHECK(!AnswersSameBinding(&engineA, &viewA, &engineB, &viewB));  // a different resource is not the hold's binding
        CHECK(!AnswersSameBinding(&engineA, &viewA, nullptr, &viewB));   // a different view and nothing behind it answers nothing
        CHECK(!AnswersSameBinding(nullptr, nullptr, &engineA, &viewB));  // a hold the pass never recorded answers nothing
        // The hold's own gates: the wetness latched, an output the pass owns, and a camera the layout check has not refused.
        CHECK(HoldAnswersShape(true, true, true));     // held, a whole answer, a servable camera: the hold answers
        CHECK(!HoldAnswersShape(false, true, true));   // a dry frame: the engine's picture has no pools, and the hold would keep them
        CHECK(!HoldAnswersShape(true, false, true));   // no shape the pass owns: with nothing to hold, the engine's picture stands
        CHECK(!HoldAnswersShape(true, true, false));   // a camera the layout refuses is another scene: the hold waits for its retry
        // And it reaches the formula: a texel that is a puddle at full rain stays the engine's texel when WetnessFor says dry.
        CHECK_EQ(ShapedChannel(0.3f, kGateK, true, true, 1.0f, 2.5f, 7.5f, WetnessFor(0.0f, 1.0f), BuildParams(true, 1.0f)), 1.0f);
        CHECK_EQ(ShapedChannel(0.3f, kGateK, true, true, 1.0f, 2.5f, 7.5f, WetnessFor(0.0f, -1.0f), BuildParams(true, 1.0f)), 0.3f);

        // The whole per-texel rule, in the order the shader applies it.
        const Params params = BuildParams(true, 1.0f, 1.0f, 0.1f); // coverage 1: every up-facing texel at full rain is puddle
        CHECK_EQ(ShapedChannel(0.3f, kGateK, true, true, 1.0f, 2.5f, 7.5f, 1.0f, params), 1.0f);
        CHECK_EQ(ShapedChannel(0.3f, kGateK, true, true, 1.0f, 2.5f, 7.5f, 0.0f, params), 0.3f); // dry
        CHECK_EQ(ShapedChannel(0.3f, kGateK, false, true, 1.0f, 2.5f, 7.5f, 1.0f, params), 0.3f); // outside the render window
        CHECK_EQ(ShapedChannel(0.3f, kGateK, true, false, 1.0f, 2.5f, 7.5f, 1.0f, params), 0.3f); // no surface
        CHECK_EQ(ShapedChannel(0.3f, kGateK, true, true, 0.0f, 2.5f, 7.5f, 1.0f, params), 0.3f);  // a wall
        CHECK_EQ(ShapedChannel(0.3f, kGateK, true, true, kUpCos, 2.5f, 7.5f, 1.0f, params), 0.3f); // exactly at the limit is not up
        CHECK_EQ(ShapedChannel(0.3f, kGateK, true, true, 1.0f, 2.5f, 7.5f, 1.0f, BuildParams(true, 0.0f)), 0.3f); // no coverage
        // The up-facing test itself: FFXIV is Y-up, and sixty degrees is the limit.
        CHECK(FacesUp(1.0f));
        CHECK(!FacesUp(0.0f));
        CHECK(!FacesUp(-1.0f));
        CHECK(FacesUp(0.51f));
    }

    // ---- the region of the target inside the render window
    {
        // The whole allocation: every texel is inside.
        ReaderFacts facts = GoodFacts();
        facts.targetWidth = facts.targetHeight = 64;
        facts.depthWidth = facts.depthHeight = 64;
        facts.normalWidth = facts.normalHeight = 64;
        facts.window[0] = facts.window[1] = 0.0f;
        facts.window[2] = facts.window[3] = 64.0f;
        Region region = RegionOf(facts);
        CHECK_EQ(region.x, 0u);
        CHECK_EQ(region.y, 0u);
        CHECK_EQ(region.width, 64u);
        CHECK_EQ(region.height, 64u);
        CHECK(!region.Empty());
        CHECK_EQ(region.Right(), 64u);
        CHECK_EQ(region.Bottom(), 64u);

        // The engine's half-pixel origin: the first texel of the window is texel 1, and the last one is texel 63.
        facts.window[0] = facts.window[1] = 0.5f;
        facts.window[2] = facts.window[3] = 64.0f;
        region = RegionOf(facts);
        CHECK_EQ(region.x, 1u);
        CHECK_EQ(region.y, 1u);
        CHECK_EQ(region.width, 63u);
        CHECK_EQ(region.height, 63u);

        // An offset window inside a larger allocation: exactly its own texels.
        facts.targetWidth = facts.targetHeight = 64;
        facts.window[0] = 10.0f;
        facts.window[1] = 20.0f;
        facts.window[2] = 24.0f;
        facts.window[3] = 8.0f;
        region = RegionOf(facts);
        CHECK_EQ(region.x, 10u);
        CHECK_EQ(region.y, 20u);
        CHECK_EQ(region.width, 24u);
        CHECK_EQ(region.height, 8u);

        // A window that runs past the allocation is clamped, and one that is smaller than a texel covers nothing.
        facts.window[0] = 60.0f;
        facts.window[1] = 60.0f;
        facts.window[2] = 100.0f;
        facts.window[3] = 100.0f;
        region = RegionOf(facts);
        CHECK_EQ(region.x, 60u);
        CHECK_EQ(region.width, 4u);
        CHECK_EQ(region.height, 4u);
        facts.window[0] = 3.2f;
        facts.window[1] = 3.2f;
        facts.window[2] = 0.5f;
        facts.window[3] = 0.5f;
        CHECK(RegionOf(facts).Empty());
        // A target of no size has no region at all (the pass never sees one, but the rule is the rule).
        facts.targetWidth = 0;
        CHECK(RegionOf(facts).Empty());
    }

    // ---- the validation: one reason at a time, and the whole shape of the session's own reader
    {
        CHECK(Classify(GoodFacts()) == Decline::None);
        // The session's shape: 1920x1080 at 0.5,0.5 inside a 3840x2160 allocation.
        const Region region = RegionOf(GoodFacts());
        CHECK_EQ(region.x, 1u);
        CHECK_EQ(region.y, 1u);
        CHECK_EQ(region.width, 1919u);
        CHECK_EQ(region.height, 1079u);

        ReaderFacts facts = GoodFacts();
        facts.replay = true;
        CHECK(Classify(facts) == Decline::Replay);

        facts = GoodFacts();
        facts.hasTarget = false;
        CHECK(Classify(facts) == Decline::NoTarget);

        facts = GoodFacts();
        facts.targetFormat = 10; // R16G16B16A16_FLOAT: the normal's own target, which this pass cannot write
        CHECK(Classify(facts) == Decline::TargetShape);
        facts = GoodFacts();
        facts.targetWidth = 128;
        CHECK(Classify(facts) == Decline::TargetShape);
        facts = GoodFacts();
        facts.targetSamples = 2;
        CHECK(Classify(facts) == Decline::TargetShape);

        facts = GoodFacts();
        facts.hasDepth = false;
        CHECK(Classify(facts) == Decline::NoDepth);
        facts = GoodFacts();
        facts.depthWidth = 0;
        CHECK(Classify(facts) == Decline::NoDepth);
        facts = GoodFacts();
        facts.depthSamples = 4;
        CHECK(Classify(facts) == Decline::NoDepth);

        facts = GoodFacts();
        facts.depthFormat = 28; // R8G8B8A8_UNORM: a colour texture under the depth name
        CHECK(Classify(facts) == Decline::DepthFormat);
        for (uint32_t format : { 40u, 41u, 42u, 43u, 45u, 46u })
        {
            facts = GoodFacts();
            facts.depthFormat = format;
            CHECK(Classify(facts) == Decline::None);
        }

        facts = GoodFacts();
        facts.hasNormal = false;
        CHECK(Classify(facts) == Decline::NoNormal);
        facts = GoodFacts();
        facts.normalSamples = 2;
        CHECK(Classify(facts) == Decline::NoNormal);

        facts = GoodFacts();
        facts.hasCamera = false;
        CHECK(Classify(facts) == Decline::NoCamera);
        facts = GoodFacts();
        facts.cameraBytes = kCameraBytes - 16;
        CHECK(Classify(facts) == Decline::NoCamera);

        // The three allocations have to be one: a depth at another size is a build this module has not measured.
        facts = GoodFacts();
        facts.depthWidth = 1920;
        facts.depthHeight = 1080;
        CHECK(Classify(facts) == Decline::SizeMismatch);
        facts = GoodFacts();
        facts.normalWidth = 1920;
        facts.normalHeight = 1080;
        CHECK(Classify(facts) == Decline::SizeMismatch);
        facts = GoodFacts();
        facts.targetWidth = 1920;
        facts.targetHeight = 1080;
        CHECK(Classify(facts) == Decline::SizeMismatch);

        // The render window: a picture, not a preview; inside the target, with the engine's half pixel of slack; and covering a whole texel.
        facts = GoodFacts();
        facts.window[2] = 8.0f;
        CHECK(Classify(facts) == Decline::SizeMismatch);
        facts = GoodFacts();
        facts.window[0] = -1.0f;
        CHECK(Classify(facts) == Decline::SizeMismatch);
        facts = GoodFacts();
        facts.window[2] = 3840.0f; // 0.5 + 3840 is one half pixel past the allocation's edge, which the slack allows
        CHECK(Classify(facts) == Decline::None);
        facts = GoodFacts();
        facts.window[2] = 3841.0f;
        CHECK(Classify(facts) == Decline::SizeMismatch);
        facts = GoodFacts();
        facts.window[2] = 0.0f;
        CHECK(Classify(facts) == Decline::SizeMismatch);
    }

    // ---- the per-frame memory: one attempt a frame, the engine's texture matched by resource, the next frame forgetting it
    {
        FrameCache cache;
        CHECK(cache.MayRun()); // no frame read yet: the first reader of a session may run
        cache.Begin(7);
        CHECK(cache.MayRun());
        cache.TakeAttempt();
        CHECK(!cache.MayRun());
        CHECK(!cache.Has(nullptr));
        CHECK(!cache.Has(reinterpret_cast<const void*>(0x1000))); // nothing recorded yet

        const void* engineResource = reinterpret_cast<const void*>(0x1000);
        const void* view = reinterpret_cast<const void*>(0x2000);
        const void* ours = reinterpret_cast<const void*>(0x3000);
        cache.Record(engineResource, view, ours);
        CHECK(cache.valid);
        CHECK(cache.Has(engineResource));
        CHECK(!cache.Has(view));
        CHECK_EQ(cache.Ours(), ours);

        // One reader served, and then a long run without a frame change: the entry expires so a token that never advances cannot keep a stale
        // texture.
        cache.Serve();
        CHECK_EQ(cache.served, 1u);
        for (uint32_t i = 1; i < kMaxServedDraws; ++i)
            cache.Serve();
        CHECK(!cache.valid);
        CHECK(cache.MayRun()); // and the next reader runs the pass again

        // A new frame token clears the attempt and the entry, and the cache holds the token it was begun with: the entry of an ended frame can
        // never answer a later one, and the frame the module's own lines report is the frame it is actually in.
        cache.Begin(8);
        CHECK(cache.hasFrame);
        CHECK_EQ(cache.frame, 8ull);
        CHECK(cache.MayRun());
        CHECK(!cache.valid);
        CHECK(!cache.Has(engineResource));
        // The same token again keeps the frame's state.
        cache.TakeAttempt();
        cache.Begin(8);
        CHECK(!cache.MayRun());
        CHECK_EQ(cache.frame, 8ull);
        cache.Reset();
        CHECK(cache.MayRun() && !cache.valid && !cache.hasFrame);
    }

    // ---- the switch-off: this module's own streak, sixty-four refusals in a row, and a run starts it over
    {
        Session session;
        CHECK(session.Armed());
        for (uint32_t i = 0; i < kDeclineLimit - 1; ++i)
            CHECK(!session.NoteDecline());
        CHECK(session.Armed());
        CHECK(session.NoteDecline()); // the sixty-fourth
        CHECK(!session.Armed());
        CHECK(!session.NoteDecline()); // already off: not the one that switched it off
        Session other;
        for (uint32_t i = 0; i < kDeclineLimit - 1; ++i)
            other.NoteDecline();
        other.NoteRun();
        CHECK_EQ(other.declines, 0u);
        CHECK(!other.NoteDecline());
        CHECK(other.Armed());
        // The module's own limit is the shared one: a reader that is refused every frame stands down at the same count.
        CHECK_EQ(kDeclineLimit, engine::kConsecutiveDeclineLimit);
    }

    // ---- the log gate: one line per change of state, a repeat is dropped, and a budget per session
    {
        LogGate gate;
        CHECK(gate.Take(1));
        CHECK(!gate.Take(1));
        CHECK(gate.Take(2));
        unsigned lines = 2;
        for (uint32_t tag = 3; tag < 3 + kMaxLogLines; ++tag)
            if (gate.Take(tag))
                ++lines;
        CHECK_EQ(lines, kMaxLogLines);
        CHECK(!gate.Take(99));
    }

    // ---- the constant block: the five float4s the shader reads, and the values that carry the keys into it
    {
        const ReaderFacts facts = GoodFacts();
        const Params params = BuildParams(true, 0.75f, 2.0f, 0.0f, 2, Channel::G, 0.25f);
        const Constants constants = ConstantsOf(facts, params);
        CHECK_EQ(sizeof constants, 80u);
        CHECK_EQ(constants.window[0], facts.window[0]);
        CHECK_EQ(constants.window[2], facts.window[2]);
        CHECK_EQ(constants.sizes[0], 3840.0f);
        CHECK_EQ(constants.sizes[1], 2160.0f);
        CHECK_EQ(constants.tuning[0], 0.75f);
        CHECK_EQ(constants.tuning[1], 2.0f);
        CHECK_EQ(constants.tuning[2], kSoftnessFloor); // the key's zero becomes the width the shader needs
        CHECK_EQ(constants.tuning[3], 0.25f);
        CHECK_EQ(constants.control[0], 1.0f); // the channel key, as the shader's index
        CHECK_EQ(constants.control[1], kUpCos);
        CHECK_EQ(constants.control[2], kWetnessFloor);
        const Region region = RegionOf(facts);
        CHECK_EQ(constants.region[0], float(region.x));
        CHECK_EQ(constants.region[1], float(region.y));
        CHECK_EQ(constants.region[2], float(region.width));
        CHECK_EQ(constants.region[3], float(region.height));
        // The target key travels as its own index, which is what the module looks up in a reader's stamp.
        CHECK_EQ(TargetIndex(params), 1u);
        CHECK_EQ(constants.control[0], float(uint32_t(params.channel)));
    }

    // ---- the slot parse: a texture binding and nothing else, on the capture's own reflection order
    {
        using ext::ffxiv::diag::ParseBindings;
        using ext::ffxiv::diag::ShaderBinding;
        // (a) the real blob, whose sun/ambient reader declares the primary normal as g_SamplerGBuffer: its table is the samplers first
        // (g_SamplerDepth.S at s0, g_SamplerGBuffer.S at s3, g_SamplerGBuffer1.S at s4, g_SamplerGBuffer4.S at s5), then the textures (the depth
        // at t1, the primary normal g_SamplerGBuffer at t4, the material pack g_SamplerGBuffer1 at t5, the secondary buffer g_SamplerGBuffer4 at
        // t6) and the camera table at b0: a parse that took the first name match recorded the sampler's slot for the depth and for every
        // target.
        const std::string repo = argc > 1 ? std::string(argv[1]) : std::string();
        const std::string blob = ReadFixture(repo, kReaderFixture);
        CHECK(!blob.empty());
        ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
        const size_t count = ParseBindings(blob.data(), blob.size(), parsed, ext::ffxiv::diag::kMaxBindings);
        CHECK(count >= 9);
        // The order that makes the test a regression test: a sampler carries the depth's name at a lower slot than the texture does.
        int depthSampler = -1, depthTexture = -1, packSampler = -1, packTexture = -1;
        for (size_t i = 0; i < count; ++i)
        {
            const std::string_view name(parsed[i].name, parsed[i].nameLength);
            const auto type = parsed[i].type;
            if (name == "g_SamplerDepth.S" && type == ext::ffxiv::diag::kBindSampler)
                depthSampler = int(parsed[i].bindPoint);
            if (name == "g_SamplerDepth.T" && type == ext::ffxiv::diag::kBindTexture)
                depthTexture = int(parsed[i].bindPoint);
            if (name == "g_SamplerGBuffer1.S" && type == ext::ffxiv::diag::kBindSampler)
                packSampler = int(parsed[i].bindPoint);
            if (name == "g_SamplerGBuffer1.T" && type == ext::ffxiv::diag::kBindTexture)
                packTexture = int(parsed[i].bindPoint);
        }
        CHECK(depthSampler >= 0 && depthTexture >= 0 && depthSampler != depthTexture);
        CHECK(packSampler >= 0 && packTexture >= 0 && packSampler != packTexture);
        CHECK(depthSampler < depthTexture); // the samplers come first, which is what the old first-match scan recorded
        CHECK(packSampler < packTexture);

        const Inputs inputs = InputsFromBindings(parsed, count);
        CHECK_EQ(uint32_t(inputs.depth), uint32_t(depthTexture));
        CHECK_EQ(uint32_t(inputs.camera), 1u);
        CHECK_EQ(uint32_t(inputs.targets[0]), uint32_t(packTexture));
        CHECK(!inputs.HasTarget(1)); // this shader declares G-buffer 2 and 3 nowhere
        CHECK(!inputs.HasTarget(2));
        CHECK_EQ(uint32_t(inputs.targets[3]), 4u); // the primary world-space normal, g_SamplerGBuffer.T at t4 (its sampler is at s3)
        CHECK(inputs.Complete(0));        // the draw the pass may run at
        // Each recorded slot is the TEXTURE entry of the name it belongs to: the sampler of that name sits at a lower slot, and a reader that
        // took it would sample the blue-noise texture under the depth's name.
        CHECK(depthTexture != depthSampler && int(inputs.depth) != depthSampler);
        CHECK(packTexture != packSampler && int(inputs.targets[0]) != packSampler);
        CHECK(int(inputs.targets[3]) != 3); // the sampler slot of the normal's name

        // (b) the same order built by hand, so the rule is pinned without the fixture: a sampler for a name and then the texture.
        const ShaderBinding table[] {
            Bound("g_SamplerDepth.S", ext::ffxiv::diag::kBindSampler, 0),
            Bound("g_SamplerGBuffer1.S", ext::ffxiv::diag::kBindSampler, 4),
            Bound("g_SamplerGBuffer.S", ext::ffxiv::diag::kBindSampler, 5),
            Bound("g_CameraParameter", ext::ffxiv::diag::kBindConstantBuffer, 1),
            Bound("g_SamplerDepth.T", ext::ffxiv::diag::kBindTexture, 1),
            Bound("g_SamplerGBuffer1.T", ext::ffxiv::diag::kBindTexture, 5),
            Bound("g_SamplerGBuffer.T", ext::ffxiv::diag::kBindTexture, 6),
        };
        const Inputs hand = InputsFromBindings(table, std::size(table));
        CHECK_EQ(uint32_t(hand.depth), 1u);
        CHECK_EQ(uint32_t(hand.camera), 1u);
        CHECK_EQ(uint32_t(hand.targets[0]), 5u);
        CHECK_EQ(uint32_t(hand.targets[3]), 6u);
        CHECK(hand.Complete(0));

        // (c) a sampler on its own is not a reader: a shader whose only G-buffer name is the sampler state's is passed on.
        const ShaderBinding samplerOnly[] {
            Bound("g_SamplerGBuffer1.S", ext::ffxiv::diag::kBindSampler, 4),
            Bound("g_SamplerDepth.S", ext::ffxiv::diag::kBindSampler, 0),
        };
        const Inputs none = InputsFromBindings(samplerOnly, std::size(samplerOnly));
        CHECK(!none.HasTarget(0));
        CHECK(!none.HasDepth());
        CHECK(!none.Complete(0));
        // A texture that carries the target's name is a reader; the slot past D3D11's 128 is not a slot.
        const ShaderBinding wide[] {
            Bound("g_SamplerGBuffer1.T", ext::ffxiv::diag::kBindTexture, 128),
            Bound("g_SamplerGBuffer2.T", ext::ffxiv::diag::kBindTexture, 7),
        };
        const Inputs wideInputs = InputsFromBindings(wide, std::size(wide));
        CHECK(!wideInputs.HasTarget(0));
        CHECK_EQ(uint32_t(wideInputs.targets[1]), 7u);
        // A table with no entries, a null table, and a null name are no slots at all.
        const Inputs emptyTable = InputsFromBindings(wide, 0);
        CHECK(!emptyTable.HasTarget(1) && !emptyTable.HasDepth());
        const Inputs nullTable = InputsFromBindings(nullptr, std::size(wide));
        CHECK(!nullTable.HasTarget(1) && !nullTable.HasDepth());
        const ext::ffxiv::diag::ShaderBinding nullName[] { { nullptr, 0, ext::ffxiv::diag::kBindTexture, 3, 1 } };
        const Inputs skipped = InputsFromBindings(nullName, 1);
        CHECK(!skipped.HasTarget(0) && !skipped.HasDepth() && !skipped.HasCamera());
    }

    // ---- the gate of a reader draw: the target alone serves the swap once the frame exists, and only a complete reader may run the pass
    {
        // A target-only reader: the composite and every forward draw of a frame declare the material pack and nothing else.
        Inputs targetOnly;
        targetOnly.targets[0] = 5;
        CHECK(!targetOnly.Complete(0));
        CHECK(ServeAnswer(targetOnly, 0, false, true, false) == Serve::PassOn); // no frame texture yet, and this draw cannot make one
        CHECK(ServeAnswer(targetOnly, 0, true, false, false) == Serve::Serve); // the frame's texture answers it, spent attempt or not
        CHECK(ServeAnswer(targetOnly, 0, true, true, false) == Serve::Serve);
        // The complete reader runs the frame's pass, once.
        Inputs complete;
        complete.targets[0] = 5;
        complete.targets[3] = 6;
        complete.depth = 1;
        complete.camera = 1;
        CHECK(complete.Complete(0));
        CHECK(ServeAnswer(complete, 0, false, true, false) == Serve::Attempt);
        CHECK(ServeAnswer(complete, 0, false, false, false) == Serve::PassOn); // the frame's one attempt is spent
        CHECK(ServeAnswer(complete, 0, true, true, false) == Serve::Serve);
        // A draw that declares the depth but not the key's target is not a reader of it, whatever else it has.
        Inputs otherTarget;
        otherTarget.targets[1] = 7;
        otherTarget.targets[3] = 6;
        otherTarget.depth = 1;
        otherTarget.camera = 1;
        CHECK(ServeAnswer(otherTarget, 0, true, true, false) == Serve::PassOn);
        CHECK(ServeAnswer(otherTarget, 1, false, true, false) == Serve::Attempt);

        // A replay of a game draw by another module (the character preview, a diagnostic replay) is never served: the frame's water does not
        // reach the preview's picture.
        CHECK(ServeAnswer(targetOnly, 0, true, false, true) == Serve::PassOn);
        CHECK(ServeAnswer(targetOnly, 0, true, true, true) == Serve::PassOn);
        // A replayed complete reader does not spend the frame's one attempt: the same frame still answers the next draw with it, so a replay that
        // comes before the frame's own reader costs the frame nothing.
        CHECK(ServeAnswer(complete, 0, false, true, true) == Serve::PassOn);
        CHECK(ServeAnswer(complete, 0, false, true, false) == Serve::Attempt);
        CHECK(ServeAnswer(complete, 0, true, true, true) == Serve::PassOn);
        CHECK(ServeAnswer(otherTarget, 1, false, true, true) == Serve::PassOn); // a replay is passed on before the target test too
    }

    // ---- the target key and the channel pair: only the two 8-bit targets can be shaped, and the material pack's red is a selector the pass must
    //      not write, so a value of either refused one is today's picture
    {
        CHECK(TargetServable(1) && TargetServable(2));
        CHECK(!TargetServable(0) && !TargetServable(3) && !TargetServable(4));
        for (int32_t target = 1; target <= 4; ++target)
        {
            const Params params = BuildParams(true, 0.5f, 1.0f, 0.1f, target);
            CHECK_EQ(params.target, uint32_t(target)); // the key keeps what the ini said, so the log can name it
            CHECK_EQ(IsIdentity(params), !TargetServable(uint32_t(target)));
            CHECK_EQ(Wants(params), TargetServable(uint32_t(target)));
        }
        CHECK(IsIdentity(BuildParams(true, 0.5f, 1.0f, 0.1f, 3)));
        CHECK(IsIdentity(BuildParams(true, 0.5f, 1.0f, 0.1f, 4)));
        CHECK(!IsIdentity(BuildParams(true, 0.5f, 1.0f, 0.1f, 2)));
        // The channel pair that would flip the engine's own surface selector: target 1's red is not a gloss, and the pass must refuse it rather
        // than write the value that turned the ground green.
        CHECK(!ChannelServable(1, Channel::R));
        CHECK(ChannelServable(1, Channel::G) && ChannelServable(1, Channel::B) && ChannelServable(1, Channel::A));
        for (Channel channel : { Channel::R, Channel::G, Channel::B, Channel::A })
            CHECK(ChannelServable(2, channel)); // target 2 has no selector: every channel of it is shaped when the key names it
        const Params selector = BuildParams(true, 0.5f, 1.0f, 0.1f, 1, Channel::R, 1.0f);
        CHECK(IsIdentity(selector));
        CHECK(!Wants(selector));
        CHECK(!IsIdentity(BuildParams(true, 0.5f, 1.0f, 0.1f, 1, Channel::A, 1.0f)));
        CHECK(!IsIdentity(BuildParams(true, 0.5f, 1.0f, 0.1f, 2, Channel::R, 1.0f)));
        // The line names the value that was refused, so a session can find the ini line that carried it.
        CHECK(FormatTargetRefused(4).find("target 4") != std::string::npos || FormatTargetRefused(4).find("G-buffer target 4") != std::string::npos);
        CHECK(FormatTargetRefused(3).find("G-buffer target 3") != std::string::npos);
        CHECK(FormatChannelRefused(1, Channel::R).find("G-buffer 1's r channel") != std::string::npos);
        CHECK(FormatChannelRefused(1, Channel::R).find("selector") != std::string::npos);
    }

    // ---- a frame the pass cannot serve is a skip, not a refusal: only a real failure feeds the streak
    {
        CHECK(CountsTowardsSwitchOff(Decline::Failure));
        CHECK(CountsTowardsSwitchOff(Decline::Exception));
        CHECK(CountsTowardsSwitchOff(Decline::TargetShape)); // a target of a format or size the copy cannot answer is this build's
        for (const Decline scene : { Decline::NoTarget, Decline::NoDepth, Decline::DepthFormat, Decline::NoNormal, Decline::NoCamera,
                                     Decline::CameraLayout, Decline::CameraNotReady, Decline::SizeMismatch, Decline::Replay, Decline::Reentry,
                                     Decline::FeatureOff })
            CHECK(!CountsTowardsSwitchOff(scene));
        // A hundred frames of a scene this pass cannot serve leave the module armed; the sixty-four real failures are what stand it down.
        Session session;
        for (uint32_t frame = 0; frame < 100; ++frame)
            if (CountsTowardsSwitchOff(Decline::NoDepth) && session.NoteDecline())
                CHECK(false);
        CHECK(session.Armed());
        for (uint32_t frame = 0; frame < kDeclineLimit; ++frame)
            if (CountsTowardsSwitchOff(Decline::Failure) && session.NoteDecline())
                CHECK_EQ(frame, kDeclineLimit - 1);
        CHECK(!session.Armed());
    }

    // ---- the lines a session reads back
    {
        const std::string refused = FormatRefusedReader(Decline::NoDepth, GoodFacts(), Inputs {}, 0);
        CHECK(refused.find("Puddles:") == 0);
        CHECK(refused.find("G-buffer 1 3840x2160") != std::string::npos);
        CHECK(refused.find("slot none") != std::string::npos);
        CHECK(refused.find("window 1920x1080 at 0,0") != std::string::npos);
        const std::string run = FormatRun(BuildParams(true, 0.5f, 1.0f, 0.1f, 1, Channel::G), 3840, 2160, 0.5f, 0.5f, true, -1.0f);
        CHECK(run.find("G-buffer 1's g channel") != std::string::npos);
        CHECK(run.find("3840x2160") != std::string::npos);
        // The run line says the wetness the shape gated on, with both values it was chosen from: the channel value `wet` is a different number,
        // and a session must not read it as "it is raining".
        CHECK(run.find("wet 1.00") != std::string::npos);
        CHECK(run.find("wetness 0.50 (engine 0.50, knob off)") != std::string::npos);
        const std::string forced = FormatRun(BuildParams(true), 3840, 2160, 1.0f, 0.0f, true, 1.0f);
        CHECK(forced.find("wetness 1.00 (engine 0.00, knob 1.00)") != std::string::npos);
        const std::string unread = FormatRun(BuildParams(true), 3840, 2160, 1.0f, 0.0f, false, 1.0f);
        CHECK(unread.find("wetness 1.00 (engine not read, knob 1.00)") != std::string::npos);
        // The knob off and no engine read yet: the line says it does not know, never a figure the shape did not use.
        const std::string unknown = FormatRun(BuildParams(true), 3840, 2160, 0.0f, 0.0f, false, -1.0f);
        CHECK(unknown.find("wetness not known (engine not read, knob off)") != std::string::npos);
        CHECK(unknown.find("wetness 0.00") == std::string::npos);
        CHECK(FormatServed(354).find("354") != std::string::npos);
        CHECK(FormatSwitchedOff().find("64") != std::string::npos);
        for (uint32_t reason = 0; reason < kDeclineReasonCount; ++reason)
            CHECK(std::string(DeclineName(Decline(reason))).size() > 1);
    }


    // ---- the tone-map stage's arithmetic
    //
    // The pool the stage shades is the gloss pass's field at the same wetness, and the colour is the darkened scene plus the marched hit with
    // water's Fresnel. Each of the four numbers is pinned where a wrong one would show: the mask at zero, the Fresnel's two ends, the gain that
    // cannot add more than the hit's own colour, and the identity that keeps a dry frame a pure copy.
    {
        CHECK_EQ(PoolMask(0.5f, 0.5f, 0.0f, 0.1f), 0.0f); // dry: no pool anywhere, whatever the field says
        const float full = PoolMask(0.9f, 0.5f, 1.0f, 0.1f);
        CHECK(full > 0.99f); // a field at 0.9 against a level of 0.5 is a pool at full rain
        CHECK(PoolMask(0.2f, 0.5f, 1.0f, 0.1f) < 1e-6f); // and below the level it is none
        // Half the wetness is half the mask: the stage and the gloss pass ramp together.
        CHECK(std::fabs(PoolMask(0.9f, 0.5f, 0.5f, 0.1f) - 0.5f * full) < 1e-6f);

        // Schlick with water's F0: 2 % straight on and 100 % at grazing, monotone in between.
        CHECK(std::fabs(SchlickWater(1.0f) - kWaterF0) < 1e-6f);
        CHECK(std::fabs(SchlickWater(0.0f) - 1.0f) < 1e-6f);
        CHECK(SchlickWater(0.3f) < SchlickWater(0.2f));
        CHECK(SchlickWater(-5.0f) == 1.0f); // a cosine outside the range is a grazing angle, not a value below F0

        // The gain is the mask, the key's strength and the Fresnel, and never more than the hit's own colour.
        CHECK_EQ(PuddleReflectionGain(1.0f, 0.0f, 0.0f), 0.0f);
        CHECK_EQ(PuddleReflectionGain(0.0f, 0.0f, 1.0f), 0.0f);
        CHECK(std::fabs(PuddleReflectionGain(1.0f, 1.0f, 1.0f) - kWaterF0) < 1e-6f);
        CHECK(std::fabs(PuddleReflectionGain(0.5f, 0.0f, 0.6f) - 0.3f) < 1e-6f);
        CHECK(PuddleReflectionGain(1.0f, 0.0f, 2.0f) <= 1.0f); // a strength past the key's range cannot add more than the scene's own

        // The darkening: nothing outside a pool, the key's share inside a full one.
        CHECK_EQ(PuddleDarkening(0.0f, 0.45f), 1.0f);
        CHECK(std::fabs(PuddleDarkening(1.0f, 0.45f) - 0.55f) < 1e-6f);
        CHECK(std::fabs(PuddleDarkening(0.5f, 0.5f) - 0.75f) < 1e-6f);

        // The whole texel: outside a pool the scene to the bit, so a frame the mask misses is a pure copy; inside one the hit is added with the
        // gain and nothing else.
        CHECK_EQ(PuddleTexel(0.7f, 0.9f, 0.0f, 0.2f, 0.6f, 0.45f), 0.7f);
        // Straight on, the Fresnel is water's 2 % rather than nothing: a mirror seen head-on still returns a little of the scene.
        CHECK(std::fabs(PuddleTexel(0.7f, 0.9f, 1.0f, 1.0f, 0.6f, 0.0f) - (0.7f + 0.9f * 0.6f * kWaterF0)) < 1e-6f);
        CHECK(PuddleTexel(0.7f, 0.0f, 1.0f, 0.2f, 0.6f, 0.45f) < 0.7f); // a miss: the darkened scene, never black
        CHECK(std::fabs(PuddleTexel(0.4f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f) - 1.4f) < 1e-6f);

        // The stage's identity: the darkening at zero with the reflection off is the frame's own picture, so nothing is copied for it.
        CHECK(ToneIdentity(BuildParams(true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, false, 0.6f, 0.0f, 0.35f, 24)));
        CHECK(!ToneIdentity(BuildParams(true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, false, 0.6f, 0.01f, 0.35f, 24)));
        // Darkening 0 with the reflection on is NOT the identity: the mirror is the thing that would be skipped. Without this case the rule can be
        // narrowed to the darkening alone and still pass every other one.
        CHECK(!ToneIdentity(BuildParams(true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.0f, 0.35f, 24)));
        CHECK(!ToneIdentity(BuildParams(true)));     // the shipped keys shape something
        CHECK(ToneIdentity(BuildParams(true, 0.0f))); // a coverage of 0 is the engine's picture for both stages
        CHECK(ToneIdentity(BuildParams(false)));      // and so is the switch off

        // The new keys: clamped, and a NaN reads as the default so a broken ini cannot drive the stage out of the ranges it was written for.
        const Params outOfRange =
            BuildParams(true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 9.0f, -1.0f, 4.0f, int32_t(kMinSteps) - 1);
        CHECK(std::fabs(outOfRange.reflectionStrength - kMaxReflectionStrength) < 1e-6f);
        CHECK(std::fabs(outOfRange.darkening - kMinDarkening) < 1e-6f);
        CHECK(std::fabs(outOfRange.ripple - kMaxRipple) < 1e-6f);
        CHECK_EQ(outOfRange.reflectionSteps, kStepsDefault); // a step count out of range reads as the default
        CHECK_EQ(ClampSteps(int32_t(kMinSteps)), kMinSteps);
        CHECK_EQ(ClampSteps(int32_t(kMaxSteps)), kMaxSteps);
        CHECK_EQ(ClampSteps(int32_t(kMinSteps) - 1), kStepsDefault);
        CHECK_EQ(ClampSteps(int32_t(kMaxSteps) + 1), kStepsDefault);
        CHECK(std::fabs(BuildParams(true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, NaN()).reflectionStrength -
                        kReflectionStrengthDefault) < 1e-6f);
        CHECK(std::fabs(BuildParams(true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, NaN()).darkening -
                        kDarkeningDefault) < 1e-6f);
        // The epoch follows the new keys: two strengths that differ are two pictures, and the frame's memory has to see it.
        CHECK(EpochOf(BuildParams(true)) != EpochOf(BuildParams(true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.5f, 0.45f, 0.35f, 24)));
        CHECK(EpochOf(BuildParams(true)) != EpochOf(BuildParams(true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, false, 0.6f, 0.45f, 0.35f, 24)));
        CHECK(EpochOf(BuildParams(true)) !=
              EpochOf(BuildParams(true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.45f, 0.35f, 16)));
        CHECK(EpochOf(BuildParams(true)) != EpochOf(BuildParams(true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.45f, 0.2f, 24)));
    }

    // ---- the tone-map draw's shape: one reason at a time, from the facts of the draw a session really has
    {
        auto good = [] {
            ToneFacts facts;
            facts.renderTargets = 1;
            facts.targetWidth = 3840;
            facts.targetHeight = 2160;
            facts.targetFormat = 10; // R16G16B16A16_FLOAT
            facts.sceneFormat = 10;
            facts.sceneWidth = 3840;
            facts.sceneHeight = 2160;
            facts.sceneSampled = true;
            facts.sceneIsScene = true;
            facts.viewPositionSampled = true;
            facts.viewPositionWidth = 3840;
            facts.viewPositionHeight = 2160;
            facts.normalSampled = true;
            facts.normalWidth = 3840;
            facts.normalHeight = 2160;
            facts.cameraBytes = kCameraBytes;
            facts.window[0] = 0.5f;
            facts.window[1] = 0.5f;
            facts.window[2] = 1920.0f;
            facts.window[3] = 1080.0f;
            return facts;
        };
        CHECK(ClassifyTone(good()) == ToneDecline::None);
        {
            ToneFacts facts = good();
            facts.replay = true;
            CHECK(ClassifyTone(facts) == ToneDecline::Replay);
        }
        {
            ToneFacts facts = good();
            facts.sceneIsScene = false; // t0 is not the scene this frame's composite wrote
            CHECK(ClassifyTone(facts) == ToneDecline::NotToneMap);
        }
        {
            ToneFacts facts = good();
            facts.targetIsScene = true; // the engine's own post-tone copy: the game's draw
            CHECK(ClassifyTone(facts) == ToneDecline::NotToneMap);
        }
        {
            ToneFacts facts = good();
            facts.renderTargets = 2;
            CHECK(ClassifyTone(facts) == ToneDecline::RenderTarget);
        }
        {
            ToneFacts facts = good();
            facts.targetFormat = 87; // B8G8R8A8: not the scene's format
            CHECK(ClassifyTone(facts) == ToneDecline::TargetFormat);
        }
        {
            ToneFacts facts = good();
            facts.targetWidth = 1920;
            CHECK(ClassifyTone(facts) == ToneDecline::TargetFormat);
        }
        {
            ToneFacts facts = good();
            facts.sceneFormat = 87;
            CHECK(ClassifyTone(facts) == ToneDecline::SceneFormat);
        }
        {
            ToneFacts facts = good();
            facts.viewPositionSampled = false;
            CHECK(ClassifyTone(facts) == ToneDecline::Inputs);
        }
        {
            ToneFacts facts = good();
            facts.cameraBytes = kCameraBytes - 16;
            CHECK(ClassifyTone(facts) == ToneDecline::Inputs);
        }
        {
            ToneFacts facts = good();
            facts.normalWidth = 1920;
            CHECK(ClassifyTone(facts) == ToneDecline::SizeMismatch);
        }
        {
            ToneFacts facts = good();
            facts.sceneWidth = facts.targetWidth = facts.viewPositionWidth = facts.normalWidth = 100;
            facts.window[2] = 100.0f; // everything agrees on a scene smaller than this stage was measured on
            CHECK(ClassifyTone(facts) == ToneDecline::SizeMismatch);
        }
        {
            ToneFacts facts = good();
            facts.window[2] = 0.0f;
            CHECK(ClassifyTone(facts) == ToneDecline::Window);
        }
        {
            ToneFacts facts = good();
            facts.window[0] = 1920.5f; // the engine's half-pixel origin at the allocation's edge is still one scene
            CHECK(ClassifyTone(facts) == ToneDecline::None);
        }
        {
            ToneFacts facts = good();
            facts.window[0] = 1921.0f;
            CHECK(ClassifyTone(facts) == ToneDecline::Window); // the window does not fit the scene
        }
        // Only this build's own failures count towards the stage's switch-off: a scene it cannot serve is a frame to pass on.
        CHECK(ToneCountsTowardsSwitchOff(ToneDecline::PassFailed));
        CHECK(ToneCountsTowardsSwitchOff(ToneDecline::Exception));
        CHECK(!ToneCountsTowardsSwitchOff(ToneDecline::NotToneMap));
        CHECK(!ToneCountsTowardsSwitchOff(ToneDecline::Inputs));
        for (uint32_t reason = 0; reason < kToneDeclineReasonCount; ++reason)
            CHECK(std::string(ToneDeclineName(ToneDecline(reason))).size() > 1);
    }

    // ---- the regions and the tone stage's constant block
    {
        // The whole texels of a window at the engine's half-pixel origin: texel 1 is the first that fits inside [0.5, 1920.5).
        const Region whole = WholeTexels(0.5f, 0.5f, 1920.0f, 1080.0f, 3840, 2160);
        CHECK_EQ(whole.x, 1u);
        CHECK_EQ(whole.y, 1u);
        CHECK_EQ(whole.Right(), 1920u);
        CHECK_EQ(whole.Bottom(), 1080u);
        CHECK_EQ(WholeTexels(-1.0f, 0.0f, 100.0f, 100.0f, 320, 320).x, 0u); // clamped into the allocation
        CHECK(WholeTexels(0.0f, 0.0f, 0.0f, 100.0f, 320, 320).Empty());

        // The copy's border: one texel on every side, and never past the allocation.
        ToneFacts at;
        at.sceneWidth = 3840;
        at.sceneHeight = 2160;
        at.window[2] = 1920.0f;
        at.window[3] = 1080.0f;
        at.window[0] = 0.5f;
        at.window[1] = 0.5f;
        const Region copy = ToneCopyOf(at);
        CHECK_EQ(copy.x, 0u);
        CHECK_EQ(copy.y, 0u);
        CHECK_EQ(copy.Right(), 1921u);
        CHECK_EQ(copy.Bottom(), 1081u);
        at.window[0] = 0.0f;
        at.window[1] = 0.0f;
        at.window[2] = 3840.0f;
        at.window[3] = 2160.0f;
        const Region edge = ToneCopyOf(at);
        CHECK_EQ(edge.x, 0u);
        CHECK_EQ(edge.Right(), 3840u);
        CHECK_EQ(edge.Bottom(), 2160u);

        // The ring the draw does not write: the copy minus the window's whole texels, in at most four bands, and every texel of it outside the
        // window. A window in the middle of the allocation has four bands; a window that fills it has none (there is nothing the tone map's filter
        // reads that the draw does not write), and neither ever reaches past the copy.
        at.window[0] = 0.5f;
        at.window[1] = 0.5f;
        at.window[2] = 1920.0f;
        at.window[3] = 1080.0f;
        const RegionBands middle = ToneBorderOf(at);
        const Region inner = WholeTexels(at.window[0], at.window[1], at.window[2], at.window[3], at.sceneWidth, at.sceneHeight);
        CHECK_EQ(middle.count, 4u);
        uint64_t ringTexels = 0;
        for (uint32_t i = 0; i < middle.count; ++i)
        {
            const Region& band = middle.band[i];
            CHECK(!band.Empty());
            CHECK(band.x >= copy.x && band.Right() <= copy.Right());
            CHECK(band.y >= copy.y && band.Bottom() <= copy.Bottom());
            // Nothing the draw writes is in the ring, and the bands do not overlap (the rows first, then the columns beside the window's own rows).
            const bool overlapsWindow = band.x < inner.Right() && band.Right() > inner.x && band.y < inner.Bottom() && band.Bottom() > inner.y;
            CHECK(!overlapsWindow);
            ringTexels += uint64_t(band.width) * band.height;
        }
        // The ring's texels are the copy's own minus the window's: one texel around a 1920x1080 window, less what the allocation cuts off.
        const uint64_t copyTexels = uint64_t(copy.width) * copy.height;
        const uint64_t windowTexels = uint64_t(inner.width) * inner.height;
        CHECK_EQ(ringTexels, copyTexels - windowTexels);
        at.window[0] = 0.0f;
        at.window[1] = 0.0f;
        at.window[2] = 3840.0f;
        at.window[3] = 2160.0f;
        CHECK_EQ(ToneBorderOf(at).count, 0u); // a window that is the whole allocation: the draw writes every texel of the copy
        at.window[2] = 0.0f;
        CHECK_EQ(ToneBorderOf(at).count, 1u); // a window that covers no whole texel has no usable ring: the copy is taken whole, and the shape
                                             // rule refuses the frame before the pass ever sees it

        // The constant block: five float4s, and the values the shader reads.
        at.window[0] = 0.5f;
        at.window[1] = 0.5f;
        at.window[2] = 1920.0f;
        at.window[3] = 1080.0f;
        const Params params = BuildParams(true, 0.6f, 8.0f, 0.2f, 1, Channel::A, 1.0f, true, 0.7f, 0.5f, 0.4f, 32);
        const ToneConstants constants = ToneConstantsOf(at, params);
        CHECK_EQ(sizeof(ToneConstants), 112u);
        CHECK(std::fabs(constants.window[2] - 1920.0f) < 1e-6f);
        CHECK(std::fabs(constants.sizes[0] - 3840.0f) < 1e-6f);
        CHECK(std::fabs(constants.sizes[2] - 32.0f) < 1e-6f); // the march's steps, in the block
        CHECK(std::fabs(constants.tuning[0] - 0.6f) < 1e-6f);
        CHECK(std::fabs(constants.tuning[1] - 8.0f) < 1e-6f);
        CHECK(std::fabs(constants.tuning[2] - 0.2f) < 1e-6f);
        CHECK(std::fabs(constants.tuning[3] - 0.5f) < 1e-6f);
        CHECK(std::fabs(constants.control[0] - 0.7f) < 1e-6f);
        CHECK(std::fabs(constants.control[1] - 0.4f) < 1e-6f);
        CHECK(std::fabs(constants.control[2] - kWetnessFloor) < 1e-6f);
        CHECK(std::fabs(constants.control[3] - kUpCos) < 1e-6f);
        CHECK_EQ(constants.region[0], 1.0f); // the half-pixel origin again, in the block the shader tests against
        // The reflection key at off is a strength of zero and no march: the shader skips it, and the darkening is what is left.
        const ToneConstants plain = ToneConstantsOf(at, BuildParams(true, 0.6f, 8.0f, 0.2f, 1, Channel::A, 1.0f, false));
        CHECK_EQ(plain.control[0], 0.0f);
        CHECK_EQ(plain.sizes[2], 0.0f);
        // The raindrops' clock: a run that passes none (every host test and every reference frame) is the still ripple, and a live run's seconds
        // land in the block for the shader, wrapped into the window a float can carry the phase of.
        CHECK_EQ(constants.sizes[3], 0.0f);
        CHECK(std::fabs(RippleTimeOf(1.5f) - 1.5f) < 1e-6f);
        CHECK(std::fabs(ToneConstantsOf(at, params, 1.5f).sizes[3] - 1.5f) < 1e-6f);
        CHECK_EQ(RippleTimeOf(0.0f), 0.0f);
        CHECK_EQ(RippleTimeOf(-2.0f), 0.0f); // no clock, not a negative phase
        CHECK_EQ(RippleTimeOf(std::nanf("")), 0.0f);
        CHECK_EQ(RippleTimeOf(1.0e9f), 0.0f); // a reading this build cannot use is no clock either
        CHECK(RippleTimeOf(kRippleTimePeriod) == 0.0f); // a whole period wraps to the start
        const float wrapped = RippleTimeOf(kRippleTimePeriod + 3.25f);
        CHECK(std::fabs(wrapped - 3.25f) < 1e-3f);
        CHECK(wrapped >= 0.0f && wrapped < kRippleTimePeriod);
        // Rain's amount reaches the block, zero stops the clock so the still-pool path runs, and a wild value is clamped or no rain.
        CHECK_EQ(ToneConstantsOf(at, params, 1.5f).rain[0], 1.0f);
        CHECK(std::fabs(ToneConstantsOf(at, params, 1.5f, 0.25f).rain[0] - 0.25f) < 1e-6f);
        CHECK(std::fabs(ToneConstantsOf(at, params, 1.5f, 0.25f).sizes[3] - 1.5f) < 1e-6f);
        CHECK_EQ(ToneConstantsOf(at, params, 1.5f, 0.0f).sizes[3], 0.0f);
        CHECK_EQ(ToneConstantsOf(at, params, 1.5f, 0.0f).rain[0], 0.0f);
        CHECK_EQ(ToneConstantsOf(at, params, 1.5f, 7.0f).rain[0], 1.0f);
        CHECK_EQ(ToneConstantsOf(at, params, 1.5f, std::nanf("")).rain[0], 0.0f);
    }

    // ---- is it raining: the direction of the engine's wetness decides, with the session's captured values
    {
        RainTracker rain;
        RainStep(rain, 0.0f, false, 0.1f);
        CHECK(!rain.raining); // nothing read yet
        RainStep(rain, 0.0f, true, 0.1f);
        CHECK(!rain.raining); // a dry floor
        CHECK_EQ(rain.amount, 0.0f);
        RainStep(rain, 0.05f, true, 0.1f);
        CHECK(rain.raining); // the amount climbs: it started
        for (int i = 0; i < 50; ++i)
            RainStep(rain, 1.0f, true, 0.1f);
        CHECK(rain.raining); // a flat 1.00 keeps the answer
        CHECK_EQ(rain.amount, 1.0f); // and the fade has finished
        RainStep(rain, 0.85f, true, 0.1f);
        CHECK(!rain.raining); // it sinks: the rain stopped while the floor is still wet
        CHECK(rain.amount > 0.9f && rain.amount < 1.0f); // and the rings die away instead of cutting
        for (int i = 0; i < 50; ++i)
            RainStep(rain, 0.85f, true, 0.1f);
        CHECK(!rain.raining); // a flat stretch on the way down is still not rain
        CHECK_EQ(rain.amount, 0.0f);
        RainStep(rain, 0.60f, true, 0.1f);
        CHECK(!rain.raining);
        RainStep(rain, 0.70f, true, 0.1f);
        CHECK(rain.raining); // it starts again before the floor dried
        // Moves far below the step, as a high frame rate sees them, add up against the reference.
        RainTracker slow;
        RainStep(slow, 1.0f, true, 0.016f);
        CHECK(slow.raining); // a wet floor with no history counts as rain until the number falls
        float falling = 1.0f;
        for (int i = 0; i < 20 && slow.raining; ++i)
        {
            falling -= 0.0004f;
            RainStep(slow, falling, true, 0.016f);
        }
        CHECK(!slow.raining);
        // Dry again ends it at once, and a NaN reads as dry.
        RainTracker wet;
        RainStep(wet, 0.5f, true, 0.1f);
        RainStep(wet, 0.0f, true, 0.1f);
        CHECK(!wet.raining);
        RainStep(wet, 0.5f, true, 0.1f);
        RainStep(wet, std::nanf(""), true, 0.1f);
        CHECK(!wet.raining);
        // A step with no time passed, or a clock that went backwards, holds the fade.
        RainTracker held;
        RainStep(held, 0.5f, true, 0.0f);
        CHECK_EQ(held.amount, 0.0f);
        RainStep(held, 0.5f, true, -3.0f);
        CHECK_EQ(held.amount, 0.0f);
        // The engine's own overshoot (a reading past 1.0 mid-rain, seen in a session's log as wetness 1.00 flips)
        // saturates: the rise arms the rings, and the next reading is a flat stretch, not a fall.
        RainTracker overshoot;
        RainStep(overshoot, 1.0f, true, 0.1f);
        CHECK(overshoot.raining);
        RainStep(overshoot, 1.02f, true, 0.1f);
        CHECK(overshoot.raining);
        RainStep(overshoot, 1.00f, true, 0.1f);
        CHECK(overshoot.raining); // a steady rain stays a steady rain
        // A stamp gap (a camera cut, a menu, the first call after a publish) eases the rings in over the cap instead
        // of saturating the fade in one frame - the quick flicker when the puddles start loading.
        RainTracker gap;
        RainStep(gap, 0.5f, true, 40.0f);
        CHECK(gap.raining);
        CHECK(gap.amount > 0.0f && gap.amount <= kRainFadeStepMaxSeconds / kRainFadeSeconds + 1e-6f);
    }

    // ---- the pool's reach: open water keeps the engine's texel past it
    {
        ToneFacts reachFacts;
        reachFacts.sceneWidth = 3840;
        reachFacts.sceneHeight = 2160;
        reachFacts.window[0] = 0.0f;
        reachFacts.window[1] = 0.0f;
        reachFacts.window[2] = 1920.0f;
        reachFacts.window[3] = 1080.0f;
        CHECK(std::fabs(ClampReach(std::nanf("")) - kReachDefault) < 1e-6f); // a broken value reads as the default
        CHECK_EQ(ClampReach(-1.0f), 0.0f);                                    // below the band reads as off
        CHECK(std::fabs(ClampReach(128.0f) - kMaxReach) < 1e-6f);             // past the top reads as the top
        CHECK(std::fabs(ClampReach(30.0f) - 30.0f) < 1e-6f);
        Params reachParams = BuildParams(true);
        CHECK(std::fabs(reachParams.reach - kReachDefault) < 1e-6f); // the default rides BuildParams
        CHECK(WithinReach(10.0f, reachParams));                      // inside the reach: the stage shades
        CHECK(!WithinReach(40.0f, reachParams));                     // past it: open water keeps the engine's texel
        reachParams.reach = 0.0f;
        CHECK(WithinReach(40.0f, reachParams)); // a reach of 0 never refuses
        const ToneConstants reachConstants = ToneConstantsOf(reachFacts, reachParams);
        CHECK(std::fabs(reachConstants.reach[0] - 0.0f) < 1e-6f); // the reach rides the block for the shader's gate
        reachParams.reach = 24.0f;
        const ToneConstants defaultReach = ToneConstantsOf(reachFacts, reachParams);
        CHECK(std::fabs(defaultReach.reach[0] - 24.0f) < 1e-6f);
    }

    // ---- the shares a capture reads, and the lines
    {
        ToneStats stats;
        stats.texels = 1000;
        stats.poolTexels = 250;
        stats.hitTexels = 100;
        CHECK(std::fabs(stats.PoolShare() - 0.25f) < 1e-6f);
        CHECK(std::fabs(stats.HitShare() - 0.4f) < 1e-6f);
        const ToneStats empty;
        CHECK_EQ(empty.PoolShare(), 0.0f);
        CHECK_EQ(empty.HitShare(), 0.0f); // a frame with no pool has no hit share to report, not a division by zero

        ToneFacts facts;
        facts.renderTargets = 1;
        facts.targetWidth = facts.sceneWidth = facts.viewPositionWidth = facts.normalWidth = 3840;
        facts.targetHeight = facts.sceneHeight = facts.viewPositionHeight = facts.normalHeight = 2160;
        facts.targetFormat = facts.sceneFormat = 10;
        facts.sceneSampled = facts.sceneIsScene = facts.viewPositionSampled = facts.normalSampled = true;
        facts.cameraBytes = kCameraBytes;
        facts.window[2] = 1920.0f;
        facts.window[3] = 1080.0f;
        const std::string run = FormatToneRun(BuildParams(true), facts, stats, true, true, 0.0987);
        CHECK(run.find("Puddles:") == 0);
        CHECK(run.find("darkening 0.45") != std::string::npos);
        CHECK(run.find("reflection on at 0.60") != std::string::npos);
        CHECK(run.find("ripple 0.35") != std::string::npos);
        CHECK(run.find("24 steps") != std::string::npos);
        CHECK(run.find("scale 6.00 m") != std::string::npos);
        CHECK(run.find("pool 0.250 of the frame") != std::string::npos);
        CHECK(run.find("march hit 0.400 of it") != std::string::npos);
        CHECK(run.find("0.0987 ms") != std::string::npos);
        const std::string unmeasured = FormatToneRun(BuildParams(true), facts, stats, false, false, 0.0);
        CHECK(unmeasured.find("of the frame") == std::string::npos); // no share it has not measured ...
        CHECK(unmeasured.find("march hit") == std::string::npos);
        CHECK(unmeasured.find("ms on the GPU") == std::string::npos); // ... and no cost either

        // engine.txt's block: both stages, the reasons, the shares, and the honest "-" before a capture has measured one.
        PuddleStatus status;
        status.keyOn = true;
        status.glossArmed = true;
        status.toneArmed = true;
        status.glossRuns = 3;
        status.glossReaders = 700;
        status.toneRuns = 2;
        status.glossDeclined = 1;
        status.toneDeclined = 4;
        status.declines.emplace_back("tone: no draw of this frame handed over the view positions", 4);
        const std::string text = PuddleStatusText(status);
        CHECK(text.find("puddle module: on") == 0);
        CHECK(text.find("puddle tone armed: yes") != std::string::npos);
        CHECK(text.find("puddle tone runs: 2") != std::string::npos);
        CHECK(text.find("puddle declined: 5") != std::string::npos);
        CHECK(text.find("puddle decline: tone: no draw") != std::string::npos);
        CHECK(text.find("puddle pool share: -") != std::string::npos);
        status.statsKnown = true;
        status.poolShare = 0.3125f;
        status.hitShare = 0.625f;
        status.haveWindow = true;
        status.windowToneRuns = 2;
        const std::string measured = PuddleStatusText(status);
        CHECK(measured.find("puddle pool share: 0.312") != std::string::npos);
        CHECK(measured.find("puddle march hit share: 0.625") != std::string::npos);
        CHECK(measured.find("puddle window tone runs: 2") != std::string::npos);
        const PuddleStatus off;
        CHECK_EQ(PuddleStatusText(off), std::string("puddle module: off\n"));
    }

    // ---- the view position's slot in the stamp
    {
        Inputs inputs;
        inputs.vposition = 10;
        inputs.targets[3] = 9;
        inputs.camera = 1;
        const Inputs back = UnpackInputs(PackInputs(inputs));
        CHECK(back.HasViewPosition());
        CHECK_EQ(uint32_t(back.vposition), 10u);
        CHECK_EQ(uint32_t(back.targets[3]), 9u);
        CHECK_EQ(uint32_t(back.camera), 1u);
        CHECK(!back.HasDepth());
        // Every field is its own eight bits: a slot of 127 in one does not bleed into another.
        Inputs wide;
        wide.depth = 127;
        wide.camera = 13;
        wide.vposition = 127;
        for (uint32_t t = 0; t < kTargetCount; ++t)
            wide.targets[t] = uint8_t(120 + t);
        const Inputs wideBack = UnpackInputs(PackInputs(wide));
        CHECK_EQ(uint32_t(wideBack.depth), 127u);
        CHECK_EQ(uint32_t(wideBack.camera), 13u);
        CHECK_EQ(uint32_t(wideBack.vposition), 127u);
        for (uint32_t t = 0; t < kTargetCount; ++t)
            CHECK_EQ(uint32_t(wideBack.targets[t]), 120u + t);
        CHECK_EQ(PackInputs(Inputs {}), 0ull); // nothing declared is still the empty word
    }

    // ---- the exposure gate: pools only where the engine's own ground is wet
    {
        // The three measured frames pin the gate (the gloss weight is the target's alpha; the ratio is alpha / (k * wetness)):
        //   the rain hall: alpha 0.000 on every floor texel at wetness 1.0 (the engine withholds the wet look under cover),
        //   Bentbranch 10/31: alpha 0.831 on the open ground at wetness 1.0 (p5 0.545),
        //   c1: alpha 0.141 on open rock at wetness 0.187 (0.83 x wetness).
        CHECK_EQ(Exposure(0.0f, 1.0f), 0.0f);
        CHECK(Exposure(0.831f, 1.0f) > 0.999f);
        CHECK(Exposure(0.545f, 1.0f) > 0.999f); // the p5 of the open ground still reads fully exposed
        CHECK(Exposure(0.141f, 0.187f) > 0.999f);
        CHECK(std::fabs(GateRatio(0.831f, 1.0f) - 1.0f) < 0.01f); // k is the engine's alpha at full exposure and wetness 1
        CHECK(std::fabs(GateRatio(0.141f, 0.187f) - 0.908f) < 0.01f);
        CHECK_EQ(kGateK, 0.83f);

        // The band's reasons, as numbers: the top sits under the open ground's p5 ratio (0.545 / 0.83), the bottom above one 8-bit step of alpha at
        // the lowest wetness the ratio is taken at.
        CHECK(kGateBandHigh <= 0.65f);
        CHECK(kGateBandHigh > kGateBandLow);
        CHECK_EQ(Exposure(1.0f / 255.0f, kGateWetnessFloor), 0.0f); // one step of 8-bit noise opens nothing
        CHECK_EQ(Exposure(0.0f, kGateWetnessFloor), 0.0f);

        // A soft transition, not a cut: the band's ends are 0 and 1, its middle is strictly between, and the half-way ratio is one half.
        CHECK(Exposure(kGateK * kGateBandLow, 1.0f) < 1e-4f);
        CHECK(Exposure(kGateK * kGateBandHigh, 1.0f) > 1.0f - 1e-4f);
        const float middle = 0.5f * (kGateBandLow + kGateBandHigh);
        CHECK(std::fabs(Exposure(kGateK * middle, 1.0f) - 0.5f) < 1e-4f);
        CHECK(Exposure(kGateK * (middle - 0.05f), 1.0f) > 0.0f && Exposure(kGateK * (middle - 0.05f), 1.0f) < 0.5f);
        CHECK(Exposure(kGateK * (middle + 0.05f), 1.0f) > 0.5f && Exposure(kGateK * (middle + 0.05f), 1.0f) < 1.0f);

        // Monotonic in the engine's alpha, saturating at 1, and never leaving 0..1.
        float previous = -1.0f;
        for (int step = 0; step <= 255; ++step)
        {
            const float exposure = Exposure(float(step) / 255.0f, 0.6f);
            CHECK(exposure >= previous);
            CHECK(exposure >= 0.0f && exposure <= 1.0f);
            previous = exposure;
        }
        CHECK_EQ(Exposure(1.0f, 1.0f), 1.0f);
        CHECK_EQ(Exposure(50.0f, 1.0f), 1.0f);
        // A wetter frame asks for more alpha before the same ground reads exposed: the same alpha is less exposed at a higher wetness.
        CHECK(Exposure(0.3f, 1.0f) < Exposure(0.3f, 0.5f));

        // What cannot be read answers 0: a NaN or negative alpha is not exposed ground, and a pool is never the default.
        CHECK_EQ(Exposure(NaN(), 1.0f), 0.0f);
        CHECK_EQ(Exposure(-0.5f, 1.0f), 0.0f);
        CHECK_EQ(Exposure(-1.0e9f, 1.0f), 0.0f);
        CHECK_EQ(Exposure(std::numeric_limits<float>::infinity(), 1.0f), 1.0f); // +inf saturates like the shader's saturate()
        CHECK_EQ(GateRatio(NaN(), 1.0f), 0.0f);
        CHECK_EQ(GateRatio(-0.2f, 1.0f), 0.0f);

        // The wetness floor: no division by a tiny number. Below the floor the denominator stops shrinking, so the exposure falls with the wetness
        // (the pools do too) instead of reading the engine's quantised alpha as a huge ratio.
        CHECK_EQ(Exposure(kGateK * 0.001f, 0.001f), 0.0f); // exposed ground at wetness 0.001: an alpha of 0.00083, a fifth of a step
        CHECK_EQ(Exposure(0.5f, 0.001f), 1.0f);            // an alpha far above anything wetness 0.001 explains is exposed
        CHECK(std::isfinite(Exposure(0.0f, 0.0f)) && Exposure(0.0f, 0.0f) == 0.0f);
        CHECK(std::isfinite(Exposure(1.0f, 0.0f)));
        CHECK(Exposure(1.0f, 0.0f) >= 0.0f && Exposure(1.0f, 0.0f) <= 1.0f);
        CHECK_EQ(Exposure(0.3f, NaN()), Exposure(0.3f, 0.0f)); // a NaN wetness reads as none
        CHECK(Exposure(kGateK * 0.02f, 0.02f) < Exposure(kGateK * kGateWetnessFloor, kGateWetnessFloor)); // below the floor: less, not equal
        CHECK_EQ(Exposure(kGateK * kGateWetnessFloor, kGateWetnessFloor), 1.0f);                          // at the floor: the plain ratio of 1

        // Which targets carry an engine gloss to read: the material pack only. Another target's alpha is not the gloss weight.
        CHECK(GateReadable(1));
        CHECK(!GateReadable(2));
        CHECK(!GateReadable(3));
        CHECK(!GateReadable(4));
        CHECK(!GateReadable(0));

        // The whole per-texel rule with the gate: the hall's floor keeps the engine's value at full rain with a coverage of 1 (a pool anywhere
        // else), the open ground gets the pool, and the band is between.
        const Params all = BuildParams(true, 1.0f, 1.0f, 0.1f);
        CHECK_EQ(ShapedChannel(0.3f, 0.0f, true, true, 1.0f, 2.5f, 7.5f, 1.0f, all), 0.3f);
        CHECK_EQ(ShapedChannel(0.3f, 0.831f, true, true, 1.0f, 2.5f, 7.5f, 1.0f, all), 1.0f);
        const float partial = ShapedChannel(0.3f, kGateK * middle, true, true, 1.0f, 2.5f, 7.5f, 1.0f, all);
        CHECK(partial > 0.3f && partial < 1.0f);
        CHECK_EQ(ShapedChannel(0.3f, NaN(), true, true, 1.0f, 2.5f, 7.5f, 1.0f, all), 0.3f);
        Params otherTarget = all;
        otherTarget.target = 2; // the gate has no gloss to read there: no pool at all
        CHECK_EQ(ShapedChannel(0.3f, 0.831f, true, true, 1.0f, 2.5f, 7.5f, 1.0f, otherTarget), 0.3f);

        // The shader's block says whether the gate can read the target's alpha.
        const ReaderFacts gateFacts = GoodFacts();
        CHECK_EQ(ConstantsOf(gateFacts, BuildParams(true, 0.5f)).control[3], 1.0f);
        CHECK_EQ(ConstantsOf(gateFacts, BuildParams(true, 0.5f, 6.0f, 0.03f, 2)).control[3], 0.0f);
        CHECK_EQ(sizeof(Constants), 80u);

        // What the gate removed: the share of the pool field whose weight the exposure took below one half.
        const GateStats none;
        CHECK_EQ(none.RemovedShare(), 0.0f); // nothing measured: nothing removed
        const GateStats hall { 1000, 0 };
        CHECK_EQ(hall.RemovedShare(), 1.0f);
        const GateStats street { 1000, 940 };
        CHECK(std::fabs(street.RemovedShare() - 0.06f) < 1e-6f);
        const GateStats broken { 100, 400 }; // more kept than there were: never a negative share
        CHECK_EQ(broken.RemovedShare(), 0.0f);
        CHECK(GateLevelOf(1.0f) == GateLevel::Covered);
        CHECK(GateLevelOf(kGateCoveredFrom) == GateLevel::Covered);
        CHECK(GateLevelOf(0.5f) == GateLevel::Mixed);
        CHECK(GateLevelOf(kGateOpenBelow) == GateLevel::Mixed);
        CHECK(GateLevelOf(0.06f) == GateLevel::Open);
        CHECK(GateLevelOf(0.0f) == GateLevel::Open);
        CHECK(GateLevelOf(NaN()) == GateLevel::Open);
        CHECK(kGateStatsEvery >= 30 && kGateStatsStride >= 2);
        CHECK(!(GateStats { kGateMinPoolSamples - 1, 0 }.Measured())); // almost no pool in view: nothing to say about cover
        CHECK((GateStats { kGateMinPoolSamples, 0 }.Measured()));
        CHECK(!none.Measured());

        // The log lines. The rain-indoors reading is a high share and the open street's a low one, in the same sentence.
        const std::string indoors = FormatGateShare(hall);
        CHECK(indoors.find("Puddles: the engine's own ground is dry under 100% of the pool field (gate), the pools show on the rest") == 0);
        CHECK(FormatGateShare(street).find("dry under 6% of the pool field (gate)") != std::string::npos);
        CHECK(FormatGateUnreadable(2).find("Puddles:") == 0);
        CHECK(FormatGateUnreadable(2).find("target 2") != std::string::npos);
        CHECK(FormatGateUnreadable(2).find("no pool") != std::string::npos);
        CHECK(FormatGateUnreadable(2).find("G-buffer 1") != std::string::npos); // names where the engine's gloss lives
        CHECK(FormatGateUnreadable(2).find("both stages") != std::string::npos); // and that the tone stage draws none either

        // The shader mirrors these numbers (the two files change together): the constants it declares are this header's.
        const std::string gateRepo = argc > 1 ? std::string(argv[1]) : std::string();
        const std::string gateShader = ReadFixture(gateRepo, "/OptiScaler/ext/ffxiv/engine/shaders/PuddleGate.hlsli");
        CHECK(!gateShader.empty());
        CHECK_EQ(ShaderConstant(gateShader, "kGateK"), kGateK);
        CHECK_EQ(ShaderConstant(gateShader, "kGateWetnessFloor"), kGateWetnessFloor);
        CHECK_EQ(ShaderConstant(gateShader, "kGateBandLow"), kGateBandLow);
        CHECK_EQ(ShaderConstant(gateShader, "kGateBandHigh"), kGateBandHigh);
        CHECK_EQ(ShaderConstant(ReadFixture(gateRepo, "/OptiScaler/ext/ffxiv/engine/shaders/GlossPuddles.hlsl"), "kFirstOctave"), kFirstOctave);
        // ONE HLSL gate: both shaders include the file and neither declares a gate constant of its own.
        for (const char* shaderName : { "GlossPuddles.hlsl", "PuddleReflection.hlsl" })
        {
            const std::string text = ReadFixture(gateRepo, (std::string("/OptiScaler/ext/ffxiv/engine/shaders/") + shaderName).c_str());
            CHECK(text.find("#include \"PuddleGate.hlsli\"") != std::string::npos);
            CHECK(text.find("static const float kGate") == std::string::npos);
        }
    }

    // ---- the tone stage's gate: the same exposure, on the same frame's engine gloss
    {
        const float noise = 0.9f; // a field value that is a full pool at coverage 0.5, softness 0.1 and full rain
        const float open = PoolMask(noise, 0.5f, 1.0f, 0.1f);
        CHECK(open > 0.99f);
        // One gate function: the mask the tone stage shades is the ungated mask times the SAME Exposure the gloss stage uses.
        CHECK_EQ(GatedPoolMask(noise, 0.5f, 1.0f, 0.1f, 0.0f), 0.0f);                        // the hall: sheltered, no pool
        CHECK(std::fabs(GatedPoolMask(noise, 0.5f, 1.0f, 0.1f, 0.831f) - open) < 1e-6f);      // open ground: today's mask to the bit
        CHECK(std::fabs(GatedPoolMask(noise, 0.5f, 0.187f, 0.1f, 0.141f) - PoolMask(noise, 0.5f, 0.187f, 0.1f)) < 1e-6f); // c1
        const float middleAlpha = kGateK * 0.5f * (kGateBandLow + kGateBandHigh);
        CHECK(std::fabs(GatedPoolMask(noise, 0.5f, 1.0f, 0.1f, middleAlpha) - open * Exposure(middleAlpha, 1.0f)) < 1e-6f);
        CHECK(GatedPoolMask(noise, 0.5f, 1.0f, 0.1f, middleAlpha) > 0.0f && GatedPoolMask(noise, 0.5f, 1.0f, 0.1f, middleAlpha) < open);
        CHECK_EQ(GatedPoolMask(noise, 0.5f, 1.0f, 0.1f, NaN()), 0.0f);
        CHECK_EQ(GatedPoolMask(noise, 0.5f, 1.0f, 0.1f, -1.0f), 0.0f);
        CHECK_EQ(GatedPoolMask(noise, 0.5f, 0.0f, 0.1f, 0.83f), 0.0f); // a dry frame is still no pool, whatever the gloss says
        // the gate reads what the frame's composite bound: the gloss stage's own pairing, and a stage that has no gloss draws nothing
        const uint32_t kPack = 87u, kPackSrgb = 91u;
        CHECK(ToneGateReasonOf(true, true, kPack, 3840, 2160, 3840, 2160) == ToneGateReason::None);
        CHECK(ToneGateReasonOf(true, true, kPackSrgb, 3840, 2160, 3840, 2160) == ToneGateReason::None);
        CHECK(ToneGateReasonOf(false, true, kPack, 3840, 2160, 3840, 2160) == ToneGateReason::TargetNotPack); // target 2 draws none, in the tone stage too
        CHECK(ToneGateReasonOf(true, false, kPack, 3840, 2160, 3840, 2160) == ToneGateReason::NoSlot);       // the composite declares no material pack
        CHECK(ToneGateReasonOf(true, true, 10u, 3840, 2160, 3840, 2160) == ToneGateReason::BadFormat);        // a half-float scene is not the pack
        CHECK(ToneGateReasonOf(true, true, 0u, 3840, 2160, 3840, 2160) == ToneGateReason::BadFormat);
        CHECK(ToneGateReasonOf(true, true, kPack, 1920, 2160, 3840, 2160) == ToneGateReason::SizeMismatch);
        CHECK(ToneGateReasonOf(true, true, kPack, 3840, 1080, 3840, 2160) == ToneGateReason::SizeMismatch);
        // precedence: the key first, then the slot, the format, the size
        CHECK(ToneGateReasonOf(false, false, 10u, 1, 1, 2, 2) == ToneGateReason::TargetNotPack);
        CHECK(ToneGateReasonOf(true, false, 10u, 1, 1, 2, 2) == ToneGateReason::NoSlot);
        CHECK(ToneGateReasonOf(true, true, 10u, 1, 1, 2, 2) == ToneGateReason::BadFormat);
        // every reason has its own line, and none of them says a pool is drawn
        std::vector<std::string> lines;
        for (uint32_t r = 1; r < kToneGateReasonCount; ++r)
        {
            const std::string line = FormatToneGateMissing(ToneGateReason(r));
            CHECK(line.find("Puddles:") == 0);
            CHECK(line.find("tone") != std::string::npos);
            CHECK(line.find("no pool") != std::string::npos);
            for (const std::string& other : lines)
                CHECK(line != other);
            lines.push_back(line);
        }
    }

    return Finish("gloss_puddles_rules");
}
