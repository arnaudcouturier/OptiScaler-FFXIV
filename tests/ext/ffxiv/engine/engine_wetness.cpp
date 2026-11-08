// Engine pass overrides, the wetness knob's pure rules (ext/ffxiv/engine/EnginePassRules.h). No device, no shader, no file.
//
// The rules decide: which shaders the wetness knob may touch and at which slot (the packed word the pixel-shader seam stores carries both knobs'
// bits and both slots), what the set entry writes and which components it keeps, when the knob is off (-1.0 and every value below it, so a
// fresh ini is today's picture), what the feature bit needs (at least one knob off the engine's own value), which draws are candidates at all,
// which knob a refusal belongs to (a draw a knob never asked for is neither its run nor its refusal, and each knob keeps its own streak), and
// how the per-frame cache of one patched buffer moves from frame to frame.
//
// The candidate rule has no render-target or viewport fact at all. A material draw writes the five-target G-buffer and the shadow atlas's
// depth-only draws write no colour, and the wetness amount cannot change a draw that writes no colour, so both carry the patch; a state query
// per draw would cost more than the slot swap it would save.
#include "check.h"

#include "ext/ffxiv/engine/EnginePassRules.h"

#include <bit>
#include <cstring>
#include <limits>
#include <string>

using namespace ext::ffxiv::engine;

namespace
{
// The bit pattern of a float, so the fixture compares bits rather than values.
uint32_t Bits(float value) { return std::bit_cast<uint32_t>(value); }

// A layout with one name found at one slot.
PassLayout LayoutOf(uint32_t nameIndex, uint32_t slot)
{
    PassLayout layout;
    layout.found = 1u << nameIndex;
    layout.slots[nameIndex] = slot;
    return layout;
}
} // namespace

int main()
{
    // ---- the packed word: two bits and two slots in one integer, so a draw costs one relaxed load
    {
        PassLayout composite;
        composite.found = (1u << 0) | (1u << 1);
        composite.slots[0] = 2; // g_AmbientParamArray
        composite.slots[1] = 4; // g_SamplerLightDiffuse
        CHECK(IsAmbientDraw(composite));
        CHECK(!IsWetnessDraw(composite));
        const Anchors ambientOnly = DrawAnchors(composite);
        CHECK(IsAmbientAnchors(ambientOnly));
        CHECK(!IsWetnessAnchors(ambientOnly));
        CHECK_EQ(AmbientSlotOf(ambientOnly), 2u);

        PassLayout material = LayoutOf(2, 4); // g_WetnessParameter at b4, the common case of the measured session
        CHECK(IsWetnessDraw(material));
        CHECK(!IsAmbientDraw(material));
        const Anchors wetnessOnly = DrawAnchors(material);
        CHECK(IsWetnessAnchors(wetnessOnly));
        CHECK(!IsAmbientAnchors(wetnessOnly));
        CHECK_EQ(WetnessSlotOf(wetnessOnly), 4u);
        CHECK_EQ(AmbientSlotOf(wetnessOnly), 0u);

        // both names on one shader: both patches, each at its own slot, in one value
        PassLayout both = composite;
        both.found |= 1u << 2;
        both.slots[2] = 5;
        CHECK(IsAmbientDraw(both) && IsWetnessDraw(both));
        const Anchors bothAnchors = DrawAnchors(both);
        CHECK(IsAmbientAnchors(bothAnchors) && IsWetnessAnchors(bothAnchors));
        CHECK_EQ(AmbientSlotOf(bothAnchors), 2u);
        CHECK_EQ(WetnessSlotOf(bothAnchors), 5u);

        // slot zero is a real slot for both: the flags say which knob the bits belong to
        PassLayout zero = LayoutOf(2, 0);
        CHECK(IsWetnessDraw(zero));
        CHECK_EQ(WetnessSlotOf(DrawAnchors(zero)), 0u);
        CHECK(IsWetnessAnchors(DrawAnchors(zero)));

        // a slot past the last one D3D11 exposes cannot be bound by the engine, so it is not a wetness draw
        CHECK(!IsWetnessDraw(LayoutOf(2, kConstantBufferSlots)));
        CHECK(IsWetnessDraw(LayoutOf(2, kConstantBufferSlots - 1)));

        // another name of the table is neither knob's draw (cCommonTexParam is the tone map's, which the light shafts anchor, not a knob's)
        CHECK(!IsAmbientAnchors(DrawAnchors(LayoutOf(3, 1))));
        CHECK(!IsWetnessAnchors(DrawAnchors(LayoutOf(3, 1))));
        CHECK_EQ(DrawAnchors(PassLayout {}), 0u);
        CHECK(!IsAmbientAnchors(0u) && !IsWetnessAnchors(0u));

        // the two slot fields and the two flags never overlap
        CHECK_EQ(kAmbientSlotMask & kAmbientDrawBit, 0u);
        CHECK_EQ(kAmbientSlotMask & kWetnessDrawBit, 0u);
        CHECK_EQ(kWetnessSlotMask & kAmbientDrawBit, 0u);
        CHECK_EQ(kWetnessSlotMask & kWetnessDrawBit, 0u);
        CHECK_EQ(kWetnessSlotMask & kAmbientSlotMask, 0u);
        CHECK(kWetnessDrawBit != kAmbientDrawBit);
    }

    // ---- the set entry: .x and .y replaced with exact bits, .zw and the second float4 left alone
    {
        const SetEntry entry = WetnessEntry(1.0f);
        CHECK_EQ(entry.byteOffset, 0u);
        CHECK_EQ(entry.stride, 16u);
        CHECK_EQ(entry.count, 1u);
        CHECK_EQ(entry.mask, kComponentX | kComponentY);
        CHECK_EQ(entry.mask & (kComponentZ | kComponentW), 0u);
        CHECK_EQ(entry.value[0], Bits(1.0f));
        CHECK_EQ(entry.value[1], Bits(0.5f)); // 1 - 1/2, the engine's own dryness blend for full wetness
        CHECK(SetEntryFits(entry, kWetnessBytes));
        // the entry addresses the first float4 alone, so the 32-byte requirement comes from the candidate rule, not from this table
        CHECK(SetEntryFits(entry, 16));

        const SetEntry dry = WetnessEntry(0.0f);
        CHECK_EQ(dry.value[0], Bits(0.0f));
        CHECK_EQ(dry.value[1], Bits(1.0f)); // dry ground: the blend is the engine's own 1.0
        CHECK_EQ(dry.mask, kComponentX | kComponentY);

        // the value the session measured for a ramping weather: 0.46 wet, 1 - 0.46/2 = 0.77
        const SetEntry ramp = WetnessEntry(0.46f);
        CHECK_EQ(ramp.value[0], Bits(0.46f));
        CHECK_EQ(ramp.value[1], Bits(1.0f - 0.46f * 0.5f));

        // out of range values are the ends of the knob, not a different picture
        CHECK_EQ(WetnessEntry(4.0f).value[0], Bits(1.0f));
        CHECK_EQ(WetnessEntry(-3.0f).value[0], Bits(0.0f)); // the knob is off, so the entry is the dry end (the caller never builds it)

        // the table refuses what would write past the buffer
        SetEntry bad = entry;
        bad.count = 2; // both float4s of the 32-byte buffer, and exactly that
        CHECK(SetEntryFits(bad, kWetnessBytes));
        CHECK(!SetEntryFits(bad, 31));
        bad.count = 3;
        CHECK(!SetEntryFits(bad, kWetnessBytes));
        bad = entry;
        bad.count = 2;
        bad.stride = 32;
        CHECK(!SetEntryFits(bad, kWetnessBytes));
        bad = entry;
        bad.stride = 0;
        CHECK(!SetEntryFits(bad, kWetnessBytes));
        bad = entry;
        bad.stride = 24;
        CHECK(!SetEntryFits(bad, kWetnessBytes));
        bad = entry;
        bad.count = 0;
        CHECK(!SetEntryFits(bad, kWetnessBytes));
        bad = entry;
        bad.byteOffset = 8;
        CHECK(!SetEntryFits(bad, kWetnessBytes));
        bad = entry;
        bad.byteOffset = 16; // the second float4, which is the last one a 32-byte buffer has
        CHECK(SetEntryFits(bad, kWetnessBytes));
        bad.count = 2;
        CHECK(!SetEntryFits(bad, kWetnessBytes));
        CHECK(!SetEntryFits(entry, 0));
    }

    // ---- the knob's range: -1 is off, the whole dry half is off, and a broken value never turns the pass on
    {
        CHECK_EQ(ClampWetnessOverride(-1.0f), -1.0f);
        CHECK_EQ(ClampWetnessOverride(-9.0f), kMinWetnessOverride);
        CHECK_EQ(ClampWetnessOverride(9.0f), kMaxWetnessOverride);
        CHECK_EQ(ClampWetnessOverride(0.46f), 0.46f);
        CHECK_EQ(ClampWetnessOverride(-0.5f), -0.5f); // in range, and still off
        CHECK_EQ(ClampWetnessOverride(std::numeric_limits<float>::quiet_NaN()), kWetnessOverrideDefault);
        CHECK(IsWetnessOff(-1.0f));
        CHECK(IsWetnessOff(-0.5f));
        CHECK(IsWetnessOff(std::numeric_limits<float>::quiet_NaN()));
        CHECK(!IsWetnessOff(0.0f)); // forcing dry ground is a real request
        CHECK(!IsWetnessOff(1.0f));
        CHECK_EQ(WetnessAmount(-1.0f), 0.0f);
        CHECK_EQ(WetnessAmount(0.46f), 0.46f);
        CHECK_EQ(WetnessAmount(2.0f), 1.0f);

        // the default is off, so a fresh ini runs nothing and the picture is today's
        const Params defaults = BuildParams(kAmbientScaleDefault, kWetnessOverrideDefault);
        SetEntry entries[kMaxSetEntries] {};
        CHECK_EQ(WetnessEntries(defaults, entries, kMaxSetEntries), 0u);
        CHECK_EQ(WetnessEntries(defaults, nullptr, kMaxSetEntries), 0u);
        CHECK_EQ(WetnessEntries(defaults, entries, 0), 0u);
        const Params forced = BuildParams(kAmbientScaleDefault, 0.46f);
        CHECK_EQ(WetnessEntries(forced, entries, kMaxSetEntries), 1u);
        CHECK_EQ(entries[0].value[0], Bits(0.46f));
        CHECK_EQ(entries[0].mask, kComponentX | kComponentY);
        const Params wild = BuildParams(kAmbientScaleDefault, 9.0f);
        CHECK_EQ(wild.wetnessOverride, kMaxWetnessOverride);
        CHECK_EQ(WetnessEntries(wild, entries, kMaxSetEntries), 1u);
        CHECK_EQ(entries[0].value[0], Bits(1.0f));
    }

    // ---- the feature bit: the master and at least one knob off the engine's own value
    {
        CHECK(!AnyKnobOn(BuildParams(kAmbientScaleDefault, kWetnessOverrideDefault)));
        CHECK(!AnyKnobOn(BuildParams(kAmbientScaleDefault, -0.5f))); // the dry half of the wetness knob is off too
        CHECK(AnyKnobOn(BuildParams(0.7f, kWetnessOverrideDefault)));
        CHECK(AnyKnobOn(BuildParams(kAmbientScaleDefault, 0.0f)));
        CHECK(AnyKnobOn(BuildParams(kAmbientScaleDefault, 0.46f)));
        CHECK(AnyKnobOn(BuildParams(0.7f, 0.46f)));
        // the identity epsilon still applies to the ambient knob, and a broken value of either reads as the engine's own
        CHECK(!AnyKnobOn(BuildParams(1.0f + kIdentityEpsilon * 0.5f, kWetnessOverrideDefault)));
        CHECK(!AnyKnobOn(BuildParams(kAmbientScaleDefault, std::numeric_limits<float>::quiet_NaN())));
        CHECK(!AnyKnobOn(BuildParams(std::numeric_limits<float>::quiet_NaN(), kWetnessOverrideDefault)));
    }

    // ---- the candidate rule: the engine's own 32 bytes at the reflected slot, and never a replay
    {
        WetnessFacts facts;
        facts.boundBytes = kWetnessBytes;
        CHECK(IsWetnessCandidate(facts));
        facts.boundBytes = 0;
        CHECK(!IsWetnessCandidate(facts));
        facts.boundBytes = 31;
        CHECK(!IsWetnessCandidate(facts));
        facts.boundBytes = 33;
        CHECK(!IsWetnessCandidate(facts));
        facts.boundBytes = kAmbientBoundBytes; // the composite's table under the wetness name is another draw
        CHECK(!IsWetnessCandidate(facts));
        facts.boundBytes = kWetnessBytes;
        facts.replay = true;
        CHECK(!IsWetnessCandidate(facts)); // a character-preview replay of a material draw is not a draw of the frame
        facts.replay = false;
        CHECK(IsWetnessCandidate(facts));
        // the replay is named before the buffer, exactly like the composite's validation: a replay is not this module's draw whatever it bound
        facts.boundBytes = 0;
        facts.replay = true;
        CHECK(!IsWetnessCandidate(facts));
    }

    // ---- the interaction: a draw is a refusal only for the knob that asked for it, and each knob has its own streak
    {
        // The layouts of the two draws the session measured: the composite declares both of the ambient knob's anchors at b2 and b4, and a
        // material draw declares g_WetnessParameter at b4.
        PassLayout composite;
        composite.found = (1u << 0) | (1u << 1);
        composite.slots[0] = 2; // g_AmbientParamArray
        composite.slots[1] = 4; // g_SamplerLightDiffuse
        const Anchors compositeAnchors = DrawAnchors(composite);
        const Anchors materialAnchors = DrawAnchors(LayoutOf(2, 4));

        // The intent is the table builders' own answer: a knob asks for a draw exactly when its table has an entry and the draw is its kind.
        for (const Params& params : { BuildParams(kAmbientScaleDefault, kWetnessOverrideDefault), BuildParams(0.7f, kWetnessOverrideDefault),
                                      BuildParams(kAmbientScaleDefault, 0.0f), BuildParams(0.7f, 0.46f),
                                      BuildParams(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()) })
        {
            ScaleEntry scale[kMaxScaleEntries] {};
            SetEntry set[kMaxSetEntries] {};
            const bool ambientOn = AmbientEntries(params, scale, kMaxScaleEntries) != 0;
            const bool wetnessOn = WetnessEntries(params, set, kMaxSetEntries) != 0;
            CHECK_EQ(IntentOf(params, compositeAnchors).ambient, ambientOn && IsAmbientAnchors(compositeAnchors));
            CHECK_EQ(IntentOf(params, compositeAnchors).wetness, wetnessOn && IsWetnessAnchors(compositeAnchors));
            CHECK_EQ(IntentOf(params, materialAnchors).ambient, ambientOn && IsAmbientAnchors(materialAnchors));
            CHECK_EQ(IntentOf(params, materialAnchors).wetness, wetnessOn && IsWetnessAnchors(materialAnchors));
            CHECK(!IntentOf(params, materialAnchors).ambient); // a material draw is never the composite
        }

        // (a) the ambient knob at the engine's own value, the wetness knob forcing: a thousand composite draws are not refusals of anything,
        // even though the composite's facts would fail the ambient validation if it were asked.
        {
            const Params params = BuildParams(kAmbientScaleDefault, 0.5f);
            const KnobIntent intent = IntentOf(params, compositeAnchors);
            CHECK(!intent.ambient);
            CHECK(!intent.wetness);
            DrawFacts wouldFailAmbient {};
            WetnessFacts nothingBound;
            Session session;
            for (uint32_t i = 0; i < 1000; ++i)
            {
                CHECK(AmbientVerdict(intent, wouldFailAmbient).kind == KnobVerdict::NotAsked);
                CHECK(WetnessVerdict(intent, nothingBound, false, false).kind == KnobVerdict::NotAsked);
            }
            CHECK(session.Armed());
            CHECK_EQ(session.ambientDeclines, 0u);
            CHECK_EQ(session.wetnessDeclines, 0u);
        }

        // (b) the wetness knob off, the ambient knob forcing: a thousand material draws are not refusals either, and neither is a material
        // draw whose buffer under the wetness name is another size.
        {
            const Params params = BuildParams(0.7f, kWetnessOverrideDefault);
            const KnobIntent intent = IntentOf(params, materialAnchors);
            CHECK(!intent.ambient);
            CHECK(!intent.wetness);
            WetnessFacts otherBuffer;
            otherBuffer.boundBytes = 16; // under the wetness name, but not the engine's own buffer
            Session session;
            for (uint32_t i = 0; i < 1000; ++i)
            {
                CHECK(AmbientVerdict(intent, DrawFacts {}).kind == KnobVerdict::NotAsked);
                CHECK(WetnessVerdict(intent, otherBuffer, false, false).kind == KnobVerdict::NotAsked);
            }
            CHECK(session.Armed());
            CHECK_EQ(session.ambientDeclines, 0u);
            CHECK_EQ(session.wetnessDeclines, 0u);
        }

        // (c) sixty-four real refusals of one knob in a row stand the module down, once, on the sixty-fourth
        {
            const Params params = BuildParams(kAmbientScaleDefault, 0.5f);
            const KnobIntent intent = IntentOf(params, materialAnchors);
            CHECK(intent.wetness);
            WetnessFacts engineBuffer;
            engineBuffer.boundBytes = kWetnessBytes;
            CHECK(IsWetnessCandidate(engineBuffer));
            Session session;
            for (uint32_t i = 0; i + 1 < kConsecutiveDeclineLimit; ++i)
            {
                const Verdict verdict = WetnessVerdict(intent, engineBuffer, false, false); // the frame's patch budget is spent
                CHECK(verdict.kind == KnobVerdict::Refused);
                CHECK(verdict.reason == Decline::TooManyBuffers);
                CHECK(!session.NoteDecline(Knob::Wetness));
            }
            CHECK(session.Armed());
            CHECK_EQ(session.wetnessDeclines, kConsecutiveDeclineLimit - 1);
            CHECK_EQ(session.ambientDeclines, 0u);
            CHECK(session.NoteDecline(Knob::Wetness));
            CHECK(!session.Armed());
            CHECK(!session.NoteDecline(Knob::Wetness)); // the stand-down is announced once
            CHECK_EQ(session.wetnessDeclines, kConsecutiveDeclineLimit);
        }

        // (d) a draw the knob did not ask for, between refusals, is neither a refusal nor a run: the streak neither advances nor restarts
        {
            const Params params = BuildParams(kAmbientScaleDefault, 0.5f);
            const KnobIntent intent = IntentOf(params, materialAnchors);
            WetnessFacts otherBuffer;
            otherBuffer.boundBytes = 16;
            WetnessFacts engineBuffer;
            engineBuffer.boundBytes = kWetnessBytes;
            Session session;
            for (uint32_t i = 0; i + 1 < kConsecutiveDeclineLimit; ++i)
            {
                CHECK(WetnessVerdict(intent, otherBuffer, false, false).kind == KnobVerdict::NotAsked);
                CHECK(WetnessVerdict(intent, engineBuffer, false, false).kind == KnobVerdict::Refused);
                CHECK(!session.NoteDecline(Knob::Wetness));
            }
            CHECK(session.Armed());
            CHECK_EQ(session.wetnessDeclines, kConsecutiveDeclineLimit - 1);
            CHECK(session.NoteDecline(Knob::Wetness));
            CHECK(!session.Armed());
        }

        // (e) one knob's run clears its own streak alone, and a both-names draw gives each knob its own verdict
        {
            Session session;
            for (uint32_t i = 0; i + 1 < kConsecutiveDeclineLimit; ++i)
                session.NoteDecline(Knob::Ambient);
            session.NoteRun(Knob::Wetness); // 1,500 working material draws a frame must not keep the ambient knob alive
            CHECK_EQ(session.ambientDeclines, kConsecutiveDeclineLimit - 1);
            CHECK(session.NoteDecline(Knob::Ambient));
            CHECK(!session.Armed());

            PassLayout bothNames = composite;
            bothNames.found |= 1u << 2;
            bothNames.slots[2] = 5;
            WetnessFacts engineBuffer;
            engineBuffer.boundBytes = kWetnessBytes;
            const KnobIntent bothIntent = IntentOf(BuildParams(0.7f, 0.5f), DrawAnchors(bothNames));
            CHECK(bothIntent.ambient && bothIntent.wetness);
            DrawFacts scene;
            scene.boundBytes = kAmbientBoundBytes;
            scene.renderTargets = 1;
            scene.targetWidth = 1920;
            scene.targetHeight = 1080;
            scene.window[2] = 1920.0f;
            scene.window[3] = 1080.0f;
            CHECK(AmbientVerdict(bothIntent, scene).kind == KnobVerdict::Patch);
            CHECK(WetnessVerdict(bothIntent, engineBuffer, false, true).kind == KnobVerdict::Patch);
            CHECK(WetnessVerdict(bothIntent, engineBuffer, true, true).kind == KnobVerdict::Serve);
            CHECK(WetnessVerdict(bothIntent, engineBuffer, true, false).kind == KnobVerdict::Serve); // a cached copy answers with the budget spent
            // the ambient knob at its own value on the same shader: only the wetness knob asks
            const KnobIntent ambientOff = IntentOf(BuildParams(kAmbientScaleDefault, 0.5f), DrawAnchors(bothNames));
            CHECK(!ambientOff.ambient && ambientOff.wetness);
            CHECK(AmbientVerdict(ambientOff, DrawFacts {}).kind == KnobVerdict::NotAsked);
        }
    }

    // ---- the per-frame cache: one (pointer, frame, epoch, dst) entry, a patch budget per frame, and the fail-safe
    {
        FrameCache cache;
        CHECK(!cache.Has(&cache)); // nothing patched yet
        CHECK_EQ(cache.Dst(), nullptr);
        CHECK(cache.MayPatch());

        // the first token of a session starts a frame: 0 is a real token, not "no frame"
        cache.Begin(0, 0);
        CHECK(!cache.Has(&cache));
        CHECK_EQ(cache.patches, 0u);
        cache.Record(&cache, &cache);
        CHECK(cache.Has(&cache));
        CHECK_EQ(cache.Dst(), &cache);
        CHECK_EQ(cache.patches, 1u);

        // the same tokens again keep the entry: the frame's later draws bind the result the frame already patched
        cache.Begin(0, 0);
        CHECK(cache.Has(&cache));
        CHECK_EQ(cache.patches, 1u);

        // a new frame token clears the entry and the budget (the present ticked)
        cache.Begin(1, 0);
        CHECK(!cache.Has(&cache));
        CHECK_EQ(cache.patches, 0u);
        CHECK(cache.MayPatch());

        // a new epoch clears the entry even inside one frame: the knob moved, the master switched, or the patch primitive re-created the buffers
        cache.Record(&cache, &cache);
        cache.Begin(1, 1);
        CHECK(!cache.Has(&cache));
        CHECK_EQ(cache.patches, 0u);
        cache.Record(&cache, &cache);
        cache.Begin(1, 1); // and only a change clears it: the same epoch keeps the frame's entry
        CHECK(cache.Has(&cache));
        cache.Begin(2, 1);
        CHECK(!cache.Has(&cache));

        // a second pointer in one frame is a miss, and a fourth patch is the last: the fifth is the caller's TooManyBuffers
        const int buffers[5] {};
        for (uint32_t i = 0; i < kMaxPatchesPerFrame; ++i)
        {
            CHECK(cache.MayPatch());
            cache.Record(&buffers[i], &buffers[i]);
        }
        CHECK(!cache.MayPatch());
        CHECK(!cache.Has(&buffers[4]));
        // the first pointer still answers from the cache while the budget is spent
        CHECK(cache.Has(&buffers[3]));
        cache.Begin(3, 1);
        CHECK(cache.MayPatch());

        // the fail-safe: a frame token that never moves expires the entry after a long run of binds, and the budget starts over, so a session
        // whose present counter is not running never reaches TooManyBuffers - it patches once more instead. The run is 4,095 binds written out
        // (one less than the fail-safe's bound) and the constant is checked beside it: a bound of zero cannot empty the run and leave every
        // assertion below true.
        CHECK_EQ(kMaxServedDraws, 4096u);
        cache.Record(&buffers[0], &buffers[0]);
        for (uint32_t i = 0; i < kMaxPatchesPerFrame; ++i)
            cache.Record(&buffers[1], &buffers[1]);
        CHECK(!cache.MayPatch());
        for (uint32_t i = 0; i < 4095; ++i)
            cache.Serve();
        CHECK(cache.Has(&buffers[1])); // the last served bind of the run still read the cached result
        cache.Serve();
        CHECK(!cache.Has(&buffers[1]));
        CHECK_EQ(cache.patches, 0u);
        CHECK(cache.MayPatch());

        // Reset() drops the tokens too (a whole-session reset, as after a release)
        cache.Record(&buffers[0], &buffers[0]);
        cache.Reset();
        CHECK(!cache.Has(&buffers[0]));
        CHECK_EQ(cache.patches, 0u);
        CHECK_EQ(cache.served, 0u);
        CHECK(!cache.hasFrame);
        CHECK(!cache.hasEpoch);
        CHECK_EQ(cache.frame, 0u);
        CHECK_EQ(cache.epoch, 0u);
    }

    // ---- the log: the wetness line names the value, the blend and the slot, and the cost line carries both knobs' numbers
    {
        const std::string run = FormatWetnessRun(BuildParams(kAmbientScaleDefault, 1.0f), 4, kWetnessBytes, true, 0.0042);
        CHECK(run.find("1.000") != std::string::npos);
        CHECK(run.find("0.500") != std::string::npos);
        CHECK(run.find("slot 4") != std::string::npos);
        CHECK(run.find("32 bytes") != std::string::npos);
        CHECK(FormatWetnessRun(BuildParams(kAmbientScaleDefault, 0.46f), 1, kWetnessBytes, false, 0.0).find("not measured") !=
              std::string::npos);
        const std::string cost = FormatCost(CostMeter {}, 262208, 2, 3, 1500, 7, 1'000'000);
        CHECK(cost.find("2 ambient patches, 3 wetness patches on 1500 binds, 7 declined") != std::string::npos);
        CHECK(FormatSwitchedOff().find("composite") == std::string::npos); // either knob's declines reach the limit
        CHECK(FormatSwitchedOff().find("64") != std::string::npos);

        // every reason has words, including the per-frame bound, and none of them says "unknown"
        for (uint8_t i = 0; i <= uint8_t(Decline::Exception); ++i)
            CHECK(std::strcmp(DeclineName(Decline(i)), "unknown") != 0);
        CHECK(std::strcmp(DeclineName(Decline::TooManyBuffers), DeclineName(Decline::PatchFailed)) != 0);
        CHECK(FormatDecline(Decline::TooManyBuffers).find("per-frame cache") != std::string::npos);
    }

    return Finish("engine_wetness");
}
