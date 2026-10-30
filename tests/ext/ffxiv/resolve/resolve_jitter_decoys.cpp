// The jitter-source group: every predicate removes its PLANTED decoy (most anchors are unique on the real image even without a predicate,
// so a dropped predicate is invisible there: here a decoy is planted in the committed layout), the Offset / constant ranges (ValueIn,
// ValueEquals) are exact, a foreign hook over a Function entry reads EntryPatched (and what that does to the fields and to the dependents),
// the two Detoured functions can never resolve to the same address, and the device reader's two byte-identical template copies behave as
// documented. Always on: no executable.
#include "resolve_jitter_shape_data.h"

#include "ext/ffxiv/jitter/FfxivJitterSourceAnchors.h"

#include <cstring>

using namespace jt;

namespace
{
ResolvedBuild Run(const DenseImage& img, const Registry& reg) { return ResolveImage(img, reg); }

// plants the bytes a pattern describes (wildcards 0) at an RVA, then returns that RVA
std::uint32_t PlantPattern(DenseImage& img, std::uint32_t rva, const std::string& pattern, std::uint8_t filler = 0)
{
    const auto bytes = Materialize(pattern, filler);
    if (!img.Put(rva, bytes))
    {
        std::printf("FAIL: could not plant a pattern at 0x%x\n", rva);
        ++g_failures;
    }
    return rva;
}

void PutLe32(DenseImage& img, std::uint32_t rva, std::uint32_t v) { img.Put(rva, &v, sizeof(v)); }

const AnchorSpec& AnchorOf(const Registry& reg, const char* seam, std::size_t i)
{
    static const AnchorSpec none;
    const SeamSpec* s = FindSeam(reg, seam);
    if (s == nullptr || i >= s->anchors.size())
    {
        std::printf("FAIL: no anchor %zu in seam %s\n", i, seam);
        ++g_failures;
        return none;
    }
    return s->anchors[i];
}

// the registry with the first predicate of `kind` removed from one anchor (what a mutant that drops it would build)
Registry WithoutPredicate(const char* seam, std::size_t anchor, PredicateKind kind)
{
    Registry reg = JitterRegistry();
    SeamSpec* s = FindSeam(reg, seam);
    if (s == nullptr || anchor >= s->anchors.size())
    {
        std::printf("FAIL: no anchor %zu in seam %s\n", anchor, seam);
        ++g_failures;
        return reg;
    }
    auto& preds = s->anchors[anchor].predicates;
    for (auto it = preds.begin(); it != preds.end(); ++it)
    {
        if (it->kind == kind)
        {
            preds.erase(it);
            if (s->anchors[anchor].minMargin == 0 && preds.empty())
                s->anchors[anchor].minMargin = 3; // ValidateRegistry refuses margin 0 without a predicate (tested below): a mutant keeps the registry valid
            return reg;
        }
    }
    std::printf("FAIL: seam %s anchor %zu has no predicate of that kind\n", seam, anchor);
    ++g_failures;
    return reg;
}

bool Contains(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

// ---- getter: Before(cameraHead) --------------------------------------------------------------------------------------------

void TestGetterPosition()
{
    auto img = MakeJitterImage(0x11111111);
    REQUIRE(img != nullptr); // the committed image carries the real decoy at 0x148c408 (the second raw hit of the 8-byte pattern)
    const ResolvedBuild rb = Run(*img, JitterRegistry());
    const SeamResult& g = Get(rb, "jitterSource.cameraGetter");
    CHECK(g.status == SeamStatus::Found && g.rva == 0x25f610);
    CHECK(g.anchors[0].patternHits == 2 && g.anchors[0].candidates == 1 && g.anchors[0].outcome == AnchorOutcome::Unique);
    CHECK(Contains(g.anchors[0].note, "jitterSource.cameraHead"));

    // the predicate dropped: both raw hits survive, the anchor is ambiguous, the second anchor (the caller) still names 0x25f610, so the
    // seam can only be a single-anchor one: NOT plain Found
    const ResolvedBuild dropped = Run(*img, WithoutPredicate("jitterSource.cameraGetter", 0, PredicateKind::Within));
    const SeamResult& d = Get(dropped, "jitterSource.cameraGetter");
    CHECK(d.anchors[0].outcome == AnchorOutcome::Multiple && d.anchors[0].candidates == 2);
    CHECK(d.status == SeamStatus::FoundSingleAnchor && d.rva == 0x25f610);

    // the registry validation is a second line: margin 0 ("the predicate is the disambiguator") without a predicate is refused
    {
        Registry bare = JitterRegistry();
        FindSeam(bare, "jitterSource.cameraGetter")->anchors[0].predicates.clear();
        CHECK(!ValidateRegistry(bare).empty());
    }

    // the caller anchor on its own is unique and decodes the target (independent of the bytes of the getter)
    CHECK(g.anchors[1].outcome == AnchorOutcome::Unique && g.anchors[1].value == 0x25f610);
}

// The window of Before(cameraHead, 0x1000) is exact on its far side: a copy of the 8 bytes planted at the LAST position inside the window is a
// candidate (the anchor becomes ambiguous), one byte further it is not (the anchor stays unique). A widened window (the review's mutant: 0x2d0000
// kept the real decoy at 0x148c408 out but let anything closer in) fails the second case, a narrowed one the first.
void TestGetterWindow()
{
    constexpr std::uint32_t kHead = 0x25f690, kWindow = 0x1000;
    const std::string pattern = AnchorOf(JitterRegistry(), "jitterSource.cameraGetter", 0).pattern;
    struct Case
    {
        const char* what;
        std::uint32_t at;
        std::uint32_t candidates; // of the first getter anchor
    };
    for (const Case& c : { Case { "the last position inside the window (head - 0x1000)", kHead - kWindow, 2 },
                           Case { "one byte outside the window (head - 0x1001)", kHead - kWindow - 1, 1 },
                           Case { "well inside the window (head - 0x100)", kHead - 0x100, 2 },
                           Case { "after the camera update (head + 0x10000)", kHead + 0x10000, 1 },
                           Case { "far before it (head - 0x100000)", kHead - 0x100000, 1 } })
    {
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        PlantPattern(*img, c.at, pattern);
        const ResolvedBuild rb = Run(*img, JitterRegistry());
        const SeamResult& g = Get(rb, "jitterSource.cameraGetter");
        const AnchorResult& a = g.anchors[0];
        if (a.candidates != c.candidates)
            std::printf("FAIL: getter window, decoy %s: %u candidates, expected %u\n", c.what, a.candidates, c.candidates);
        CHECK(a.patternHits == 3 && a.candidates == c.candidates);
        CHECK(a.outcome == (c.candidates == 1 ? AnchorOutcome::Unique : AnchorOutcome::Multiple));
        // the caller anchor names the getter either way, so the seam keeps the right value (single-anchor when the first anchor is ambiguous)
        CHECK(g.rva == 0x25f610 && rb.Usable("jitterSource.cameraGetter"));
        CHECK((g.status == SeamStatus::Found) == (c.candidates == 1));
    }
}

// ---- phases / strength / producer: SameFunctionAs and CallsSeam ------------------------------------------------------------
//
// Each of these seams has two anchors. For every anchor that carries a predicate of `kind`, a decoy copy of THAT anchor's pattern is planted
// (a second raw hit that only the predicate can remove): with the predicate the anchor keeps its single candidate, without it (a registry in
// which that one predicate is dropped, everything else equal) the anchor has two and the seam can no longer be plain Found.
//
// `sharedPattern`: the strength seam's second anchor scans the very pattern the generator seam's caller anchor scans, so a decoy of it is a
// decoy of the generator's anchor as well (the generator, and through its CallsSeam the strength, then read single-anchor): the seam is then
// only required to stay usable with the right value.
struct DecoyCase
{
    const char* seam;
    PredicateKind kind;
    std::vector<ExtraFunction> extra; // functions the exception directory lists (the decoys' homes)
    std::uint32_t base;               // where the first decoy goes
    std::uint32_t spacing;
    std::uint32_t callTarget;         // the decoy's E8 target for a Rel32 anchor (0 = harmless, its own next byte)
    const char* note;                 // what the anchor's note must say about the dropped decoy
    std::uint32_t value;              // the seam's real value
};

void RunDecoyCase(const DecoyCase& c)
{
    const Registry real = JitterRegistry();
    const SeamSpec* seam = FindSeam(real, c.seam);
    REQUIRE(seam != nullptr);
    std::size_t ran = 0;
    for (std::size_t i = 0; i < seam->anchors.size(); ++i)
    {
        const AnchorSpec& spec = seam->anchors[i];
        if (std::none_of(spec.predicates.begin(), spec.predicates.end(), [&](const Predicate& p) { return p.kind == c.kind; }))
            continue;
        ++ran;
        auto img = MakeJitterImage(0x11111111, c.extra);
        REQUIRE(img != nullptr);
        const std::uint32_t site = PlantPattern(*img, c.base, spec.pattern);
        if (spec.decode.kind == DecodeKind::Rel32)
            PutLe32(*img, site + spec.decode.at + 1, c.callTarget == 0 ? 0u : c.callTarget - (site + spec.decode.at + 5));
        const bool shared = spec.pattern == AnchorOf(real, "jitterSource.generator", 1).pattern;

        const ResolvedBuild rb = Run(*img, real);
        const SeamResult& s = Get(rb, c.seam);
        if (!(shared ? rb.Usable(c.seam) : s.status == SeamStatus::Found))
            std::printf("FAIL: %s anchor %zu with its decoy: %s (%s)\n", c.seam, i, StatusName(s.status), s.reason.c_str());
        CHECK(s.rva == c.value && (shared ? rb.Usable(c.seam) : s.status == SeamStatus::Found));
        CHECK(s.anchors[i].patternHits == 2 && s.anchors[i].candidates == 1 && s.anchors[i].outcome == AnchorOutcome::Unique);
        CHECK(Contains(s.anchors[i].note, c.note));

        const ResolvedBuild dropped = Run(*img, WithoutPredicate(c.seam, i, c.kind));
        const SeamResult& d = Get(dropped, c.seam);
        if (d.anchors[i].candidates != 2 || d.status == SeamStatus::Found)
            std::printf("FAIL: %s anchor %zu without the predicate: %s, %u candidates\n", c.seam, i, StatusName(d.status), d.anchors[i].candidates);
        CHECK(d.anchors[i].candidates == 2 && d.anchors[i].outcome == AnchorOutcome::Multiple);
        CHECK(d.status != SeamStatus::Found && d.rva == c.value && dropped.Usable(c.seam)); // the other anchor still names the value: single anchor
    }
    CHECK(ran == seam->anchors.size() && ran == 2); // both anchors of each of these seams carry the predicate: nothing was skipped silently
}

void TestPhasesSameFunction()
{
    // a copy in ANOTHER function that has its own unwind entry ...
    RunDecoyCase({ "jitterSource.phases", PredicateKind::SameFunctionAs, { { 0x2ff000, 0x301000 } }, 0x300000, 0x100, 0,
                   "another function than 'jitterSource.cameraHead'", 0x25f721 });
    // ... and a copy where no unwind entry covers it (unknown is refused as well)
    RunDecoyCase({ "jitterSource.phases", PredicateKind::SameFunctionAs, {}, 0x400000, 0x100, 0, "no unwind entry", 0x25f721 });
}

void TestStrengthCallsAndFunction()
{
    // (a) a copy IN the camera update (same function) that calls something else: CallsSeam(generator) drops it
    RunDecoyCase({ "jitterSource.strength", PredicateKind::CallsSeam, {}, 0x25f880, 0x40, 0, "not a call of 'jitterSource.generator'", 0x25f799 });
    // (b) a copy in ANOTHER function that really calls the generator: SameFunctionAs(cameraHead) drops it
    RunDecoyCase({ "jitterSource.strength", PredicateKind::SameFunctionAs, { { 0x2ff000, 0x301000 } }, 0x300000, 0x100, 0x2ebd90,
                   "another function than 'jitterSource.cameraHead'", 0x25f799 });
}

void TestProducerCallsGetter()
{
    // a copy of each anchor (an E8 with the same surroundings) that calls its own next byte
    RunDecoyCase({ "jitterSource.producerCall", PredicateKind::CallsSeam, {}, 0x500000, 0x100, 0, "not a call of 'jitterSource.cameraGetter'", 0x37411d });

    // the producer's call lands somewhere else than on the function with the getter's bytes: the two anchors of the getter disagree
    // (Conflict, inert) and the call seam has no getter to compare with: nothing is hooked
    const Registry reg = JitterRegistry();
    auto moved = MakeJitterImage(0x11111111);
    REQUIRE(moved != nullptr);
    PutLe32(*moved, 0x37411e, 0x1000); // E8 rel32: another target
    const ResolvedBuild rb2 = Run(*moved, reg);
    CHECK(Get(rb2, "jitterSource.cameraGetter").status == SeamStatus::Conflict && rb2.build.jitterSource.cameraGetterRva == 0);
    CHECK(!rb2.Usable("jitterSource.producerCall") && !rb2.Usable("jitterSource.producerCallShape"));
    CHECK(rb2.build.jitterSource.producerReturnRva == 0 && rb2.build.jitterSource.producerCallInstructions[0] == 0);
}

// ---- deviceSize: TargetIs(devicePointer), two byte-identical template copies ---------------------------------------------------

void TestDeviceSizeCopies()
{
    const Registry reg = JitterRegistry();
    const std::string pattern = AnchorOf(reg, "jitterSource.deviceSize", 0).pattern;

    // the committed layout has the two real copies: the first one is the seam's (a swapped pickIndex gives 0xd4013)
    {
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        const ResolvedBuild rb = Run(*img, reg);
        const SeamResult& s = Get(rb, "jitterSource.deviceSize");
        CHECK(s.status == SeamStatus::Found && s.rva == 0xd3f73 && s.anchors[0].match == 0xd3f73 && s.hits == 2);
        CHECK(s.anchors[0].candidateValues.size() == 2 && s.anchors[0].candidateValues[0] == 0xd3f73 && s.anchors[0].candidateValues[1] == 0xd4013);
    }
    // a third copy that reads ANOTHER global is dropped by TargetIs; without the predicate the seam is ambiguous (3 candidates, 2 expected)
    {
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        const std::uint32_t site = PlantPattern(*img, 0x400000, pattern);
        PutLe32(*img, site + 3, 0x2907d40 - (site + 7)); // a bss global that is not the device global
        const ResolvedBuild rb = Run(*img, reg);
        const SeamResult& s = Get(rb, "jitterSource.deviceSize");
        CHECK(s.status == SeamStatus::Found && s.rva == 0xd3f73 && s.anchors[0].patternHits == 3 && s.anchors[0].candidates == 2);
        CHECK(Contains(s.anchors[0].note, "not the value of 'jitterSource.devicePointer'"));
        CHECK(Get(Run(*img, WithoutPredicate("jitterSource.deviceSize", 0, PredicateKind::TargetIsSeam)), "jitterSource.deviceSize").status ==
              SeamStatus::Ambiguous);
    }
    // a third copy that reads the SAME global makes the seam ambiguous (fail closed: nothing says which copy is meant)
    {
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        const std::uint32_t site = PlantPattern(*img, 0x400000, pattern);
        PutLe32(*img, site + 3, 0x2907d00 - (site + 7));
        const ResolvedBuild rb = Run(*img, reg);
        CHECK(Get(rb, "jitterSource.deviceSize").status == SeamStatus::Ambiguous && rb.build.jitterSource.deviceSizeRva == 0);
        CHECK(rb.build.jitterSource.deviceSizeInstructions[0] == 0);
    }
    // only ONE copy left: the seam expects two, so it is ambiguous too (and the hooks refuse)
    {
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        const std::vector<std::uint8_t> zeros(20, 0);
        img->Put(0xd4013, zeros);
        const ResolvedBuild rb = Run(*img, reg);
        CHECK(Get(rb, "jitterSource.deviceSize").status == SeamStatus::Ambiguous);
        CHECK(!ext::ffxiv::jitter::source::VerifyAnchors(ReaderOf(*img), rb.build.jitterSource, kBase).ok);
    }
}

// ---- the globals: TargetIn(Bss) --------------------------------------------------------------------------------------------

void TestGlobalsMustBeBss()
{
    struct Case
    {
        const char* seam;
        std::uint32_t dispAt;      // the RVA of the disp32 of ONE of the loads, patched to point into initialized .data
        std::uint32_t loadAt;      // the RVA of that instruction (rip = loadAt + 7)
        std::size_t anchorIndex;   // its anchor in the seam
        SeamStatus expect;         // what is left of the seam: a global with two anchors keeps its value as a single anchor
        SeamStatus expectDropped;  // the same with the predicate removed: the derivations disagree (Conflict), a one-anchor global is quietly wrong
    };
    const Case cases[] = {
        { "jitterSource.settingsPointer", 0x25f6c7, 0x25f6c4, 0, SeamStatus::FoundSingleAnchor, SeamStatus::Conflict }, // 2 anchors: one left
        { "jitterSource.rendererPointer", 0x25f724, 0x25f721, 0, SeamStatus::Found, SeamStatus::Conflict },             // 3 anchors: two agree
        { "jitterSource.devicePointer", 0x3740da, 0x3740d7, 1, SeamStatus::FoundSingleAnchor, SeamStatus::Conflict },   // 2 anchors: one left
        { "jitterSource.cameraGlobal", 0x3740cc, 0x3740c9, 0, SeamStatus::NotFound, SeamStatus::Found },                // 1 anchor: nothing left
    };
    for (const Case& c : cases)
    {
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        PutLe32(*img, c.dispAt, 0x28c0000 - (c.loadAt + 7)); // target 0x28c0000: initialized .data, not bss
        const ResolvedBuild rb = Run(*img, JitterRegistry());
        const SeamResult& s = Get(rb, c.seam);
        if (s.status != c.expect)
            std::printf("FAIL: %s is %s (%s)\n", c.seam, StatusName(s.status), s.reason.c_str());
        CHECK(s.status == c.expect && s.hasValue == (c.expect != SeamStatus::NotFound));
        CHECK(s.anchors[c.anchorIndex].outcome == AnchorOutcome::Zero && Contains(s.anchors[c.anchorIndex].note, "not in the required region"));
        // the rest of the chain is untouched: the shapes mask the displacement, the other derivations carry the global
        CHECK(Get(rb, "jitterSource.phasesShape").status == SeamStatus::Found && Get(rb, "jitterSource.cameraHeadShape").status == SeamStatus::Found);

        // without the predicate the anchors DISAGREE (Conflict, inert) or, with one anchor, the wrong global is simply taken
        Registry bare = JitterRegistry();
        SeamSpec* seam = FindSeam(bare, c.seam);
        REQUIRE(seam != nullptr);
        seam->anchors[c.anchorIndex].predicates.clear();
        const ResolvedBuild dropped = Run(*img, bare);
        CHECK(Get(dropped, c.seam).status == c.expectDropped);
        if (c.expectDropped == SeamStatus::Found)
            CHECK(Get(dropped, c.seam).rva == 0x28c0000); // the quiet wrong global: exactly what TargetIn(Bss) prevents
    }
}

// ---- the ranges of the Offset seams and of the constants ---------------------------------------------------------------------

// Plants a marker, a site behind it and tests ONE anchor of the real registry (its fromSeam replaced by the marker's seam): the decoded value
// must be accepted exactly inside [lo, hi] (Field) or for the constant at the target (RipDeref32).
//
// The bounds are LITERALS of this test (the review: reading lo/hi from the registry and then testing "accepts lo, hi, refuses lo-1, hi+1" passes
// for ANY widening). Each row names the anchor, pins the registry's bounds to the literals and drives the accept / refuse values from the literals,
// so a widened (or narrowed) registry fails here whatever the pin text says.
struct Bound
{
    const char* id;
    std::size_t anchor;
    const char* name;
    std::uint32_t lo, hi;
};
const Bound kBounds[] = {
    { "jitterSource.settingsModeOffset", 0, "modeByte", 0x10, 0x7f },
    { "jitterSource.settingsScaleOffset", 0, "scale", 0x10, 0x7f },
    { "jitterSource.settingsAmountOffset", 0, "amount", 0x10, 0x7f },
    { "jitterSource.rendererSceneSizeOffset", 0, "sceneInPhases", 0x100, 0xfff },
    { "jitterSource.rendererSceneSizeOffset", 1, "sceneInProducer", 0x100, 0xfff },
    { "jitterSource.rendererExtentOffset", 0, "extents", 0x100, 0xfff },
    { "jitterSource.deviceOutputSizeOffset", 0, "widthInPhases", 0x10, 0xff },
    { "jitterSource.deviceOutputSizeOffset", 1, "widthInDeviceSize", 0x10, 0xff },
    { "jitterSource.strengthFloor", 0, "floorInSubss", 0x3a83126f, 0x3a83126f }, // exactly 0.001f: the value the core hard-codes
    { "jitterSource.strengthFloor", 1, "floorInAddss", 0x3a83126f, 0x3a83126f },
    { "jitterSource.ndcSignMask", 0, "signMask", 0x80000000u, 0x80000000u },
};

void TestOffsetRanges()
{
    const Registry real = JitterRegistry();
    // every anchor of the registry that carries a ValueIn predicate has a row in kBounds (a new bound cannot slip in untested)
    {
        std::size_t withBound = 0;
        for (const SeamSpec& s : real.seams)
        {
            for (const AnchorSpec& a : s.anchors)
                withBound += std::any_of(a.predicates.begin(), a.predicates.end(), [](const Predicate& p) { return p.kind == PredicateKind::ValueIn; }) ? 1 : 0;
        }
        CHECK(withBound == sizeof(kBounds) / sizeof(kBounds[0]));
    }
    constexpr std::uint32_t kMarker = 0x600000, kSite = 0x600010, kConstant = 0x2120000;
    const std::string marker = "5A 5A 5A 5A 5A 5A 5A 5A";
    for (const Bound& bound : kBounds)
    {
        const char* id = bound.id;
        const SeamSpec* seam = FindSeam(real, id);
        REQUIRE(seam != nullptr && bound.anchor < seam->anchors.size());
        {
            const std::size_t ai = bound.anchor;
            const AnchorSpec& spec = seam->anchors[ai];
            REQUIRE(spec.name == bound.name && spec.predicates.size() == 1 && spec.predicates[0].kind == PredicateKind::ValueIn);
            const std::uint32_t lo = bound.lo, hi = bound.hi;
            if (spec.predicates[0].valueLo != lo || spec.predicates[0].valueHi != hi)
                std::printf("FAIL: %s/%s bounds are 0x%x..0x%x in the registry, 0x%x..0x%x in the test\n", id, spec.name.c_str(), spec.predicates[0].valueLo,
                            spec.predicates[0].valueHi, lo, hi);
            CHECK(spec.predicates[0].valueLo == lo && spec.predicates[0].valueHi == hi);

            std::vector<std::uint32_t> accept = { lo, hi, lo + (hi - lo) / 2 }, reject;
            if (spec.decode.kind == DecodeKind::Field)
            {
                const std::uint32_t capacity = spec.decode.width == 1 ? 0xff : 0xffff; // the fixed high bytes of the pattern are zero
                if (lo > 0)
                    reject.push_back(lo - 1);
                if (hi < capacity)
                    reject.push_back(hi + 1);
                reject.push_back(0);
            }
            else
            {
                CHECK(spec.decode.kind == DecodeKind::RipDeref32);
                if (lo > 0)
                    reject.push_back(lo - 1);
                if (hi < 0xffffffffu)
                    reject.push_back(hi + 1);
            }
            const bool exact = lo == hi; // ValueEquals: only one value passes

            auto test = [&](std::uint32_t value) -> bool
            {
                auto img = MakeJitterImage(0x11111111);
                if (!img)
                    return false;
                PlantPattern(*img, kMarker, marker);
                PlantPattern(*img, kSite, spec.pattern);
                if (spec.decode.kind == DecodeKind::Field)
                {
                    for (unsigned k = 0; k < spec.decode.width; ++k)
                        img->Bytes()[kSite + spec.decode.at + k] = static_cast<std::uint8_t>(value >> (8 * k));
                }
                else
                {
                    const std::uint32_t insn = kSite + spec.decode.at;
                    PutLe32(*img, insn + spec.decode.dispAt, kConstant - (insn + spec.decode.length));
                    PutLe32(*img, kConstant, value);
                }
                Registry t;
                AnchorSpec m;
                m.name = "marker";
                m.pattern = marker;
                m.valueOffset = static_cast<std::int32_t>(kSite - kMarker); // the site is 0x10 after the marker
                t.seams.push_back({ "t.site", SeamKind::CodeSite, { m } });
                AnchorSpec a = spec;
                a.fromSeam = "t.site";
                a.fromOffset = 0;
                t.seams.push_back({ "t.value", SeamKind::Offset, { a } });
                const ResolvedBuild rb = Run(*img, t);
                const SeamResult& s = Get(rb, "t.value");
                return s.status == SeamStatus::Found && s.rva == value;
            };
            for (std::uint32_t v : accept)
            {
                const bool ok = test(v);
                if (!ok)
                    std::printf("FAIL: %s/%s should accept 0x%x\n", id, spec.name.c_str(), v);
                CHECK(ok);
                if (exact)
                    break;
            }
            for (std::uint32_t v : reject)
            {
                const bool refused = !test(v);
                if (!refused)
                    std::printf("FAIL: %s/%s should refuse 0x%x\n", id, spec.name.c_str(), v);
                CHECK(refused);
            }
        }
    }
}

// ---- EntryPatched ---------------------------------------------------------------------------------------------------------

void TestForeignHookOverAnEntry()
{
    struct Case
    {
        const char* seam;
        const char* shape;
        std::uint32_t entry;
        const char* rvaKey;
        const char* guardKey; // the Guard field of the shape seam
        const char* block;    // the VerifyAnchors block of that site
    };
    const Registry reg = JitterRegistry();
    for (const Case& c : { Case { "jitterSource.generator", "jitterSource.generatorShape", 0x2ebd90, "generatorRva", "generatorInstructions", "generator" },
                           Case { "jitterSource.cameraGetter", "jitterSource.cameraGetterShape", 0x25f610, "cameraGetterRva", "cameraGetterInstructions", "cameraGetter" },
                           Case { "jitterSource.cameraHead", "jitterSource.cameraHeadShape", 0x25f690, "cameraHeadRva", "cameraHeadInstructions", "cameraHead" } })
    {
        // unknown build: a jump over the entry; the seam still FINDS the function (a body anchor / the caller), but it is EntryPatched, its fields
        // stay zero and so does its shape's guard: the hooks' byte compare refuses
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        img->Bytes()[c.entry] = 0xe9;
        const ResolvedBuild rb = Run(*img, reg);
        const SeamResult& s = Get(rb, c.seam);
        if (s.status != SeamStatus::EntryPatched)
            std::printf("FAIL: %s is %s (%s)\n", c.seam, StatusName(s.status), s.reason.c_str());
        CHECK(s.status == SeamStatus::EntryPatched && s.hasValue && s.rva == c.entry);
        CHECK(!rb.Usable(c.seam) && !rb.Usable(c.shape));
        const FieldRow* row = FindField(std::string("jitterSource.") + c.rvaKey);
        REQUIRE(row != nullptr);
        CHECK(*FieldU32(rb.build, *row) == 0); // the patched seam fills nothing; the others still do
        CHECK(!ext::ffxiv::jitter::source::VerifyAnchors(ReaderOf(*img), rb.build.jitterSource, kBase).ok);

        // known build (the stamp is C's and the table is this build): an EntryPatched seam is NOT filled from the table (fail closed) and its
        // shape (NotFound, failClosed) is not filled either: its Guard stays zero, so the location AND the shape keep the hooks off. (Before the
        // failClosed fix the shape fell back to the table's short guard prefix and only the location refused.)
        auto known = MakeJitterImage(kStampC);
        REQUIRE(known != nullptr);
        known->Bytes()[c.entry] = 0xe9;
        ResolveOptions opts;
        opts.knownTable = &ext::ffxiv::kCurrentBuild;
        const ResolvedBuild kb = ResolveImage(*known, reg, opts);
        const SeamResult& ks = Get(kb, c.seam);
        CHECK(kb.knownBuild && ks.status == SeamStatus::EntryPatched && !ks.fellBackToTable && !kb.UsableOrTable(c.seam));
        CHECK(*FieldU32(kb.build, *row) == 0);
        // the known-build behaviour: the location is EntryPatched (found, unfilled), the shape is NotFound and is NOT filled from the table
        CHECK(Get(kb, c.shape).status == SeamStatus::NotFound && !Get(kb, c.shape).fellBackToTable && !kb.UsableOrTable(c.shape));
        CHECK(GuardIsZero(kb, (std::string("jitterSource.") + c.guardKey).c_str()));
        const auto kr = ext::ffxiv::jitter::source::VerifyAnchors(ReaderOf(*known), kb.build.jitterSource, kBase);
        CHECK(!kr.ok && std::strcmp(kr.failed, c.block) == 0); // the shape's own block refuses (its Guard is zero), not only the location's
    }

    // what hangs on the patched seam is flagged weak (usable, WARN), never plain Found
    {
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        img->Bytes()[0x25f610] = 0xe9; // the getter is hooked by somebody else
        const ResolvedBuild rb = Run(*img, reg);
        const SeamResult& p = Get(rb, "jitterSource.producerCall");
        CHECK(Get(rb, "jitterSource.cameraGetter").status == SeamStatus::EntryPatched);
        CHECK(p.status == SeamStatus::FoundSingleAnchor && p.viaWeakDependency && p.rva == 0x37411d);
        CHECK(Get(rb, "jitterSource.producerCallShape").status == SeamStatus::FoundSingleAnchor);
        // the getter anchors: the 8-byte pattern no longer matches (E9 over its first byte), the caller anchor still decodes the target
        CHECK(Get(rb, "jitterSource.cameraGetter").anchors[0].outcome == AnchorOutcome::Zero && Get(rb, "jitterSource.cameraGetter").anchors[1].outcome == AnchorOutcome::Unique);
    }
    {
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        img->Bytes()[0x25f690] = 0xe9; // the camera update is hooked by somebody else
        const ResolvedBuild rb = Run(*img, reg);
        CHECK(Get(rb, "jitterSource.cameraHead").status == SeamStatus::EntryPatched);
        for (const char* id : { "jitterSource.phases", "jitterSource.strength", "jitterSource.cameraGetter" })
        {
            const SeamResult& s = Get(rb, id);
            CHECK(s.status == SeamStatus::FoundSingleAnchor && s.viaWeakDependency);
        }
    }
    {
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        img->Bytes()[0x2ebd90] = 0xe9; // the generator is hooked by somebody else
        const ResolvedBuild rb = Run(*img, reg);
        const SeamResult& st = Get(rb, "jitterSource.strength");
        CHECK(Get(rb, "jitterSource.generator").status == SeamStatus::EntryPatched);
        CHECK(st.status == SeamStatus::FoundSingleAnchor && st.viaWeakDependency && st.rva == 0x25f799);
    }
}

// ---- mustDiffer ----------------------------------------------------------------------------------------------------------------

void TestDetouredFunctionsMustDiffer()
{
    // a registry in which the getter is found by the generator's anchors: both land on 0x2ebd90, the later member of the group is the one in doubt
    Registry reg = JitterRegistry();
    SeamSpec* getter = FindSeam(reg, "jitterSource.cameraGetter");
    const SeamSpec* generator = FindSeam(reg, "jitterSource.generator");
    REQUIRE(getter != nullptr && generator != nullptr);
    getter->anchors = generator->anchors;
    auto img = MakeJitterImage(0x11111111);
    REQUIRE(img != nullptr);
    const ResolvedBuild rb = Run(*img, reg);
    CHECK(Get(rb, "jitterSource.generator").status == SeamStatus::Found && Get(rb, "jitterSource.generator").rva == 0x2ebd90);
    const SeamResult& g = Get(rb, "jitterSource.cameraGetter");
    CHECK(g.status == SeamStatus::Conflict && g.rva == 0 && Contains(g.reason, "must differ"));
    CHECK(rb.build.jitterSource.cameraGetterRva == 0);

    // the real registry resolves them to two different addresses
    const ResolvedBuild ok = Run(*img, JitterRegistry());
    CHECK(Get(ok, "jitterSource.generator").rva != Get(ok, "jitterSource.cameraGetter").rva);
}
} // namespace

int main()
{
    TestGetterPosition();
    TestGetterWindow();
    TestPhasesSameFunction();
    TestStrengthCallsAndFunction();
    TestProducerCallsGetter();
    TestDeviceSizeCopies();
    TestGlobalsMustBeBss();
    TestOffsetRanges();
    TestForeignHookOverAnEntry();
    TestDetouredFunctionsMustDiffer();
    return TEST_RESULT("resolve_jitter_decoys");
}
