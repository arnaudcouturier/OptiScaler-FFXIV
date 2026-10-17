#pragma once
// The probe session (S10 package D0): receives the ReShade runtime events, keeps the runtime registry, the F3 frame
// ledger and the report, and (later in this file) runs the armed one-frame probe. Everything ReShade- or D3D-specific
// sits behind IBackend so the logic is tested with a fake (tests/ext/reshade). No ReShade or ImGui types here.

#include "ext/reshade/ReShadeProbeCore.h"
#include "ext/reshade/ReShadeReport.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ext::reshade::probe
{
// ---- the armed one-frame probe --------------------------------------------------------------------------------------
// How to make ReShade's own end-of-frame pass not run after the probe's render_effects call, for ONE frame. The header
// documents render_effects itself as doing that; the second method is the fallback and is only meant to be used when the
// first one was seen NOT to work (one arm = one frame = one method).
enum class SkipMethod : uint8_t
{
    RenderEffectsOnly,  // rely on the documented side effect of render_effects
    AlsoDisableEffects, // additionally set_effects_state(false) until reshade_present, then restore
};
const char* SkipMethodName(SkipMethod method);

// The render target bound at the boundary, as seen by the backend.
struct BoundaryInfo
{
    bool ok = false;           // acceptable as render_effects target
    std::string refusal;       // why not
    uint64_t context = 0;      // the ID3D11DeviceContext the caller passed
    uint64_t cmdListNative = 0; // reshade immediate command list ->get_native()
    uint64_t resource = 0;     // bound RT0 resource
    uint64_t rtv = 0;          // what is passed as rtv / rtv_srgb
    uint64_t rtvSrgb = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;       // DXGI value (ReShade's format enum has the same numbers)
    uint32_t samples = 0;
    uint32_t arraySize = 0;
    bool isRuntimeBackBuffer = false; // the resource IS one of the runtime's back buffers
    std::string note;          // how rtv_srgb was derived etc.
};

struct DepthReport
{
    bool anyDepthBound = false;         // a depth-family texture is bound to a depth-named effect variable
    bool sizeMatchesBackBuffer = false;  // ... and its size equals the runtime back buffer
    std::string summary;                 // first such binding: name, size, format, back buffer size
    std::vector<std::string> lines;      // "[F4] ..." lines
};

struct CallOutcome
{
    bool ok = false;         // render_effects returned normally
    bool exception = false;  // C++ exception or SEH (access violation, ...)
    uint32_t code = 0;       // SEH exception code
    std::string what;
    bool stateRestored = false; // the D3D11 pipeline state block was restored after the call
    std::string stateNote;
};

struct PixelDiff
{
    bool valid = false;
    std::string note;           // why not valid / sampling description
    double meanAbs = 0;         // mean absolute channel delta, 0..255 scale
    double changedFraction = 0; // share of sampled pixels with any channel delta
    uint32_t maxDelta = 0;
    uint64_t sampled = 0;
    double readbackUs = 0;      // CPU time spent in the two staging readbacks (GPU stalls), excluded from the net probe time
};

struct IBackend
{
    virtual ~IBackend() = default;

    virtual int64_t Qpc() = 0;
    virtual int64_t QpcFrequency() = 0;

    // Fresh description of a live runtime (the id is an effect_runtime pointer the caller got from an event a moment
    // ago). `withTechniques` also lists the techniques (valid only after reshade_reloaded_effects). False = failed.
    virtual bool Describe(uint64_t runtime, bool withTechniques, RuntimeInfo& out) = 0;

    // ---- invasive probe steps (boundary thread). None of these may throw or let an access violation escape.
    // The runtime of the game chain among `alive` for this D3D11 context (0 = none), with the reason in `why`.
    virtual uint64_t PickRuntime(void* context, const std::vector<uint64_t>& alive, std::string& why) = 0;
    // RT0 bound on `context`, checked against the runtime's back buffer.
    virtual BoundaryInfo Inspect(void* context, uint64_t runtime) = 0;
    virtual DepthReport QueryDepth(uint64_t runtime, const BoundaryInfo& boundary) = 0;
    virtual void SnapshotBefore(void* context, const BoundaryInfo& boundary) = 0;
    virtual PixelDiff SnapshotAfterAndDiff(void* context, const BoundaryInfo& boundary) = 0;
    // effect_runtime::render_effects in place on the boundary target, with the pipeline state saved/restored, the fork's
    // hooks bypassed and SEH around the call.
    virtual CallOutcome RenderEffects(void* context, uint64_t runtime, const BoundaryInfo& boundary) = 0;
    virtual bool GetEffectsState(uint64_t runtime, bool& enabled) = 0;
    virtual bool SetEffectsState(uint64_t runtime, bool enabled) = 0;

    // ---- S10 package E (live boundary, ReShadeBoundaryCore.h). Non-pure with inert defaults so the probe's fakes stay valid.
    // Number of techniques of the runtime and how many are enabled (names are not read: cheap). False = unknown.
    virtual bool CountTechniques(uint64_t runtime, uint32_t& total, uint32_t& enabled)
    {
        (void)runtime, (void)total, (void)enabled;
        return false;
    }
    // Drops every reference Inspect() kept (render target views, the target resource). A reference held across Present makes
    // the game's ResizeBuffers fail, so the live path calls this on every exit.
    virtual void EndBoundary() {}
};

// True on the thread that is inside the probe's own render_effects call: the begin/finish events it fires belong to the
// probe, not to ReShade's end-of-frame pass.
bool InProbeCall();
struct ProbeCallScope
{
    ProbeCallScope();
    ~ProbeCallScope();
    ProbeCallScope(const ProbeCallScope&) = delete;
    ProbeCallScope& operator=(const ProbeCallScope&) = delete;
};

// Snapshot for the menu card.
struct SessionStatus
{
    EventCounts counts;
    uint32_t aliveRuntimes = 0;
    uint64_t framesSeen = 0; // reshade_present events of all runtimes
    std::string reportFolder; // empty until the first line was written
    bool reportFailed = false;
};

class Session
{
  public:
    Session(IBackend& backend, IReport& report);

    // First line of the report: why we are here (registration result, version, options). Safe to call once.
    void Begin(const std::string& registrationLine);

    // Event entry points. Any thread, never throw.
    void OnInitRuntime(uint64_t runtime);
    void OnDestroyRuntime(uint64_t runtime);
    void OnReloadedEffects(uint64_t runtime);
    void OnPresetPath(uint64_t runtime, const char* path);
    void OnEffects(EffectsEvent event);
    void OnReshadePresent(uint64_t runtime);
    void OnQueuePresent();

    // Number of runtimes currently alive / description of one (copy). Used by the probe and the menu.
    uint32_t AliveRuntimes() const;
    bool Runtime(uint64_t runtime, RuntimeInfo& out) const;

    EventCounts Counts() const;
    void Note(const std::string& line); // free-form report line (thread-safe)

    SessionStatus Status(const std::string& reportFolder, bool reportFailed) const;

    // ---- the armed probe -----------------------------------------------------------------------------------------
    static constexpr uint32_t kArmTimeoutFrames = 240; // reshade_present events before an unused arm is dropped
    static constexpr uint32_t kAwaitTimeoutFrames = 5; // ... before an unanswered frame end is given up

    enum class ProbeState : uint8_t
    {
        Idle,
        Armed,         // waiting for ProbeAtBoundary
        Running,       // inside ProbeAtBoundary
        AwaitFrameEnd, // render_effects done, waiting for reshade_present to count the end-of-frame passes
    };
    struct ProbeStatus
    {
        ProbeState state = ProbeState::Idle;
        SkipMethod method = SkipMethod::RenderEffectsOnly;
        uint32_t runs = 0;
        bool inert = false;      // latched after an exception in the probe: no further arm
        std::string lastResult;  // one line for the menu card
    };

    // Menu thread. False (with the reason in `why`) when the probe is not idle or latched inert.
    bool Arm(SkipMethod method, std::string* why = nullptr);
    void Disarm();
    bool Armed() const { return _state.load(std::memory_order_relaxed) == ProbeState::Armed; } // one load per call
    ProbeStatus ProbeStatusCopy() const;

    // Boundary thread (the detector's candidate boundary, immediate context). Does nothing unless armed.
    void ProbeAtBoundary(void* context);

  private:
    struct Pending
    {
        uint64_t runtime = 0;
        SkipMethod method = SkipMethod::RenderEffectsOnly;
        bool effectsStateChanged = false;
        bool previousEffectsState = true;
        uint32_t framesWaited = 0;
        CallOutcome outcome;
        PixelDiff diff;
        double callUs = 0;
        double totalUs = 0;
        bool statePerformed = false;
    };

    void FinishFrame(uint64_t runtime);   // reshade_present of the awaited runtime
    void GiveUpPending(const char* why);
    void Fail(const std::string& why, bool latchInert);
    void SetResult(const std::string& line);
    std::vector<uint64_t> AliveIds() const;

    struct Entry
    {
        RuntimeInfo info;
        bool alive = false;
        uint32_t generation = 0; // how many times this pointer value was initialised (ResizeBuffers reuses it)
    };
    static constexpr size_t kRuntimes = 8;

    Entry* FindLocked(uint64_t runtime);
    const Entry* FindLocked(uint64_t runtime) const;
    Entry& FindOrCreateLocked(uint64_t runtime);
    void WriteLines(const std::vector<std::string>& lines);

    IBackend& _backend;
    IReport& _report;
    mutable std::mutex _mutex;
    std::array<Entry, kRuntimes> _runtimes {};
    FrameLedger _ledger;
    std::array<std::atomic<uint64_t>, kEventKinds> _counts {};
    std::atomic<uint64_t> _framesSeen { 0 };
    std::atomic<bool> _f1Verdict { false };

    // probe state machine: _state is the only field read without the mutex (the per-call early-out)
    std::atomic<ProbeState> _state { ProbeState::Idle };
    std::atomic<bool> _inert { false };
    SkipMethod _method = SkipMethod::RenderEffectsOnly; // guarded by _mutex
    uint32_t _runs = 0;                                   // guarded by _mutex
    uint32_t _armedFrames = 0;                            // guarded by _mutex
    Pending _pending;                                     // guarded by _mutex
    std::string _lastResult;                              // guarded by _mutex
};
} // namespace ext::reshade::probe
