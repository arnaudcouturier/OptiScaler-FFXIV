#pragma once
// Pure rules of the character preview Neural Rendering module (CharaNr.cpp): facts in, decisions out, so every rule is tested
// without a device. Nothing here calls Direct3D, NGX or the game.
//
// What is decided here
//   * availability       : whether preview NR may run at all, and the reason when it may not (Evaluate, ReasonText)
//   * the two latches    : a failure of the super-resolution path and a failure of NR are separate states (Latches)
//   * streams            : one NR stream per preview image, bound by the image's texture pointer (StreamTable)
//   * linger and release : a stream outlives its window by a few seconds and is released only after its GPU work completed
//   * history reset      : when the model starts from nothing (DecideReset)
//   * the fence hand-off : the order of the D3D11 -> D3D12 -> D3D11 synchronisation (RunHandoff)
//   * the late variant   : which result image a consumer draw gets (ConsumerBuffer)
//   * cost logging       : at most one cost line per interval (CostAccumulator)
//
// Times are milliseconds from any monotonic clock (GetTickCount64 in the game).

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ext::ffxiv::chara::nr
{
inline constexpr size_t kMaxStreams = 16; // one per preview slot (kMaxSlots in CharaSlots.h)
inline constexpr int kNoStream = -1;
inline constexpr uint32_t kDefaultLingerSeconds = 5;
inline constexpr uint32_t kMaxLingerSeconds = 600;
inline constexpr uint64_t kLiveMs = 250;  // a stream is "live" while its preview was drawn within this time
inline constexpr uint64_t kResetGapMs = 250; // the model history is reset after a gap longer than this between two runs

constexpr uint64_t LingerMs(uint32_t seconds)
{
    return static_cast<uint64_t>(seconds > kMaxLingerSeconds ? kMaxLingerSeconds : seconds) * 1000ull;
}

// ---- Availability -----------------------------------------------------------------------------------------------------------
enum class Probe : uint8_t
{
    Unknown, // not looked at yet: the module tries and finds out
    Present,
    Missing,
};

struct GateFacts
{
    bool configOn = false;         // [FFXIV] PreviewNR
    bool latched = false;          // an NR failure ended preview NR for this session
    bool bridgeUp = false;         // the D3D11-on-D3D12 bridge has a D3D12 device and queue
    bool bridgeSuspended = false;  // the bridge is shutting down (the main NR is suspended)
    bool releasePending = false;   // the objects of a previous bridge device are not released yet (the GPU did not finish in time)
    Probe nvidia = Probe::Unknown; // the bridge's adapter is an NVIDIA adapter
    Probe forwarder = Probe::Unknown; // nvngx.dll_dlssnr.dll was found and exports the NR calls
    Probe model = Probe::Unknown;     // nvngx_dlssnr.dll was found
};

enum class Availability : uint8_t
{
    Available,
    ConfigOff,
    Latched,
    BridgeNotUp,
    BridgeShuttingDown,
    ReleasePending,
    NotNvidia,
    ForwarderMissing,
    ModelMissing,
};

constexpr Availability Evaluate(const GateFacts& f)
{
    if (!f.configOn)
        return Availability::ConfigOff;
    if (f.latched)
        return Availability::Latched;
    if (!f.bridgeUp)
        return Availability::BridgeNotUp;
    if (f.bridgeSuspended)
        return Availability::BridgeShuttingDown;
    if (f.releasePending)
        return Availability::ReleasePending;
    if (f.nvidia == Probe::Missing)
        return Availability::NotNvidia;
    if (f.forwarder == Probe::Missing)
        return Availability::ForwarderMissing;
    if (f.model == Probe::Missing)
        return Availability::ModelMissing;
    return Availability::Available;
}

// The reason in log text; the same strings serve as menu notes (static storage).
constexpr const char* ReasonText(Availability a)
{
    switch (a)
    {
    case Availability::Available: return "";
    case Availability::ConfigOff: return "";
    case Availability::Latched: return "switched off for this session after a failure";
    case Availability::BridgeNotUp: return "the DirectX 11 on 12 bridge is not running (the main upscaler must be the DLSS-on-12 backend)";
    case Availability::BridgeShuttingDown: return "the graphics bridge is shutting down";
    case Availability::ReleasePending: return "the objects of the previous graphics device are still being released";
    case Availability::NotNvidia: return "Neural Rendering needs an NVIDIA graphics card";
    case Availability::ForwarderMissing: return "nvngx.dll_dlssnr.dll is missing or incomplete";
    case Availability::ModelMissing: return "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
    }
    return "";
}

// A state that explains a missing effect and is worth a log line.
constexpr bool WorthLogging(Availability a) { return a != Availability::Available && a != Availability::ConfigOff; }

// One log line per change of availability, at most `kMaxStateLines` over the session (a flapping bridge must not flood the log).
inline constexpr uint32_t kMaxStateLines = 12;

struct StateLog
{
    Availability last = Availability::ConfigOff;
    uint32_t lines = 0;

    // True when the caller should write a line for `a` now. Returning to Available is also worth a line after a reason was logged.
    bool ShouldLog(Availability a)
    {
        if (a == last)
            return false;
        const bool wasReason = WorthLogging(last);
        last = a;
        if (lines >= kMaxStateLines)
            return false;
        if (!WorthLogging(a) && !(a == Availability::Available && wasReason))
            return false;
        ++lines;
        return true;
    }
};

// ---- The two latches ---------------------------------------------------------------------------------------------------------
// The super-resolution path and preview NR fail independently: a failed SR feature must not stop NR, and a failed NR must not stop
// SR. "Plumbing alive" is what keeps the preview dispatcher offering draws: it stays true while either path may still run.
class Latches
{
  public:
    // Each returns true for the call that set the latch (the caller writes the one log line).
    bool LatchSr() { return !_sr.exchange(true); }
    bool LatchNr() { return !_nr.exchange(true); }
    bool SrLatched() const { return _sr.load(std::memory_order_relaxed); }
    bool NrLatched() const { return _nr.load(std::memory_order_relaxed); }

    bool SrMayRun(bool srWanted) const { return srWanted && !SrLatched(); }
    bool NrMayRun(bool nrWanted) const { return nrWanted && !NrLatched(); }
    bool PlumbingAlive(bool srWanted, bool nrWanted) const { return SrMayRun(srWanted) || NrMayRun(nrWanted); }

  private:
    std::atomic<bool> _sr { false };
    std::atomic<bool> _nr { false };
};

// ---- Streams: binding, linger and release -------------------------------------------------------------------------------------
// What a stream's GPU objects were built for. A change of any of these needs a new stream: the size, the model's creation-time
// preset and style, and the hand-off variant (the late variant is decided when the stream is created).
struct StreamShape
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t preset = 0;
    int style = 0;
    bool late = false;

    friend bool operator==(const StreamShape&, const StreamShape&) = default;
};

struct StreamState
{
    uintptr_t key = 0; // the preview image (the finisher's output texture) the stream belongs to; 0 = free
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t lastLive = 0;      // last time the stream's preview was drawn and run
    uint64_t lastRun = 0;       // last time NR ran for it
    uint64_t runs = 0;          // successful NR runs (results produced); the parity picks the result image
    uint64_t chain = 0;         // consecutive runs since the history was last reset (a late consumer needs two)
    bool everRan = false;
    uint64_t lastSubmitted = 0; // completion value of the newest list that used the stream (0 = nothing submitted)
    uint64_t attempts = 0;      // bring-up calls that reached this stream
    uint64_t ranAttempt = 0;    // the attempt whose run produced the newest result (0 = none)

    StreamShape built {};       // what the GPU objects were built for (valid when hasBuilt)
    bool hasBuilt = false;
    StreamShape pending {};     // a different shape that was asked for and is waiting to hold still
    uint64_t pendingSince = 0;
    bool hasPending = false;

    bool Used() const { return key != 0; }
    // Something was submitted to the GPU that may not be finished: the single source of truth for "release waits for the GPU".
    bool HasGpuWork() const { return lastSubmitted != 0; }
    // The newest bring-up call for this stream produced the result that is there. A call that was skipped, still preparing or failed
    // makes the older result stale: it belongs to an earlier render and must not be drawn for this one.
    bool ResultIsCurrent() const { return everRan && ranAttempt != 0 && ranAttempt == attempts; }
};

// A size, preset, style or variant change must hold still before the stream is rebuilt: a window dragged to a new size reports a new
// size every frame, and each one would otherwise mean a new model (the main pass settles in the same way).
inline constexpr uint64_t kRebuildSettleMs = 500;

enum class RebuildAction : uint8_t
{
    Keep,    // the stream is what is wanted
    Settle,  // something else is wanted but has not held still yet: keep the native image for now
    Rebuild, // it held still: release the stream and build a new one
};

inline RebuildAction DecideRebuild(StreamState& st, const StreamShape& wanted, uint64_t now)
{
    if (!st.hasBuilt || st.built == wanted)
    {
        st.hasPending = false;
        return RebuildAction::Keep;
    }
    if (!st.hasPending || !(st.pending == wanted))
    {
        st.pending = wanted;
        st.pendingSince = now;
        st.hasPending = true;
        return RebuildAction::Settle;
    }
    return (now >= st.pendingSince && now - st.pendingSince >= kRebuildSettleMs) ? RebuildAction::Rebuild : RebuildAction::Settle;
}

class StreamTable
{
  public:
    int Find(uintptr_t key) const
    {
        if (key == 0)
            return kNoStream;
        for (size_t i = 0; i < kMaxStreams; ++i)
        {
            if (_streams[i].key == key)
                return static_cast<int>(i);
        }
        return kNoStream;
    }

    // The stream of `key`, claiming a free one when it has none. kNoStream when the table is full.
    int Bind(uintptr_t key, uint32_t width, uint32_t height, uint64_t now)
    {
        const int existing = Find(key);
        if (existing != kNoStream)
            return existing;
        if (key == 0)
            return kNoStream;
        for (size_t i = 0; i < kMaxStreams; ++i)
        {
            if (!_streams[i].Used())
            {
                _streams[i] = StreamState {};
                _streams[i].key = key;
                _streams[i].width = width;
                _streams[i].height = height;
                _streams[i].lastLive = now;
                return static_cast<int>(i);
            }
        }
        return kNoStream;
    }

    StreamState& At(int index) { return _streams[static_cast<size_t>(index)]; }
    const StreamState& At(int index) const { return _streams[static_cast<size_t>(index)]; }
    void Free(int index) { _streams[static_cast<size_t>(index)] = StreamState {}; }

    size_t LiveCount(uint64_t now) const
    {
        size_t n = 0;
        for (const auto& s : _streams)
            n += (s.Used() && now >= s.lastLive && now - s.lastLive <= kLiveMs) ? 1 : 0;
        return n;
    }
    size_t UsedCount() const
    {
        size_t n = 0;
        for (const auto& s : _streams)
            n += s.Used() ? 1 : 0;
        return n;
    }

  private:
    std::array<StreamState, kMaxStreams> _streams {};
};

enum class StreamAction : uint8_t
{
    Keep,    // still within its window or its linger time
    WaitGpu, // due for release, but its GPU work is not proven complete yet: ask again later
    Release,
};

struct StreamFacts
{
    bool disabled = false; // preview NR is off, latched or its bridge is going away: no linger
    uint64_t now = 0;
    uint64_t lastLive = 0;
    uint64_t lingerMs = LingerMs(kDefaultLingerSeconds);
    bool hasGpuWork = false;
    bool gpuDone = true; // meaningful when hasGpuWork
};

// A stream is released `max(lingerMs, kLiveMs)` after the last time its preview was live (a reopened window within that time finds the
// model built), at once when NR is disabled, and in both cases only after its GPU work completed. A window that is open is drawn at the
// preview's redraw rate, so whatever linger time is configured (0 included) a stream is never released before the live window has
// passed since its last draw; otherwise it would be rebuilt between two draws.
constexpr StreamAction DecideStream(const StreamFacts& f)
{
    const uint64_t grace = f.lingerMs > kLiveMs ? f.lingerMs : kLiveMs;
    const bool idle = f.now >= f.lastLive && f.now - f.lastLive > grace;
    if (!idle && !f.disabled)
        return StreamAction::Keep;
    if (f.hasGpuWork && !f.gpuDone)
        return StreamAction::WaitGpu;
    return StreamAction::Release;
}

// The facts DecideStream needs, from the stream's own state: the "has GPU work" input is the stream's, not a second copy of the rule.
inline StreamFacts FactsOf(const StreamState& st, bool disabled, uint64_t now, uint64_t lingerMs, bool gpuDone)
{
    StreamFacts f;
    f.disabled = disabled;
    f.now = now;
    f.lastLive = st.lastLive;
    f.lingerMs = lingerMs;
    f.hasGpuWork = st.HasGpuWork();
    f.gpuDone = gpuDone;
    return f;
}

// ---- History reset ------------------------------------------------------------------------------------------------------------
enum class ResetReason : uint8_t
{
    None,
    FirstRun, // a new stream, or one rebuilt after a resize: nothing to continue from
    Gap,      // the window was closed or hidden for longer than kResetGapMs
};

constexpr ResetReason DecideReset(bool everRan, uint64_t lastRun, uint64_t now)
{
    if (!everRan)
        return ResetReason::FirstRun;
    if (now < lastRun || now - lastRun > kResetGapMs)
        return ResetReason::Gap;
    return ResetReason::None;
}

constexpr const char* ResetReasonText(ResetReason r)
{
    switch (r)
    {
    case ResetReason::None: return "continued";
    case ResetReason::FirstRun: return "first run";
    case ResetReason::Gap: return "after a gap";
    }
    return "?";
}

// ---- The fence hand-off ------------------------------------------------------------------------------------------------------
// Two fences, each signalled from ONE side only and with strictly growing values (a fence signalled from two queues can be raised by
// one side while the other side's earlier work is still running, and an earlier value written later lowers it):
//   ready : created on D3D11, opened on D3D12. Only the D3D11 context signals it; only the D3D12 queue waits on it.
//   done  : created on D3D12, opened on D3D11. Only the D3D12 queue signals it (its values are also the completion values the module
//           tracks for allocators and releases); only the D3D11 context waits on it.
// One NR run in the order the GPU queues must see it:
//   0. (late variant) D3D11 : Wait(done of the run that last read this colour image)   the copy below may overwrite it
//      D3D11 context  : copy the native image into the shared colour image
//   1. D3D11 context  : Signal(ready)            the preview image is written
//   2. D3D12 queue    : Wait(ready)              NR starts after it
//   3. D3D12          : record, execute the NR list, Signal(done)
//   4. D3D11 context  : Wait(done)               the consumer draws after NR (the late variant waits one frame later, in the consumer)
// Rule: step 4 is issued only after the D3D12 signal was queued. A D3D11 wait on a value that is never signalled would hang the game's
// D3D11 queue, so on any failure in steps 0-3 nothing waits and the native or super-resolution image is used. The module owns these
// fences and their values; the bridge's own fence is never touched.
enum class HandoffStep : uint8_t
{
    ReuseWait,
    Signal11,
    Wait12,
    Record,
    Wait11,
};

constexpr const char* HandoffStepName(HandoffStep s)
{
    switch (s)
    {
    case HandoffStep::ReuseWait: return "D3D11 wait before the colour copy";
    case HandoffStep::Signal11: return "D3D11 signal";
    case HandoffStep::Wait12: return "D3D12 wait";
    case HandoffStep::Record: return "NR recording or D3D12 signal";
    case HandoffStep::Wait11: return "D3D11 wait";
    }
    return "?";
}

struct FenceCounter
{
    uint64_t last = 0;
    uint64_t Next() { return ++last; } // values only ever grow, also across failures
};

struct HandoffOutcome
{
    bool ok = false;
    HandoffStep failedAt = HandoffStep::Signal11; // meaningful when !ok
    uint64_t ready = 0;
    uint64_t done = 0; // 0 until the D3D12 signal was queued
};

// `Ops` provides bool Signal11(uint64_t ready), Wait12(uint64_t ready), Record(uint64_t& done) and Wait11(uint64_t done).
// Record executes the list and queues the D3D12 signal of `done`; it sets `done` to the value it took from the done counter and returns
// false when the recording or the signal failed. With `waitInline` false (the late variant) step 4 is not issued here: the consumer
// waits on `done` one frame later.
template <class Ops> HandoffOutcome RunHandoff(Ops& ops, FenceCounter& ready, bool waitInline = true)
{
    HandoffOutcome out;
    out.ready = ready.Next();
    if (!ops.Signal11(out.ready))
    {
        out.failedAt = HandoffStep::Signal11;
        return out;
    }
    if (!ops.Wait12(out.ready))
    {
        out.failedAt = HandoffStep::Wait12;
        return out;
    }
    uint64_t done = 0;
    if (!ops.Record(done))
    {
        out.failedAt = HandoffStep::Record;
        return out;
    }
    out.done = done;
    if (waitInline && !ops.Wait11(done))
    {
        out.failedAt = HandoffStep::Wait11;
        return out;
    }
    out.ok = true;
    return out;
}

// The value the D3D11 queue waits for on the done fence before the colour copy overwrites a colour image: the late variant has two
// colour images, and the run that last read this one (two runs ago) has to be finished. 0: nothing to wait for.
constexpr uint64_t ColourReuseWait(bool late, uint64_t doneValueOfBuffer) { return late ? doneValueOfBuffer : 0; }

// A whole run on the D3D11 side and the hand-off, as the module issues it. `Ops` additionally provides bool WaitColourFree(uint64_t
// done) and void CopyColour().
template <class Ops> HandoffOutcome RunFrame(Ops& ops, FenceCounter& ready, bool late, uint64_t doneValueOfBuffer)
{
    const uint64_t reuse = ColourReuseWait(late, doneValueOfBuffer);
    if (reuse != 0 && !ops.WaitColourFree(reuse))
    {
        HandoffOutcome out;
        out.failedAt = HandoffStep::ReuseWait;
        return out;
    }
    ops.CopyColour();
    return RunHandoff(ops, ready, !late);
}

// ---- The late variant ---------------------------------------------------------------------------------------------------------
// Results alternate between two images. A consumer draw gets the newest result, or in the late variant the one before it. -1: there is
// no result to show yet. The late variant needs two results of the same chain (since the last history reset): the one before the
// newest may be from before a gap, or from before a rebuild, and is not shown.
constexpr int ResultBufferForRun(uint64_t runIndex) { return static_cast<int>(runIndex & 1u); }

constexpr int ConsumerBuffer(bool late, uint64_t runs, uint64_t chain)
{
    if (runs == 0)
        return -1;
    if (!late)
        return ResultBufferForRun(runs - 1);
    return (runs >= 2 && chain >= 2) ? ResultBufferForRun(runs - 2) : -1;
}

// The value a consumer waits for on the done fence before it samples the result: the late variant did not wait inline, so it waits for
// the run that wrote the image it is about to read; the default variant waited right after the run. 0: nothing to wait for.
constexpr uint64_t ConsumerWaitValue(bool late, uint64_t doneValueOfBuffer) { return late ? doneValueOfBuffer : 0; }

// What the consumer path issues (`Ops` provides bool WaitResult(uint64_t done)); false: the wait could not be queued.
template <class Ops> bool ConsumerWait(Ops& ops, bool late, uint64_t doneValueOfBuffer)
{
    const uint64_t wait = ConsumerWaitValue(late, doneValueOfBuffer);
    return wait == 0 || ops.WaitResult(wait);
}

// ---- Cost logging -------------------------------------------------------------------------------------------------------------
inline constexpr uint64_t kCostLogIntervalMs = 5000;

// Collects the cost of runs and says when a summary line is due: the first run after a pause and then every interval.
struct CostAccumulator
{
    uint64_t count = 0;
    double sum = 0.0;
    double max = 0.0;
    uint64_t lastLog = 0;
    bool logged = false;

    void Add(double ms)
    {
        ++count;
        sum += ms;
        if (ms > max)
            max = ms;
    }

    bool Due(uint64_t now) const
    {
        if (count == 0)
            return false;
        return !logged || now < lastLog || now - lastLog >= kCostLogIntervalMs;
    }

    struct Summary
    {
        uint64_t count = 0;
        double average = 0.0;
        double max = 0.0;
    };

    Summary Take(uint64_t now)
    {
        Summary s;
        s.count = count;
        s.average = count != 0 ? sum / static_cast<double>(count) : 0.0;
        s.max = max;
        count = 0;
        sum = 0.0;
        max = 0.0;
        lastLog = now;
        logged = true;
        return s;
    }
};

// ---- The boundary towards the game ----------------------------------------------------------------------------------------------
// The module's entry points run inside the game's Draw hook and the bridge's callbacks; an exception that leaves one of them would
// leave the game's call. Every entry runs its body through Guarded: an exception is stopped here, `onThrow` (which latches the module
// with one fixed log line and returns the fallback value) answers instead, and nothing is thrown out. `onThrow` must not format paths or
// call anything that can throw; if it does anyway, a value-initialised result is returned.
template <class Body, class OnThrow> auto Guarded(Body&& body, OnThrow&& onThrow) noexcept -> decltype(body())
{
    try
    {
        return body();
    }
    catch (...)
    {
        try
        {
            return onThrow();
        }
        catch (...)
        {
            return decltype(body())();
        }
    }
}
} // namespace ext::ffxiv::chara::nr
