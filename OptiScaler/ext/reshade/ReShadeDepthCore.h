#pragma once
// Automatic ReShade scene depth: the decision logic over a small backend interface, so it is tested with a fake
// (tests/ext/reshade/reshade_depth_core.cpp). No ReShade, D3D or ImGui types, no globals, no logging.
//
// While ReShade renders its effects (begin_effects .. finish_effects) the DEPTH semantic points at a copy of the game's depth
// that has the size of the effect runtime's back buffer; finish_effects puts the original binding back. Whatever is not clearly
// right (no or stale capture, another device, a size mismatch, a manual depth transform, no depth input) leaves ReShade alone
// and ReShade keeps the depth it detected itself.
//
// Threading: the capture side calls PublishCapture on the NGX thread. The ReShade events, Status() and GetCounters() may come
// from any thread. One recursive mutex guards the per-runtime state and is taken inside each event, never held from begin to
// finish. A second small mutex guards the capture record and the status text, so PublishCapture and Status() never wait for a
// ReShade call.

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ext::reshade
{
enum class RegisterStatus : uint8_t; // ReShadeRegister.h
}

namespace ext::reshade::depth
{
// ---- pure helpers -------------------------------------------------------------------------------------------------------
inline constexpr int64_t kMaxCaptureAgeMs = 250; // a capture older than this is stale (251 ms is stale)
inline constexpr int64_t kMaxCaptureFrames = 1;  // ... and so is one more than this many frames old (or from the future)

// The capture the copy was made from (the capture side fills it after every successful evaluate).
struct DepthCapture
{
    bool valid = false;
    uint32_t outputW = 0, outputH = 0; // NGX Output size, which is also the size of the copy
    uint32_t renderW = 0, renderH = 0; // the feature's render size
    uint64_t frame = 0;                // monotonically increasing frame counter (same counter as IBackend::Frame)
    int64_t tickMs = 0;                // IBackend::NowMs() at the capture
    uint64_t copyView = 0;             // shader resource view of the copy (the core takes its own reference per pass)
    uint64_t device = 0;               // the D3D11 device the copy lives on
};

// valid, at most kMaxCaptureAgeMs old and 0 or kMaxCaptureFrames frames behind `frame`. Signed arithmetic: a capture from the
// future (negative age or frame delta) is stale.
bool IsFresh(const DepthCapture& capture, int64_t nowMs, uint64_t frame);

// RESHADE_DEPTH_INPUT_* macro value check: true only if the whole string (surrounding white space removed, one trailing f/F
// allowed) is a number exactly equal to `neutral`. Empty, unparsable, inf/nan, trailing junk and a leading '+' are not neutral.
bool IsNeutralDefinition(std::string_view value, double neutral);

// Log the first occurrence of a counter and every 1000th after it.
bool ShouldLog(uint64_t count);

// ---- late registration --------------------------------------------------------------------------------------------------
// The ReShade events the depth binder uses, by the ReShade addon_event they map to.
enum class DepthEvent : uint8_t
{
    BeginEffects,         // reshade_begin_effects
    FinishEffects,        // reshade_finish_effects
    DestroyEffectRuntime, // destroy_effect_runtime
    ReloadedEffects,      // reshade_reloaded_effects
};
const char* DepthEventName(DepthEvent event);

// Whether the capture seam should try to register with a ReShade that loaded after OptiScaler (the load-time attempt runs once
// inside DllMain and misses it).
struct LateRegistrationPlanner
{
    static constexpr int64_t kRetryIntervalMs = 1000;
    // The late path subscribes exactly these events, never the boundary tracking, probe or window events.
    static constexpr std::array<DepthEvent, 4> kEvents { DepthEvent::BeginEffects, DepthEvent::FinishEffects,
                                                         DepthEvent::DestroyEffectRuntime, DepthEvent::ReloadedEffects };

    // `attempted`/`status`: result of the last registration attempt (load time or late). Retry only when the option is on, an
    // attempt found no ReShade module and the last attempt is at least kRetryIntervalMs old. Never after success (never
    // register twice), a refusal, a missing export, an exception or while the load-time attempt has not run.
    static bool ShouldRetry(bool enabled, bool attempted, RegisterStatus status, int64_t nowMs, int64_t lastAttemptMs);
};

// ---- the backend --------------------------------------------------------------------------------------------------------
// Everything ReShade- or D3D-specific. Ids and handles are opaque 64-bit values: a runtime id is the effect_runtime pointer, a
// view is an ID3D11ShaderResourceView pointer (resource_view::handle), variables are ReShade's variable handles. Matching of
// names, annotations and definitions is the core's job: the backend returns them raw. Methods return false (or 0) on failure
// and may throw; the core never lets either reach ReShade.
struct TextureVar
{
    uint64_t handle = 0;
    std::string name; // unique name as ReShade reports it, e.g. "ReShade::DepthBufferTex"
};
struct UniformVar
{
    uint64_t handle = 0;
    std::string source; // the "source" annotation, empty when there is none
};
struct EffectTechnique
{
    std::string effect; // file name of the effect the technique belongs to
    bool enabled = false;
};

struct IBackend
{
    virtual ~IBackend() = default;

    virtual int64_t NowMs() = 0;  // monotonic milliseconds, the clock of DepthCapture::tickMs
    virtual uint64_t Frame() = 0; // the frame counter of DepthCapture::frame (the current frame)

    // COM reference counting of a shader resource view.
    virtual void AddRef(uint64_t view) = 0;
    virtual void Release(uint64_t view) = 0;

    // ---- one effect runtime
    virtual bool IsD3D11(uint64_t runtime) = 0;
    virtual uint64_t Device(uint64_t runtime) = 0; // device->get_native(), 0 = unknown
    virtual bool BackBufferSize(uint64_t runtime, uint32_t& width, uint32_t& height) = 0;

    virtual bool TextureVariables(uint64_t runtime, std::vector<TextureVar>& out) = 0;
    virtual bool TextureBinding(uint64_t runtime, uint64_t variable, uint64_t& srv, uint64_t& srvSrgb) = 0;
    virtual bool UpdateTextureBindings(uint64_t runtime, const char* semantic, uint64_t srv, uint64_t srvSrgb) = 0;

    virtual bool UniformVariables(uint64_t runtime, std::vector<UniformVar>& out) = 0;
    virtual bool GetUniformBool(uint64_t runtime, uint64_t variable, bool& value) = 0;
    virtual bool SetUniformBool(uint64_t runtime, uint64_t variable, bool value) = 0;

    virtual bool Techniques(uint64_t runtime, std::vector<EffectTechnique>& out) = 0;
    // A preprocessor definition: `effect` null = global scope, otherwise that effect's. True = defined (value in `value`).
    virtual bool Definition(uint64_t runtime, const char* effect, const char* name, std::string& value) = 0;
};

// ---- the binder ---------------------------------------------------------------------------------------------------------
enum class Outcome : uint8_t
{
    Bound,             // the DEPTH semantic points at the copy until finish_effects
    Disabled,          // feature off
    Duplicate,         // second begin of this runtime in the same frame
    OtherDevice,       // the runtime's device is not the capture's device
    NotD3D11,          // the runtime is not a D3D11 runtime
    CaptureUnavailable, // no capture, a stale one, or one without a copy
    SizeMismatch,      // the runtime's back buffer is not the capture's output size
    ManualTransform,   // a RESHADE_DEPTH_INPUT_* macro is set
    NoDepthInput,      // no texture variable with a known depth input name
    Failed,            // a backend call failed or threw (everything it changed was put back)
};

enum class Counter : uint8_t
{
    OverwrittenByOther,
    StaleCaptures,
    SizeMismatches,
    ManualTransformStandDowns,
    NoDepthInput,
    PassesBound,
    Count
};
inline constexpr size_t kCounters = static_cast<size_t>(Counter::Count);
const char* CounterName(Counter counter);

struct Counters
{
    uint64_t overwrittenByOther = 0; // finish_effects found the binding replaced (by another add-on) and left it
    uint64_t staleCaptures = 0;      // no capture, a stale one, or one without a copy
    uint64_t sizeMismatches = 0;
    uint64_t manualTransformStandDowns = 0;
    uint64_t noDepthInput = 0;
    uint64_t passesBound = 0;
};

// Called for the first increment of a counter and every 1000th (ShouldLog), with the new value. May be null.
using LogFn = void (*)(Counter counter, uint64_t count);

class DepthBinder
{
  public:
    explicit DepthBinder(IBackend& backend, LogFn log = nullptr);

    // Starts disabled. Any thread. Disabling does not cut a pass short: finish_effects still puts the binding back.
    void SetEnabled(bool enabled);

    // The capture side, after every evaluate (valid = false withdraws the capture). The core takes its reference to
    // capture.copyView under the capture lock, so the owner must keep the view alive until the call that replaces it has
    // returned.
    void PublishCapture(const DepthCapture& capture);

    // ReShade events (any thread, never throw). `runtime` = the effect_runtime pointer.
    Outcome OnBeginEffects(uint64_t runtime);
    void OnFinishEffects(uint64_t runtime);
    void OnDestroyRuntime(uint64_t runtime);  // forgets the runtime; releases the references of an open pass, no ReShade call
    void OnReloadedEffects(uint64_t runtime); // same: the variable handles of an open pass are stale

    // One line for the menu: why the last begin did what it did. Copy.
    std::string Status() const;
    Counters GetCounters() const;

  private:
    struct SavedBool
    {
        uint64_t variable;
        bool value;
    };
    struct Runtime
    {
        bool hasFrame = false;
        uint64_t frame = 0;
        std::optional<bool> manual; // RESHADE_DEPTH_INPUT_* macros non-neutral; known until effects reload or the runtime ends

        // the open pass (begin without finish yet)
        bool open = false;
        bool bound = false;      // update_texture_bindings(copy) was issued
        uint64_t copy = 0;       // our reference (always held while open)
        uint64_t variable = 0;   // the depth input variable the original was read from
        uint64_t origSrv = 0, origSrgb = 0;
        bool refSrv = false, refSrgb = false; // a reference to the original view is held
        std::vector<SavedBool> bools;         // bufready_depth values before the pass
    };
    enum class End : uint8_t
    {
        Finish,   // finish_effects: restore, count a replaced binding
        Rollback, // a failed begin: restore, nothing to count
        Forget,   // destroy / reload: the handles are stale, release only
    };

    Outcome Begin(uint64_t runtime);
    Outcome Bind(uint64_t runtime, Runtime& state, uint64_t frame);
    void EndPass(uint64_t runtime, Runtime& state, End how);
    void RestoreBinding(uint64_t runtime, Runtime& state, bool count);
    void Forget(uint64_t runtime);
    bool ManualTransform(uint64_t runtime, bool& manual);
    bool AnyNonNeutral(uint64_t runtime, const char* effect);
    DepthCapture TakeCapture(uint64_t& copyView);
    void ReleaseView(uint64_t view);
    Outcome Done(Outcome outcome);
    void SetStatus(std::string text);
    void Bump(Counter counter);

    IBackend& _backend;
    LogFn _log;
    std::atomic<bool> _enabled { false };
    std::atomic<uint64_t> _counts[kCounters] {};

    std::recursive_mutex _mutex; // _runtimes
    std::map<uint64_t, Runtime> _runtimes;

    mutable std::mutex _publicMutex; // _capture, _status
    DepthCapture _capture;
    std::string _status;
};
} // namespace ext::reshade::depth
