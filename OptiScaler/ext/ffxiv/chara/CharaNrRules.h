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
// SR. What keeps the preview dispatcher offering draws is decided from these flags by PlumbingActive (CharaNrPlumbing.h).
class Latches
{
  public:
    // Each returns true for the call that set the latch (the caller writes the one log line).
    bool LatchSr() { return !_sr.exchange(true); }
    bool LatchNr() { return !_nr.exchange(true); }
    bool SrLatched() const { return _sr.load(std::memory_order_relaxed); }
    bool NrLatched() const { return _nr.load(std::memory_order_relaxed); }

  private:
    std::atomic<bool> _sr { false };
    std::atomic<bool> _nr { false };
};

// ---- Streams: binding, linger and release -------------------------------------------------------------------------------------
// What a stream's GPU objects were built for. A change of any of these needs a new stream: the size, the model's creation-time
// preset and style, the hand-off variant (the late variant is decided when the stream is created) and the mode.
//
// Two modes share the stream table, keyed by the preview image's texture pointer (one stream per image, whichever mode runs):
//   bring-up   : colour = the native image (width x height), guides = synthetic images of the same size; the result is drawn instead of
//                the native image by the UI draws (ResultFor); PreviewNR without PreviewDLSS, and renders the SR path did not evaluate.
//   production : colour = the super-resolution output (width x height = twice the render size), guides = copies of the preview's own
//                depth and motion at the render size (guideWidth x guideHeight); the result is read by the alpha pass of the SR path.
// The two never run at once for one image. When the wanted mode changes the stream is rebuilt once the change has held still for the
// settle time (DecideRebuild), exactly like a size change; a render in between keeps the native image or the SR result.
struct StreamShape
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t preset = 0;
    int style = 0;
    bool late = false;
    bool production = false;
    uint32_t guideWidth = 0;  // production only: size of the depth and motion images (0 in bring-up: same as width x height)
    uint32_t guideHeight = 0;

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
    // A render of this preview image happened and the module produced no result for it (the finisher was rejected, the bring-up was not
    // run or returned without a result): whatever result the stream holds belongs to an earlier render and must not be shown for this
    // one. It also breaks the chain: the late variant shows the run before the newest, which must not reach back over
    // this render.
    void NoteRenderWithoutResult()
    {
        ++attempts;
        chain = 0;
    }
};

// What kind of result a stream holds right now.
enum class ResultKind : uint8_t
{
    None,       // no current result
    BringUp,    // the NR result of the native image (native size, original alpha): UI draws sample it instead of the native image
    Production, // the raw NR output of the super-resolution image (2x, before the alpha pass): only the SR path's alpha pass reads it
};

inline ResultKind ResultKindOf(const StreamState& st)
{
    if (!st.hasBuilt || !st.ResultIsCurrent())
        return ResultKind::None;
    return st.built.production ? ResultKind::Production : ResultKind::BringUp;
}

// A UI draw that asks for the bring-up result of an image is given only a bring-up result. A production stream's image has no alpha
// recomposition and a different size; handing it out would show the SR path's intermediate image on screen.
constexpr bool ServesBringUpConsumer(ResultKind kind) { return kind == ResultKind::BringUp; }

// The images a production run copies into its stream, as the D3D11 side reports them (DXGI format numbers). A copy between images of a
// different size or format is not allowed by Direct3D, so the module checks first and treats a mismatch as a failure of the module.
struct ImageInfo
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
};
inline constexpr uint32_t kFormatRgba16Float = 10; // DXGI_FORMAT_R16G16B16A16_FLOAT: the super-resolution output
inline constexpr uint32_t kFormatR32Float = 41;    // DXGI_FORMAT_R32_FLOAT: the guides pass's depth
inline constexpr uint32_t kFormatRg16Float = 34;   // DXGI_FORMAT_R16G16_FLOAT: the guides pass's motion

constexpr bool ProductionInputsFit(const StreamShape& s, const ImageInfo& colour, const ImageInfo& depth, const ImageInfo& motion)
{
    return s.production && s.width != 0 && s.height != 0 && s.guideWidth != 0 && s.guideHeight != 0 &&
           colour.width == s.width && colour.height == s.height && colour.format == kFormatRgba16Float &&
           depth.width == s.guideWidth && depth.height == s.guideHeight && depth.format == kFormatR32Float &&
           motion.width == s.guideWidth && motion.height == s.guideHeight && motion.format == kFormatRg16Float;
}

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

// ---- Renders without a result --------------------------------------------------------------------------------------------------
// The dispatcher calls these for a finisher whose bring-up did not produce a result (CharaNr.h Invalidate), and the bring-up itself for
// a call that did not run. `key` is the preview image's texture pointer. True: a stream of that key exists and was marked.
inline bool InvalidateResult(StreamTable& table, uintptr_t key)
{
    const int index = table.Find(key);
    if (index == kNoStream)
        return false;
    table.At(index).NoteRenderWithoutResult();
    return true;
}

// A bring-up call that reached the stream (it counted as an attempt when it started) and ended without a run: the chain is broken so the
// late variant does not show a run from before this render.
inline void NoteBringUpEnded(StreamTable& table, uintptr_t key, bool ran)
{
    if (ran)
        return;
    const int index = table.Find(key);
    if (index != kNoStream)
        table.At(index).chain = 0;
}

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

// ---- The done signal ------------------------------------------------------------------------------------------------------------
// The done fence value of a list is taken BEFORE its signal is queued (a caller that tracks the list has the value even when the signal
// fails), and values only grow. A signal that fails behind a list that ran leaves the fence below that value for good: the fence is
// "broken", nothing waiting for the value (allocator reuse, the release of the stream) would ever be satisfied, and the next repair signals
// a newer value behind everything queued before it, which catches the fence up.
// `signal(value)` queues the signal on the D3D12 queue and returns whether it was accepted.
template <class Signal> bool SignalDoneStep(FenceCounter& done, bool& broken, uint64_t& value, Signal&& signal)
{
    value = done.Next();
    if (signal(value))
        return true;
    broken = true;
    return false;
}

// Repairs a broken done fence. `queueUsable` is false when the queue pointer may be gone (a device change or a bridge restart): then
// nothing is signalled and the old objects are released once the fence reports them complete. True when the fence was repaired.
template <class Signal> bool RepairDoneStep(FenceCounter& done, bool& broken, bool queueUsable, Signal&& signal)
{
    if (!broken || !queueUsable)
        return false;
    if (!signal(done.Next()))
        return false;
    broken = false;
    return true;
}

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

// ---- Renders that kept the image ----------------------------------------------------------------------------------------------------
// A render for which a call ended without a result (not available, still preparing, skipped, failed, or not offered at all) kept the native
// image (bring-up) or the plain super-resolution image (production). Counted ONCE per call, at the one place a call ends (the module's
// RunCore wrapper, its failure handlers and NoteProductionDeclined): inner steps pass their outcome up without counting. `production`
// counts the calls of the production entry (a subset of `all`).
struct FallbackTally
{
    std::atomic<uint64_t> all { 0 };
    std::atomic<uint64_t> production { 0 };

    void Note(bool fromProduction)
    {
        all.fetch_add(1, std::memory_order_relaxed);
        if (fromProduction)
            production.fetch_add(1, std::memory_order_relaxed);
    }
};

// The one place a call ends: runs `steps` (which pass their outcome up without counting) and counts the call once when `ran(outcome)` is
// false.
template <class Steps, class Ran> auto CountedCall(FallbackTally& tally, bool fromProduction, Steps&& steps, Ran&& ran) -> decltype(steps())
{
    auto outcome = steps();
    if (!ran(outcome))
        tally.Note(fromProduction);
    return outcome;
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
// call anything that can throw; if it does anyway, a value-initialised result is returned (for RunOutcome that is Failed, see CharaNr.h).
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

// The boundary of an entry that tears objects down (Maintain, OnBridgeShutdown, Detach). An exception is stopped like in Guarded, and
// before anything that can throw again the release is marked pending (`markPending` must be a non-throwing store): the retry in Maintain
// then frees what the failed pass left, and nothing builds new objects meanwhile. `onThrow` latches the module (one log line, once).
template <class Body, class MarkPending, class OnThrow> void GuardedTeardown(Body&& body, MarkPending&& markPending, OnThrow&& onThrow) noexcept
{
    Guarded(body,
            [&]
            {
                markPending();
                onThrow();
            });
}
} // namespace ext::ffxiv::chara::nr
