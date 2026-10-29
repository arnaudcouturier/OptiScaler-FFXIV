#pragma once
// Bounded multi-producer, single-consumer ring for the developer probe's raw events (the game thread and the pacer threads push, the
// flush thread drains). Lock free: a producer claims a slot with one compare-exchange and publishes it with a release store. A full ring
// drops the new event and counts it; it never blocks and never waits (docs/HUDLESS.md, "Developer probes").

#include "ext/hudless/PlateProbeAccounting.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace ext::hudless::plate
{
template <size_t N> class EventRing
{
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");

  public:
    EventRing()
    {
        _cells = std::make_unique<Cell[]>(N);
        for (size_t i = 0; i < N; ++i)
            _cells[i].sequence.store(i, std::memory_order_relaxed);
    }

    // Any thread. False (and one more drop) when the ring is full.
    bool Push(const Event& e)
    {
        uint64_t pos = _head.load(std::memory_order_relaxed);
        for (;;)
        {
            Cell& cell = _cells[pos & (N - 1)];
            const uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
            const int64_t diff = static_cast<int64_t>(sequence) - static_cast<int64_t>(pos);
            if (diff == 0)
            {
                if (_head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    cell.event = e;
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0)
            {
                _drops.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            else
                pos = _head.load(std::memory_order_relaxed);
        }
    }

    // The consumer thread only. Copies up to `max` published events into `out`, oldest first.
    size_t Drain(Event* out, size_t max)
    {
        size_t n = 0;
        while (n < max)
        {
            Cell& cell = _cells[_tail & (N - 1)];
            if (cell.sequence.load(std::memory_order_acquire) != _tail + 1)
                break;
            out[n++] = cell.event;
            cell.sequence.store(_tail + N, std::memory_order_release);
            ++_tail;
        }
        return n;
    }

    uint64_t Drops() const { return _drops.load(std::memory_order_relaxed); }
    static constexpr size_t Capacity() { return N; }

  private:
    struct Cell
    {
        std::atomic<uint64_t> sequence { 0 };
        Event event;
    };

    std::unique_ptr<Cell[]> _cells;
    alignas(64) std::atomic<uint64_t> _head { 0 };
    alignas(64) uint64_t _tail = 0;
    std::atomic<uint64_t> _drops { 0 };
};
} // namespace ext::hudless::plate
