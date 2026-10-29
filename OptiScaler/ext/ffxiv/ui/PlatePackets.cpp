#include "pch.h"

#include "ext/ffxiv/ui/PlatePackets.h"

#include "ext/core/ProcessMemory.h"
#include "ext/ffxiv/ActiveBuild.h"
#include "ext/ffxiv/BuildBounds.h"
#include "ext/hudless/PlateAssocCounters.h"
#include "ext/hudless/PlateProbePackets.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

// The game builds its UI as a list of packets {queue key, header*} per batch call (batch16 / batch32); each header names a run of vertices in
// the renderer's CPU vertex allocation. Every draw command of the batch passes through the game's enqueue function, and right before the draw
// the game binds the command's state. This file follows those three moments, on the frames the probe logs, with state of its own:
//   batch call   the packets of the batch become vertex ranges (this thread only). The headers are read as blocks, and the batch call has a time
//                budget for all the probe's reads (headers and the draws it enqueues): a batch that runs out of time before it has every header,
//                or lists more than the hard limit of packets, is not used at all, so a draw never gets the key of a later packet than its own
//   enqueue      the draw's index list is read from the renderer's CPU index buffer, matched with the ranges, the vertices it reaches are
//                copied, and a record is put in a map under the command's address
//   bind         the record leaves the map, if the command bytes are still what they were, and waits for the next indexed draw of the thread
//   draw         NoteDraw (PlateDraws.h) calls AssignPacket: the record is checked against the draw's own arguments and kept for its row
// Everything is a read of the game's memory through ReadProcessMemory (a failed read is a value) and of copies made here; no pointer of the
// game is kept, nothing is mapped or waited for, and the shared map is only ever try-locked.
namespace ext::ffxiv::ui::plate
{
namespace
{
namespace pf = ext::hudless::plate;
using Entry = pf::QueueEntry;

uint64_t NowMs() { return GetTickCount64(); }

struct Slot
{
    bool known = false, ok = false;
    uint32_t stride = 0, offset = 0;
};

// The packets of the batch call in progress on this thread.
struct BatchState
{
    bool active = false;
    uintptr_t base = 0, renderer = 0, allocation = 0;
    uint32_t indexBytes = 0;
    pf::RangeSet ranges;
    std::array<Slot, 16> slots {};
    pf::ReadBudget budget; // the probe's own time in this batch call: its headers and the draws it enqueues
    bool cutCounted = false;
};

thread_local BatchState t_batch;
thread_local pf::Ordinals t_ordinals;
thread_local uint32_t t_batchOrdinal = 0;
thread_local std::vector<Entry> t_entries;
thread_local std::vector<uint32_t> t_indices;

// The record bound to this thread (or the reason it was refused), and the one of the draw in progress.
thread_local std::unique_ptr<pf::DrawRecord> t_bound;
thread_local pf::RowOutcome t_boundFailure = pf::RowOutcome::NoPacket; // NoPacket = no failure
thread_local std::unique_ptr<pf::DrawRecord> t_current;
thread_local pf::RowOutcome t_currentOutcome = pf::RowOutcome::NoPacket;

std::atomic<uint64_t> g_frameHint { 0 };
std::atomic<uint64_t> g_overlayFrame { 0 };
std::atomic<size_t> g_mapSize { 0 };

struct Shared
{
    std::mutex mutex;
    pf::AssocMap map;
    std::mutex overlayMutex;
    OverlayFrame overlay;
};

Shared& S()
{
    static Shared* shared = new Shared; // never destroyed: no destructor may run under the loader lock
    return *shared;
}

pf::HeaderOffsets HeaderOffsetsOf(const ext::ffxiv::CompanionBuild& b)
{
    return { b.batchKindOffset, b.batchCountOffset, b.batchVerticesOffset, b.batchFormatOffset };
}

// Byte offset and stride of the vertex buffer slot a layout draws from, as the game's batch functions read them (renderer + 0x500 / 0x504
// per slot). Remembered for the batch call.
bool SlotOf(BatchState& b, uint32_t layout, uint32_t& stride, uint32_t& offset)
{
    if (layout >= b.slots.size())
        return false;
    Slot& s = b.slots[layout];
    if (!s.known)
    {
        s.known = true;
        const auto& build = ext::ffxiv::ActiveBuild().companion;
        int32_t slot = -1;
        uint32_t st = 0, off = 0;
        if (ext::core::ReadProcessValue(b.base + build.layoutSlotTableRva + uintptr_t(layout) * build.layoutSlotEntrySize, slot) && slot >= 0 &&
            slot <= 2 &&
            ext::core::ReadProcessValue(b.renderer + build.rendererVertexStrideOffset + uintptr_t(slot) * build.rendererVertexSlotStride, st) &&
            ext::core::ReadProcessValue(b.renderer + build.rendererVertexOffsetOffset + uintptr_t(slot) * build.rendererVertexSlotStride, off))
        {
            s.ok = true;
            s.stride = st;
            s.offset = off;
        }
    }
    stride = s.stride;
    offset = s.offset;
    return s.ok;
}

// The renderer's CPU copy of the index buffer, where the game wrote this batch's indices (the pointer that holds them depends on the buffer
// flags, as in the game's own readers).
bool ReadIndices(const BatchState& b, uint32_t start, uint32_t count, std::vector<uint32_t>& out)
{
    const auto& build = ext::ffxiv::ActiveBuild().companion;
    uintptr_t ib = 0, cpu = 0;
    uint32_t flags = 0, capacity = 0;
    if (!ext::core::ReadProcessValue(b.renderer + build.rendererIndexBufferOffset, ib) || !ib ||
        !ext::core::ReadProcessValue(ib + build.indexBufferFlagsOffset, flags) || !ext::core::ReadProcessValue(ib + build.indexBufferCapacityOffset, capacity) ||
        !ext::core::ReadProcessValue(ib + ((!(flags & 0x11) || (flags & 0x40)) ? build.indexBufferCpuStartOffset : build.indexBufferCpuEndOffset), cpu) ||
        !cpu || (uint64_t(start) + count) * b.indexBytes > capacity)
        return false;
    out.resize(count);
    if (b.indexBytes == 4)
        return ext::core::ReadProcessBytes(cpu + uint64_t(start) * 4, out.data(), size_t(count) * 4);
    if (b.indexBytes != 2)
        return false;
    std::vector<uint16_t> narrow(count);
    if (!ext::core::ReadProcessBytes(cpu + uint64_t(start) * 2, narrow.data(), size_t(count) * 2))
        return false;
    std::copy(narrow.begin(), narrow.end(), out.begin());
    return true;
}

void Count(std::atomic<uint64_t>& counter) { pf::AssocCounters::Bump(counter); }

uint64_t Ticks()
{
    LARGE_INTEGER v {};
    QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}

uint64_t BatchBudgetTicks()
{
    static const uint64_t ticks = []
    {
        LARGE_INTEGER f {};
        QueryPerformanceFrequency(&f);
        return pf::BudgetTicks(static_cast<uint64_t>(f.QuadPart), pf::kBatchBudgetMs);
    }();
    return ticks;
}

// The batch call ran out of probe time: counted once per batch call.
void NoteCut(BatchState& b)
{
    if (b.cutCounted)
        return;
    b.cutCounted = true;
    Count(pf::Counters().batchCut);
}

// Charges the time between its construction and its end to the batch call's budget.
struct Spend
{
    explicit Spend(BatchState& batch) : b(batch), from(Ticks()) {}
    ~Spend() { b.budget.Add(from, Ticks()); }
    Spend(const Spend&) = delete;
    Spend& operator=(const Spend&) = delete;
    BatchState& b;
    uint64_t from;
};
} // namespace

void SetFrameHint(uint64_t frame) { g_frameHint.store(frame, std::memory_order_relaxed); }

namespace detail
{
bool BeginBatch(uintptr_t base, uintptr_t renderer, uint32_t indexBytes)
{
    try
    {
        BatchState& b = t_batch;
        if (b.active)
            return false; // a batch call inside a batch call: the outer one keeps the state
        const uint64_t hint = g_frameHint.load(std::memory_order_relaxed);
        t_batchOrdinal = t_ordinals.NextBatch(hint);
        if (!pf::ReadOnFrame(hint))
            return false;

        const auto& build = ext::ffxiv::ActiveBuild().companion;
        if (!ext::ffxiv::CompanionOffsetsInBounds(build))
            return false;
        const uint64_t started = Ticks();
        uintptr_t list = 0, allocation = 0;
        uint32_t count = 0;
        if (!ext::core::ReadProcessValue(renderer + build.rendererBatchListOffset, list) ||
            !ext::core::ReadProcessValue(renderer + build.rendererBatchCountOffset, count) ||
            !ext::core::ReadProcessValue(renderer + build.rendererVertexAllocationOffset, allocation) || !list || !allocation)
            return false;
        const pf::ListCheck check = pf::CheckPacketList(count);
        if (check == pf::ListCheck::Empty)
            return false;
        Count(pf::Counters().batchReads);
        if (check == pf::ListCheck::TooMany)
        {
            Count(pf::Counters().headerCap); // not read at all: a part of the packets would give a draw the key of a later packet
            return false;
        }
        t_entries.resize(count);
        if (!ext::core::ReadProcessBytes(list, t_entries.data(), size_t(count) * sizeof(Entry)))
            return false;

        b = BatchState {};
        b.base = base;
        b.renderer = renderer;
        b.allocation = allocation;
        b.indexBytes = indexBytes;
        b.budget.Begin(BatchBudgetTicks());
        b.budget.Add(started, Ticks());
        // the headers are read as blocks (the whole list is usually a few calls), in list order, and a batch that runs out of time before it has
        // all of them is not used at all: a draw must not get the key of a later packet than the one that holds its first vertex
        const pf::CollectResult result = pf::CollectRanges(
            t_entries, allocation, HeaderOffsetsOf(build), static_cast<uint32_t>(ext::ffxiv::kNativeBatchHeaderBytes), b.budget,
            [](uint64_t address, void* out, size_t size) { return ext::core::ReadProcessBytes(static_cast<uintptr_t>(address), out, size); },
            [] { return Ticks(); }, [&b](uint32_t layout, uint32_t& stride, uint32_t& offset) { return SlotOf(b, layout, stride, offset); }, b.ranges);
        if (result == pf::CollectResult::Cut)
        {
            NoteCut(b);
            b.ranges.Clear();
            return false;
        }
        b.active = true;
        return true;
    }
    catch (...)
    {
        t_batch.active = false;
        return false;
    }
}

void EndBatch()
{
    t_batch.active = false;
    t_batch.ranges.Clear();
}

void Enqueue(uintptr_t command)
{
    try
    {
        Shared& shared = S();
        // The command pool reuses addresses: whatever an earlier submission left at this one is stale, whether or not this one is a draw.
        if (g_mapSize.load(std::memory_order_relaxed) != 0)
        {
            std::unique_lock lock(shared.mutex, std::try_to_lock);
            if (lock)
            {
                shared.map.Erase(command);
                g_mapSize.store(shared.map.Size(), std::memory_order_relaxed);
            }
        }
        BatchState& b = t_batch;
        if (!b.active)
            return;
        // the batch call's time for reading game memory is used up: this and the draws after it get no record
        if (b.budget.Exhausted())
        {
            NoteCut(b);
            return;
        }
        Spend spend(b);

        const auto& build = ext::ffxiv::ActiveBuild().companion;
        std::array<unsigned char, ext::ffxiv::kDrawCommandBytes> bytes {};
        if (!ext::core::ReadProcessBytes(command, bytes.data(), bytes.size()))
        {
            Count(pf::Counters().refusedRead);
            return;
        }
        const pf::CommandFields fields =
            pf::ParseCommand(bytes, { build.drawCommandTypeOffset, build.baseVertexOffset, build.startIndexOffset, build.indexCountOffset });
        if (!fields.ok || fields.type != build.drawCommandType)
            return; // not a draw command
        const uint32_t drawOrdinal = t_ordinals.NextDraw();
        if (!pf::UsableIndexCount(fields.indexCount))
            return;
        if (!ReadIndices(b, fields.startIndex, fields.indexCount, t_indices))
        {
            Count(pf::Counters().refusedRead);
            return;
        }

        auto record = std::make_unique<pf::DrawRecord>();
        record->command = command;
        record->commandBytes.assign(bytes.begin(), bytes.end());
        record->batch = t_batchOrdinal;
        record->batchDraw = drawOrdinal;
        record->indexCount = fields.indexCount;
        record->startIndex = fields.startIndex;
        record->baseVertex = fields.baseVertex;
        record->timeMs = NowMs();

        const pf::DrawUse use = pf::Analyze(t_indices, fields.baseVertex, b.ranges);
        if (use.mapped == 0)
            Count(pf::Counters().unmapped);
        record->hasKey = use.hasKey;
        record->qkey = use.qkey;
        record->entries = use.entries;
        record->layout = use.layout;
        uint32_t stride = 0, slotOffset = 0;
        if (use.hasKey && SlotOf(b, use.layout, stride, slotOffset))
        {
            const pf::RunPlan plan = pf::PlanRun(use, stride, pf::kMaxRunBytes);
            if (plan.ok)
            {
                record->run.resize(size_t(plan.count) * stride);
                if (ext::core::ReadProcessBytes(b.allocation + slotOffset + uintptr_t(plan.first) * stride, record->run.data(), record->run.size()))
                {
                    record->stride = stride;
                    record->used = pf::MarkUsed(t_indices, fields.baseVertex, plan);
                }
                else
                {
                    record->run.clear();
                    Count(pf::Counters().refusedRead);
                }
            }
        }

        std::unique_lock lock(shared.mutex, std::try_to_lock);
        if (!lock)
        {
            Count(pf::Counters().refusedBusy);
            return;
        }
        if (shared.map.Store(std::move(record), NowMs()) == pf::AssocMap::Put::Full)
            Count(pf::Counters().refusedCapacity);
        else
            Count(pf::Counters().made);
        g_mapSize.store(shared.map.Size(), std::memory_order_relaxed);
    }
    catch (...)
    {
    }
}

void Bind(uintptr_t /*nativeContext*/, uintptr_t state)
{
    try
    {
        // a record that no draw took is dropped: this bind is the one the next draw belongs to
        t_bound.reset();
        t_boundFailure = pf::RowOutcome::NoPacket;
        g_packetPending = false;
        if (g_mapSize.load(std::memory_order_relaxed) == 0)
            return;
        const auto& build = ext::ffxiv::ActiveBuild().companion;
        if (state < build.drawStateCommandBackPointerOffset)
            return;
        const uintptr_t command = state - build.drawStateCommandBackPointerOffset;

        Shared& shared = S();
        {
            std::unique_lock lock(shared.mutex, std::try_to_lock);
            if (!lock)
            {
                Count(pf::Counters().refusedBusy);
                return;
            }
            if (!shared.map.Has(command))
                return;
        }
        std::array<unsigned char, ext::ffxiv::kDrawCommandBytes> current {};
        const bool read = ext::core::ReadProcessBytes(command, current.data(), current.size());

        std::unique_ptr<pf::DrawRecord> record;
        pf::AssocMap::Bind result;
        {
            std::unique_lock lock(shared.mutex, std::try_to_lock);
            if (!lock)
            {
                Count(pf::Counters().refusedBusy);
                return;
            }
            result = shared.map.Take(command, read ? std::span<const unsigned char>(current) : std::span<const unsigned char>(), NowMs(), record);
            g_mapSize.store(shared.map.Size(), std::memory_order_relaxed);
        }
        switch (result)
        {
        case pf::AssocMap::Bind::Bound:
            t_bound = std::move(record);
            g_packetPending = true;
            break;
        case pf::AssocMap::Bind::Stale:
            t_boundFailure = pf::RowOutcome::Stale;
            g_packetPending = true;
            break;
        case pf::AssocMap::Bind::Changed:
            t_boundFailure = pf::RowOutcome::Changed;
            g_packetPending = true;
            break;
        case pf::AssocMap::Bind::NotFound:
            break;
        }
    }
    catch (...)
    {
        t_bound.reset();
        g_packetPending = false;
    }
}

void AssignPacket()
{
    try
    {
        g_packetHas = false;
        t_current.reset();
        t_currentOutcome = pf::RowOutcome::NoPacket;
        if (!g_packetPending || g_args.kind != 'I')
            return; // only an indexed draw can be the one a record was bound for
        g_packetPending = false;
        g_packetHas = true;
        if (t_boundFailure != pf::RowOutcome::NoPacket)
        {
            t_currentOutcome = t_boundFailure;
            t_boundFailure = pf::RowOutcome::NoPacket;
            t_bound.reset();
            return;
        }
        std::unique_ptr<pf::DrawRecord> record = std::move(t_bound);
        if (!record)
            return;
        switch (pf::MatchDraw(*record, g_args.kind, g_args.count, g_args.start, g_args.baseVertex, NowMs()))
        {
        case pf::DrawMatch::Ok:
            t_currentOutcome = record->hasKey && record->entries > 0 ? pf::RowOutcome::Associated : pf::RowOutcome::NoPacket;
            t_current = std::move(record);
            break;
        case pf::DrawMatch::Stale: t_currentOutcome = pf::RowOutcome::Stale; break;
        case pf::DrawMatch::Mismatch: t_currentOutcome = pf::RowOutcome::Mismatch; break;
        }
    }
    catch (...)
    {
        g_packetHas = false;
        g_packetPending = false;
    }
}
} // namespace detail

PacketInfo CurrentPacket()
{
    PacketInfo info;
    if (g_packetHas)
    {
        info.outcome = t_currentOutcome;
        info.record = t_current.get();
    }
    else
        info.outcome = g_args.kind == 'I' ? pf::RowOutcome::NoPacket : pf::RowOutcome::NotIndexed;
    return info;
}

void PublishOverlay(OverlayFrame&& frame)
{
    Shared& shared = S();
    std::lock_guard lock(shared.overlayMutex);
    g_overlayFrame.store(frame.frame, std::memory_order_relaxed);
    shared.overlay = std::move(frame);
}

uint64_t LastLoggedFrame() { return g_overlayFrame.load(std::memory_order_relaxed); }

bool CopyOverlay(OverlayFrame& out)
{
    Shared& shared = S();
    std::unique_lock lock(shared.overlayMutex, std::try_to_lock);
    if (!lock || shared.overlay.frame == 0)
        return false;
    out = shared.overlay;
    return true;
}
} // namespace ext::ffxiv::ui::plate
