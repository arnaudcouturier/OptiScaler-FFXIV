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
struct StreamState
{
    uintptr_t key = 0; // the preview image (the finisher's output texture) the stream belongs to; 0 = free
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t lastLive = 0;     // last time the stream's preview was drawn and run
    uint64_t lastRun = 0;      // last time NR ran for it
    uint64_t runs = 0;         // successful NR runs (results produced)
    bool everRan = false;
    bool hasGpuWork = false;   // something was submitted that has not been proven complete
    bool Used() const { return key != 0; }
};

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

    // A different size than the stream was built for: its GPU objects and history are rebuilt.
    bool NeedsRebuild(int index, uint32_t width, uint32_t height) const
    {
        const StreamState& s = At(index);
        return s.width != width || s.height != height;
    }

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

// A stream is released `lingerMs` after the last time its preview was live (a reopened window within that time finds the model
// built), at once when NR is disabled, and in both cases only after its GPU work completed.
constexpr StreamAction DecideStream(const StreamFacts& f)
{
    const bool idle = f.now >= f.lastLive && f.now - f.lastLive > f.lingerMs;
    if (!idle && !f.disabled)
        return StreamAction::Keep;
    if (f.hasGpuWork && !f.gpuDone)
        return StreamAction::WaitGpu;
    return StreamAction::Release;
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
// One NR run in the order the GPU queues must see it:
//   1. D3D11 context  : Signal(ready)            the preview image is written
//   2. D3D12 queue    : Wait(ready)              NR starts after it
//   3. D3D12          : record and execute the NR list
//   4. D3D12 queue    : Signal(done)             NR has finished
//   5. D3D11 context  : Wait(done)               the consumer draws after NR
// Rule: step 5 is issued only after step 4 was queued. A D3D11 wait on a value that is never signalled would hang the game's
// D3D11 queue, so on any failure in steps 1-4 nothing waits and the native or super-resolution image is used. The module owns
// this fence and its values; the bridge's own fence is never touched.
enum class HandoffStep : uint8_t
{
    Signal11,
    Wait12,
    Record,
    Signal12,
    Wait11,
};

constexpr const char* HandoffStepName(HandoffStep s)
{
    switch (s)
    {
    case HandoffStep::Signal11: return "D3D11 signal";
    case HandoffStep::Wait12: return "D3D12 wait";
    case HandoffStep::Record: return "NR recording";
    case HandoffStep::Signal12: return "D3D12 signal";
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
    uint64_t done = 0; // 0 until step 4 was queued
};

// `Ops` provides bool Signal11(uint64_t), Wait12(uint64_t), Record(), Signal12(uint64_t) and Wait11(uint64_t).
// With `waitInline` false (the late variant) step 5 is not issued here: the consumer waits on `done` one frame later.
template <class Ops> HandoffOutcome RunHandoff(Ops& ops, FenceCounter& counter, bool waitInline = true)
{
    HandoffOutcome out;
    out.ready = counter.Next();
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
    if (!ops.Record())
    {
        out.failedAt = HandoffStep::Record;
        return out;
    }
    const uint64_t done = counter.Next();
    if (!ops.Signal12(done))
    {
        out.failedAt = HandoffStep::Signal12;
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

// ---- The late variant ---------------------------------------------------------------------------------------------------------
// Results alternate between two images. A consumer draw gets the newest result, or in the late variant the one before it (the GPU
// has certainly finished it, so no wait is needed in the frame that consumes it). -1: there is no result to show yet.
constexpr int ResultBufferForRun(uint64_t runIndex) { return static_cast<int>(runIndex & 1u); }

constexpr int ConsumerBuffer(bool late, uint64_t runs)
{
    if (runs == 0)
        return -1;
    if (!late)
        return ResultBufferForRun(runs - 1);
    return runs >= 2 ? ResultBufferForRun(runs - 2) : -1;
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
} // namespace ext::ffxiv::chara::nr
