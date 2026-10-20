#pragma once
// Thread-id lines at the DX11 upscaler's create, evaluate and release (see NgxThreadTrace.h for the calls). Pure, std only:
// tests/ext/upscaler/ngx_thread_trace_test.cpp pins the formats the in-game check greps for.
//
// Release and create are rare (once per quality change) and always logged. Evaluate runs every frame, so it logs only the first
// call of a key, a change of thread, and then one summary line per interval with the call count.

#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace ext::upscaler::threadtrace
{
enum class Reason
{
    First,
    Moved,
    Periodic,
};

struct Decision
{
    bool log = false;
    Reason reason = Reason::First;
    uint32_t previousTid = 0; // Moved
    uint64_t count = 0;       // Periodic: calls since the previous line, this one included
    uint64_t intervalMs = 0;  // Periodic
    uint64_t suppressed = 0;  // first/moved lines held back by the budget since the last line that was written
};

inline std::string FormatCreate(uint32_t tid, unsigned renderW, unsigned renderH, unsigned displayW, unsigned displayH)
{
    return std::format("FFXIV NGX thread: create tid={} render={}x{} display={}x{}", tid, renderW, renderH, displayW, displayH);
}

inline std::string FormatRelease(unsigned handleId, uint32_t tid)
{
    return std::format("FFXIV NGX thread: release handle={} tid={}", handleId, tid);
}

inline std::string FormatEvaluateBase(uintptr_t key, uint32_t tid, const Decision& d);

inline std::string FormatEvaluate(uintptr_t key, uint32_t tid, const Decision& d)
{
    std::string line = FormatEvaluateBase(key, tid, d);
    if (d.suppressed > 0)
        line += std::format(" suppressed={}", d.suppressed);
    return line;
}

inline std::string FormatEvaluateBase(uintptr_t key, uint32_t tid, const Decision& d)
{
    switch (d.reason)
    {
    case Reason::First:
        return std::format("FFXIV NGX thread: evaluate key={:x} tid={} first", key, tid);
    case Reason::Moved:
        return std::format("FFXIV NGX thread: evaluate key={:x} tid={} moved from={}", key, tid, d.previousTid);
    case Reason::Periodic:
        return std::format("FFXIV NGX thread: evaluate key={:x} tid={} evaluations={} in the last {} ms", key, tid, d.count,
                           d.intervalMs);
    }
    return {};
}

// Not thread safe on its own: the caller serialises (one lock per frame is far below the cost of the evaluate it sits in).
class EvaluateTracker
{
  public:
    static constexpr size_t kMaxKeys = 8;
    // First and moved lines are the interesting ones but also the unbounded ones (two threads alternating on one key, or more
    // keys than slots cycling): at most this many per interval, the rest counted in the next line that is written.
    static constexpr uint64_t kLinesPerInterval = 4;

    explicit EvaluateTracker(uint64_t intervalMs, uint64_t linesPerInterval = kLinesPerInterval)
        : _intervalMs(intervalMs), _linesPerInterval(linesPerInterval)
    {
    }

    Decision OnEvaluate(uintptr_t key, uint32_t tid, uint64_t nowMs)
    {
        Decision d = Classify(key, tid, nowMs);
        if (!d.log)
            return d;
        if (d.reason != Reason::Periodic)
        {
            if (nowMs < _windowStartMs || nowMs - _windowStartMs >= _intervalMs)
            {
                _windowStartMs = nowMs;
                _linesInWindow = 0;
            }
            if (_linesInWindow >= _linesPerInterval)
            {
                ++_suppressed;
                d.log = false;
                return d;
            }
            ++_linesInWindow;
        }
        d.suppressed = _suppressed;
        _suppressed = 0;
        return d;
    }

    size_t TrackedKeys() const
    {
        size_t n = 0;
        for (const auto& s : _slots)
            n += s.used ? 1 : 0;
        return n;
    }

  private:
    Decision Classify(uintptr_t key, uint32_t tid, uint64_t nowMs)
    {
        Slot* slot = nullptr;
        for (auto& s : _slots)
            if (s.used && s.key == key)
                slot = &s;
        Decision d;
        if (slot == nullptr)
        {
            slot = &_slots[_next];
            _next = (_next + 1) % kMaxKeys; // the oldest slot goes first
            *slot = Slot { true, key, tid, nowMs, 0 };
            d.log = true;
            d.reason = Reason::First;
            return d;
        }
        ++slot->calls;
        if (slot->tid != tid)
        {
            d.log = true;
            d.reason = Reason::Moved;
            d.previousTid = slot->tid;
            slot->tid = tid;
            slot->lastLogMs = nowMs;
            slot->calls = 0;
            return d;
        }
        // a clock that went backwards never logs
        if (nowMs >= slot->lastLogMs && nowMs - slot->lastLogMs >= _intervalMs)
        {
            d.log = true;
            d.reason = Reason::Periodic;
            d.count = slot->calls;
            d.intervalMs = _intervalMs;
            slot->lastLogMs = nowMs;
            slot->calls = 0;
        }
        return d;
    }

    struct Slot

    {
        bool used = false;
        uintptr_t key = 0;
        uint32_t tid = 0;
        uint64_t lastLogMs = 0;
        uint64_t calls = 0; // since the last line
    };
    std::array<Slot, kMaxKeys> _slots {};
    size_t _next = 0;
    uint64_t _intervalMs;
    uint64_t _linesPerInterval;
    uint64_t _windowStartMs = 0;
    uint64_t _linesInWindow = 0;
    uint64_t _suppressed = 0;
};
} // namespace ext::upscaler::threadtrace
