// Engine pass overrides: the pure rules (ext/ffxiv/engine/EnginePassRules.h). No device, no shader, no file.
//
// The rules are what keeps the module fail-open: which draw may be patched, which buffer size counts as the ambient table, when the knob is
// today's picture, when the module switches itself off, and what the log says. Every check here is a decision the draw path makes before it
// touches a binding.
#include "check.h"

#include "ext/ffxiv/engine/EnginePassRules.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

using namespace ext::ffxiv::engine;

namespace
{
// A draw that passes every rule: the composite shape measured in game (a 2560x1440 target, one render target, the 16,384-byte buffer).
DrawFacts Good()
{
    DrawFacts facts;
    facts.boundBytes = kAmbientBoundBytes;
    facts.renderTargets = 1;
    facts.targetWidth = 2560;
    facts.targetHeight = 1440;
    facts.window[0] = 0.5f;
    facts.window[1] = 0.5f;
    facts.window[2] = 2560.0f;
    facts.window[3] = 1440.0f;
    return facts;
}
} // namespace

int main()
{
    // ---- the name table: the knobs' anchors, and a name that is not there
    {
        CHECK_EQ(kNameCount, 20u); // 13 original + the march's five rows (the atlas, the cascades, the fog post's two and the material family's negative anchor) + the chara family's own g_AmbientParam row (the lighting tab's knob) + the depth of field's weight-pass buffer (no anchor bit: it only lets the creation hook reach the stage)
        CHECK_EQ(IndexOfName("cWeightSamplingParam"), 19);
        CHECK_EQ(IndexOfName("g_AmbientParamArray"), 0);
        CHECK_EQ(IndexOfName("g_SamplerLightDiffuse"), 1);
        CHECK_EQ(IndexOfName("g_WetnessParameter"), 2);
        CHECK_EQ(IndexOfName("cCommonTexParam"), 3);
        CHECK_EQ(IndexOfName("cBrightPassParam"), 4);
        CHECK_EQ(IndexOfName("g_SamplerOcclusion"), 5);
        // the light shafts' light draw, the puddles pass's four G-buffer targets, and the reflection pass's map
        CHECK_EQ(IndexOfName("g_LightParam"), 6);
        CHECK_EQ(IndexOfName("g_SamplerGBuffer1"), 7);
        CHECK_EQ(IndexOfName("g_SamplerGBuffer2"), 8);
        CHECK_EQ(IndexOfName("g_SamplerGBuffer3"), 9);
        CHECK_EQ(IndexOfName("g_SamplerGBuffer"), 10); // G-buffer 0, whose name carries no number
        CHECK_EQ(IndexOfName("g_SamplerGBuffer0"), -1);
        CHECK_EQ(IndexOfName("g_SamplerReflectionMap"), 11);
        CHECK_EQ(IndexOfName("cSunParam"), 12); // the sky's own visual sun, whose first row carries the disk's screen UV
        CHECK_EQ(IndexOfName("cSunParameter"), -1); // one character longer, and a different buffer
        CHECK_EQ(IndexOfName(""), -1);
        // the kinds: a texture is not a patch target
        CHECK(kNames[0].kind == NameKind::ConstantBuffer);
        CHECK(kNames[1].kind == NameKind::Texture);
        CHECK(kNames[5].kind == NameKind::Texture);
        CHECK(kNames[9].kind == NameKind::Texture);
        CHECK(kNames[10].kind == NameKind::Texture);
        CHECK(kNames[6].kind == NameKind::ConstantBuffer);
        CHECK(kNames[11].kind == NameKind::Texture); // the planar reflection map is a texture, not a patch target
        CHECK(kNames[12].kind == NameKind::ConstantBuffer); // the sky's sun parameters are a patch target's shape, read for its UV
        // a layout answers per name, and a slot outside the table is 0 rather than a read past its end
        PassLayout layout;
        layout.found = 1u | (1u << 3);
        layout.slots[0] = 2;
        layout.slots[3] = 0;
        CHECK(layout.Has(0) && !layout.Has(1) && layout.Has(3));
        CHECK(!layout.Has(kNameCount));
        CHECK_EQ(layout.SlotOf(0), 2u);
        CHECK_EQ(layout.SlotOf(1), 0u);
        CHECK_EQ(layout.SlotOf(kNameCount + 7), 0u);
    }

    // ---- the scale entry: the byte offset the probe layout gives, and the identity multiplier
    {
        const ScaleEntry entry = AmbientSkyVisibilityEntry(0.7f);
        CHECK_EQ(entry.byteOffset, 240u); // 0x40 header + 0xB0 skyVisibilityScale
        CHECK_EQ(entry.stride, 192u);
        CHECK_EQ(entry.count, 64u);
        CHECK_EQ(entry.multiplier[0], 0.7f);
        CHECK_EQ(entry.multiplier[1], 1.0f); // .y (ssaoMask) is not touched
        CHECK_EQ(entry.multiplier[2], 1.0f);
        CHECK_EQ(entry.multiplier[3], 1.0f);
        // it fits both sizes the engine binds
        CHECK(EntryFits(entry, kAmbientDeclaredBytes));
        CHECK(EntryFits(entry, kAmbientBoundBytes));
        // 240 + 63 * 192 + 16 = 12,352 exactly: the last addressed float4 ends on the declared end
        CHECK_EQ(240u + 63u * 192u + 16u, kAmbientDeclaredBytes);
    }

    // ---- the second entry: the probe's SH reciprocal scale, the one per-probe multiplier the composite applies to every probe it uses
    {
        const ScaleEntry entry = AmbientSkyRecipEntry(0.7f);
        CHECK_EQ(entry.byteOffset, 112u); // 0x40 header + 0x30 color.xyz / shRecipScale
        CHECK_EQ(entry.stride, 192u);
        CHECK_EQ(entry.count, 64u);
        CHECK_EQ(entry.multiplier[0], 1.0f); // .x/.y/.z (color.xyz) are not touched
        CHECK_EQ(entry.multiplier[1], 1.0f);
        CHECK_EQ(entry.multiplier[2], 1.0f);
        CHECK_EQ(entry.multiplier[3], 0.7f);
        // it fits both sizes the engine binds, and its last addressed float4 ends at 12,224 - inside the declared table
        CHECK(EntryFits(entry, kAmbientDeclaredBytes));
        CHECK(EntryFits(entry, kAmbientBoundBytes));
        CHECK_EQ(112u + 63u * 192u + 16u, 12224u);
        CHECK(12224u < kAmbientDeclaredBytes);
        // the two entries address different float4s of the same probes: one dispatch can apply both
        const ScaleEntry visibility = AmbientSkyVisibilityEntry(0.7f);
        CHECK_EQ(visibility.stride, entry.stride);
        CHECK_EQ(visibility.count, entry.count);
        CHECK(visibility.byteOffset != entry.byteOffset);
        CHECK_EQ((entry.byteOffset - visibility.byteOffset) % 16u, 0u);
        CHECK((entry.byteOffset - visibility.byteOffset) % entry.stride != 0u); // never the same float4 of a probe
    }

    // ---- the entry table refuses what would write past the buffer
    {
        ScaleEntry entry = AmbientSkyVisibilityEntry(0.5f);
        CHECK(EntryFits(entry, kAmbientDeclaredBytes));
        entry.count = 0;
        CHECK(!EntryFits(entry, kAmbientBoundBytes));
        entry = AmbientSkyVisibilityEntry(0.5f);
        entry.stride = 0;
        CHECK(!EntryFits(entry, kAmbientBoundBytes));
        entry = AmbientSkyVisibilityEntry(0.5f);
        entry.stride = 200; // not a float4 stride
        CHECK(!EntryFits(entry, kAmbientBoundBytes));
        entry = AmbientSkyVisibilityEntry(0.5f);
        entry.byteOffset = 8; // not a float4 boundary
        CHECK(!EntryFits(entry, kAmbientBoundBytes));
        entry = AmbientSkyVisibilityEntry(0.5f);
        entry.count = 65; // one probe too many for the declared table (the bound buffer has room, the declared one does not)
        CHECK(EntryFits(entry, kAmbientBoundBytes));
        CHECK(!EntryFits(entry, kAmbientDeclaredBytes));
        entry = AmbientSkyVisibilityEntry(0.5f);
        CHECK(!EntryFits(entry, 240)); // the first addressed float4 is not even there
        CHECK(!EntryFits(entry, 0));
    }

    // ---- the knob's range, and the rule that 1.0 is today's picture
    {
        CHECK_EQ(ClampAmbientScale(1.0f), 1.0f);
        CHECK_EQ(ClampAmbientScale(-1.0f), kMinAmbientScale);
        CHECK_EQ(ClampAmbientScale(9.0f), kMaxAmbientScale);
        CHECK_EQ(ClampAmbientScale(std::numeric_limits<float>::quiet_NaN()), kAmbientScaleDefault);
        CHECK(IsIdentityScale(1.0f));
        CHECK(IsIdentityScale(1.0f + kIdentityEpsilon * 0.5f));
        CHECK(IsIdentityScale(1.0f - kIdentityEpsilon * 0.5f));
        CHECK(!IsIdentityScale(0.99f));
        CHECK(!IsIdentityScale(1.01f));
        CHECK(!IsIdentityScale(0.0f));
        CHECK(IsIdentityScale(std::numeric_limits<float>::quiet_NaN())); // a broken value never turns the pass on
        // the default is the vanilla picture, so a fresh ini runs nothing
        const Params defaults = BuildParams(kAmbientScaleDefault);
        ScaleEntry entries[kMaxScaleEntries] {};
        CHECK_EQ(AmbientEntries(defaults, entries, kMaxScaleEntries), 0u);
        // a value out of range still runs, clamped: 3.0 becomes the maximum rather than being ignored
        const Params wild = BuildParams(3.0f);
        CHECK_EQ(wild.ambientScale, kMaxAmbientScale);
        CHECK_EQ(AmbientEntries(wild, entries, kMaxScaleEntries), 2u);
        CHECK_EQ(entries[0].multiplier[0], kMaxAmbientScale);
        CHECK_EQ(entries[1].multiplier[3], kMaxAmbientScale);
        // a capacity of zero writes nothing, and a caller with room for one of the two entries gets a table of none rather than half a knob
        const Params dark = BuildParams(0.7f);
        CHECK_EQ(AmbientEntries(dark, entries, 0), 0u);
        CHECK_EQ(AmbientEntries(dark, nullptr, kMaxScaleEntries), 0u);
        CHECK_EQ(AmbientEntries(dark, entries, 1), 0u);
        CHECK_EQ(entries[0].multiplier[0], kMaxAmbientScale); // a table of one entry is not written at all: the caller still holds the old one
        // the knob writes two entries, and they are the two fields the composite weights the ambient with
        CHECK_EQ(AmbientEntries(dark, entries, kMaxScaleEntries), 2u);
        CHECK_EQ(entries[0].byteOffset, 240u);
        CHECK_EQ(entries[0].stride, 192u);
        CHECK_EQ(entries[0].count, 64u);
        CHECK_EQ(entries[0].multiplier[0], 0.70f);
        CHECK_EQ(entries[0].multiplier[1], 1.0f);
        CHECK_EQ(entries[0].multiplier[2], 1.0f);
        CHECK_EQ(entries[0].multiplier[3], 1.0f);
        CHECK_EQ(entries[1].byteOffset, 112u);
        CHECK_EQ(entries[1].stride, 192u);
        CHECK_EQ(entries[1].count, 64u);
        CHECK_EQ(entries[1].multiplier[0], 1.0f);
        CHECK_EQ(entries[1].multiplier[1], 1.0f);
        CHECK_EQ(entries[1].multiplier[2], 1.0f);
        CHECK_EQ(entries[1].multiplier[3], 0.70f);
        // both entries fit the declared table the shader states, so a patch of it addresses no byte past its end
        CHECK(EntryFits(entries[0], kAmbientDeclaredBytes));
        CHECK(EntryFits(entries[1], kAmbientDeclaredBytes));
        CHECK_EQ(entries[1].byteOffset + 63u * 192u + 16u, 12224u);
    }

    // ---- the shape test: only a shader that declares both anchors is the composite, and its ambient slot travels with the answer
    {
        PassLayout both;
        both.found = (1u << 0) | (1u << 1);
        both.slots[0] = 2; // g_AmbientParamArray
        both.slots[1] = 4; // g_SamplerLightDiffuse
        CHECK(IsAmbientDraw(both));
        CHECK(IsAmbientAnchors(DrawAnchors(both)));
        CHECK_EQ(AmbientSlotOf(DrawAnchors(both)), 2u);

        // one anchor only: a wetness draw, a tone map draw or a bright pass is another draw of the frame for the composite's test, so it is
        // passed on before anything is measured or counted
        PassLayout wetness;
        wetness.found = 1u << 2;
        wetness.slots[2] = 1;
        CHECK(!IsAmbientDraw(wetness));
        CHECK(!IsAmbientAnchors(DrawAnchors(wetness)));
        CHECK(IsWetnessAnchors(DrawAnchors(wetness))); // the wetness knob's own bit, and no ambient one

        // the ambient table without the diffuse light the composite samples, and the other way round: neither is this pass
        PassLayout ambientOnly;
        ambientOnly.found = 1u << 0;
        ambientOnly.slots[0] = 2;
        CHECK(!IsAmbientDraw(ambientOnly));
        PassLayout lightOnly;
        lightOnly.found = 1u << 1;
        lightOnly.slots[1] = 4;
        CHECK(!IsAmbientDraw(lightOnly));

        // a slot past the last one D3D11 exposes cannot be bound by the engine, so the shader is not this draw either
        PassLayout past = both;
        past.slots[0] = kConstantBufferSlots;
        CHECK(!IsAmbientDraw(past));
        past.slots[0] = kConstantBufferSlots - 1;
        CHECK(IsAmbientDraw(past));
        // the last bindable slot written out: D3D11 exposes 14 constant-buffer slots, 0..13, so slot 13 is the probe table and slot 14 is
        // a slot the engine can never bind. Both ends are literals, so a bound moved by one is a different answer, not a shifted pair.
        CHECK_EQ(kConstantBufferSlots, 14u);
        past.slots[0] = 13;
        CHECK(IsAmbientDraw(past));
        past.slots[0] = 14;
        CHECK(!IsAmbientDraw(past));

        // slot zero is a real slot: a packed value of 0 is reserved for "not this draw"
        PassLayout zero = both;
        zero.slots[0] = 0;
        CHECK(IsAmbientAnchors(DrawAnchors(zero)));
        CHECK_EQ(AmbientSlotOf(DrawAnchors(zero)), 0u);

        // a layout that is not in the index at all answers the same as a geometry draw
        CHECK(!IsAmbientDraw(PassLayout {}));
        CHECK_EQ(DrawAnchors(PassLayout {}), 0u);
        CHECK(!IsAmbientAnchors(0u));
        // the flag bit is above every slot the packed value can carry, so the two never mix
        CHECK_EQ(kAmbientSlotMask & kAmbientDrawBit, 0u);
    }

    // ---- the anchors word: 64 bits, one fixed map, and the shipped knobs' values unchanged inside the low word
    {
        // the shipped layout, written out: bit 31 the composite, bit 30 the wetness name, the ambient slot in bits 0..7, the wetness slot in 8..15
        CHECK_EQ(kAmbientDrawBit, Anchors(1) << 31);
        CHECK_EQ(kWetnessDrawBit, Anchors(1) << 30);
        CHECK_EQ(kAmbientSlotMask, Anchors(0xFF));
        CHECK_EQ(kWetnessSlotMask, Anchors(0xFF) << 8);
        CHECK_EQ(kWetnessSlotShift, 8u);

        // the later knobs' fields sit above the shipped word, so no accessor of the shipped knobs can read one of them
        CHECK_EQ(kOcclusionReaderBit, Anchors(1) << 32);
        CHECK_EQ(kOcclusionReaderSlotMask, Anchors(0xFF) << 33);
        CHECK_EQ(kShaftsLightBit, Anchors(1) << 41);
        CHECK_EQ(kShaftsCompositeBit, Anchors(1) << 42);
        CHECK_EQ(kShaftsToneMapBit, Anchors(1) << 43);
        CHECK_EQ(kShaftsSlotMask, Anchors(0xFF) << 44);
        CHECK_EQ(kGlossReaderBit, Anchors(1) << 52);
        CHECK_EQ(kGlossReaderSlotMask, Anchors(0xFF) << 53);
        CHECK_EQ(kReflectionReaderBit, Anchors(1) << 61); // one bit and no slot field: the slot lives on the shader object
        CHECK_EQ(kShaftsSunParamBit, Anchors(1) << 62);   // the sky's visual sun: one bit, and its buffer slot in the light pass's field

        // the map's rule, checked again at run time so the header's assert is not the only place it holds: no two fields share a bit, and every
        // slot field is 8 contiguous bits
        constexpr uint32_t kFieldCount = uint32_t(sizeof(kAnchorsFeatureMasks) / sizeof(kAnchorsFeatureMasks[0]));
        CHECK_EQ(kFieldCount, 16u); // fifteen owned fields and the reserved range of the shipped word's free bits, plus the march's fog-post bit
        CHECK_EQ(kAnchorsReservedLowBits, Anchors(0x3FFF0000ull)); // bits 16..29: a knob written against an older map lands here and fails the map's check
        for (uint32_t i = 0; i < kFieldCount; ++i)
        {
            CHECK(kAnchorsFeatureMasks[i] != 0);
            for (uint32_t j = i + 1; j < kFieldCount; ++j)
                CHECK_EQ(kAnchorsFeatureMasks[i] & kAnchorsFeatureMasks[j], Anchors(0));
        }
        for (const Anchors field : { kAmbientSlotMask, kWetnessSlotMask, kOcclusionReaderSlotMask, kShaftsSlotMask, kGlossReaderSlotMask })
        {
            CHECK_EQ(std::popcount(field), 8);
            CHECK_EQ(field >> std::countr_zero(field), Anchors(0xFF));
        }
        CHECK(AnchorsMapIsSound()); // the same rule the header asserts

        // a 64-bit word round-trips, and the shipped knobs read their own fields only
        PassLayout both;
        both.found = (1u << 0) | (1u << 1);
        both.slots[0] = 2; // g_AmbientParamArray
        both.slots[1] = 4; // g_SamplerLightDiffuse
        PassLayout bothNames = both;
        bothNames.found |= 1u << 2;
        bothNames.slots[2] = 5; // g_WetnessParameter
        const Anchors drawn = DrawAnchors(bothNames);
        // The deferred composite is also the draw the light shafts add the shafts before, so its word carries that bit beside the ambient knob's:
        // one shader can belong to more than one knob, and each reads its own bits.
        CHECK_EQ(drawn, kAmbientDrawBit | kWetnessDrawBit | kShaftsCompositeBit | Anchors(2) | (Anchors(5) << kWetnessSlotShift));
        CHECK_EQ(drawn >> 32, kShaftsCompositeBit >> 32); // no other later knob's bit is set by a shipped layout
        CHECK(IsAmbientAnchors(drawn) && IsWetnessAnchors(drawn));
        CHECK_EQ(AmbientSlotOf(drawn), 2u);
        CHECK_EQ(WetnessSlotOf(drawn), 5u);

        // the later knobs' bits never answer a shipped accessor, alone or beside a shipped layout
        const Anchors later =
            kOcclusionReaderBit | kShaftsLightBit | kShaftsCompositeBit | kShaftsToneMapBit | kShaftsSunParamBit | kGlossReaderBit | kReflectionReaderBit;
        CHECK(!IsAmbientAnchors(later) && !IsWetnessAnchors(later));
        CHECK_EQ(AmbientSlotOf(later), 0u);
        CHECK_EQ(WetnessSlotOf(later), 0u);
        const Anchors mixed = drawn | later | (Anchors(9) << kShaftsSlotShift);
        CHECK_EQ(AmbientSlotOf(mixed), 2u);
        CHECK_EQ(WetnessSlotOf(mixed), 5u);
        CHECK(IsAmbientAnchors(mixed) && IsWetnessAnchors(mixed));
        CHECK_EQ(uint32_t(mixed), uint32_t(drawn)); // nothing above the low word moves a shipped bit
    }

    // ---- the sky's own sun: its draw is recognised by its own name alone, and its buffer slot travels in the light pass's field
    {
        PassLayout sunParam;
        sunParam.found = 1u << 12; // cSunParam
        sunParam.slots[12] = 1;
        CHECK(IsSunParamDraw(sunParam));
        CHECK(!IsLightDraw(sunParam)); // the sky's sun draw declares no g_LightParam: the two draws are never the same one
        const Anchors sunAnchors = DrawAnchors(sunParam);
        CHECK(IsShaftsSunParamAnchors(sunAnchors));
        CHECK(!IsShaftsLightAnchors(sunAnchors) && !IsShaftsCompositeAnchors(sunAnchors) && !IsShaftsToneMapAnchors(sunAnchors));
        CHECK(!IsAmbientAnchors(sunAnchors) && !IsWetnessAnchors(sunAnchors));
        CHECK_EQ(ShaftsSlotOf(sunAnchors), 1u);
        CHECK_EQ(sunAnchors, kShaftsSunParamBit | (Anchors(1) << kShaftsSlotShift)); // nothing else is set for a shader that declares cSunParam alone

        // a shader that declares both is the light pass: the sun it draws with is the one the frame's shadow cascade is built from, so the
        // light draw's own anchors win and the sky's bit is not set
        PassLayout lightAndSun;
        lightAndSun.found = (1u << 6) | (1u << 12);
        lightAndSun.slots[6] = 3;
        lightAndSun.slots[12] = 1;
        const Anchors bothAnchors = DrawAnchors(lightAndSun);
        CHECK(IsShaftsLightAnchors(bothAnchors) && !IsShaftsSunParamAnchors(bothAnchors));
        CHECK_EQ(ShaftsSlotOf(bothAnchors), 3u);

        // a slot past the last bindable one is not the sky's sun draw either, and a plain layout is not
        PassLayout pastSlot = sunParam;
        pastSlot.slots[12] = kConstantBufferSlots;
        CHECK(!IsSunParamDraw(pastSlot));
        CHECK_EQ(DrawAnchors(pastSlot), 0u);
    }

    // ---- the occlusion reader's own field: the slot sits at the reader bit's position, and D3D11's last texture slot is the bound
    {
        PassLayout reader;
        reader.found = 1u << 5; // g_SamplerOcclusion
        reader.slots[5] = 5;
        CHECK(IsOcclusionDraw(reader));
        CHECK(IsOcclusionAnchors(DrawAnchors(reader)));
        CHECK_EQ(OcclusionSlotOf(DrawAnchors(reader)), 5u);
        CHECK_EQ(kOcclusionReaderSlotShift, 33u); // the field's own position in the map, above the shipped word's free bits

        // slot 0 is a texture slot like any other; 0 means "not this draw" only as a whole word, never as a slot
        PassLayout first = reader;
        first.slots[5] = 0;
        CHECK(IsOcclusionDraw(first));
        CHECK_EQ(OcclusionSlotOf(DrawAnchors(first)), 0u);
        CHECK(IsOcclusionAnchors(DrawAnchors(first)));

        // D3D11 exposes 128 shader-resource slots, 0..127: the last one binds the engine's occlusion texture, the 128th cannot be bound at all,
        // so a shader that declares the name there is not a reader this pass serves
        PassLayout bound = reader;
        bound.slots[5] = 127;
        CHECK(IsOcclusionDraw(bound));
        bound.slots[5] = 128;
        CHECK(!IsOcclusionDraw(bound));
        CHECK(!IsOcclusionAnchors(DrawAnchors(bound)));
    }

    // ---- the frame's other indexed draws never reach the refusal counter: the shape test is decided before the draw is looked at
    {
        Session session;
        for (uint32_t i = 0; i < 200000; ++i)
        {
            PassLayout other; // the wetness shader: in the index, and not the composite
            other.found = 1u << 2;
            other.slots[2] = 1;
            if (IsAmbientDraw(other))
                session.NoteDecline(Knob::Ambient); // the draw path's order: the shape test, then the validation and its count
        }
        CHECK(session.Armed());
        CHECK_EQ(session.ambientDeclines, 0u);

        // the composite of the same frame is still looked at, and a real composite decline is what the counter counts
        PassLayout composite;
        composite.found = (1u << 0) | (1u << 1);
        composite.slots[0] = 2;
        composite.slots[1] = 4;
        CHECK(IsAmbientDraw(composite));
        DrawFacts facts = Good();
        facts.boundBytes = 0;
        const KnobIntent intent = IntentOf(BuildParams(0.7f, kWetnessOverrideDefault), DrawAnchors(composite));
        CHECK(intent.ambient);
        CHECK(AmbientVerdict(intent, facts).kind == KnobVerdict::Refused);
        if (AmbientVerdict(intent, facts).kind == KnobVerdict::Refused)
            session.NoteDecline(Knob::Ambient);
        CHECK_EQ(session.ambientDeclines, 1u);
        CHECK(session.Armed());
    }

    // ---- the features' switch-offs are independent: a stood-down knob neither patches nor switches another feature off
    {
        // The seam is entered while one of this module's knobs or any of the four features wants a draw, and only while one of them does. Each
        // feature is one argument, so the cases below are the session alone, each feature alone, and the whole set.
        CHECK(!DrawPathWanted(false, false, false, false, false));
        CHECK(DrawPathWanted(true, false, false, false, false));
        CHECK(DrawPathWanted(false, /*aoArmed*/ true, false, false, false));
        CHECK(DrawPathWanted(false, false, /*glossArmed*/ true, false, false)); // a working puddles session keeps the seam entered
        CHECK(DrawPathWanted(false, false, false, /*shaftsArmed*/ true, false)); // and so does a working light shafts session
        CHECK(DrawPathWanted(false, false, false, false, /*reflectionsArmed*/ true));
        CHECK(DrawPathWanted(true, true, true, true, true));
        CHECK(!AnyFeatureArmed(false, false, false, false));
        CHECK(AnyFeatureArmed(false, true, false, false));
        CHECK(!AnyFeatureArmed()); // a call that names no feature wants nothing

        // The case a session loses the reflections by: the ambient/wetness session stood itself down while the reflection pass wants a draw. The
        // draw path has to stay entered, or the water's slot is never swapped and the feature is dead without a line.
        CHECK(DrawPathWanted(/*sessionArmed*/ false, /*aoArmed*/ false, /*glossArmed*/ false, /*shaftsArmed*/ false, /*reflectionsArmed*/ true));
        CHECK(DrawPathWanted(/*sessionArmed*/ false, /*aoArmed*/ true, /*glossArmed*/ false, /*shaftsArmed*/ false, /*reflectionsArmed*/ false));
        CHECK(!DrawPathWanted(/*sessionArmed*/ false, /*aoArmed*/ false, /*glossArmed*/ false, /*shaftsArmed*/ false, /*reflectionsArmed*/ false));

        // A module that stood itself down patches nothing, whatever the anchors say: the draw is then the game's own call even though the seam
        // was entered for the occlusion pass.
        const Params params = BuildParams(0.7f, 0.25f);
        PassLayout composite;
        composite.found = (1u << 0) | (1u << 1);
        composite.slots[0] = 2;
        composite.slots[1] = 4;
        const KnobIntent live = IntentOfSession(params, DrawAnchors(composite), /*sessionArmed*/ true, /*masterOn*/ true);
        CHECK(live.ambient);
        const KnobIntent stoodDown = IntentOfSession(params, DrawAnchors(composite), /*sessionArmed*/ false, /*masterOn*/ true);
        CHECK(!stoodDown.ambient);
        CHECK(!stoodDown.wetness);

        // The occlusion pass's own streak is not the session's: standing the module down leaves it armed, which is what keeps the feature bit
        // on and the reader's slot swap in place.
        Session session;
        for (uint32_t i = 0; i < kConsecutiveDeclineLimit; ++i)
            session.NoteDecline(Knob::Ambient);
        CHECK(!session.Armed());
        CHECK(DrawPathWanted(session.Armed(), /*aoArmed*/ true, /*glossArmed*/ false, /*shaftsArmed*/ false, /*reflectionsArmed*/ false));
        CHECK(DrawPathWanted(session.Armed(), /*aoArmed*/ false, /*glossArmed*/ false, /*shaftsArmed*/ false, /*reflectionsArmed*/ true));
        CHECK(!DrawPathWanted(session.Armed(), /*aoArmed*/ false, /*glossArmed*/ false, /*shaftsArmed*/ false, /*reflectionsArmed*/ false));
    }

    // ---- the feature bit: the master's knobs, or any feature on its own switch (a feature arms the seam with the master key off)
    {
        // The four combinations of the master key and one feature's own switch. The bit is the seam: with the reflections on it is set even
        // though the master key is off, which is what makes a session that enables only the reflection key serve its water draws.
        CHECK(!FeatureBitWanted(/*masterOn*/ false, /*anyKnobOff*/ false, /*reflectionsArmed*/ false));
        CHECK(FeatureBitWanted(/*masterOn*/ false, /*anyKnobOff*/ false, /*reflectionsArmed*/ true));
        CHECK(!FeatureBitWanted(/*masterOn*/ true, /*anyKnobOff*/ false, /*reflectionsArmed*/ false)); // both sliders at vanilla
        CHECK(FeatureBitWanted(/*masterOn*/ true, /*anyKnobOff*/ true, /*reflectionsArmed*/ false));  // a knob off the engine's value

        // Every feature that wants a draw holds the bit, whatever the others do: the bit is cleared only when all of them stand down.
        CHECK(FeatureBitWanted(/*masterOn*/ false, /*anyKnobOff*/ false, /*aoArmed*/ false, /*glossArmed*/ false, /*shaftsArmed*/ false,
                                /*reflectionsArmed*/ true));
        CHECK(FeatureBitWanted(/*masterOn*/ false, /*anyKnobOff*/ false, /*aoArmed*/ true, /*glossArmed*/ false, /*shaftsArmed*/ false,
                                /*reflectionsArmed*/ false));
        CHECK(FeatureBitWanted(/*masterOn*/ false, /*anyKnobOff*/ false, /*aoArmed*/ false, /*glossArmed*/ true, /*shaftsArmed*/ false,
                                /*reflectionsArmed*/ false));
        CHECK(FeatureBitWanted(/*masterOn*/ false, /*anyKnobOff*/ false, /*aoArmed*/ false, /*glossArmed*/ false, /*shaftsArmed*/ true,
                                /*reflectionsArmed*/ false));
        CHECK(FeatureBitWanted(/*masterOn*/ false, /*anyKnobOff*/ false, /*aoArmed*/ true, /*glossArmed*/ true, /*shaftsArmed*/ true,
                                /*reflectionsArmed*/ true));
        CHECK(!FeatureBitWanted(/*masterOn*/ false, /*anyKnobOff*/ false, /*aoArmed*/ false, /*glossArmed*/ false, /*shaftsArmed*/ false,
                                 /*reflectionsArmed*/ false));
        // The master's own knobs and a feature's arming add up, not one over the other.
        CHECK(FeatureBitWanted(/*masterOn*/ true, /*anyKnobOff*/ true, /*aoArmed*/ false, /*glossArmed*/ false, /*shaftsArmed*/ false,
                                /*reflectionsArmed*/ false));
        CHECK(FeatureBitWanted(/*masterOn*/ true, /*anyKnobOff*/ false, /*aoArmed*/ true, /*glossArmed*/ false, /*shaftsArmed*/ false,
                                /*reflectionsArmed*/ false));
    }

    // ---- the master key gates both knobs, whatever keeps the seam entered
    {
        // The four combinations: the master key on or off, and the ambient occlusion pass on or off. With the pass on the feature bit stays set and
        // the seam is entered for the reader draws it serves, but neither knob of this key may patch a draw while the key is off, no refusal may be
        // counted for one, and the card reads "off" - the two knobs belong to the key, not to the bit.
        const Params params = BuildParams(0.7f, 0.25f);
        PassLayout both;
        both.found = (1u << 0) | (1u << 1) | (1u << 2);
        both.slots[0] = 2; // g_AmbientParamArray
        both.slots[1] = 4; // g_SamplerLightDiffuse
        both.slots[2] = 5; // g_WetnessParameter
        const Anchors anchors = DrawAnchors(both);
        WetnessFacts wetness;
        wetness.boundBytes = kWetnessBytes;
        // Both knobs off the engine's own value, one draw each of them patches: with the master on this draw is a patch, with it off it is not the
        // module's draw at all (NotAsked is the verdict a refusal cannot come from, so nothing is counted or logged against either knob).
        for (const bool master : { false, true })
        {
            for (const bool occlusion : { false, true })
            {
                CHECK(DrawPathWanted(/*sessionArmed*/ true, occlusion, /*glossArmed*/ false, /*shaftsArmed*/ false,
                                      /*reflectionsArmed*/ false)); // the pass's arming keeps the seam entered whichever way the key is
                CHECK_EQ(KnobsArmed(master, params, /*sessionArmed*/ true), master);
                CHECK(!KnobsArmed(master, params, /*sessionArmed*/ false)); // a stood-down session patches nothing, key or no key
                CHECK(!KnobsArmed(master, BuildParams(1.0f), /*sessionArmed*/ true)); // a master on with both sliders at vanilla reads off

                const KnobIntent intent = IntentOfSession(params, anchors, /*sessionArmed*/ true, master);
                CHECK_EQ(intent.ambient, master);
                CHECK_EQ(intent.wetness, master);
                const KnobVerdict ambient = AmbientVerdict(intent, Good()).kind;
                const KnobVerdict wet = WetnessVerdict(intent, wetness, /*cached*/ false, /*mayPatch*/ true).kind;
                CHECK(ambient == (master ? KnobVerdict::Patch : KnobVerdict::NotAsked));
                CHECK(wet == (master ? KnobVerdict::Patch : KnobVerdict::NotAsked));
            }
        }

        // The same fold applies to a knob whose draw has nothing to patch, and to a draw that is the other knob's: an empty intent either way.
        const KnobIntent off = IntentOfSession(params, anchors, /*sessionArmed*/ false, /*masterOn*/ true);
        CHECK(!off.ambient && !off.wetness);
        CHECK(AmbientVerdict(off, Good()).kind == KnobVerdict::NotAsked);
        CHECK(WetnessVerdict(off, wetness, /*cached*/ true, /*mayPatch*/ false).kind == KnobVerdict::NotAsked);
    }

    // ---- the validation: the draw the knob patches, and every way it is left alone
    {
        CHECK(Classify(Good()) == Decline::None);

        DrawFacts facts = Good();
        facts.boundBytes = kAmbientDeclaredBytes; // the declared size is the same table
        CHECK(Classify(facts) == Decline::None);

        facts = Good();
        facts.boundBytes = 0;
        CHECK(Classify(facts) == Decline::NoBuffer);

        facts = Good();
        facts.boundBytes = 12353; // one byte more than the declared table
        CHECK(Classify(facts) == Decline::BufferSize);

        facts = Good();
        facts.renderTargets = 0;
        CHECK(Classify(facts) == Decline::RenderTargetCount);

        facts = Good();
        facts.renderTargets = 2; // a multiple-render-target pass is not the composite
        CHECK(Classify(facts) == Decline::RenderTargetCount);

        facts = Good();
        facts.targetWidth = 1280;
        facts.targetHeight = 720;
        facts.window[2] = 1280.0f;
        facts.window[3] = 720.0f;
        CHECK(Classify(facts) == Decline::None); // half the resolution is still a scene (dynamic resolution, a smaller window)

        facts = Good();
        facts.targetWidth = 64;
        facts.targetHeight = 64;
        facts.window[2] = 64.0f;
        facts.window[3] = 64.0f;
        CHECK(Classify(facts) == Decline::RenderTargetSize);

        // a wide but short target is a preview strip or an effect's scratch surface, not the frame's lighting: the height rule stands on its
        // own, it is not a fraction of the width, so 1280x100 is refused although the width alone would pass
        facts = Good();
        facts.targetWidth = 1280;
        facts.targetHeight = 100;
        facts.window[2] = 1280.0f;
        facts.window[3] = 100.0f;
        CHECK(Classify(facts) == Decline::RenderTargetSize);
        // the smallest scene, written out: 320 x 180 is accepted and one pixel less of height is not
        CHECK_EQ(kMinSceneWidth, 320u);
        CHECK_EQ(kMinSceneHeight, 180u);
        facts = Good();
        facts.targetWidth = 320;
        facts.targetHeight = 180;
        facts.window[2] = 320.0f;
        facts.window[3] = 180.0f;
        CHECK(Classify(facts) == Decline::None);
        facts.targetHeight = 179;
        facts.window[3] = 179.0f;
        CHECK(Classify(facts) == Decline::RenderTargetSize);

        facts = Good();
        facts.window[2] = 0.0f;
        CHECK(Classify(facts) == Decline::Window);

        facts = Good();
        facts.window[3] = -1.0f;
        CHECK(Classify(facts) == Decline::Window);

        facts = Good();
        facts.window[0] = -0.5f;
        CHECK(Classify(facts) == Decline::Window);

        facts = Good();
        facts.window[2] = 2561.0f; // a window wider than the target
        CHECK(Classify(facts) == Decline::Window);

        facts = Good();
        facts.window[0] = 0.5f;
        facts.window[2] = 1200.0f; // a sub-rectangle inside a pooled target (dynamic resolution): accepted
        facts.window[3] = 700.0f;
        CHECK(Classify(facts) == Decline::None);

        facts = Good();
        facts.replay = true;
        CHECK(Classify(facts) == Decline::CharaReplay);

        // a replay is named before the buffer is looked at, because a replayed game draw is not this module's draw whatever it bound
        facts = Good();
        facts.boundBytes = 0;
        facts.replay = true;
        CHECK(Classify(facts) == Decline::CharaReplay);

        // the enum values are distinct and None is zero (the log packs the reason into a tag)
        CHECK(int(Decline::None) == 0);
        CHECK(Decline::FeatureOff != Decline::NoBuffer);
    }

    // ---- the session: sixty-four refusals in a row switch the module off, a run resets the count
    {
        Session session;
        CHECK(session.Armed());
        for (uint32_t i = 0; i + 1 < kConsecutiveDeclineLimit; ++i)
            CHECK(!session.NoteDecline(Knob::Ambient));
        CHECK(session.Armed());
        CHECK(session.NoteDecline(Knob::Ambient)); // the sixty-fourth is the one that reports it
        CHECK(!session.Armed());
        CHECK(!session.NoteDecline(Knob::Ambient)); // and it is only reported once
        CHECK_EQ(kConsecutiveDeclineLimit, 64u);

        Session other;
        for (uint32_t i = 0; i + 1 < kConsecutiveDeclineLimit; ++i)
            other.NoteDecline(Knob::Ambient);
        other.NoteRun(Knob::Ambient);
        CHECK_EQ(other.ambientDeclines, 0u);
        CHECK(other.Armed());
        for (uint32_t i = 0; i + 1 < kConsecutiveDeclineLimit; ++i)
            CHECK(!other.NoteDecline(Knob::Ambient)); // a run in between starts the count again
        CHECK(other.Armed());
    }

    // ---- the log: one line per change of state, a budget, and the words the design promises
    {
        LogGate gate;
        CHECK(gate.Take(1));
        CHECK(!gate.Take(1)); // the same state is not written twice
        CHECK(gate.Take(2));
        for (unsigned i = 0; i < kMaxLogLines; ++i)
            gate.Take(100 + i);
        CHECK(!gate.Take(999)); // the budget is spent
        CHECK_EQ(kMaxLogLines, 32u);

        CHECK(FormatDecline(Decline::BufferSize).find("16,384") != std::string::npos);
        CHECK(FormatSwitchedOff().find("64") != std::string::npos);
        const std::string run = FormatRun(BuildParams(0.7f), kAmbientBoundBytes, 2560, 1440, true, 0.0123);
        CHECK(run.find("0.700") != std::string::npos);
        CHECK(run.find("16384") != std::string::npos);
        CHECK(run.find("2560x1440") != std::string::npos);
        // the line names both fields the knob scales, because the log is the only place a session says what it changed
        CHECK(run.find("sky visibility") != std::string::npos);
        CHECK(run.find("SH reciprocal scale") != std::string::npos);
        CHECK(FormatRun(BuildParams(0.7f), kAmbientBoundBytes, 2560, 1440, false, 0.0).find("not measured") != std::string::npos);
        // every reason has words: a log line never says "unknown"
        for (uint8_t i = 0; i <= uint8_t(Decline::Exception); ++i)
            CHECK(std::strcmp(DeclineName(Decline(i)), "unknown") != 0);
    }

    // ---- the cost meter: a sample of 64 draws after 262,144 of them, and no clock between samples
    {
        CostMeter meter;
        CHECK(!meter.Sampling());
        for (uint32_t i = 0; i < kCostEveryDraws; ++i)
            meter.Skip();
        CHECK(meter.Sampling());
        for (uint32_t i = 0; i < kCostSampleDraws; ++i)
        {
            CHECK(meter.Sampling() || meter.Done());
            meter.Sample(1000);
        }
        CHECK(meter.Done());
        CHECK(!meter.Sampling());
        CHECK(meter.MillisecondsPerDraw(1'000'000) > 0.0);
        CHECK_EQ(CostMeter {}.MillisecondsPerDraw(0), 0.0);
    }

    // ---- the shader index's cap: the room rule, and the line a bytecode created past it writes
    {
        // The rule the insert reads: the index keeps its first kMaxIndexedShaders bytecodes and refuses every later one, so a session that
        // creates more than that is served the ones it saw instead of losing a late shader without a trace. Both sides of the boundary.
        CHECK(IndexHasRoom(0));
        CHECK(IndexHasRoom(kMaxIndexedShaders - 1));
        CHECK(!IndexHasRoom(kMaxIndexedShaders));
        CHECK(!IndexHasRoom(kMaxIndexedShaders + 1));
        // The cap is above the ~33,000 pixel shaders a session creates (the number the comment quotes), and the slot count keeps the table at
        // most half full: the second static_assert of the header is what carries that, and this is the value a session's capture reads.
        CHECK_EQ(kMaxIndexedShaders, 40960u);
        CHECK(kMaxIndexedShaders > 33000u);
        CHECK_EQ(kSeenSlots, 131072u);

        // The refusal's line says what happened and why the draws of that shader are left alone. It is not the table's own full line: the two
        // are different causes (the index's cap, and a probe that ran out), and a session reads the right one.
        const std::string refused = FormatIndexRefused();
        CHECK(refused.find("at its cap") != std::string::npos);
        CHECK(refused.find("40960") != std::string::npos);
        CHECK(refused.find("not indexed") != std::string::npos);
        CHECK(refused.find("left alone") != std::string::npos);
        CHECK(refused != FormatIndexFull());
        CHECK(FormatIndexFull().find("is full") != std::string::npos);
    }

    return Finish("engine_pass_rules");
}
