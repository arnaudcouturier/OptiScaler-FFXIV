#pragma once
// Developer probes, packet association side (docs/HUDLESS.md, "Developer probes"): which game packets sit behind a UI draw, and where their
// vertices lie. Pure data and arithmetic: no I/O, no D3D, no clock (the caller passes the time in milliseconds), no game memory (the caller
// passes bytes it has read). tests/ext/hudless builds this header standalone.
//
// How the pieces fit (the runtime is ext/ffxiv/ui/PlatePackets.cpp):
//   batch call   the game submits a list of packets {queue key, header*}; each header names a run of vertices in the renderer's CPU vertex
//                allocation. ParseHeader + MakeRange turn the list into PacketRanges (vertex numbers), RangeSet finds the packet of a vertex.
//   enqueue      every draw command of the batch passes through the game's enqueue function. Its index list says which vertices the draw
//                reaches: Analyze (packets reached, queue key of the first, vertex run), then the bytes of that run are read and kept in a
//                DrawRecord, put in the AssocMap under the command's address.
//   bind         right before the draw the game binds the command's state: the record is taken out of the map (Bind) after the command bytes
//                are compared with the ones read at enqueue time (the pool reuses addresses).
//   draw         the next draw on the thread takes the bound record (MatchDraw compares index count, start index and base vertex), then
//                ComputeBox reads the position of every used vertex with the input layout the draw is bound with.
// Nothing here ever keeps a game pointer to dereference later, and every failure is a value, never an exception.

#include "ext/hudless/PlateProbeFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ext::hudless::plate
{
// ---- when packets are read ------------------------------------------------------------------------------------------
// Packet contents are read only for frames that draws.csv logs (the first 600, then every 100th). The game submits the batches of a frame
// from one thread and draws them from another, so the submitting side does not know the frame the draw will be logged under for sure:
// `frameHint` is the frame the drawing side is in (or is about to start), and the frame after it counts too.
inline bool ReadOnFrame(uint64_t frameHint) { return LogThisFrame(frameHint) || LogThisFrame(frameHint + 1); }

// ---- packet headers and vertex ranges -------------------------------------------------------------------------------
inline constexpr size_t kMaxHeaderBytes = 0x1000;
inline constexpr uint32_t kMaxPacketVertices = 65536;
inline constexpr uint32_t kMaxVertexStride = 128;

// The vertex stride of each layout the packet readers know (layout = format & 15). 0 = unknown layout.
inline uint32_t LayoutStride(uint32_t layout)
{
    switch (layout)
    {
    case 0: return 40;
    case 4: return 24;
    case 5:
    case 6: return 32;
    default: return 0;
    }
}

// Packets that carry plain geometry (the others are clip and effect records with no vertex run).
inline bool GeometryKind(uint32_t kind) { return kind == 0x10 || kind == 0x11 || (kind >= 0x20 && kind <= 0x23); }

struct HeaderOffsets
{
    uint32_t kind = 0, count = 0, vertices = 0, format = 0; // byte offsets inside the header
};

struct PacketHeader
{
    bool ok = false;
    uint32_t kind = 0, count = 0, layout = 0;
    uint64_t vertices = 0; // address of the first vertex in the renderer's CPU vertex allocation
};

inline bool Inside(uint64_t offset, uint64_t size, uint64_t bytes) { return size <= bytes && offset <= bytes - size; }

// Decodes the fields of a packet header copied out of the game. Any field outside the bytes makes the whole header unusable.
inline PacketHeader ParseHeader(std::span<const unsigned char> bytes, const HeaderOffsets& o)
{
    PacketHeader h;
    if (!Inside(o.kind, 4, bytes.size()) || !Inside(o.count, 4, bytes.size()) || !Inside(o.vertices, 8, bytes.size()) ||
        !Inside(o.format, 4, bytes.size()))
        return h;
    uint32_t format = 0;
    std::memcpy(&h.kind, bytes.data() + o.kind, 4);
    std::memcpy(&h.count, bytes.data() + o.count, 4);
    std::memcpy(&h.vertices, bytes.data() + o.vertices, 8);
    std::memcpy(&format, bytes.data() + o.format, 4);
    h.layout = format & 15;
    h.ok = true;
    return h;
}

// A packet as a run of vertex numbers of the renderer's vertex buffer: [first, first + count).
struct PacketRange
{
    uint64_t key = 0; // queue entry key (entry[0])
    uint32_t first = 0, count = 0, layout = 0;
};

enum class RangeResult : uint8_t
{
    Ok,
    BadHeader,
    NotGeometry,
    BadCount,
    BadLayout,   // not a layout the readers know, or the renderer's slot stride is not the stride of that layout
    BadPointer,  // the vertices lie before the slot's start or not on a vertex boundary
};

// `allocation` is the renderer's CPU vertex allocation, `slotOffset` and `slotStride` the byte offset and stride of the vertex buffer slot the
// layout draws from.
inline RangeResult MakeRange(const PacketHeader& h, uint64_t key, uint64_t allocation, uint32_t slotOffset, uint32_t slotStride, PacketRange& out)
{
    if (!h.ok)
        return RangeResult::BadHeader;
    if (!GeometryKind(h.kind))
        return RangeResult::NotGeometry;
    if (h.count == 0 || h.count > kMaxPacketVertices)
        return RangeResult::BadCount;
    const uint32_t expected = LayoutStride(h.layout);
    if (expected == 0 || slotStride != expected)
        return RangeResult::BadLayout;
    const uint64_t start = allocation + slotOffset;
    if (allocation == 0 || start < allocation || h.vertices < start || (h.vertices - start) % slotStride != 0)
        return RangeResult::BadPointer;
    const uint64_t first = (h.vertices - start) / slotStride;
    if (first + h.count > 0xFFFFFFFFull)
        return RangeResult::BadPointer;
    out = { key, static_cast<uint32_t>(first), h.count, h.layout };
    return RangeResult::Ok;
}

// The packets of one batch call, searchable by vertex number.
class RangeSet
{
  public:
    void Clear() { _ranges.clear(); }
    void Add(const PacketRange& r) { _ranges.push_back(r); }
    void Seal()
    {
        std::stable_sort(_ranges.begin(), _ranges.end(), [](const PacketRange& a, const PacketRange& b) { return a.first < b.first; });
    }
    size_t Size() const { return _ranges.size(); }
    const PacketRange& At(size_t i) const { return _ranges[i]; }

    // Index of the packet that holds vertex `v`, or -1. Ranges are expected not to overlap; the one that starts last at or before `v` is the
    // only candidate.
    int64_t Find(int64_t v) const
    {
        if (v < 0 || _ranges.empty())
            return -1;
        auto it = std::upper_bound(_ranges.begin(), _ranges.end(), v, [](int64_t value, const PacketRange& r) { return value < int64_t(r.first); });
        if (it == _ranges.begin())
            return -1;
        --it;
        return v < int64_t(it->first) + int64_t(it->count) ? int64_t(it - _ranges.begin()) : -1;
    }

  private:
    std::vector<PacketRange> _ranges; // sorted by `first` after Seal()
};

// ---- a draw's indices against the packets ---------------------------------------------------------------------------
struct DrawUse
{
    uint32_t indices = 0;   // indices looked at
    uint32_t mapped = 0;    // of them inside a known packet
    uint32_t unmapped = 0;  // outside every packet (or below vertex 0)
    uint32_t entries = 0;   // distinct packets reached
    bool hasKey = false;
    uint64_t qkey = 0;      // key of the packet of the first mapped index
    uint32_t layout = 0;    // layout of that packet
    bool hasRun = false;
    int64_t minVertex = 0, maxVertex = 0; // over every index that is a vertex number >= 0, mapped or not
};

// The vertex number of an index is index + base vertex, as the draw call computes it.
inline DrawUse Analyze(std::span<const uint32_t> indices, int32_t baseVertex, const RangeSet& ranges)
{
    DrawUse use;
    std::vector<uint8_t> seen(ranges.Size(), 0);
    for (const uint32_t index : indices)
    {
        ++use.indices;
        const int64_t v = int64_t(index) + int64_t(baseVertex);
        if (v < 0)
        {
            ++use.unmapped;
            continue;
        }
        if (!use.hasRun)
        {
            use.hasRun = true;
            use.minVertex = use.maxVertex = v;
        }
        else
        {
            use.minVertex = (std::min)(use.minVertex, v);
            use.maxVertex = (std::max)(use.maxVertex, v);
        }
        const int64_t at = ranges.Find(v);
        if (at < 0)
        {
            ++use.unmapped;
            continue;
        }
        ++use.mapped;
        if (!seen[size_t(at)])
        {
            seen[size_t(at)] = 1;
            ++use.entries;
        }
        if (!use.hasKey)
        {
            use.hasKey = true;
            use.qkey = ranges.At(size_t(at)).key;
            use.layout = ranges.At(size_t(at)).layout;
        }
    }
    return use;
}

// The contiguous run of vertices a draw reaches, to be read from the vertex allocation; empty when it would be too big.
struct RunPlan
{
    bool ok = false;
    uint64_t first = 0; // vertex number of the first vertex of the run
    uint32_t count = 0;
};

inline RunPlan PlanRun(const DrawUse& use, uint32_t stride, uint64_t maxBytes)
{
    RunPlan plan;
    if (!use.hasRun || stride == 0 || use.maxVertex < use.minVertex)
        return plan;
    const uint64_t count = uint64_t(use.maxVertex - use.minVertex) + 1;
    if (count > 0xFFFFFFFFull || count * stride > maxBytes)
        return plan;
    plan.ok = true;
    plan.first = uint64_t(use.minVertex);
    plan.count = static_cast<uint32_t>(count);
    return plan;
}

// One flag per vertex of the run: 1 when some index of the draw reaches it (so a vertex between two packets that the draw skips does not
// widen the box).
inline std::vector<uint8_t> MarkUsed(std::span<const uint32_t> indices, int32_t baseVertex, const RunPlan& plan)
{
    std::vector<uint8_t> used;
    if (!plan.ok)
        return used;
    used.assign(plan.count, 0);
    for (const uint32_t index : indices)
    {
        const int64_t v = int64_t(index) + int64_t(baseVertex) - int64_t(plan.first);
        if (v >= 0 && v < int64_t(plan.count))
            used[size_t(v)] = 1;
    }
    return used;
}

// ---- the vertex box -------------------------------------------------------------------------------------------------
struct VertexLayout
{
    uint32_t stride = 0;    // bytes per vertex
    uint32_t posOffset = 0; // byte offset of the POSITION element
    uint32_t posFloats = 0; // 2, 3 or 4 32-bit floats; the first two are x and y
};

// Whether the layout of a draw is understood well enough to read positions: the stride of the renderer's slot, the stride the vertex buffer
// is bound with and the stride of the packets agree, and the position element is floats that fit in the vertex.
inline bool LayoutUnderstood(uint32_t slotStride, uint32_t boundStride, const VertexLayout& l)
{
    return slotStride != 0 && slotStride <= kMaxVertexStride && slotStride == boundStride && l.stride == slotStride && l.posFloats >= 2 &&
           l.posFloats <= 4 && l.posOffset % 4 == 0 && l.posOffset + 4 * l.posFloats <= l.stride;
}

struct Box
{
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t vertices = 0; // vertices that took part
    uint32_t skipped = 0;  // vertices with a position that is not a finite number
};

// Bounding box of the x, y positions of the used vertices of `run` (`used` empty = all). False when the buffer cannot hold whole vertices of
// that stride, is empty, `used` does not match it, the layout cannot hold a position, or no vertex has a finite position.
inline bool ComputeBox(std::span<const unsigned char> run, const VertexLayout& layout, std::span<const uint8_t> used, Box& out)
{
    out = {};
    if (layout.stride == 0 || layout.stride > kMaxVertexStride || layout.posFloats < 2 || layout.posFloats > 4 || layout.posOffset % 4 != 0 ||
        layout.posOffset + 4 * layout.posFloats > layout.stride)
        return false;
    if (run.empty() || run.size() % layout.stride != 0)
        return false;
    const size_t vertices = run.size() / layout.stride;
    if (!used.empty() && used.size() != vertices)
        return false;
    bool any = false;
    for (size_t i = 0; i < vertices; ++i)
    {
        if (!used.empty() && !used[i])
            continue;
        float p[2];
        std::memcpy(p, run.data() + i * layout.stride + layout.posOffset, 8);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]))
        {
            ++out.skipped;
            continue;
        }
        if (!any)
        {
            out.x0 = out.x1 = p[0];
            out.y0 = out.y1 = p[1];
            any = true;
        }
        else
        {
            out.x0 = (std::min)(out.x0, p[0]);
            out.y0 = (std::min)(out.y0, p[1]);
            out.x1 = (std::max)(out.x1, p[0]);
            out.y1 = (std::max)(out.y1, p[1]);
        }
        ++out.vertices;
    }
    return any;
}

// Why a draw that has a packet has no box, as one reason (the order of the checks is the order of the reasons): no usable vertex bytes, a layout
// without a position tag, strides that disagree, or no used vertex with a finite position. `layout` is the position element of the draw's bound
// input layout (`tagged` false = the layout carries no tag), `slotStride` the stride the vertices were copied with, `boundStride` the stride of the
// bound vertex buffer. Every no-box path ends in exactly one reason.
inline BoxReason ResolveBox(std::span<const unsigned char> run, uint32_t slotStride, bool tagged, VertexLayout layout, uint32_t boundStride,
                            std::span<const uint8_t> used, Box& out)
{
    out = {};
    if (run.empty())
        return BoxReason::NoRun;
    if (!tagged)
        return BoxReason::Untagged;
    layout.stride = slotStride;
    if (!LayoutUnderstood(slotStride, boundStride, layout))
        return BoxReason::Strides;
    if (run.size() % slotStride != 0 || (!used.empty() && used.size() != run.size() / slotStride))
        return BoxReason::NoRun;
    if (ComputeBox(run, layout, used, out))
        return BoxReason::Ok;
    return out.skipped > 0 ? BoxReason::NonFinite : BoxReason::NoRun; // nothing finite among the used vertices, or no vertex was used at all
}

// ---- reading the packets of a batch call without taking long ----------------------------------------------------------
// The list of packets of one batch call is up to a few thousand {queue key, header address} entries. Reading each header with a call of its own
// costs a system call per packet; the headers of one batch sit close together, so they are read as blocks. The reading of one batch call also has
// a time budget (its headers and every draw it enqueues share it): a batch that runs out of time before all its headers are in is dropped as a
// whole, because a partial set of packets would give a draw the key of a LATER packet than the one that holds its first vertex.
inline constexpr uint32_t kBatchBudgetMs = 2;                // probe time one batch call may spend reading game memory
inline constexpr uint32_t kMaxBatchHeaders = 2048;           // hard limit: a batch call with more packets is not read (counted)
inline constexpr uint32_t kHeaderBlockMaxBytes = 128u << 10; // one block read covers at most this many bytes
inline constexpr uint32_t kHeaderBlockMaxGapBytes = 4096;    // headers up to this far apart share a block read

using QueueEntry = std::array<uint64_t, 2>; // {queue key, address of the packet header}

// Ticks of the clock (`freq` per second) in `ms` milliseconds.
inline uint64_t BudgetTicks(uint64_t freq, uint32_t ms) { return freq / 1000 * ms + freq % 1000 * ms / 1000; }

// The budget is spent when `budget` ticks or more have passed between the two timestamps. A clock that reads earlier than `start` has spent nothing.
inline bool OverBudget(uint64_t start, uint64_t now, uint64_t budget) { return now >= start && now - start >= budget; }

// The probe's own time in one batch call, added up: the time between operations belongs to the game and is not charged.
struct ReadBudget
{
    uint64_t limit = UINT64_MAX; // ticks; Begin() sets it, a budget nobody began never runs out
    uint64_t spent = 0;

    void Begin(uint64_t limitTicks)
    {
        limit = limitTicks;
        spent = 0;
    }
    void Add(uint64_t from, uint64_t to)
    {
        if (to > from)
            spent += to - from;
    }
    bool Exhausted() const { return OverBudget(0, spent, limit); }
};

enum class ListCheck : uint8_t { Ok, Empty, TooMany };

// Whether the packet list of a batch call is read: not empty, and not more than the hard limit.
inline ListCheck CheckPacketList(uint32_t count) { return count == 0 ? ListCheck::Empty : count > kMaxBatchHeaders ? ListCheck::TooMany : ListCheck::Ok; }

struct ReadBlock
{
    uint64_t address = 0;
    uint32_t bytes = 0;
    uint32_t first = 0, count = 0; // members [first, first + count) of HeaderPlan::members sit in this block
};

struct BlockMember
{
    uint32_t item = 0;   // index into the list of addresses
    uint32_t offset = 0; // byte offset of its header inside the block
};

struct HeaderPlan
{
    std::vector<ReadBlock> blocks; // ascending addresses
    std::vector<BlockMember> members;
};

// Groups the header addresses (a zero address is skipped) into block reads: sorted by address, a header joins the block of the one before it when
// it starts no more than `maxGapBytes` after that block's end and the block stays within `maxBlockBytes`.
inline HeaderPlan PlanHeaderReads(std::span<const uint64_t> addresses, uint32_t headerBytes, uint32_t maxBlockBytes, uint32_t maxGapBytes)
{
    HeaderPlan plan;
    if (headerBytes == 0 || maxBlockBytes < headerBytes)
        return plan;
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < addresses.size(); ++i)
        if (addresses[i] != 0 && addresses[i] <= UINT64_MAX - headerBytes - maxGapBytes)
            order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return addresses[a] < addresses[b]; });
    for (const uint32_t item : order)
    {
        const uint64_t a = addresses[item];
        if (!plan.blocks.empty())
        {
            ReadBlock& block = plan.blocks.back();
            const uint64_t end = block.address + block.bytes;
            if (a <= end + maxGapBytes && a + headerBytes - block.address <= maxBlockBytes)
            {
                block.bytes = static_cast<uint32_t>((std::max)(end, a + headerBytes) - block.address);
                plan.members.push_back({ item, static_cast<uint32_t>(a - block.address) });
                ++block.count;
                continue;
            }
        }
        plan.blocks.push_back({ a, headerBytes, static_cast<uint32_t>(plan.members.size()), 1 });
        plan.members.push_back({ item, 0 });
    }
    return plan;
}

enum class CollectResult : uint8_t
{
    Complete, // every header was read (or could not be: those packets are simply not in the set)
    Cut,      // the time budget ran out before the last header was read: `ranges` is left empty
};

struct CollectStats
{
    uint32_t reads = 0; // calls of `read` (blocks, and single headers after a block that failed)
};

// Reads the headers of `entries`, turns each into a vertex range and adds the ranges to `ranges` in list order (so the set is the one a header by
// header reading would give). `read(address, out, bytes)` returns whether the bytes were read, `now()` the clock, `slot(layout, stride, offset)`
// the renderer's slot of a layout. A block that cannot be read is read header by header. The set is sealed.
template <class Read, class Now, class Slot>
CollectResult CollectRanges(std::span<const QueueEntry> entries, uint64_t allocation, const HeaderOffsets& offsets, uint32_t headerBytes,
                            ReadBudget& budget, Read&& read, Now&& now, Slot&& slot, RangeSet& ranges, CollectStats* stats = nullptr)
{
    std::vector<uint64_t> addresses(entries.size());
    for (size_t i = 0; i < entries.size(); ++i)
        addresses[i] = entries[i][1];
    const HeaderPlan plan = PlanHeaderReads(addresses, headerBytes, kHeaderBlockMaxBytes, kHeaderBlockMaxGapBytes);
    std::vector<PacketRange> found(entries.size());
    std::vector<uint8_t> have(entries.size(), 0);
    std::vector<unsigned char> block;
    uint64_t last = now();
    auto spend = [&]
    {
        const uint64_t t = now();
        budget.Add(last, t);
        last = t;
    };
    auto decode = [&](uint32_t item, const unsigned char* bytes)
    {
        const PacketHeader h = ParseHeader(std::span<const unsigned char>(bytes, headerBytes), offsets);
        uint32_t stride = 0, slotOffset = 0;
        if (!h.ok || !GeometryKind(h.kind) || !slot(h.layout, stride, slotOffset))
            return;
        PacketRange range;
        if (MakeRange(h, entries[item][0], allocation, slotOffset, stride, range) == RangeResult::Ok)
        {
            found[item] = range;
            have[item] = 1;
        }
    };
    for (const ReadBlock& b : plan.blocks)
    {
        if (budget.Exhausted())
            return CollectResult::Cut;
        block.resize(b.bytes);
        const bool ok = read(b.address, block.data(), b.bytes);
        if (stats)
            ++stats->reads;
        spend();
        if (ok)
        {
            for (uint32_t k = b.first; k < b.first + b.count; ++k)
                decode(plan.members[k].item, block.data() + plan.members[k].offset);
            spend();
        }
        else if (b.count > 1)
        {
            for (uint32_t k = b.first; k < b.first + b.count; ++k)
            {
                if (budget.Exhausted())
                    return CollectResult::Cut;
                std::vector<unsigned char> one(headerBytes);
                const bool single = read(b.address + plan.members[k].offset, one.data(), headerBytes);
                if (stats)
                    ++stats->reads;
                if (single)
                    decode(plan.members[k].item, one.data());
                spend();
            }
        }
    }
    for (size_t i = 0; i < entries.size(); ++i)
        if (have[i])
            ranges.Add(found[i]);
    ranges.Seal();
    return CollectResult::Complete;
}

// ---- ordinals -------------------------------------------------------------------------------------------------------
// batch = 1-based ordinal of the game's UI batch call within a frame (it restarts when the frame hint moves), batch_draw = 1-based ordinal of
// the draw command within that batch call. One instance per submitting thread.
class Ordinals
{
  public:
    uint32_t NextBatch(uint64_t frameHint)
    {
        if (frameHint != _frame)
        {
            _frame = frameHint;
            _batch = 0;
        }
        _draw = 0;
        return ++_batch;
    }
    uint32_t NextDraw() { return ++_draw; }

  private:
    uint64_t _frame = ~0ull;
    uint32_t _batch = 0, _draw = 0;
};

// ---- the association map --------------------------------------------------------------------------------------------
inline constexpr uint64_t kMaxAgeMs = 250;             // enqueue to bind, and enqueue to draw
inline constexpr size_t kMaxRecords = 256;
inline constexpr uint64_t kMaxRecordBytes = 8ull << 20; // all records together
inline constexpr uint64_t kMaxRunBytes = 512ull << 10;  // vertex bytes of one record

// Everything the probe keeps about one draw command. No pointer into the game: `command` is only a key.
struct DrawRecord
{
    uint64_t command = 0;
    std::vector<unsigned char> commandBytes; // the draw command as read at enqueue time
    uint32_t batch = 0, batchDraw = 0;
    uint32_t indexCount = 0, startIndex = 0;
    int32_t baseVertex = 0;
    bool hasKey = false;
    uint64_t qkey = 0;
    uint32_t entries = 0;
    uint32_t layout = 0;
    uint32_t stride = 0;                     // slot stride the vertex run was read with
    std::vector<unsigned char> run;          // vertex bytes (empty = none)
    std::vector<uint8_t> used;               // one flag per vertex of the run
    uint64_t timeMs = 0;

    uint64_t Bytes() const { return sizeof(DrawRecord) + commandBytes.size() + run.size() + used.size(); }
};

class AssocMap
{
  public:
    enum class Put { Stored, Full };
    enum class Bind { Bound, NotFound, Stale, Changed };

    // A new record for `record->command`. Records older than the age limit are dropped first; an older record at the same address is replaced
    // (the command pool reuses addresses). Refused when the map holds too many records or bytes.
    Put Store(std::unique_ptr<DrawRecord> record, uint64_t nowMs)
    {
        Prune(nowMs);
        Erase(record->command);
        if (_records.size() >= kMaxRecords || _bytes + record->Bytes() > kMaxRecordBytes)
            return Put::Full;
        _bytes += record->Bytes();
        const uint64_t key = record->command;
        _records[key] = std::move(record);
        return Put::Stored;
    }

    void Erase(uint64_t command)
    {
        auto it = _records.find(command);
        if (it == _records.end())
            return;
        _bytes -= it->second->Bytes();
        _records.erase(it);
    }

    void Prune(uint64_t nowMs)
    {
        for (auto it = _records.begin(); it != _records.end();)
        {
            if (nowMs - it->second->timeMs > kMaxAgeMs)
            {
                _bytes -= it->second->Bytes();
                it = _records.erase(it);
            }
            else
                ++it;
        }
    }

    // The record of `command` leaves the map in every case but NotFound. It is handed out only when it is young enough and the command's
    // bytes are what they were at enqueue time.
    Bind Take(uint64_t command, std::span<const unsigned char> currentBytes, uint64_t nowMs, std::unique_ptr<DrawRecord>& out)
    {
        out.reset();
        auto it = _records.find(command);
        if (it == _records.end())
            return Bind::NotFound;
        std::unique_ptr<DrawRecord> found = std::move(it->second);
        _bytes -= found->Bytes();
        _records.erase(it);
        if (nowMs - found->timeMs > kMaxAgeMs)
            return Bind::Stale;
        if (currentBytes.size() != found->commandBytes.size() ||
            !std::equal(currentBytes.begin(), currentBytes.end(), found->commandBytes.begin()))
            return Bind::Changed;
        out = std::move(found);
        return Bind::Bound;
    }

    // The record submitted as draw `batchDraw` of batch call `batch` (both 1-based), or null.
    const DrawRecord* FindByOrdinal(uint32_t batch, uint32_t batchDraw) const
    {
        for (const auto& [command, record] : _records)
            if (record->batch == batch && record->batchDraw == batchDraw)
                return record.get();
        return nullptr;
    }

    bool Has(uint64_t command) const { return _records.find(command) != _records.end(); }
    size_t Size() const { return _records.size(); }
    uint64_t Bytes() const { return _bytes; }
    void Clear()
    {
        _records.clear();
        _bytes = 0;
    }

  private:
    std::map<uint64_t, std::unique_ptr<DrawRecord>> _records;
    uint64_t _bytes = 0;
};

// The draw that follows a bind must be the draw the record was made for: an indexed draw with the command's own index count, start index and
// base vertex, not older than the age limit.
enum class DrawMatch : uint8_t { Ok, Mismatch, Stale };

inline DrawMatch MatchDraw(const DrawRecord& r, char kind, uint32_t count, uint32_t start, int32_t baseVertex, uint64_t nowMs)
{
    if (kind != 'I' || count != r.indexCount || start != r.startIndex || baseVertex != r.baseVertex)
        return DrawMatch::Mismatch;
    if (nowMs - r.timeMs > kMaxAgeMs)
        return DrawMatch::Stale;
    return DrawMatch::Ok;
}

// ---- the draw command ------------------------------------------------------------------------------------------------
struct CommandOffsets
{
    uint32_t type = 0, baseVertex = 0, startIndex = 0, indexCount = 0; // byte offsets inside the draw command
};

struct CommandFields
{
    bool ok = false;
    uint32_t type = 0, indexCount = 0, startIndex = 0;
    int32_t baseVertex = 0;
};

inline CommandFields ParseCommand(std::span<const unsigned char> bytes, const CommandOffsets& o)
{
    CommandFields f;
    if (!Inside(o.type, 4, bytes.size()) || !Inside(o.baseVertex, 4, bytes.size()) || !Inside(o.startIndex, 4, bytes.size()) ||
        !Inside(o.indexCount, 4, bytes.size()))
        return f;
    std::memcpy(&f.type, bytes.data() + o.type, 4);
    std::memcpy(&f.indexCount, bytes.data() + o.indexCount, 4);
    std::memcpy(&f.startIndex, bytes.data() + o.startIndex, 4);
    std::memcpy(&f.baseVertex, bytes.data() + o.baseVertex, 4);
    f.ok = true;
    return f;
}

// A draw command worth looking at: whole triangles, and not more indices than the visibility layer accepts either.
inline bool UsableIndexCount(uint32_t count) { return count != 0 && count % 3 == 0 && count <= 256 * 1024; }

// ---- box units (the removed overlay read them three ways) -----------------------------------------------------------------
// The raw boxes are in the game's own vertex units, which nobody has measured against pixels yet. No product code reads them anymore; the mapping stays unit-tested.
enum class OverlayUnits : uint8_t
{
    Pixels = 0, // pixels of the viewport the draw was made with
    Unit = 1,   // 0..1 across the display, y down
    Ndc = 2,    // -1..1 across the display, y up
};

inline constexpr uint8_t kOverlayUnitCount = 3;

struct ScreenRect
{
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool ok = false;
};

// Maps a raw box x0,y0,x1,y1 to display pixels. `refW` x `refH` is the viewport of the draw (its render target when the viewport is not known);
// it is only used by Pixels, where it is what the raw units are measured in. A box with a number that is not finite, or a reference of no
// size in Pixels, gives no rectangle. The viewport's origin is not added.
inline ScreenRect MapBox(OverlayUnits units, const float box[4], float displayW, float displayH, float refW, float refH)
{
    ScreenRect r;
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(box[i]))
            return r;
    if (!(displayW > 0) || !(displayH > 0))
        return r;
    float x0 = box[0], y0 = box[1], x1 = box[2], y1 = box[3];
    switch (units)
    {
    case OverlayUnits::Pixels:
    {
        if (!(refW > 0) || !(refH > 0))
            return r;
        const float sx = displayW / refW, sy = displayH / refH;
        x0 *= sx;
        x1 *= sx;
        y0 *= sy;
        y1 *= sy;
        break;
    }
    case OverlayUnits::Unit:
        x0 *= displayW;
        x1 *= displayW;
        y0 *= displayH;
        y1 *= displayH;
        break;
    case OverlayUnits::Ndc:
    {
        const float ax = (x0 + 1.0f) * 0.5f * displayW, bx = (x1 + 1.0f) * 0.5f * displayW;
        const float ay = (1.0f - y0) * 0.5f * displayH, by = (1.0f - y1) * 0.5f * displayH;
        x0 = ax;
        x1 = bx;
        y0 = ay;
        y1 = by;
        break;
    }
    default:
        return r;
    }
    r.x0 = (std::min)(x0, x1);
    r.x1 = (std::max)(x0, x1);
    r.y0 = (std::min)(y0, y1);
    r.y1 = (std::max)(y0, y1);
    r.ok = true;
    return r;
}

// The assumption in words, with the factor, for the overlay line: "raw box as pixels of the viewport, x1.00" and so on.
inline std::string OverlayAssumption(OverlayUnits units, float displayW, float displayH, float refW, float refH)
{
    char b[160] = {};
    switch (units)
    {
    case OverlayUnits::Pixels:
        if (refW > 0 && refH > 0)
        {
            const float sx = displayW / refW, sy = displayH / refH;
            if (std::fabs(sx - sy) < 0.005f)
                snprintf(b, sizeof b, "raw box as pixels of the viewport, x%.2f", sx);
            else
                snprintf(b, sizeof b, "raw box as pixels of the viewport, x%.2f by x%.2f", sx, sy);
        }
        else
            snprintf(b, sizeof b, "raw box as pixels of the viewport, no viewport size");
        break;
    case OverlayUnits::Unit: snprintf(b, sizeof b, "raw box as 0..1 of the display, x%.0f by x%.0f", displayW, displayH); break;
    case OverlayUnits::Ndc: snprintf(b, sizeof b, "raw box as -1..1 of the display (y up), x%.0f by x%.0f", displayW * 0.5f, displayH * 0.5f); break;
    }
    return b;
}

// The overlay line: "plate probe: <assumption>, frame F, N boxes".
inline std::string OverlayLine(OverlayUnits units, float displayW, float displayH, float refW, float refH, uint64_t frame, size_t boxes)
{
    return "plate probe: " + OverlayAssumption(units, displayW, displayH, refW, refH) + ", frame " + std::to_string(frame) + ", " +
           std::to_string(boxes) + (boxes == 1 ? " box" : " boxes");
}

// There is room for one more overlay in the menu's fixed table.
inline bool OverlaySlotFree(size_t registered, size_t capacity) { return registered < capacity; }

// The overlay draws only for a probe that has a logged frame to show and is switched on.
inline bool OverlayShouldDraw(bool wanted, uint64_t lastLoggedFrame) { return wanted && lastLoggedFrame != 0; }
} // namespace ext::hudless::plate
