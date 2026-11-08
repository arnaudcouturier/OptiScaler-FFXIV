// The lighting tab's chara ambient: the pure rules (ext/ffxiv/lighting/CharaAmbientRules.h) on the host.
//
// What a player would feel if it broke: the lift at 1.0 (or the master off, or no light colour read yet, or a day frame) is byte-exact
// today's picture; the clamp keeps the slider inside its range; the one scale entry addresses the block's reciprocal-scale field and nothing
// else; the per-frame cache answers one (frame, buffer pointer) with the cache and starts a new one only when the frame or the keys change;
// and the curve is the shadow mapping module's own curve, not a copy of it.
#include "check.h"

#include "ext/ExtFeatures.h"
#include "ext/ffxiv/lighting/CharaAmbientRules.h"

#include <cmath>
#include <cstring>
#include <type_traits>

using namespace ext::ffxiv::lighting::chara;
namespace engine = ext::ffxiv::engine;
namespace shadows = ext::ffxiv::shadows;

int main()
{
    // ---- ActiveAny: ONE variadic whatever the count, so a slice that adds a draw-seam feature appends a flag and never an overload
    {
        using ext::F;
        static_assert(std::is_same_v<decltype(ext::ActiveAny(F::Nr)), bool>);
        static_assert(std::is_same_v<decltype(ext::ActiveAny(F::Nr, F::Fg, F::Hdr, F::Hudless, F::Gpu, F::Restrack, F::Present, F::Streamline)), bool>);
        // nothing active: false for every count the seams use (1, 2, 3, 4, 5 and a long list)
        CHECK(!ext::ActiveAny(F::Nr));
        CHECK(!ext::ActiveAny(F::Nr, F::Fg));
        CHECK(!ext::ActiveAny(F::Hudless, F::PlateProbe, F::Plate));
        CHECK(!ext::ActiveAny(F::ContactShadows, F::EnginePassOverrides, F::LightShafts, F::GodraysVolumetric));
        CHECK(!ext::ActiveAny(F::ContactShadows, F::EnginePassOverrides, F::LightShafts, F::GodraysVolumetric, F::CharaAmbient));
        CHECK(!ext::ActiveAny(F::Nr, F::Fg, F::Hdr, F::Hudless, F::Gpu, F::Restrack, F::Present, F::Streamline));
        // the LAST flag of the draw seam's call is honoured (a fold that dropped the final operand would miss the chara ambient)
        ext::SetActive(F::CharaAmbient, true);
        CHECK(ext::ActiveAny(F::ContactShadows, F::EnginePassOverrides, F::LightShafts, F::GodraysVolumetric, F::CharaAmbient));
        CHECK(!ext::ActiveAny(F::ContactShadows, F::EnginePassOverrides, F::LightShafts, F::GodraysVolumetric));
        CHECK(ext::ActiveAny(F::CharaAmbient));
        ext::SetActive(F::CharaAmbient, false);
        // the first, a middle and the last flag of a long call
        ext::SetActive(F::Nr, true);
        CHECK(ext::ActiveAny(F::Nr, F::Fg, F::Hdr, F::Hudless, F::Gpu, F::Restrack, F::Present, F::Streamline));
        CHECK(!ext::ActiveAny(F::Fg, F::Hdr, F::Hudless, F::Gpu, F::Restrack, F::Present, F::Streamline));
        ext::SetActive(F::Nr, false);
        ext::SetActive(F::Hudless, true);
        CHECK(ext::ActiveAny(F::Nr, F::Fg, F::Hdr, F::Hudless, F::Gpu, F::Restrack, F::Present, F::Streamline));
        CHECK(!ext::ActiveAny(F::Nr, F::Fg, F::Hdr, F::Gpu, F::Restrack, F::Present, F::Streamline));
        CHECK(ext::ActiveAny(F::Hudless, F::PlateProbe) && ext::ActiveAny(F::Hudless, F::PlateProbe, F::Plate)); // the seams of the nameplate engine
        ext::SetActive(F::Hudless, false);
        ext::SetActive(F::Streamline, true);
        CHECK(ext::ActiveAny(F::Nr, F::Fg, F::Hdr, F::Hudless, F::Gpu, F::Restrack, F::Present, F::Streamline));
        CHECK(!ext::ActiveAny(F::Nr, F::Fg, F::Hdr, F::Hudless, F::Gpu, F::Restrack, F::Present));
        ext::SetActive(F::Streamline, false);
        CHECK(!ext::ActiveAny(F::Nr, F::Fg, F::Hdr, F::Hudless, F::Gpu, F::Restrack, F::Present, F::Streamline)); // and back to quiet
    }

    // ---- the name row: the shared table carries the chara block's own row, distinct from the composite's Array row
    {
        CHECK_EQ(engine::IndexOfName("g_AmbientParam"), kCharaNameIndex); // the rules resolve the row from the table itself
        CHECK(kCharaNameIndex >= 0);
        CHECK(strcmp(engine::kNames[size_t(kCharaNameIndex)].name, "g_AmbientParam") == 0);
        CHECK(engine::kNames[size_t(kCharaNameIndex)].kind == engine::NameKind::ConstantBuffer);
        CHECK(strcmp(engine::kNames[size_t(engine::IndexOfName("g_AmbientParamArray"))].name, "g_AmbientParamArray") == 0);
        CHECK(engine::IndexOfName("g_AmbientParamArray") != kCharaNameIndex); // the composite's bigger table is a different name
    }

    // ---- which shaders are this module's: the character shading family by the two names it declares together, and the pack the stamp keeps
    {
        // The engine-wide ambient block (g_AmbientParam) is declared by the world's material shaders too (a played session's shader list:
        // 22 of its 48 declaring shaders sit at slots 5/6 beside no g_CustomizeParameter), so the name alone would patch the world; the
        // character family declares the customize block (slots 7/8/9) as well.
        CHECK(strcmp(kCharaCustomizeName, "g_CustomizeParameter") == 0);
        CHECK(IsCharaShader(true, true));
        CHECK(!IsCharaShader(true, false)); // a material shader of the world: never patched
        CHECK(!IsCharaShader(false, true)); // a customize block without the ambient block: nothing to patch
        CHECK(!IsCharaShader(false, false));

        // the pack is two nibbles of slot + 1: slot 0 is a real slot and survives the round trip, 0 means "not declared"
        CHECK_EQ(PackOf(kNoSlot, kNoSlot), 0u);
        CHECK_EQ(CharaSlotOf(0u), kNoSlot);
        CHECK_EQ(LightSlotOf(0u), kNoSlot);
        for (uint32_t slot = 0; slot < engine::kConstantBufferSlots; ++slot)
        {
            CHECK_EQ(CharaSlotOf(PackOf(slot, kNoSlot)), slot);
            CHECK_EQ(LightSlotOf(PackOf(kNoSlot, slot)), slot);
            CHECK_EQ(LightSlotOf(PackOf(slot, 13u)), 13u);  // the other nibble does not leak into this one
            CHECK_EQ(CharaSlotOf(PackOf(7u, slot)), 7u);
        }
        CHECK_EQ(CharaSlotOf(PackOf(0u, 3u)), 0u); // slot 0 is not the "none" answer
        CHECK(CharaSlotOf(PackOf(0u, 3u)) != kNoSlot);
        // a slot past b13 cannot be bound by the engine: refused at the pack, so the draw is never patched or captured
        CHECK_EQ(CharaSlotOf(PackOf(engine::kConstantBufferSlots, kNoSlot)), kNoSlot);
        CHECK_EQ(LightSlotOf(PackOf(kNoSlot, engine::kConstantBufferSlots)), kNoSlot);
        CHECK_EQ(PackOf(engine::kConstantBufferSlots, engine::kConstantBufferSlots), 0u);
        CHECK_EQ(CharaSlotOf(PackOf(9u, 4u)), 9u);
        CHECK_EQ(LightSlotOf(PackOf(9u, 4u)), 4u);
    }

    // ---- the lift key: the clamp with the NaN-identity behaviour, and the range
    {
        CHECK_EQ(ClampCharaLift(1.0f), 1.0f);
        CHECK_EQ(ClampCharaLift(0.4f), kMinCharaLift);
        CHECK_EQ(ClampCharaLift(2.4f), kMaxCharaLift);
        CHECK_EQ(ClampCharaLift(kCharaLiftDefault), 1.0f);
        CHECK_EQ(ClampCharaLift(std::nanf("")), kCharaLiftDefault); // a NaN reads as the default: a broken value never turns the pass on
        CHECK_EQ(ClampCharaLift(-0.25f), kMinCharaLift);
        CHECK(IsCharaLiftIdentity(1.0f));
        CHECK(IsCharaLiftIdentity(1.0f + engine::kIdentityEpsilon)); // a thousandth is too small to run the pass for
        CHECK(!IsCharaLiftIdentity(1.0f + 0.01f));
        CHECK(IsCharaLiftIdentity(std::nanf(""))); // and the same for the identity test
    }

    // ---- the one scale entry: the block's float4 at byte 48, its .w the scalar (byte 60), nothing else addressed
    {
        const ext::ffxiv::engine::ScaleEntry entry = CharaRecipEntry(1.5f);
        CHECK_EQ(entry.byteOffset, kCharaRecipFloat4);
        CHECK_EQ(entry.byteOffset, 48u); // the block's colour + reciprocal-scale float4
        CHECK_EQ(entry.stride, 16u);     // one float4, addressed once
        CHECK_EQ(entry.count, 1u);
        CHECK_EQ(entry.multiplier[0], 1.0f); // the colour keeps its bits through the scale
        CHECK_EQ(entry.multiplier[1], 1.0f);
        CHECK_EQ(entry.multiplier[2], 1.0f);
        CHECK_EQ(entry.multiplier[kCharaRecipComponent], 1.5f);
        CHECK_EQ(kCharaRecipComponent, 3u); // .w
        // the entry addresses the whole 48..63 float4 of a 160-byte buffer, and nothing past it
        CHECK(EntryFitsChara(entry));
        CHECK(engine::EntryFits(entry, kCharaAmbientBytes));
        CHECK(!engine::EntryFits(entry, 48u)); // a smaller buffer under the name is not this block
        // the identity entry writes no multiplier that is not 1: the compute shader's own rule keeps the other bytes bit for bit
        const ext::ffxiv::engine::ScaleEntry idle = CharaRecipEntry(1.0f);
        CHECK_EQ(idle.multiplier[3], 1.0f);
    }

    // ---- the buffer-size rule: exactly the 160-byte block shape, nothing else
    {
        CHECK(IsCharaBufferSize(160u));
        CHECK(!IsCharaBufferSize(161u));
        CHECK(!IsCharaBufferSize(48u));
        CHECK(!IsCharaBufferSize(0u));
        CHECK(!IsCharaBufferSize(16384u)); // the composite's probe array is a different table under a different name
    }

    // ---- the blend: the engine's own picture at the defaults and in day, the keyed lift at night, the curve exactly the shadows'
    {
        const float nightLum = 0.10f; // below the provisional night threshold
        const float dayLum = 1.54f;   // the day row's luminance from the same capture the curves cite
        const float midLum = 0.45f;   // between them

        // the master off: identity, whatever the keys say
        {
            const Blend blend = FrameBlend(false, 1.75f, true, nightLum);
            CHECK(!blend.runs);
            CHECK_EQ(blend.scale, 1.0f);
        }
        // no reading yet: identity, whatever the keys say (the default state is byte-exact today's picture)
        {
            const Blend blend = FrameBlend(true, 1.75f, false, 0.25f);
            CHECK(!blend.runs);
            CHECK_EQ(blend.scale, 1.0f);
            CHECK_EQ(blend.curve, 0.0f);
        }
        // the lift at its idle: identity, whatever the colour says
        {
            const Blend blend = FrameBlend(true, 1.0f, true, 2.0f);
            CHECK(!blend.runs);
            CHECK_EQ(blend.scale, 1.0f);
        }
        // night: full effect, the key itself
        {
            const Blend blend = FrameBlend(true, 1.75f, true, nightLum);
            CHECK(blend.runs);
            CHECK_EQ(blend.curve, 1.0f);
            CHECK_EQ(blend.scale, 1.75f);
        }
        // day: the engine's own picture (the curve walks the lift DOWN to identity, never through "no multiplier")
        {
            const Blend blend = FrameBlend(true, 1.75f, true, dayLum);
            CHECK(!blend.runs);
            CHECK_EQ(blend.curve, 0.0f);
            CHECK_EQ(blend.scale, 1.0f);
        }
        // between the thresholds: the anchor is 1.0, not 0
        {
            const Blend blend = FrameBlend(true, 1.75f, true, midLum);
            const float expected = 1.0f + (1.75f - 1.0f) * shadows::CurveAt(midLum);
            CHECK(std::abs(blend.scale - expected) < 0.0001f);
            CHECK_EQ(blend.curve, shadows::CurveAt(midLum)); // the curve function is the shadows module's own, not a copy
        }
        // the curve reuse identity at 1.0: this module's blend curve IS shadows' curve on every luminance
        for (float lum = 0.0f; lum <= 2.0f; lum += 0.05f)
            CHECK_EQ(FrameBlend(true, 1.0f, true, lum).curve, shadows::CurveStrength(true, true, lum));
    }

    // ---- the night reading (2026-11-07 integration): the character ambient keys on the SAME curve as shadow mapping's mask lift and night ambient -
    // the visible sun (the fog sun's y) while it is fresh, the light colour otherwise. On the rc6 session's frame 4 (fog sun y -0.012: the sun just
    // under the horizon, a daylight-looking frame) the main view's light is the dim moon-like one (luminance ~0.09): the light colour said full
    // night (1.0) while the world's curve said 0, so characters got the night lift alone.
    {
        const float moonLum = 0.09f;
        shadows::NightReading frame4;
        frame4.fogFresh = true;
        frame4.fogSunY = -0.012f;
        frame4.haveColour = true;
        frame4.luminance = moonLum;
        const Blend day = FrameBlendNight(true, 1.75f, frame4);
        CHECK_EQ(day.curve, 0.0f); // the world's curve, not the light colour's 1.0
        CHECK(!day.runs);
        CHECK_EQ(day.scale, 1.0f);

        shadows::NightReading frame2 = frame4; // fog sun y -0.109: past civil twilight
        frame2.fogSunY = -0.109f;
        const Blend night = FrameBlendNight(true, 1.75f, frame2);
        CHECK_EQ(night.curve, shadows::NightCurve(true, frame2).curve); // exactly shadow mapping's curve
        CHECK(night.curve > 0.9f && night.runs);

        shadows::NightReading stale = frame4; // the fog sun went stale: the light colour, as before
        stale.fogFresh = false;
        CHECK_EQ(FrameBlendNight(true, 1.75f, stale).curve, shadows::CurveAt(moonLum));

        shadows::NightReading none; // neither: the identity
        CHECK(!FrameBlendNight(true, 1.75f, none).runs);
        CHECK(!FrameBlendNight(false, 1.75f, frame2).runs); // the master off: identity

        // the colour-only entry is the night reading without a fog sun, on every luminance
        for (float lum = 0.0f; lum <= 2.0f; lum += 0.05f)
        {
            shadows::NightReading colourOnly;
            colourOnly.haveColour = true;
            colourOnly.luminance = lum;
            CHECK_EQ(FrameBlend(true, 1.5f, true, lum).scale, FrameBlendNight(true, 1.5f, colourOnly).scale);
        }
    }

    // ---- the curve's thresholds stay the shadows module's: the provisional numbers are not re-declared in this module
    // (the rules header re-uses the shared curve functions and declares no threshold of its own; the name-lookup test above answers the
    // structural question: the reference is to the shadows rules' functions, and no constant of this header redefines one)
    {
        CHECK_EQ(shadows::CurveAt(-1.0f), 1.0f); // below the provisional night threshold: full effect
        CHECK_EQ(shadows::CurveAt(3.0f), 0.0f);  // above the provisional day threshold: zero
    }

    // ---- the per-frame cache: one patch per (frame, buffer pointer), the epoch clearing it, the budget
    {
       CharaFrameCache cache;
        CHECK(cache.Begin(101, 7));          // the first frame starts it
        CHECK(!cache.Begin(101, 7));         // the same frame and epoch again: no reset (the ring read stays once a frame)
        CHECK(!cache.Has(nullptr) && !cache.Has((const void*)0x1));
        cache.Record((const void*)0x1000, (const void*)0x2000);
        CHECK(cache.Has((const void*)0x1000));
        CHECK_EQ(cache.Dst(), (const void*)0x2000);
        CHECK(cache.MayPatch());
        // a different buffer in the same frame: not cached, and the budget counts to its cap
        CHECK(!cache.Has((const void*)0x1001));
        for (uint32_t i = 2; i <= kMaxPatchesPerFrame; ++i)
        {
            char game = char(i * 8);
            char dst = char(i * 16);
            cache.Record(&game, &dst);
        }
        CHECK_EQ(cache.patches, kMaxPatchesPerFrame);
        CHECK(!cache.MayPatch());            // the 9th distinct pointer is passed through, logged once
        // a new epoch clears the entry first (a knob moved, a device re-created the buffers)
        CHECK(cache.Begin(101, 8));
        CHECK(!cache.valid && !cache.Has((const void*)0x1000));
        CHECK(cache.MayPatch());
        // a new frame after that starts a new one, and the served count expires after kMaxServedDraws
        CHECK(cache.Begin(102, 8));
        cache.Record((const void*)0x3000, (const void*)0x3001);
        for (uint32_t i = 1; i < kMaxServedDraws; ++i)
        {
            cache.Serve();
            CHECK(cache.Has((const void*)0x3000)); // the binds below the expiry keep the cache
        }
        cache.Serve(); // the expiry itself: the entry clears, and the next draw patches again rather than a stale value
        CHECK(!cache.Has((const void*)0x3000));
        // and a frame token of 0 first read is a frame, not a "no token yet"
        CharaFrameCache zero;
        CHECK(zero.Begin(0, 1));
        CHECK(!zero.Begin(0, 1));
    }

    // ---- the stand-down streak: sixty-four of the knob's own declines, and a run starts it over
    {
        Session session;
        for (uint32_t i = 0; i < kConsecutiveDeclineLimit - 1; ++i)
            CHECK(!session.NoteDecline());
        CHECK(session.Armed());
        CHECK(session.NoteDecline()); // the 64th switches the module off
        CHECK(!session.Armed());
        for (uint32_t i = 0; i < 10; ++i)
            CHECK(!session.NoteDecline()); // an off session stays off and light
        session.NoteRun();
        CHECK(!session.Armed()); // a run does not re-arm: only the card's re-arm does
        Session healthy;
        healthy.NoteDecline();
        healthy.NoteRun();
        for (uint32_t i = 0; i < kConsecutiveDeclineLimit - 1; ++i)
            CHECK(!healthy.NoteDecline());
        CHECK(healthy.Armed()); // 1 + 63 declines: under the limit, because the run reset the streak
    }

    // ---- the log lines: one line per state, with the numbers a session reads first
    {
        const std::string run = FormatRun(1.75f, 0.8f, 9u, 160u, true, 0.0123);
        CHECK(run.find("1.750") != std::string::npos);
        CHECK(run.find("curve 0.800") != std::string::npos);
        CHECK(run.find("provisional") != std::string::npos); // the thresholds are marked as provisional in the product text too
        CHECK(run.find("160 bytes at slot 9") != std::string::npos);
        const std::string gpuless = FormatRun(1.75f, 0.8f, 9u, 160u, false, 0.0);
        CHECK(gpuless.find("not measured yet") != std::string::npos);
        CHECK(FormatSwitchedOff().find("64") != std::string::npos);
        CHECK(FormatWaitingForColour().find("light colour") != std::string::npos);
        CHECK(FormatBudgetSpent(kMaxPatchesPerFrame).find("8") != std::string::npos);
        CHECK(strcmp(DeclineName(Decline::TooManyBuffers), "the frame bound more chara ambient blocks than the per-frame budget patches") == 0);
        CHECK(strcmp(DeclineName(Decline::None), "run") == 0);
    }

    return Finish("lighting_rules");
}
