#pragma once
// Runtime counters of the draw probe (docs/HUDLESS.md, "Developer probes"): the packet association, the draws.csv row cap and the batch reads.
// The submitting threads, the drawing thread and the probe's writer thread all touch them, so they are relaxed atomics; Snapshot() copies them
// into the plain AssocStats the summary line prints. Never destroyed, like the rest of the probe's state.

#include "ext/hudless/PlateProbeFormat.h"

#include <atomic>
#include <cstdint>

namespace ext::hudless::plate
{
struct AssocCounters
{
    std::atomic<uint64_t> rows { 0 }, notIndexed { 0 }, associated { 0 }, noPacket { 0 };
    std::atomic<uint64_t> stale { 0 }, changed { 0 }, mismatch { 0 };
    std::atomic<uint64_t> boxes { 0 }, noBox { 0 };
    std::atomic<uint64_t> noBoxNoRun { 0 }, noBoxUntagged { 0 }, noBoxStrides { 0 }, noBoxNonFinite { 0 };
    std::atomic<uint64_t> made { 0 }, refusedCapacity { 0 }, refusedRead { 0 }, refusedBusy { 0 };
    std::atomic<uint64_t> unmapped { 0 };
    std::atomic<uint64_t> rowsCapped { 0 }, framesCapped { 0 }, priorityCapped { 0 };
    std::atomic<uint64_t> batchReads { 0 }, batchCut { 0 }, headerCap { 0 };

    static void Bump(std::atomic<uint64_t>& counter, uint64_t by = 1) { counter.fetch_add(by, std::memory_order_relaxed); }

    // one logged row (the same bookkeeping as AssocStats::NoteRow)
    void NoteRow(RowOutcome outcome, BoxReason reason)
    {
        Bump(rows);
        switch (outcome)
        {
        case RowOutcome::NotIndexed: Bump(notIndexed); break;
        case RowOutcome::Associated:
            Bump(associated);
            if (reason == BoxReason::Ok)
            {
                Bump(boxes);
                break;
            }
            Bump(noBox);
            switch (reason)
            {
            case BoxReason::NoRun: Bump(noBoxNoRun); break;
            case BoxReason::Untagged: Bump(noBoxUntagged); break;
            case BoxReason::Strides: Bump(noBoxStrides); break;
            default: Bump(noBoxNonFinite); break;
            }
            break;
        case RowOutcome::NoPacket: Bump(noPacket); break;
        case RowOutcome::Stale: Bump(stale); break;
        case RowOutcome::Changed: Bump(changed); break;
        case RowOutcome::Mismatch: Bump(mismatch); break;
        }
    }

    // a frame of draws.csv that refused rows (the same bookkeeping as AssocStats::NoteCappedFrame)
    void NoteCappedFrame(uint64_t dropped, uint64_t priorityDropped)
    {
        Bump(rowsCapped, dropped);
        Bump(priorityCapped, priorityDropped);
        Bump(framesCapped);
    }

    AssocStats Snapshot() const
    {
        AssocStats s;
        s.rows = rows.load(std::memory_order_relaxed);
        s.notIndexed = notIndexed.load(std::memory_order_relaxed);
        s.associated = associated.load(std::memory_order_relaxed);
        s.noPacket = noPacket.load(std::memory_order_relaxed);
        s.stale = stale.load(std::memory_order_relaxed);
        s.changed = changed.load(std::memory_order_relaxed);
        s.mismatch = mismatch.load(std::memory_order_relaxed);
        s.boxes = boxes.load(std::memory_order_relaxed);
        s.noBox = noBox.load(std::memory_order_relaxed);
        s.noBoxNoRun = noBoxNoRun.load(std::memory_order_relaxed);
        s.noBoxUntagged = noBoxUntagged.load(std::memory_order_relaxed);
        s.noBoxStrides = noBoxStrides.load(std::memory_order_relaxed);
        s.noBoxNonFinite = noBoxNonFinite.load(std::memory_order_relaxed);
        s.made = made.load(std::memory_order_relaxed);
        s.refusedCapacity = refusedCapacity.load(std::memory_order_relaxed);
        s.refusedRead = refusedRead.load(std::memory_order_relaxed);
        s.refusedBusy = refusedBusy.load(std::memory_order_relaxed);
        s.unmapped = unmapped.load(std::memory_order_relaxed);
        s.rowsCapped = rowsCapped.load(std::memory_order_relaxed);
        s.framesCapped = framesCapped.load(std::memory_order_relaxed);
        s.priorityCapped = priorityCapped.load(std::memory_order_relaxed);
        s.batchReads = batchReads.load(std::memory_order_relaxed);
        s.batchCut = batchCut.load(std::memory_order_relaxed);
        s.headerCap = headerCap.load(std::memory_order_relaxed);
        return s;
    }
};

inline AssocCounters& Counters()
{
    static AssocCounters* counters = new AssocCounters; // never destroyed
    return *counters;
}
} // namespace ext::hudless::plate
