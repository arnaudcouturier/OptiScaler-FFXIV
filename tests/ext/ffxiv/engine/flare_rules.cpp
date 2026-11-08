// Lens flare rules (ext/ffxiv/engine/FlareRules.h): every decision the card makes without a device.
//
// What is pinned here is what a wrong rule would break in a session: the four reflected names the recognition keys on (never a CRC), the
// classification of the family's own four pixel shaders on their real bytecodes (the fixtures are the capture's CRCs), the stamp's packing,
// the three buffers' sizes as bound and declared with the capture's technical offsets, the three entries (their offsets and multipliers,
// their fit against both widths), the parts bitfield's decode and clamps, the three scale keys' clamps and identity gate, the verdicts per
// family (the veil's split from the exposure-meter readers by the target's size, the replay refusal, the sun's patch orphaning the ghost
// step), the refusal streak and the log gate's budget, and the lines a session reads back.
#include "check.h"

#include "ext/ffxiv/diag/ShaderBindings.h"
#include "ext/ffxiv/engine/FlareRules.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace ext::ffxiv::flare;
namespace engine = ext::ffxiv::engine;
namespace diag = ext::ffxiv::diag;

namespace
{
float NaN() { return std::numeric_limits<float>::quiet_NaN(); }

// One binding of a hand-built reflection row, with the name's length taken from the literal so a typo in a count cannot pass.
diag::ShaderBinding Bound(const char* name, uint32_t type, uint32_t slot)
{
    return diag::ShaderBinding { name, uint32_t(std::string_view(name).size()), type, slot, 1 };
}

// The real blobs the capture identifies: the CRCs and the byte sizes the flare study's rows name.
const char* kHalo = "/tests/ext/ffxiv/engine/fixtures/halo_0x9B2E1FE1.dxbc";
const char* kSun = "/tests/ext/ffxiv/engine/fixtures/sun_0xBBCB756B.dxbc";
const char* kGhost = "/tests/ext/ffxiv/engine/fixtures/radialblur_0x6C9EC0F5.dxbc";
const char* kVeil = "/tests/ext/ffxiv/engine/fixtures/lensflareps_0xE1EECC6A.dxbc";
const char* kMeter = "/tests/ext/ffxiv/engine/fixtures/measurelumfinal_0xCC67540A.dxbc";

std::vector<char> ReadBlob(const std::string& repo, const char* relative)
{
    std::ifstream file(repo + relative, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Classify one real blob the way the creation hook does: the scan, the parse, the layout, the stamp.
uint32_t StampOfBlob(const std::string& repo, const char* relative)
{
    const std::vector<char> blob = ReadBlob(repo, relative);
    CHECK(!blob.empty());
    CHECK(MentionsFlareName(blob.data(), blob.size()));
    diag::ShaderBinding parsed[diag::kMaxBindings];
    const size_t count = diag::ParseBindings(blob.data(), blob.size(), parsed, diag::kMaxBindings);
    CHECK(count > 0);
    const FlareLayout layout = LayoutOf(parsed, count);
    return StampOfLayout(layout);
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";

    // ---- the names: four reflected rows, and the scan that drops what cannot be a flare draw
    {
        CHECK_EQ(kFlareNameCount, 4u);
        CHECK(std::strcmp(kFlareNames[0].name, "cHaloParam") == 0 &&
                  kFlareNames[0].kind == diag::BindKind::ConstantBuffer);
        CHECK(std::strcmp(kFlareNames[1].name, "cSunParam") == 0 && kFlareNames[1].kind == diag::BindKind::ConstantBuffer);
        CHECK(std::strcmp(kFlareNames[2].name, "cRadialBlurParam") == 0 && kFlareNames[2].kind == diag::BindKind::ConstantBuffer);
        CHECK(std::strcmp(kFlareNames[3].name, "sToneMap") == 0 && kFlareNames[3].kind == diag::BindKind::Texture);
        CHECK(MentionsFlareName(nullptr, 0) == false);
        const char* tooShort = "cHalo";
        CHECK(MentionsFlareName(tooShort, 5) == false);
        CHECK(MentionsFlareName("(what if the game's comment names cHaloParam?)",
                                 std::strlen("(what if the game's comment names cHaloParam?)")));
        // The engine module's own name table is untouched: the card's names are not in it, and its soundness holds.
        CHECK_EQ(engine::IndexOfName("cHaloParam"), -1);
        CHECK_EQ(engine::IndexOfName("cRadialBlurParam"), -1);
        CHECK_EQ(engine::IndexOfName("sToneMap"), -1);
        CHECK(engine::AnchorsMapIsSound());
        // The stamps: family in the high bits, the slot in the low eight, the veil's own 0xFF slot bits.
        CHECK_EQ(StampOfLayout(FlareLayout {}), 0u);
        CHECK_EQ(StampOf(Family::None, 1), 0u);
        CHECK_EQ(StampOf(Family::Ring, kStampSlotMask + 1), 0u);
        CHECK_EQ(StampOf(Family::Ring, 1), (1u << kStampSlotShift) | 1u);
        CHECK_EQ(FamilyOf((1u << kStampSlotShift) | 1u), Family::Ring);
        CHECK_EQ(FamilyOf(0), Family::None);
        CHECK_EQ(StampSlotOf((2u << kStampSlotShift) | 5u), 5u);
        for (const uint32_t beyond : { 5u, 6u, 64u })
            CHECK_EQ(FamilyOf(beyond << kStampSlotShift), Family::None);
        CHECK_EQ(StampSlotOf(StampOf(Family::Veil, kStampSlotMask)), kStampSlotMask);
        CHECK(StampOf(Family::Veil, kStampSlotMask) != 0u);
    }

    // ---- the classification of the family's own shaders, on their real bytecodes: the four names never mix
    {
        // The capture's CRCs: the ring's blob is the one that declares cHaloParam, and the four never overlap.
        CHECK_EQ(StampOfBlob(repo, kHalo), StampOf(Family::Ring, 1));       // cHaloParam binds at slot 1 (the capture's row)
        CHECK_EQ(StampOfBlob(repo, kSun), StampOf(Family::Sun, 1));         // cSunParam at slot 1, 128 bytes
        CHECK_EQ(StampOfBlob(repo, kGhost), StampOf(Family::Ghost, 1));     // cRadialBlurParam at slot 1, 16 bytes
        CHECK_EQ(StampOfBlob(repo, kVeil), StampOf(Family::Veil, kStampSlotMask));
        // The exposure-meter reader's reflection is the veil's too (sToneMap, no constant buffers): the split is the draw's, not the parse's.
        CHECK_EQ(StampOfBlob(repo, kMeter), StampOf(Family::Veil, kStampSlotMask));
        // ...and the sun's blob alone carries sGeometry with its cSunParam, so the shafts' draw is answered by both cards on the same terms.
        CHECK(MentionsFlareName("cSunParam", 9));
    }

    // ---- hand-built layouts: the exclusions the real tables only happen to satisfy
    {
        {
            diag::ShaderBinding rows[] = { Bound("cHaloParam", diag::kBindConstantBuffer, 1),
                                           Bound("cSunParam", diag::kBindConstantBuffer, 2) };
            const FlareLayout layout = LayoutOf(rows, 2);
            CHECK_EQ(layout.constantBuffers, 2u);
            CHECK_EQ(StampOfLayout(layout), 0u); // a draw that names two of the three is not this card's draw
        }
        {
            diag::ShaderBinding rows[] = { Bound("sToneMap.S", diag::kBindSampler, 0), Bound("sToneMap.T", diag::kBindTexture, 0),
                                           Bound("cAdaptLumParam", diag::kBindConstantBuffer, 0) };
            const FlareLayout layout = LayoutOf(rows, 3);
            CHECK_EQ(layout.constantBuffers, 1u);
            CHECK_EQ(StampOfLayout(layout), 0u); // the veil's name beside one constant buffer is not the veil (AdaptLum)
        }
        {
            diag::ShaderBinding rows[] = { Bound("sToneMap.S", diag::kBindSampler, 0), Bound("sToneMap.T", diag::kBindTexture, 0) };
            const FlareLayout layout = LayoutOf(rows, 2);
            CHECK_EQ(layout.constantBuffers, 0u);
            CHECK_EQ(StampOfLayout(layout), StampOf(Family::Veil, kStampSlotMask));
        }
        // A sampler that carries the veil's name without its texture is not the veil's input: the kind is part of the match.
        {
            diag::ShaderBinding rows[] = { Bound("sToneMap.S", diag::kBindSampler, 0) };
            CHECK_EQ(StampOfLayout(LayoutOf(rows, 1)), 0u);
        }
        // Null and empty: not a layout this card answers.
        CHECK_EQ(StampOfLayout(LayoutOf(nullptr, 0)), 0u);
        CHECK_EQ(StampOfLayout(LayoutOf(nullptr, 3)), 0u);
    }

    // ---- the buffers: the capture's bound sizes beside the declared ones, and the capture's technical offsets
    {
        CHECK_EQ(kRingDeclaredBytes, 48u);
        CHECK_EQ(kRingBufferBytes, 64u);  // the capture's row: cHaloParam, slot 1, 64 bytes
        CHECK_EQ(kSunDeclaredBytes, 80u);
        CHECK_EQ(kSunBufferBytes, 128u);  // the capture's row: cSunParam, slot 1, 128 bytes
        CHECK_EQ(kGhostDeclaredBytes, 16u);
        CHECK_EQ(kGhostBufferBytes, 16u);
        CHECK_EQ(kRingColourOffset, 0x20u); // the ring's colour row: the capture's float4 at +0x20
        CHECK_EQ(kSunColourOffset, 0x40u);  // the sun's colour row: the capture's float4 at +0x40
        CHECK_EQ(kGhostStepOffset, 0x0Cu);  // the ghost step: the .w of the float4 at +0x00 (the entry's own byteOffset)
        CHECK(IsBufferSize(Family::Ring, 64));
        CHECK(IsBufferSize(Family::Sun, 128));
        CHECK(IsBufferSize(Family::Ghost, 16));
        CHECK(!IsBufferSize(Family::Ring, 48)); // a declared-size binding is another pool's, not this draw's patch
        CHECK(!IsBufferSize(Family::Sun, 80));
        CHECK(!IsBufferSize(Family::Ghost, 32));
        CHECK(!IsBufferSize(Family::Veil, 64)); // the veil is never patched
        CHECK_EQ(DeclaredBytesOf(Family::Ring), 48u);
        CHECK_EQ(DeclaredBytesOf(Family::Veil), 0u);
        // The entries fit the bound widths and the declared widths both (the proposal's own pin).
        CHECK(EntryFitsFamily(Family::Ring, kRingBufferBytes));
        CHECK(EntryFitsFamily(Family::Sun, kSunBufferBytes));
        CHECK(EntryFitsFamily(Family::Ghost, kGhostBufferBytes));
        CHECK(!EntryFitsFamily(Family::Ring, 32)); // past the ring's colour row: not a table this entry addresses
        CHECK(!EntryFitsFamily(Family::Sun, 48));
    }

    // ---- the entries: offsets and multipliers, and what a frame at the defaults emits (nothing)
    {
        const ext::ffxiv::engine::ScaleEntry ring = RingEntry(0.5f);
        CHECK_EQ(ring.byteOffset, kRingColourOffset);
        CHECK_EQ(ring.stride, 16u);
        CHECK_EQ(ring.count, 1u);
        CHECK_EQ(ring.multiplier[0], 0.5f);
        CHECK_EQ(ring.multiplier[1], 0.5f);
        CHECK_EQ(ring.multiplier[2], 0.5f);
        CHECK_EQ(ring.multiplier[3], 1.0f); // the mask fill's own value stays
        const ext::ffxiv::engine::ScaleEntry sun = SunEntry(0.25f);
        CHECK_EQ(sun.byteOffset, kSunColourOffset);
        CHECK_EQ(sun.multiplier[0], 0.25f);
        CHECK_EQ(sun.multiplier[1], 0.25f);
        CHECK_EQ(sun.multiplier[2], 0.25f);
        CHECK_EQ(sun.multiplier[3], 1.0f);
        const ext::ffxiv::engine::ScaleEntry ghost = GhostEntry(2.0f);
        CHECK_EQ(ghost.byteOffset, 0x00u);  // the float4 whose .w is the +0x0C field
        CHECK_EQ(ghost.multiplier[3], 2.0f);
        CHECK_EQ(ghost.multiplier[0], 1.0f);
        CHECK_EQ(ghost.multiplier[1], 1.0f);
        CHECK_EQ(ghost.multiplier[2], 1.0f);
        // The identity: every draw at the engine's own values, the card's frame is the game's own picture.
        CHECK_EQ(RingEntry(1.0f).multiplier[0], 1.0f);
        CHECK_EQ(EntryOf(Family::Veil, 1.0f).byteOffset, 0u); // the veil's entry is the default: no offset, but also
        CHECK(!engine::EntryFits(EntryOf(Family::Veil, 1.0f), 64)); // no stride (0), so it can never become a patch
    }

    // ---- the parts bitfield: the brief's three bits, clamped, and today's picture at the defaults
    {
        CHECK_EQ(ClampParts(-1), 0u);
        CHECK_EQ(ClampParts(0), 0u);
        CHECK_EQ(ClampParts(3), 3u);
        CHECK_EQ(ClampParts(7), 7u);
        CHECK_EQ(ClampParts(8), 7u);
        CHECK_EQ(ClampParts(255), 7u);
        Params params = BuildParams(1.0f, 1.0f, 1.0f, 7);
        CHECK_EQ(params.parts, kFlarePartsAll);
        CHECK(VeilSkipAsked(params) == false);
        CHECK(AnyFlareChange(params) == false); // the defaults are the game's own picture
        params = BuildParams(1.0f, 1.0f, 1.0f, 3);
        CHECK(AnyFlareChange(params) == true);  // the veil's bit clear: the skip is the frame's own state change
        params = BuildParams(0.0f, 1.0f, 1.0f, 7);
        CHECK(AnyFlareChange(params) == true);  // a part at removal
        params = BuildParams(1.0f, 1.0f, 0.5f, 7);
        CHECK(AnyFlareChange(params) == true);
        params = BuildParams(NaN(), NaN(), NaN(), 7);
        CHECK_EQ(params.ring, 1.0f); // a NaN reads as the engine's own value, so a broken ini cannot turn the card on
        CHECK(AnyFlareChange(params) == false);
        // Every clear bit of the three arms the card on its own, at the engine's own scales: the ring's bit alone (6), the sun+orbs bit
        // alone (5), the veil's alone (3), two at once (4, 2, 1) and all three (0). Only the full set at identity is the game's picture.
        // (A clear ring or sun+orbs bit with the veil's bit set used to leave the card idle, so FlareParts = 4 or 6 removed nothing.)
        for (int32_t parts = 0; parts <= 6; ++parts)
            CHECK(AnyFlareChange(BuildParams(1.0f, 1.0f, 1.0f, parts)) == true);
        CHECK(AnyFlareChange(BuildParams(1.0f, 1.0f, 1.0f, 7)) == false);
        // A scale off the engine's value arms it with the parts full, whichever of the three it is.
        CHECK(AnyFlareChange(BuildParams(0.5f, 1.0f, 1.0f, 7)) == true);
        CHECK(AnyFlareChange(BuildParams(1.0f, 0.5f, 1.0f, 7)) == true);
        CHECK(AnyFlareChange(BuildParams(1.0f, 1.0f, 0.5f, 7)) == true);
        // ...and a thousandth away is not a look: the epsilon keeps the card idle.
        CHECK(AnyFlareChange(BuildParams(1.0005f, 0.9995f, 1.0005f, 7)) == false);
    }

    // ---- the three scale keys: the clamps, and the identity epsilon the frame's cost rides on
    {
        CHECK_EQ(ClampScale(NaN()), 1.0f);
        CHECK_EQ(ClampScale(-0.5f), 0.0f);
        CHECK_EQ(ClampScale(0.0f), 0.0f);
        CHECK_EQ(ClampScale(0.999f), 0.999f); // inside the epsilon is a value in its own right (a slider's land)
        CHECK_EQ(ClampScale(2.0f), 2.0f);
        CHECK_EQ(ClampScale(3.0f), 2.0f);
        CHECK(IsIdentityScale(1.0f));
        CHECK(IsIdentityScale(1.0f + 0.0009f));
        CHECK(IsIdentityScale(1.0f - 0.0009f));
        CHECK(!IsIdentityScale(1.1f));
        CHECK(!IsIdentityScale(0.9f));
        CHECK(IsIdentityScale(NaN())); // a NaN is no look anyone asked for
    }

    // ---- the verdicts, per family
    {
        const Params identity = BuildParams(1.0f, 1.0f, 1.0f, 7);
        CHECK_EQ(RingVerdict(identity).kind, VerdictKind::None);
        CHECK_EQ(SunVerdict(identity).kind, VerdictKind::None);
        CHECK_EQ(GhostVerdict(identity).kind, VerdictKind::None);
        // The ring: the part on with the slider, the part off, and the slider alone.
        CHECK_EQ(RingVerdict(BuildParams(0.5f, 1.0f, 1.0f, 7)).kind, VerdictKind::Patch);
        CHECK_EQ(RingVerdict(BuildParams(0.5f, 1.0f, 1.0f, 7)).scale, 0.5f);
        CHECK_EQ(RingVerdict(BuildParams(0.5f, 1.0f, 1.0f, 6)).kind, VerdictKind::Patch); // the bit off asks it removed at 0
        CHECK_EQ(RingVerdict(BuildParams(0.5f, 1.0f, 1.0f, 6)).scale, 0.0f);
        CHECK_EQ(RingVerdict(BuildParams(1.0f, 1.0f, 1.0f, 6)).scale, 0.0f);
        // The sun: the same shape, shared with the orbs via the part.
        CHECK_EQ(SunVerdict(BuildParams(1.0f, 0.25f, 1.0f, 7)).kind, VerdictKind::Patch);
        CHECK_EQ(SunVerdict(BuildParams(1.0f, 0.25f, 1.0f, 7)).scale, 0.25f);
        CHECK_EQ(SunVerdict(BuildParams(1.0f, 0.25f, 1.0f, 5)).kind, VerdictKind::Patch);
        CHECK_EQ(SunVerdict(BuildParams(1.0f, 0.25f, 1.0f, 5)).scale, 0.0f);
        // The ghost step: the sanity of the sun's own value, and the removal through the sun's patch.
        CHECK_EQ(GhostVerdict(BuildParams(1.0f, 0.25f, 1.0f, 7)).kind, VerdictKind::None);
        CHECK_EQ(GhostVerdict(BuildParams(1.0f, 0.25f, 2.0f, 7)).kind, VerdictKind::Patch);
        CHECK_EQ(GhostVerdict(BuildParams(1.0f, 0.25f, 2.0f, 7)).scale, 2.0f);
        CHECK_EQ(GhostVerdict(BuildParams(1.0f, 0.0f, 2.0f, 7)).kind, VerdictKind::None); // the orbs die with the sun's patch
        CHECK_EQ(GhostVerdict(BuildParams(1.0f, 0.5f, 2.0f, 5)).kind, VerdictKind::None); // the part off: the orbs go with the sun
        // FlareParts = 4 (the capture's Halo test): the ring and the sun+orbs are asked removed at 0, the veil's bit is on so it is the game's draw.
        {
            const Params onlyVeil = BuildParams(1.0f, 1.0f, 1.0f, 4);
            CHECK_EQ(RingVerdict(onlyVeil).kind, VerdictKind::Patch);
            CHECK_EQ(RingVerdict(onlyVeil).scale, 0.0f);
            CHECK_EQ(SunVerdict(onlyVeil).kind, VerdictKind::Patch);
            CHECK_EQ(SunVerdict(onlyVeil).scale, 0.0f);
            CHECK_EQ(GhostVerdict(onlyVeil).kind, VerdictKind::None);
            CHECK_EQ(VeilVerdict(onlyVeil, VeilFacts { 1920, 1080, false }).kind, VerdictKind::None);
        }
        // The veil: the parts on, the parts off, the exposure meters, and the replay.
        const Params veilOff = BuildParams(1.0f, 1.0f, 1.0f, 3);
        CHECK_EQ(VeilVerdict(identity, VeilFacts { 1920, 1080, false }).kind, VerdictKind::None);
        CHECK_EQ(VeilVerdict(identity, VeilFacts { 960, 540, false }).kind, VerdictKind::None);
        CHECK_EQ(VeilVerdict(identity, VeilFacts { 1, 1, false }).kind, VerdictKind::None);
        CHECK_EQ(VeilVerdict(veilOff, VeilFacts { 1920, 1080, false }).kind, VerdictKind::Skip);
        CHECK_EQ(VeilVerdict(veilOff, VeilFacts { 960, 540, false }).kind, VerdictKind::Skip);
        CHECK_EQ(VeilVerdict(veilOff, VeilFacts { 1, 1, false }).kind, VerdictKind::None);  // the exposure meter's own target
        CHECK_EQ(VeilVerdict(veilOff, VeilFacts { 16, 16, false }).kind, VerdictKind::None);
        CHECK_EQ(VeilVerdict(veilOff, VeilFacts { 4, 4, false }).kind, VerdictKind::None);
        CHECK_EQ(VeilVerdict(veilOff, VeilFacts { 1920, 1080, true }).kind, VerdictKind::None); // a replay keeps the game's own picture
        CHECK(VeilTargetIs(320, 180));
        CHECK(!VeilTargetIs(319, 1080));
        CHECK(!VeilTargetIs(1920, 179));
    }

    // ---- the session: one streak, sixty-four, and the runs that clear it
    {
        Session session;
        CHECK(session.Armed());
        for (uint32_t i = 0; i < kConsecutiveDeclineLimit - 1; ++i)
            CHECK(session.NoteDecline() == false);
        CHECK(session.Armed()); // sixty-three are still one short
        CHECK(session.NoteDecline());
        CHECK(!session.Armed());
        CHECK(session.NoteDecline() == false); // standing down answers false but costs nothing else
        session.NoteRun();
        CHECK(!session.Armed()); // a run after the stand-down re-arms only through the publish, not the count
    }
    {
        Session session;
        for (uint32_t i = 0; i < kConsecutiveDeclineLimit - 1; ++i)
            session.NoteDecline();
        session.NoteRun(); // the streak resets: a run is the session's own proof the shape still holds
        CHECK(session.NoteDecline() == false);
        CHECK(session.Armed());
    }

    // ---- the log gate: one line per change of state, and the budget a session cannot exceed
    {
        LogGate log;
        CHECK(log.Take(1));
        CHECK(!log.Take(1)); // the same reason does not write twice in a row
        CHECK(log.Take(2));
        CHECK(log.Take(1)); // a tagged inside again writes again
        unsigned writes = 0;
        for (unsigned i = 0; i < 1000; ++i)
            if (log.Take(10u + i))
                ++writes;
        CHECK_EQ(writes, 29u); // the gate's budget: the first three takes wrote 3 lines, so 29 of this loop's 32 fit in the 32-line cap
    }

    // ---- the lines a session reads back
    {
        const std::string run = FormatRun(Family::Ring, 0.5f, 1, 64, true, 0.0042);
        CHECK(run.find("Lens flare:") == 0);
        CHECK(run.find("the ring's colour") != std::string::npos);
        CHECK(run.find("0.500") != std::string::npos);
        CHECK(run.find("64-byte") != std::string::npos && run.find("cHaloParam") != std::string::npos && run.find("slot 1") != std::string::npos);
        CHECK_EQ(FormatRun(Family::Ghost, 2.0f, 1, 16, false, 0.0).find("cRadialBlurParam") != std::string::npos, true);
        CHECK_EQ(FormatRun(Family::Sun, 0.0f, 1, 128, true, 0.001).find("cSunParam") != std::string::npos, true);
        const std::string keys = FormatKeys(BuildParams(0.5f, 1.0f, 1.0f, 7));
        CHECK(keys.find("ring x0.50") != std::string::npos && keys.find("parts 0x7") != std::string::npos);
        CHECK(FormatDecline(Decline::BufferSize).find("the bound buffer is not the size this family's patch is given") != std::string::npos);
        CHECK(FormatDecline(Decline::NoBuffer).find("nothing is bound at the slot the shader declares") != std::string::npos);
        CHECK(FormatDecline(Decline::PatchFailed).find("compute scale could not run") != std::string::npos);
        CHECK(FormatDecline(Decline::Exception).find("a call threw") != std::string::npos);
        CHECK(FormatVeilSkip().find("skipped") != std::string::npos && FormatVeilSkip().find("vertices") != std::string::npos);
        CHECK(FormatStoodDown().find("off for this session") != std::string::npos);
        CHECK(strcmp(DeclineName(Decline::None), "run") == 0);
        // The reason the veil is skipped is written: the sky's own add, not the patch's.
        CHECK(FormatVeilSkip().find("the veil's colour and bias live in its four vertices' data") != std::string::npos);
    }

    return ::Finish("flare_rules");
}
