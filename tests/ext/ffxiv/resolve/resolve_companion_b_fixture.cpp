// Previous-build (B, PE stamp 0x6ac3dc96) evidence for the companion group, from "B-read" rows only (old_build_b_companion.h). What B has and
// what it does not:
//   * enqueue, bindState: the 16-byte guard is recorded, and it IS the first anchor of each seam (entry). Planted at the B RVA it resolves the B
//     function: 0x240bc0 and 0x22d7f0 (C minus 0x40 and 0xa0). The other anchors need bytes B never recorded, so the seams are single-anchor
//     there (a WARN), which is exactly what the policy says.
//   * layout slot table: the 72 bytes are recorded and are the .rdata content anchor: B 0x217bc00 (C minus 0xb60).
//   * batch16 / batch32: only the shared 16-byte prologue is recorded. No anchor of the batch seams is that short on purpose (the prologue
//     alone hits two sites and sits one byte from another function), so the B image resolves nothing for them; what B CAN prove is that the
//     first 16 bytes of the strict anchors equal the recorded B prologue (the part of the pattern B can confirm).
//   * everything else (dispatcher call, enqueue tail, bindState body, all Offset seams, the index-buffer map) and the nameplate module global
//     (an RVA only, medium confidence, bss) have no B bytes: single-build evidence, said plainly in the report.
// A C anchor is never planted at a B RVA (the 0x25f610 reuse trap): every B row goes to its own B address.
#include "resolve_companion_bytes.h"
#include "old_build_b_companion.h"

#include <cstdio>
#include <cstring>

using namespace rt;
using namespace oldb;
using namespace cb;

namespace
{
const AnchorResult& AnchorOf(const ResolvedBuild& rb, const std::string& seam, const std::string& anchor)
{
    static const AnchorResult missing;
    const SeamResult& s = Get(rb, seam);
    for (const AnchorResult& a : s.anchors)
    {
        if (a.name == anchor)
            return a;
    }
    std::printf("FAIL: seam %s has no anchor %s\n", seam.c_str(), anchor.c_str());
    ++g_failures;
    return missing;
}

ResolvedBuild ResolveB(const DenseImage& img) { return ResolveImage(img, CompanionRegistry()); }

// the first `n` bytes of a pattern text as a Pattern
Pattern Prefix(const std::string& text, std::size_t n)
{
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : text + " ")
    {
        if (c == ' ')
        {
            if (!cur.empty())
                tokens.push_back(cur);
            cur.clear();
        }
        else
            cur.push_back(c);
    }
    std::string joined;
    for (std::size_t i = 0; i < n && i < tokens.size(); ++i)
        joined += (i ? " " : "") + tokens[i];
    Pattern p;
    ParsePattern(joined, p);
    return p;
}

void TestFixtureRules()
{
    // the rows are B-read, except the nameplate global which is an RVA only and is refused by PlantBRow
    CHECK(std::string(kBatch16B.confidence) == "B-read" && std::string(kLayoutTableB.confidence) == "B-read");
    auto img = MakeBImage();
    REQUIRE(img != nullptr);
    CHECK(PlantBRow(*img, kEnqueueB) && PlantBRow(*img, kBindStateB) && PlantBRow(*img, kBatch16B) && PlantBRow(*img, kBatch32B) && PlantBRow(*img, kLayoutTableB));
    CHECK(!PlantBRow(*img, kNameplateGlobalB)); // nothing to plant: it was never read from the file
    // the layout table of B equals the C bytes (the fixture says the two are identical) and both are 72 bytes
    CHECK(std::string(kLayoutTableB.bytes) == std::string(kLayoutTable.hex) && std::string(kLayoutTableB.bytes).size() == 144);
    // the two batch rows are the same 16 bytes (one shared guard)
    CHECK(std::string(kBatch16B.bytes) == std::string(kBatch32B.bytes));
    // B-to-C shifts the fixtures document
    CHECK(0x6e56d0 - kBatch16B.rva == 0x160 && 0x6e6270 - kBatch32B.rva == 0x160);
    CHECK(kEnqueueB.rva - 0x240b80 == 0x40 && kBindStateB.rva - 0x22d750 == 0xa0);
    CHECK(0x217c760 - kLayoutTableB.rva == 0xb60);
}

void TestEnqueueAndBindStateOnB()
{
    auto img = MakeBImage();
    REQUIRE(img != nullptr);
    CHECK(PlantBRow(*img, kEnqueueB) && PlantBRow(*img, kBindStateB));
    const ResolvedBuild rb = ResolveB(*img);
    CHECK(!rb.knownBuild && rb.timeDateStamp == kStampB && rb.sizeOfImage == kSizeB);

    const SeamResult& enq = Get(rb, "companion.enqueue");
    CHECK(enq.status == SeamStatus::FoundSingleAnchor && enq.rva == 0x240bc0 && enq.rva == kEnqueueB.rva);
    CHECK(AnchorOf(rb, "companion.enqueue", "entry").outcome == AnchorOutcome::Unique && AnchorOf(rb, "companion.enqueue", "entry").match == 0x240bc0);
    CHECK(AnchorOf(rb, "companion.enqueue", "entryWriteTail").outcome == AnchorOutcome::Zero); // B never recorded the tail
    CHECK(rb.build.companion.enqueueRva == 0x240bc0);
    // the enqueue SHAPE is the 16-byte head, which B recorded in full: it verifies at the B entry (single-anchor location: the shape is capped too,
    // still usable) and fills the Guard with the recorded bytes, exactly as before the shape seams existed
    const SeamResult& enqShape = Get(rb, "companion.enqueueShape");
    CHECK(enqShape.status == SeamStatus::FoundSingleAnchor && enqShape.viaWeakDependency && enqShape.rva == 0x240bc0 && rb.Usable("companion.enqueueShape"));
    CHECK(ToHex(std::vector<std::uint8_t>(rb.build.companion.enqueueInstructions.begin(), rb.build.companion.enqueueInstructions.end())) == kEnqueueB.bytes);

    const SeamResult& bind = Get(rb, "companion.bindState");
    CHECK(bind.status == SeamStatus::FoundSingleAnchor && bind.rva == 0x22d7f0 && bind.rva == kBindStateB.rva);
    CHECK(AnchorOf(rb, "companion.bindState", "entry").outcome == AnchorOutcome::Unique && AnchorOf(rb, "companion.bindState", "entry").match == 0x22d7f0);
    CHECK(AnchorOf(rb, "companion.bindState", "argumentMapping").outcome == AnchorOutcome::Zero);
    CHECK(AnchorOf(rb, "companion.bindState", "contextCall").outcome == AnchorOutcome::Zero);
    CHECK(rb.build.companion.bindStateRva == 0x22d7f0);
    // the bindState SHAPE is the 47-byte entry plus argument mapping; B recorded only the first 16: the shape cannot be verified on B, so the
    // Guard stays ZERO (the previous build is not hookable by this resolver: stated, not hidden; the prefix evidence is TestBatchPrologueOnB)
    CHECK(Get(rb, "companion.bindStateShape").status == SeamStatus::NotFound && !rb.Usable("companion.bindStateShape"));
    for (std::uint8_t v : rb.build.companion.bindStateInstructions)
        CHECK(v == 0);

    // the C bytes are NOT at the B addresses of the other build and nothing else was planted: the batch, table and global seams find nothing
    CHECK(Get(rb, "companion.batch16").status == SeamStatus::NotFound && Get(rb, "companion.layoutSlotTable").status == SeamStatus::NotFound);
    CHECK(Get(rb, "companion.nameplateModulePointer").status == SeamStatus::NotFound);
}

void TestLayoutTableOnB()
{
    auto img = MakeBImage();
    REQUIRE(img != nullptr);
    CHECK(PlantBRow(*img, kLayoutTableB));
    const ResolvedBuild rb = ResolveB(*img);
    const SeamResult& t = Get(rb, "companion.layoutSlotTable");
    CHECK(t.status == SeamStatus::FoundSingleAnchor && t.rva == 0x217bc00 && t.rva == kLayoutTableB.rva);
    CHECK(AnchorOf(rb, "companion.layoutSlotTable", "content").outcome == AnchorOutcome::Unique);
    CHECK(AnchorOf(rb, "companion.layoutSlotTable", "inBatchA").outcome == AnchorOutcome::Zero); // the lookup code of B was not recorded
    CHECK(rb.build.companion.layoutSlotTableRva == 0x217bc00);

    // the same bytes in B's WRITABLE data are not read-only content
    auto data = MakeBImage();
    REQUIRE(data != nullptr);
    Row inData = kLayoutTableB;
    inData.rva = 0x289d000; // inside B's .data, initialized part
    CHECK(PlantBRow(*data, inData));
    CHECK(Get(ResolveB(*data), "companion.layoutSlotTable").status == SeamStatus::NotFound);
}

// The batch functions: B recorded only the 16-byte prologue. It is the first 16 bytes of both strict anchors, and of the enqueue / bindState entry
// anchors it is the whole pattern. No anchor of the batch seams accepts the prologue alone, so B resolves nothing for them (stated, not hidden).
void TestBatchPrologueOnB()
{
    const Registry reg = CompanionRegistry();
    const SeamSpec* b16 = FindSpec(reg, "companion.batch16");
    const SeamSpec* b32 = FindSpec(reg, "companion.batch32");
    REQUIRE(b16 != nullptr && b32 != nullptr);
    const std::vector<std::uint8_t> bytes = FromHex(kBatch16B.bytes);
    REQUIRE(bytes.size() == 16);
    for (const SeamSpec* s : { b16, b32 })
    {
        const Pattern p = Prefix(s->anchors[0].pattern, 16);
        REQUIRE(p.size() == 16);
        CHECK(p.fixedBytes() == 16 && MatchAt(bytes.data(), p)); // the recorded B bytes satisfy the first 16 bytes of the strict anchor
    }
    // enqueue and bindState: the recorded 16 bytes are the whole pattern of the entry anchor
    const SeamSpec* enq = FindSpec(reg, "companion.enqueue");
    const SeamSpec* bind = FindSpec(reg, "companion.bindState");
    REQUIRE(enq != nullptr && bind != nullptr);
    const auto enqBytes = FromHex(kEnqueueB.bytes);
    const auto bindBytes = FromHex(kBindStateB.bytes);
    const Pattern pe = Prefix(enq->anchors[0].pattern, 16), pb = Prefix(bind->anchors[0].pattern, 16);
    CHECK(pe.size() == 16 && MatchAt(enqBytes.data(), pe) && pb.size() == 16 && MatchAt(bindBytes.data(), pb));
    // and the argument-mapping anchor of bindState starts with the same 16 bytes
    CHECK(MatchAt(bindBytes.data(), Prefix(bind->anchors[1].pattern, 16)));

    auto img = MakeBImage();
    REQUIRE(img != nullptr);
    CHECK(PlantBRow(*img, kBatch16B) && PlantBRow(*img, kBatch32B));
    const ResolvedBuild rb = ResolveB(*img);
    CHECK(Get(rb, "companion.batch16").status == SeamStatus::NotFound && Get(rb, "companion.batch32").status == SeamStatus::NotFound);
    CHECK(rb.build.companion.batch16Rva == 0 && rb.build.companion.batch32Rva == 0);
}

// The C functions are not found at the B addresses and the B bytes are not found at the C addresses: planting a C row at a B RVA (the reuse
// trap) is not what this test does; it shows the two layouts are distinguishable by the cross-check alone (a known table of the OTHER build).
void TestOtherBuildTableIsIgnored()
{
    auto img = MakeBImage();
    REQUIRE(img != nullptr);
    CHECK(PlantBRow(*img, kEnqueueB));
    ResolveOptions opts;
    opts.knownTable = &ext::ffxiv::kCurrentBuild; // build C's table on the B image: ignored (stamp and size differ)
    const ResolvedBuild rb = ResolveImage(*img, CompanionRegistry(), opts);
    CHECK(!rb.knownBuild);
    CHECK(Get(rb, "companion.enqueue").rva == 0x240bc0 && Get(rb, "companion.enqueue").agreement == Agreement::NotChecked);
    CHECK(rb.build.companion.enqueueRva == 0x240bc0 && rb.build.companion.batch16Rva == 0); // B's value, never C's
}
} // namespace

int main()
{
    TestFixtureRules();
    TestEnqueueAndBindStateOnB();
    TestLayoutTableOnB();
    TestBatchPrologueOnB();
    TestOtherBuildTableIsIgnored();
    return TEST_RESULT("resolve_companion_b_fixture");
}
