// P2a: the controls of the quality group, all over committed bytes (no executable):
//   1. every predicate removes its PLANTED decoy (most anchors are unique on the real executable without any predicate, so an exe-only test
//      cannot show a dropped predicate): TargetIn(Bss), SameFunctionAs and ValueIn, for scan anchors (a decoy copy is planted in the image) and
//      for guard anchors (the site itself is made to violate the predicate); the same image resolves differently once the predicate is removed;
//   2. EntryPatched for each entry anchor (querySize, rendererUpdate, resizeCallbacks, cacheCompare): a foreign E9 over the entry, with the other
//      anchors present and with the entry anchor alone (the relaxed retry), and the control that a changed, non-jump entry is NotFound;
//   3. the bytes the consumers rely on are pinned (flipping one makes the seam NotFound or, where another independent anchor still pins the
//      same facts, FoundSingleAnchor), and the bytes that are masked on purpose do not matter (a recompile that only moves displacements);
//   4. the struct offsets follow the code: a shifted offset is decoded into the Offset seam, disagreeing sites are a Conflict, an offset outside
//      its sanity range is refused, the pair-structured offsets are verified rather than healed;
//   5. the failure chain: a seam that cannot resolve takes its dependants down and nothing else.
#include "resolve_quality_util.h"

#include <algorithm>
#include <functional>
#include <set>

using namespace qt;

namespace
{
std::size_t PatternLength(const std::string& pattern)
{
    std::size_t n = 0;
    for (char c : pattern)
        n += c == ' ' ? 0 : 1;
    return (n + 1) / 2;
}

std::vector<std::uint8_t> ReadBytes(const DenseImage& img, std::uint32_t rva, std::size_t n)
{
    std::vector<std::uint8_t> v(n);
    if (!img.Read(rva, v.data(), n))
    {
        std::printf("FAIL: cannot read %zu bytes at 0x%x\n", n, rva);
        ++g_failures;
    }
    return v;
}

std::size_t IndexOf(const Registry& reg, const char* id)
{
    for (std::size_t i = 0; i < reg.seams.size(); ++i)
    {
        if (reg.seams[i].id == id)
            return i;
    }
    std::printf("FAIL: no seam %s\n", id);
    ++g_failures;
    return 0;
}

Registry WithoutPredicate(Registry reg, std::size_t seam, std::size_t anchor, PredicateKind kind)
{
    auto& preds = reg.seams[seam].anchors[anchor].predicates;
    preds.erase(std::remove_if(preds.begin(), preds.end(), [kind](const Predicate& p) { return p.kind == kind; }), preds.end());
    return reg;
}

const char* KindName(PredicateKind k)
{
    return k == PredicateKind::TargetIn ? "TargetIn" : (k == PredicateKind::SameFunctionAs ? "SameFunctionAs" : "ValueIn");
}

// Writes the decoded value of `a` at the place its decode reads it: a RIP displacement that makes the target `value`, or a field.
void WriteDecoded(DenseImage& img, const AnchorSpec& a, std::uint32_t match, std::uint32_t value)
{
    const std::uint32_t insn = match + a.decode.at;
    if (a.decode.kind == DecodeKind::RipDisp32)
        SetU32(img, insn + a.decode.dispAt, value - (insn + a.decode.length));
    else if (a.decode.kind == DecodeKind::Field)
    {
        for (int k = 0; k < a.decode.width; ++k)
            SetByte(img, insn + k, static_cast<std::uint8_t>(value >> (8 * k)));
    }
    else
        CHECK(false);
}

// 1. predicates and their decoys ------------------------------------------------------------------------------------------------------------
//
// For anchor (seam, anchor) and predicate kind K:
//   scan anchor : copy the matched bytes to a decoy address and make the copy violate K only: TargetIn -> the RIP target in .rdata, ValueIn -> the
//                 field out of range, SameFunctionAs -> the copy lies in an unrelated function (it has its own .pdata entry). With K the real site
//                 wins (2 raw hits, 1 candidate); without K both survive (Multiple).
//   guard anchor: the site itself violates K (the bytes are masked in the seam's own anchors, so the seam still resolves): with K the anchor is
//                 dropped (Zero, the pattern hit once), without K it is Unique and carries the violating value.
int g_decoysChecked = 0;

void CheckDecoy(const Registry& reg, const ResolvedBuild& base, std::size_t si, std::size_t ai, PredicateKind kind, int serial)
{
    const SeamSpec& seam = reg.seams[si];
    const AnchorSpec& a = seam.anchors[ai];
    const AnchorResult& r0 = base.seams[si].anchors[ai];
    const std::string where = seam.id + "/" + a.name + " " + KindName(kind);
    if (!r0.hasValue)
    {
        std::printf("FAIL: %s: the baseline anchor has no value\n", where.c_str());
        ++g_failures;
        return;
    }
    const std::size_t length = PatternLength(a.pattern);
    const bool guard = !a.fromSeam.empty();
    // a decoy that must fail ONE predicate: inside rendererUpdate (0x2dab50, free bytes of a chained range) when another predicate asks for that
    // function, else in the unrelated decoy function
    // (the function a SameFunctionAs predicate names: rendererUpdate has free bytes at 0x2dab50, contextResolver at 0x374740)
    std::uint32_t inFunction = 0;
    for (const Predicate& p : a.predicates)
    {
        if (p.kind == PredicateKind::SameFunctionAs)
            inFunction = p.seam == "quality.contextResolver" ? 0x374740 : (p.seam == "quality.rendererUpdate" ? 0x2dab50 : 0);
    }
    const std::uint32_t decoyAt = kind == PredicateKind::SameFunctionAs ? kDecoyFunction + 0x100 * (1 + serial % 8)
                                  : (inFunction != 0 ? inFunction : kDecoyFunction + 0x100 * (1 + serial % 8));
    const std::uint32_t badValue = a.decode.kind == DecodeKind::Field ? (a.decode.width == 1 ? 0xf0u : 0x7ffffff0u) : kDecoyRdata;

    auto img = MakeQualityCWithSites();
    REQUIRE(img != nullptr);
    std::uint32_t decodedBad = 0;
    if (guard)
    {
        // in place: the guard site is where the seam's own anchors masked these bytes
        if (kind == PredicateKind::SameFunctionAs)
            return; // a guard anchor sits at the seam's address: no such decoy
        WriteDecoded(*img, a, r0.match, badValue);
        decodedBad = badValue;
        // a field that the guard pattern itself pins (contextCacheOffset: both members of the pair are fixed): the changed site no longer matches the
        // pattern at all, with or without the predicate; the predicate is a redundant sanity range there. Proved as such.
        Pattern pat;
        CHECK(ParsePattern(a.pattern, pat));
        bool pinned = false;
        if (kind == PredicateKind::ValueIn && a.decode.kind == DecodeKind::Field)
        {
            for (int k = 0; k < a.decode.width; ++k)
                pinned = pinned || pat.mask[static_cast<std::size_t>(a.decode.at + k)] != 0;
        }
        if (pinned)
        {
            const ResolvedBuild w = ResolveImage(*img, reg);
            const ResolvedBuild wo = ResolveImage(*img, WithoutPredicate(reg, si, ai, kind));
            const AnchorResult& p = w.seams[si].anchors[ai];
            const AnchorResult& po = wo.seams[si].anchors[ai];
            if (!(p.outcome == AnchorOutcome::Zero && p.patternHits == 0 && po.outcome == AnchorOutcome::Zero && po.patternHits == 0))
                std::printf("FAIL: %s: pinned guard field: the changed site should not match (hits %u / %u)\n", where.c_str(), p.patternHits, po.patternHits);
            CHECK(p.outcome == AnchorOutcome::Zero && p.patternHits == 0 && po.outcome == AnchorOutcome::Zero && po.patternHits == 0);
            ++g_decoysChecked;
            return;
        }
    }
    else
    {
        // a ValueIn predicate on a field that the PATTERN pins (the accessor of camera.cameraOffset keeps +0x3028 fixed): the mutated copy is not
        // even a pattern hit, the predicate is a redundant sanity range there. Proved as such, not skipped.
        Pattern pat;
        CHECK(ParsePattern(a.pattern, pat));
        bool pinned = false;
        if (kind == PredicateKind::ValueIn && a.decode.kind == DecodeKind::Field)
        {
            for (int k = 0; k < a.decode.width; ++k)
                pinned = pinned || pat.mask[static_cast<std::size_t>(a.decode.at + k)] != 0;
        }
        const auto bytes = ReadBytes(*img, r0.match, length);
        CHECK(img->Put(decoyAt, bytes));
        if (pinned)
        {
            WriteDecoded(*img, a, decoyAt, badValue);
            const ResolvedBuild w = ResolveImage(*img, reg);
            const AnchorResult& p = w.seams[si].anchors[ai];
            if (!(p.outcome == AnchorOutcome::Unique && p.patternHits == 1 && p.candidates == 1 && p.value == r0.value))
                std::printf("FAIL: %s: pinned field: the mutated copy should not match (hits %u)\n", where.c_str(), p.patternHits);
            CHECK(p.outcome == AnchorOutcome::Unique && p.patternHits == 1 && p.candidates == 1 && p.value == r0.value);
            ++g_decoysChecked;
            return;
        }
        if (kind == PredicateKind::TargetIn || kind == PredicateKind::ValueIn)
            WriteDecoded(*img, a, decoyAt, badValue);
        // SameFunctionAs: the copy is left as it is
    }

    const ResolvedBuild with = ResolveImage(*img, reg);
    const AnchorResult& aw = with.seams[si].anchors[ai];
    const Registry noPred = WithoutPredicate(reg, si, ai, kind);
    const ResolvedBuild without = ResolveImage(*img, noPred);
    const AnchorResult& ao = without.seams[si].anchors[ai];

    if (guard)
    {
        const bool ok = aw.outcome == AnchorOutcome::Zero && aw.patternHits == 1 && aw.candidates == 0 && ao.outcome == AnchorOutcome::Unique &&
                        ao.value == decodedBad;
        if (!ok)
            std::printf("FAIL: %s: guard anchor with=%d(hits %u cand %u) without=%d(value 0x%x)\n", where.c_str(), static_cast<int>(aw.outcome),
                        aw.patternHits, aw.candidates, static_cast<int>(ao.outcome), ao.value);
        CHECK(ok);
    }
    else
    {
        const bool ok = aw.outcome == AnchorOutcome::Unique && aw.patternHits == 2 && aw.candidates == 1 && aw.value == r0.value &&
                        ao.outcome == AnchorOutcome::Multiple && ao.candidates == 2;
        if (!ok)
            std::printf("FAIL: %s: scan anchor with=%d(hits %u cand %u value 0x%x, real 0x%x) without=%d(cand %u)\n", where.c_str(),
                        static_cast<int>(aw.outcome), aw.patternHits, aw.candidates, aw.value, r0.value, static_cast<int>(ao.outcome), ao.candidates);
        CHECK(ok);
    }
    ++g_decoysChecked;
}

void TestPredicateDecoys()
{
    const Registry reg = QualityRegistry();
    auto baseImg = MakeQualityCWithSites();
    REQUIRE(baseImg != nullptr);
    const ResolvedBuild base = ResolveImage(*baseImg, reg);
    int serial = 0;
    int predicates = 0;
    for (std::size_t si = 0; si < reg.seams.size(); ++si)
    {
        for (std::size_t ai = 0; ai < reg.seams[si].anchors.size(); ++ai)
        {
            for (const Predicate& p : reg.seams[si].anchors[ai].predicates)
            {
                ++predicates;
                // chara.targetsOffset / chara.managerPointer: the accessor's own scan (frozen, pinned by the P1 test) pins or tests the very bytes an
                // in-place decoy would change, so the accessor itself would go: they have their own tests below
                if (reg.seams[si].id == "chara.targetsOffset" || reg.seams[si].id == "chara.managerPointer")
                    continue;
                CheckDecoy(reg, base, si, ai, p.kind, serial++);
            }
        }
    }
    // every (anchor, predicate) pair of the group was seen; the guard anchors that sit at their seam have no SameFunctionAs decoy (none of them uses it)
    CHECK(predicates == 34); // 11 TargetIn + 4 SameFunctionAs + 19 ValueIn
    CHECK(g_decoysChecked == 32); // all but the two of chara.targetsOffset / chara.managerPointer (own tests below)
    std::printf("    decoys checked: %d of %d (anchor, predicate) pairs\n", g_decoysChecked, predicates);
}

// chara.managerPointer: its guard reads the displacement that the accessor's own scan also tests (TargetIn(Bss)); an in-place decoy would remove
// the accessor first. A registry copy without the accessor's predicate shows what the manager's predicate does on its own.
void TestCharaManagerGuard()
{
    Registry reg = QualityRegistry();
    reg.seams[IndexOf(reg, "chara.accessor")].anchors[0].predicates.clear();
    auto img = MakeQualityCWithSites();
    REQUIRE(img != nullptr);
    SetU32(*img, 0x26abd0 + 3, kDecoyRdata - (0x26abd0 + 7));
    const ResolvedBuild with = ResolveImage(*img, reg);
    CHECK(Get(with, "chara.accessor").status == SeamStatus::Found);
    CHECK(Get(with, "chara.managerPointer").status == SeamStatus::NotFound && Get(with, "chara.managerPointer").anchors[0].patternHits == 1 &&
          Get(with, "chara.managerPointer").anchors[0].candidates == 0);
    Registry noPred = reg;
    noPred.seams[IndexOf(noPred, "chara.managerPointer")].anchors[0].predicates.clear();
    const ResolvedBuild without = ResolveImage(*img, noPred);
    CHECK(Get(without, "chara.managerPointer").status == SeamStatus::Found && Get(without, "chara.managerPointer").rva == kDecoyRdata);
}

// chara.targetsOffset: its guard reads the four bytes the accessor scan pins. Prove the ValueIn predicate on a registry in which the accessor scan
// masks them (the frozen pattern text cannot be changed, a registry copy can).
void TestCharaTargetsRange()
{
    Registry reg = QualityRegistry();
    reg.seams[IndexOf(reg, "chara.accessor")].anchors[0].pattern = "48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 ?? ?? ?? ?? C3";
    reg.seams[IndexOf(reg, "chara.managerPointer")].anchors[0].pattern = reg.seams[0].anchors[0].pattern;
    auto img = MakeQualityCWithSites();
    REQUIRE(img != nullptr);
    const ResolvedBuild ok = ResolveImage(*img, reg);
    CHECK(Get(ok, "chara.targetsOffset").status == SeamStatus::Found && Get(ok, "chara.targetsOffset").rva == 0x2c8);
    SetU32(*img, 0x26abd0 + 13, 0x40000); // outside 0x100..0x1000
    const ResolvedBuild bad = ResolveImage(*img, reg);
    CHECK(Get(bad, "chara.accessor").status == SeamStatus::Found); // the accessor does not care
    CHECK(Get(bad, "chara.targetsOffset").status == SeamStatus::NotFound && Get(bad, "chara.targetsOffset").anchors[0].patternHits == 1 &&
          Get(bad, "chara.targetsOffset").anchors[0].candidates == 0);
    Registry noRange = reg;
    auto& preds = noRange.seams[IndexOf(noRange, "chara.targetsOffset")].anchors[0].predicates;
    preds.clear();
    const ResolvedBuild loose = ResolveImage(*img, noRange);
    CHECK(Get(loose, "chara.targetsOffset").status == SeamStatus::Found && Get(loose, "chara.targetsOffset").rva == 0x40000);
}

// 2. EntryPatched ---------------------------------------------------------------------------------------------------------------------------
void TestEntryPatched()
{
    struct Entry
    {
        const char* seam;
        std::uint32_t rva;
        const char* entryAnchor; // the anchor whose relaxed retry reports the jump
    };
    const Entry entries[] = { { "quality.querySize", 0x352350, "entry" },
                              { "quality.rendererUpdate", 0x2da5b0, "entry" },
                              { "quality.resizeCallbacks", 0x2369a0, "entry" },
                              { "quality.cacheCompare", 0x374f10, "entry" } };
    const Registry full = QualityRegistry();
    for (const Entry& e : entries)
    {
        const std::size_t si = IndexOf(full, e.seam);
        // (i) with every anchor: the other anchors find the function, the entry reads as a jump -> EntryPatched, not usable
        {
            auto img = MakeQualityCWithSites();
            REQUIRE(img != nullptr);
            const std::uint8_t jmp[5] = { 0xe9, 0x10, 0x20, 0x30, 0x00 };
            CHECK(img->Put(e.rva, jmp, sizeof(jmp)));
            const ResolvedBuild rb = ResolveImage(*img, full);
            const SeamResult& s = Get(rb, e.seam);
            if (s.status != SeamStatus::EntryPatched)
                std::printf("FAIL: %s with a jump over its entry is %s (%s)\n", e.seam, StatusName(s.status), s.reason.c_str());
            CHECK(s.status == SeamStatus::EntryPatched && s.hasValue && s.rva == e.rva && !rb.Usable(e.seam));
        }
        // (ii) with the entry anchor alone: the relaxed retry (first 14 bytes masked) finds exactly one function whose entry is a jump
        for (std::uint8_t first : { std::uint8_t(0xe9), std::uint8_t(0xeb) })
        {
            Registry reg = full;
            SeamSpec& seam = reg.seams[si];
            AnchorSpec entry;
            for (const AnchorSpec& a : seam.anchors)
            {
                if (a.name == e.entryAnchor)
                    entry = a;
            }
            REQUIRE(entry.entryAnchor);
            seam.anchors = { entry };
            auto img = MakeQualityCWithSites();
            REQUIRE(img != nullptr);
            const std::uint8_t jmp[5] = { first, 0x10, 0x20, 0x30, 0x00 };
            CHECK(img->Put(e.rva, jmp, sizeof(jmp)));
            const ResolvedBuild rb = ResolveImage(*img, reg);
            const SeamResult& s = Get(rb, e.seam);
            CHECK(s.status == SeamStatus::EntryPatched && s.rva == e.rva && s.anchors[0].outcome == AnchorOutcome::Patched && !rb.Usable(e.seam));
            // the dependants of an EntryPatched seam still get its address but are weak (no plain Found on top of it)
        }
        // (iii) control: the entry changed to something that is NOT a jump (here int3) is NotFound, never EntryPatched or Found
        {
            Registry reg = full;
            SeamSpec& seam = reg.seams[si];
            AnchorSpec entry;
            for (const AnchorSpec& a : seam.anchors)
            {
                if (a.name == e.entryAnchor)
                    entry = a;
            }
            seam.anchors = { entry };
            auto img = MakeQualityCWithSites();
            REQUIRE(img != nullptr);
            const std::uint8_t cc[5] = { 0xcc, 0xcc, 0xcc, 0xcc, 0xcc };
            CHECK(img->Put(e.rva, cc, sizeof(cc)));
            const ResolvedBuild rb = ResolveImage(*img, reg);
            CHECK(Get(rb, e.seam).status == SeamStatus::NotFound);
        }
        // (iv) control: an untouched entry with the same registry is plain Found
        {
            auto img = MakeQualityCWithSites();
            REQUIRE(img != nullptr);
            CHECK(ResolveImage(*img, full).Usable(e.seam));
        }
    }
}

// 2b. HIGH-1: the SHAPE seams carry the Guard fields ----------------------------------------------------------------------------------------
//
// On an UNKNOWN build a Guard field that follows a location seam is the live bytes at whatever address that seam resolved to, so the hooks'
// own compare of live bytes against it checks nothing. The Guard fields follow the SHAPE seams instead (one strict guard anchor, failClosed):
// a shape that does not match leaves the field ZERO, on an unknown AND on a known build (a table guard is only a prefix of the strict pattern).
struct HookCase
{
    const char* location;
    const char* shape;
    const char* rvaKey;
    const char* guardKey;
    std::uint32_t rva;
    std::uint32_t prologueByte; // a byte of the strict entry pattern that the body / caller anchors do not read
    std::uint8_t to;
    SeamStatus locationAfter; // what the location seam still says with that one prologue byte changed (the other anchors agree)
};

const HookCase kHooks[] = {
    { "quality.querySize", "quality.querySizeShape", "nativeQuality.querySizeRva", "nativeQuality.querySizeInstructions", 0x352350, 0x352360, 0x57,
      SeamStatus::Found },
    { "quality.rendererUpdate", "quality.rendererUpdateShape", "nativeQuality.rendererUpdateRva", "nativeQuality.rendererUpdateInstructions", 0x2da5b0,
      0x2da5b2, 0xdd, SeamStatus::Found },
    { "quality.resizeCallbacks", "quality.resizeCallbacksShape", "nativeQuality.resizeCallbacksRva", "nativeQuality.resizeCallbacksInstructions", 0x2369a0,
      0x2369af, 0x56, SeamStatus::FoundSingleAnchor },
};

void TestShapeSeams()
{
    const Registry reg = QualityRegistry();
    ResolveOptions known;
    known.knownTable = &ext::ffxiv::kCurrentBuild;
    for (const HookCase& h : kHooks)
    {
        // baseline: both seams Found, the Guard is the live bytes of the (verified) shape and equals the table's
        {
            auto img = MakeQualityCWithSites();
            REQUIRE(img != nullptr);
            const ResolvedBuild rb = ResolveImage(*img, reg);
            CHECK(Get(rb, h.location).status == SeamStatus::Found && Get(rb, h.shape).status == SeamStatus::Found && Get(rb, h.shape).rva == h.rva);
            CHECK(HookSiteUsable(rb, h.location, h.shape));
            CHECK(RvaOf(rb.build, h.rvaKey) == h.rva);
            CHECK(!AllZero(GuardOf(rb.build, h.guardKey)) && GuardOf(rb.build, h.guardKey) == GuardOf(ext::ffxiv::kCurrentBuild, h.guardKey));
        }
        // ONE changed byte of the prologue (the headline case: the location seam still says it found the function, with the other anchors in
        // agreement; nothing in its status says that the bytes a Detour overwrites are not the ones that were verified). The shape seam does:
        // NotFound, the Guard field is ZERO and the Rva field is filled; the site is not usable
        for (int build = 0; build < 2; ++build)
        {
            auto img = MakeQualityCWithSites();
            REQUIRE(img != nullptr);
            const std::vector<std::uint8_t> before = ReadBytes(*img, h.rva, GuardOf(ext::ffxiv::kCurrentBuild, h.guardKey).size());
            SetByte(*img, h.prologueByte, h.to);
            const ResolvedBuild rb = build == 0 ? ResolveImage(*img, reg) : ResolveImage(*img, reg, known);
            const char* which = build == 0 ? "unknown build" : "known build";
            const SeamResult& loc = Get(rb, h.location);
            const SeamResult& shp = Get(rb, h.shape);
            if (loc.status != h.locationAfter || shp.status != SeamStatus::NotFound)
                std::printf("FAIL: %s, %s: location %s (expected %s), shape %s\n", h.location, which, StatusName(loc.status), StatusName(h.locationAfter),
                            StatusName(shp.status));
            CHECK(loc.status == h.locationAfter && loc.rva == h.rva && !loc.fellBackToTable);
            CHECK(shp.status == SeamStatus::NotFound && !shp.fellBackToTable && !rb.Usable(h.shape) && !rb.UsableOrTable(h.shape));
            CHECK(RvaOf(rb.build, h.rvaKey) == h.rva);     // the location is still reported
            CHECK(AllZero(GuardOf(rb.build, h.guardKey))); // and the Guard is zero: the hooks' compare (live bytes vs zero) refuses
            CHECK(!HookSiteUsable(rb, h.location, h.shape));
            // what the zero protects: the live bytes are not the ones that were verified
            CHECK(before != ReadBytes(*img, h.rva, before.size()));
        }
    }
}

// The review's compound scenario, for the three functions that a CALL / reference anchor can point somewhere else: the function was recompiled (entry and
// body die), the register the caller passes changes (the real caller anchor dies) and a NEW call site of the same generic idiom appears whose
// target is another function F2 (F2 is not the one that was hooked). The call anchor is then the only one left and unique at F2: the LOCATION
// seam is FoundSingleAnchor at the wrong function. The shape is checked at F2 and fails, so the Guard stays zero. The control: the same registry
// with the Guard rebound to the location seam (the old binding) reads F2's live bytes into the Guard, which the hooks' own compare accepts.
void TestWrongFunctionBehindACaller()
{
    constexpr std::uint32_t kF2 = 0x600800;
    struct Case
    {
        const char* location;
        const char* shape;
        const char* guardKey;
        std::uint32_t realSite;                      // the real call / reference site (its bytes are copied to newSite, then the real one is broken)
        std::size_t siteLength;
        std::uint32_t dispAt;                        // offset of the rel32 / RIP displacement the anchor decodes, inside the site
        std::uint32_t insnEnd;                       // offset of the end of that instruction (the displacement is relative to it)
        std::uint32_t newSite;                       // where the copy goes (resizeCallbacks: rendererUpdate's free bytes, for SameFunctionAs)
        std::uint32_t breakCaller;                   // one fixed byte of the real site (kills the real caller anchor)
        std::uint8_t breakTo;
        std::vector<std::uint32_t> killEntryAndBody; // a byte of the other anchors of the location seam
        std::uint8_t killTo;
    };
    const Case cases[] = {
        // rendererUpdate: the caller anchor `mov rcx,[g] ; movaps xmm1,xmm10 ; call` (xmm10 -> xmm11); entry and body share the movss displacement
        { "quality.rendererUpdate", "quality.rendererUpdateShape", "nativeQuality.rendererUpdateInstructions", 0x2b9e69, 33, 12, 16, 0x600400, 0x2b9e73,
          0xcb, { 0x2da5ce }, 0x11 },
        // querySize: the third anchor, `lea rcx,[querySize]` of the manager constructor (the planted copy is a second constructor storing F2; the real
        // one loses a fixed byte of its renderer-slot store); entry and body share the optimal store
        { "quality.querySize", "quality.querySizeShape", "nativeQuality.querySizeInstructions", 0x3537c8, 44, 10, 14, 0x600400, 0x3537c8 + 24, 0x31,
          { 0x3523a9 }, 0x07 },
        // resizeCallbacks: the call inside rendererUpdate (the copy lies in rendererUpdate's chained range 0x2daa8f..0x2dab80, free bytes at 0x2dab50)
        { "quality.resizeCallbacks", "quality.resizeCallbacksShape", "nativeQuality.resizeCallbacksInstructions", 0x2dab0d, 57, 5, 9, 0x2dab50, 0x2dab0f,
          0x4f, { 0x2369eb }, 0x10 },
    };
    for (const Case& c : cases)
    {
        for (int variant = 0; variant < 2; ++variant) // 0: the real registry, 1: the Guard rebound to the location seam (the old binding)
        {
            Registry reg = QualityRegistry();
            if (variant == 1)
            {
                for (Binding& b : reg.bindings)
                {
                    if (b.key == c.guardKey)
                        b.seam = c.location;
                }
            }
            auto img = MakeQualityCWithSites();
            REQUIRE(img != nullptr);
            // F2: some other function (non-zero bytes so that a zero Guard can only come from a refused shape)
            CHECK(img->Put(kF2, std::vector<std::uint8_t>(0x100, 0x90)));
            std::vector<std::uint8_t> site = ReadBytes(*img, c.realSite, c.siteLength);
            const std::uint32_t rel = kF2 - (c.newSite + c.insnEnd);
            std::memcpy(site.data() + c.dispAt, &rel, 4);
            CHECK(img->Put(c.newSite, site));
            SetByte(*img, c.breakCaller, c.breakTo);
            for (std::uint32_t at : c.killEntryAndBody)
                SetByte(*img, at, c.killTo);
            const ResolvedBuild rb = ResolveImage(*img, reg);
            const SeamResult& loc = Get(rb, c.location);
            const SeamResult& shp = Get(rb, c.shape);
            if (loc.status != SeamStatus::FoundSingleAnchor || loc.rva != kF2)
                std::printf("FAIL: %s (variant %d): location %s rva=0x%x (%s)\n", c.location, variant, StatusName(loc.status), loc.rva, loc.reason.c_str());
            CHECK(loc.status == SeamStatus::FoundSingleAnchor && loc.rva == kF2); // the wrong function, usable: only the shape stands in the way
            CHECK(shp.status == SeamStatus::NotFound && !HookSiteUsable(rb, c.location, c.shape));
            const std::vector<std::uint8_t> guard = GuardOf(rb.build, c.guardKey);
            if (variant == 0)
                CHECK(AllZero(guard));
            else
                CHECK(!AllZero(guard) && guard[0] == 0x90); // the old binding: F2's live bytes, which the hooks' compare would accept
        }
    }
}

// failClosed on a KNOWN build (the table is this exe's): a shape that does not match is not replaced by the table's guard array (a short prefix
// of the strict pattern); the location seam falls back or resolves as before. The same registry with failClosed switched off falls back, so the
// flag is what changes the outcome.
void TestShapeFailClosedOnKnownBuild()
{
    ResolveOptions known;
    known.knownTable = &ext::ffxiv::kCurrentBuild;
    for (const HookCase& h : kHooks)
    {
        for (int variant = 0; variant < 3; ++variant) // 0: one prologue byte changed, 1: the same with failClosed off, 2: the whole function erased
        {
            Registry reg = QualityRegistry();
            if (variant == 1)
                FindSeam(reg, h.shape)->failClosed = false; // the control: without the flag the table fills it
            auto img = MakeQualityCWithSites();
            REQUIRE(img != nullptr);
            if (variant == 2)
                CHECK(img->Put(h.rva, std::vector<std::uint8_t>(0x20, 0)));
            else
                SetByte(*img, h.prologueByte, h.to);
            const ResolvedBuild rb = ResolveImage(*img, reg, known);
            REQUIRE(rb.knownBuild);
            const SeamResult& loc = Get(rb, h.location);
            const SeamResult& shp = Get(rb, h.shape);
            const std::vector<std::uint8_t> guard = GuardOf(rb.build, h.guardKey);
            if (variant == 1)
            {
                CHECK(shp.status == SeamStatus::NotFound && shp.fellBackToTable && rb.UsableOrTable(h.shape));
                CHECK(guard == GuardOf(ext::ffxiv::kCurrentBuild, h.guardKey));
                continue;
            }
            if (shp.status != SeamStatus::NotFound || shp.fellBackToTable || rb.UsableOrTable(h.shape) || !AllZero(guard))
                std::printf("FAIL: %s (variant %d): shape %s fellBack=%d usableOrTable=%d guardZero=%d\n", h.shape, variant, StatusName(shp.status),
                            shp.fellBackToTable ? 1 : 0, rb.UsableOrTable(h.shape) ? 1 : 0, AllZero(guard) ? 1 : 0);
            CHECK(shp.status == SeamStatus::NotFound && !shp.fellBackToTable && !rb.UsableOrTable(h.shape) && AllZero(guard));
            CHECK(!HookSiteUsable(rb, h.location, h.shape));
            if (variant == 0)
                CHECK(loc.status == h.locationAfter && !loc.fellBackToTable && RvaOf(rb.build, h.rvaKey) == h.rva); // as before
            else // the location resolves through its other anchors or falls back to the table, the shape does not
                CHECK((IsUsable(loc.status) || loc.fellBackToTable) && RvaOf(rb.build, h.rvaKey) == h.rva);
        }
    }
}

// 3. what is pinned and what is masked -------------------------------------------------------------------------------------------------------
struct Flip
{
    const char* what;
    std::uint32_t rva;
    std::uint8_t to;
    const char* seam;
    SeamStatus expected;
    // for a flip whose SEAM status is the same with the byte pinned or masked (a seam with three anchors keeps its status when one of them dies):
    // the outcome of one ANCHOR of the seam, which does tell them apart (-1: not checked)
    int anchor = -1;
    AnchorOutcome anchorOutcome = AnchorOutcome::Zero;
};

void RunFlips(const char* title, const std::vector<Flip>& flips)
{
    const Registry reg = QualityRegistry();
    for (const Flip& f : flips)
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        const std::uint8_t was = ByteAt(*img, f.rva);
        CHECK(was != f.to);
        SetByte(*img, f.rva, f.to);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        const SeamResult& s = Get(rb, f.seam);
        if (s.status != f.expected)
            std::printf("FAIL: %s: %s (0x%x: %02x -> %02x): %s is %s, expected %s (%s)\n", title, f.what, f.rva, was, f.to, f.seam, StatusName(s.status),
                        StatusName(f.expected), s.reason.c_str());
        CHECK(s.status == f.expected);
        if (f.anchor >= 0)
        {
            const bool ok = static_cast<std::size_t>(f.anchor) < s.anchors.size() && s.anchors[static_cast<std::size_t>(f.anchor)].outcome == f.anchorOutcome;
            if (!ok)
                std::printf("FAIL: %s: %s (0x%x): anchor #%d of %s is not %d\n", title, f.what, f.rva, f.anchor, f.seam, static_cast<int>(f.anchorOutcome));
            CHECK(ok);
        }
    }
}

void TestConsumerPins()
{
    const SeamStatus NF = SeamStatus::NotFound, SA = SeamStatus::FoundSingleAnchor, OK = SeamStatus::Found;
    // querySize: the detour forwards (display, optimal, minimum, maximum) = (rcx, rdx, r8, r9) and reads the three outputs: entry and body pin the
    // argument moves and the three output stores; the manager constructor still names the function (FoundSingleAnchor, the location is right), and
    // the SHAPE seam is what refuses (TestShapeSeams, the sweep). A changed PROLOGUE only costs the entry anchor: body and constructor agree (Found).
    RunFlips("querySize",
             { { "mov rdi,r9 -> r8 (maximum)", 0x35236e, 0xf8, "quality.querySize", SA },
               { "mov rsi,r8 -> r9 (minimum)", 0x352371, 0xf1, "quality.querySize", SA },
               { "mov r14,rdx -> rbx (optimal)", 0x352374, 0xf3, "quality.querySize", SA },
               { "mov rdx,rcx -> rax (display)", 0x352390, 0xd0, "quality.querySize", SA },
               { "store of optimal [r14]: 06 -> 07", 0x3523a9, 0x07, "quality.querySize", SA },
               { "load of minimum +0xdc -> +0xdd", 0x3523ad, 0xdd, "quality.querySize", SA },
               { "store of maximum [rdi]: 07 -> 06", 0x3523bd, 0x06, "quality.querySize", SA },
               { "mov al,1 -> 0", 0x3523bf, 0x00, "quality.querySize", SA },
               { "the settings mode compare 2 -> 3", 0x35238b, 0x03, "quality.querySize", SA },
               { "push r14 -> push r15 (prologue only)", 0x352360, 0x57, "quality.querySize", OK },
               { "mov [rsp+8],rbx: displacement (prologue only)", 0x352354, 0x09, "quality.querySize", OK } });
    // rendererUpdate: rcx = renderer, xmm1 = elapsed. The entry and body anchors pin the uses inside the function, the caller pins what it is
    // given; a change inside the function leaves the caller alone (FoundSingleAnchor), a change in the caller leaves the other two (Found).
    RunFlips("rendererUpdate",
             { { "mov rbx,rcx -> rax", 0x2da5e6, 0xd8, "quality.rendererUpdate", SA },
               { "movaps xmm3,xmm1 -> xmm0", 0x2da5df, 0xd8, "quality.rendererUpdate", SA },
               { "movss xmm0,[rcx+0x710] -> +0x711", 0x2da5ce, 0x11, "quality.rendererUpdate", SA },
               // three anchors: one dying leaves the seam Found, so these three assert the ANCHOR that holds the byte (entry #0, caller #2) and that
               // the other two still agree: a byte that was masked in that anchor would leave it Unique
               { "mov r11,rsp -> mov r11,rbp (entry only)", 0x2da5b2, 0xdd, "quality.rendererUpdate", OK, 0 },
               { "caller: movaps xmm1,xmm10 -> xmm1,xmm11", 0x2b9e73, 0xcb, "quality.rendererUpdate", OK, 2 },
               { "caller: test rcx,rcx modrm (tail only)", 0x2b9e81, 0x84, "quality.rendererUpdate", OK, 2 } });
    // resizeCallbacks: the call with rcx = [device + CALLBACKS] and the lock / loop / AND of the function itself
    RunFlips("resizeCallbacks",
             { { "caller: mov rcx,[r14+..] -> [r15+..]", 0x2dab0f, 0x4f, "quality.resizeCallbacks", SA },
               { "caller: call opcode E8 -> E9", 0x2dab11, 0xe9, "quality.resizeCallbacks", SA },
               { "entry: mov rdi,[rdi+8] loop argument", 0x2369eb, 0x10, "quality.resizeCallbacks", SA },
               { "entry: bpl AND (cmove ebp,r14d) -> cmovne", 0x2369f6, 0x45, "quality.resizeCallbacks", SA },
               { "entry: mov bpl,1 -> 0", 0x2369c9, 0x00, "quality.resizeCallbacks", SA },
               { "entry: lea rbx,[rcx+8] -> [rcx+9]", 0x2369b7, 0x09, "quality.resizeCallbacks", SA } });
    // the settings block: the DLAA gate compares mode == 2 and dlaa == 0 (xor r12d,r12d ; cmp [r8+..],r12b) and reads the selector
    RunFlips("settingsBlock",
             { { "mode compare value 2 -> 3", 0x3745e5, 0x03, "quality.settingsInstructions", NF },
               { "xor r12d,r12d -> xor r13d,r13d", 0x3745e0, 0xed, "quality.settingsInstructions", NF },
               { "cmp [r8+dlaa],r12b -> r13b", 0x3745f0, 0x68, "quality.settingsInstructions", NF },
               { "the je after the mode compare 06 -> 07", 0x3745e7, 0x07, "quality.settingsInstructions", NF },
               { "cmp eax,6 -> 7", 0x3745fb, 0x07, "quality.settingsInstructions", NF },
               { "mov r8,[global]: r8 -> r9", 0x3745d8, 0x8d, "quality.settingsInstructions", NF } });
    // the context chain the hooks write through
    RunFlips("context chain",
             { { "cacheCompare: the +0x154 height compare -> +0x155", 0x374f2f, 0x55, "quality.cacheCompare", SA },
               { "cacheCompare: cmp [rsi],eax -> [rsi+4]", 0x374f2a, 0x46, "quality.cacheCompare", SA },
               // the gate `(flags & 1) != 0 && (flags & 0x30) == 0`: each half is pinned by one of the two flags anchors, so one flip leaves the other
               { "flags: test byte [rcx+FLAGS],1 -> 2", 0x374598 + 6, 0x02, "quality.contextFlagsOffset", SA },
               { "flags: test al,0x30 -> 0x31", 0x37479c + 13, 0x31, "quality.contextFlagsOffset", SA },
               { "flags: test al,0x30 -> test al,0x10", 0x37479c + 13, 0x10, "quality.contextFlagsOffset", SA },
               { "flags: jne -> je after test al,0x30", 0x37479c + 14, 0x74, "quality.contextFlagsOffset", SA },
               { "flags: movzx eax,[rdi+..] -> [rsi+..]", 0x37479c + 2, 0x86, "quality.contextFlagsOffset", SA } });
}

// Bytes that a recompile moves and that are masked on purpose: RIP displacements nobody decodes (the stack cookie, the constant pool), the
// import calls of resizeCallbacks, call targets that nobody decodes. Everything still resolves, to the same values.
void TestMaskedBytesDoNotMatter()
{
    const Registry reg = QualityRegistry();
    auto img = MakeQualityCWithSites();
    REQUIRE(img != nullptr);
    const ResolvedBuild base = ResolveImage(*img, reg);
    // rendererUpdate: cookie load (0x2da5bb+3), mulss constant (0x2da5d5+4)
    SetU32(*img, 0x2da5be, 0x00123456);
    SetU32(*img, 0x2da5d9, 0x00654321);
    // resizeCallbacks: the two indirect import calls
    SetU32(*img, 0x2369c0, 0x00010000);
    SetU32(*img, 0x236a0c, 0x00020000);
    // the manager constructor: both calls
    SetU32(*img, 0x3537e8, 0x00001234);
    SetU32(*img, 0x3537f0, 0x00005678);
    // contextResolver prelude: the two calls that nobody decodes (0x3745a8 and 0x3745b4)
    SetU32(*img, 0x3745a8, 0x00000444);
    SetU32(*img, 0x3745b4, 0x00000555);
    // cacheCompare: the final jcc rel32
    SetU32(*img, 0x374f38, 0x00000666);
    // contextResolver: the displacement of the jne behind the flags test (consumer does not read it), in the guard window and in the second anchor
    SetByte(*img, 0x374598 + 14, 0x40);
    SetByte(*img, 0x37479c + 15, 0x20);
    const ResolvedBuild rb = ResolveImage(*img, reg);
    for (const SeamResult& s : base.seams)
    {
        const SeamResult& t = Get(rb, s.id.c_str());
        if (t.status != SeamStatus::Found || t.rva != s.rva)
            std::printf("FAIL: masked bytes changed the result of %s: %s 0x%x (was 0x%x)\n", s.id.c_str(), StatusName(t.status), t.rva, s.rva);
        CHECK(t.status == SeamStatus::Found && t.rva == s.rva);
    }
}

// Every wildcard of every anchor that no decode reads is free: flip each one (xor 0x5a) at its baseline site, all at once, and every seam still
// resolves to the same value. This is what kills an under-wildcarded pattern (a masked byte turned into a fixed one) without the pins test: the
// positions are derived from the registry itself, not listed by hand. Positions that another anchor pins at the same address are excluded.
void TestEveryUnreadMaskedByteIsFree()
{
    const Registry reg = QualityRegistry();
    auto img = MakeQualityCWithSites();
    REQUIRE(img != nullptr);
    const ResolvedBuild base = ResolveImage(*img, reg);

    std::set<std::uint32_t> masked, decoded, pinned;
    for (std::size_t si = 0; si < reg.seams.size(); ++si)
    {
        for (std::size_t ai = 0; ai < reg.seams[si].anchors.size(); ++ai)
        {
            const AnchorSpec& a = reg.seams[si].anchors[ai];
            const AnchorResult& r = base.seams[si].anchors[ai];
            REQUIRE(r.hasValue);
            Pattern p;
            REQUIRE(ParsePattern(a.pattern, p));
            for (std::size_t k = 0; k < p.size(); ++k)
                (p.mask[k] == 0 ? masked : pinned).insert(r.match + static_cast<std::uint32_t>(k));
            const std::uint32_t insn = r.match + a.decode.at;
            switch (a.decode.kind)
            {
            case DecodeKind::RipDisp32:
                for (std::uint32_t k = 0; k < 4; ++k)
                    decoded.insert(insn + a.decode.dispAt + k);
                break;
            case DecodeKind::Rel32:
                for (std::uint32_t k = 1; k < 5; ++k)
                    decoded.insert(insn + k);
                break;
            case DecodeKind::Field:
                for (int k = 0; k < a.decode.width; ++k)
                    decoded.insert(insn + static_cast<std::uint32_t>(k));
                break;
            default:
                break;
            }
        }
    }
    std::size_t flipped = 0;
    for (std::uint32_t rva : masked)
    {
        if (decoded.count(rva) != 0 || pinned.count(rva) != 0)
            continue;
        SetByte(*img, rva, static_cast<std::uint8_t>(ByteAt(*img, rva) ^ 0x5a));
        ++flipped;
    }
    std::printf("    free masked bytes flipped: %zu (of %zu masked positions, %zu decoded)\n", flipped, masked.size(), decoded.size());
    CHECK(flipped >= 40);
    const ResolvedBuild rb = ResolveImage(*img, reg);
    for (const SeamResult& s : base.seams)
    {
        const SeamResult& t = Get(rb, s.id.c_str());
        if (t.status != SeamStatus::Found || t.rva != s.rva)
            std::printf("FAIL: a free masked byte changed %s: %s 0x%x (was 0x%x)\n", s.id.c_str(), StatusName(t.status), t.rva, s.rva);
        CHECK(t.status == SeamStatus::Found && t.rva == s.rva);
    }
}

// The globals move (build B to C moved all of them by +0x15000): shift every RIP displacement a decode reads by +0x40 at its site. Each global seam
// follows to the new address from every site that loads it, the functions and offsets do not notice. This kills an un-masked displacement of any
// decoded load (B evidence covers three of them only).
void TestGlobalsMove()
{
    const Registry reg = QualityRegistry();
    auto img = MakeQualityCWithSites();
    REQUIRE(img != nullptr);
    const ResolvedBuild base = ResolveImage(*img, reg);
    std::set<std::uint32_t> sites;
    for (std::size_t si = 0; si < reg.seams.size(); ++si)
    {
        for (std::size_t ai = 0; ai < reg.seams[si].anchors.size(); ++ai)
        {
            const AnchorSpec& a = reg.seams[si].anchors[ai];
            if (a.decode.kind == DecodeKind::RipDisp32 && reg.seams[si].kind == SeamKind::Global) // (a Function seam's lea has its own test below)
                sites.insert(base.seams[si].anchors[ai].match + static_cast<std::uint32_t>(a.decode.at) + a.decode.dispAt);
        }
    }
    CHECK(sites.size() == 9); // settings x3, manager, device x2, camera x2, chara
    for (std::uint32_t at : sites)
    {
        std::uint32_t disp = 0;
        CHECK(img->Read(at, &disp, 4));
        SetU32(*img, at, disp + 0x40);
    }
    const ResolvedBuild rb = ResolveImage(*img, reg);
    for (const SeamResult& s : base.seams)
    {
        const SeamResult& t = Get(rb, s.id.c_str());
        const bool isGlobal = reg.seams[&s - &base.seams[0]].kind == SeamKind::Global;
        const std::uint32_t want = isGlobal ? s.rva + 0x40 : s.rva;
        if (t.status != SeamStatus::Found || t.rva != want)
            std::printf("FAIL: after the globals moved %s: %s 0x%x (expected 0x%x)\n", s.id.c_str(), StatusName(t.status), t.rva, want);
        CHECK(t.status == SeamStatus::Found && t.rva == want);
    }
    CHECK(rb.build.nativeQuality.settingsPointerRva == 0x290e470 + 0x40 && rb.build.nativeQuality.managerPointerRva == 0x290e498 + 0x40 &&
          rb.build.nativeQuality.devicePointerRva == 0x2907d00 + 0x40 && rb.build.jitter.cameraGlobalRva == 0x290dba0 + 0x40 &&
          rb.build.charaView.managerPointerRva == 0x290e490 + 0x40);
}

// The call targets move: every anchor that decodes a call (the five Rel32 anchors) or a function address (the RIP-relative lea of the manager
// constructor, a Function seam) is the only anchor of its seam, the displacement at its site is shifted by +0x40, and the seam follows to the new
// target. This is what kills an un-masked call displacement of a decoded call (nothing else reads those bytes).
void TestCallTargetsFollow()
{
    const Registry full = QualityRegistry();
    auto baseImg = MakeQualityCWithSites();
    REQUIRE(baseImg != nullptr);
    const ResolvedBuild base = ResolveImage(*baseImg, full);
    int moved = 0;
    for (std::size_t si = 0; si < full.seams.size(); ++si)
    {
        for (std::size_t ai = 0; ai < full.seams[si].anchors.size(); ++ai)
        {
            const AnchorSpec& a = full.seams[si].anchors[ai];
            const bool lea = a.decode.kind == DecodeKind::RipDisp32 && a.valueFrom == ValueFrom::Target && full.seams[si].kind == SeamKind::Function;
            if (a.decode.kind != DecodeKind::Rel32 && !lea)
                continue;
            Registry reg = full;
            reg.seams[si].anchors = { a };
            auto img = MakeQualityCWithSites();
            REQUIRE(img != nullptr);
            const std::uint32_t at = base.seams[si].anchors[ai].match + static_cast<std::uint32_t>(a.decode.at) + (lea ? a.decode.dispAt : 1);
            std::int32_t rel = 0;
            CHECK(img->Read(at, &rel, 4));
            SetU32(*img, at, static_cast<std::uint32_t>(rel + 0x40));
            const ResolvedBuild rb = ResolveImage(*img, reg);
            const SeamResult& r = Get(rb, full.seams[si].id.c_str());
            const std::uint32_t want = base.seams[si].rva + 0x40;
            if (r.status != SeamStatus::Found || r.rva != want)
                std::printf("FAIL: the call of %s/%s moved by 0x40: %s 0x%x (expected 0x%x)\n", full.seams[si].id.c_str(), a.name.c_str(), StatusName(r.status), r.rva, want);
            CHECK(r.status == SeamStatus::Found && r.rva == want);
            ++moved;
        }
    }
    CHECK(moved == 6); // rendererUpdate caller, resize call, contextResolver call, cacheCompare call, camera getter call, the constructor's lea of querySize
}

// 4. offsets ----------------------------------------------------------------------------------------------------------------------------------
void TestOffsetsFollowTheCode()
{
    const Registry reg = QualityRegistry();
    const std::uint32_t kS1 = 0x3745d7, kResize = 0x2dab0d, kQuery = 0x352350;

    // all three sites of the mode offset moved: 0x54 -> 0x50 (inside the snapshot of the consumer, see TestSettingsOffsetsStayInsideTheSnapshot). The
    // seam follows, the build field takes the new value, the functions still resolve.
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        SetByte(*img, kS1 + 13, 0x50);
        SetByte(*img, kResize + 18, 0x50);
        SetByte(*img, kQuery + 0x3a, 0x50);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(Get(rb, "quality.settingsModeOffset").status == SeamStatus::Found && Get(rb, "quality.settingsModeOffset").rva == 0x50);
        CHECK(rb.build.nativeQuality.settingsModeOffset == 0x50);
        for (const char* id : { "quality.settingsInstructions", "quality.querySize", "quality.resizeCallbacks", "quality.settingsPointer" })
            CHECK(Get(rb, id).status == SeamStatus::Found);
        CHECK(rb.build.nativeQuality.settingsQualityOffset == 0x44 && rb.build.nativeQuality.settingsDlaaOffset == 0x45); // the others unmoved
    }
    // the sites disagree (one moved, two not): a Conflict, inert: nothing is filled, the field keeps the table default, a feature must not use it
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        SetByte(*img, kS1 + 13, 0x50);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(Get(rb, "quality.settingsModeOffset").status == SeamStatus::Conflict && !rb.Usable("quality.settingsModeOffset"));
        CHECK(Get(rb, "quality.settingsInstructions").status == SeamStatus::Found); // the block itself matches (the offset is masked): P3 must gate on the offset seams too
    }
    // a single-anchor offset (the selector) follows the one site that carries it
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        SetByte(*img, kS1 + 33, 0x53);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(Get(rb, "quality.settingsSelectorOffset").status == SeamStatus::Found && rb.build.nativeQuality.settingsSelectorOffset == 0x53);
    }
    // out of the sanity range: refused (NotFound), the field is untouched; the function seam does not notice
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        SetByte(*img, kS1 + 13, 0xf0);
        SetByte(*img, kResize + 18, 0xf0);
        SetByte(*img, kQuery + 0x3a, 0xf0);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(Get(rb, "quality.settingsModeOffset").status == SeamStatus::NotFound);
        CHECK(rb.build.nativeQuality.settingsModeOffset == 0x54);
        CHECK(Get(rb, "quality.settingsInstructions").status == SeamStatus::Found);
    }
    // the flags offset (two anchors: the test of the resolver prelude and the movzx of the second block): both moved: followed; one moved: Conflict, inert
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        SetU32(*img, 0x374598 + 2, 0x1a1);
        SetU32(*img, 0x37479c + 3, 0x1a1);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(Get(rb, "quality.contextFlagsOffset").status == SeamStatus::Found && rb.build.nativeQuality.contextFlagsOffset == 0x1a1);
        SetU32(*img, 0x37479c + 3, 0x181);
        const ResolvedBuild rb2 = ResolveImage(*img, reg);
        CHECK(Get(rb2, "quality.contextFlagsOffset").status == SeamStatus::Conflict && !rb2.Usable("quality.contextFlagsOffset"));
        CHECK(rb2.build.nativeQuality.contextFlagsOffset == 0x181); // inert: the table default stays
    }
    // the manager context offset (4 bytes) and the callbacks offset (1 byte) follow as well
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        SetU32(*img, kQuery + 0x28, 0x4a40);
        SetU32(*img, 0x3537c8 + 3, 0x4a40);
        SetByte(*img, kResize + 3, 0x38);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(Get(rb, "quality.managerContextOffset").status == SeamStatus::Found && rb.build.nativeQuality.managerContextOffset == 0x4a40);
        CHECK(Get(rb, "quality.deviceCallbacksOffset").status == SeamStatus::Found && rb.build.nativeQuality.deviceCallbacksOffset == 0x38);
        CHECK(Get(rb, "quality.querySize").status == SeamStatus::Found && Get(rb, "quality.resizeCallbacks").status == SeamStatus::Found);
    }
    // the camera offsets. A recompile that moved the offset everywhere: the accessor pins +0x3028 (fixed bytes) and stops matching, the producer
    // window decodes the new value: FoundSingleAnchor (usable, a WARN) with the new value; camera.global rests on the window alone too
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        SetU32(*img, 0x3740c9 + 24, 0x3040);
        SetU32(*img, 0x795b90 + 10, 0x3040);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(Get(rb, "camera.cameraOffset").status == SeamStatus::FoundSingleAnchor && Get(rb, "camera.cameraOffset").rva == 0x3040);
        CHECK(rb.build.jitter.cameraOffset == 0x3040 && rb.Usable("camera.cameraOffset"));
        CHECK(Get(rb, "camera.global").status == SeamStatus::FoundSingleAnchor && Get(rb, "camera.global").rva == 0x290dba0);
        CHECK(Get(rb, "camera.getter").status == SeamStatus::Found);
        // and the S -> P offset follows the getter's own instruction
        SetU32(*img, 0x25f613, 0x280);
        const ResolvedBuild rb2 = ResolveImage(*img, reg);
        CHECK(Get(rb2, "camera.jitterPointerOffset").status == SeamStatus::Found && rb2.build.jitter.jitterPointerOffset == 0x280);
    }
    // only the producer window moved (the accessor still says +0x3028): the two disagree, a Conflict, inert; the global is still agreed on
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        SetU32(*img, 0x3740c9 + 24, 0x3040);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(Get(rb, "camera.cameraOffset").status == SeamStatus::Conflict && !rb.Usable("camera.cameraOffset"));
        CHECK(rb.build.jitter.cameraOffset == 0x3028); // inert: the table default stays
        CHECK(Get(rb, "camera.global").status == SeamStatus::Found);
    }
    // the scene-size seam reports what the code says about the FIRST compare of the pair (the second member is fixed in the pattern, see below)
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        SetU32(*img, 0x2dab9a + 15, 0x430);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(Get(rb, "quality.rendererSceneSizeOffset").status == SeamStatus::Found && rb.build.nativeQuality.rendererSceneSizeOffset == 0x430);
        CHECK(Get(rb, "quality.devicePointer").status == SeamStatus::Found); // the device load of the same block does not care
    }
    // the pairs (+0x42c next to +0x428, +0x154 next to +0x150) are VERIFIED, not healed: the pattern keeps the second member fixed, so a layout
    // that moved the pair makes the seam NotFound instead of reading a wrong cache
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        SetByte(*img, 0x2dab9a + 29, 0x30); // +0x42c -> +0x430
        SetByte(*img, 0x374f10 + 31, 0x58); // +0x154 -> +0x158
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(Get(rb, "quality.rendererSceneSizeOffset").status == SeamStatus::NotFound);
        CHECK(Get(rb, "quality.contextCacheOffset").status != SeamStatus::Found);
        CHECK(rb.build.nativeQuality.rendererSceneSizeOffset == 0x428 && rb.build.nativeQuality.contextCacheOffset == 0x150); // untouched defaults
    }
    // the first member of the cache pair moved (+0x150 -> +0x158) while +0x154 stayed: the hooks WRITE at the first one, so this must not be followed.
    // The function is still found (its scan masks the first member); the offset seam is NotFound and the field keeps the table default.
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        SetU32(*img, 0x374f10 + 15, 0x158);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(Get(rb, "quality.cacheCompare").status == SeamStatus::Found);
        CHECK(Get(rb, "quality.contextCacheOffset").status == SeamStatus::NotFound && Get(rb, "quality.contextCacheOffset").anchors[0].patternHits == 0);
        CHECK(rb.build.nativeQuality.contextCacheOffset == 0x150 && !rb.Usable("quality.contextCacheOffset"));
    }
}

// MEDIUM-1: the consumer snapshots the settings struct into a std::array of kSettingsSnapshotSize = 0x56 bytes (a compile-time value taken from the
// table offsets) and indexes it with the resolved offsets WITHOUT a bounds check: an offset above 0x55 would read past the array. Until P3 sizes the
// snapshot from the resolved offsets, the four settings offsets that have a seam here (mode, quality, dlaa, selector) must not resolve above 0x55
// (the other two members of that snapshot, settingsRenderer 0x47 and settingsAdditional 0x49, are bound to no seam: they keep the table default).
void TestSettingsOffsetsStayInsideTheSnapshot()
{
    const Registry reg = QualityRegistry();
    constexpr std::uint32_t kS1 = 0x3745d7, kResize = 0x2dab0d, kQuery = 0x352350;
    struct OffsetSeam
    {
        const char* id;
        const char* key;
        std::vector<std::uint32_t> sites; // every byte that carries the offset
        std::uint32_t tableDefault;
    };
    const OffsetSeam seams[] = {
        { "quality.settingsModeOffset", "nativeQuality.settingsModeOffset", { kS1 + 13, kResize + 18, kQuery + 0x3a }, 0x54 },
        { "quality.settingsQualityOffset", "nativeQuality.settingsQualityOffset", { kS1 + 20, kResize + 24 }, 0x44 },
        { "quality.settingsDlaaOffset", "nativeQuality.settingsDlaaOffset", { kS1 + 26, kResize + 30 }, 0x45 },
        { "quality.settingsSelectorOffset", "nativeQuality.settingsSelectorOffset", { kS1 + 33 }, 0x55 },
    };
    for (const OffsetSeam& o : seams)
    {
        for (std::uint32_t to : { 0x10u, 0x55u, 0x56u, 0x58u, 0x7fu })
        {
            auto img = MakeQualityCWithSites();
            REQUIRE(img != nullptr);
            for (std::uint32_t at : o.sites)
                SetByte(*img, at, static_cast<std::uint8_t>(to));
            const ResolvedBuild rb = ResolveImage(*img, reg);
            const SeamResult& s = Get(rb, o.id);
            const bool inside = to <= 0x55;
            if (inside != (s.status == SeamStatus::Found) || (inside && s.rva != to) || (!inside && (s.status != SeamStatus::NotFound || s.rva != 0)))
                std::printf("FAIL: %s moved to 0x%x: %s rva=0x%x\n", o.id, to, StatusName(s.status), s.rva);
            CHECK(inside == (s.status == SeamStatus::Found));
            CHECK(inside ? s.rva == to : (s.status == SeamStatus::NotFound && s.rva == 0)); // refused: no value
            // a refused offset leaves the table default in the build (never a value the snapshot cannot hold), an accepted one is the new value
            CHECK(RvaOf(rb.build, o.key) == (inside ? to : o.tableDefault));
            CHECK(RvaOf(rb.build, o.key) <= 0x55);
        }
    }
    // the unbound members of the snapshot keep their table defaults whatever the code says
    const ResolvedBuild rb = ResolveImage(*MakeQualityCWithSites(), reg);
    CHECK(rb.build.nativeQuality.settingsRendererOffset == 0x47 && rb.build.nativeQuality.settingsAdditionalOffset == 0x49);
}

// The weak-seam cascade (P1fix CHANGES 14): a seam that is only FoundSingleAnchor makes every anchor that depends on it weak, and one weak unique
// anchor caps a seam at FoundSingleAnchor even when another anchor agrees. The seams stay USABLE (a WARN each), but `ffxiv-resolve --strict` exits
// 2 on such a build. Pinned here so that the report's list is a tested fact.
std::vector<std::string> NotPlainFound(const ResolvedBuild& rb)
{
    std::vector<std::string> ids;
    for (const SeamResult& s : rb.seams)
    {
        if (s.status != SeamStatus::Found)
            ids.push_back(s.id + ":" + StatusName(s.status));
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

void TestWeakCascade()
{
    const Registry reg = QualityRegistry();
    // querySize: a changed PROLOGUE (push r14 -> push r15) costs the entry anchor only; body and the manager constructor still agree: plain Found,
    // no weak seam anywhere (this is what the third anchor buys); only the shape refuses
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        SetByte(*img, 0x352360, 0x57);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        const std::vector<std::string> expected = { "quality.querySizeShape:NotFound" };
        const auto got = NotPlainFound(rb);
        if (got != expected)
        {
            for (const std::string& g : got)
                std::printf("    not plain Found: %s\n", g.c_str());
        }
        CHECK(got == expected);
        CHECK(AllZero(GuardOf(rb.build, "nativeQuality.querySizeInstructions")));
        CHECK(Get(rb, "quality.querySize").anchors[0].outcome == AnchorOutcome::Zero); // the entry anchor is the one that did not confirm
        CHECK(rb.build.nativeQuality.querySizeRva == 0x352350 && rb.build.nativeQuality.managerPointerRva == 0x290e498);
    }
    // querySize: entry AND body lost (a changed output store), only the manager constructor names the function: FoundSingleAnchor, and the weak
    // cascade of the report (fact 3) starts: the windows that hang off querySize, and everything that names them
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        SetByte(*img, 0x3523a9, 0x07);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        const std::vector<std::string> expected = { "quality.contextFlagsOffset:FoundSingleAnchor", "quality.contextResolver:FoundSingleAnchor",
                                                    "quality.managerContextOffset:FoundSingleAnchor", "quality.managerPointer:FoundSingleAnchor",
                                                    "quality.querySize:FoundSingleAnchor", "quality.querySizeShape:NotFound",
                                                    "quality.settingsModeOffset:FoundSingleAnchor", "quality.settingsPointer:FoundSingleAnchor" };
        const auto got = NotPlainFound(rb);
        if (got != expected)
        {
            for (const std::string& g : got)
                std::printf("    not plain Found: %s\n", g.c_str());
        }
        CHECK(got == expected);
        for (const SeamResult& s : rb.seams)
            CHECK(IsUsable(s.status) || s.id == "quality.querySizeShape"); // every one still usable, but the shape: it refuses (Guard stays zero)
        CHECK(AllZero(GuardOf(rb.build, "nativeQuality.querySizeInstructions")));
        CHECK(Get(rb, "quality.querySize").anchors[0].outcome == AnchorOutcome::Zero && Get(rb, "quality.querySize").anchors[1].outcome == AnchorOutcome::Zero &&
              Get(rb, "quality.querySize").anchors[2].outcome == AnchorOutcome::Unique);
        CHECK(rb.build.nativeQuality.querySizeRva == 0x352350 && rb.build.nativeQuality.managerPointerRva == 0x290e498);
    }
    // rendererUpdate: a changed body leaves the caller alone; the seams that name rendererUpdate (guard or SameFunctionAs) become weak
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        SetByte(*img, 0x2da5ce, 0x11); // movss xmm0,[rcx+0x710] -> +0x711: entry and body no longer match
        const ResolvedBuild rb = ResolveImage(*img, reg);
        const std::vector<std::string> expected = { "quality.devicePointer:FoundSingleAnchor", "quality.rendererSceneSizeOffset:FoundSingleAnchor",
                                                    "quality.rendererUpdate:FoundSingleAnchor", "quality.rendererUpdateShape:NotFound",
                                                    "quality.resizeCallbacks:FoundSingleAnchor", "quality.resizeCallbacksShape:FoundSingleAnchor" };
        const auto got = NotPlainFound(rb);
        if (got != expected)
        {
            for (const std::string& g : got)
                std::printf("    not plain Found: %s\n", g.c_str());
        }
        CHECK(got == expected);
        CHECK(Get(rb, "quality.rendererUpdate").anchors[2].outcome == AnchorOutcome::Unique && Get(rb, "quality.rendererUpdate").anchors[0].outcome == AnchorOutcome::Zero);
    }
}

// 5. the failure chain ----------------------------------------------------------------------------------------------------------------------
void TestFailureChain()
{
    const Registry reg = QualityRegistry();
    // querySize erased (entry and body gone, the constructor still names it): FoundSingleAnchor at the right address, but every window that hangs off
    // it is zero: the manager (one anchor, a guard on querySize) goes, the settings global survives on its two other anchors (the settings block and
    // the resize block) and its third anchor (the guard on querySize) found nothing
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        const std::vector<std::uint8_t> zeros(112, 0);
        CHECK(img->Put(0x352350, zeros));
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(Get(rb, "quality.querySize").status == SeamStatus::FoundSingleAnchor && Get(rb, "quality.querySize").rva == 0x352350);
        CHECK(Get(rb, "quality.querySizeShape").status == SeamStatus::NotFound);
        CHECK(Get(rb, "quality.managerPointer").status == SeamStatus::NotFound);
        CHECK(Get(rb, "quality.managerContextOffset").status == SeamStatus::FoundSingleAnchor); // the constructor store still carries it
        const SeamResult& sp = Get(rb, "quality.settingsPointer");
        CHECK(sp.status == SeamStatus::Found && sp.rva == 0x290e470 && sp.anchors[2].outcome == AnchorOutcome::Zero);
        CHECK(Get(rb, "quality.contextResolver").status == SeamStatus::FoundSingleAnchor); // through the settings block alone (a WARN)
        CHECK(Get(rb, "quality.contextFlagsOffset").status == SeamStatus::FoundSingleAnchor && Get(rb, "quality.contextFlagsOffset").anchors[0].weakDependency);
        CHECK(Get(rb, "quality.rendererUpdate").status == SeamStatus::Found && Get(rb, "quality.devicePointer").status == SeamStatus::Found);
    }
    // rendererUpdate erased everywhere: the device global (both anchors depend on it) and the resize call (SameFunctionAs) are NotFound /
    // single-sourced, the settings seams are untouched
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        const std::vector<std::uint8_t> zeros(73, 0);
        CHECK(img->Put(0x2da5b0, zeros));
        const std::vector<std::uint8_t> zeros2(33, 0);
        CHECK(img->Put(0x2b9e69, zeros2));
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(Get(rb, "quality.rendererUpdate").status == SeamStatus::NotFound);
        CHECK(Get(rb, "quality.devicePointer").status == SeamStatus::NotFound);
        CHECK(Get(rb, "quality.resizeCallbacks").status == SeamStatus::FoundSingleAnchor); // the function's own body is the only anchor left
        CHECK(Get(rb, "quality.querySize").status == SeamStatus::Found && Get(rb, "quality.settingsInstructions").status == SeamStatus::Found);
    }
    // the settings block erased: its three dependants (CodeSite, the selector offset, cacheCompare's call anchor) lose their anchor; the rest of the
    // group resolves
    {
        auto img = MakeQualityCWithSites();
        REQUIRE(img != nullptr);
        const std::vector<std::uint8_t> zeros(37, 0);
        CHECK(img->Put(0x3745d7, zeros));
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(Get(rb, "quality.settingsInstructions").status == SeamStatus::NotFound);
        CHECK(Get(rb, "quality.settingsSelectorOffset").status == SeamStatus::NotFound);
        CHECK(Get(rb, "quality.cacheCompare").status == SeamStatus::FoundSingleAnchor); // the body anchor alone
        CHECK(Get(rb, "quality.settingsPointer").status == SeamStatus::Found && Get(rb, "quality.querySize").status == SeamStatus::Found);
    }
    // nothing planted: nothing is found, every seam says NotFound, no field is filled
    {
        auto img = MakeQualityC();
        REQUIRE(img != nullptr);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        for (const SeamResult& s : rb.seams)
            CHECK(s.status == SeamStatus::NotFound);
        CHECK(rb.build.nativeQuality.querySizeRva == 0 && rb.build.nativeQuality.settingsInstructions[0] == 0 && rb.build.jitter.cameraGlobalRva == 0);
        // and on a known build the table fills it (M11), while an EntryPatched seam would not be (fail closed)
        ResolveOptions opts;
        opts.knownTable = &ext::ffxiv::kCurrentBuild;
        const ResolvedBuild known = ResolveImage(*img, reg, opts);
        CHECK(known.knownBuild && Get(known, "quality.querySize").fellBackToTable && known.build.nativeQuality.querySizeRva == 0x352350);
    }
}
} // namespace

int main()
{
    TestPredicateDecoys();
    TestCharaManagerGuard();
    TestCharaTargetsRange();
    TestEntryPatched();
    TestShapeSeams();
    TestWrongFunctionBehindACaller();
    TestShapeFailClosedOnKnownBuild();
    TestConsumerPins();
    TestMaskedBytesDoNotMatter();
    TestEveryUnreadMaskedByteIsFree();
    TestGlobalsMove();
    TestCallTargetsFollow();
    TestOffsetsFollowTheCode();
    TestSettingsOffsetsStayInsideTheSnapshot();
    TestWeakCascade();
    TestFailureChain();
    return TEST_RESULT("resolve_quality_controls");
}
