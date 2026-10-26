// The resolver core on synthetic images: single anchors, every decode mode, every predicate (with decoys that only the predicate removes),
// the multi-anchor policy truth table, expected-hit pairs, guard anchors derived from another seam, dependencies, EntryPatched, and the
// registry validation. Everything here is committed bytes: no executable is read.
#include "resolve_test_util.h"

#include "ext/ffxiv/resolve/Resolver.h"

#include <cstdio>

using namespace rt;

namespace
{
const char* const kAccessor = "48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 C8 02 00 00 C3";

Registry Reg(std::vector<SeamSpec> seams)
{
    Registry r;
    r.seams = std::move(seams);
    return r;
}

const SeamResult& Get(const ResolvedBuild& rb, const char* id)
{
    static const SeamResult missing;
    const SeamResult* s = rb.Find(id);
    if (s == nullptr)
    {
        std::printf("FAIL: no result for seam %s\n", id);
        ++g_failures;
        return missing;
    }
    return *s;
}

std::string AccessorHex(std::int32_t disp)
{
    std::string hex = "488b05";
    const auto* p = reinterpret_cast<const std::uint8_t*>(&disp);
    hex += ToHex(std::vector<std::uint8_t>(p, p + 4));
    hex += "8bca488b84c8c8020000c3";
    return hex;
}

// the accessor code at `at` loading the global at `target`
void PlantAccessor(DenseImage& img, std::uint32_t at, std::uint32_t target)
{
    Plant(img, at, AccessorHex(static_cast<std::int32_t>(static_cast<std::int64_t>(target) - (static_cast<std::int64_t>(at) + 7))));
}

AnchorSpec Plain(const char* pattern, std::int32_t valueOffset = 0)
{
    AnchorSpec a;
    a.name = "a";
    a.pattern = pattern;
    a.valueOffset = valueOffset;
    return a;
}

void TestSingleAnchor()
{
    const Registry reg = Reg({ SeamSpec { "t.func", SeamKind::Function, { Plain(kAccessor) } } });
    auto img = MakeImage();

    auto rb = ResolveImage(*img, reg);
    const SeamResult& none = Get(rb, "t.func");
    CHECK(none.status == SeamStatus::NotFound && !none.hasValue && none.hits == 0 && !none.reason.empty());
    REQUIRE(none.anchors.size() == 1);
    CHECK(none.anchors[0].outcome == AnchorOutcome::Zero && none.anchors[0].patternHits == 0);

    PlantAccessor(*img, 0x2000, 0x9100);
    rb = ResolveImage(*img, reg);
    const SeamResult& one = Get(rb, "t.func");
    CHECK(one.status == SeamStatus::Found && one.hasValue && one.rva == 0x2000 && one.hits == 1 && one.reason.empty());
    CHECK(one.anchors[0].outcome == AnchorOutcome::Unique && one.anchors[0].patternHits == 1 && one.anchors[0].match == 0x2000);
    CHECK(rb.Usable("t.func") && !rb.Usable("t.other") && rb.Find("t.other") == nullptr);

    PlantAccessor(*img, 0x2800, 0x9100);
    rb = ResolveImage(*img, reg);
    const SeamResult& two = Get(rb, "t.func");
    CHECK(two.status == SeamStatus::Ambiguous && !two.hasValue && two.hits == 2 && !two.reason.empty());
    CHECK(two.anchors[0].outcome == AnchorOutcome::Multiple && two.anchors[0].candidateValues.size() == 2);

    // an offset back to the function entry
    const Registry body = Reg({ SeamSpec { "t.body", SeamKind::Function, { Plain("CA FE BA BE 11 22 33 44", -0x20) } } });
    auto img2 = MakeImage();
    Plant(*img2, 0x2120, "cafebabe11223344");
    rb = ResolveImage(*img2, body);
    CHECK(Get(rb, "t.body").status == SeamStatus::Found && Get(rb, "t.body").rva == 0x2100);
}

void TestDecode()
{
    auto img = MakeImage();
    AnchorSpec rip;
    rip.name = "g";
    rip.pattern = kAccessor;
    rip.valueFrom = ValueFrom::Target;
    rip.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };

    PlantAccessor(*img, 0x2000, 0x9100);
    auto rb = ResolveImage(*img, Reg({ SeamSpec { "t.global", SeamKind::Global, { rip } } }));
    CHECK(Get(rb, "t.global").status == SeamStatus::Found && Get(rb, "t.global").rva == 0x9100);

    // the instruction does not start at the match: target = match + at + length + disp
    {
        auto i2 = MakeImage();
        Plant(*i2, 0x2100, "9090488b05");
        const std::int32_t disp = static_cast<std::int32_t>(0x9200 - (0x2100 + 2 + 7));
        i2->Put(0x2105, &disp, 4);
        AnchorSpec a;
        a.name = "g";
        a.pattern = "90 90 48 8B 05";
        a.valueFrom = ValueFrom::Target;
        a.decode = { DecodeKind::RipDisp32, 2, 3, 7, 0 };
        // the pattern ends before the displacement: the decoder still reads it from the image
        rb = ResolveImage(*i2, Reg({ SeamSpec { "t.g2", SeamKind::Global, { a } } }));
        CHECK(Get(rb, "t.g2").status == SeamStatus::Found && Get(rb, "t.g2").rva == 0x9200);
    }

    // rel32 call and jmp
    {
        auto i3 = MakeImage();
        const std::int32_t rel = static_cast<std::int32_t>(0x3000 - (0x2000 + 5));
        Plant(*i3, 0x2000, "e8");
        i3->Put(0x2001, &rel, 4);
        Plant(*i3, 0x2005, "0f57c041bd");
        AnchorSpec a;
        a.name = "call";
        a.pattern = "E8 ?? ?? ?? ?? 0F 57 C0 41 BD";
        a.valueFrom = ValueFrom::Target;
        a.decode = { DecodeKind::Rel32, 0, 0, 0, 0 };
        rb = ResolveImage(*i3, Reg({ SeamSpec { "t.call", SeamKind::Function, { a } } }));
        CHECK(Get(rb, "t.call").status == SeamStatus::Found && Get(rb, "t.call").rva == 0x3000);

        auto i4 = MakeImage();
        const std::int32_t back = static_cast<std::int32_t>(0x2400 - (0x2800 + 5)); // a backward jmp
        Plant(*i4, 0x2800, "e9");
        i4->Put(0x2801, &back, 4);
        Plant(*i4, 0x2805, "cccc");
        a.pattern = "E9 ?? ?? ?? ?? CC CC";
        rb = ResolveImage(*i4, Reg({ SeamSpec { "t.jmp", SeamKind::Function, { a } } }));
        CHECK(Get(rb, "t.jmp").status == SeamStatus::Found && Get(rb, "t.jmp").rva == 0x2400);

        // a byte that is neither E8 nor E9 under a wildcarded opcode is not a call: the candidate is dropped, with a note
        auto i5 = MakeImage();
        Plant(*i5, 0x2000, "9000000000cccc");
        a.pattern = "?? ?? ?? ?? ?? CC CC";
        rb = ResolveImage(*i5, Reg({ SeamSpec { "t.notcall", SeamKind::Function, { a } } }));
        CHECK(Get(rb, "t.notcall").status == SeamStatus::NotFound);
        CHECK(!Get(rb, "t.notcall").anchors.empty() && !Get(rb, "t.notcall").anchors[0].note.empty());
    }

    // raw fields: a one byte immediate, a four byte displacement, a two byte value
    {
        auto i6 = MakeImage();
        Plant(*i6, 0x2000, "41807854020000");
        Plant(*i6, 0x2100, "488b84c8c8020000c3");
        AnchorSpec a;
        a.name = "f";
        a.valueFrom = ValueFrom::Target;
        a.pattern = "41 80 78 ?? 02";
        a.decode = { DecodeKind::Field, 3, 0, 0, 1 };
        rb = ResolveImage(*i6, Reg({ SeamSpec { "t.f1", SeamKind::Offset, { a } } }));
        CHECK(Get(rb, "t.f1").status == SeamStatus::Found && Get(rb, "t.f1").rva == 0x54);
        a.pattern = "48 8B 84 C8 ?? ?? ?? ?? C3";
        a.decode = { DecodeKind::Field, 4, 0, 0, 4 };
        rb = ResolveImage(*i6, Reg({ SeamSpec { "t.f4", SeamKind::Offset, { a } } }));
        CHECK(Get(rb, "t.f4").status == SeamStatus::Found && Get(rb, "t.f4").rva == 0x2c8);
        a.decode = { DecodeKind::Field, 4, 0, 0, 2 };
        rb = ResolveImage(*i6, Reg({ SeamSpec { "t.f2", SeamKind::Offset, { a } } }));
        CHECK(Get(rb, "t.f2").status == SeamStatus::Found && Get(rb, "t.f2").rva == 0x2c8);
    }

    // RipDeref32: the constant stored at the target (a float's bits in .rdata)
    {
        auto i7 = MakeImage();
        PlantAccessor(*i7, 0x2000, 0x6100);
        Plant(*i7, 0x6100, "6f12833a");
        AnchorSpec a = rip;
        a.decode.kind = DecodeKind::RipDeref32;
        rb = ResolveImage(*i7, Reg({ SeamSpec { "t.floor", SeamKind::Offset, { a } } }));
        CHECK(Get(rb, "t.floor").status == SeamStatus::Found && Get(rb, "t.floor").rva == 0x3a83126f);
        // a target in the unmapped gap cannot be read: dropped
        auto i8 = MakeImage();
        PlantAccessor(*i8, 0x2000, 0x5800);
        rb = ResolveImage(*i8, Reg({ SeamSpec { "t.floor", SeamKind::Offset, { a } } }));
        CHECK(Get(rb, "t.floor").status == SeamStatus::NotFound);
    }

    // a RIP target outside the image or below zero is dropped, never reported
    {
        auto i9 = MakeImage();
        PlantAccessor(*i9, 0x2000, 0x7fffff00);
        rb = ResolveImage(*i9, Reg({ SeamSpec { "t.global", SeamKind::Global, { rip } } }));
        CHECK(Get(rb, "t.global").status == SeamStatus::NotFound && !Get(rb, "t.global").anchors[0].note.empty());
        auto i10 = MakeImage();
        Plant(*i10, 0x2000, AccessorHex(-0x100000));
        rb = ResolveImage(*i10, Reg({ SeamSpec { "t.global", SeamKind::Global, { rip } } }));
        CHECK(Get(rb, "t.global").status == SeamStatus::NotFound);
    }
}

// the five regions: one decoy accessor per region, one predicate per region selects exactly its own
void TestTargetRegions()
{
    auto img = MakeImage();
    PlantAccessor(*img, 0x2000, 0x2800);  // code
    PlantAccessor(*img, 0x2100, 0x6100);  // read-only data
    PlantAccessor(*img, 0x2200, 0x8100);  // initialized data
    PlantAccessor(*img, 0x2300, 0x9100);  // bss
    PlantAccessor(*img, 0x2400, 0xB100);  // .pdata: inside the image, in no searched class

    struct Case
    {
        RegionKind region;
        std::uint32_t expectMatch; // 0 = several
        std::uint32_t hits;
    };
    const Case cases[] = { { RegionKind::Code, 0x2000, 1 }, { RegionKind::ReadOnlyData, 0x2100, 1 }, { RegionKind::InitData, 0x2200, 1 },
                           { RegionKind::Bss, 0x2300, 1 },  { RegionKind::Image, 0, 5 } };
    for (const Case& c : cases)
    {
        AnchorSpec a = Plain(kAccessor);
        a.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
        a.predicates = { pred::TargetIn(c.region) };
        const auto rb = ResolveImage(*img, Reg({ SeamSpec { "t.acc", SeamKind::Function, { a } } }));
        const SeamResult& r = Get(rb, "t.acc");
        if (c.hits == 1)
            CHECK(r.status == SeamStatus::Found && r.rva == c.expectMatch);
        else
            CHECK(r.status == SeamStatus::Ambiguous && r.hits == c.hits);
        CHECK(!r.anchors.empty() && r.anchors[0].patternHits == 5);
    }

    // The advisor's case: unique decoy-free on the real exe, so the predicate must be proven on a decoy. Two accessors, the real one
    // loads a bss global, the decoy loads a .rdata constant: with the predicate Found, without it Ambiguous.
    auto img2 = MakeImage();
    PlantAccessor(*img2, 0x2000, 0x9100);
    PlantAccessor(*img2, 0x2800, 0x6100);
    AnchorSpec with = Plain(kAccessor);
    with.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
    with.predicates = { pred::TargetIn(RegionKind::Bss) };
    AnchorSpec without = with;
    without.predicates.clear();
    auto rb = ResolveImage(*img2, Reg({ SeamSpec { "t.acc", SeamKind::Function, { with } } }));
    CHECK(Get(rb, "t.acc").status == SeamStatus::Found && Get(rb, "t.acc").rva == 0x2000);
    CHECK(Get(rb, "t.acc").anchors[0].patternHits == 2 && Get(rb, "t.acc").anchors[0].candidates == 1);
    rb = ResolveImage(*img2, Reg({ SeamSpec { "t.acc", SeamKind::Function, { without } } }));
    CHECK(Get(rb, "t.acc").status == SeamStatus::Ambiguous && Get(rb, "t.acc").hits == 2);

    // a predicate that needs a decode, on an anchor without one, never passes (and ValidateRegistry says so)
    AnchorSpec nodecode = Plain(kAccessor);
    nodecode.predicates = { pred::TargetIn(RegionKind::Bss) };
    const Registry bad = Reg({ SeamSpec { "t.acc", SeamKind::Function, { nodecode } } });
    CHECK(!ValidateRegistry(bad).empty());
    CHECK(Get(ResolveImage(*img2, bad), "t.acc").status == SeamStatus::NotFound);
}

SeamSpec Marker(const char* id, const char* pattern)
{
    return SeamSpec { id, SeamKind::Function, { Plain(pattern) } };
}

void TestSeamRelationPredicates()
{
    // TargetIs: two accessors load different globals, the predicate keeps the one that loads the seam's value. The dependent seam is
    // listed BEFORE its dependency: the order of the registry does not matter.
    {
        auto img = MakeImage();
        Plant(*img, 0x2400, "deadbeef");
        PlantAccessor(*img, 0x2000, 0x2400);
        PlantAccessor(*img, 0x2800, 0x9100);
        AnchorSpec a = Plain(kAccessor);
        a.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
        a.predicates = { pred::TargetIs("t.known") };
        const auto rb = ResolveImage(*img, Reg({ SeamSpec { "t.acc", SeamKind::Function, { a } }, Marker("t.known", "DE AD BE EF") }));
        CHECK(Get(rb, "t.known").status == SeamStatus::Found && Get(rb, "t.known").rva == 0x2400);
        CHECK(Get(rb, "t.acc").status == SeamStatus::Found && Get(rb, "t.acc").rva == 0x2000);
    }

    // CallsSeam: two calls to different targets; and a jmp to the right target is not a call
    {
        auto img = MakeImage();
        Plant(*img, 0x2400, "deadbeef");
        auto call = [&](std::uint32_t at, std::uint32_t target, const char* op)
        {
            const std::int32_t rel = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - (at + 5));
            Plant(*img, at, op);
            img->Put(at + 1, &rel, 4);
            Plant(*img, at + 5, "90909090");
        };
        call(0x2000, 0x2500, "e8");
        call(0x2100, 0x2400, "e9"); // a jump to the seam: not a call
        call(0x2200, 0x2400, "e8");
        AnchorSpec a = Plain("E? ?? ?? ?? ?? 90 90 90 90");
        a.decode = { DecodeKind::Rel32, 0, 0, 0, 0 };
        a.predicates = { pred::CallsSeam("t.known") };
        auto rb = ResolveImage(*img, Reg({ Marker("t.known", "DE AD BE EF"), SeamSpec { "t.site", SeamKind::Function, { a } } }));
        CHECK(Get(rb, "t.site").status == SeamStatus::Found && Get(rb, "t.site").rva == 0x2200);
        a.predicates = { pred::TargetIs("t.known") }; // a jmp counts as "target equals" but not as "calls"
        rb = ResolveImage(*img, Reg({ Marker("t.known", "DE AD BE EF"), SeamSpec { "t.site", SeamKind::Function, { a } } }));
        CHECK(Get(rb, "t.site").status == SeamStatus::Ambiguous && Get(rb, "t.site").hits == 2);
    }

    // Within / After / Before: inclusive bounds, exact edges
    {
        auto img = MakeImage();
        Plant(*img, 0x2400, "deadbeef");
        Plant(*img, 0x2000, "cafebabe"); // -0x400
        Plant(*img, 0x2410, "cafebabe"); // +0x10
        Plant(*img, 0x2500, "cafebabe"); // +0x100
        auto run = [&](Predicate p)
        {
            AnchorSpec a = Plain("CA FE BA BE");
            a.predicates = { p };
            return ResolveImage(*img, Reg({ Marker("t.known", "DE AD BE EF"), SeamSpec { "t.x", SeamKind::Function, { a } } }));
        };
        auto rb = run(pred::After("t.known", 0x10));
        CHECK(Get(rb, "t.x").status == SeamStatus::Found && Get(rb, "t.x").rva == 0x2410); // exactly at the bound
        CHECK(Get(run(pred::After("t.known", 0x0f)), "t.x").status == SeamStatus::NotFound);  // one byte short
        rb = run(pred::Before("t.known", 0x400));
        CHECK(Get(rb, "t.x").status == SeamStatus::Found && Get(rb, "t.x").rva == 0x2000);
        CHECK(Get(run(pred::Before("t.known", 0x3ff)), "t.x").status == SeamStatus::NotFound);
        CHECK(Get(run(pred::Within("t.known", -0x400, 0x100)), "t.x").status == SeamStatus::Ambiguous);
        CHECK(Get(run(pred::Within("t.known", -0x400, 0x100)), "t.x").hits == 3);
        CHECK(Get(run(pred::Within("t.known", 0x11, 0xff)), "t.x").status == SeamStatus::NotFound);
        CHECK(Get(run(pred::Within("t.known", 0x100, 0x100)), "t.x").rva == 0x2500); // lo == hi
    }
}

void TestFunctionPredicates()
{
    // F = 0x2000..0x2100 plain, 0x2100..0x2180 chained to F, G = 0x2200..0x2300 plain, nothing at 0x2400 (a leaf function)
    auto img = MakeImage(1, 3 * 12);
    const std::uint32_t uF = kRdataRva, uChain = kRdataRva + 0x40, uG = kRdataRva + 0x80;
    PutUnwind(*img, uF, 0, 1);
    PutUnwind(*img, uChain, 4, 0, 0x2000, 0x2100, uF);
    PutUnwind(*img, uG, 0, 1);
    PutRuntimeFunction(*img, 0, 0x2000, 0x2100, uF);
    PutRuntimeFunction(*img, 1, 0x2100, 0x2180, uChain);
    PutRuntimeFunction(*img, 2, 0x2200, 0x2300, uG);
    Plant(*img, 0x2000, "abcdef01");
    Plant(*img, 0x2050, "cafebabe");
    Plant(*img, 0x2120, "cafebabe"); // the chained fragment of F
    Plant(*img, 0x2250, "cafebabe"); // G
    Plant(*img, 0x2450, "cafebabe"); // a leaf: no entry

    auto run = [&](std::vector<Predicate> preds, ValueFrom from = ValueFrom::Match)
    {
        AnchorSpec a = Plain("CA FE BA BE");
        a.predicates = std::move(preds);
        a.valueFrom = from;
        return ResolveImage(*img, Reg({ Marker("t.f", "AB CD EF 01"), SeamSpec { "t.x", SeamKind::Function, { a } } }));
    };
    auto rb = run({ pred::SameFunctionAs("t.f") });
    CHECK(Get(rb, "t.x").status == SeamStatus::Ambiguous && Get(rb, "t.x").hits == 2); // 0x2050 and the fragment 0x2120
    rb = run({ pred::SameFunctionAs("t.f"), pred::After("t.f", 0x80) });
    CHECK(Get(rb, "t.x").status == SeamStatus::Found && Get(rb, "t.x").rva == 0x2050);
    rb = run({ pred::SameFunctionAs("t.f"), pred::Within("t.f", 0x100, 0x1ff) });
    CHECK(Get(rb, "t.x").status == SeamStatus::Found && Get(rb, "t.x").rva == 0x2120); // the fragment counts as the same function

    // the enclosing function as the VALUE; the leaf has none: dropped, never guessed
    rb = run({ pred::Within("t.f", 0x100, 0x1ff) }, ValueFrom::EnclosingFunction);
    CHECK(Get(rb, "t.x").status == SeamStatus::Found && Get(rb, "t.x").rva == 0x2000);
    rb = run({ pred::Within("t.f", 0x440, 0x460) }, ValueFrom::EnclosingFunction);
    CHECK(Get(rb, "t.x").status == SeamStatus::NotFound && !Get(rb, "t.x").anchors[0].note.empty());
    rb = run({ pred::Within("t.f", 0x200, 0x2ff) }, ValueFrom::EnclosingFunction);
    CHECK(Get(rb, "t.x").status == SeamStatus::Found && Get(rb, "t.x").rva == 0x2200);

    // SameFunctionAs when the seam itself is in a leaf (no entry): no candidate qualifies
    Plant(*img, 0x2440, "11223344");
    AnchorSpec a = Plain("CA FE BA BE");
    a.predicates = { pred::SameFunctionAs("t.leaf") };
    rb = ResolveImage(*img, Reg({ Marker("t.leaf", "11 22 33 44"), SeamSpec { "t.x", SeamKind::Function, { a } } }));
    CHECK(Get(rb, "t.x").status == SeamStatus::NotFound);
}

// the multi-anchor policy
void TestMultiAnchorPolicy()
{
    // A1 finds "11 22 33 44" (value = match), A2 finds "55 66 77 88" (value = match - 0x10): they agree when A2 sits 0x10 after A1.
    auto seam = [](bool third = false)
    {
        SeamSpec s { "t.m", SeamKind::Function, { Plain("11 22 33 44", 0), Plain("55 66 77 88", -0x10) } };
        if (third)
            s.anchors.push_back(Plain("99 AA BB CC", -0x20));
        return s;
    };
    auto image = [](std::initializer_list<std::pair<std::uint32_t, const char*>> plants)
    {
        auto img = MakeImage();
        for (const auto& p : plants)
            Plant(*img, p.first, p.second);
        return img;
    };
    constexpr const char* a1 = "11223344";
    constexpr const char* a2 = "55667788";
    constexpr const char* a3 = "99aabbcc";
    auto run = [&](const std::unique_ptr<DenseImage>& img, bool third = false) { return ResolveImage(*img, Reg({ seam(third) })); };

    // U + U agree
    auto rb = run(image({ { 0x2000, a1 }, { 0x2010, a2 } }));
    CHECK(Get(rb, "t.m").status == SeamStatus::Found && Get(rb, "t.m").rva == 0x2000 && Get(rb, "t.m").hits == 2 && Get(rb, "t.m").reason.empty());
    // U + U disagree: inert, no value
    rb = run(image({ { 0x2000, a1 }, { 0x2210, a2 } }));
    CHECK(Get(rb, "t.m").status == SeamStatus::Conflict && !Get(rb, "t.m").hasValue && Get(rb, "t.m").rva == 0 && !Get(rb, "t.m").reason.empty());
    CHECK(!rb.Usable("t.m"));
    // U + 0, both ways round: usable, single anchor
    rb = run(image({ { 0x2000, a1 } }));
    CHECK(Get(rb, "t.m").status == SeamStatus::FoundSingleAnchor && Get(rb, "t.m").rva == 0x2000 && Get(rb, "t.m").hasValue && rb.Usable("t.m"));
    rb = run(image({ { 0x2010, a2 } }));
    CHECK(Get(rb, "t.m").status == SeamStatus::FoundSingleAnchor && Get(rb, "t.m").rva == 0x2000);
    // 0 + 0
    rb = run(image({}));
    CHECK(Get(rb, "t.m").status == SeamStatus::NotFound && !Get(rb, "t.m").hasValue);
    // U + A that contains the unique value / does not contain it
    rb = run(image({ { 0x2000, a1 }, { 0x2010, a2 }, { 0x2300, a2 } }));
    CHECK(Get(rb, "t.m").status == SeamStatus::FoundSingleAnchor && Get(rb, "t.m").rva == 0x2000);
    rb = run(image({ { 0x2000, a1 }, { 0x2210, a2 }, { 0x2300, a2 } }));
    CHECK(Get(rb, "t.m").status == SeamStatus::Conflict && !Get(rb, "t.m").hasValue);
    // A + 0, 0 + A, A + A
    rb = run(image({ { 0x2000, a1 }, { 0x2100, a1 } }));
    CHECK(Get(rb, "t.m").status == SeamStatus::Ambiguous && Get(rb, "t.m").hits == 2);
    rb = run(image({ { 0x2010, a2 }, { 0x2110, a2 } }));
    CHECK(Get(rb, "t.m").status == SeamStatus::Ambiguous);
    rb = run(image({ { 0x2000, a1 }, { 0x2100, a1 }, { 0x2010, a2 }, { 0x2110, a2 }, { 0x2200, a2 } }));
    CHECK(Get(rb, "t.m").status == SeamStatus::Ambiguous && Get(rb, "t.m").hits == 5);

    // three anchors: two agree and one finds nothing, two agree and one disagrees, one alone
    rb = run(image({ { 0x2000, a1 }, { 0x2010, a2 } }), true);
    CHECK(Get(rb, "t.m").status == SeamStatus::Found && Get(rb, "t.m").rva == 0x2000);
    rb = run(image({ { 0x2000, a1 }, { 0x2010, a2 }, { 0x2420, a3 } }), true);
    CHECK(Get(rb, "t.m").status == SeamStatus::Conflict);
    rb = run(image({ { 0x2000, a1 }, { 0x2010, a2 }, { 0x2020, a3 } }), true);
    CHECK(Get(rb, "t.m").status == SeamStatus::Found && Get(rb, "t.m").hits == 3);
    rb = run(image({ { 0x2000, a1 } }), true);
    CHECK(Get(rb, "t.m").status == SeamStatus::FoundSingleAnchor);
    rb = run(image({ { 0x2020, a3 } }), true);
    CHECK(Get(rb, "t.m").status == SeamStatus::FoundSingleAnchor && Get(rb, "t.m").rva == 0x2000);
}

void TestExpectedHitsAndGuards()
{
    // one pattern marks two sites (the batch16 / batch32 pair): the address order picks which is which
    auto run = [](const DenseImage& img)
    {
        AnchorSpec lo = Plain("BA BA BA BA 77");
        lo.expectedHits = 2;
        lo.pickIndex = 0;
        AnchorSpec hi = lo;
        hi.pickIndex = 1;
        return ResolveImage(img, Reg({ SeamSpec { "t.lo", SeamKind::Function, { lo } }, SeamSpec { "t.hi", SeamKind::Function, { hi } } }));
    };
    auto img = MakeImage();
    Plant(*img, 0x2000, "babababa77");
    Plant(*img, 0x2400, "babababa77");
    auto rb = run(*img);
    CHECK(Get(rb, "t.lo").status == SeamStatus::Found && Get(rb, "t.lo").rva == 0x2000);
    CHECK(Get(rb, "t.hi").status == SeamStatus::Found && Get(rb, "t.hi").rva == 0x2400);
    CHECK(rb.scanRequests == 1); // the pair shares one pattern: it is scanned once
    Plant(*img, 0x2800, "babababa77"); // a third site: the pair is no longer a pair
    rb = run(*img);
    CHECK(Get(rb, "t.lo").status == SeamStatus::Ambiguous && Get(rb, "t.hi").status == SeamStatus::Ambiguous && Get(rb, "t.lo").hits == 3);
    auto one = MakeImage();
    Plant(*one, 0x2000, "babababa77");
    rb = run(*one);
    CHECK(Get(rb, "t.lo").status == SeamStatus::Ambiguous && Get(rb, "t.hi").status == SeamStatus::Ambiguous); // fewer than expected

    // a guard anchor: not scanned, verified at value(fromSeam) + fromOffset, then decoded like any anchor
    auto g = MakeImage();
    PlantAccessor(*g, 0x2000, 0x9100);
    PlantAccessor(*g, 0x2800, 0x6100); // a decoy accessor: the guard must not scan for it
    AnchorSpec fn = Plain("48 8B 05 ?? ?? ?? ?? 8B CA");
    fn.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
    fn.predicates = { pred::TargetIn(RegionKind::Bss) };
    AnchorSpec derived;
    derived.name = "guard";
    derived.pattern = "48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 C8 02";
    derived.fromSeam = "t.fn";
    derived.valueFrom = ValueFrom::Target;
    derived.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
    derived.predicates = { pred::TargetIn(RegionKind::Bss) };
    rb = ResolveImage(*g, Reg({ SeamSpec { "t.global", SeamKind::Global, { derived } }, SeamSpec { "t.fn", SeamKind::Function, { fn } } }));
    CHECK(Get(rb, "t.fn").status == SeamStatus::Found && Get(rb, "t.fn").rva == 0x2000);
    CHECK(Get(rb, "t.global").status == SeamStatus::Found && Get(rb, "t.global").rva == 0x9100);
    CHECK(Get(rb, "t.global").anchors[0].patternHits == 1);
    CHECK(rb.scanRequests == 1); // the guard anchor is verified, not scanned
    // guard bytes that do not match: NotFound
    derived.pattern = "48 8B 05 ?? ?? ?? ?? 8B CB";
    rb = ResolveImage(*g, Reg({ SeamSpec { "t.global", SeamKind::Global, { derived } }, SeamSpec { "t.fn", SeamKind::Function, { fn } } }));
    CHECK(Get(rb, "t.global").status == SeamStatus::NotFound && Get(rb, "t.global").anchors[0].patternHits == 0);
    // a guard at an offset
    derived.pattern = "8B CA 48 8B 84 C8 C8 02";
    derived.fromOffset = 7;
    derived.decode = { DecodeKind::Field, 6, 0, 0, 4 };
    derived.predicates.clear(); // a raw field has no address to test
    rb = ResolveImage(*g, Reg({ SeamSpec { "t.global", SeamKind::Offset, { derived } }, SeamSpec { "t.fn", SeamKind::Function, { fn } } }));
    CHECK(Get(rb, "t.global").status == SeamStatus::Found && Get(rb, "t.global").rva == 0x2c8);
    // a guard whose source seam did not resolve
    auto empty = MakeImage();
    rb = ResolveImage(*empty, Reg({ SeamSpec { "t.global", SeamKind::Offset, { derived } }, SeamSpec { "t.fn", SeamKind::Function, { fn } } }));
    CHECK(Get(rb, "t.global").status == SeamStatus::NotFound && Get(rb, "t.global").anchors[0].outcome == AnchorOutcome::DependencyMissing);
    CHECK(Get(rb, "t.global").reason.find("t.fn") != std::string::npos);
}

void TestDependencies()
{
    // a dependency that is Ambiguous / Conflict / NotFound leaves the dependent NotFound, naming it
    auto img = MakeImage();
    Plant(*img, 0x2000, "deadbeef");
    Plant(*img, 0x2100, "deadbeef"); // t.amb: two hits
    Plant(*img, 0x2400, "cafebabe");
    AnchorSpec dep = Plain("CA FE BA BE");
    dep.predicates = { pred::After("t.amb", 0x10) };
    const Registry reg = Reg({ SeamSpec { "t.dep", SeamKind::Function, { dep } }, Marker("t.amb", "DE AD BE EF") });
    auto rb = ResolveImage(*img, reg);
    CHECK(Get(rb, "t.amb").status == SeamStatus::Ambiguous);
    CHECK(Get(rb, "t.dep").status == SeamStatus::NotFound && Get(rb, "t.dep").reason.find("t.amb") != std::string::npos);
    CHECK(Get(rb, "t.dep").anchors[0].outcome == AnchorOutcome::DependencyMissing);

    // a chain a -> b -> c resolves in dependency order whatever the registry order
    auto img2 = MakeImage();
    Plant(*img2, 0x2000, "00000001");
    Plant(*img2, 0x2100, "00000002");
    Plant(*img2, 0x2200, "00000003");
    AnchorSpec b = Plain("00 00 00 02");
    b.predicates = { pred::After("t.c", 0x100) };
    AnchorSpec a = Plain("00 00 00 03");
    a.predicates = { pred::After("t.b", 0x100) };
    const Registry chain = Reg({ SeamSpec { "t.a", SeamKind::Function, { a } }, SeamSpec { "t.b", SeamKind::Function, { b } },
                                 Marker("t.c", "00 00 00 01") });
    rb = ResolveImage(*img2, chain);
    CHECK(Get(rb, "t.c").rva == 0x2000 && Get(rb, "t.b").rva == 0x2100 && Get(rb, "t.a").rva == 0x2200);
    CHECK(rb.Count(SeamStatus::Found) == 3);
    CHECK(rb.seams[0].id == "t.a" && rb.seams[2].id == "t.c"); // results keep the registry order

    // a cycle is a registry error: nothing resolves, every seam is NotFound
    AnchorSpec ca = Plain("00 00 00 01");
    ca.predicates = { pred::After("t.y", 1) };
    AnchorSpec cb = Plain("00 00 00 02");
    cb.predicates = { pred::After("t.x", 1) };
    const Registry cyc = Reg({ SeamSpec { "t.x", SeamKind::Function, { ca } }, SeamSpec { "t.y", SeamKind::Function, { cb } } });
    CHECK(!ValidateRegistry(cyc).empty());
    rb = ResolveImage(*img2, cyc);
    CHECK(!rb.registryErrors.empty() && Get(rb, "t.x").status == SeamStatus::NotFound && Get(rb, "t.y").status == SeamStatus::NotFound);
}

void TestEntryPatched()
{
    auto body = [](SeamKind kind = SeamKind::Function)
    { return Reg({ SeamSpec { "t.fn", kind, { Plain("CA FE BA BE 11 22 33 44", -0x20) } } }); };
    auto img = MakeImage();
    Plant(*img, 0x2120, "cafebabe11223344");

    // a clean entry
    Plant(*img, 0x2100, "48895c2408");
    auto rb = ResolveImage(*img, body());
    CHECK(Get(rb, "t.fn").status == SeamStatus::Found && Get(rb, "t.fn").rva == 0x2100);

    // E9, EB, FF 25 written over the entry: a foreign hook. The RVA is still reported, the status is its own
    const char* hooks[] = { "e901020304", "eb10", "ff2500000000" };
    for (const char* hook : hooks)
    {
        Plant(*img, 0x2100, "909090909090");
        Plant(*img, 0x2100, hook);
        rb = ResolveImage(*img, body());
        const SeamResult& r = Get(rb, "t.fn");
        CHECK(r.status == SeamStatus::EntryPatched && r.rva == 0x2100 && r.hasValue && !rb.Usable("t.fn") && !r.reason.empty());
    }
    // only a Function seam has an entry to check
    rb = ResolveImage(*img, body(SeamKind::Global));
    CHECK(Get(rb, "t.fn").status == SeamStatus::Found);

    // a dependent of a patched function still gets its value (the address is right, only the entry is not ours), but it is not plain
    // Found any more: the value rests on a weak seam, so the weakness propagates (M1)
    AnchorSpec after = Plain("11 22 33 44");
    after.predicates = { pred::After("t.fn", 0x40) };
    Registry two = body();
    two.seams.push_back(SeamSpec { "t.after", SeamKind::Function, { after } });
    rb = ResolveImage(*img, two);
    CHECK(Get(rb, "t.after").status == SeamStatus::FoundSingleAnchor && Get(rb, "t.after").rva == 0x2124 && Get(rb, "t.after").hasValue);
    CHECK(Get(rb, "t.after").viaWeakDependency && Get(rb, "t.after").reason.find("t.fn") != std::string::npos);
    CHECK(rb.Usable("t.after") && Get(rb, "t.after").anchors[0].weakDependency);

    // the relaxed retry: the pattern starts AT the entry and the first bytes were overwritten
    const char* entryPattern = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 41 08 C3 CC"; // 20 bytes, 6 fixed after the first 14
    const char* entryHex = "48895c240848897424105748 83ec208b4108c3cc";
    auto relaxed = [&](bool entryAnchor, std::vector<Predicate> preds = {}, const char* pattern = nullptr)
    {
        AnchorSpec a = Plain(pattern != nullptr ? pattern : entryPattern);
        a.entryAnchor = entryAnchor;
        a.predicates = std::move(preds);
        return Reg({ SeamSpec { "t.entry", SeamKind::Function, { a } } });
    };
    auto fresh = [&](const char* overwrite)
    {
        auto i = MakeImage();
        Plant(*i, 0x2000, entryHex);
        if (overwrite != nullptr)
            Plant(*i, 0x2000, overwrite);
        return i;
    };
    CHECK(Get(ResolveImage(*fresh(nullptr), relaxed(true)), "t.entry").status == SeamStatus::Found); // untouched: the normal path
    auto patched = fresh("e911223344");
    CHECK(Get(ResolveImage(*patched, relaxed(false)), "t.entry").status == SeamStatus::NotFound);   // not marked: no retry
    rb = ResolveImage(*patched, relaxed(true));
    CHECK(Get(rb, "t.entry").status == SeamStatus::EntryPatched && Get(rb, "t.entry").rva == 0x2000 && !rb.Usable("t.entry"));
    CHECK(Get(rb, "t.entry").anchors[0].outcome == AnchorOutcome::Patched);
    rb = ResolveImage(*fresh("ff2500000000aabbccdd1122"), relaxed(true)); // the 14 byte absolute jump
    CHECK(Get(rb, "t.entry").status == SeamStatus::EntryPatched);
    // the longest jump a hook writes is 14 bytes (FF 25 00000000 + an absolute address): all 14 are masked by the retry, the 15th is not
    rb = ResolveImage(*fresh("ff2500000000aabbccddeeff1122"), relaxed(true));
    CHECK(Get(rb, "t.entry").status == SeamStatus::EntryPatched && Get(rb, "t.entry").anchors[0].outcome == AnchorOutcome::Patched);
    CHECK(Get(ResolveImage(*fresh("ff2500000000aabbccddeeff112233"), relaxed(true)), "t.entry").status == SeamStatus::NotFound); // 15 changed bytes
    CHECK(Get(ResolveImage(*fresh("9090909090"), relaxed(true)), "t.entry").status == SeamStatus::NotFound); // changed, but not a jump
    // a predicate still has to pass on the relaxed hit (the marker sits 0x800 before the entry)
    {
        auto marked = fresh("e911223344");
        Plant(*marked, 0x1800, "deadbeef");
        auto withPred = [&](std::int32_t lo, std::int32_t hi)
        {
            AnchorSpec a = Plain(entryPattern);
            a.entryAnchor = true;
            a.predicates = { pred::Within("t.mark", lo, hi) };
            return Reg({ Marker("t.mark", "DE AD BE EF"), SeamSpec { "t.entry", SeamKind::Function, { a } } });
        };
        CHECK(Get(ResolveImage(*marked, withPred(0x800, 0x800)), "t.entry").status == SeamStatus::EntryPatched);
        CHECK(Get(ResolveImage(*marked, withPred(1, 0x10)), "t.entry").status == SeamStatus::NotFound);
    }
    // a second site that matches the relaxed pattern (its tail bytes): not unique, never EntryPatched
    auto two2 = fresh("e911223344");
    Plant(*two2, 0x280e, "208b4108c3cc");
    CHECK(Get(ResolveImage(*two2, relaxed(true)), "t.entry").status == SeamStatus::NotFound);
    // too few fixed bytes would remain after masking the entry: no retry at all
    auto shortPattern = fresh("e911223344");
    CHECK(Get(ResolveImage(*shortPattern, relaxed(true, {}, "48 89 5C 24 08 48 89 74 24 10 57 48")), "t.entry").status == SeamStatus::NotFound);
    // the boundary: 5 fixed bytes remain after the first 14 (no retry), 6 remain (retry, tested above with the 20 byte pattern)
    CHECK(Get(ResolveImage(*shortPattern, relaxed(true, {}, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 41 08 C3")), "t.entry").status == SeamStatus::NotFound);
    CHECK(Get(ResolveImage(*shortPattern, relaxed(true)), "t.entry").status == SeamStatus::EntryPatched); // 6 fixed: the retry runs
}

void TestValidation()
{
    auto ok = []() { return Reg({ SeamSpec { "t.a", SeamKind::Function, { Plain("11 22 33 44") } } }); };
    CHECK(ValidateRegistry(ok()).empty());
    auto errors = [](Registry r) { return ValidateRegistry(r).size(); };

    Registry r = ok();
    r.seams.push_back(r.seams[0]);
    CHECK(errors(r) >= 1); // duplicate id
    r = ok();
    r.seams[0].id = "";
    CHECK(errors(r) >= 1);
    r = ok();
    r.seams[0].anchors.clear();
    CHECK(errors(r) >= 1); // no anchor
    r = ok();
    r.seams[0].anchors[0].pattern = "11 2";
    CHECK(errors(r) >= 1); // bad pattern
    r = ok();
    r.seams[0].anchors[0].pattern = "?? ?? ??";
    CHECK(errors(r) >= 1);
    r = ok();
    r.seams[0].anchors[0].expectedHits = 0;
    CHECK(errors(r) >= 1);
    r = ok();
    r.seams[0].anchors[0].pickIndex = 1;
    CHECK(errors(r) >= 1); // pick beyond expectedHits
    r = ok();
    r.seams[0].anchors[0].valueFrom = ValueFrom::Target;
    CHECK(errors(r) >= 1); // Target without a decode
    r = ok();
    r.seams[0].anchors[0].valueFrom = ValueFrom::Target;
    r.seams[0].anchors[0].decode = { DecodeKind::Field, 0, 0, 0, 3 };
    CHECK(errors(r) >= 1); // field width 3
    r.seams[0].anchors[0].decode = { DecodeKind::Field, 2, 0, 0, 4 };
    CHECK(errors(r) == 0); // a decode may read bytes after the pattern: they come from the image
    r.seams[0].anchors[0].decode = { DecodeKind::Field, 0, 0, 0, 4 };
    CHECK(errors(r) == 0);
    r = ok();
    r.seams[0].anchors[0].decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
    r.seams[0].anchors[0].pattern = "48 8B 05 ?? ?? ?? ??";
    CHECK(errors(r) == 0);
    r.seams[0].anchors[0].decode = { DecodeKind::RipDisp32, 0, 5, 7, 0 };
    CHECK(errors(r) >= 1); // disp32 would end after the instruction
    r.seams[0].anchors[0].decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
    r.seams[0].anchors[0].valueOffset = 4;
    r.seams[0].anchors[0].valueFrom = ValueFrom::Target;
    CHECK(errors(r) >= 1); // a value offset with a decoded value
    r = ok();
    r.seams[0].anchors[0].predicates = { pred::CallsSeam("t.a") };
    CHECK(errors(r) >= 1); // CallsSeam needs Rel32 (and depends on itself)
    r = ok();
    r.seams[0].anchors[0].predicates = { pred::After("t.missing", 4) };
    CHECK(errors(r) >= 1); // unknown dependency
    r = ok();
    r.seams[0].anchors[0].fromSeam = "t.missing";
    CHECK(errors(r) >= 1);
    r = ok();
    r.seams[0].anchors[0].fromSeam = "t.a";
    CHECK(errors(r) >= 1); // itself
    r = ok();
    r.seams[0].anchors[0].fromSeam = "t.a";
    r.seams[0].anchors[0].entryAnchor = true;
    CHECK(errors(r) >= 1);
    r = ok();
    r.seams[0].anchors[0].predicates = { pred::Within("t.a", 5, 1) };
    CHECK(errors(r) >= 1); // lo > hi (and self)
    r = ok();
    r.bindings.push_back({ "charaView.accessorRva", "t.a", 0 });
    CHECK(errors(r) == 0);
    r.bindings.push_back({ "charaView.accessorRva", "t.a", 0 });
    CHECK(errors(r) >= 1); // two bindings for one field
    r = ok();
    r.bindings.push_back({ "nope.nothing", "t.a", 0 });
    CHECK(errors(r) >= 1); // unknown field
    r = ok();
    r.bindings.push_back({ "charaView.accessorRva", "t.missing", 0 });
    CHECK(errors(r) >= 1); // unknown seam
    r = ok();
    r.bindings.push_back({ "charaView.targetsOffset", "t.a", 0 }); // an Offset field needs an Offset seam
    CHECK(errors(r) >= 1);
    r = ok();
    r.seams[0].kind = SeamKind::Offset;
    r.bindings.push_back({ "charaView.accessorRva", "t.a", 0 }); // an address field needs an address seam
    CHECK(errors(r) >= 1);
    r.bindings[0].key = "charaView.targetsOffset";
    CHECK(errors(r) == 0);
    r = ok();
    r.bindings.push_back({ "charaView.entryCount", "t.a", 0 }); // a Constant field is never bound
    CHECK(errors(r) >= 1);
    r = ok();
    r.bindings.push_back({ "charaView.accessorRva", "t.a", 4 }); // a guard offset on a non-guard field
    CHECK(errors(r) >= 1);

    // entryAnchor rules: Function seams only, the pattern starts at the entry, one hit
    r = ok();
    r.seams[0].anchors[0].entryAnchor = true;
    CHECK(errors(r) == 0);
    r.seams[0].kind = SeamKind::Global;
    CHECK(errors(r) >= 1);
    r = ok();
    r.seams[0].anchors[0].entryAnchor = true;
    r.seams[0].anchors[0].valueOffset = -4;
    CHECK(errors(r) >= 1);
    r = ok();
    r.seams[0].anchors[0].entryAnchor = true;
    r.seams[0].anchors[0].expectedHits = 2;
    CHECK(errors(r) >= 1);

    // minMargin 0 waives the offline margin gate: only where the predicates are the disambiguator (or the anchor is a guard, never scanned)
    r = ok();
    r.seams[0].anchors[0].minMargin = 0;
    CHECK(errors(r) >= 1); // nothing disambiguates
    r.seams[0].anchors[0].predicates = { pred::TargetIn(RegionKind::Image) };
    r.seams[0].anchors[0].decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
    r.seams[0].anchors[0].pattern = "48 8B 05 ?? ?? ?? ??";
    CHECK(errors(r) >= 1); // TargetIn(Image) is a no-op
    r.seams[0].anchors[0].predicates = { pred::TargetIn(RegionKind::Bss) };
    CHECK(errors(r) == 0);
    r = ok();
    r.seams[0].anchors[0].minMargin = 0;
    r.seams.push_back(SeamSpec { "t.b", SeamKind::Function, { Plain("55 66 77 88") } });
    r.seams[0].anchors[0].fromSeam = "t.b";
    CHECK(errors(r) == 0); // a guard anchor is not scanned: no margin to waive
    r.seams[0].anchors[0].fromSeam.clear();
    r.seams[0].anchors[0].predicates = { pred::After("t.b", 0x40) };
    CHECK(errors(r) == 0);

    // a broken registry resolves to nothing, with the errors recorded
    r = ok();
    r.seams[0].anchors[0].pattern = "11 2";
    auto img = MakeImage();
    Plant(*img, 0x2000, "11223344");
    const auto rb = ResolveImage(*img, r);
    CHECK(!rb.registryErrors.empty() && rb.Count(SeamStatus::NotFound) == 1);
}
void TestOverflow()
{
    // a pattern that matches at nearly every byte of .text: the raw list stops at the cap, the anchor is Multiple and never Unique, and
    // the note says why (the hits beyond the cap were not looked at)
    auto img = MakeImage();
    const auto rb = ResolveImage(*img, Reg({ SeamSpec { "t.many", SeamKind::Function, { Plain("9? ?? ??") } } }));
    const SeamResult& r = Get(rb, "t.many");
    REQUIRE(r.anchors.size() == 1);
    CHECK(r.status == SeamStatus::Ambiguous && r.anchors[0].overflow && r.anchors[0].patternHits == kMaxScanHits);
    CHECK(r.anchors[0].note.find("stopped at") != std::string::npos);
    CHECK(r.anchors[0].candidateValues.size() == 64); // the report keeps the first 64 only
}

void TestUnreadableScan()
{
    // a code section that cannot be read: every scanned anchor is NotFound with the reason, never "not found because absent"; a guard
    // anchor (not scanned) is unaffected by the failed scan, but it depends on a seam that did not resolve
    auto img = MakeImage();
    Plant(*img, 0x2000, "11223344");
    struct Failing : ImageView
    {
        const DenseImage& inner;
        explicit Failing(const DenseImage& i) : inner(i) { _info = i.Info(); }
        bool Read(std::uint32_t rva, void* out, std::size_t n) const override { return rva + n > 0x3000 ? false : inner.Read(rva, out, n); } // the code section read runs past 0x3000
    } failing(*img);
    const Registry reg = Reg({ SeamSpec { "t.a", SeamKind::Function, { Plain("11 22 33 44") } } });
    const auto rb = ResolveImage(failing, reg);
    const SeamResult& r = Get(rb, "t.a");
    CHECK(r.status == SeamStatus::NotFound && !r.hasValue);
    REQUIRE(r.anchors.size() == 1);
    CHECK(r.anchors[0].note.find("could not be scanned") != std::string::npos && r.reason.find("could not be scanned") != std::string::npos);
}

void TestInvalidImage()
{
    // an image whose headers did not validate resolves nothing and says so; it never throws and never scans
    struct Invalid : ImageView
    {
        Invalid() { _info.valid = false; }
        bool Read(std::uint32_t, void*, std::size_t) const override { return false; }
    } invalid;
    const Registry reg = Reg({ SeamSpec { "t.a", SeamKind::Function, { Plain("11 22 33 44") } } });
    const auto rb = ResolveImage(invalid, reg);
    CHECK(rb.seams.size() == 1 && rb.seams[0].status == SeamStatus::NotFound && !rb.registryErrors.empty() && !rb.seams[0].reason.empty());
}

// M8: the registry owns its text. Specs built from temporaries or from buffers that are overwritten afterwards stay correct.
Registry BuildDynamicRegistry(int n)
{
    Registry r;
    for (int i = 0; i < n; ++i)
    {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%02X", 0x40 + i);
        const std::string pattern = std::string("11 22 33 ") + hex; // every temporary dies when this function returns
        AnchorSpec a;
        a.name = "anchor" + std::to_string(i);
        a.pattern = pattern;
        r.seams.push_back({ "t.dyn" + std::to_string(i), SeamKind::Function, { a } });
        r.bindings.push_back({ i == 0 ? "charaView.accessorRva" : "charaView.managerPointerRva", "t.dyn" + std::to_string(i), 0 });
    }
    r.bindings.resize(1);
    return r;
}

void TestOwnedStrings()
{
    const Registry r = BuildDynamicRegistry(3);
    auto img = MakeImage();
    Plant(*img, 0x2000, "11223340");
    Plant(*img, 0x2100, "11223341");
    Plant(*img, 0x2200, "11223342");
    const auto rb = ResolveImage(*img, r);
    CHECK(ValidateRegistry(r).empty());
    CHECK(Get(rb, "t.dyn0").rva == 0x2000 && Get(rb, "t.dyn1").rva == 0x2100 && Get(rb, "t.dyn2").rva == 0x2200);
    CHECK(Get(rb, "t.dyn1").anchors[0].name == "anchor1" && rb.build.charaView.accessorRva == 0x2000);

    // a source buffer that is clobbered after the spec was filled: with non-owning pointers the pattern would now be all 'x'
    std::string pattern = "AA BB CC DD", id = "t.clobber", name = "src", seam = "t.clobber", key = "charaView.accessorRva", from = "t.clobber";
    AnchorSpec a;
    a.name = name.c_str();
    a.pattern = pattern.c_str();
    AnchorSpec guard;
    guard.name = "g";
    guard.pattern = "AA BB CC DD";
    guard.fromSeam = from.c_str();
    guard.predicates = { pred::After(seam.c_str(), 1) };
    SeamSpec spec { id.c_str(), SeamKind::Function, { a } };
    Binding b { key.c_str(), seam.c_str(), 0 };
    for (std::string* text : { &pattern, &id, &name, &seam, &key, &from })
        text->assign(text->size(), 'x');
    Registry reg;
    reg.seams = { spec };
    reg.bindings = { b };
    auto img2 = MakeImage();
    Plant(*img2, 0x2400, "aabbccdd");
    const auto rb2 = ResolveImage(*img2, reg);
    CHECK(Get(rb2, "t.clobber").status == SeamStatus::Found && Get(rb2, "t.clobber").rva == 0x2400 && rb2.build.charaView.accessorRva == 0x2400);
    CHECK(guard.fromSeam == "t.clobber" && guard.predicates[0].seam == "t.clobber");

    // the aggregate and designated-initializer ergonomics of the frozen doc still work with owned strings
    AnchorSpec d { .name = "des", .pattern = "AA BB CC DD", .expectedHits = 2, .pickIndex = 1, .minMargin = 0 };
    CHECK(d.name == "des" && d.pattern == "AA BB CC DD" && d.expectedHits == 2 && d.pickIndex == 1 && d.minMargin == 0 && d.fromSeam.empty());
    AnchorSpec e { .name = "e", .pattern = "48 8B 05 ?? ?? ?? ??", .valueFrom = ValueFrom::Target, .decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 },
                   .predicates = { pred::TargetIn(RegionKind::Bss) } };
    CHECK(e.decode.length == 7 && e.predicates.size() == 1 && e.searchIn == SearchIn::Code && e.minMargin == 3);
    const SeamSpec ds { .id = "t.des", .kind = SeamKind::CodeSite, .anchors = { d } };
    const Binding db { .key = "charaView.accessorRva", .seam = "t.des", .valueOffset = 5 };
    CHECK(ds.id == "t.des" && ds.kind == SeamKind::CodeSite && db.valueOffset == 5 && db.guardOffset == 0);
    const SeamSpec positional { "t.pos", SeamKind::Function, { d, e } };
    CHECK(positional.anchors.size() == 2);
    // a default-constructed predicate or anchor has no seam name (an empty string means none)
    CHECK(AnchorSpec {}.fromSeam.empty() && Predicate {}.seam.empty() && pred::TargetIn(RegionKind::Bss).seam.empty());
}

// M4: a CodeSite is a position inside code, not a function entry: no foreign-hook jump check, never EntryPatched
void TestCodeSite()
{
    // a code site that legitimately starts with a short jump (EB), an E9 and an FF 25 (a jump table thunk inside a function)
    const char* sites[] = { "eb10909011223344", "e901020304112233", "ff25000000001122" };
    const char* patterns[] = { "EB 10 90 90 11 22 33 44", "E9 01 02 03 04 11 22 33", "FF 25 00 00 00 00 11 22" };
    for (int i = 0; i < 3; ++i)
    {
        auto img = MakeImage();
        Plant(*img, 0x2100, sites[i]);
        const auto fn = ResolveImage(*img, Reg({ SeamSpec { "t.site", SeamKind::Function, { Plain(patterns[i]) } } }));
        const auto cs = ResolveImage(*img, Reg({ SeamSpec { "t.site", SeamKind::CodeSite, { Plain(patterns[i]) } } }));
        CHECK(Get(fn, "t.site").status == SeamStatus::EntryPatched && !fn.Usable("t.site")); // as before: an entry that starts with a jump
        CHECK(Get(cs, "t.site").status == SeamStatus::Found && Get(cs, "t.site").rva == 0x2100 && cs.Usable("t.site")); // not an entry: no jump test
    }
    // a CodeSite can still be reached through a body anchor and an offset, and it feeds dependents like any seam
    auto img = MakeImage();
    Plant(*img, 0x2120, "eb10909011223344");
    AnchorSpec dep = Plain("11 22 33 44");
    dep.predicates = { pred::After("t.site", 0x40) };
    const auto rb = ResolveImage(*img, Reg({ SeamSpec { "t.site", SeamKind::CodeSite, { Plain("EB 10 90 90", 0) } }, SeamSpec { "t.dep", SeamKind::CodeSite, { dep } } }));
    CHECK(Get(rb, "t.site").status == SeamStatus::Found && Get(rb, "t.dep").status == SeamStatus::Found && Get(rb, "t.dep").rva == 0x2124);

    // an entryAnchor makes no sense for a CodeSite (the retry reports an ENTRY): a registry error
    Registry bad = Reg({ SeamSpec { "t.site", SeamKind::CodeSite, { Plain("11 22 33 44") } } });
    CHECK(ValidateRegistry(bad).empty());
    bad.seams[0].anchors[0].entryAnchor = true;
    CHECK(!ValidateRegistry(bad).empty());
    // the CodeSite seam binds address fields like a Function seam, and is refused for an Offset field
    Registry bind = Reg({ SeamSpec { "t.site", SeamKind::CodeSite, { Plain("11 22 33 44") } } });
    bind.bindings = { { "jitterSource.phasesRva", "t.site", 0 }, { "jitterSource.phasesInstructions", "t.site", 0 } };
    CHECK(ValidateRegistry(bind).empty());
    bind.bindings = { { "charaView.targetsOffset", "t.site", 0 } };
    CHECK(!ValidateRegistry(bind).empty());
}

std::string RipHex(std::uint32_t at, std::uint32_t target, const char* head, const char* tail)
{
    // rip = the instruction start + the bytes of `head` + the 4 displacement bytes
    const std::int32_t disp = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - (static_cast<std::int64_t>(at) + std::string(head).size() / 2 + 4));
    return std::string(head) + ToHex(std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&disp), reinterpret_cast<const std::uint8_t*>(&disp) + 4)) + tail;
}

// M6: the RIP decode checks the ModRM byte, ValueIn bounds a Field / RipDeref32 result, RipDeref32 reads read-only data only
void TestDecodeShapeAndValuePredicates()
{
    // --- the ModRM byte before the displacement must be mod=00 r/m=101 ---
    // (1) a real RIP-relative lea / mov r32 (other registers): accepted
    {
        auto img = MakeImage();
        Plant(*img, 0x2000, RipHex(0x2000, 0x9100, "488d05", "90909090")); // lea rax,[rip+x]
        Plant(*img, 0x2100, RipHex(0x2100, 0x9200, "8b0d", "9090909090")); // mov ecx,[rip+x]: modrm 0x0D
        AnchorSpec lea = Plain("48 8D 05 ?? ?? ?? ?? 90 90 90 90");
        lea.valueFrom = ValueFrom::Target;
        lea.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
        lea.predicates = { pred::TargetIn(RegionKind::Bss) };
        auto rb = ResolveImage(*img, Reg({ SeamSpec { "t.lea", SeamKind::Global, { lea } } }));
        CHECK(Get(rb, "t.lea").status == SeamStatus::Found && Get(rb, "t.lea").rva == 0x9100);
        AnchorSpec mov = Plain("8B 0D ?? ?? ?? ?? 90 90 90 90 90");
        mov.valueFrom = ValueFrom::Target;
        mov.decode = { DecodeKind::RipDisp32, 0, 2, 6, 0 };
        mov.predicates = { pred::TargetIn(RegionKind::Bss) };
        rb = ResolveImage(*img, Reg({ SeamSpec { "t.mov", SeamKind::Global, { mov } } }));
        CHECK(Get(rb, "t.mov").status == SeamStatus::Found && Get(rb, "t.mov").rva == 0x9200);
    }
    // (2) a [rax+rcx*8+disp32] operand (modrm 0x84 + SIB): the same bytes read as "RIP relative" would give a garbage target that still
    //     lands in a section; the ModRM test drops the candidate, with the reason
    {
        auto img = MakeImage();
        Plant(*img, 0x2000, "488b84c8c8020000c3");
        AnchorSpec a = Plain("48 8B 84 C8 ?? ?? ?? ?? C3");
        a.valueFrom = ValueFrom::Target;
        a.decode = { DecodeKind::RipDisp32, 0, 4, 8, 0 }; // pretends the disp32 at +4 is RIP relative: the byte at +3 is the SIB 0xC8
        a.predicates = { pred::TargetIn(RegionKind::Image) };
        const auto rb = ResolveImage(*img, Reg({ SeamSpec { "t.shape", SeamKind::Global, { a } } }));
        CHECK(Get(rb, "t.shape").status == SeamStatus::NotFound && Get(rb, "t.shape").anchors[0].patternHits == 1 && Get(rb, "t.shape").anchors[0].candidates == 0);
        CHECK(Get(rb, "t.shape").anchors[0].note.find("ModRM") != std::string::npos);
        // a [rax+disp32] operand (modrm 0x80, mod=10) is not RIP relative either. (Known limit: the absolute [disp32] form has the SIB byte
        // 0x25 right before the displacement, which has the shape of a RIP ModRM; compiled x64 code does not use it.)
        auto reg = MakeImage();
        Plant(*reg, 0x2000, "8b80c8020000cc");
        AnchorSpec b = Plain("8B 80 ?? ?? ?? ?? CC");
        b.valueFrom = ValueFrom::Target;
        b.decode = { DecodeKind::RipDisp32, 0, 2, 6, 0 };
        const auto rb2 = ResolveImage(*reg, Reg({ SeamSpec { "t.reg", SeamKind::Global, { b } } }));
        CHECK(Get(rb2, "t.reg").status == SeamStatus::NotFound && Get(rb2, "t.reg").anchors[0].note.find("ModRM") != std::string::npos);
        // Both halves of the test matter. r/m=101 is not enough, the MOD bits must be 00: mod=01 (0x45), mod=10 (0x85, [rbp+disp32]) and
        // mod=11 (0xC5, a register operand) all have r/m=101 and are refused (a check on `modrm & 7` alone would take them for RIP-relative
        // operands). And mod=00 is not enough, r/m must be 101: 0x00 ([rax]), 0x04 (a SIB form) and 0x0C (reg 1, SIB) are refused (a check
        // on the mod bits alone would take them).
        struct ModRmCase
        {
            const char* hex;
            const char* pattern;
        };
        for (const ModRmCase& c : { ModRmCase { "8b45c8020000cc", "8B 45 ?? ?? ?? ?? CC" }, ModRmCase { "8b85c8020000cc", "8B 85 ?? ?? ?? ?? CC" },
                                    ModRmCase { "8bc5c8020000cc", "8B C5 ?? ?? ?? ?? CC" }, ModRmCase { "8b00c8020000cc", "8B 00 ?? ?? ?? ?? CC" },
                                    ModRmCase { "8b04c8020000cc", "8B 04 ?? ?? ?? ?? CC" }, ModRmCase { "8b0cc8020000cc", "8B 0C ?? ?? ?? ?? CC" } })
        {
            auto imgM = MakeImage();
            Plant(*imgM, 0x2000, c.hex);
            AnchorSpec m = Plain(c.pattern);
            m.valueFrom = ValueFrom::Target;
            m.decode = { DecodeKind::RipDisp32, 0, 2, 6, 0 };
            const auto rbM = ResolveImage(*imgM, Reg({ SeamSpec { "t.mod", SeamKind::Global, { m } } }));
            CHECK(Get(rbM, "t.mod").status == SeamStatus::NotFound && Get(rbM, "t.mod").anchors[0].patternHits == 1 &&
                  Get(rbM, "t.mod").anchors[0].candidates == 0);
            CHECK(Get(rbM, "t.mod").anchors[0].note.find("ModRM") != std::string::npos);
        }
    }
    // (3) the validator needs a disp32 that something (opcode + ModRM) precedes
    {
        Registry r = Reg({ SeamSpec { "t.a", SeamKind::Global, { Plain("48 8B 05 ?? ?? ?? ??") } } });
        r.seams[0].anchors[0].valueFrom = ValueFrom::Target;
        r.seams[0].anchors[0].decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
        CHECK(ValidateRegistry(r).empty());
        r.seams[0].anchors[0].decode = { DecodeKind::RipDisp32, 0, 1, 7, 0 };
        CHECK(!ValidateRegistry(r).empty());
        r.seams[0].anchors[0].decode = { DecodeKind::RipDisp32, 0, 0, 7, 0 };
        CHECK(!ValidateRegistry(r).empty());
        r.seams[0].anchors[0].decode = { DecodeKind::RipDisp32, 0, 2, 6, 0 };
        CHECK(ValidateRegistry(r).empty());
    }

    // --- ValueIn: unsigned inclusive bounds on the decoded value of a Field / RipDeref32 anchor ---
    {
        auto img = MakeImage();
        Plant(*img, 0x2000, "488b84c8c8020000c3"); // the field 0x2c8
        auto run = [&](std::uint32_t lo, std::uint32_t hi)
        {
            AnchorSpec a = Plain("48 8B 84 C8 ?? ?? ?? ?? C3");
            a.valueFrom = ValueFrom::Target;
            a.decode = { DecodeKind::Field, 4, 0, 0, 4 };
            a.predicates = { pred::ValueIn(lo, hi) };
            return ResolveImage(*img, Reg({ SeamSpec { "t.f", SeamKind::Offset, { a } } }));
        };
        CHECK(Get(run(0x2c0, 0x2d0), "t.f").status == SeamStatus::Found && Get(run(0x2c0, 0x2d0), "t.f").rva == 0x2c8);
        CHECK(Get(run(0x2c8, 0x2c8), "t.f").status == SeamStatus::Found);         // both edges inclusive
        CHECK(Get(run(0x2c9, 0x2d0), "t.f").status == SeamStatus::NotFound);       // one above the value
        CHECK(Get(run(0x100, 0x2c7), "t.f").status == SeamStatus::NotFound);       // one below
        CHECK(!Get(run(0x2c9, 0x2d0), "t.f").anchors[0].note.empty());
        // a value predicate that fails leaves the decoy out: two sites, only one in range
        Plant(*img, 0x2200, "488b84c8000100c3c3");
        AnchorSpec both = Plain("48 8B 84 C8 ?? ?? ?? ??");
        both.valueFrom = ValueFrom::Target;
        both.decode = { DecodeKind::Field, 4, 0, 0, 4 };
        auto rb = ResolveImage(*img, Reg({ SeamSpec { "t.f", SeamKind::Offset, { both } } }));
        CHECK(Get(rb, "t.f").status == SeamStatus::Ambiguous && Get(rb, "t.f").hits == 2);
        both.predicates = { pred::ValueEquals(0x2c8) };
        rb = ResolveImage(*img, Reg({ SeamSpec { "t.f", SeamKind::Offset, { both } } }));
        CHECK(Get(rb, "t.f").status == SeamStatus::Found && Get(rb, "t.f").rva == 0x2c8 && Get(rb, "t.f").anchors[0].patternHits == 2);
        // the full unsigned range: 0xffffffff (a -1 immediate) and a float's bits do not fit an int32
        auto high = MakeImage();
        Plant(*high, 0x2000, "ba" "ffffffff" "c3c3c3");
        AnchorSpec imm = Plain("BA ?? ?? ?? ?? C3 C3 C3");
        imm.valueFrom = ValueFrom::Target;
        imm.decode = { DecodeKind::Field, 1, 0, 0, 4 };
        imm.predicates = { pred::ValueIn(0xfffffff0u, 0xffffffffu) };
        rb = ResolveImage(*high, Reg({ SeamSpec { "t.imm", SeamKind::Offset, { imm } } }));
        CHECK(Get(rb, "t.imm").status == SeamStatus::Found && Get(rb, "t.imm").rva == 0xffffffffu);
        imm.predicates = { pred::ValueIn(0, 0x7fffffffu) };
        rb = ResolveImage(*high, Reg({ SeamSpec { "t.imm", SeamKind::Offset, { imm } } }));
        CHECK(Get(rb, "t.imm").status == SeamStatus::NotFound);
        imm.predicates = { pred::ValueEquals(0xffffffffu) };
        CHECK(Get(ResolveImage(*high, Reg({ SeamSpec { "t.imm", SeamKind::Offset, { imm } } })), "t.imm").status == SeamStatus::Found);
        imm.predicates = { pred::ValueEquals(0xfffffffeu) };
        CHECK(Get(ResolveImage(*high, Reg({ SeamSpec { "t.imm", SeamKind::Offset, { imm } } })), "t.imm").status == SeamStatus::NotFound);
    }
    // RipDeref32 with ValueIn: a float constant in .rdata (-1.0f = 0xBF800000)
    {
        auto img = MakeImage();
        PlantAccessor(*img, 0x2000, 0x6100);
        Plant(*img, 0x6100, "000080bf");
        AnchorSpec a = Plain(kAccessor);
        a.valueFrom = ValueFrom::Target;
        a.decode = { DecodeKind::RipDeref32, 0, 3, 7, 0 };
        a.predicates = { pred::ValueIn(0xbf800000u, 0xbf800000u) };
        CHECK(Get(ResolveImage(*img, Reg({ SeamSpec { "t.k", SeamKind::Offset, { a } } })), "t.k").rva == 0xbf800000u);
        a.predicates = { pred::ValueIn(0x3f800000u, 0x3f800000u) };
        CHECK(Get(ResolveImage(*img, Reg({ SeamSpec { "t.k", SeamKind::Offset, { a } } })), "t.k").status == SeamStatus::NotFound);
    }
    // validation: ValueIn needs a value decode, and lo <= hi
    {
        Registry r = Reg({ SeamSpec { "t.a", SeamKind::Offset, { Plain("11 22 33 44") } } });
        r.seams[0].anchors[0].predicates = { pred::ValueIn(1, 2) };
        CHECK(!ValidateRegistry(r).empty()); // no decode
        r.seams[0].anchors[0].valueFrom = ValueFrom::Target;
        r.seams[0].anchors[0].decode = { DecodeKind::Rel32, 0, 0, 0, 0 };
        CHECK(!ValidateRegistry(r).empty()); // an address decode
        r.seams[0].anchors[0].decode = { DecodeKind::Field, 0, 0, 0, 4 };
        CHECK(ValidateRegistry(r).empty());
        r.seams[0].anchors[0].predicates = { pred::ValueIn(5, 1) };
        CHECK(!ValidateRegistry(r).empty()); // lo above hi
    }

    // --- RipDeref32 reads read-only data only: the live process and the offline file agree there ---
    {
        const struct { std::uint32_t target; bool accepted; const char* what; } cases[] = {
            { 0x6100, true, ".rdata" }, { 0x8100, false, ".data (initialized, writable)" }, { 0x9100, false, "bss" }, { 0x2800, false, ".text" },
            { 0xb100, false, ".pdata" } };
        for (const auto& c : cases)
        {
            auto img = MakeImage();
            PlantAccessor(*img, 0x2000, c.target);
            Plant(*img, c.target, "78563412");
            AnchorSpec a = Plain(kAccessor);
            a.valueFrom = ValueFrom::Target;
            a.decode = { DecodeKind::RipDeref32, 0, 3, 7, 0 }; // no TargetIn predicate: the decode itself refuses
            const auto rb = ResolveImage(*img, Reg({ SeamSpec { "t.k", SeamKind::Offset, { a } } }));
            if (c.accepted)
                CHECK(Get(rb, "t.k").status == SeamStatus::Found && Get(rb, "t.k").rva == 0x12345678);
            else
            {
                if (Get(rb, "t.k").status != SeamStatus::NotFound)
                    std::printf("FAIL: a RipDeref32 into %s was accepted\n", c.what);
                CHECK(Get(rb, "t.k").status == SeamStatus::NotFound && Get(rb, "t.k").anchors[0].note.find("read-only") != std::string::npos);
            }
        }
    }
}

// M5: seams that must differ cannot silently share an address
void TestMustDiffer()
{
    // t.a finds 0x2000 by its own bytes, t.b reaches the same function through a body anchor (valueOffset -0x10)
    auto img = MakeImage();
    Plant(*img, 0x2000, "11223344");
    Plant(*img, 0x2010, "55667788");
    auto reg = [](std::vector<std::vector<std::string>> groups)
    {
        Registry r = Reg({ SeamSpec { "t.a", SeamKind::Function, { Plain("11 22 33 44") } }, SeamSpec { "t.b", SeamKind::Function, { Plain("55 66 77 88", -0x10) } } });
        r.mustDiffer = std::move(groups);
        return r;
    };
    // without a group both are Found at the same address: this is the silent failure the group exists for
    auto rb = ResolveImage(*img, reg({}));
    CHECK(Get(rb, "t.a").status == SeamStatus::Found && Get(rb, "t.b").status == SeamStatus::Found && Get(rb, "t.a").rva == Get(rb, "t.b").rva);
    // with the group the later one is Conflict (inert, no value), the earlier keeps its value
    rb = ResolveImage(*img, reg({ { "t.a", "t.b" } }));
    CHECK(ValidateRegistry(reg({ { "t.a", "t.b" } })).empty());
    CHECK(Get(rb, "t.a").status == SeamStatus::Found && Get(rb, "t.a").rva == 0x2000);
    CHECK(Get(rb, "t.b").status == SeamStatus::Conflict && !Get(rb, "t.b").hasValue && Get(rb, "t.b").rva == 0 && !rb.Usable("t.b"));
    CHECK(Get(rb, "t.b").reason.find("t.a") != std::string::npos && Get(rb, "t.b").reason.find("must differ") != std::string::npos);
    // "later" is the order of the group, not of the registry
    rb = ResolveImage(*img, reg({ { "t.b", "t.a" } }));
    CHECK(Get(rb, "t.b").status == SeamStatus::Found && Get(rb, "t.a").status == SeamStatus::Conflict);
    // different values: nothing happens
    auto apart = MakeImage();
    Plant(*apart, 0x2000, "11223344");
    Plant(*apart, 0x2210, "55667788");
    rb = ResolveImage(*apart, reg({ { "t.a", "t.b" } }));
    CHECK(Get(rb, "t.a").status == SeamStatus::Found && Get(rb, "t.b").status == SeamStatus::Found && Get(rb, "t.b").rva == 0x2200);
    // a member without a value (not found) takes part in nothing
    auto onlyB = MakeImage();
    Plant(*onlyB, 0x2010, "55667788");
    rb = ResolveImage(*onlyB, reg({ { "t.a", "t.b" } }));
    CHECK(Get(rb, "t.a").status == SeamStatus::NotFound && Get(rb, "t.b").status == SeamStatus::Found);

    // the clash is decided BEFORE anything depends on the later seam: its dependent sees no value
    AnchorSpec dep = Plain("99 AA BB CC");
    dep.predicates = { pred::After("t.b", 0x100) };
    Registry withDep = reg({ { "t.a", "t.b" } });
    withDep.seams.push_back(SeamSpec { "t.dep", SeamKind::Function, { dep } });
    Plant(*img, 0x2080, "99aabbcc");
    rb = ResolveImage(*img, withDep);
    CHECK(Get(rb, "t.b").status == SeamStatus::Conflict && Get(rb, "t.dep").status == SeamStatus::NotFound && Get(rb, "t.dep").anchors[0].outcome == AnchorOutcome::DependencyMissing);

    // a patched earlier member still counts (the address is taken), a three member group checks against every earlier member
    auto patched = MakeImage();
    Plant(*patched, 0x2000, "e911223344"); // t.a's own bytes overwritten by a hook: EntryPatched through the body anchor of t.a2 below
    Plant(*patched, 0x2040, "aabbccdd");
    Registry three = Reg({ SeamSpec { "t.a", SeamKind::Function, { Plain("AA BB CC DD", -0x40) } }, SeamSpec { "t.b", SeamKind::Function, { Plain("AA BB CC DD", -0x40) } },
                           SeamSpec { "t.c", SeamKind::Function, { Plain("AA BB CC DD", -0x40) } } });
    three.mustDiffer = { { "t.a", "t.b", "t.c" } };
    rb = ResolveImage(*patched, three);
    CHECK(Get(rb, "t.a").status == SeamStatus::EntryPatched && Get(rb, "t.a").hasValue);
    CHECK(Get(rb, "t.b").status == SeamStatus::Conflict && Get(rb, "t.c").status == SeamStatus::Conflict);

    // validation: at least two seams, known ids, Function / CodeSite only, no repeats, and the group order must not contradict a dependency
    CHECK(!ValidateRegistry(reg({ { "t.a" } })).empty());
    CHECK(!ValidateRegistry(reg({ {} })).empty());
    CHECK(!ValidateRegistry(reg({ { "t.a", "t.nothing" } })).empty());
    CHECK(!ValidateRegistry(reg({ { "t.a", "t.a" } })).empty());
    Registry glob = reg({ { "t.a", "t.g" } });
    glob.seams.push_back(SeamSpec { "t.g", SeamKind::Global, { Plain("77 77 77 77") } });
    CHECK(!ValidateRegistry(glob).empty());
    glob.seams.back().kind = SeamKind::CodeSite;
    CHECK(ValidateRegistry(glob).empty());
    Registry cyc = withDep; // t.dep depends on t.b: listing t.dep BEFORE t.b contradicts it
    cyc.mustDiffer = { { "t.dep", "t.b" } };
    CHECK(!ValidateRegistry(cyc).empty());
    cyc.mustDiffer = { { "t.b", "t.dep" } };
    CHECK(ValidateRegistry(cyc).empty());
    // a broken group resolves nothing, like any registry error
    rb = ResolveImage(*img, reg({ { "t.a", "t.nothing" } }));
    CHECK(!rb.registryErrors.empty() && Get(rb, "t.a").status == SeamStatus::NotFound);
}

// L9: a seam that is an Offset (a raw number) is no reference point for a guard or a predicate
void TestOffsetSeamIsNoReference()
{
    AnchorSpec off = Plain("41 80 78 ?? 02");
    off.valueFrom = ValueFrom::Target;
    off.decode = { DecodeKind::Field, 3, 0, 0, 1 };
    AnchorSpec user = Plain("11 22 33 44");
    user.predicates = { pred::After("t.off", 4) };
    Registry r = Reg({ SeamSpec { "t.off", SeamKind::Offset, { off } }, SeamSpec { "t.user", SeamKind::Function, { user } } });
    CHECK(!ValidateRegistry(r).empty());
    AnchorSpec guard = Plain("11 22 33 44");
    guard.fromSeam = "t.off";
    r.seams[1].anchors[0] = guard;
    CHECK(!ValidateRegistry(r).empty());
    r.seams[0].kind = SeamKind::Global;
    CHECK(ValidateRegistry(r).empty());
}

// M2: the multi-anchor policy looks at the WHOLE candidate list of an ambiguous anchor, not at the 64 values kept for reports
void TestCandidateListPolicy()
{
    // anchor "u" is unique at 0x19f0. Anchor "a" matches 100 sites (match = 0x1020 + 0x20 * i, value = match - 0x10): the 80th (i = 79) has the value 0x19f0.
    auto image = [](std::uint32_t shift)
    {
        auto img = MakeImage();
        Plant(*img, 0x19f0, "11223344");
        for (std::uint32_t i = 0; i < 100; ++i)
            Plant(*img, 0x1020 + shift + 0x20 * i, "55667788");
        return img;
    };
    const Registry reg = Reg({ SeamSpec { "t.m", SeamKind::Function, { Plain("11 22 33 44", 0), Plain("55 66 77 88", -0x10) } } });

    auto contains = image(0);
    auto rb = ResolveImage(*contains, reg);
    const SeamResult& ok = Get(rb, "t.m");
    CHECK(ok.status == SeamStatus::FoundSingleAnchor && ok.rva == 0x19f0 && ok.hasValue); // decision 2: unique + an ambiguous anchor that contains it
    REQUIRE(ok.anchors.size() == 2);
    CHECK(ok.anchors[1].outcome == AnchorOutcome::Multiple && ok.anchors[1].candidates == 100 && ok.anchors[1].patternHits == 100);
    // the report keeps 64 values, and the unique value is NOT among them: the policy cannot have used that list
    CHECK(ok.anchors[1].candidateValues.size() == 64);
    CHECK(std::find(ok.anchors[1].candidateValues.begin(), ok.anchors[1].candidateValues.end(), 0x19f0u) == ok.anchors[1].candidateValues.end());

    // the same 100 candidates without the unique value: Conflict (the unique anchor is not among them)
    auto apart = image(8);
    rb = ResolveImage(*apart, reg);
    CHECK(Get(rb, "t.m").status == SeamStatus::Conflict && !Get(rb, "t.m").hasValue);

    // an anchor whose scan overflowed (4096 raw matches) has an incomplete list: no information, neither a contradiction nor a confirmation
    auto img = MakeImage();
    Plant(*img, 0x19f0, "11223344");
    AnchorSpec loose = Plain("9? ?? ??", 0); // matches nearly every filler byte of .text
    rb = ResolveImage(*img, Reg({ SeamSpec { "t.m", SeamKind::Function, { Plain("11 22 33 44", 0), loose } } }));
    CHECK(Get(rb, "t.m").status == SeamStatus::FoundSingleAnchor && Get(rb, "t.m").rva == 0x19f0);
    CHECK(Get(rb, "t.m").anchors[1].overflow && Get(rb, "t.m").anchors[1].outcome == AnchorOutcome::Multiple);
    CHECK(Get(rb, "t.m").reason.find("too often") != std::string::npos);
    // and two overflowed anchors alone are still Ambiguous
    rb = ResolveImage(*img, Reg({ SeamSpec { "t.m", SeamKind::Function, { loose, Plain("9? ?? ??", 0) } } }));
    CHECK(Get(rb, "t.m").status == SeamStatus::Ambiguous);
}

// M1 / M9: a value that rests on a weak seam (EntryPatched, single-anchor) is never plain Found
void TestWeakDependencies()
{
    auto img = MakeImage();
    Plant(*img, 0x2120, "cafebabe11223344"); // t.fn's body anchor
    Plant(*img, 0x2100, "e901020304");       // a foreign hook over its entry: EntryPatched
    Plant(*img, 0x3000, "12345678");         // t.single: found by this anchor only
    Plant(*img, 0x3500, "99aabbcc");         // dependents
    Plant(*img, 0x3600, "ddeeff00");

    auto fn = [](const char* id) { return SeamSpec { id, SeamKind::Function, { Plain("CA FE BA BE 11 22 33 44", -0x20) } }; };
    auto dependent = [](const char* id, const char* pattern, const char* on, std::int32_t gap)
    {
        AnchorSpec a = Plain(pattern);
        a.predicates = { pred::After(on, gap) };
        return SeamSpec { id, SeamKind::Function, { a } };
    };

    // a plain Found dependency: the dependent stays plain Found
    auto good = MakeImage();
    Plant(*good, 0x2000, "48895c2408");
    Plant(*good, 0x2120, "cafebabe11223344");
    Plant(*good, 0x3500, "99aabbcc");
    auto rb = ResolveImage(*good, Reg({ fn("t.fn"), dependent("t.dep", "99 AA BB CC", "t.fn", 0x2000) }));
    CHECK(Get(rb, "t.fn").status == SeamStatus::Found && Get(rb, "t.dep").status == SeamStatus::Found && !Get(rb, "t.dep").viaWeakDependency);
    CHECK(Get(rb, "t.dep").reason.empty() && !Get(rb, "t.dep").anchors[0].weakDependency);

    // an EntryPatched dependency: the dependent is FoundSingleAnchor (still usable), says why, and the weakness is transitive
    rb = ResolveImage(*img, Reg({ fn("t.fn"), dependent("t.dep", "99 AA BB CC", "t.fn", 0x2000), dependent("t.dep2", "DD EE FF 00", "t.dep", 0x2000) }));
    CHECK(Get(rb, "t.fn").status == SeamStatus::EntryPatched);
    CHECK(Get(rb, "t.dep").status == SeamStatus::FoundSingleAnchor && Get(rb, "t.dep").viaWeakDependency && rb.Usable("t.dep"));
    CHECK(Get(rb, "t.dep").reason.find("t.fn") != std::string::npos && Get(rb, "t.dep").rva == 0x3500);
    CHECK(Get(rb, "t.dep2").status == SeamStatus::FoundSingleAnchor && Get(rb, "t.dep2").viaWeakDependency && Get(rb, "t.dep2").reason.find("t.dep") != std::string::npos);

    // a single-anchor dependency (two anchors, one found nothing) weakens its dependent the same way
    SeamSpec single { "t.single", SeamKind::Function, { Plain("12 34 56 78"), Plain("77 77 77 77") } };
    rb = ResolveImage(*img, Reg({ single, dependent("t.dep", "99 AA BB CC", "t.single", 0x1000) }));
    CHECK(Get(rb, "t.single").status == SeamStatus::FoundSingleAnchor && !Get(rb, "t.single").viaWeakDependency); // weak because single, not via a dependency
    CHECK(Get(rb, "t.dep").status == SeamStatus::FoundSingleAnchor && Get(rb, "t.dep").viaWeakDependency);

    // a guard anchor (fromSeam) of a weak seam is weak too
    AnchorSpec guard = Plain("12 34 56 78");
    guard.fromSeam = "t.single";
    guard.valueFrom = ValueFrom::Match;
    rb = ResolveImage(*img, Reg({ single, SeamSpec { "t.guard", SeamKind::Global, { guard } } }));
    CHECK(Get(rb, "t.guard").status == SeamStatus::FoundSingleAnchor && Get(rb, "t.guard").viaWeakDependency);

    // two agreeing anchors where one of them depends on a weak seam: still capped (conservative: any weak dependency among the unique anchors)
    AnchorSpec own = Plain("99 AA BB CC");
    AnchorSpec viaWeak = Plain("99 AA BB CC");
    viaWeak.predicates = { pred::After("t.fn", 0x2000) };
    rb = ResolveImage(*img, Reg({ fn("t.fn"), SeamSpec { "t.two", SeamKind::Function, { own, viaWeak } } }));
    CHECK(Get(rb, "t.two").status == SeamStatus::FoundSingleAnchor && Get(rb, "t.two").viaWeakDependency && Get(rb, "t.two").hits == 2);
    // the same seam against a strong dependency is plain Found
    rb = ResolveImage(*good, Reg({ fn("t.fn"), SeamSpec { "t.two", SeamKind::Function, { own, viaWeak } } }));
    CHECK(Get(rb, "t.two").status == SeamStatus::Found && !Get(rb, "t.two").viaWeakDependency);

    // a weak seam that is not usable at all is not a weak value: the dependent is NotFound (nothing changes there)
    auto none = MakeImage();
    Plant(*none, 0x3500, "99aabbcc");
    rb = ResolveImage(*none, Reg({ fn("t.fn"), dependent("t.dep", "99 AA BB CC", "t.fn", 0x2000) }));
    CHECK(Get(rb, "t.dep").status == SeamStatus::NotFound && !Get(rb, "t.dep").viaWeakDependency);

    // the relaxed retry (EntryPatched without a body anchor) weakens its dependents as well
    const char* entryHex = "48895c240848897424105748 83ec208b4108c3cc";
    auto relaxedImg = MakeImage();
    Plant(*relaxedImg, 0x2000, entryHex);
    Plant(*relaxedImg, 0x2000, "e911223344");
    Plant(*relaxedImg, 0x2300, "99aabbcc");
    AnchorSpec entry = Plain("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 41 08 C3 CC");
    entry.entryAnchor = true;
    rb = ResolveImage(*relaxedImg, Reg({ SeamSpec { "t.entry", SeamKind::Function, { entry } }, dependent("t.dep", "99 AA BB CC", "t.entry", 0x400) }));
    CHECK(Get(rb, "t.entry").status == SeamStatus::EntryPatched && Get(rb, "t.dep").status == SeamStatus::FoundSingleAnchor && Get(rb, "t.dep").viaWeakDependency);
}

// L1: a relaxed entry scan that fails part-way has a partial hit list: it is never used (a partial list could read as "exactly one")
void TestRelaxedScanUnreadable()
{
    // a 3 MiB .text: the scan reads it in 1 MiB windows. The hooked function sits in the first window; the reader fails the far windows
    // of the SECOND (relaxed) scan only.
    constexpr std::uint32_t kBigText = 0x300000;
    auto big = DenseImage::Synthetic(0x1234, 0x400000,
                                     { { ".text", 0x1000, kBigText, kBigText, DenseImage::kText }, { ".pdata", 0x301000, 0x1000, 0x1000, DenseImage::kRdata } },
                                     0x301000, 0);
    REQUIRE(big != nullptr);
    for (std::uint32_t i = 0x1000; i < 0x1000 + kBigText; ++i)
        big->Bytes()[i] = static_cast<std::uint8_t>(0x90 + (i * 7) % 5);
    Plant(*big, 0x2000, "48895c240848897424105748 83ec208b4108c3cc");
    Plant(*big, 0x2000, "e911223344"); // the hook
    AnchorSpec entry = Plain("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 41 08 C3 CC");
    entry.entryAnchor = true;
    const Registry reg = Reg({ SeamSpec { "t.entry", SeamKind::Function, { entry } } });

    // healthy: EntryPatched through the relaxed scan
    CHECK(Get(ResolveImage(*big, reg), "t.entry").status == SeamStatus::EntryPatched);

    struct FailsLate : ImageView
    {
        const DenseImage& inner;
        mutable int farReads = 0;
        explicit FailsLate(const DenseImage& i) : inner(i) { _info = i.Info(); }
        bool Read(std::uint32_t rva, void* out, std::size_t n) const override
        {
            // the main scan reads windows 2 and 3 (rva >= 0x101000) once each: the third far read is the relaxed scan's, and it fails
            if (rva >= 0x101000 && n >= 0x1000 && ++farReads > 2)
                return false;
            return inner.Read(rva, out, n);
        }
    } failing(*big);
    const auto rb = ResolveImage(failing, reg);
    CHECK(failing.farReads >= 3); // the relaxed scan really ran into the failing read
    CHECK(Get(rb, "t.entry").status == SeamStatus::NotFound && !Get(rb, "t.entry").hasValue);
    CHECK(Get(rb, "t.entry").anchors[0].note.find("relaxed entry scan could not read") != std::string::npos);
}
} // namespace

int main()
{
    TestOverflow();
    TestUnreadableScan();
    TestInvalidImage();
    TestSingleAnchor();
    TestDecode();
    TestTargetRegions();
    TestSeamRelationPredicates();
    TestFunctionPredicates();
    TestMultiAnchorPolicy();
    TestExpectedHitsAndGuards();
    TestDependencies();
    TestEntryPatched();
    TestValidation();
    TestOwnedStrings();
    TestCodeSite();
    TestDecodeShapeAndValuePredicates();
    TestMustDiffer();
    TestOffsetSeamIsNoReference();
    TestCandidateListPolicy();
    TestWeakDependencies();
    TestRelaxedScanUnreadable();
    return TEST_RESULT("resolve_seam_policy");
}
