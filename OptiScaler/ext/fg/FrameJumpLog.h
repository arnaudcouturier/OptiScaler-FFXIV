#pragma once
// Rate limit of the frame generator's frame-count re-sync warning. Upstream's IFGFeature::StartNewFrame wrote one WARN per
// re-sync; when a frame generator keeps being started while nothing is presented (released provider, a base that stopped
// swapping) that line runs thousands of times a minute and pushes everything else out of the log. The limiter keeps the
// information and drops the repetition: the first jump logs its two counters verbatim, later jumps inside kIntervalMs only
// count, and the first jump after the interval logs how many it swallowed.
//
// Pure (the caller passes the clock) so the rule is unit-tested offline (tests/ext/fg/frame_jump_log.cpp).
// Users: ext/fg/FrameJumpLog.cpp (the seam IFGFeature::StartNewFrame calls) and its host framegen/IFGFeature.cpp.

#include <cstdint>

namespace ext::fg
{
// What the caller does with one observed jump.
struct JumpNote
{
    bool log = false;        // write a line now
    uint64_t suppressed = 0; // jumps not logged since the last line (0 = none was swallowed)
    uint64_t sinceMs = 0;    // ms since the last line; >= kIntervalMs whenever log is set by a repeat
};

class JumpLimiter
{
  public:
    static constexpr uint64_t kIntervalMs = 1000;

    void Reset()
    {
        _logged = false;
        _lastMs = 0;
        _suppressed = 0;
    }

    // One jump at `nowMs`. The first one logs; later ones inside the interval are only counted. A clock that jumped backwards
    // (a suspend, a system time change) reads as a gap of at least kIntervalMs here, so it logs instead of swallowing a line.
    JumpNote Note(uint64_t nowMs)
    {
        JumpNote note;
        if (!_logged)
        {
            _logged = true;
            _lastMs = nowMs;
            _suppressed = 0;
            note.log = true;
            return note;
        }
        const uint64_t since = nowMs - _lastMs;
        if (since < kIntervalMs)
        {
            ++_suppressed;
            return note;
        }
        note.log = true;
        note.sinceMs = since;
        note.suppressed = _suppressed;
        _suppressed = 0;
        _lastMs = nowMs;
        return note;
    }

  private:
    bool _logged = false;
    uint64_t _lastMs = 0;
    uint64_t _suppressed = 0;
};
} // namespace ext::fg
