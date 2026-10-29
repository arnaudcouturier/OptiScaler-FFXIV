// Packet association of the developer probes (ext/hudless/PlateProbePackets.h): the logged-frame gate, header and range decoding, the index
// list against the packets, the vertex run, the box (min and max, stride, position offset, used flags), the ordinals, the association map
// (age, changed command, capacity, lookup by ordinal), the draw match, the draw command fields and the overlay mapping.
#include "ext/hudless/PlateAssocCounters.h"
#include "ext/hudless/PlateProbePackets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace ext::hudless::plate;

namespace
{
int g_failures = 0;
#define EXPECT(cond, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s  ", __LINE__, #cond);                                                    \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)
#define EXPECT_EQ(a, b)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        const std::string va_ = (a), vb_ = (b);                                                                        \
        if (va_ != vb_)                                                                                                \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s\n      got      [%s]\n      expected [%s]\n", __LINE__, #a, va_.c_str(), vb_.c_str());\
        }                                                                                                              \
    } while (0)

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// the offsets of the known build, as the runtime reads them from the active build table
const HeaderOffsets kHeader { 0x00, 0x20, 0x28, 0x30 };

std::vector<unsigned char> Header(uint32_t kind, uint32_t count, uint64_t vertices, uint32_t format, size_t size = 64)
{
    std::vector<unsigned char> b((std::max)(size, size_t(0x38)), 0);
    std::memcpy(b.data() + kHeader.kind, &kind, 4);
    std::memcpy(b.data() + kHeader.count, &count, 4);
    std::memcpy(b.data() + kHeader.vertices, &vertices, 8);
    std::memcpy(b.data() + kHeader.format, &format, 4);
    b.resize(size);
    return b;
}

void Gate()
{
    EXPECT(ReadOnFrame(0), "before the first frame: frame 1 is the next one");
    EXPECT(ReadOnFrame(1) && ReadOnFrame(300) && ReadOnFrame(599) && ReadOnFrame(600), "the first 600 frames");
    EXPECT(!ReadOnFrame(601) && !ReadOnFrame(650) && !ReadOnFrame(698), "between the early frames and the first sample");
    EXPECT(ReadOnFrame(699), "one frame before a sampled frame: the submitting side may be ahead");
    EXPECT(ReadOnFrame(700), "a sampled frame");
    EXPECT(!ReadOnFrame(701) && !ReadOnFrame(750) && !ReadOnFrame(798), "not around the other frames");
    EXPECT(ReadOnFrame(799) && ReadOnFrame(800) && !ReadOnFrame(801), "every 100th");
    EXPECT(ReadOnFrame(123399) && ReadOnFrame(123400), "far into the run");
}

void Tables()
{
    EXPECT(LayoutStride(0) == 40 && LayoutStride(4) == 24 && LayoutStride(5) == 32 && LayoutStride(6) == 32, "layout strides");
    EXPECT(LayoutStride(1) == 0 && LayoutStride(2) == 0 && LayoutStride(3) == 0 && LayoutStride(7) == 0 && LayoutStride(8) == 0 && LayoutStride(15) == 0,
           "other layouts are unknown");
    for (uint32_t kind : { 0x10u, 0x11u, 0x20u, 0x21u, 0x22u, 0x23u })
        EXPECT(GeometryKind(kind), "kind %x", kind);
    for (uint32_t kind : { 0u, 0x0Fu, 0x12u, 0x1Fu, 0x24u, 0x30u })
        EXPECT(!GeometryKind(kind), "kind %x", kind);
}

void Headers()
{
    const auto bytes = Header(0x22, 12, 0x7FF600001000ull, 0x1234);
    const PacketHeader h = ParseHeader(bytes, kHeader);
    EXPECT(h.ok && h.kind == 0x22 && h.count == 12 && h.vertices == 0x7FF600001000ull && h.layout == 4, "fields (layout is the low 4 bits of the format)");
    EXPECT(ParseHeader(Header(0x22, 1, 1, 0x0F), kHeader).layout == 15, "all four bits");
    EXPECT(ParseHeader(Header(0x22, 1, 1, 0x10), kHeader).layout == 0, "bit 4 is not the layout");

    // a header shorter than a field makes it unusable, field by field
    EXPECT(!ParseHeader(Header(0x22, 12, 1, 0, 0x2F), kHeader).ok, "format cut");
    EXPECT(ParseHeader(Header(0x22, 12, 1, 0, 0x34), kHeader).ok, "exactly long enough");
    EXPECT(!ParseHeader(Header(0x22, 12, 1, 0, 0x33), kHeader).ok, "format one byte short");
    EXPECT(!ParseHeader(std::vector<unsigned char>(), kHeader).ok, "empty");
    HeaderOffsets far = kHeader;
    far.vertices = 60;
    EXPECT(!ParseHeader(bytes, far).ok, "vertices pointer past the end");
    far = kHeader;
    far.count = 61;
    EXPECT(!ParseHeader(bytes, far).ok, "count past the end");
    far = kHeader;
    far.kind = 0xFFFFFFFFu;
    EXPECT(!ParseHeader(bytes, far).ok, "a huge offset does not wrap");
}

void Ranges()
{
    const uint64_t alloc = 0x200000;
    PacketRange r;
    // layout 0 (stride 40), slot offset 16: vertex 10 sits at alloc + 16 + 400
    PacketHeader h = ParseHeader(Header(0x22, 8, alloc + 16 + 400, 0), kHeader);
    EXPECT(MakeRange(h, 0xABCD, alloc, 16, 40, r) == RangeResult::Ok && r.key == 0xABCD && r.first == 10 && r.count == 8 && r.layout == 0, "range");
    EXPECT(MakeRange(ParseHeader(Header(0x22, 8, alloc + 16, 0), kHeader), 1, alloc, 16, 40, r) == RangeResult::Ok && r.first == 0, "first vertex");

    EXPECT(MakeRange(ParseHeader(Header(0x22, 8, alloc + 16 + 401, 0), kHeader), 1, alloc, 16, 40, r) == RangeResult::BadPointer, "off a vertex boundary");
    EXPECT(MakeRange(ParseHeader(Header(0x22, 8, alloc + 15, 0), kHeader), 1, alloc, 16, 40, r) == RangeResult::BadPointer, "before the slot");
    EXPECT(MakeRange(ParseHeader(Header(0x22, 8, alloc + 16 + 40, 0), kHeader), 1, 0, 16, 40, r) == RangeResult::BadPointer, "no allocation");
    EXPECT(MakeRange(ParseHeader(Header(0x22, 8, alloc + 16 + 40ull * 0xFFFFFFFFull, 0), kHeader), 1, alloc, 16, 40, r) == RangeResult::BadPointer,
           "vertex number past 32 bits");
    EXPECT(MakeRange(ParseHeader(Header(0x22, 8, alloc + 16, 5), kHeader), 1, alloc, 16, 32, r) == RangeResult::Ok && r.layout == 5, "layout 5, stride 32");
    EXPECT(MakeRange(ParseHeader(Header(0x22, 8, alloc + 16, 5), kHeader), 1, alloc, 16, 24, r) == RangeResult::BadLayout, "slot stride is not the layout's");
    EXPECT(MakeRange(ParseHeader(Header(0x22, 8, alloc + 16, 1), kHeader), 1, alloc, 16, 40, r) == RangeResult::BadLayout, "unknown layout");
    EXPECT(MakeRange(ParseHeader(Header(0x22, 8, alloc + 16, 6), kHeader), 1, alloc, 16, 0, r) == RangeResult::BadLayout, "no slot stride");
    EXPECT(MakeRange(ParseHeader(Header(0x30, 8, alloc + 16, 0), kHeader), 1, alloc, 16, 40, r) == RangeResult::NotGeometry, "clip record");
    EXPECT(MakeRange(ParseHeader(Header(0x22, 0, alloc + 16, 0), kHeader), 1, alloc, 16, 40, r) == RangeResult::BadCount, "no vertices");
    EXPECT(MakeRange(ParseHeader(Header(0x22, 65536, alloc + 16, 0), kHeader), 1, alloc, 16, 40, r) == RangeResult::Ok, "65536 vertices");
    EXPECT(MakeRange(ParseHeader(Header(0x22, 65537, alloc + 16, 0), kHeader), 1, alloc, 16, 40, r) == RangeResult::BadCount, "65537 vertices");
    EXPECT(MakeRange(PacketHeader(), 1, alloc, 16, 40, r) == RangeResult::BadHeader, "bad header");
}

RangeSet TwoPackets()
{
    RangeSet set;
    set.Add({ 0xB0, 8, 8, 0 }); // added out of order on purpose
    set.Add({ 0xA0, 0, 8, 0 });
    set.Seal();
    return set;
}

void Find()
{
    RangeSet set;
    EXPECT(set.Find(0) == -1 && set.Find(-3) == -1, "empty set");
    set.Add({ 1, 20, 4, 0 });
    set.Add({ 2, 10, 4, 0 });
    set.Seal();
    EXPECT(set.Size() == 2, "size");
    EXPECT(set.Find(9) == -1 && set.Find(14) == -1 && set.Find(19) == -1 && set.Find(24) == -1, "outside");
    EXPECT(set.Find(10) == 0 && set.Find(13) == 0, "first packet (sorted by first vertex)");
    EXPECT(set.Find(20) == 1 && set.Find(23) == 1, "second packet");
    EXPECT(set.At(0).key == 2 && set.At(1).key == 1, "sorted");
    EXPECT(set.Find(-1) == -1 && set.Find(int64_t(1) << 40) == -1, "negative and huge");
    set.Clear();
    EXPECT(set.Size() == 0 && set.Find(10) == -1, "cleared");
}

void Uses()
{
    const RangeSet set = TwoPackets();
    const std::vector<uint32_t> both { 0, 1, 2, 8, 9, 10 };
    DrawUse u = Analyze(both, 0, set);
    EXPECT(u.indices == 6 && u.mapped == 6 && u.unmapped == 0 && u.entries == 2, "two packets");
    EXPECT(u.hasKey && u.qkey == 0xA0 && u.layout == 0, "key of the packet of the first index");
    EXPECT(u.hasRun && u.minVertex == 0 && u.maxVertex == 10, "run");

    const std::vector<uint32_t> reversed { 8, 9, 10, 0, 1, 2 };
    u = Analyze(reversed, 0, set);
    EXPECT(u.qkey == 0xB0 && u.entries == 2, "the first index decides the key, not the lowest vertex");

    const std::vector<uint32_t> one { 3, 3, 4, 5 };
    u = Analyze(one, 0, set);
    EXPECT(u.entries == 1 && u.qkey == 0xA0 && u.mapped == 4, "a packet counts once");

    const std::vector<uint32_t> shifted { 0, 1, 2 };
    u = Analyze(shifted, 8, set);
    EXPECT(u.qkey == 0xB0 && u.entries == 1 && u.minVertex == 8 && u.maxVertex == 10, "base vertex moves the indices");
    u = Analyze(shifted, -1, set);
    EXPECT(u.unmapped == 1 && u.mapped == 2 && u.minVertex == 0 && u.maxVertex == 1, "a vertex below zero is outside every packet");

    const std::vector<uint32_t> stray { 2, 100, 3 };
    u = Analyze(stray, 0, set);
    EXPECT(u.mapped == 2 && u.unmapped == 1 && u.maxVertex == 100 && u.entries == 1, "an index in no packet is counted and widens the run");

    const std::vector<uint32_t> none { 50, 51 };
    u = Analyze(none, 0, set);
    EXPECT(u.mapped == 0 && u.unmapped == 2 && !u.hasKey && u.entries == 0 && u.hasRun, "no packet at all");

    u = Analyze(std::span<const uint32_t>(), 0, set);
    EXPECT(u.indices == 0 && !u.hasKey && !u.hasRun && u.entries == 0, "no indices");
    u = Analyze(both, 0, RangeSet());
    EXPECT(u.mapped == 0 && u.unmapped == 6 && u.entries == 0, "no packets");
}

void Runs()
{
    DrawUse use;
    use.hasRun = true;
    use.minVertex = 10;
    use.maxVertex = 19;
    RunPlan p = PlanRun(use, 24, 240);
    EXPECT(p.ok && p.first == 10 && p.count == 10, "run of 10 vertices");
    EXPECT(!PlanRun(use, 24, 239).ok, "one byte over the cap");
    EXPECT(PlanRun(use, 24, 240).ok, "exactly the cap");
    EXPECT(!PlanRun(use, 0, 1000).ok, "no stride");
    use.hasRun = false;
    EXPECT(!PlanRun(use, 24, 1000).ok, "no vertices");
    use.hasRun = true;
    use.maxVertex = 9;
    EXPECT(!PlanRun(use, 24, 1000).ok, "inverted run");
    use.minVertex = 0;
    use.maxVertex = 0xFFFFFFFFll;
    EXPECT(!PlanRun(use, 1, 0xFFFFFFFFull * 4).ok, "more than 32 bits of vertices");
    use.minVertex = 5;
    use.maxVertex = 5;
    p = PlanRun(use, 40, 40);
    EXPECT(p.ok && p.first == 5 && p.count == 1, "a single vertex");

    // used flags: a vertex between two packets that no index reaches stays unused
    const std::vector<uint32_t> indices { 10, 11, 12, 12, 15, 16, 17 };
    use.minVertex = 10;
    use.maxVertex = 17;
    p = PlanRun(use, 24, 1000);
    const std::vector<uint8_t> used = MarkUsed(indices, 0, p);
    EXPECT(used.size() == 8, "one flag per vertex of the run");
    const std::vector<uint8_t> expected { 1, 1, 1, 0, 0, 1, 1, 1 };
    EXPECT(used == expected, "flags");
    const std::vector<uint32_t> small { 0, 1, 2 };
    const std::vector<uint8_t> moved = MarkUsed(small, 10, p);
    const std::vector<uint8_t> movedExpected { 1, 1, 1, 0, 0, 0, 0, 0 };
    EXPECT(moved == movedExpected, "base vertex moves the flags");
    const std::vector<uint32_t> outside { 9, 10, 18, 100 };
    const std::vector<uint8_t> clipped = MarkUsed(outside, 0, p);
    const std::vector<uint8_t> clippedExpected { 1, 0, 0, 0, 0, 0, 0, 0 };
    EXPECT(clipped == clippedExpected, "indices outside the run are ignored");
    EXPECT(MarkUsed(indices, 0, RunPlan()).empty(), "no plan, no flags");
}

// a run of vertices with a stride and a position offset; every vertex also carries junk at offset 0 and in the tail so a wrong offset or a
// wrong stride gives a different box
std::vector<unsigned char> Vertices(uint32_t stride, uint32_t posOffset, const std::vector<std::pair<float, float>>& positions)
{
    std::vector<unsigned char> run(stride * positions.size(), 0);
    for (size_t i = 0; i < positions.size(); ++i)
    {
        unsigned char* v = run.data() + i * stride;
        const float junk[2] = { 9000.0f + float(i), -9000.0f - float(i) };
        std::memcpy(v, junk, 8);
        std::memcpy(v + posOffset, &positions[i].first, 4);
        std::memcpy(v + posOffset + 4, &positions[i].second, 4);
        if (stride >= posOffset + 16)
            std::memcpy(v + posOffset + 8, junk, 8);
    }
    return run;
}

void Boxes()
{
    const std::vector<std::pair<float, float>> pts { { 100.5f, 50.0f }, { 260.5f, 74.0f }, { 120.0f, -12.5f }, { -30.0f, 200.0f } };
    const VertexLayout l24 { 24, 8, 2 };
    Box b;
    EXPECT(ComputeBox(Vertices(24, 8, pts), l24, {}, b), "box");
    EXPECT(b.x0 == -30.0f && b.y0 == -12.5f && b.x1 == 260.5f && b.y1 == 200.0f && b.vertices == 4 && b.skipped == 0,
           "min and max on each axis (%.1f %.1f %.1f %.1f)", b.x0, b.y0, b.x1, b.y1);

    // the position offset: the same bytes read at another offset give another box (junk lies at 0)
    EXPECT(ComputeBox(Vertices(24, 8, pts), VertexLayout { 24, 0, 2 }, {}, b) && b.x0 >= 9000.0f, "a different offset reads different bytes");
    // the stride: vertices are `stride` apart
    const VertexLayout l40 { 40, 16, 3 };
    EXPECT(ComputeBox(Vertices(40, 16, pts), l40, {}, b) && b.x0 == -30.0f && b.y1 == 200.0f && b.vertices == 4, "stride 40, offset 16, float3 position");
    EXPECT(!ComputeBox(Vertices(40, 16, pts), VertexLayout { 24, 16, 3 }, {}, b), "a stride that does not divide the buffer");
    const VertexLayout l32 { 32, 24, 2 };
    EXPECT(ComputeBox(Vertices(32, 24, pts), l32, {}, b) && b.x1 == 260.5f, "stride 32, offset 24 is the last position that fits");

    // used flags
    const std::vector<uint8_t> used { 1, 0, 1, 0 };
    EXPECT(ComputeBox(Vertices(24, 8, pts), l24, used, b) && b.x0 == 100.5f && b.y0 == -12.5f && b.x1 == 120.0f && b.y1 == 50.0f && b.vertices == 2,
           "only the used vertices (%.1f %.1f %.1f %.1f)", b.x0, b.y0, b.x1, b.y1);
    EXPECT(!ComputeBox(Vertices(24, 8, pts), l24, std::vector<uint8_t>(3, 1), b), "flags that do not match the buffer");
    EXPECT(!ComputeBox(Vertices(24, 8, pts), l24, std::vector<uint8_t>(4, 0), b), "nothing used");

    // single vertex
    const std::vector<std::pair<float, float>> single { { 7.0f, 9.0f } };
    EXPECT(ComputeBox(Vertices(24, 8, single), l24, {}, b) && b.x0 == 7.0f && b.x1 == 7.0f && b.y0 == 9.0f && b.y1 == 9.0f && b.vertices == 1, "one vertex");

    // bad numbers are skipped, and the box is of the others
    const std::vector<std::pair<float, float>> bad { { kNaN, 1.0f }, { 5.0f, 6.0f }, { 1.0f, kInf }, { 8.0f, 2.0f } };
    EXPECT(ComputeBox(Vertices(24, 8, bad), l24, {}, b) && b.skipped == 2 && b.vertices == 2 && b.x0 == 5.0f && b.y0 == 2.0f && b.x1 == 8.0f && b.y1 == 6.0f,
           "non-finite positions are skipped");
    const std::vector<std::pair<float, float>> allBad { { kNaN, kNaN }, { -kInf, 0.0f } };
    EXPECT(!ComputeBox(Vertices(24, 8, allBad), l24, {}, b) && b.skipped == 2, "all bad is no box");

    // empty, short, bad layout
    EXPECT(!ComputeBox({}, l24, {}, b), "empty buffer");
    std::vector<unsigned char> shortRun = Vertices(24, 8, pts);
    shortRun.resize(shortRun.size() - 1);
    EXPECT(!ComputeBox(shortRun, l24, {}, b), "a buffer that ends inside a vertex");
    EXPECT(!ComputeBox(Vertices(24, 8, pts), VertexLayout { 0, 0, 2 }, {}, b), "stride 0");
    EXPECT(!ComputeBox(Vertices(24, 8, pts), VertexLayout { 129, 0, 2 }, {}, b), "stride over 128");
    EXPECT(!ComputeBox(Vertices(24, 8, pts), VertexLayout { 24, 20, 2 }, {}, b), "position does not fit (offset 20 + 8 > 24)");
    EXPECT(ComputeBox(Vertices(24, 8, pts), VertexLayout { 24, 16, 2 }, {}, b), "position fits exactly at offset 16");
    EXPECT(!ComputeBox(Vertices(24, 8, pts), VertexLayout { 24, 6, 2 }, {}, b), "unaligned offset");
    EXPECT(!ComputeBox(Vertices(24, 8, pts), VertexLayout { 24, 8, 1 }, {}, b), "one float is no position");
    EXPECT(!ComputeBox(Vertices(24, 8, pts), VertexLayout { 24, 8, 5 }, {}, b), "five floats is not a position");
    EXPECT(ComputeBox(Vertices(24, 8, pts), VertexLayout { 24, 8, 4 }, {}, b), "float4 position");
    EXPECT(!ComputeBox(Vertices(24, 8, pts), VertexLayout { 24, 12, 4 }, {}, b), "float4 at offset 12 does not fit");
    EXPECT(!ComputeBox(Vertices(24, 8, pts), VertexLayout { 24, 16, 3 }, {}, b), "float3 at offset 16 does not fit");
}

void Layouts()
{
    const VertexLayout l { 32, 8, 2 };
    EXPECT(LayoutUnderstood(32, 32, l), "all agree");
    EXPECT(!LayoutUnderstood(32, 24, l), "the bound buffer has another stride");
    EXPECT(!LayoutUnderstood(24, 32, VertexLayout { 32, 8, 2 }), "the slot has another stride");
    EXPECT(!LayoutUnderstood(32, 32, VertexLayout { 24, 8, 2 }), "the layout has another stride");
    EXPECT(!LayoutUnderstood(0, 0, VertexLayout { 0, 0, 2 }), "no stride");
    EXPECT(!LayoutUnderstood(136, 136, VertexLayout { 136, 0, 2 }), "stride over 128");
    EXPECT(LayoutUnderstood(32, 32, VertexLayout { 32, 24, 2 }) && !LayoutUnderstood(32, 32, VertexLayout { 32, 28, 2 }), "position must fit");
    EXPECT(!LayoutUnderstood(32, 32, VertexLayout { 32, 6, 2 }), "aligned to a float");
    EXPECT(!LayoutUnderstood(32, 32, VertexLayout { 32, 8, 1 }) && !LayoutUnderstood(32, 32, VertexLayout { 32, 8, 5 }), "float count");
    EXPECT(LayoutUnderstood(32, 32, VertexLayout { 32, 8, 3 }) && LayoutUnderstood(32, 32, VertexLayout { 32, 8, 4 }), "float3 and float4 positions");
    EXPECT(!LayoutUnderstood(32, 32, VertexLayout { 32, 20, 4 }) && LayoutUnderstood(32, 32, VertexLayout { 32, 16, 4 }), "the whole position must fit");
}

void Counting()
{
    Ordinals o;
    EXPECT(o.NextBatch(7) == 1 && o.NextDraw() == 1 && o.NextDraw() == 2 && o.NextDraw() == 3, "first batch of a frame");
    EXPECT(o.NextBatch(7) == 2 && o.NextDraw() == 1, "second batch: draws restart");
    EXPECT(o.NextBatch(7) == 3, "third batch");
    EXPECT(o.NextBatch(8) == 1 && o.NextDraw() == 1, "a new frame restarts the batches");
    EXPECT(o.NextBatch(8) == 2, "and counts on");
    Ordinals fresh;
    EXPECT(fresh.NextBatch(0) == 1, "frame 0 is a frame like any other");
}

std::unique_ptr<DrawRecord> Record(uint64_t command, uint64_t timeMs, uint32_t batch = 1, uint32_t draw = 1, size_t runBytes = 0)
{
    auto r = std::make_unique<DrawRecord>();
    r->command = command;
    r->commandBytes = { 1, 2, 3, 4, 5, 6, 7, 8 };
    r->timeMs = timeMs;
    r->batch = batch;
    r->batchDraw = draw;
    r->indexCount = 36;
    r->startIndex = 120;
    r->baseVertex = 0;
    r->run.assign(runBytes, 0);
    return r;
}

const std::vector<unsigned char> kSame { 1, 2, 3, 4, 5, 6, 7, 8 };

void Map()
{
    AssocMap m;
    std::unique_ptr<DrawRecord> out;
    EXPECT(m.Store(Record(0x1000, 100), 100) == AssocMap::Put::Stored && m.Size() == 1 && m.Bytes() == Record(0x1000, 0)->Bytes(), "stored");
    EXPECT(m.Has(0x1000) && !m.Has(0x2000) && !m.Has(0xFFF) && !m.Has(0x1001), "membership by command address");
    EXPECT(m.Take(0x2000, kSame, 100, out) == AssocMap::Bind::NotFound && !out, "not found");
    EXPECT(m.Size() == 1, "a miss leaves the map alone");
    EXPECT(m.Take(0x1000, kSame, 150, out) == AssocMap::Bind::Bound && out && out->command == 0x1000, "bound");
    EXPECT(m.Size() == 0 && m.Bytes() == 0 && !m.Has(0x1000), "taking removes the record and its bytes");

    // age: 250 ms is still fine, 251 is stale, and a stale record is gone either way
    m.Store(Record(0x1000, 100), 100);
    EXPECT(m.Take(0x1000, kSame, 350, out) == AssocMap::Bind::Bound, "250 ms old");
    m.Store(Record(0x1000, 100), 100);
    EXPECT(m.Take(0x1000, kSame, 351, out) == AssocMap::Bind::Stale && !out && m.Size() == 0, "251 ms old");

    // the command changed under the same address
    m.Store(Record(0x1000, 100), 100);
    const std::vector<unsigned char> other { 1, 2, 3, 4, 5, 6, 7, 9 };
    EXPECT(m.Take(0x1000, other, 110, out) == AssocMap::Bind::Changed && !out && m.Size() == 0, "different bytes");
    m.Store(Record(0x1000, 100), 100);
    const std::vector<unsigned char> shorter { 1, 2, 3, 4, 5, 6, 7 };
    EXPECT(m.Take(0x1000, shorter, 110, out) == AssocMap::Bind::Changed, "different length");

    // a new record at an address replaces the old one
    m.Store(Record(0x1000, 100, 1, 1), 100);
    m.Store(Record(0x1000, 120, 2, 5), 120);
    EXPECT(m.Size() == 1 && m.Take(0x1000, kSame, 130, out) == AssocMap::Bind::Bound && out->batch == 2 && out->batchDraw == 5, "replaced");

    // old records are dropped when a new one comes in
    AssocMap p;
    p.Store(Record(1, 100), 100);
    p.Store(Record(2, 200), 200);
    p.Store(Record(3, 351), 351);
    EXPECT(p.Size() == 2 && p.Take(1, kSame, 351, out) == AssocMap::Bind::NotFound, "the 251 ms old record was pruned");
    p.Prune(700);
    EXPECT(p.Size() == 0 && p.Bytes() == 0, "prune");

    // capacity: records and bytes
    AssocMap full;
    for (uint64_t i = 0; i < kMaxRecords; ++i)
        EXPECT(full.Store(Record(0x100 + i, 100), 100) == AssocMap::Put::Stored, "record %llu", (unsigned long long) i);
    EXPECT(full.Store(Record(0x9999, 100), 100) == AssocMap::Put::Full && full.Size() == kMaxRecords, "256 records is the limit");
    EXPECT(full.Store(Record(0x100, 100), 100) == AssocMap::Put::Stored && full.Size() == kMaxRecords, "replacing a record fits");
    full.Clear();
    EXPECT(full.Size() == 0 && full.Bytes() == 0, "clear");
    const size_t big = (4u << 20) - 4096;
    EXPECT(full.Store(Record(1, 100, 1, 1, big), 100) == AssocMap::Put::Stored, "4 MiB");
    EXPECT(full.Store(Record(2, 100, 1, 2, big), 100) == AssocMap::Put::Stored, "8 MiB minus the headers fits");
    EXPECT(full.Store(Record(3, 100, 1, 3, 16384), 100) == AssocMap::Put::Full, "bytes are the limit as well");

    // the lookup by ordinal, both numbers 1-based
    AssocMap ord;
    ord.Store(Record(10, 100, 1, 1), 100);
    ord.Store(Record(11, 100, 1, 2), 100);
    ord.Store(Record(12, 100, 2, 1), 100);
    EXPECT(ord.FindByOrdinal(1, 1) && ord.FindByOrdinal(1, 1)->command == 10, "batch 1 draw 1");
    EXPECT(ord.FindByOrdinal(1, 2) && ord.FindByOrdinal(1, 2)->command == 11, "batch 1 draw 2");
    EXPECT(ord.FindByOrdinal(2, 1) && ord.FindByOrdinal(2, 1)->command == 12, "batch 2 draw 1");
    EXPECT(!ord.FindByOrdinal(2, 2) && !ord.FindByOrdinal(0, 1) && !ord.FindByOrdinal(1, 0) && !ord.FindByOrdinal(3, 1), "no other record");
}

void Matching()
{
    auto r = Record(0x1000, 100);
    EXPECT(MatchDraw(*r, 'I', 36, 120, 0, 100) == DrawMatch::Ok, "the draw of the record");
    EXPECT(MatchDraw(*r, 'I', 36, 120, 0, 350) == DrawMatch::Ok, "250 ms later");
    EXPECT(MatchDraw(*r, 'I', 36, 120, 0, 351) == DrawMatch::Stale, "251 ms later");
    EXPECT(MatchDraw(*r, 'I', 37, 120, 0, 100) == DrawMatch::Mismatch, "index count");
    EXPECT(MatchDraw(*r, 'I', 35, 120, 0, 100) == DrawMatch::Mismatch, "index count below");
    EXPECT(MatchDraw(*r, 'I', 36, 121, 0, 100) == DrawMatch::Mismatch, "start index");
    EXPECT(MatchDraw(*r, 'I', 36, 120, 1, 100) == DrawMatch::Mismatch, "base vertex");
    EXPECT(MatchDraw(*r, 'I', 36, 120, -1, 100) == DrawMatch::Mismatch, "base vertex below");
    EXPECT(MatchDraw(*r, 'X', 36, 120, 0, 100) == DrawMatch::Mismatch, "not a plain indexed draw");
    EXPECT(MatchDraw(*r, 'D', 36, 120, 0, 100) == DrawMatch::Mismatch, "not indexed");
    EXPECT(MatchDraw(*r, 'I', 37, 120, 0, 9999) == DrawMatch::Mismatch, "a different draw is a mismatch before it is stale");
    r->baseVertex = -4;
    EXPECT(MatchDraw(*r, 'I', 36, 120, -4, 100) == DrawMatch::Ok, "negative base vertex");
}

void Commands()
{
    const CommandOffsets o { 0x00, 0x08, 0x14, 0x18 };
    std::vector<unsigned char> c(0xB0, 0);
    const uint32_t type = 6, count = 36, start = 120;
    const int32_t base = -3;
    std::memcpy(c.data() + o.type, &type, 4);
    std::memcpy(c.data() + o.indexCount, &count, 4);
    std::memcpy(c.data() + o.startIndex, &start, 4);
    std::memcpy(c.data() + o.baseVertex, &base, 4);
    CommandFields f = ParseCommand(c, o);
    EXPECT(f.ok && f.type == 6 && f.indexCount == 36 && f.startIndex == 120 && f.baseVertex == -3, "fields");
    c.resize(0x1C);
    EXPECT(ParseCommand(c, o).ok, "exactly long enough");
    c.resize(0x1B);
    EXPECT(!ParseCommand(c, o).ok, "one byte short");
    EXPECT(!ParseCommand({}, o).ok, "empty");
    CommandOffsets wild = o;
    wild.startIndex = 0xFFFFFFFEu;
    EXPECT(!ParseCommand(std::vector<unsigned char>(0xB0, 0), wild).ok, "a wild offset does not wrap");

    EXPECT(UsableIndexCount(3) && UsableIndexCount(36) && UsableIndexCount(262143), "usable counts (262143 is the largest multiple of 3 up to 256 KiB)");
    EXPECT(!UsableIndexCount(0) && !UsableIndexCount(4) && !UsableIndexCount(262146) && !UsableIndexCount(2), "unusable counts");
}

void Overlay()
{
    const float box[4] = { 100.0f, 50.0f, 300.0f, 90.0f };
    ScreenRect r = MapBox(OverlayUnits::Pixels, box, 3840, 2160, 3840, 2160);
    EXPECT(r.ok && r.x0 == 100.0f && r.y0 == 50.0f && r.x1 == 300.0f && r.y1 == 90.0f, "pixels, same size");
    r = MapBox(OverlayUnits::Pixels, box, 3840, 2160, 1920, 1080);
    EXPECT(r.ok && r.x0 == 200.0f && r.y0 == 100.0f && r.x1 == 600.0f && r.y1 == 180.0f, "pixels of a smaller viewport scale up");
    r = MapBox(OverlayUnits::Pixels, box, 1920, 1080, 3840, 1080);
    EXPECT(r.ok && r.x0 == 50.0f && r.y0 == 50.0f && r.x1 == 150.0f && r.y1 == 90.0f, "each axis has its own factor");
    EXPECT(!MapBox(OverlayUnits::Pixels, box, 3840, 2160, 0, 1080).ok && !MapBox(OverlayUnits::Pixels, box, 3840, 2160, 1920, 0).ok, "no viewport size");
    EXPECT(!MapBox(OverlayUnits::Pixels, box, 0, 2160, 1920, 1080).ok && !MapBox(OverlayUnits::Pixels, box, 3840, 0, 1920, 1080).ok, "no display size");

    const float unit[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    r = MapBox(OverlayUnits::Unit, unit, 1000, 400, 0, 0);
    EXPECT(r.ok && r.x0 == 250.0f && r.y0 == 200.0f && r.x1 == 750.0f && r.y1 == 400.0f, "0..1 across the display (the viewport size is not used)");

    const float ndc[4] = { -1.0f, -1.0f, 0.0f, 0.5f };
    r = MapBox(OverlayUnits::Ndc, ndc, 1000, 400, 0, 0);
    EXPECT(r.ok && r.x0 == 0.0f && r.x1 == 500.0f && r.y0 == 100.0f && r.y1 == 400.0f, "-1..1, y up (%.1f %.1f %.1f %.1f)", r.x0, r.y0, r.x1, r.y1);

    const float swapped[4] = { 300.0f, 90.0f, 100.0f, 50.0f };
    r = MapBox(OverlayUnits::Pixels, swapped, 3840, 2160, 3840, 2160);
    EXPECT(r.ok && r.x0 == 100.0f && r.y0 == 50.0f && r.x1 == 300.0f && r.y1 == 90.0f, "a box given upside down is put right");
    const float nan[4] = { 0.0f, kNaN, 1.0f, 1.0f };
    EXPECT(!MapBox(OverlayUnits::Unit, nan, 100, 100, 100, 100).ok, "not a number");
    const float inf[4] = { 0.0f, 0.0f, kInf, 1.0f };
    EXPECT(!MapBox(OverlayUnits::Ndc, inf, 100, 100, 100, 100).ok, "infinite");

    EXPECT_EQ(OverlayAssumption(OverlayUnits::Pixels, 3840, 2160, 3840, 2160), "raw box as pixels of the viewport, x1.00");
    EXPECT_EQ(OverlayAssumption(OverlayUnits::Pixels, 3840, 2160, 1920, 1080), "raw box as pixels of the viewport, x2.00");
    EXPECT_EQ(OverlayAssumption(OverlayUnits::Pixels, 1920, 1080, 3840, 1080), "raw box as pixels of the viewport, x0.50 by x1.00");
    EXPECT_EQ(OverlayAssumption(OverlayUnits::Pixels, 1920, 1080, 0, 0), "raw box as pixels of the viewport, no viewport size");
    EXPECT_EQ(OverlayAssumption(OverlayUnits::Unit, 3840, 2160, 0, 0), "raw box as 0..1 of the display, x3840 by x2160");
    EXPECT_EQ(OverlayAssumption(OverlayUnits::Ndc, 3840, 2160, 0, 0), "raw box as -1..1 of the display (y up), x1920 by x1080");
    EXPECT_EQ(OverlayLine(OverlayUnits::Pixels, 3840, 2160, 3840, 2160, 1200, 12), "plate probe: raw box as pixels of the viewport, x1.00, frame 1200, 12 boxes");
    EXPECT_EQ(OverlayLine(OverlayUnits::Unit, 3840, 2160, 0, 0, 7, 1), "plate probe: raw box as 0..1 of the display, x3840 by x2160, frame 7, 1 box");
    EXPECT_EQ(OverlayLine(OverlayUnits::Unit, 3840, 2160, 0, 0, 7, 0), "plate probe: raw box as 0..1 of the display, x3840 by x2160, frame 7, 0 boxes");

    // the menu's overlay table holds 8
    EXPECT(OverlaySlotFree(0, 8) && OverlaySlotFree(7, 8), "room");
    EXPECT(!OverlaySlotFree(8, 8) && !OverlaySlotFree(9, 8) && !OverlaySlotFree(0, 0), "no room");
    EXPECT(OverlayShouldDraw(true, 1200), "wanted, with a logged frame");
    EXPECT(!OverlayShouldDraw(true, 0), "no logged frame yet");
    EXPECT(!OverlayShouldDraw(false, 1200), "not wanted");
}

bool SameStats(const AssocStats& a, const AssocStats& b)
{
    return a.rows == b.rows && a.notIndexed == b.notIndexed && a.associated == b.associated && a.noPacket == b.noPacket && a.stale == b.stale &&
           a.changed == b.changed && a.mismatch == b.mismatch && a.boxes == b.boxes && a.noBox == b.noBox && a.noBoxNoRun == b.noBoxNoRun &&
           a.noBoxUntagged == b.noBoxUntagged && a.noBoxStrides == b.noBoxStrides && a.noBoxNonFinite == b.noBoxNonFinite && a.made == b.made &&
           a.refusedCapacity == b.refusedCapacity && a.refusedRead == b.refusedRead && a.refusedBusy == b.refusedBusy && a.unmapped == b.unmapped &&
           a.rowsCapped == b.rowsCapped && a.framesCapped == b.framesCapped && a.priorityCapped == b.priorityCapped && a.batchReads == b.batchReads &&
           a.batchCut == b.batchCut && a.headerCap == b.headerCap;
}

// the runtime keeps atomics and copies them for the summary line: they must read exactly what the plain counters read, field by field
void RuntimeCounters()
{
    AssocStats expected;
    AssocCounters counters;
    EXPECT(SameStats(counters.Snapshot(), AssocStats()) && !counters.Snapshot().Any(), "all zero at the start");
    const RowOutcome outcomes[] = { RowOutcome::NotIndexed, RowOutcome::Associated, RowOutcome::NoPacket,
                                    RowOutcome::Stale,      RowOutcome::Changed,    RowOutcome::Mismatch };
    const BoxReason reasons[] = { BoxReason::Ok, BoxReason::NoRun, BoxReason::Untagged, BoxReason::Strides, BoxReason::NonFinite };
    int times = 1; // every outcome with every reason, a different number of times, so a swapped field cannot read the same
    for (const RowOutcome outcome : outcomes)
        for (const BoxReason reason : reasons)
        {
            ++times;
            for (int i = 0; i < times; ++i)
            {
                expected.NoteRow(outcome, reason);
                counters.NoteRow(outcome, reason);
            }
        }
    AssocStats got = counters.Snapshot();
    // times runs 2..31; NotIndexed took 2..6, Associated 7..11, NoPacket 12..16, Stale 17..21, Changed 22..26, Mismatch 27..31
    EXPECT(got.notIndexed == 20 && got.associated == 45 && got.noPacket == 70 && got.stale == 95 && got.changed == 120 && got.mismatch == 145,
           "outcomes %llu %llu %llu", (unsigned long long) got.notIndexed, (unsigned long long) got.associated, (unsigned long long) got.noPacket);
    EXPECT(got.boxes == 7 && got.noBoxNoRun == 8 && got.noBoxUntagged == 9 && got.noBoxStrides == 10 && got.noBoxNonFinite == 11, "box and its reasons");
    EXPECT(got.noBox == 38 && got.NoBoxReasons() == got.noBox && got.associated == got.boxes + got.noBox, "no box = the sum of its reasons");
    EXPECT(SameStats(got, expected), "the atomics read what the plain counters read");
    EXPECT(got.rows == got.notIndexed + got.associated + got.noPacket + got.Rejected(), "every row is one outcome");

    for (int i = 0; i < 3; ++i)
        AssocCounters::Bump(counters.made);
    for (int i = 0; i < 5; ++i)
        AssocCounters::Bump(counters.refusedCapacity);
    for (int i = 0; i < 7; ++i)
        AssocCounters::Bump(counters.refusedRead);
    for (int i = 0; i < 11; ++i)
        AssocCounters::Bump(counters.refusedBusy);
    for (int i = 0; i < 13; ++i)
        AssocCounters::Bump(counters.unmapped);
    for (int i = 0; i < 17; ++i)
        AssocCounters::Bump(counters.batchReads);
    for (int i = 0; i < 19; ++i)
        AssocCounters::Bump(counters.batchCut);
    for (int i = 0; i < 23; ++i)
        AssocCounters::Bump(counters.headerCap);
    got = counters.Snapshot();
    EXPECT(got.made == 3 && got.refusedCapacity == 5 && got.refusedRead == 7 && got.refusedBusy == 11 && got.unmapped == 13, "enqueue side");
    EXPECT(got.batchReads == 17 && got.batchCut == 19 && got.headerCap == 23, "batch reads, cut batches, refused batches");
    EXPECT(got.stale == 95, "the row side is untouched by them");

    // the row cap: frames that lost rows, rows lost, priority rows among them
    counters.NoteCappedFrame(60, 0);
    counters.NoteCappedFrame(10, 4);
    expected = AssocStats();
    expected.NoteCappedFrame(60, 0);
    expected.NoteCappedFrame(10, 4);
    got = counters.Snapshot();
    EXPECT(got.rowsCapped == 70 && got.framesCapped == 2 && got.priorityCapped == 4, "capped frames %llu %llu %llu",
           (unsigned long long) got.rowsCapped, (unsigned long long) got.framesCapped, (unsigned long long) got.priorityCapped);
    EXPECT(got.rowsCapped == expected.rowsCapped && got.framesCapped == expected.framesCapped && got.priorityCapped == expected.priorityCapped,
           "same as the plain counters");
    EXPECT(&Counters() == &Counters(), "one set per process");
}

// ---- the time budget, the packet list limit, and block reads -----------------------------------------------------------
void Budget()
{
    EXPECT(BudgetTicks(10000000, 2) == 20000 && BudgetTicks(1000, 2) == 2 && BudgetTicks(2500, 2) == 5 && BudgetTicks(1999, 2) == 3 &&
               BudgetTicks(0, 2) == 0,
           "ticks in a budget");
    EXPECT(kBatchBudgetMs == 2 && kMaxBatchHeaders == 2048, "the named limits");
    // OverBudget: spent when the budget has passed, exactly the budget counts
    EXPECT(!OverBudget(1000, 1999, 1000) && OverBudget(1000, 2000, 1000) && OverBudget(1000, 2001, 1000), "one tick before, at, after the budget");
    EXPECT(!OverBudget(5000, 4000, 1) && !OverBudget(5000, 5000, 1) && OverBudget(5000, 5001, 1), "a clock that went back has spent nothing");
    EXPECT(OverBudget(7, 7, 0) && !OverBudget(7, 6, 0), "a budget of nothing is spent at once");

    ReadBudget budget;
    EXPECT(!budget.Exhausted(), "a budget nobody began never runs out");
    budget.Begin(100);
    EXPECT(!budget.Exhausted(), "fresh");
    budget.Add(10, 60);
    EXPECT(budget.spent == 50 && !budget.Exhausted(), "50 of 100");
    budget.Add(200, 200);
    budget.Add(300, 250); // a clock that went back adds nothing
    EXPECT(budget.spent == 50, "only forward time is charged");
    budget.Add(0, 49);
    EXPECT(budget.spent == 99 && !budget.Exhausted(), "99 of 100");
    budget.Add(0, 1);
    EXPECT(budget.spent == 100 && budget.Exhausted(), "100 of 100 is spent");
    budget.Begin(100);
    EXPECT(budget.spent == 0 && !budget.Exhausted(), "a new batch call starts again");

    // the limit on the number of packets in a batch call
    EXPECT(CheckPacketList(0) == ListCheck::Empty, "empty list");
    EXPECT(CheckPacketList(1) == ListCheck::Ok && CheckPacketList(2047) == ListCheck::Ok && CheckPacketList(2048) == ListCheck::Ok, "up to the limit");
    EXPECT(CheckPacketList(2049) == ListCheck::TooMany && CheckPacketList(8192) == ListCheck::TooMany && CheckPacketList(0xFFFFFFFFu) == ListCheck::TooMany,
           "above the limit");
}

void BlockPlans()
{
    const uint32_t h = 64;
    auto plan = [&](std::vector<uint64_t> addresses, uint32_t maxBlock = 4096, uint32_t maxGap = 0) { return PlanHeaderReads(addresses, h, maxBlock, maxGap); };

    // contiguous headers: one block, one call
    std::vector<uint64_t> contiguous;
    for (uint64_t i = 0; i < 100; ++i)
        contiguous.push_back(0x10000 + i * h);
    HeaderPlan p = PlanHeaderReads(contiguous, h, kHeaderBlockMaxBytes, kHeaderBlockMaxGapBytes);
    EXPECT(p.blocks.size() == 1 && p.blocks[0].address == 0x10000 && p.blocks[0].bytes == 100 * h && p.blocks[0].count == 100, "one block of 100 headers");
    EXPECT(p.members.size() == 100 && p.members[0].item == 0 && p.members[0].offset == 0 && p.members[99].item == 99 && p.members[99].offset == 99 * h,
           "members");
    // unsorted input and duplicates: sorted by address, each item once with its own offset, block bytes cover the last header
    p = plan({ 0x1000 + 2 * h, 0x1000, 0x1000 + h, 0x1000 + h });
    EXPECT(p.blocks.size() == 1 && p.blocks[0].address == 0x1000 && p.blocks[0].bytes == 3 * h && p.blocks[0].count == 4, "unsorted, duplicated");
    bool seen[4] = {};
    for (const BlockMember& m : p.members)
    {
        EXPECT(m.item < 4 && !seen[m.item], "each item once");
        seen[m.item] = true;
        const uint64_t addr[4] = { 0x1000 + 2 * h, 0x1000, 0x1000 + h, 0x1000 + h };
        EXPECT(p.blocks[0].address + m.offset == addr[m.item], "offset of item %u", m.item);
    }
    // a zero address has no header to read
    p = plan({ 0, 0x1000, 0 });
    EXPECT(p.blocks.size() == 1 && p.members.size() == 1 && p.members[0].item == 1, "zero addresses are skipped");
    EXPECT(plan({}).blocks.empty() && plan({ 0, 0 }).blocks.empty(), "nothing to read");

    // the gap rule: a header joins the block when it starts at most maxGap after the block's end
    p = plan({ 0x1000, 0x1000 + h + 100 }, 4096, 100);
    EXPECT(p.blocks.size() == 1 && p.blocks[0].bytes == h + 100 + h, "a gap of exactly the limit joins");
    p = plan({ 0x1000, 0x1000 + h + 101 }, 4096, 100);
    EXPECT(p.blocks.size() == 2 && p.blocks[0].bytes == h && p.blocks[1].address == 0x1000 + h + 101 && p.blocks[1].first == 1, "one more byte does not");
    p = plan({ 0x1000, 0x1000 + h }, 4096, 0);
    EXPECT(p.blocks.size() == 1, "touching headers with no gap allowed join");
    p = plan({ 0x1000, 0x1000 + h + 1 }, 4096, 0);
    EXPECT(p.blocks.size() == 2, "a gap of one byte with none allowed does not");
    // the block size rule
    p = plan({ 0x1000, 0x1000 + h, 0x1000 + 2 * h }, 3 * h, 0);
    EXPECT(p.blocks.size() == 1 && p.blocks[0].bytes == 3 * h, "a block of exactly the limit");
    p = plan({ 0x1000, 0x1000 + h, 0x1000 + 2 * h, 0x1000 + 3 * h }, 3 * h, 0);
    EXPECT(p.blocks.size() == 2 && p.blocks[0].count == 3 && p.blocks[1].count == 1 && p.blocks[1].address == 0x1000 + 3 * h && p.blocks[1].first == 3,
           "the fourth header starts a new block");
    EXPECT(PlanHeaderReads(contiguous, 0, 4096, 0).blocks.empty() && PlanHeaderReads(contiguous, 64, 63, 0).blocks.empty(), "bad sizes read nothing");
    // 2048 contiguous headers of 64 bytes are 128 KiB: one block with the shipped limits
    std::vector<uint64_t> many;
    for (uint64_t i = 0; i < kMaxBatchHeaders; ++i)
        many.push_back(0x200000 + i * h);
    EXPECT(PlanHeaderReads(many, h, kHeaderBlockMaxBytes, kHeaderBlockMaxGapBytes).blocks.size() == 1, "the largest batch is one call");
    // scattered headers: far apart, one block each
    p = PlanHeaderReads(std::vector<uint64_t> { 0x100000, 0x200000, 0x300000 }, h, kHeaderBlockMaxBytes, kHeaderBlockMaxGapBytes);
    EXPECT(p.blocks.size() == 3, "scattered headers");
}

// A fake game memory for CollectRanges: headers written at addresses, optional unreadable ranges.
struct FakeMemory
{
    std::vector<std::pair<uint64_t, std::vector<unsigned char>>> chunks;
    std::vector<std::pair<uint64_t, uint64_t>> unreadable; // [from, to)
    int reads = 0;

    void Put(uint64_t address, const std::vector<unsigned char>& bytes) { chunks.push_back({ address, bytes }); }
    bool Read(uint64_t address, void* out, size_t size)
    {
        ++reads;
        for (const auto& [from, to] : unreadable)
            if (address < to && address + size > from)
                return false;
        std::memset(out, 0, size);
        bool any = false;
        for (const auto& [base, bytes] : chunks)
            if (address < base + bytes.size() && address + size > base)
            {
                any = true;
                const uint64_t lo = (std::max)(address, base), hi = (std::min)(address + size, base + bytes.size());
                std::memcpy(static_cast<unsigned char*>(out) + (lo - address), bytes.data() + (lo - base), size_t(hi - lo));
            }
        return any;
    }
};

void Collect()
{
    const uint64_t alloc = 0x400000;
    const uint32_t slotOffset = 64, stride = 24; // layout 4
    const uint32_t headerBytes = 64;
    auto slot = [&](uint32_t layout, uint32_t& st, uint32_t& off)
    {
        if (layout != 4)
            return false;
        st = stride;
        off = slotOffset;
        return true;
    };
    auto vertexAddress = [&](uint32_t first) { return alloc + slotOffset + uint64_t(first) * stride; };

    // 20 packets of 4 vertices (vertices 0..79), headers contiguous from 0x50000 in list order = reverse address order
    FakeMemory memory;
    std::vector<QueueEntry> entries;
    for (uint32_t i = 0; i < 20; ++i)
    {
        const uint64_t at = 0x50000 + uint64_t(19 - i) * headerBytes;
        memory.Put(at, Header(0x22, 4, vertexAddress(i * 4), 4));
        entries.push_back({ 0x1000 + i, at });
    }
    ReadBudget budget;
    budget.Begin(1000000);
    uint64_t clock = 0;
    auto now = [&] { return clock += 1; };
    RangeSet set;
    CollectStats stats;
    CollectResult result = CollectResult::Cut;
    {
        FakeMemory& m = memory;
        result = CollectRanges(entries, alloc, kHeader, headerBytes, budget, [&](uint64_t a, void* o, size_t n) { return m.Read(a, o, n); }, now, slot, set, &stats);
    }
    EXPECT(result == CollectResult::Complete && set.Size() == 20, "all 20 packets");
    EXPECT(stats.reads == 1 && memory.reads == 1, "contiguous headers are one read, not 20: %u", stats.reads);
    for (uint32_t i = 0; i < 20; ++i)
    {
        const int64_t at = set.Find(int64_t(i) * 4 + 1);
        EXPECT(at >= 0 && set.At(size_t(at)).key == 0x1000 + i && set.At(size_t(at)).first == i * 4, "packet %u is where its header says", i);
    }

    // the same set as header by header reading, packet 5 has no header memory and packet 6 is not geometry
    FakeMemory partial;
    std::vector<QueueEntry> some;
    for (uint32_t i = 0; i < 8; ++i)
    {
        const uint64_t at = 0x60000 + uint64_t(i) * headerBytes;
        if (i != 5)
            partial.Put(at, Header(i == 6 ? 0x30 : 0x22, 4, vertexAddress(i * 4), 4));
        some.push_back({ 0x2000 + i, at });
    }
    some.push_back({ 0x2008, 0 }); // an entry without a header address
    ReadBudget b2;
    b2.Begin(1000000);
    RangeSet set2;
    CollectStats stats2;
    EXPECT(CollectRanges(some, alloc, kHeader, headerBytes, b2, [&](uint64_t a, void* o, size_t n) { return partial.Read(a, o, n); }, now, slot, set2, &stats2) ==
               CollectResult::Complete,
           "complete");
    EXPECT(set2.Size() == 6, "packets 5 (unreadable zeros) and 6 (kind 0x30) and the entry without an address are not in the set: %zu", set2.Size());
    EXPECT(set2.Find(5 * 4) < 0 && set2.Find(6 * 4) < 0 && set2.Find(7 * 4) >= 0, "lookups");

    // list order decides ties: two entries naming the same vertices keep the first one's key (as a header by header reading does)
    FakeMemory twin;
    twin.Put(0x70000, Header(0x22, 4, vertexAddress(0), 4));
    twin.Put(0x70040, Header(0x22, 4, vertexAddress(0), 4));
    std::vector<QueueEntry> pair { { 0xAAA, 0x70040 }, { 0xBBB, 0x70000 } }; // the first entry has the HIGHER address
    ReadBudget b3;
    b3.Begin(1000000);
    RangeSet set3;
    CollectRanges(pair, alloc, kHeader, headerBytes, b3, [&](uint64_t a, void* o, size_t n) { return twin.Read(a, o, n); }, now, slot, set3);
    EXPECT(set3.Size() == 2 && set3.At(0).key == 0xAAA && set3.At(1).key == 0xBBB, "ranges are added in list order");

    // a block that cannot be read is read header by header; a header whose own read fails is skipped
    FakeMemory split;
    std::vector<QueueEntry> four;
    for (uint32_t i = 0; i < 4; ++i)
    {
        const uint64_t at = 0x80000 + uint64_t(i) * headerBytes;
        split.Put(at, Header(0x22, 4, vertexAddress(i * 4), 4));
        four.push_back({ 0x3000 + i, at });
    }
    split.unreadable.push_back({ 0x80000 + 2 * headerBytes, 0x80000 + 3 * headerBytes }); // header 2: the block read fails, so does its own read
    ReadBudget b4;
    b4.Begin(1000000);
    RangeSet set4;
    CollectStats stats4;
    EXPECT(CollectRanges(four, alloc, kHeader, headerBytes, b4, [&](uint64_t a, void* o, size_t n) { return split.Read(a, o, n); }, now, slot, set4, &stats4) ==
               CollectResult::Complete,
           "complete");
    EXPECT(stats4.reads == 5 && set4.Size() == 3, "one failed block read, then four single reads: %u reads, %zu ranges", stats4.reads, set4.Size());
    EXPECT(set4.Find(2 * 4) < 0 && set4.Find(3 * 4) >= 0, "the unreadable header is missing, the others are there");

    // the time budget: the clock runs 1000 ticks per call, the budget is 1500, scattered headers are one block each. The third block is never read.
    FakeMemory scattered;
    std::vector<QueueEntry> far;
    for (uint32_t i = 0; i < 6; ++i)
    {
        const uint64_t at = 0x100000 + uint64_t(i) * 0x100000;
        scattered.Put(at, Header(0x22, 4, vertexAddress(i * 4), 4));
        far.push_back({ 0x4000 + i, at });
    }
    ReadBudget tight;
    tight.Begin(1500);
    uint64_t t = 0;
    auto slowClock = [&] { return t += 1000; };
    RangeSet cut;
    CollectStats stats5;
    EXPECT(CollectRanges(far, alloc, kHeader, headerBytes, tight, [&](uint64_t a, void* o, size_t n) { return scattered.Read(a, o, n); }, slowClock, slot, cut,
                         &stats5) == CollectResult::Cut,
           "cut");
    EXPECT(cut.Size() == 0, "a cut batch has no ranges at all, so no draw can get a key from half a set: %zu", cut.Size());
    EXPECT(stats5.reads < 6 && stats5.reads >= 1, "reading stopped: %u reads", stats5.reads);
    {
        const std::vector<uint32_t> indices { 0, 1, 2 };
        const DrawUse use = Analyze(indices, 0, cut);
        EXPECT(!use.hasKey && use.mapped == 0, "no key from a cut batch");
    }
    // a budget that is spent exactly by the last read does not cut: every header was read
    ReadBudget exact;
    exact.Begin(500); // the read (250) and the decodes (250) use exactly all of it
    uint64_t t6 = 0;
    auto clock6 = [&] { return t6 += 250; }; // per block: now() before the read (start), after the read, after the decodes
    FakeMemory one;
    one.Put(0x90000, Header(0x22, 4, vertexAddress(0), 4));
    RangeSet set6;
    EXPECT(CollectRanges(std::vector<QueueEntry> { { 0x5000, 0x90000 } }, alloc, kHeader, headerBytes, exact,
                         [&](uint64_t a, void* o, size_t n) { return one.Read(a, o, n); }, clock6, slot, set6) == CollectResult::Complete &&
               set6.Size() == 1,
           "the last block is read even if it uses the budget up");
    // a budget that is already spent reads nothing
    ReadBudget spent;
    spent.Begin(10);
    spent.Add(0, 10);
    RangeSet set7;
    FakeMemory untouched;
    EXPECT(CollectRanges(std::vector<QueueEntry> { { 0x5000, 0x90000 } }, alloc, kHeader, headerBytes, spent,
                         [&](uint64_t a, void* o, size_t n) { return untouched.Read(a, o, n); }, now, slot, set7) == CollectResult::Cut &&
               untouched.reads == 0,
           "no read after the budget is spent");
}

void BoxReasons()
{
    const uint32_t stride = 24;
    const VertexLayout layout { 0, 8, 2 }; // position at byte 8, two floats
    std::vector<unsigned char> good = Vertices(stride, 8, { { 10, 20 }, { 30, 40 }, { 50, 60 } });
    Box box;
    // order of the reasons: no run, untagged, strides, then the numbers
    EXPECT(ResolveBox({}, stride, true, layout, stride, {}, box) == BoxReason::NoRun, "an empty run first, even with a bad layout");
    EXPECT(ResolveBox({}, stride, false, layout, 0, {}, box) == BoxReason::NoRun, "an empty run comes before the layout is looked at");
    EXPECT(ResolveBox(good, stride, false, layout, stride, {}, box) == BoxReason::Untagged, "no position tag");
    EXPECT(ResolveBox(good, stride, false, layout, 7, {}, box) == BoxReason::Untagged, "untagged comes before strides");
    EXPECT(ResolveBox(good, stride, true, layout, stride + 8, {}, box) == BoxReason::Strides, "bound stride differs");
    EXPECT(ResolveBox(good, 0, true, layout, 0, {}, box) == BoxReason::Strides, "a stride of nothing");
    EXPECT(ResolveBox(good, kMaxVertexStride + 4, true, layout, kMaxVertexStride + 4, {}, box) == BoxReason::Strides, "a stride too big");
    EXPECT(ResolveBox(good, stride, true, VertexLayout { 0, 8, 1 }, stride, {}, box) == BoxReason::Strides, "a one-float position does not fit");
    EXPECT(ResolveBox(good, stride, true, VertexLayout { 0, 20, 2 }, stride, {}, box) == BoxReason::Strides, "a position that runs out of the vertex");
    // a run that is not whole vertices, or flags that do not match it, is a run problem
    std::vector<unsigned char> ragged(good.begin(), good.end() - 4);
    EXPECT(ResolveBox(ragged, stride, true, layout, stride, {}, box) == BoxReason::NoRun, "not whole vertices");
    const std::vector<uint8_t> shortFlags { 1, 1 };
    EXPECT(ResolveBox(good, stride, true, layout, stride, shortFlags, box) == BoxReason::NoRun, "flags of another length");
    // nothing used at all: no vertex was looked at, which is not a number problem
    const std::vector<uint8_t> none { 0, 0, 0 };
    EXPECT(ResolveBox(good, stride, true, layout, stride, none, box) == BoxReason::NoRun, "no vertex used");
    // all used positions are not numbers
    std::vector<unsigned char> bad = Vertices(stride, 8, { { kNaN, 1 }, { 2, kInf } });
    EXPECT(ResolveBox(bad, stride, true, layout, stride, {}, box) == BoxReason::NonFinite, "every position is not finite");
    const std::vector<uint8_t> second { 0, 1 };
    EXPECT(ResolveBox(bad, stride, true, layout, stride, second, box) == BoxReason::NonFinite, "the only used one is not finite");
    // one finite vertex is enough
    std::vector<unsigned char> mixed = Vertices(stride, 8, { { kNaN, 1 }, { 5, 6 }, { 2, kInf } });
    EXPECT(ResolveBox(mixed, stride, true, layout, stride, {}, box) == BoxReason::Ok && box.x0 == 5 && box.y1 == 6 && box.skipped == 2, "a finite vertex");
    EXPECT(ResolveBox(good, stride, true, layout, stride, {}, box) == BoxReason::Ok && box.x0 == 10 && box.y0 == 20 && box.x1 == 50 && box.y1 == 60, "the box");
    const std::vector<uint8_t> first2 { 1, 1, 0 };
    EXPECT(ResolveBox(good, stride, true, layout, stride, first2, box) == BoxReason::Ok && box.x1 == 30 && box.y1 == 40, "used flags are honoured");
    // the layout's own stride field is ignored: the slot stride rules
    EXPECT(ResolveBox(good, stride, true, VertexLayout { 999, 8, 2 }, stride, {}, box) == BoxReason::Ok, "the slot stride is the stride");
}

// the whole path on a small fixture: two packets in one batch, one draw that reaches both, enqueue -> bind -> draw -> box
void Pipeline()
{
    const uint64_t alloc = 0x400000;
    const uint32_t slotOffset = 64, stride = 24; // layout 4
    RangeSet set;
    PacketRange range;
    // packet A: vertices 4..7, packet B: vertices 8..11 (a draw reaches both), packet C: vertices 40..43 (not reached)
    for (const auto& [key, first] : std::vector<std::pair<uint64_t, uint32_t>> { { 0x30, 8 }, { 0x10, 4 }, { 0x20, 40 } })
    {
        const PacketHeader h = ParseHeader(Header(0x22, 4, alloc + slotOffset + uint64_t(first) * stride, 4), kHeader);
        EXPECT(MakeRange(h, key, alloc, slotOffset, stride, range) == RangeResult::Ok, "range of packet %llx", (unsigned long long) key);
        set.Add(range);
    }
    set.Seal();
    const std::vector<uint32_t> indices { 4, 5, 6, 6, 5, 7, 8, 9, 10, 10, 9, 11 };
    const DrawUse use = Analyze(indices, 0, set);
    EXPECT(use.entries == 2 && use.qkey == 0x10 && use.unmapped == 0 && use.minVertex == 4 && use.maxVertex == 11, "the draw reaches packets 10 and 30");
    const RunPlan plan = PlanRun(use, stride, kMaxRunBytes);
    EXPECT(plan.ok && plan.first == 4 && plan.count == 8, "run of 8 vertices");

    const std::vector<std::pair<float, float>> pts { { 10, 20 }, { 30, 20 }, { 10, 40 }, { 30, 40 }, { 100, 120 }, { 140, 120 }, { 100, 150 }, { 140, 150 } };
    auto record = std::make_unique<DrawRecord>();
    record->command = 0x7FF0000;
    record->commandBytes = { 9, 9, 9 };
    record->timeMs = 1000;
    record->batch = 3;
    record->batchDraw = 2;
    record->indexCount = 12;
    record->startIndex = 60;
    record->hasKey = use.hasKey;
    record->qkey = use.qkey;
    record->entries = use.entries;
    record->layout = use.layout;
    record->stride = stride;
    record->run = Vertices(stride, 8, pts);
    record->used = MarkUsed(indices, 0, plan);

    AssocMap map;
    EXPECT(map.Store(std::move(record), 1000) == AssocMap::Put::Stored, "stored");
    EXPECT(map.FindByOrdinal(3, 2) != nullptr, "found by ordinal");
    std::unique_ptr<DrawRecord> bound;
    EXPECT(map.Take(0x7FF0000, std::vector<unsigned char> { 9, 9, 9 }, 1020, bound) == AssocMap::Bind::Bound, "bound");
    EXPECT(MatchDraw(*bound, 'I', 12, 60, 0, 1030) == DrawMatch::Ok, "the draw");
    Box box;
    EXPECT(LayoutUnderstood(stride, stride, VertexLayout { stride, 8, 2 }), "layout");
    EXPECT(ComputeBox(bound->run, VertexLayout { stride, 8, 2 }, bound->used, box), "box");
    EXPECT(box.x0 == 10 && box.y0 == 20 && box.x1 == 140 && box.y1 == 150 && box.vertices == 8, "the box of both packets");
    EXPECT(bound->qkey == 0x10 && bound->entries == 2 && bound->batch == 3 && bound->batchDraw == 2, "what the row carries");
}

// ---- literal limits and boundaries found unpinned by the mutation checks -------------------------------------------------
static_assert(kMaxHeaderBytes == 0x1000 && kMaxPacketVertices == 65536 && kMaxVertexStride == 128, "packet limits");
static_assert(kMaxAgeMs == 250 && kMaxRecords == 256 && kMaxRecordBytes == (8ull << 20) && kMaxRunBytes == (512ull << 10), "record limits");
static_assert(kBatchBudgetMs == 2 && kMaxBatchHeaders == 2048 && kHeaderBlockMaxBytes == (128u << 10) && kHeaderBlockMaxGapBytes == 4096, "batch read limits");

void LimitsAndBoundaries()
{
    // Inside: a field that exactly fills the bytes is inside
    EXPECT(Inside(0, 8, 8) && !Inside(0, 9, 8) && Inside(4, 4, 8) && !Inside(5, 4, 8) && Inside(8, 0, 8) && !Inside(9, 0, 8) && !Inside(0, 1, 0),
           "Inside");

    const uint64_t alloc = 0x200000;
    PacketRange r;
    // an unknown layout with a slot stride of nothing must not reach the division
    EXPECT(MakeRange(ParseHeader(Header(0x22, 8, alloc + 16, 9), kHeader), 1, alloc, 16, 0, r) == RangeResult::BadLayout, "unknown layout, stride 0");
    EXPECT(MakeRange(ParseHeader(Header(0x22, 8, alloc + 16, 1), kHeader), 1, alloc, 16, 0, r) == RangeResult::BadLayout, "layout 1, stride 0");
    // no allocation, with vertices that would be a clean vertex number from address 0
    EXPECT(MakeRange(ParseHeader(Header(0x22, 8, 16 + 40 * 3, 0), kHeader), 1, 0, 16, 40, r) == RangeResult::BadPointer, "allocation 0 is refused");
    // the last vertex number that fits in 32 bits
    EXPECT(MakeRange(ParseHeader(Header(0x22, 8, alloc + 16 + 40ull * 0xFFFFFFF7ull, 0), kHeader), 1, alloc, 16, 40, r) == RangeResult::Ok &&
               r.first == 0xFFFFFFF7u && r.count == 8,
           "first + count == 0xFFFFFFFF is the largest accepted");
    EXPECT(MakeRange(ParseHeader(Header(0x22, 8, alloc + 16 + 40ull * 0xFFFFFFF8ull, 0), kHeader), 1, alloc, 16, 40, r) == RangeResult::BadPointer,
           "one more is refused");

    // Find below the first range
    RangeSet set;
    set.Add({ 1, 100, 4, 0 });
    set.Seal();
    EXPECT(set.Find(50) == -1 && set.Find(99) == -1 && set.Find(100) == 0 && set.Find(103) == 0 && set.Find(104) == -1, "ranges that start at 100");

    // the layout of a draw is the layout of the packet of its first index
    RangeSet two;
    two.Add({ 0xA0, 0, 8, 0 });
    two.Add({ 0xB0, 8, 8, 5 });
    two.Seal();
    const std::vector<uint32_t> inSecond { 8, 9, 0, 1 };
    const DrawUse use = Analyze(inSecond, 0, two);
    EXPECT(use.hasKey && use.qkey == 0xB0 && use.layout == 5 && use.entries == 2, "key %llx layout %u", (unsigned long long) use.qkey, use.layout);

    // the stride limit is 128, inclusive
    const std::vector<std::pair<float, float>> pts { { 1.0f, 2.0f }, { 3.0f, 4.0f } };
    EXPECT(LayoutUnderstood(128, 128, VertexLayout { 128, 0, 2 }) && !LayoutUnderstood(129, 129, VertexLayout { 129, 0, 2 }), "layout stride 128/129");
    Box b;
    EXPECT(ComputeBox(Vertices(128, 8, pts), VertexLayout { 128, 8, 2 }, {}, b) && b.vertices == 2, "a box from 128-byte vertices");
    EXPECT(!ComputeBox(Vertices(132, 8, pts), VertexLayout { 132, 8, 2 }, {}, b), "132-byte vertices are over the limit");
    EXPECT(!ComputeBox(Vertices(129, 8, pts), VertexLayout { 129, 8, 3 }, {}, b) && !ComputeBox(Vertices(129, 8, pts), VertexLayout { 129, 8, 2 }, {}, b),
           "129 is over it too");

    // the plan of header reads
    const uint32_t h = 64;
    HeaderPlan p = PlanHeaderReads(std::vector<uint64_t> { 0x1000, 0x2000 }, h, 64, 0);
    EXPECT(p.blocks.size() == 2 && p.members.size() == 2, "a block limit equal to one header still reads one header per block: %zu", p.blocks.size());
    p = PlanHeaderReads(std::vector<uint64_t> { 0xFFFFFFFFFFFFFFF0ull, 0x1000 }, h, kHeaderBlockMaxBytes, kHeaderBlockMaxGapBytes);
    EXPECT(p.blocks.size() == 1 && p.members.size() == 1 && p.members[0].item == 1 && p.blocks[0].address == 0x1000,
           "an address too close to the top of memory has no header to read");
    p = PlanHeaderReads(std::vector<uint64_t> { 0x1000, 0x1000 + h + 4096 }, h, kHeaderBlockMaxBytes, kHeaderBlockMaxGapBytes);
    EXPECT(p.blocks.size() == 1, "a gap of 4096 joins");
    p = PlanHeaderReads(std::vector<uint64_t> { 0x1000, 0x1000 + h + 4097 }, h, kHeaderBlockMaxBytes, kHeaderBlockMaxGapBytes);
    EXPECT(p.blocks.size() == 2, "a gap of 4097 does not");

    // the usable index counts: multiples of 3 up to 262143
    EXPECT(UsableIndexCount(262143) && !UsableIndexCount(262144) && !UsableIndexCount(262146), "index count limit");
}

void MapEdges()
{
    // 256 records fit, the 257th does not
    AssocMap full;
    for (uint64_t i = 0; i < 256; ++i)
        EXPECT(full.Store(Record(0x100 + i, 100), 100) == AssocMap::Put::Stored, "record %llu", (unsigned long long) i);
    EXPECT(full.Store(Record(0x9999, 100), 100) == AssocMap::Put::Full && full.Size() == 256, "the 257th is refused");

    // the records may add up to exactly 8 MiB, not a byte more
    AssocMap m;
    const uint64_t header = Record(1, 0)->Bytes();
    const size_t run1 = (4u << 20) - 4096;
    const uint64_t first = Record(1, 100, 1, 1, run1)->Bytes();
    const size_t run2 = static_cast<size_t>((8ull << 20) - first - header);
    EXPECT(m.Store(Record(1, 100, 1, 1, run1), 100) == AssocMap::Put::Stored, "first");
    EXPECT(m.Store(Record(2, 100, 1, 2, run2), 100) == AssocMap::Put::Stored && m.Bytes() == (8ull << 20), "exactly 8 MiB: %llu", (unsigned long long) m.Bytes());
    EXPECT(m.Store(Record(3, 100, 1, 3, 0), 100) == AssocMap::Put::Full, "one more record of any size is over");

    // a record exactly 250 ms old is not pruned, 251 ms is
    AssocMap p;
    p.Store(Record(1, 100), 100);
    p.Prune(350);
    EXPECT(p.Size() == 1, "250 ms old survives Prune");
    p.Prune(351);
    EXPECT(p.Size() == 0 && p.Bytes() == 0, "251 ms old does not");

    // Erase gives the bytes back
    AssocMap e;
    e.Store(Record(5, 100, 1, 1, 1000), 100);
    EXPECT(e.Bytes() == Record(5, 0, 1, 1, 1000)->Bytes(), "bytes after Store");
    e.Erase(7);
    EXPECT(e.Size() == 1, "erasing a command that is not there changes nothing");
    e.Erase(5);
    EXPECT(e.Size() == 0 && e.Bytes() == 0, "bytes after Erase: %llu", (unsigned long long) e.Bytes());
}

void CollectEdges()
{
    const uint64_t alloc = 0x400000;
    const uint32_t slotOffset = 64, stride = 24; // layout 4
    const uint32_t headerBytes = 64;
    auto slot = [&](uint32_t layout, uint32_t& st, uint32_t& off)
    {
        if (layout != 4)
            return false;
        st = stride;
        off = slotOffset;
        return true;
    };
    auto vertexAddress = [&](uint32_t first) { return alloc + slotOffset + uint64_t(first) * stride; };
    uint64_t tick = 0;
    auto now = [&] { return tick += 1; };

    // two headers 3000 bytes apart share one read (the gap limit is 4096)
    {
        FakeMemory memory;
        memory.Put(0x50000, Header(0x22, 4, vertexAddress(0), 4));
        memory.Put(0x50000 + 3000, Header(0x22, 4, vertexAddress(4), 4));
        std::vector<QueueEntry> entries { { 0x1, 0x50000 }, { 0x2, 0x50000 + 3000 } };
        ReadBudget budget;
        budget.Begin(1000000);
        RangeSet set;
        CollectStats stats;
        EXPECT(CollectRanges(entries, alloc, kHeader, headerBytes, budget, [&](uint64_t a, void* o, size_t n) { return memory.Read(a, o, n); }, now, slot, set,
                             &stats) == CollectResult::Complete,
               "complete");
        EXPECT(stats.reads == 1 && set.Size() == 2, "3000 bytes apart: %u reads, %zu ranges", stats.reads, set.Size());
    }

    // entries listed in descending vertex order: the set is sorted, every packet is found
    {
        FakeMemory memory;
        std::vector<QueueEntry> entries;
        for (uint32_t i = 0; i < 12; ++i)
        {
            const uint32_t k = 11 - i; // the first entry holds the highest vertices
            const uint64_t at = 0x60000 + uint64_t(i) * headerBytes;
            memory.Put(at, Header(0x22, 4, vertexAddress(k * 4), 4));
            entries.push_back({ 0x1000 + k, at });
        }
        ReadBudget budget;
        budget.Begin(1000000);
        RangeSet set;
        CollectRanges(entries, alloc, kHeader, headerBytes, budget, [&](uint64_t a, void* o, size_t n) { return memory.Read(a, o, n); }, now, slot, set);
        EXPECT(set.Size() == 12, "12 ranges");
        for (uint32_t k = 0; k < 12; ++k)
        {
            const int64_t at = set.Find(int64_t(k) * 4 + 2);
            EXPECT(at >= 0 && set.At(size_t(at)).key == 0x1000 + k, "packet %u found", k);
        }
    }

    // a header that says "no vertices" gives no range
    {
        FakeMemory memory;
        memory.Put(0x70000, Header(0x22, 0, vertexAddress(0), 4));
        memory.Put(0x70040, Header(0x22, 4, vertexAddress(4), 4));
        std::vector<QueueEntry> entries { { 0x1, 0x70000 }, { 0x2, 0x70040 } };
        ReadBudget budget;
        budget.Begin(1000000);
        RangeSet set;
        CollectRanges(entries, alloc, kHeader, headerBytes, budget, [&](uint64_t a, void* o, size_t n) { return memory.Read(a, o, n); }, now, slot, set);
        EXPECT(set.Size() == 1 && set.At(0).key == 0x2, "a header with count 0 is not a packet: %zu ranges", set.Size());
    }

    // a block of one header that cannot be read is not read again
    {
        FakeMemory memory;
        memory.Put(0x80000, Header(0x22, 4, vertexAddress(0), 4));
        memory.unreadable.push_back({ 0x80000, 0x80000 + headerBytes });
        std::vector<QueueEntry> entries { { 0x1, 0x80000 } };
        ReadBudget budget;
        budget.Begin(1000000);
        RangeSet set;
        CollectStats stats;
        EXPECT(CollectRanges(entries, alloc, kHeader, headerBytes, budget, [&](uint64_t a, void* o, size_t n) { return memory.Read(a, o, n); }, now, slot, set,
                             &stats) == CollectResult::Complete,
               "complete");
        EXPECT(stats.reads == 1 && memory.reads == 1 && set.Size() == 0, "one read, no retry: %u reads", stats.reads);
    }

    // the budget runs out between the single reads that follow a failed block: the batch is cut and has no ranges
    {
        FakeMemory memory;
        std::vector<QueueEntry> entries;
        for (uint32_t i = 0; i < 3; ++i)
        {
            const uint64_t at = 0x90000 + uint64_t(i) * headerBytes;
            memory.Put(at, Header(0x22, 4, vertexAddress(i * 4), 4));
            entries.push_back({ 0x3000 + i, at });
        }
        memory.unreadable.push_back({ 0x90000 + 2 * headerBytes, 0x90000 + 3 * headerBytes });
        ReadBudget budget;
        budget.Begin(1500);
        uint64_t t = 0;
        auto slow = [&] { return t += 1000; }; // each timestamp is 1000 ticks after the one before
        RangeSet set;
        CollectStats stats;
        const CollectResult result = CollectRanges(entries, alloc, kHeader, headerBytes, budget, [&](uint64_t a, void* o, size_t n) { return memory.Read(a, o, n); },
                                                   slow, slot, set, &stats);
        EXPECT(result == CollectResult::Cut, "cut between single reads");
        EXPECT(set.Size() == 0, "and no ranges: %zu", set.Size());
        EXPECT(stats.reads == 2, "the block read and one single read, then stop: %u", stats.reads);
    }
}
} // namespace

int main()
{
    LimitsAndBoundaries();
    MapEdges();
    CollectEdges();
    Gate();
    Tables();
    Headers();
    Ranges();
    Find();
    Uses();
    Runs();
    Boxes();
    Layouts();
    Counting();
    Map();
    Matching();
    Commands();
    Overlay();
    RuntimeCounters();
    Budget();
    BlockPlans();
    Collect();
    BoxReasons();
    Pipeline();
    if (g_failures)
    {
        std::printf("FAIL: %d expectation(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: packet association (gate, headers, ranges, indices, run, box, ordinals, map, match, command, overlay mapping, pipeline)");
    return 0;
}
