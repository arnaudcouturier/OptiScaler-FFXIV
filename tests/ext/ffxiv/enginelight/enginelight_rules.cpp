// Engine light controls, writer 1 (eng-cb): the pure rules of the constant-buffer patch family (ext/ffxiv/enginelight/CbFamilyRules.h).
//
// What a player would feel if one of these broke: the moon dims in the wrong colour (tint channels swapped), the night weight feeds on its own
// darkened moon, the master or the A/B does not give the engine's picture back, the water's moon gets two factors at once, a character or a point
// light is dimmed with the moon, the engine's buffers are patched at the wrong offset (a different number darkens), or a published state reaches
// half a frame.
#include "check.h"

#include "ext/ffxiv/enginelight/CbFamilyRules.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace ext::ffxiv::enginelight;
using namespace ext::ffxiv::enginelight::cb; // the constant-buffer family's own namespace (look3 integration: its names collided with the lamps / exposure module's)
namespace eng = ext::ffxiv::engine;

namespace
{
Params NightParams()
{
    Params p;
    p.master = true;
    p.referenceAB = false;
    p.moonIntensity = 0.1f;
    p.moonTint[0] = 1.2f;
    p.moonTint[1] = 1.0f;
    p.moonTint[2] = 0.8f;
    p.ambientIntensity = 0.5f;
    p.ambientTint[0] = 0.9f;
    p.ambientTint[1] = 1.0f;
    p.ambientTint[2] = 1.3f;
    p.waterMoon = 0.2f;
    p.waterAmbient = 0.4f;
    p.waterTint[0] = 1.1f;
    p.waterTint[1] = 1.0f;
    p.waterTint[2] = 0.7f;
    return p;
}

bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

void Factors()
{
    const Params p = NightParams();
    // The moon: intensity x tint, per channel, in channel order.
    const Rgb m = MoonFactors(p, 1.0f);
    CHECK(Near(m.c[0], 0.1f * 1.2f));
    CHECK(Near(m.c[1], 0.1f * 1.0f));
    CHECK(Near(m.c[2], 0.1f * 0.8f));
    // The water pass's moon: waterMoon REPLACES the intensity (0.2 x tint), not 0.1 x 0.2 x tint.
    const Rgb wm = WaterMoonFactors(p, 1.0f);
    CHECK(Near(wm.c[0], 0.2f * 1.2f));
    CHECK(Near(wm.c[1], 0.2f * 1.0f));
    CHECK(Near(wm.c[2], 0.2f * 0.8f));
    CHECK(wm.c[1] > 5.0f * 0.1f * 0.2f); // mutant: stacking would be 0.02
    // The composite's ambient and the water's block.
    const Rgb a = AmbientFactors(p, 1.0f);
    CHECK(Near(a.c[0], 0.5f * 0.9f) && Near(a.c[1], 0.5f) && Near(a.c[2], 0.5f * 1.3f));
    CHECK(Near(AmbientScalar(p, 1.0f), 0.5f));
    const Rgb wa = WaterAmbientFactors(p, 1.0f);
    CHECK(Near(wa.c[0], 0.4f * 1.1f) && Near(wa.c[1], 0.4f) && Near(wa.c[2], 0.4f * 0.7f));
    CHECK(Near(WaterAmbientScalar(p, 1.0f), 0.4f));
    // Half weight: lerp(1, factor, 0.5) per factor, then the product.
    const Rgb half = MoonFactors(p, 0.5f);
    CHECK(Near(half.c[1], (1.0f + (0.1f - 1.0f) * 0.5f) * 1.0f));
    CHECK(Near(half.c[0], (1.0f + (0.1f - 1.0f) * 0.5f) * (1.0f + (1.2f - 1.0f) * 0.5f)));
    // Weight 0 (day) is the identity for every arm: nothing is patched.
    CHECK(IsIdentityRgb(MoonFactors(p, 0.0f)) && IsIdentityRgb(WaterMoonFactors(p, 0.0f)) && IsIdentityRgb(AmbientFactors(p, 0.0f)) &&
          IsIdentityRgb(WaterAmbientFactors(p, 0.0f)));
    CHECK(FamilyIdentity(p, 0.0f) && !FamilyIdentity(p, 1.0f) && !FamilyIdentity(p, 0.2f));
    // The weight outside [0, 1] clamps (the contract's Effective): a weight of 7 is 1, a negative weight 0.
    CHECK(Near(MoonFactors(p, 7.0f).c[1], 0.1f) && IsIdentityRgb(MoonFactors(p, -3.0f)));
}

void MasterAndAb()
{
    Params p = NightParams();
    p.master = false;
    CHECK(IsIdentityRgb(MoonFactors(p, 1.0f)) && IsIdentityRgb(WaterMoonFactors(p, 1.0f)) && IsIdentityRgb(AmbientFactors(p, 1.0f)) &&
          IsIdentityRgb(WaterAmbientFactors(p, 1.0f)));
    CHECK(FamilyIdentity(p, 1.0f)); // master off = the engine's own values
    p = NightParams();
    p.referenceAB = true;
    CHECK(FamilyIdentity(p, 1.0f)); // A/B = the engine's own values for one look
    // Every table is empty at identity: no copy, no dispatch, no swap.
    eng::ScaleEntry e[eng::kMaxScaleEntries];
    CHECK(LightEntries(MoonFactors(p, 1.0f), e, 8) == 0);
    CHECK(CompositeEntries(p, 1.0f, e, 8) == 0);
    CHECK(WaterAmbientEntries(p, 1.0f, e, 8) == 0);
    // A published NaN or an absurd value never darkens the game or blows it out.
    p = NightParams();
    p.moonIntensity = std::nanf("");
    p.moonTint[0] = std::nanf("");
    CHECK(Near(MoonFactors(p, 1.0f).c[0], 1.0f));
    p.moonIntensity = 1e9f;
    CHECK(MoonFactors(p, 1.0f).c[1] <= kMaxFactor);
    p.moonIntensity = -2.0f;
    CHECK(MoonFactors(p, 1.0f).c[1] >= 0.0f);
    // A control at 1 with the master on is also the identity (a thousandth is not a look).
    p = NightParams();
    p.moonIntensity = 1.0f;
    p.moonTint[0] = p.moonTint[1] = p.moonTint[2] = 1.0f;
    p.waterMoon = 1.0f;
    p.ambientIntensity = 1.0f;
    p.ambientTint[0] = p.ambientTint[1] = p.ambientTint[2] = 1.0f;
    p.waterAmbient = 1.0f;
    p.waterTint[0] = p.waterTint[1] = p.waterTint[2] = 1.0f;
    CHECK(FamilyIdentity(p, 1.0f));
    p.moonIntensity = 1.0005f;
    CHECK(FamilyIdentity(p, 1.0f));
    p.moonIntensity = 0.99f;
    CHECK(!FamilyIdentity(p, 1.0f));
    // The module that was never published starts at the engine's values, not at Params{} (master on, moon 0.1).
    const Published never;
    CHECK(!never.params.master && PublishedIdentity(never));
    CHECK(Params {}.master && !PublishedIdentity(Published { Params {}, 1.0f, 1 }));
}

void LightTable()
{
    const Params p = NightParams();
    eng::ScaleEntry e[eng::kMaxScaleEntries];
    const uint32_t n = LightEntries(MoonFactors(p, 1.0f), e, 8);
    CHECK_EQ(n, 2u);
    // Diffuse at +32 and specular at +48 of the 512-byte g_LightParam, one float4 each, .w untouched.
    CHECK_EQ(e[0].byteOffset, 32u);
    CHECK_EQ(e[1].byteOffset, 48u);
    CHECK_EQ(e[0].count, 1u);
    CHECK_EQ(e[1].count, 1u);
    CHECK_EQ(e[0].stride, 16u);
    for (uint32_t i = 0; i < 2; ++i)
    {
        CHECK(Near(e[i].multiplier[0], 0.1f * 1.2f) && Near(e[i].multiplier[1], 0.1f) && Near(e[i].multiplier[2], 0.1f * 0.8f));
        CHECK(e[i].multiplier[3] == 1.0f);
        CHECK(eng::EntryFits(e[i], kLightBoundBytes));
    }
    CHECK(LightEntries(MoonFactors(p, 1.0f), e, 1) == 0); // no room: nothing, not half a table
    CHECK(IsLightBufferSize(512) && !IsLightBufferSize(392) && !IsLightBufferSize(256) && !IsLightBufferSize(0));
    // The water pass's table is the same table with the water's factors.
    const uint32_t nw = LightEntries(WaterMoonFactors(p, 1.0f), e, 8);
    CHECK_EQ(nw, 2u);
    CHECK(Near(e[0].multiplier[1], 0.2f));
}

void CompositeTable()
{
    const Params p = NightParams();
    eng::ScaleEntry e[eng::kMaxScaleEntries];
    CHECK_EQ(CompositeEntries(p, 1.0f, e, 8), 4u);
    CHECK(CompositeEntries(p, 1.0f, e, 3) == 0);
    // The SH rows R, G, B at 64 + 16c, stride 192 over the 64 probes, each carrying that channel's factor on all four components.
    const float expect[3] = { 0.5f * 0.9f, 0.5f, 0.5f * 1.3f };
    for (uint32_t c = 0; c < 3; ++c)
    {
        CHECK_EQ(e[c].byteOffset, 64u + 16u * c);
        CHECK_EQ(e[c].stride, 192u);
        CHECK_EQ(e[c].count, 64u);
        for (int i = 0; i < 4; ++i)
            CHECK(Near(e[c].multiplier[i], expect[c]));
    }
    // The header's skyShRecipScale (byte 4 = the first float4's .y), the intensity alone.
    CHECK_EQ(e[3].byteOffset, 0u);
    CHECK_EQ(e[3].count, 1u);
    CHECK(e[3].multiplier[0] == 1.0f && Near(e[3].multiplier[1], 0.5f) && e[3].multiplier[2] == 1.0f && e[3].multiplier[3] == 1.0f);
    // The entries fit both sizes the engine binds the table at, and the family plus the shipped knob (2) stay inside the primitive's table.
    for (uint32_t i = 0; i < 4; ++i)
        CHECK(eng::EntryFits(e[i], eng::kAmbientBoundBytes) && eng::EntryFits(e[i], eng::kAmbientDeclaredBytes));
    CHECK(eng::kAmbientEntryCount + kCompositeEntryCount <= eng::kMaxScaleEntries);
    // Row offsets sit ON the SH rows the composite reads, not on the colour/recip field the shipped knob and the shadows' lift scale (112 + 192k .w).
    CHECK(e[0].byteOffset + 192u * 3u != 112u && e[2].byteOffset < 112u);
    // The family's ambient and the shipped knob's entries address different floats (the knob's are +112 and +240): they compose, never collide.
    const eng::ScaleEntry knobA = eng::AmbientSkyRecipEntry(0.7f), knobB = eng::AmbientSkyVisibilityEntry(0.7f);
    for (uint32_t c = 0; c < 4; ++c)
        CHECK(e[c].byteOffset != knobA.byteOffset && e[c].byteOffset != knobB.byteOffset);
}

void WaterTable()
{
    const Params p = NightParams();
    eng::ScaleEntry e[eng::kMaxScaleEntries];
    CHECK_EQ(WaterAmbientEntries(p, 1.0f, e, 8), 8u);
    CHECK(WaterAmbientEntries(p, 1.0f, e, 7) == 0);
    const float m[3] = { 0.4f * 1.1f, 0.4f, 0.4f * 0.7f };
    // Probe SH rows 0..2 at +0/+16/+32 and sky SH rows 6..8 at +96/+112/+128: one row per entry, that channel's factor.
    for (uint32_t c = 0; c < 3; ++c)
    {
        CHECK_EQ(e[c].byteOffset, 16u * c);
        CHECK_EQ(e[3 + c].byteOffset, 96u + 16u * c);
        for (int i = 0; i < 4; ++i)
            CHECK(Near(e[c].multiplier[i], m[c]) && Near(e[3 + c].multiplier[i], m[c]));
    }
    // The colour.xyz at +48 per channel, shRecipScale (.w) untouched; the reflection scale/offset at +80 by the intensity, bake and index untouched.
    CHECK_EQ(e[6].byteOffset, 48u);
    CHECK(Near(e[6].multiplier[0], m[0]) && Near(e[6].multiplier[1], m[1]) && Near(e[6].multiplier[2], m[2]) && e[6].multiplier[3] == 1.0f);
    CHECK_EQ(e[7].byteOffset, 80u);
    CHECK(Near(e[7].multiplier[0], 0.4f) && Near(e[7].multiplier[1], 0.4f) && e[7].multiplier[2] == 1.0f && e[7].multiplier[3] == 1.0f);
    for (uint32_t i = 0; i < 8; ++i)
        CHECK(eng::EntryFits(e[i], kWaterAmbientBoundBytes) && eng::EntryFits(e[i], kWaterAmbientDeclaredBytes));
    // No two entries address the same float4 (no double application).
    for (uint32_t i = 0; i < 8; ++i)
        for (uint32_t j = i + 1; j < 8; ++j)
            CHECK(e[i].byteOffset != e[j].byteOffset);
    CHECK(IsWaterAmbientBufferSize(256) && IsWaterAmbientBufferSize(160) && !IsWaterAmbientBufferSize(512) && !IsWaterAmbientBufferSize(0));
}

void Stamp()
{
    Declared d;
    d.lightParam = d.directionalShadow = d.maskSampler = true;
    d.lightSlot = 2;
    ShaderKind k = KindOfDeclared(d);
    CHECK(k.directional && !k.fakeSpecular && !k.water && k.lightSlot == 2);
    d.fakeSpecular = true;
    k = KindOfDeclared(d);
    CHECK(k.directional && k.fakeSpecular);
    // A point-light volume declares the light buffer and the mask but no cascade parameters; a shadow-mask reader declares the cascades and no mask.
    Declared volume = d;
    volume.directionalShadow = false;
    CHECK(!KindOfDeclared(volume).directional);
    Declared reader = d;
    reader.maskSampler = false;
    CHECK(!KindOfDeclared(reader).directional);
    Declared noLight = d;
    noLight.lightParam = false;
    CHECK(!KindOfDeclared(noLight).directional);
    Declared farSlot = d;
    farSlot.lightSlot = 14;
    CHECK(!KindOfDeclared(farSlot).directional);
    // The water surface: g_AmbientParam AND g_WaterParameter. A chara shader declares only the first.
    Declared water;
    water.ambientParam = water.waterParameter = true;
    water.ambientSlot = 3;
    k = KindOfDeclared(water);
    CHECK(k.water && k.ambientSlot == 3 && !k.directional);
    Declared chara = water;
    chara.waterParameter = false;
    CHECK(!KindOfDeclared(chara).water);
    Declared onlyWater = water;
    onlyWater.ambientParam = false;
    CHECK(!KindOfDeclared(onlyWater).water);
    // Pack / unpack round trip, and an empty stamp reads as nothing.
    for (const Declared& decl : { d, water, volume })
    {
        const ShaderKind a = KindOfDeclared(decl);
        const ShaderKind b = UnpackShaderKind(PackShaderKind(a));
        CHECK(a.directional == b.directional && a.fakeSpecular == b.fakeSpecular && a.water == b.water);
        CHECK(a.lightSlot == b.lightSlot && a.ambientSlot == b.ambientSlot);
    }
    CHECK(PackShaderKind(ShaderKind {}) == 0);
    const ShaderKind none = UnpackShaderKind(0);
    CHECK(!none.directional && !none.water);
    // Slot 0 survives the +1 packing.
    Declared slot0 = d;
    slot0.lightSlot = 0;
    CHECK(UnpackShaderKind(PackShaderKind(KindOfDeclared(slot0))).directional);
}

LightDrawFacts SceneFacts(uintptr_t rt0, uintptr_t rt1)
{
    LightDrawFacts f;
    f.rtCount = 2;
    f.bothR11G11B10 = true;
    f.width = 3840;
    f.height = 2160;
    f.rt0 = rt0;
    f.rt1 = rt1;
    f.boundBytes = 512;
    return f;
}

void Classification()
{
    ShaderKind mainLight;
    mainLight.directional = mainLight.fakeSpecular = true;
    ShaderKind plain;
    plain.directional = true;
    ShaderKind volume; // no cascade parameters: not directional
    FrameLight frame;
    frame.Begin(10);
    // The main moon: directional + fake specular + the scene-sized R11G11B10 pair.
    CHECK(ClassifyLight(mainLight, SceneFacts(1, 2), frame) == LightKind::Main);
    // Before the main moon is seen, a plain directional draw is nothing (a mid-frame arm patches neither or both).
    CHECK(ClassifyLight(plain, SceneFacts(3, 4), frame) == LightKind::None);
    frame.NoteMain(SceneFacts(1, 2));
    // The water pass's copy: same size, another pair, no fake specular.
    CHECK(ClassifyLight(plain, SceneFacts(3, 4), frame) == LightKind::Water);
    // Leftover numbers from an earlier frame do not make a main draw: without haveMain even a same-size draw on another pair is nothing.
    FrameLight stale;
    stale.width = 3840;
    stale.height = 2160;
    stale.rt0 = 1;
    stale.rt1 = 2;
    CHECK(ClassifyLight(plain, SceneFacts(3, 4), stale) == LightKind::None);
    // The same pair as the main draw is not the water pass (mutant: pair test dropped).
    CHECK(ClassifyLight(plain, SceneFacts(1, 2), frame) == LightKind::None);
    CHECK(ClassifyLight(plain, SceneFacts(1, 4), frame) == LightKind::Water); // one target differs: another pair
    // The character preview's directional light has another size (mutant: size test dropped).
    LightDrawFacts preview = SceneFacts(5, 6);
    preview.width = 512;
    preview.height = 512;
    CHECK(ClassifyLight(plain, preview, frame) == LightKind::None);
    // A volume or a shadow-mask reader is never patched.
    CHECK(ClassifyLight(volume, SceneFacts(3, 4), frame) == LightKind::None);
    // Shape: two targets in R11G11B10 at a scene's size, and not a replay.
    LightDrawFacts f = SceneFacts(3, 4);
    f.rtCount = 1;
    CHECK(ClassifyLight(plain, f, frame) == LightKind::None);
    f = SceneFacts(3, 4);
    f.bothR11G11B10 = false;
    CHECK(ClassifyLight(plain, f, frame) == LightKind::None);
    f = SceneFacts(3, 4);
    f.replay = true;
    CHECK(ClassifyLight(plain, f, frame) == LightKind::None);
    f = SceneFacts(1, 2);
    f.replay = true;
    CHECK(ClassifyLight(mainLight, f, frame) == LightKind::None);
    f = SceneFacts(1, 2);
    f.width = 160;
    f.height = 90;
    CHECK(ClassifyLight(mainLight, f, frame) == LightKind::None);
    // A new frame forgets the main draw.
    frame.Begin(11);
    CHECK(!frame.haveMain);
    CHECK(ClassifyLight(plain, SceneFacts(3, 4), frame) == LightKind::None);
    frame.Begin(11);
    frame.NoteMain(SceneFacts(1, 2));
    frame.Begin(11); // the same frame keeps it
    CHECK(frame.haveMain);
    // The water surface.
    ShaderKind water;
    water.water = true;
    WaterDrawFacts w;
    w.boundBytes = 256;
    CHECK(IsWaterCandidate(water, w));
    w.boundBytes = 160;
    CHECK(IsWaterCandidate(water, w));
    w.boundBytes = 512;
    CHECK(!IsWaterCandidate(water, w));
    w.boundBytes = 256;
    w.replay = true;
    CHECK(!IsWaterCandidate(water, w));
    w.replay = false;
    w.wholeBuffer = false; // a constant-buffer range: the plain restore would lose it, so the draw is left alone
    CHECK(!IsWaterCandidate(water, w));
    w.wholeBuffer = true;
    CHECK(!IsWaterCandidate(ShaderKind {}, w)); // a chara shader (no water stamp)
}

void LatchTest()
{
    Published a;
    a.params = NightParams();
    a.weight = 1.0f;
    a.sequence = 1;
    Latch latch;
    CHECK(PublishedIdentity(latch.active)); // before any publish: the engine's values
    CHECK(latch.Begin(100, a));            // the first arm of frame 100 latches
    CHECK(!PublishedIdentity(latch.active));
    // A publish mid-frame (a new sequence) is not applied until the next frame.
    Published b = a;
    b.params.master = false;
    b.sequence = 2;
    CHECK(!latch.Begin(100, b));
    CHECK(!PublishedIdentity(latch.active));
    CHECK(latch.Begin(101, b));
    CHECK(PublishedIdentity(latch.active));
    // The same sequence at a new frame changes nothing.
    CHECK(!latch.Begin(102, b));
    // Ownership of the ambient follows the published master.
    CHECK(OwnsAmbient(a) && !OwnsAmbient(b));
}

void SessionTest()
{
    Session s;
    for (uint32_t i = 0; i + 1 < kFailureLimit; ++i)
        CHECK(!s.NoteFailure());
    s.NoteRun();
    for (uint32_t i = 0; i + 1 < kFailureLimit; ++i)
        CHECK(!s.NoteFailure()); // a run starts the streak again
    CHECK(s.NoteFailure() && s.off);
    CHECK(!s.NoteFailure()); // the switch-off is reported once
    // The engine module's own streak is a different type: nothing here can feed it.
    eng::Session engine;
    CHECK(engine.Armed());
}

void Intent()
{
    // The family asks the composite through the seam's "ambient-table asker beyond the knob" word (EnginePass.cpp's ShadowsAmbientAsks, which
    // includes the family's ask): that is intent.shadowsAmbient, and the composite's validation answers it. A lone family ask is therefore never
    // patched without the composite's shape rules; with nobody asking, the verdict is NotAsked.
    eng::Params engineParams; // the shipped knob at 1.0
    const eng::Anchors composite = eng::kAmbientDrawBit | 2u;
    eng::DrawFacts facts;
    CHECK(eng::AmbientVerdict(eng::WithShadowsAmbient(eng::IntentOfSession(engineParams, composite, true, true), composite, false), facts).kind ==
          eng::KnobVerdict::NotAsked);
    const eng::KnobIntent intent = eng::WithShadowsAmbient(eng::IntentOfSession(engineParams, composite, true, true), composite, true);
    CHECK(intent.shadowsAmbient && !intent.ambient);
    CHECK(eng::AmbientVerdict(intent, facts).kind == eng::KnobVerdict::Refused); // no buffer, no target: a refusal, not a patch
    facts.boundBytes = eng::kAmbientBoundBytes;
    facts.renderTargets = 1;
    facts.targetWidth = 1920;
    facts.targetHeight = 1080;
    facts.window[2] = 1920.0f;
    facts.window[3] = 1080.0f;
    CHECK(eng::AmbientVerdict(intent, facts).kind == eng::KnobVerdict::Patch);
}

void Dump()
{
    const std::string header = DumpHeader();
    CHECK(header.find("frame,kind,slot,game_ptr,bytes_bound,weight,factor_r,factor_g,factor_b,before_hex,after_hex") != std::string::npos);
    const unsigned char before[4] = { 0x00, 0x7f, 0x80, 0xff };
    const unsigned char after[4] = { 0x01, 0x02, 0x03, 0x04 };
    const std::string row = DumpRow(7, PatchKind::WaterMoon, 2, 0x20413a823e0ull, 512, 0.75f, Rgb { { 0.2f, 0.3f, 0.4f } }, before, after, 4);
    CHECK(row == "7,water-moon,2,20413a823e0,512,0.750,0.2000,0.3000,0.4000,007f80ff,01020304\n");
    CHECK(std::string(PatchKindName(PatchKind::Moon)) == "moon" && std::string(PatchKindName(PatchKind::Composite)) == "composite-ambient" &&
          std::string(PatchKindName(PatchKind::WaterAmbient)) == "water-ambient");
}
} // namespace

int main()
{
    Factors();
    MasterAndAb();
    LightTable();
    CompositeTable();
    WaterTable();
    Stamp();
    Classification();
    LatchTest();
    SessionTest();
    Intent();
    Dump();
    return Finish("enginelight_rules");
}
