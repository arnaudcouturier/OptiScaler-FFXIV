// CharaView as the first full seam of the resolver, proven on committed bytes only (no executable):
//   * the registry's pattern is the pattern of CharaViewBuild (GameBuild.h) byte for byte and mask for mask,
//   * on a dense image with the layout of the CURRENT build the resolver finds the same accessor and manager RVAs as the table and as
//     the existing CharaTable scan (CharaTable.h ResolveFromImage, which P2 migrates later),
//   * on a dense image with the layout of the PREVIOUS build (B, bytes read from the B file before it was removed)
//     the same pattern finds B's accessor 0x26ab40 and decodes B's manager pointer 0x28f9490,
//   * the fixture rules: only "B-read" rows are evidence, maskB placeholders are never planted, derived rows are refused.
#include "old_build_b_quality.h"

#include <algorithm>
#include "ext/ffxiv/chara/CharaTable.h"
#include "ext/ffxiv/resolve/Entry.h"
#include "ext/ffxiv/resolve/Registry.h"
#include "ext/ffxiv/resolve/Resolver.h"

using namespace rt;
using namespace oldb;

namespace
{
constexpr std::uintptr_t kBase = 0x140000000ull;

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

// The reader the existing CharaTable scan wants, over the dense image.
auto ReaderOf(const DenseImage& img)
{
    return [&img](std::uintptr_t address, void* out, std::size_t bytes)
    {
        return address >= kBase && address - kBase <= 0xffffffffull && img.Read(static_cast<std::uint32_t>(address - kBase), out, bytes);
    };
}

void TestSpecEqualsTable()
{
    const Registry reg = BuildRegistry();
    CHECK(ValidateRegistry(reg).empty());
    const SeamSpec* accessor = nullptr;
    for (const SeamSpec& s : reg.seams)
        accessor = std::string_view(s.id) == "chara.accessor" ? &s : accessor;
    REQUIRE(accessor != nullptr && accessor->anchors.size() == 1);

    // The pattern text pinned on its own, independent of CharaViewBuild (P3 removes that table: this literal is then the only pin of the
    // CharaView pattern, and over-wildcarding or a changed fixed byte passes every other gate: unique hit, margin floor, decode right)
    CHECK(accessor->anchors[0].pattern == "48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 C8 02 00 00 C3");
    CHECK(accessor->anchors[0].minMargin == 3 && accessor->anchors[0].searchIn == SearchIn::Code && accessor->anchors[0].expectedHits == 1);
    CHECK(accessor->anchors[0].predicates.size() == 1 && accessor->anchors[0].predicates[0].kind == PredicateKind::TargetIn &&
          accessor->anchors[0].predicates[0].region == RegionKind::Bss);
    Pattern p;
    REQUIRE(ParsePattern(accessor->anchors[0].pattern, p));
    CHECK(p.size() == 18 && p.fixedBytes() == 14);
    const ext::ffxiv::CharaViewBuild table {};
    REQUIRE(p.size() == table.pattern.size());
    for (std::size_t i = 0; i < p.size(); ++i)
    {
        CHECK(p.mask[i] == (table.patternMask[i] != 0 ? 0xff : 0x00));
        if (table.patternMask[i] != 0)
            CHECK(p.bytes[i] == table.pattern[i]);
    }
    CHECK(accessor->anchors[0].decode.kind == DecodeKind::RipDisp32 && accessor->anchors[0].decode.length == table.ripInstructionLength);
    CHECK(accessor->kind == SeamKind::Function);

    // the bindings that fill the table fields
    bool a = false, m = false;
    for (const Binding& b : reg.bindings)
    {
        a = a || (std::string_view(b.key) == "charaView.accessorRva" && std::string_view(b.seam) == "chara.accessor");
        m = m || (std::string_view(b.key) == "charaView.managerPointerRva" && std::string_view(b.seam) == "chara.managerPointer");
    }
    CHECK(a && m);
}

void TestCurrentBuild()
{
    // Only the CharaView seams: this image holds only the CharaView accessor row, so another group's seams would be unresolved here and
    // print '# key unresolved' comments (their own tests and resolve_golden_exe cover them).
    Registry reg = BuildRegistry();
    reg.seams.erase(std::remove_if(reg.seams.begin(), reg.seams.end(), [](const SeamSpec& sp) { return sp.id.rfind("chara.", 0) != 0; }), reg.seams.end());
    reg.bindings.erase(std::remove_if(reg.bindings.begin(), reg.bindings.end(), [](const Binding& b) { return b.key.rfind("charaView.", 0) != 0; }),
                       reg.bindings.end());
    reg.mustDiffer.clear();
    auto img = MakeCImage();
    REQUIRE(img != nullptr);
    CHECK(PlantRow(*img, kCharaAccessorC));

    LogCapture log;
    ResolveOptions opts;
    opts.knownTable = &ext::ffxiv::kCurrentBuild;
    opts.sink = log.Sink();
    const ResolvedBuild rb = ResolveImage(*img, reg, opts);
    const SeamResult& acc = Get(rb, "chara.accessor");
    const SeamResult& mgr = Get(rb, "chara.managerPointer");
    CHECK(acc.status == SeamStatus::Found && acc.rva == 0x26abd0 && acc.hits == 1);
    CHECK(mgr.status == SeamStatus::Found && mgr.rva == 0x290e490 && mgr.hits == 1);
    CHECK(acc.rva == ext::ffxiv::kCurrentBuild.charaView.accessorRva && mgr.rva == ext::ffxiv::kCurrentBuild.charaView.managerPointerRva);
    CHECK(acc.agreement == Agreement::Agree && mgr.agreement == Agreement::Agree);
    CHECK(rb.build.charaView.accessorRva == 0x26abd0 && rb.build.charaView.managerPointerRva == 0x290e490);
    CHECK(rb.timeDateStamp == kStampC && rb.sizeOfImage == kSizeC && rb.knownBuild);
    REQUIRE(!log.lines.empty());
    CHECK(log.lines[0].second.rfind("[ffxiv-resolve] chara.accessor Found rva=0x26abd0 hits=1 table=0x26abd0 agree ms=", 0) == 0);
    CHECK(log.lines[1].second.rfind("[ffxiv-resolve] chara.managerPointer Found rva=0x290e490 hits=1 table=0x290e490 agree ms=", 0) == 0);

    // the golden comparison on committed bytes: the entry the resolver writes equals the table text for the bound fields
    CHECK(FormatEntry(rb, reg) == FormatTableEntry(ext::ffxiv::kCurrentBuild, reg));

    // and the existing scan in CharaTable.h finds the same two RVAs on the same image
    const auto old = ext::ffxiv::chara::table::ResolveFromImage(ReaderOf(*img), kBase, ext::ffxiv::kCurrentBuild.charaView);
    CHECK(old.state == ext::ffxiv::chara::table::AccessorState::Ready);
    CHECK(old.accessorRva == acc.rva && old.managerPointerRva == mgr.rva);
}

void TestOldBuildB()
{
    const Registry reg = BuildRegistry();
    auto img = MakeBImage();
    REQUIRE(img != nullptr);
    CHECK(PlantBRow(*img, kCharaAccessorB));

    const ResolvedBuild rb = ResolveImage(*img, reg);
    const SeamResult& acc = Get(rb, "chara.accessor");
    const SeamResult& mgr = Get(rb, "chara.managerPointer");
    CHECK(acc.status == SeamStatus::Found && acc.rva == kCharaAccessorB.rva && acc.rva == 0x26ab40);
    CHECK(mgr.status == SeamStatus::Found && mgr.rva == kCharaManagerB && mgr.rva == 0x28f9490); // the decoded global, not only the match
    CHECK(rb.timeDateStamp == kStampB && rb.sizeOfImage == kSizeB && !rb.knownBuild);
    CHECK(rb.build.charaView.accessorRva == 0x26ab40 && rb.build.charaView.managerPointerRva == 0x28f9490);
    // the B-to-C shift the notes record: +0x90 for the code, +0x15000 for the global
    CHECK(0x26abd0 - acc.rva == 0x90 && 0x290e490 - mgr.rva == 0x15000);

    // the existing scan agrees when it is given B's documented values
    ext::ffxiv::CharaViewBuild cvB;
    cvB.accessorRva = 0x26ab40;
    cvB.managerPointerRva = 0x28f9490;
    const auto old = ext::ffxiv::chara::table::ResolveFromImage(ReaderOf(*img), kBase, cvB);
    CHECK(old.state == ext::ffxiv::chara::table::AccessorState::Ready && old.accessorRva == acc.rva && old.managerPointerRva == mgr.rva);

    // exactly one raw match of the pattern in this image. B's .text is zero except the 18 planted bytes (a sparse B image cannot prove
    // uniqueness on the real build): the uniqueness on the real executable is the golden test's and the margin gate's job
    CHECK(acc.anchors[0].patternHits == 1);
}

void TestNegativeControls()
{
    const Registry reg = BuildRegistry();

    // B's accessor bytes in a C-layout image: the decoded global 0x28f9490 lies in C's INITIALIZED data (C's bss starts at 0x2907800), so
    // the TargetIn(Bss) predicate rejects the match. A wrong-build accessor is NotFound, not "found at the wrong place".
    auto c = MakeCImage();
    REQUIRE(c != nullptr);
    CHECK(PlantRow(*c, kCharaAccessorB));
    ResolvedBuild rb = ResolveImage(*c, reg);
    CHECK(Get(rb, "chara.accessor").status == SeamStatus::NotFound && Get(rb, "chara.accessor").anchors[0].patternHits == 1);
    CHECK(Get(rb, "chara.accessor").anchors[0].candidates == 0);
    CHECK(Get(rb, "chara.managerPointer").status == SeamStatus::NotFound);

    // C's bytes at their C address in a B-layout image: the global 0x290e490 is past B's data (0x289c000 + 0x6e38a0 = 0x2f7f8a0 holds it as
    // bss there, so this one IS accepted: the predicate is a section test, not a build test). It resolves to the C values, which differ
    // from the B values; only the known-build cross-check tells builds apart.
    auto b = MakeBImage();
    REQUIRE(b != nullptr);
    CHECK(PlantRow(*b, kCharaAccessorC));
    ext::ffxiv::GameBuild bTable;
    bTable.timeDateStamp = kStampB; // the table IS build B: identity gate (stamp and size both equal the image's)
    bTable.sizeOfImage = kSizeB;
    bTable.charaView.accessorRva = 0x26ab40;
    bTable.charaView.managerPointerRva = 0x28f9490;
    ResolveOptions opts;
    opts.knownTable = &bTable;
    rb = ResolveImage(*b, reg, opts);
    CHECK(rb.knownBuild);
    CHECK(Get(rb, "chara.accessor").status == SeamStatus::Found && Get(rb, "chara.accessor").rva == 0x26abd0);
    CHECK(Get(rb, "chara.accessor").agreement == Agreement::Disagree && rb.build.charaView.accessorRva == 0x26ab40); // the table wins

    // H1: a table that is NOT this image's build (here: build C's stamp and size on the B image, the old test's setup) is ignored: the
    // correct resolution is not overwritten by stale values, and there is no cross-check at all
    ext::ffxiv::GameBuild cTable = bTable;
    cTable.timeDateStamp = kStampC;
    cTable.sizeOfImage = kSizeC;
    cTable.charaView.accessorRva = 0x26abd0;
    cTable.charaView.managerPointerRva = 0x290e490;
    auto bReal = MakeBImage();
    REQUIRE(bReal != nullptr);
    CHECK(PlantBRow(*bReal, kCharaAccessorB));
    opts.knownTable = &cTable;
    rb = ResolveImage(*bReal, reg, opts);
    CHECK(!rb.knownBuild);
    CHECK(Get(rb, "chara.accessor").status == SeamStatus::Found && Get(rb, "chara.accessor").rva == 0x26ab40);
    CHECK(Get(rb, "chara.accessor").agreement == Agreement::NotChecked && !Get(rb, "chara.accessor").hasTable);
    CHECK(rb.build.charaView.accessorRva == 0x26ab40 && rb.build.charaView.managerPointerRva == 0x28f9490); // B's values, never C's
    opts.knownTable = &bTable;

    // nothing planted: nothing found, a distinct status
    auto empty = MakeBImage();
    rb = ResolveImage(*empty, reg);
    CHECK(Get(rb, "chara.accessor").status == SeamStatus::NotFound && Get(rb, "chara.managerPointer").status == SeamStatus::NotFound);
    CHECK(Get(rb, "chara.managerPointer").anchors[0].outcome == AnchorOutcome::DependencyMissing);
    CHECK(rb.build.charaView.accessorRva == 0 && rb.build.charaView.managerPointerRva == 0);

    // a decoy: a second copy of the accessor that loads a .rdata constant. With the Bss predicate the real one wins.
    auto d = MakeBImage();
    CHECK(PlantBRow(*d, kCharaAccessorB));
    const std::uint32_t decoyAt = 0x300000;
    const std::int32_t disp = static_cast<std::int32_t>(0x2150000 - (decoyAt + 7)); // a .rdata address
    std::vector<std::uint8_t> bytes = FromHex("488b05");
    bytes.insert(bytes.end(), reinterpret_cast<const std::uint8_t*>(&disp), reinterpret_cast<const std::uint8_t*>(&disp) + 4);
    const auto tail = FromHex("8bca488b84c8c8020000c3");
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    CHECK(d->Put(decoyAt, bytes));
    rb = ResolveImage(*d, reg);
    CHECK(Get(rb, "chara.accessor").status == SeamStatus::Found && Get(rb, "chara.accessor").rva == 0x26ab40);
    CHECK(Get(rb, "chara.accessor").anchors[0].patternHits == 2 && Get(rb, "chara.accessor").anchors[0].candidates == 1);
    // the same decoy WITHOUT the predicate is ambiguous: the predicate is what makes it resolve
    Registry noPred = reg;
    for (SeamSpec& s : noPred.seams)
    {
        if (std::string_view(s.id) == "chara.accessor")
            s.anchors[0].predicates.clear();
    }
    rb = ResolveImage(*d, noPred);
    CHECK(Get(rb, "chara.accessor").status == SeamStatus::Ambiguous && Get(rb, "chara.accessor").hits == 2);
}

void TestFixtureRules()
{
    auto img = MakeImage();
    REQUIRE(img != nullptr);
    auto at = [&](std::uint32_t rva)
    {
        std::uint8_t v = 0;
        img->Read(rva, &v, 1);
        return v;
    };

    // maskB: the unknown byte is a 00 placeholder and must NOT be planted
    const Row partial = { "x.partial", 0x2000, "aa00bbcc", "ff00ffff", "B-read-partial" };
    CHECK(PlantBRow(*img, partial));
    CHECK(at(0x2000) == 0xaa && at(0x2002) == 0xbb && at(0x2003) == 0xcc);
    CHECK(at(0x2001) == 0x90 + (0x2001 * 7) % 5); // still the filler: nothing was planted over the unknown byte

    // only B-read and B-read-partial are evidence about B
    const char* refused[] = { "derived-from-C", "derived-from-B-RVAs", "B-rva-only", "B-rva-by-rip-arithmetic", "B-not-recorded", "C-exe", "" };
    for (const char* conf : refused)
    {
        const Row r = { "x.derived", 0x2100, "deadbeef", "", conf };
        CHECK(!PlantBRow(*img, r));
        CHECK(at(0x2100) == 0x90 + (0x2100 * 7) % 5); // nothing planted
    }
    // a malformed mask is refused as well
    const Row bad = { "x.bad", 0x2200, "aabbcc", "ffff", "B-read" };
    CHECK(!PlantBRow(*img, bad));
    CHECK(PlantBRow(*img, { "x.ok", 0x2200, "aabbcc", "", "B-read" }) && at(0x2200) == 0xaa);

    // the recorded CharaView row is evidence, and complete (no mask)
    CHECK(std::string(kCharaAccessorB.confidence) == "B-read" && std::string(kCharaAccessorB.mask).empty() && FromHex(kCharaAccessorB.bytes).size() == 18);
}
} // namespace

int main()
{
    TestSpecEqualsTable();
    TestCurrentBuild();
    TestOldBuildB();
    TestNegativeControls();
    TestFixtureRules();
    return TEST_RESULT("resolve_chara_b_fixture");
}
