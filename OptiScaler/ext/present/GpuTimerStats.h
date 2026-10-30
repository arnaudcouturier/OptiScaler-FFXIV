#pragma once
// Pure rules of the frame-pacing GPU stage timers ([FramePacing] GpuTimers, docs/FEATURES.md "Frame pacing GPU").
// The timed stages are the present copy-in (D3D11), the ReShade effects (D3D11) and the D3D12 interop copy; the
// "Bridge excl. NR" row is derived (the upstream "Interop" entry minus NR's own time, latest values read at the
// same present, clamped at 0). No D3D type, no config, no clock and no logger of its own (times, frames and fence
// values are passed in), so every rule is unit-testable (tests/ext/present/gpu_timer_stats.cpp). Clocks, queries
// and logging live in ext/present/GpuBridgeTimers.cpp.
//
// Sampling follows the NR pre-SR pattern (4 of 16 frames pay for timestamps). A timer never waits: D3D11 queries
// are read only at Present with DONOTFLUSH, D3D12 slots report only once the copy-queue fence has passed the fence
// value that covers their resolve (never SetEventOnCompletion, never WaitForSingleObject), and durations are always
// taken within one API (D3D11 timestamps are never subtracted from D3D12 ones).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace ext::present::gputimers
{
// What is timed. The order is the order of kGpuStageNames.
enum class GpuStage : unsigned
{
    PresentCopyIn, // the backbuffer copy into the shared texture (BeforeSharedCopy .. AfterSharedCopy)
    ReshadeEffects, // ReShade's own end-of-frame pass (begin_effects .. finish_effects, the fork's own calls left out)
    InteropCopy, // the shared texture into the FG backbuffer (ConvertCopySource .. PrepareScreenshot)
    Count
};

inline constexpr const char* kGpuStageNames[] = { "present copy-in", "ReShade effects", "interop copy" };

inline const char* GpuStageName(GpuStage s)
{
    const auto i = static_cast<unsigned>(s);
    return i < static_cast<unsigned>(GpuStage::Count) ? kGpuStageNames[i] : "?";
}

// Four of every 16 frames carry timestamps, so about one frame in four pays a few microseconds.
inline constexpr unsigned kTimerSlots = 16;
inline constexpr unsigned kSampledSlots = 4;

inline constexpr bool ShouldSample(uint64_t frame) { return (frame % kTimerSlots) < kSampledSlots; }

// The reporting window, the same 10 s as the CPU trace.
inline constexpr int64_t kGpuWindowUs = 10'000'000;

// p95 position (0-based) in an ascending sort of n samples: the first slot holding at least 95% of them.
// Always inside [0, n-1] for n >= 1 (the division rounds up, the -1 stays above -1 because the quotient is >= 1).
inline constexpr size_t P95Index(size_t n) { return n == 0 ? 0 : (n * 95 + 99) / 100 - 1; }

struct GpuSummary
{
    uint64_t samples = 0;
    double meanMs = 0.0;
    double p95Ms = 0.0;
    double maxMs = 0.0;
};

// One stage on the present thread. The first sample opens the window; CloseIfDue reports (at most) once per 10 s.
// A clock that went backwards restarts the window at that sample. An empty window reports zeros (no division by zero).
// Reset forgets the window, so samples of a previous on-period never report after the timers come back on.
class GpuWindow
{
  public:
    void Add(double ms, int64_t nowUs)
    {
        if (open_ && nowUs < startUs_)
        {
            samples_.clear();
            open_ = false;
        }
        if (!open_)
        {
            open_ = true;
            startUs_ = nowUs;
        }
        samples_.push_back(ms);
    }

    // True and the summary when 10 s or more passed since the window opened (the samples added so far belong to
    // it). The window is empty afterwards; a window with no samples still reports once, with zeros.
    bool CloseIfDue(int64_t nowUs, GpuSummary& out)
    {
        if (!open_ || nowUs - startUs_ < kGpuWindowUs)
            return false;
        out = Summarize(samples_);
        samples_.clear();
        open_ = false;
        return true;
    }

    void Reset()
    {
        samples_.clear();
        open_ = false;
    }

    static GpuSummary Summarize(const std::vector<double>& v)
    {
        GpuSummary s;
        s.samples = v.size();
        if (v.empty())
            return s;
        double sum = 0.0, mx = v[0];
        for (double x : v)
        {
            sum += x;
            if (x > mx)
                mx = x;
        }
        s.meanMs = sum / static_cast<double>(v.size());
        s.maxMs = mx;
        std::vector<double> sorted = v;
        std::sort(sorted.begin(), sorted.end());
        s.p95Ms = sorted[P95Index(sorted.size())];
        return s;
    }

  private:
    std::vector<double> samples_;
    int64_t startUs_ = 0;
    bool open_ = false;
};

// "Frame pacing GPU [<stage>]: samples N, mean x.xxx ms, p95 y.xxx ms, max z.zzz ms".
inline std::string FormatGpuLine(const char* stage, const GpuSummary& s)
{
    char buf[256];
    const int n = std::snprintf(buf, sizeof(buf), "Frame pacing GPU [%s]: samples %llu, mean %.3f ms, p95 %.3f ms, max %.3f ms",
                                stage, static_cast<unsigned long long>(s.samples), s.meanMs, s.p95Ms, s.maxMs);
    return std::string(buf, n > 0 ? static_cast<size_t>(n < static_cast<int>(sizeof(buf)) ? n : static_cast<int>(sizeof(buf)) - 1) : 0);
}

// The corrected row: upstream's "Interop" entry minus NR's own time, both read at the same present. Either side
// missing is not a measurement (not zero: zero would claim the bridge cost nothing). A negative difference is
// timing noise and reads 0, as does a NaN input.
inline std::optional<double> BridgeExclNr(std::optional<double> interopMs, std::optional<double> nrMs)
{
    if (!interopMs.has_value() || !nrMs.has_value())
        return std::nullopt;
    const double d = *interopMs - *nrMs;
    if (!(d > 0.0))
        return 0.0;
    return d;
}

// Outstanding timestamp pairs, one per timer slot. The capture code records a pair for a slot and asks here whether
// the slot may be reused and whether a pending slot has finished on the GPU. Fence values are plain integers and
// frames are plain counters, so the whole discipline is unit-testable:
//
// - a slot reports at most once per recording (Consume clears it);
// - a slot whose GPU work never finishes is never reported;
// - reusing a slot whose old sample is still unfinished drops the old sample on a fenced pair (its resolve never
//   retired) and reports it first when it did (the recycle rule of the NR pre-SR timing);
// - on an unfenced pair a second writer in the same frame keeps out, so the first runtime of the frame wins;
// - switching the timers off forgets every slot (Clear).
class SlotBook
{
  public:
    static constexpr unsigned kSlots = 16;

    // The answer of TryBegin, the call that opens a new recording on a slot:
    // Open = free, record; KeepOut = a sample of this frame already waits, drop this one; Recycle = the old
    // sample finished, report it, then reuse the slot; Drop = the old sample never finished, forget it, then reuse.
    enum class Verdict
    {
        Open,
        KeepOut,
        Recycle,
        Drop
    };

    struct Slot
    {
        bool pending = false; // a pair was recorded and not yet reported
        uint64_t frame = 0;   // the frame counter of the recording
        uint64_t fence = 0;   // fenced pairs: the fence value already covering the list at the recording
        uint32_t tag = 0;     // fenced pairs: which copy list the pair was recorded into
    };

    // May a new pair be recorded on `slot` in `frame`? Fenced pairs (D3D12) answer from the fence: `completed` is
    // the fence's completed value, `current` the value now covering the recorded list. Unfenced pairs (D3D11)
    // answer from the frame: a second writer in the same frame keeps out, a later frame reuses the slot.
    Verdict TryBegin(unsigned slot, uint64_t frame, uint64_t completed, uint64_t current, bool fenced)
    {
        if (slot >= kSlots)
            return Verdict::KeepOut;
        const Slot& s = slots_[slot];
        if (!s.pending)
            return Verdict::Open;
        if (!fenced)
            return s.frame == frame ? Verdict::KeepOut : Verdict::Drop;
        return Ready(slot, completed, current) ? Verdict::Recycle : Verdict::Drop;
    }

    // Record a pair on `slot`: it becomes pending (a still-pending pair is superseded; callers ask TryBegin
    // first when the old one must be reported rather than forgotten).
    void Arm(unsigned slot, uint64_t frame, uint64_t fence, uint32_t tag)
    {
        if (slot >= kSlots)
            return;
        slots_[slot] = Slot { true, frame, fence, tag };
    }

    // Forget the pending pair of `slot` (the failed-conversion path: its list is closed and never executed, so
    // its resolve never runs and must never be read).
    void Cancel(unsigned slot)
    {
        if (slot < kSlots)
            slots_[slot].pending = false;
    }

    // True when the pending pair of `slot` has finished on the GPU: a fence value recorded after the pair
    // (`current`, read from the recorded list) covers it, and the fence has passed that value. Both comparisons
    // are strict: equality with the recorded value means nothing new was submitted, equality with the covering
    // value means its execution has not retired yet.
    bool Ready(unsigned slot, uint64_t completed, uint64_t current) const
    {
        if (slot >= kSlots)
            return false;
        const Slot& s = slots_[slot];
        return s.pending && current > s.fence && completed >= current;
    }

    // Take the pending pair of `slot` for reporting. True once per recording: a second take without a new Arm
    // reads false, so every value is reported exactly once.
    bool Consume(unsigned slot)
    {
        if (slot >= kSlots || !slots_[slot].pending)
            return false;
        slots_[slot].pending = false;
        return true;
    }

    bool Pending(unsigned slot) const { return slot < kSlots && slots_[slot].pending; }
    uint64_t Frame(unsigned slot) const { return slot < kSlots ? slots_[slot].frame : 0; }
    uint64_t Fence(unsigned slot) const { return slot < kSlots ? slots_[slot].fence : 0; }
    uint32_t Tag(unsigned slot) const { return slot < kSlots ? slots_[slot].tag : 0; }

    void Clear() { slots_ = {}; }

  private:
    std::array<Slot, kSlots> slots_ {};
};

static_assert(SlotBook::kSlots == kTimerSlots, "one book slot per timer slot of the 16-frame cycle");

// Which begin wins the effects span. The span belongs to one frame: a begin while a span of the same frame is open
// keeps out (the first runtime of the frame wins and its pair stays intact); a begin in a later frame reaps the
// stale span first (its queries end where they opened and its slot is disarmed) and then proceeds, so one missed
// finish never blocks later frames. Mirrors the unfenced SlotBook verdicts (same frame keeps out, later frame drops).
enum class EffectsBegin
{
    Proceed,        // no span open: record
    KeepOut,        // a span of this frame is open: leave it intact
    ReapThenProceed // a span of an earlier frame is open: end it, disarm its slot, then record
};

inline EffectsBegin DecideEffectsBegin(bool open, uint64_t openFrame, uint64_t frame)
{
    if (!open)
        return EffectsBegin::Proceed;
    return openFrame == frame ? EffectsBegin::KeepOut : EffectsBegin::ReapThenProceed;
}

// Whether the timers run. The rule of the key [FramePacing] GpuTimers:
//   true   -> on (like Trace=true: at any log level)
//   false  -> off
//   unset (auto) -> on only together with the frame-pacing trace (which itself is FFXIV-only)
inline bool ResolveGpuTimers(std::optional<bool> key, bool traceOn)
{
    if (key.has_value())
        return *key;
    return traceOn;
}

// A creation failure turns its timer off with ONE warning: the first Claim hands out the log line, later ones do not.
// A new device restarts the timer, so Reset hands it its own warning again.
class WarnLatch
{
  public:
    bool Claim()
    {
        if (claimed_)
            return false;
        claimed_ = true;
        return true;
    }

    void Reset() { claimed_ = false; }

  private:
    bool claimed_ = false;
};
} // namespace ext::present::gputimers
