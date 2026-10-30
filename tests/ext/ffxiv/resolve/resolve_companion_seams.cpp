// The companion seams on committed bytes of build C (no executable needed): resolve, golden entry, every predicate against a PLANTED decoy,
// the two batch functions told apart (collapse, role confusion and an address-order swap), EntryPatched on every function seam, the
// single-anchor degrade paths and the unknown-build behaviour. Real-executable twins: resolve_golden_exe and ffxiv_resolve_cli loop over
// BuildRegistry().
//
// What each test proves is named in its comment.
#include "resolve_companion_bytes.h"

#include <cstdio>
#include <cstring>

using namespace rt;
using namespace oldb;
using namespace cb;

namespace
{
constexpr std::uint32_t kBatch16Rva = 0x6e56d0, kBatch32Rva = 0x6e6270, kEnqueueRva = 0x240b80, kBindStateRva = 0x22d750, kIndexMapRva = 0x21eb50;
constexpr std::uint32_t kDispatcherRva = 0x63b6f0;
constexpr std::uint32_t kRdataTarget = 0x210e000; // a call target in .rdata: not code
constexpr std::uint32_t kDataTarget = 0x28b2000;  // inside the INITIALIZED part of .data: not bss

std::string SliceHex(const Blob& blob, std::uint32_t rva, std::size_t len)
{
    return std::string(blob.hex).substr(static_cast<std::size_t>(rva - blob.rva) * 2, len * 2);
}

// the dispatcher with both of its calls re-aimed: the first call (index buffer type 2) is the 32-bit batch, the second the 16-bit one
void PlantDispatcher(DenseImage& img, std::uint32_t target32, std::uint32_t target16)
{
    PlantBlob(img, kDispatcher);
    Plant(img, kDispatcherRva + 21, Rel32Hex(kDispatcherRva + 20, target32));
    Plant(img, kDispatcherRva + 28, Rel32Hex(kDispatcherRva + 27, target16));
}

// `mov rax,[rip+disp32]` at `insn` (7 bytes) re-aimed at `target`
void PlantDisp32(DenseImage& img, std::uint32_t insn, std::uint32_t target)
{
    const std::int32_t disp = static_cast<std::int32_t>(target - (insn + 7));
    std::vector<std::uint8_t> d(4);
    std::memcpy(d.data(), &disp, 4);
    Plant(img, insn + 3, ToHex(d));
}

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

bool Contains(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

ResolvedBuild Resolve(const DenseImage& img, const GameBuild* table = nullptr, LogCapture* log = nullptr)
{
    ResolveOptions opts;
    opts.knownTable = table;
    if (log != nullptr)
        opts.sink = log->Sink();
    return ResolveImage(img, CompanionRegistry(), opts);
}

std::unique_ptr<DenseImage> NoFunctions() { return MakeImageWithFunctions({}); }

template <class A>
bool AllZero(const A& a)
{
    for (auto v : a)
    {
        if (v != 0)
            return false;
    }
    return true;
}

template <class A>
bool EqualsBlob(const A& guard, const Blob& blob, std::uint32_t rva)
{
    return ToHex(std::vector<std::uint8_t>(guard.begin(), guard.end())) == SliceHex(blob, rva, guard.size());
}

// The hook-site rule of the header: a Detour site is usable only when its LOCATION seam and its SHAPE seam are both usable (UsableOrTable: on a known
// build the location may be filled from the table, the shape never is: it is failClosed).
bool SiteUsable(const ResolvedBuild& rb, const char* location, const char* shape) { return rb.UsableOrTable(location) && rb.UsableOrTable(shape); }

// ---------------------------------------------------------------------------------------------------------------------------------------
// 1. the current build: every seam Found at the table value, every anchor Unique, the golden entry equals the table, the guards equal the table
// ---------------------------------------------------------------------------------------------------------------------------------------
void TestCurrentBuild()
{
    const Registry reg = CompanionRegistry();
    REQUIRE(ValidateRegistry(reg).empty());
    auto img = MakeRealImage();
    REQUIRE(img != nullptr);
    const GameBuild& table = ext::ffxiv::kCurrentBuild;

    LogCapture log;
    const ResolvedBuild withTable = Resolve(*img, &table, &log);
    CHECK(withTable.knownBuild && withTable.registryErrors.empty());
    CHECK(withTable.seams.size() == 28); // 24 location / helper / offset seams + the four shape seams (the two-tier design)
    for (const SeamResult& s : withTable.seams)
    {
        if (s.status != SeamStatus::Found)
            std::printf("FAIL: seam %s is %s: %s\n", s.id.c_str(), StatusName(s.status), s.reason.c_str());
        CHECK(s.status == SeamStatus::Found);
        CHECK(s.agreement == Agreement::Agree || !s.hasTable);
        CHECK(!s.fellBackToTable);
        for (const AnchorResult& a : s.anchors)
        {
            if (a.outcome != AnchorOutcome::Unique)
                std::printf("FAIL: %s/%s outcome %d: %s\n", s.id.c_str(), a.name.c_str(), static_cast<int>(a.outcome), a.note.c_str());
            CHECK(a.outcome == AnchorOutcome::Unique);
        }
    }

    // the pure pattern-search answer (no table): the golden comparison and the per-field comparison
    const ResolvedBuild pure = Resolve(*img);
    CHECK(!pure.knownBuild);
    for (const SeamResult& s : pure.seams)
        CHECK(s.status == SeamStatus::Found);
    CHECK(FormatEntry(pure, reg) == FormatTableEntry(table, reg));
    for (const Binding& b : reg.bindings)
    {
        const FieldRow* row = FindField(b.key);
        REQUIRE(row != nullptr);
        if (row->kind == FieldKind::Guard)
            CHECK(std::memcmp(FieldBytes(pure.build, *row), FieldBytes(table, *row), row->size) == 0);
        else
            CHECK(*FieldU32(pure.build, *row) == *FieldU32(table, *row));
    }

    // every Rva and Guard field of the companion group is filled by a seam (nothing is left to the table on an unknown build)
    for (const FieldRow& row : kFieldTable)
    {
        if (std::string(row.group) != "companion" || (row.kind != FieldKind::Rva && row.kind != FieldKind::Guard))
            continue;
        if (row.kind == FieldKind::Rva)
            CHECK(*FieldU32(pure.build, row) != 0);
        else
        {
            bool any = false;
            for (std::size_t i = 0; i < row.size; ++i)
                any = any || FieldBytes(pure.build, row)[i] != 0;
            CHECK(any);
        }
    }

    // explicit values (literals, so that a change of the table alone cannot move them)
    CHECK(Get(pure, "companion.batch16").rva == 0x6e56d0 && Get(pure, "companion.batch32").rva == 0x6e6270);
    CHECK(Get(pure, "companion.enqueue").rva == 0x240b80 && Get(pure, "companion.bindState").rva == 0x22d750);
    CHECK(Get(pure, "companion.nameplateModulePointer").rva == 0x2ab8650 && Get(pure, "companion.layoutSlotTable").rva == 0x217c760);
    CHECK(Get(pure, "companion.indexBufferMap").rva == 0x21eb50);
    // the shape seams: Found at the address of their location seam, and they are the seams that fill the Guard fields
    CHECK(Get(pure, "companion.batch16Shape").rva == 0x6e56d0 && Get(pure, "companion.batch32Shape").rva == 0x6e6270);
    CHECK(Get(pure, "companion.enqueueShape").rva == 0x240b80 && Get(pure, "companion.bindStateShape").rva == 0x22d750);
    CHECK(EqualsBlob(pure.build.companion.batchInstructions, kBatch16, 0x6e56d0) && EqualsBlob(pure.build.companion.enqueueInstructions, kEnqueue, 0x240b80));
    CHECK(EqualsBlob(pure.build.companion.bindStateInstructions, kBindState, 0x22d750));
    const struct
    {
        const char* name;
        std::uint32_t value;
    } offsets[] = { { "rendererBatchListOffset", 0x580 },        { "rendererBatchCountOffset", 0x588 }, { "rendererIndexBufferOffset", 0x1e0 },
                    { "rendererVertexAllocationOffset", 0x570 }, { "rendererVertexStrideOffset", 0x500 }, { "rendererVertexOffsetOffset", 0x504 },
                    { "batchFormatOffset", 0x30 },               { "batchExtensionOffset", 0x04 },       { "drawStateFlagOffset", 0x49 },
                    { "nativeContextOffset", 0x17b8 },           { "moduleStatePointerOffset", 8 },      { "stateQueuePointerOffset", 8 },
                    { "indexBufferCapacityOffset", 0x38 },       { "indexBufferFlagsOffset", 0x40 },     { "indexBufferCpuStartOffset", 0x60 },
                    { "indexBufferCpuEndOffset", 0x68 },         { "entryPointerOffset", 8 } };
    for (const auto& o : offsets)
        CHECK(Get(pure, std::string("companion.") + o.name).rva == o.value);

    // the log of the run that had the table: one Found line per seam
    std::size_t found = 0;
    for (const auto& l : log.lines)
        found += l.second.find(" Found rva=") != std::string::npos ? 1 : 0;
    CHECK(found == 28);

    // hit structure on the real bytes: SameFunctionAs is what makes the patterns that occur in both batch functions unique
    CHECK(AnchorOf(pure, "companion.rendererBatchListOffset", "inBatch16").patternHits == 2);
    CHECK(AnchorOf(pure, "companion.rendererBatchListOffset", "inBatch16").candidates == 1);
    CHECK(AnchorOf(pure, "companion.rendererBatchListOffset", "inBatch16").match == 0x6e56ee);
    CHECK(AnchorOf(pure, "companion.rendererBatchListOffset", "inBatch32").match == 0x6e628e);
    CHECK(AnchorOf(pure, "companion.layoutSlotTable", "inBatchA").candidates == 2 && AnchorOf(pure, "companion.layoutSlotTable", "inBatchA").match == 0x6e5a10);
    CHECK(AnchorOf(pure, "companion.layoutSlotTable", "inBatchB").match == 0x6e65b2);
    CHECK(AnchorOf(pure, "companion.layoutSlotTable", "content").match == 0x217c760);
    CHECK(AnchorOf(pure, "companion.enqueue", "entryWriteTail").match == 0x240c1b);
    CHECK(AnchorOf(pure, "companion.enqueue", "callerEpilogue").match == 0x23c0d2 && AnchorOf(pure, "companion.enqueue", "callerEpilogue").value == 0x240b80);
    CHECK(AnchorOf(pure, "companion.bindState", "contextCall").match == 0x22d8b4);
    CHECK(AnchorOf(pure, "companion.batch16", "dispatcherCall").match == 0x63b6f0);
    CHECK(AnchorOf(pure, "companion.batch16", "dispatcherStub").match == 0x63b730 && AnchorOf(pure, "companion.batch16", "dispatcherStub").value == 0x6e56d0);
    CHECK(Get(pure, "companion.batch16").hits == 3 && Get(pure, "companion.batch32").hits == 2);

    // every batch Offset anchor sits in ITS function: inBatch16 in [0x6e56d0, 0x6e5a50), inBatch32 in [0x6e6270, 0x6e6600) (a tie to the wrong
    // function would still decode the same number, so the position is what proves the tie)
    for (const char* seam : { "companion.rendererBatchListOffset", "companion.rendererBatchCountOffset", "companion.rendererIndexBufferOffset",
                              "companion.rendererVertexOffsetOffset", "companion.rendererVertexAllocationOffset", "companion.rendererVertexStrideOffset",
                              "companion.batchFormatOffset", "companion.batchExtensionOffset" })
    {
        const std::uint32_t m16 = AnchorOf(pure, seam, "inBatch16").match, m32 = AnchorOf(pure, seam, "inBatch32").match;
        if (!(m16 >= 0x6e56d0 && m16 < 0x6e5a50 && m32 >= 0x6e6270 && m32 < 0x6e6600))
            std::printf("FAIL: %s anchors sit at 0x%x / 0x%x\n", seam, m16, m32);
        CHECK(m16 >= 0x6e56d0 && m16 < 0x6e5a50 && m32 >= 0x6e6270 && m32 < 0x6e6600);
    }
    CHECK(AnchorOf(pure, "companion.entryPointerOffset", "listEntryInBatch16").match == 0x6e58b0);
    CHECK(AnchorOf(pure, "companion.entryPointerOffset", "listEntryInBatch32").match == 0x6e6450);
    CHECK(AnchorOf(pure, "companion.entryPointerOffset", "commandStore").match == 0x240c23);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// 2. the two batch functions are told apart by what they are, not by address order
// ---------------------------------------------------------------------------------------------------------------------------------------
// Both calls of the dispatcher point at the 16-bit function: batch32 would resolve (through the caller anchor) to batch16's address. The
// seam is declared in Registry::mustDiffer, so it becomes Conflict and fills no field; batch16 itself is fine.
void TestBatchCollapse()
{
    auto img = NoFunctions();
    REQUIRE(img != nullptr);
    PlantBlob(*img, kBatch16);
    PlantDispatcher(*img, kBatch16Rva, kBatch16Rva);
    const ResolvedBuild rb = Resolve(*img);
    CHECK(Get(rb, "companion.batch16").status == SeamStatus::Found && Get(rb, "companion.batch16").rva == kBatch16Rva);
    const SeamResult& b32 = Get(rb, "companion.batch32");
    CHECK(b32.status == SeamStatus::Conflict && !b32.hasValue && b32.rva == 0);
    CHECK(Contains(b32.reason, "must differ"));
    CHECK(rb.build.companion.batch16Rva == kBatch16Rva && rb.build.companion.batch32Rva == 0);
    // the caller anchor of batch32 did decode the same address: the guard is the group, not the anchor
    CHECK(AnchorOf(rb, "companion.batch32", "dispatcherCall").hasValue && AnchorOf(rb, "companion.batch32", "dispatcherCall").value == kBatch16Rva);
}

// The two bodies sit at swapped addresses (the 32-bit function BELOW the 16-bit one): address order would give the wrong assignment, the
// shift in the body and the dispatcher's roles give the right one, and both anchors of each seam agree.
void TestBatchSwap()
{
    auto img = NoFunctions();
    REQUIRE(img != nullptr);
    PlantBlob(*img, kBatch32, kBatch16Rva); // the 32-bit body at the LOWER address
    PlantBlob(*img, kBatch16, kBatch32Rva); // the 16-bit body at the HIGHER address
    PlantDispatcher(*img, kBatch16Rva /*32-bit role*/, kBatch32Rva /*16-bit role*/);
    const ResolvedBuild rb = Resolve(*img);
    const SeamResult& b16 = Get(rb, "companion.batch16");
    const SeamResult& b32 = Get(rb, "companion.batch32");
    CHECK(b16.status == SeamStatus::Found && b16.rva == kBatch32Rva);
    CHECK(b32.status == SeamStatus::Found && b32.rva == kBatch16Rva);
    CHECK(b16.rva > b32.rva); // the 16-bit function is the higher one here
    CHECK(AnchorOf(rb, "companion.batch16", "entryToIndexShift").value == kBatch32Rva && AnchorOf(rb, "companion.batch16", "dispatcherCall").value == kBatch32Rva);
    CHECK(AnchorOf(rb, "companion.batch32", "entryToIndexShift").value == kBatch16Rva && AnchorOf(rb, "companion.batch32", "dispatcherCall").value == kBatch16Rva);
}

// The dispatcher says the 16-bit role is the function whose body is the 32-bit one: the two anchors of the seam disagree and the seam is
// inert (no value, no field), instead of hooking the wrong function with the right-looking address.
void TestBatchRoleConfusion()
{
    auto img = NoFunctions();
    REQUIRE(img != nullptr);
    PlantBlob(*img, kBatch16);
    PlantBlob(*img, kBatch32);
    PlantDispatcher(*img, kBatch32Rva, kBatch32Rva); // the 16-bit call now points at the 32-bit body
    const ResolvedBuild rb = Resolve(*img);
    const SeamResult& b16 = Get(rb, "companion.batch16");
    CHECK(b16.status == SeamStatus::Conflict && !b16.hasValue && rb.build.companion.batch16Rva == 0);
    CHECK(Contains(b16.reason, "disagree"));
    CHECK(Get(rb, "companion.batch32").status == SeamStatus::Found && Get(rb, "companion.batch32").rva == kBatch32Rva);
}

// The inverted dispatcher and the 16-bit function's second caller. The 16-bit function has a second caller, the tail-call stub at 0x63b730 (jmp batch16);
// the 32-bit one has no second caller (its je rel32 cannot be decoded: Rel32 reads E8/E9 only, RipDisp32 demands a RIP ModRM byte).
void TestBatchInvertedDispatcher()
{
    // strict heads intact + the dispatcher's call order inverted (the 32-bit call aimed at batch16 and the other way round): every anchor of
    // batch16 disagrees with another, so does batch32's: Conflict for both, inert
    {
        auto img = MakeRealImage();
        REQUIRE(img != nullptr);
        PlantDispatcher(*img, kBatch16Rva, kBatch32Rva);
        const ResolvedBuild rb = Resolve(*img);
        for (const char* seam : { "companion.batch16", "companion.batch32" })
        {
            CHECK(Get(rb, seam).status == SeamStatus::Conflict && !Get(rb, seam).hasValue);
            CHECK(!SiteUsable(rb, seam, seam == std::string("companion.batch16") ? "companion.batch16Shape" : "companion.batch32Shape"));
        }
        CHECK(rb.build.companion.batch16Rva == 0 && rb.build.companion.batch32Rva == 0);
    }
    // B3: the dispatcher's `jne` is a `je` (the roles of its two calls are inverted by the compiler): the dispatcherCall pattern refuses, the strict
    // heads and the stub still agree: batch16 plain Found (two anchors), batch32 single-anchor at its own address
    {
        auto img = MakeRealImage();
        REQUIRE(img != nullptr);
        Plant(*img, kDispatcherRva + 18, "74");
        const ResolvedBuild rb = Resolve(*img);
        CHECK(AnchorOf(rb, "companion.batch16", "dispatcherCall").outcome == AnchorOutcome::Zero);
        CHECK(Get(rb, "companion.batch16").status == SeamStatus::Found && Get(rb, "companion.batch16").rva == kBatch16Rva);
        CHECK(Get(rb, "companion.batch32").status == SeamStatus::FoundSingleAnchor && Get(rb, "companion.batch32").rva == kBatch32Rva);
        CHECK(SiteUsable(rb, "companion.batch16", "companion.batch16Shape") && SiteUsable(rb, "companion.batch32", "companion.batch32Shape"));
    }
    // B1: BOTH strict heads broken AND the dispatcher's call order inverted (a swap needs two failures at once). The stub, which says where the
    // 16-bit function is, contradicts the inverted call: batch16 is a Conflict (no value). Without the stub both seams would be FoundSingleAnchor
    // and swapped. The two shape seams fail too (the heads are broken), so neither site is hookable.
    {
        auto img = MakeRealImage();
        REQUIRE(img != nullptr);
        img->Bytes()[kBatch16Rva + 27] ^= 0xff;
        img->Bytes()[kBatch32Rva + 27] ^= 0xff;
        PlantDispatcher(*img, kBatch16Rva, kBatch32Rva);
        for (const GameBuild* t : { static_cast<const GameBuild*>(nullptr), &ext::ffxiv::kCurrentBuild })
        {
            const ResolvedBuild rb = Resolve(*img, t);
            const SeamResult& b16 = Get(rb, "companion.batch16");
            const SeamResult& b32 = Get(rb, "companion.batch32");
            CHECK(AnchorOf(rb, "companion.batch16", "entryToIndexShift").outcome == AnchorOutcome::Zero);
            CHECK(b16.status == SeamStatus::Conflict && !b16.hasValue && Contains(b16.reason, "disagree") && Contains(b16.reason, "dispatcherStub"));
            // no silent swap: batch16 never holds the 32-bit function's address
            CHECK(!(b16.hasValue && b16.rva == kBatch32Rva));
            // unknown build: the field stays zero; known build: the table's value (verified offline for this executable) stands in, never batch32's
            CHECK(rb.build.companion.batch16Rva == (t == nullptr ? 0u : kBatch16Rva));
            // the residual, pinned: on an unknown build batch32's only caller anchor leaves batch32Rva at batch16's address (the known build's
            // cross-check puts its own value back). If batch32 ever gets a second caller anchor this line flips: change it on purpose.
            CHECK(rb.build.companion.batch32Rva == (t == nullptr ? kBatch16Rva : kBatch32Rva));
            // residual (reported): batch32's single caller anchor names batch16's address, and mustDiffer cannot compare with a seam that has no
            // value. What stops the hook is the shape seam (the strict 32-bit head is not at that address)
            CHECK(!rb.Usable("companion.batch16Shape") && !rb.Usable("companion.batch32Shape"));
            CHECK(AllZero(rb.build.companion.batchInstructions));
            CHECK(!SiteUsable(rb, "companion.batch16", "companion.batch16Shape") && !SiteUsable(rb, "companion.batch32", "companion.batch32Shape"));
            CHECK(b32.status != SeamStatus::Found); // never a two-anchor result
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// 3. every predicate removes its planted decoy
// ---------------------------------------------------------------------------------------------------------------------------------------
// One image with the real functions and decoy copies of the snippets in functions of their own (.pdata ranges), plus the enqueue tail far from
// enqueue. SameFunctionAs removes the decoys of the Offset seams (raw hits 3 -> 1), After removes the tail copy, TargetIn(Code) the HDR copy
// whose call points into .rdata. Without the predicates these anchors would be Multiple.
void TestDecoysRemoved()
{
    auto ranges = RealRanges();
    ranges.push_back({ 0x1000000, 0x1001000, -1 }); // decoy function 1: copies of the batch snippets
    ranges.push_back({ 0x1001000, 0x1002000, -1 }); // decoy function 2: a copy of the index-buffer map body
    ranges.push_back({ 0x1002000, 0x1003000, -1 }); // decoy function 3: copies of the bindState snippets
    auto img = MakeImageWithFunctions(ranges);
    REQUIRE(img != nullptr);
    PlantAll(*img);
    // decoy 1: HDR (47 bytes at 0x6e56ee; its call is re-aimed into .rdata), VERT (34 at 0x6e5863), EXT (19 at 0x6e58e9), FORMAT (28 at 0x6e5933),
    // the batch list entry read (25 at 0x6e58b0)
    Plant(*img, 0x1000000, SliceHex(kBatch16, 0x6e56ee, 47));
    Plant(*img, 0x1000000 + 43, Rel32Hex(0x1000000 + 42, kRdataTarget));
    Plant(*img, 0x1000100, SliceHex(kBatch16, 0x6e5863, 34));
    Plant(*img, 0x1000200, SliceHex(kBatch16, 0x6e58e9, 19));
    Plant(*img, 0x1000300, SliceHex(kBatch16, 0x6e5933, 28));
    Plant(*img, 0x1000400, SliceHex(kBatch16, 0x6e58b0, 25)); // the batch list entry read (25 bytes)
    // decoy 2: the index-buffer map body (94 bytes at 0x21eb5d)
    Plant(*img, 0x1001000, SliceHex(kIndexMap, 0x21eb5d, 94));
    // decoy 3: the bindState snippets (flag 39 at 0x22d758, context A 19 at 0x22d7d8, context B 26 at 0x22d8b4)
    Plant(*img, 0x1002000, SliceHex(kBindState, 0x22d758, 39));
    Plant(*img, 0x1002100, SliceHex(kBindState, 0x22d7d8, 19));
    Plant(*img, 0x1002200, SliceHex(kBindState, 0x22d8b4, 49)); // loadBeforeStateCall is the full 49-byte contextCall text
    // the enqueue tail copy, 0x1004000 is far beyond enqueue + 0x200
    Plant(*img, 0x1004000, SliceHex(kEnqueue, 0x240c23, 19));
    // a copy of the tail-call stub whose jmp points into .rdata (the je keeps its target): TargetIn(Code) is what removes it
    Plant(*img, 0x1004200, kDispatcherStub.hex);
    Plant(*img, 0x1004200 + 18, Rel32Hex(0x1004200 + 17, kRdataTarget));
    // a copy of the enqueue caller whose call points into .rdata: TargetIn(Code) is what removes it
    Plant(*img, 0x1004100, kEnqueueCaller.hex);
    Plant(*img, 0x1004100 + 11, Rel32Hex(0x1004100 + 10, kRdataTarget));

    const ResolvedBuild rb = Resolve(*img);
    for (const SeamResult& s : rb.seams)
    {
        if (s.status != SeamStatus::Found)
            std::printf("FAIL: seam %s is %s: %s\n", s.id.c_str(), StatusName(s.status), s.reason.c_str());
        CHECK(s.status == SeamStatus::Found);
    }
    auto expectDecoy = [&](const char* seam, const char* anchor, std::uint32_t hits, std::uint32_t candidates)
    {
        const AnchorResult& a = AnchorOf(rb, seam, anchor);
        if (a.patternHits != hits || a.candidates != candidates)
            std::printf("FAIL: %s/%s raw hits %u (want %u), candidates %u (want %u)\n", seam, anchor, a.patternHits, hits, a.candidates, candidates);
        CHECK(a.patternHits == hits && a.candidates == candidates && a.outcome == AnchorOutcome::Unique);
    };
    for (const char* seam : { "companion.rendererBatchListOffset", "companion.rendererBatchCountOffset", "companion.rendererIndexBufferOffset",
                              "companion.rendererVertexOffsetOffset", "companion.rendererVertexAllocationOffset", "companion.rendererVertexStrideOffset",
                              "companion.batchFormatOffset", "companion.batchExtensionOffset" })
    {
        expectDecoy(seam, "inBatch16", 3, 1);
        expectDecoy(seam, "inBatch32", 3, 1);
    }
    expectDecoy("companion.indexBufferCapacityOffset", "capacityLoad", 2, 1);
    expectDecoy("companion.indexBufferFlagsOffset", "flagsLoad", 2, 1);
    expectDecoy("companion.indexBufferCpuStartOffset", "startCompare", 2, 1);
    expectDecoy("companion.indexBufferCpuStartOffset", "startReturn", 2, 1);
    expectDecoy("companion.indexBufferCpuEndOffset", "endReturn", 2, 1);
    expectDecoy("companion.drawStateFlagOffset", "firstCompare", 2, 1);
    expectDecoy("companion.nativeContextOffset", "loadBeforeVirtualCall", 2, 1);
    expectDecoy("companion.nativeContextOffset", "loadBeforeStateCall", 2, 1);
    // the same text is the location anchor of bindState: it sees the copy in the decoy function as a second function (Multiple), the seam stays Found
    // because its other anchors are unique and the candidates of this one include their value
    {
        const AnchorResult& call = AnchorOf(rb, "companion.bindState", "contextCall");
        CHECK(call.patternHits == 2 && call.candidates == 2 && call.outcome == AnchorOutcome::Multiple);
        CHECK(Get(rb, "companion.bindState").status == SeamStatus::Found && Get(rb, "companion.bindState").rva == kBindStateRva);
    }
    expectDecoy("companion.entryPointerOffset", "commandStore", 2, 1);
    expectDecoy("companion.enqueue", "callerEpilogue", 2, 1);
    expectDecoy("companion.batch16", "dispatcherStub", 2, 1);
    expectDecoy("companion.entryPointerOffset", "listEntryInBatch16", 3, 1);
    expectDecoy("companion.entryPointerOffset", "listEntryInBatch32", 3, 1);
    // TargetIn(Code): the HDR copy contains the call to the index-buffer map with its target in .rdata: raw 3, candidates 2
    expectDecoy("companion.indexBufferMap", "calledFromBatchA", 3, 2);
    expectDecoy("companion.indexBufferMap", "calledFromBatchB", 3, 2);
    // the function-body anchor of that seam (no predicate: it IS the function finder) sees the copy as a second function: Multiple. The seam
    // stays Found because the other anchors are unique and the candidates of this one include their value (the multi-anchor policy).
    const AnchorResult& body = AnchorOf(rb, "companion.indexBufferMap", "flagLogic");
    CHECK(body.patternHits == 2 && body.candidates == 2 && body.outcome == AnchorOutcome::Multiple);
    CHECK(Get(rb, "companion.indexBufferMap").status == SeamStatus::Found && Get(rb, "companion.indexBufferMap").rva == kIndexMapRva);
}

// TargetIn(Code) on the dispatcher anchors: both calls point into .rdata: the pattern matches, the decoded target is not code.
void TestDispatcherTargetMustBeCode()
{
    auto img = NoFunctions();
    REQUIRE(img != nullptr);
    PlantDispatcher(*img, kRdataTarget, kRdataTarget + 0x100);
    const ResolvedBuild rb = Resolve(*img);
    for (const char* seam : { "companion.batch16", "companion.batch32" })
    {
        const AnchorResult& a = AnchorOf(rb, seam, "dispatcherCall");
        CHECK(a.patternHits == 1 && a.candidates == 0 && a.outcome == AnchorOutcome::Zero && !a.hasValue);
        CHECK(Contains(a.note, "required region"));
        CHECK(Get(rb, seam).status == SeamStatus::NotFound);
    }
}

// TargetIn(Bss) on the three chain sites: the global they load lies in the INITIALIZED part of .data. The pointer seam is NotFound and
// fills no field. The two +8 Offset seams read their byte from the same patterns and are not tied to the global (they stay Found): the
// report says so.
void TestNameplateGlobalMustBeBss()
{
    auto img = NoFunctions();
    REQUIRE(img != nullptr);
    const std::uint32_t sites[3] = { 0x67361c, 0x673696, 0x6a5324 };
    const Blob* blobs[3] = { &kChain1, &kChain2, &kChain3 };
    for (int i = 0; i < 3; ++i)
    {
        PlantBlob(*img, *blobs[i]);
        PlantDisp32(*img, sites[i], kDataTarget);
    }
    const ResolvedBuild rb = Resolve(*img);
    const SeamResult& g = Get(rb, "companion.nameplateModulePointer");
    CHECK(g.status == SeamStatus::NotFound && !g.hasValue && rb.build.companion.nameplateModulePointerRva == 0);
    for (const AnchorResult& a : g.anchors)
        CHECK(a.patternHits == 1 && a.candidates == 0 && Contains(a.note, "required region"));
    CHECK(Get(rb, "companion.moduleStatePointerOffset").status == SeamStatus::Found);
    // the same sites with the real displacement: Found, three anchors, the global
    auto good = NoFunctions();
    REQUIRE(good != nullptr);
    for (const Blob* b : blobs)
        PlantBlob(*good, *b);
    const ResolvedBuild ok = Resolve(*good);
    CHECK(Get(ok, "companion.nameplateModulePointer").status == SeamStatus::Found && Get(ok, "companion.nameplateModulePointer").rva == 0x2ab8650);
    CHECK(Get(ok, "companion.nameplateModulePointer").hits == 3);
}

// Only ONE of the three chain sites is present: the global is still found (FoundSingleAnchor, a WARN), never plain Found.
void TestNameplateSingleSite()
{
    auto img = NoFunctions();
    REQUIRE(img != nullptr);
    PlantBlob(*img, kChain2);
    const ResolvedBuild rb = Resolve(*img);
    const SeamResult& g = Get(rb, "companion.nameplateModulePointer");
    CHECK(g.status == SeamStatus::FoundSingleAnchor && g.rva == 0x2ab8650);
    CHECK(rb.build.companion.nameplateModulePointerRva == 0x2ab8650);
    // two sites that name DIFFERENT globals disagree: Conflict, inert
    PlantBlob(*img, kChain1);
    PlantDisp32(*img, 0x67361c, 0x2ab8660);
    const ResolvedBuild conflict = Resolve(*img);
    CHECK(Get(conflict, "companion.nameplateModulePointer").status == SeamStatus::Conflict);
    CHECK(conflict.build.companion.nameplateModulePointerRva == 0);
}

// ValueIn on an Offset anchor: batch16's `mov rax,[rcx+LIST]` displacement is changed to 0x100000 (outside the range). The decoy value is
// dropped by ValueIn (without it the two anchors would disagree: Conflict). The pinned head of the batch function fails too (the strict
// anchor has the old displacement fixed), so the function falls back to its two caller anchors (the dispatcher call and the tail-call stub),
// giving plain Found, two independent anchors; batch32 (one caller anchor) would be FoundSingleAnchor in the same situation.
void TestValueInOnOffsets()
{
    auto img = MakeRealImage();
    REQUIRE(img != nullptr);
    Plant(*img, 0x6e56ee + 3, "00001000");
    const ResolvedBuild rb = Resolve(*img);
    const AnchorResult& in16 = AnchorOf(rb, "companion.rendererBatchListOffset", "inBatch16");
    CHECK(in16.outcome == AnchorOutcome::Zero && in16.patternHits == 2 && in16.candidates == 0);
    CHECK(Contains(in16.note, "required range"));
    const SeamResult& list = Get(rb, "companion.rendererBatchListOffset");
    CHECK(list.status == SeamStatus::FoundSingleAnchor && list.rva == 0x580);
    // a changed struct offset in the pinned head: the strict anchor fails, the caller anchor still finds the function
    const SeamResult& b16 = Get(rb, "companion.batch16");
    CHECK(AnchorOf(rb, "companion.batch16", "entryToIndexShift").outcome == AnchorOutcome::Zero);
    CHECK(b16.status == SeamStatus::Found && b16.rva == kBatch16Rva && AnchorOf(rb, "companion.batch16", "dispatcherStub").outcome == AnchorOutcome::Unique);
    CHECK(Get(rb, "companion.batch32").status == SeamStatus::Found);
}

// ValueIn on the layout table anchors: batch16's table displacement is changed to 0x40 (not a plausible .rdata RVA). The decoded 0x40 is
// dropped; without ValueIn the anchor of batch16 would say 0x40 and disagree with the table content (Conflict).
void TestValueInOnLayoutTable()
{
    auto img = MakeRealImage();
    REQUIRE(img != nullptr);
    Plant(*img, 0x6e5a10 + 11, "40000000");
    const ResolvedBuild rb = Resolve(*img);
    const AnchorResult& a = AnchorOf(rb, "companion.layoutSlotTable", "inBatchA");
    CHECK(a.patternHits == 2 && a.candidates == 1 && a.outcome == AnchorOutcome::Multiple);
    CHECK(Contains(a.note, "required range"));
    const SeamResult& t = Get(rb, "companion.layoutSlotTable");
    CHECK(t.status == SeamStatus::FoundSingleAnchor && t.rva == 0x217c760);
}

// The healed values must stay inside the consumer's containers. Exact bounds, one byte changed in ONE copy of the anchor:
//   batchFormatOffset    the consumer reads 4 bytes of a 64-byte header: max 0x3c (0x3d would read past the array)
//   batchExtensionOffset the consumer reads 2 bytes of the same header: max 0x3e
//   entryPointerOffset   the consumer indexes entry[1] of a {key, pointer} pair (CompanionPackets.h Substitute): exactly 8
// Each case plants the boundary value (accepted: the anchor is Unique with that value) and the next one (refused: no candidate left; the
// accepted value next to it proves the refusal is the range, not the plant).
void TestValueInTightBounds()
{
    struct Case
    {
        const char* seam;
        const char* anchor;
        std::uint32_t at;          // RVA of the displacement byte in the committed C bytes
        std::uint32_t patternHits; // raw hits of the pattern (2 where the pattern occurs in both batch functions)
        int accepted[3];           // values that must be accepted (-1 = unused)
        int refused[4];            // values that must be refused (-1 = unused)
    };
    const Case cases[] = {
        { "companion.batchFormatOffset", "inBatch16", 0x6e5933 + 4, 2, { 0x3c, 1, -1 }, { 0x3d, 0x3f, 0, 0x40 } },
        { "companion.batchFormatOffset", "inBatch32", 0x6e64d3 + 4, 2, { 0x3c, 1, -1 }, { 0x3d, 0x3f, 0, 0x40 } },
        { "companion.batchExtensionOffset", "inBatch16", 0x6e58e9 + 5, 2, { 0x3e, 1, -1 }, { 0x3f, 0x40, 0, -1 } },
        { "companion.batchExtensionOffset", "inBatch32", 0x6e6489 + 5, 2, { 0x3e, 1, -1 }, { 0x3f, 0x40, 0, -1 } },
        { "companion.entryPointerOffset", "listEntryInBatch16", 0x6e58b0 + 11, 2, { 8, -1, -1 }, { 7, 9, 0x0f, 1 } },
        { "companion.entryPointerOffset", "listEntryInBatch32", 0x6e6450 + 11, 2, { 8, -1, -1 }, { 7, 9, 0x0f, 1 } },
        { "companion.entryPointerOffset", "commandStore", 0x240c23 + 17, 1, { 8, -1, -1 }, { 7, 9, 0x0f, 1 } },
    };
    char hex[3];
    for (const Case& c : cases)
    {
        for (int v : c.accepted)
        {
            if (v < 0)
                continue;
            auto img = MakeRealImage();
            REQUIRE(img != nullptr);
            std::snprintf(hex, sizeof(hex), "%02x", v);
            Plant(*img, c.at, hex);
            const ResolvedBuild rb = Resolve(*img);
            const AnchorResult& a = AnchorOf(rb, c.seam, c.anchor);
            if (!(a.outcome == AnchorOutcome::Unique && a.hasValue && a.value == static_cast<std::uint32_t>(v)))
                std::printf("FAIL: %s/%s should accept 0x%x (outcome %d: %s)\n", c.seam, c.anchor, v, static_cast<int>(a.outcome), a.note.c_str());
            CHECK(a.outcome == AnchorOutcome::Unique && a.hasValue && a.value == static_cast<std::uint32_t>(v));
        }
        for (int v : c.refused)
        {
            if (v < 0)
                continue;
            auto img = MakeRealImage();
            REQUIRE(img != nullptr);
            std::snprintf(hex, sizeof(hex), "%02x", v);
            Plant(*img, c.at, hex);
            const ResolvedBuild rb = Resolve(*img);
            const AnchorResult& a = AnchorOf(rb, c.seam, c.anchor);
            if (!(a.outcome == AnchorOutcome::Zero && !a.hasValue && a.candidates == 0 && a.patternHits == c.patternHits))
                std::printf("FAIL: %s/%s should refuse 0x%x (outcome %d, hits %u, candidates %u: %s)\n", c.seam, c.anchor, v, static_cast<int>(a.outcome),
                            a.patternHits, a.candidates, a.note.c_str());
            CHECK(a.outcome == AnchorOutcome::Zero && !a.hasValue && a.candidates == 0 && a.patternHits == c.patternHits);
        }
    }
}

// The table seam also resolves from its content alone (the previous build's only evidence is the content), as a single anchor.
void TestLayoutTableContentOnly()
{
    auto img = NoFunctions();
    REQUIRE(img != nullptr);
    PlantBlob(*img, kLayoutTable);
    const ResolvedBuild rb = Resolve(*img);
    const SeamResult& t = Get(rb, "companion.layoutSlotTable");
    CHECK(t.status == SeamStatus::FoundSingleAnchor && t.rva == 0x217c760 && rb.build.companion.layoutSlotTableRva == 0x217c760);
    // one changed entry (layout 5 -> slot 1): no match, nothing found, a distinct status
    auto bad = NoFunctions();
    REQUIRE(bad != nullptr);
    PlantBlob(*bad, kLayoutTable);
    Plant(*bad, 0x217c760 + 40, "01");
    CHECK(Get(Resolve(*bad), "companion.layoutSlotTable").status == SeamStatus::NotFound);
    // the table in a WRITABLE section is not .rdata content: the anchor searches read-only data only
    auto data = NoFunctions();
    REQUIRE(data != nullptr);
    PlantBlob(*data, kLayoutTable, 0x28b2000);
    CHECK(Get(Resolve(*data), "companion.layoutSlotTable").status == SeamStatus::NotFound);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// 4. EntryPatched: a foreign jump over the entry of each function seam
// ---------------------------------------------------------------------------------------------------------------------------------------
void TestEntryPatched()
{
    auto img = MakeRealImage();
    REQUIRE(img != nullptr);
    for (std::uint32_t entry : { kBatch16Rva, kBatch32Rva, kEnqueueRva, kBindStateRva, kIndexMapRva })
        Plant(*img, entry, "e9efbeadde"); // jmp rel32
    const GameBuild& table = ext::ffxiv::kCurrentBuild;
    for (const GameBuild* t : { static_cast<const GameBuild*>(nullptr), &table })
    {
        const ResolvedBuild rb = Resolve(*img, t);
        for (const char* seam : { "companion.batch16", "companion.batch32", "companion.enqueue", "companion.bindState", "companion.indexBufferMap" })
        {
            const SeamResult& s = Get(rb, seam);
            if (s.status != SeamStatus::EntryPatched)
                std::printf("FAIL: %s is %s: %s\n", seam, StatusName(s.status), s.reason.c_str());
            CHECK(s.status == SeamStatus::EntryPatched && s.hasValue && !s.fellBackToTable);
            CHECK(!rb.UsableOrTable(seam));
        }
        CHECK(Get(rb, "companion.batch16").rva == kBatch16Rva && Get(rb, "companion.enqueue").rva == kEnqueueRva);
        // fail closed, also on the known build: nothing is filled, not even from the table
        CHECK(rb.build.companion.batch16Rva == 0 && rb.build.companion.batch32Rva == 0 && rb.build.companion.enqueueRva == 0 &&
              rb.build.companion.bindStateRva == 0);
        for (std::uint8_t v : rb.build.companion.enqueueInstructions)
            CHECK(v == 0);
        // the long entry anchors report Patched, the 16-byte ones cannot (2 fixed bytes would remain after the relaxed mask)
        CHECK(AnchorOf(rb, "companion.batch16", "entryToIndexShift").outcome == AnchorOutcome::Patched);
        CHECK(AnchorOf(rb, "companion.batch32", "entryToIndexShift").outcome == AnchorOutcome::Patched);
        CHECK(AnchorOf(rb, "companion.indexBufferMap", "entry").outcome == AnchorOutcome::Patched);
        CHECK(AnchorOf(rb, "companion.enqueue", "entry").outcome == AnchorOutcome::Zero);
        CHECK(AnchorOf(rb, "companion.bindState", "entry").outcome == AnchorOutcome::Zero);
        CHECK(AnchorOf(rb, "companion.enqueue", "entryWriteTail").outcome == AnchorOutcome::Unique);
        CHECK(AnchorOf(rb, "companion.enqueue", "callerEpilogue").outcome == AnchorOutcome::Unique);
        CHECK(AnchorOf(rb, "companion.batch16", "dispatcherStub").outcome == AnchorOutcome::Unique);
        CHECK(AnchorOf(rb, "companion.bindState", "contextCall").outcome == AnchorOutcome::Unique);
        // the shape seams: the foreign jump is not the strict head, nothing is filled (also not from the table) and no site is usable
        for (const char* shape : { "companion.batch16Shape", "companion.batch32Shape", "companion.enqueueShape", "companion.bindStateShape" })
        {
            CHECK(Get(rb, shape).status == SeamStatus::NotFound && !rb.UsableOrTable(shape));
        }
        CHECK(AllZero(rb.build.companion.batchInstructions) && AllZero(rb.build.companion.bindStateInstructions));
        // the seams that do not depend on a patched entry are untouched
        CHECK(Get(rb, "companion.nameplateModulePointer").status == SeamStatus::Found && Get(rb, "companion.layoutSlotTable").status == SeamStatus::Found);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// 5. degrade paths and the unknown-build safety
// ---------------------------------------------------------------------------------------------------------------------------------------
// The batch bodies alone (no dispatcher): single anchor, usable, a WARN. The dispatcher alone: the same through the other anchor.
void TestSingleAnchorBatch()
{
    auto bodies = MakeImageWithFunctions(RealRanges());
    REQUIRE(bodies != nullptr);
    PlantBlob(*bodies, kBatch16);
    PlantBlob(*bodies, kBatch32);
    const ResolvedBuild rb = Resolve(*bodies);
    for (const char* seam : { "companion.batch16", "companion.batch32" })
    {
        const SeamResult& s = Get(rb, seam);
        CHECK(s.status == SeamStatus::FoundSingleAnchor && Contains(s.reason, "only anchor 'entryToIndexShift'"));
    }
    CHECK(rb.build.companion.batch16Rva == kBatch16Rva && rb.build.companion.batch32Rva == kBatch32Rva);
    CHECK(IsUsable(Get(rb, "companion.batch16").status));
    // the offsets that sit in the batch functions rest on a single-anchor seam: capped too (the weakness propagates)
    CHECK(Get(rb, "companion.rendererBatchListOffset").status == SeamStatus::FoundSingleAnchor);
    CHECK(Get(rb, "companion.rendererBatchListOffset").viaWeakDependency);

    auto caller = NoFunctions();
    REQUIRE(caller != nullptr);
    PlantDispatcher(*caller, kBatch32Rva, kBatch16Rva);
    const ResolvedBuild rc = Resolve(*caller);
    CHECK(Get(rc, "companion.batch16").status == SeamStatus::FoundSingleAnchor && Get(rc, "companion.batch16").rva == kBatch16Rva);
    CHECK(Get(rc, "companion.batch32").status == SeamStatus::FoundSingleAnchor && Get(rc, "companion.batch32").rva == kBatch32Rva);
}

// The 16 bytes of the old guard alone are NOT an anchor: with only the prologues of both batch functions in the image nothing is found
// (the old guard matched two sites and the nearest other function differs in one byte).
void TestPrologueAloneIsNotAnAnchor()
{
    auto img = NoFunctions();
    REQUIRE(img != nullptr);
    Plant(*img, kBatch16Rva, "8854241048894c240855535641544155");
    Plant(*img, kBatch32Rva, "8854241048894c240855535641544155");
    const ResolvedBuild rb = Resolve(*img);
    CHECK(Get(rb, "companion.batch16").status == SeamStatus::NotFound && Get(rb, "companion.batch32").status == SeamStatus::NotFound);
    CHECK(rb.build.companion.batch16Rva == 0 && rb.build.companion.batch32Rva == 0);
}

// SAFETY: on an unknown build a bound Guard field holds the LIVE bytes at the resolved site, so the anchors are the only check. One changed
// prologue byte of enqueue (0x48 -> 0x49): the strict entry anchor fails, the tail anchor and the caller anchor still give the
// entry, so the LOCATION is plain Found; the Guard field is bound to the SHAPE seam, whose single strict anchor fails: the
// field stays ZERO, so the hook's live-bytes-against-guard compare refuses. Before the shape seam the field carried the changed byte (the hook
// compared it with itself). On a known build the table's 16 bytes are NOT put back either (failClosed): the shape is not usable there.
void TestChangedPrologueByte()
{
    auto img = MakeRealImage();
    REQUIRE(img != nullptr);
    Plant(*img, kEnqueueRva, "49");
    const ResolvedBuild rb = Resolve(*img);
    const SeamResult& s = Get(rb, "companion.enqueue");
    CHECK(s.status == SeamStatus::Found && s.rva == kEnqueueRva && rb.build.companion.enqueueRva == kEnqueueRva);
    CHECK(AnchorOf(rb, "companion.enqueue", "entry").outcome == AnchorOutcome::Zero);
    CHECK(Get(rb, "companion.enqueueShape").status == SeamStatus::NotFound && !rb.Usable("companion.enqueueShape"));
    CHECK(AllZero(rb.build.companion.enqueueInstructions));
    CHECK(!SiteUsable(rb, "companion.enqueue", "companion.enqueueShape"));
    // the same change on a known build: the table's guard is NOT put back (failClosed): the field stays zero and the shape is not usable
    const ResolvedBuild known = Resolve(*img, &ext::ffxiv::kCurrentBuild);
    CHECK(known.knownBuild && Get(known, "companion.enqueue").status == SeamStatus::Found && known.build.companion.enqueueRva == kEnqueueRva);
    CHECK(Get(known, "companion.enqueueShape").status == SeamStatus::NotFound && !Get(known, "companion.enqueueShape").fellBackToTable);
    CHECK(!known.UsableOrTable("companion.enqueueShape") && AllZero(known.build.companion.enqueueInstructions));
    CHECK(!SiteUsable(known, "companion.enqueue", "companion.enqueueShape"));
}

// A tail that moved relative to the entry (the strict entry bytes intact): the two anchors give different values: Conflict, inert.
void TestEnqueueTailMoved()
{
    auto img = MakeRealImage();
    REQUIRE(img != nullptr);
    // one byte inserted before the tail: move the tail up by one byte (copy the bytes from 0x240c1b to 0x240c1c)
    Plant(*img, 0x240c1c, SliceHex(kEnqueue, 0x240c1b, 27));
    Plant(*img, 0x240c1b, "90");
    const ResolvedBuild rb = Resolve(*img);
    const SeamResult& s = Get(rb, "companion.enqueue");
    CHECK(s.status == SeamStatus::Conflict && !s.hasValue && rb.build.companion.enqueueRva == 0);
    CHECK(AnchorOf(rb, "companion.enqueue", "entry").value == kEnqueueRva && AnchorOf(rb, "companion.enqueue", "entryWriteTail").value == kEnqueueRva + 1);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// enqueue is a LEAF (no .pdata) whose head and tail anchors shift together, so only the caller-side anchor (callerEpilogue: the
// call target IS the entry) pins the entry address. Scenarios E1..E7: the real function bytes with one edit each, planted
// where the real function is (the caller keeps calling 0x240b80). Whatever the edit, the seam is never USABLE at an address that is not the
// entry: Conflict (inert, no field) or a value equal to the entry.
// ---------------------------------------------------------------------------------------------------------------------------------------
struct Edit
{
    std::size_t pos, len; // in the ORIGINAL function bytes
    const char* with;
};

// the real enqueue bytes with `prefix` in front and the edits applied (descending positions, so the original offsets stay valid)
std::string EnqueueVariant(const char* prefix, std::vector<Edit> edits)
{
    std::string hex = kEnqueue.hex;
    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.pos > b.pos; });
    for (const Edit& e : edits)
        hex.replace(e.pos * 2, e.len * 2, e.with);
    return std::string(prefix) + hex;
}

// clear the real function (and its padding) and plant the variant at the same address
void PlantEnqueueVariant(DenseImage& img, const std::string& hex)
{
    std::string clear;
    for (int i = 0; i < 0xc8; ++i)
        clear += "cc";
    Plant(img, kEnqueueRva, clear);
    Plant(img, kEnqueueRva, hex);
}

constexpr std::size_t kJneRel32 = 0x18; // jne rel32 (6 bytes) after the first block counter update: the "body 4 bytes shorter" edit shortens it to rel8

const Edit kSwapHead = { 0, 7, "4c8bda4863410c" };   // the first two instructions swapped: the 16-byte head anchor no longer matches
const Edit kShorter = { kJneRel32, 6, "7585" };      // jne rel32 -> jne rel8: the body is 4 bytes shorter, the tail moves up by 4
const Edit kLonger = { kJneRel32 + 6, 0, "909090" }; // three bytes inserted in the body: the tail moves down by 3

struct Scenario
{
    const char* name;
    std::string hex;
    bool conflict;          // true: Conflict (inert); false: plain Found at the real entry
    const char* reasonHas;  // a conflict's reason names this anchor
};

void CheckEnqueueScenario(const Scenario& sc)
{
    auto img = MakeRealImage();
    REQUIRE(img != nullptr);
    PlantEnqueueVariant(*img, sc.hex);
    const ResolvedBuild rb = Resolve(*img);
    const SeamResult& s = Get(rb, "companion.enqueue");
    // the invariant: never a usable value at a wrong address
    if (s.hasValue && s.rva != kEnqueueRva)
        std::printf("FAIL: %s: enqueue is %s at 0x%x (%s)\n", sc.name, StatusName(s.status), s.rva, s.reason.c_str());
    CHECK(!(s.hasValue && s.rva != kEnqueueRva));
    // none of the edited functions is hookable: the shape (the 16-byte head at the entry) is gone or the location is a Conflict, the Guard is zero
    CHECK(!rb.Usable("companion.enqueueShape") && AllZero(rb.build.companion.enqueueInstructions));
    CHECK(!SiteUsable(rb, "companion.enqueue", "companion.enqueueShape"));
    if (sc.conflict)
    {
        if (s.status != SeamStatus::Conflict)
            std::printf("FAIL: %s: enqueue is %s at 0x%x (%s)\n", sc.name, StatusName(s.status), s.rva, s.reason.c_str());
        CHECK(s.status == SeamStatus::Conflict && !s.hasValue && rb.build.companion.enqueueRva == 0);
        CHECK(Contains(s.reason, sc.reasonHas));
        for (std::uint8_t v : rb.build.companion.enqueueInstructions)
            CHECK(v == 0);
    }
    else
    {
        if (s.status != SeamStatus::Found)
            std::printf("FAIL: %s: enqueue is %s at 0x%x (%s)\n", sc.name, StatusName(s.status), s.rva, s.reason.c_str());
        CHECK(s.status == SeamStatus::Found && s.rva == kEnqueueRva && rb.build.companion.enqueueRva == kEnqueueRva);
    }
}

void TestEnqueueEntryScenarios()
{
    // the caller anchor itself is the third anchor and finds the real entry on the real bytes
    {
        auto img = MakeRealImage();
        REQUIRE(img != nullptr);
        const ResolvedBuild rb = Resolve(*img);
        const AnchorResult& a = AnchorOf(rb, "companion.enqueue", "callerEpilogue");
        CHECK(a.outcome == AnchorOutcome::Unique && a.match == 0x23c0d2 && a.value == kEnqueueRva && a.patternHits == 1);
        CHECK(Get(rb, "companion.enqueue").hits == 3);
    }
    // E1 head swapped + body 4 shorter, E2 head swapped + 3 inserted: the tail alone said 0x240b7c / 0x240b83 (before / inside the function)
    CheckEnqueueScenario({ "E1", EnqueueVariant("", { kSwapHead, kShorter }), true, "callerEpilogue" });
    CheckEnqueueScenario({ "E2", EnqueueVariant("", { kSwapHead, kLonger }), true, "callerEpilogue" });
    // E3 only the head swapped: the tail and the caller agree on the real entry (plain Found, two anchors)
    CheckEnqueueScenario({ "E3", EnqueueVariant("", { kSwapHead }), false, "" });
    // E4 head intact, body 4 shorter: the head and the tail disagree (covered before the caller anchor existed, kept)
    CheckEnqueueScenario({ "E4", EnqueueVariant("", { kShorter }), true, "entryWriteTail" });
    // E5 / E6: a prefix before the head moves the head AND the tail together (benign only for a pure-computation prefix, silent without the caller)
    CheckEnqueueScenario({ "E5 test rdx,rdx ; je", EnqueueVariant("4885d27402", {}), true, "callerEpilogue" });
    CheckEnqueueScenario({ "E6 mov r10,rcx", EnqueueVariant("4989ca", {}), true, "callerEpilogue" });
    // E7 a stack-changing prefix: OnEnqueue would run one slot off the stack
    CheckEnqueueScenario({ "E7a push rbx", EnqueueVariant("53", {}), true, "callerEpilogue" });
    CheckEnqueueScenario({ "E7b sub rsp,0x28", EnqueueVariant("4883ec28", {}), true, "callerEpilogue" });

    // RESIDUAL, pinned: the caller site is the only anchor that does not move with the function. If the wrapper itself was recompiled (callerEpilogue
    // finds nothing) AND a prefix appears before the head, the head and the tail agree at entry+5: a silent plain Found, and the shape (the head
    // verified at that address) matches too. Only the anchor count tells it apart from the real thing: hits 2 instead of 3. The header's hook-site rule:
    // hook enqueue on an unknown build only when the callerEpilogue AnchorResult is Unique (the hits sum, 2 here instead of 3, is only an
    // observation, not the rule). If this scenario ever becomes a Conflict or
    // not usable, update this block on purpose.
    {
        auto img = MakeRealImage();
        REQUIRE(img != nullptr);
        PlantEnqueueVariant(*img, EnqueueVariant("4885d27402", {}));
        std::string blank;
        for (std::size_t i = 0; i < 42; ++i)
            blank += "cc";
        Plant(*img, 0x23c0d2, blank); // the wrapper is gone
        const ResolvedBuild rb = Resolve(*img);
        const SeamResult& s = Get(rb, "companion.enqueue");
        CHECK(AnchorOf(rb, "companion.enqueue", "callerEpilogue").outcome == AnchorOutcome::Zero);
        CHECK(s.status == SeamStatus::Found && s.rva == kEnqueueRva + 5 && s.hits == 2);
        CHECK(SiteUsable(rb, "companion.enqueue", "companion.enqueueShape"));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// LOCATION and SHAPE seams (the two-tier design). The Guard fields are bound to the SHAPE seams (one strict anchor, verified at
// the location seam's address, failClosed), the Rva fields to the location seams. A Detour site is usable only when both are.
// ---------------------------------------------------------------------------------------------------------------------------------------
struct Site
{
    const char* location;
    const char* shape;
    std::uint32_t rva;
    std::uint32_t flip; // a byte inside the strict shape that the OTHER location anchors do not read (dispatcher / tail / caller / body anchors)
    const char* what;
};

void TestShapeSeams()
{
    const Site sites[] = {
        { "companion.batch16", "companion.batch16Shape", kBatch16Rva, kBatch16Rva + 27, "frame size 0x128 -> 0xfe28 (the strict head only)" },
        { "companion.batch32", "companion.batch32Shape", kBatch32Rva, kBatch32Rva + 27, "frame size 0x128 -> 0xfe28 (the strict head only)" },
        { "companion.enqueue", "companion.enqueueShape", kEnqueueRva, kEnqueueRva + 4, "mov r11,rdx -> a different register" },
        { "companion.bindState", "companion.bindStateShape", kBindStateRva, kBindStateRva + 20, "mov rsi,rcx -> a different register" },
    };
    const GameBuild& table = ext::ffxiv::kCurrentBuild;
    for (const GameBuild* t : { static_cast<const GameBuild*>(nullptr), &table })
    {
        // the real bytes: every site usable, the shape is Found at the location's address
        auto clean = MakeRealImage();
        REQUIRE(clean != nullptr);
        const ResolvedBuild ok = Resolve(*clean, t);
        for (const Site& s : sites)
        {
            const SeamResult& loc = Get(ok, s.location);
            const SeamResult& shape = Get(ok, s.shape);
            CHECK(loc.status == SeamStatus::Found && shape.status == SeamStatus::Found && shape.rva == loc.rva && shape.rva == s.rva);
            CHECK(AnchorOf(ok, s.shape, "shape").outcome == AnchorOutcome::Unique && !shape.fellBackToTable);
            CHECK(SiteUsable(ok, s.location, s.shape));
        }
        for (const Site& s : sites)
        {
            // one byte of the strict shape changed: the location still resolves (its other anchors do not read that byte), the shape does not
            auto img = MakeRealImage();
            REQUIRE(img != nullptr);
            img->Bytes()[s.flip] ^= 0xff;
            const ResolvedBuild rb = Resolve(*img, t);
            const SeamResult& loc = Get(rb, s.location);
            const SeamResult& shape = Get(rb, s.shape);
            if (!(IsUsable(loc.status) && loc.rva == s.rva))
                std::printf("FAIL: %s with %s: location is %s at 0x%x (%s)\n", s.location, s.what, StatusName(loc.status), loc.rva, loc.reason.c_str());
            CHECK(IsUsable(loc.status) && loc.rva == s.rva);
            CHECK(shape.status == SeamStatus::NotFound && !shape.hasValue && !shape.fellBackToTable);
            CHECK(Contains(AnchorOf(rb, s.shape, "shape").note, "do not match"));
            CHECK(!rb.Usable(s.shape) && !rb.UsableOrTable(s.shape));
            CHECK(!SiteUsable(rb, s.location, s.shape));
            // the Guard fields bound to a failed shape stay ZERO, on the known build too (the table's 16 bytes are only a prefix of the strict pattern)
            if (std::string(s.shape) == "companion.batch16Shape")
                CHECK(AllZero(rb.build.companion.batchInstructions));
            else if (std::string(s.shape) == "companion.enqueueShape")
                CHECK(AllZero(rb.build.companion.enqueueInstructions));
            else if (std::string(s.shape) == "companion.bindStateShape")
                CHECK(AllZero(rb.build.companion.bindStateInstructions));
            else // batch32Shape: no field of its own (the table keeps ONE batch guard, bound to batch16Shape): the gate tests the seam
                CHECK(!rb.Usable("companion.batch32Shape") && !AllZero(rb.build.companion.batchInstructions));
            // the Rva field of the location is filled as before (a known build: the same value)
            if (std::string(s.shape) == "companion.enqueueShape")
                CHECK(rb.build.companion.enqueueRva == s.rva);
            if (std::string(s.shape) == "companion.bindStateShape")
                CHECK(rb.build.companion.bindStateRva == s.rva);
        }
    }
}

// The routes on an UNKNOWN build: the location is found by ONE anchor whose bytes do not include the prologue, so
// the old Guard (bound to the location seam) was the live bytes at that address. Now the shape reads the prologue and fails: Guard zero.
void TestShapeRoutes()
{
    // dispatcher only (no batch bodies): the call targets are function entries, nothing compares the callee prologues
    {
        auto img = NoFunctions();
        REQUIRE(img != nullptr);
        PlantDispatcher(*img, kBatch32Rva, kBatch16Rva);
        Plant(*img, kBatch16Rva, "8854241048894c240855535641544155"); // the 16-byte prologue: the old guard would have matched
        Plant(*img, kBatch32Rva, "8854241048894c240855535641544155");
        const ResolvedBuild rb = Resolve(*img);
        CHECK(IsUsable(Get(rb, "companion.batch16").status) && IsUsable(Get(rb, "companion.batch32").status));
        CHECK(rb.build.companion.batch16Rva == kBatch16Rva && rb.build.companion.batch32Rva == kBatch32Rva);
        CHECK(!rb.Usable("companion.batch16Shape") && !rb.Usable("companion.batch32Shape") && AllZero(rb.build.companion.batchInstructions));
        CHECK(!SiteUsable(rb, "companion.batch16", "companion.batch16Shape") && !SiteUsable(rb, "companion.batch32", "companion.batch32Shape"));
    }
    // enqueue: the tail alone, the caller alone (the old Guard was the live head at the address the tail or the caller named)
    {
        auto tail = NoFunctions();
        REQUIRE(tail != nullptr);
        Plant(*tail, 0x240c1b, SliceHex(kEnqueue, 0x240c1b, 27));
        const ResolvedBuild rb = Resolve(*tail);
        CHECK(Get(rb, "companion.enqueue").status == SeamStatus::FoundSingleAnchor && rb.build.companion.enqueueRva == kEnqueueRva);
        CHECK(!rb.Usable("companion.enqueueShape") && AllZero(rb.build.companion.enqueueInstructions));
        CHECK(!SiteUsable(rb, "companion.enqueue", "companion.enqueueShape"));

        auto caller = NoFunctions();
        REQUIRE(caller != nullptr);
        PlantBlob(*caller, kEnqueueCaller);
        const ResolvedBuild rc = Resolve(*caller);
        CHECK(Get(rc, "companion.enqueue").status == SeamStatus::FoundSingleAnchor && rc.build.companion.enqueueRva == kEnqueueRva);
        CHECK(!rc.Usable("companion.enqueueShape") && AllZero(rc.build.companion.enqueueInstructions));
        CHECK(!SiteUsable(rc, "companion.enqueue", "companion.enqueueShape"));
    }
    // bindState: the context-call body alone (the function start comes from .pdata)
    {
        auto img = MakeImageWithFunctions(RealRanges());
        REQUIRE(img != nullptr);
        Plant(*img, 0x22d8b4, SliceHex(kBindState, 0x22d8b4, 49));
        const ResolvedBuild rb = Resolve(*img);
        CHECK(Get(rb, "companion.bindState").status == SeamStatus::FoundSingleAnchor && rb.build.companion.bindStateRva == kBindStateRva);
        CHECK(!rb.Usable("companion.bindStateShape") && AllZero(rb.build.companion.bindStateInstructions));
        CHECK(!SiteUsable(rb, "companion.bindState", "companion.bindStateShape"));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// nativeContextOffset is dereferenced as a COM pointer (CompanionLayer.cpp: Stamp calls GetType / SetPrivateData). Anchor A
// (loadBeforeVirtualCall) proves the object has a vtable whose slot 35 (+0x118) is loaded; anchor B (loadBeforeStateCall) is now the full
// 49-byte text up to `call [rax+0x98]` (vtable slot 19 = IASetIndexBuffer, r8d 0x2a / 0x39 = R32_UINT / R16_UINT), the proof that the object is a
// D3D11 context. A changed slot makes B refuse; A alone leaves a usable FoundSingleAnchor (prefer plain Found, the header says so); both
// refusing leaves the seam NotFound (an unresolved Offset seam keeps the table default on an unknown build: bindState needs Usable).
// ---------------------------------------------------------------------------------------------------------------------------------------
void TestNativeContextVtableSlot()
{
    auto status = [](const ResolvedBuild& rb, const char* anchor)
    { return AnchorOf(rb, "companion.nativeContextOffset", anchor).outcome; };
    {
        auto img = MakeRealImage();
        REQUIRE(img != nullptr);
        const ResolvedBuild rb = Resolve(*img);
        CHECK(Get(rb, "companion.nativeContextOffset").status == SeamStatus::Found && Get(rb, "companion.nativeContextOffset").rva == 0x17b8);
        CHECK(AnchorOf(rb, "companion.nativeContextOffset", "loadBeforeStateCall").match == 0x22d8b4);
        CHECK(AnchorOf(rb, "companion.nativeContextOffset", "loadBeforeStateCall").value == 0x17b8);
    }
    // slot 19 -> slot 20 in B (the call [rax+0x98] displacement 0x98 -> 0xa0): B refuses, A keeps the seam single-anchor
    {
        auto img = MakeRealImage();
        REQUIRE(img != nullptr);
        Plant(*img, 0x22d8df + 2, "a0");
        const ResolvedBuild rb = Resolve(*img);
        CHECK(status(rb, "loadBeforeStateCall") == AnchorOutcome::Zero && status(rb, "loadBeforeVirtualCall") == AnchorOutcome::Unique);
        const SeamResult& s = Get(rb, "companion.nativeContextOffset");
        CHECK(s.status == SeamStatus::FoundSingleAnchor && s.rva == 0x17b8 && Contains(s.reason, "only anchor 'loadBeforeVirtualCall'"));
        // the bindState location does not depend on that byte through its prologue anchors: still Found (its context anchor refuses too)
        CHECK(Get(rb, "companion.bindState").status == SeamStatus::Found);
    }
    // every byte of the vtable call is pinned: the opcode, the modrm and the displacement
    for (std::uint32_t at : { 0x22d8dfu, 0x22d8e0u, 0x22d8e1u, 0x22d8e2u, 0x22d8e5u - 1 })
    {
        auto img = MakeRealImage();
        REQUIRE(img != nullptr);
        img->Bytes()[at] ^= 0xff;
        const ResolvedBuild rb = Resolve(*img);
        if (status(rb, "loadBeforeStateCall") != AnchorOutcome::Zero)
            std::printf("FAIL: flipping 0x%x did not stop loadBeforeStateCall\n", at);
        CHECK(status(rb, "loadBeforeStateCall") == AnchorOutcome::Zero);
        CHECK(Get(rb, "companion.nativeContextOffset").status == SeamStatus::FoundSingleAnchor);
    }
    // A refuses (slot 35 -> 36): the same, through B
    {
        auto img = MakeRealImage();
        REQUIRE(img != nullptr);
        Plant(*img, 0x22d7e4 + 3, "20");
        const ResolvedBuild rb = Resolve(*img);
        CHECK(status(rb, "loadBeforeVirtualCall") == AnchorOutcome::Zero && status(rb, "loadBeforeStateCall") == AnchorOutcome::Unique);
        CHECK(Get(rb, "companion.nativeContextOffset").status == SeamStatus::FoundSingleAnchor);
    }
    // both refuse: NotFound, no value (the field would keep the table default: bindState needs Usable("companion.nativeContextOffset"))
    {
        auto img = MakeRealImage();
        REQUIRE(img != nullptr);
        Plant(*img, 0x22d8df + 2, "a0");
        Plant(*img, 0x22d7e4 + 3, "20");
        const ResolvedBuild rb = Resolve(*img);
        const SeamResult& s = Get(rb, "companion.nativeContextOffset");
        CHECK(s.status == SeamStatus::NotFound && !s.hasValue && !rb.Usable("companion.nativeContextOffset"));
    }
}

// Nothing planted: every seam NotFound, every Rva and Guard field of the group stays ZERO (never a C value): the 0x25f610 reuse trap.
void TestNothingFound()
{
    auto img = NoFunctions();
    REQUIRE(img != nullptr);
    const ResolvedBuild rb = Resolve(*img);
    CHECK(rb.Count(SeamStatus::NotFound) == rb.seams.size());
    const auto& c = rb.build.companion;
    CHECK(c.batch16Rva == 0 && c.batch32Rva == 0 && c.enqueueRva == 0 && c.bindStateRva == 0 && c.nameplateModulePointerRva == 0 && c.layoutSlotTableRva == 0);
    for (std::uint8_t v : c.batchInstructions)
        CHECK(v == 0);
    for (std::uint8_t v : c.bindStateInstructions)
        CHECK(v == 0);
}

// A dependency that did not resolve leaves its dependents NotFound with the name in the reason, never a guess: no batch seam, no renderer offsets.
void TestOffsetsNeedTheirFunction()
{
    auto img = NoFunctions();
    REQUIRE(img != nullptr);
    PlantBlob(*img, kBatch16);
    PlantBlob(*img, kBatch32); // the bodies are there, but without .pdata SameFunctionAs cannot say which function a match is in
    const ResolvedBuild rb = Resolve(*img);
    const SeamResult& s = Get(rb, "companion.rendererBatchListOffset");
    CHECK(s.status == SeamStatus::NotFound);
    CHECK(AnchorOf(rb, "companion.rendererBatchListOffset", "inBatch16").patternHits == 2 && AnchorOf(rb, "companion.rendererBatchListOffset", "inBatch16").candidates == 0);
    CHECK(Get(rb, "companion.batch16").status == SeamStatus::FoundSingleAnchor); // the function itself is found without .pdata
}
} // namespace

int main()
{
    TestCurrentBuild();
    TestBatchCollapse();
    TestBatchSwap();
    TestBatchRoleConfusion();
    TestBatchInvertedDispatcher();
    TestDecoysRemoved();
    TestDispatcherTargetMustBeCode();
    TestNameplateGlobalMustBeBss();
    TestNameplateSingleSite();
    TestValueInOnOffsets();
    TestValueInOnLayoutTable();
    TestValueInTightBounds();
    TestLayoutTableContentOnly();
    TestEntryPatched();
    TestSingleAnchorBatch();
    TestPrologueAloneIsNotAnAnchor();
    TestChangedPrologueByte();
    TestEnqueueTailMoved();
    TestEnqueueEntryScenarios();
    TestShapeSeams();
    TestShapeRoutes();
    TestNativeContextVtableSlot();
    TestNothingFound();
    TestOffsetsNeedTheirFunction();
    return TEST_RESULT("resolve_companion_seams");
}
