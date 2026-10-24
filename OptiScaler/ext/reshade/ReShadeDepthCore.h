#pragma once
// Automatic ReShade scene depth: the decision logic over a small backend interface, so it is tested with a fake
// (tests/ext/reshade/reshade_depth_core.cpp). No ReShade, D3D or ImGui types, no globals, no logging.
//
// While ReShade renders its effects (begin_effects .. finish_effects) the DEPTH semantic points at a copy of the game's depth
// that has the size of the effect runtime's back buffer; finish_effects puts the original binding back. Whatever is not clearly
// right (another device, a manual depth transform, no depth input, no or stale capture, a size mismatch) leaves ReShade alone
// and ReShade keeps the depth it detected itself.
//
// The core never releases a copy reference that ReShade may still hold: ReShade's binding map does not own the view. A restore
// that fails or cannot be verified keeps the runtime "dirty" (the copy and the original stay referenced) and every begin or
// finish of that runtime retries it first. Only destroy_effect_runtime (the map is gone with the runtime) lets go without a
// ReShade call.
//
// Threading: the capture side calls PublishCapture on the NGX thread. The ReShade events, Status(), GetCounters() and
// ConsumerSeen() may come from any thread. One recursive mutex guards the per-runtime state and is taken inside each event,
// never held from begin to finish. A second small mutex guards the capture record and the status, so PublishCapture and Status()
// never wait for a ReShade call. Lock order: the runtime mutex, then the capture mutex.
//
// Backend contract: none of the backend calls the core makes fires a ReShade event, so a destroy or a reload cannot arrive inside
// one. A reload between begin_effects and finish_effects can arrive, and is handled.

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
inline constexpr int64_t kConsumerWindowMs = 1000; // ConsumerSeen: the last begin that found a depth input is at most this old

// The capture the copy was made from (the capture side fills it after every successful evaluate).
struct DepthCapture
{
    bool valid = false;
    uint32_t outputW = 0, outputH = 0; // NGX Output size, which is also the size of the copy
    uint32_t renderW = 0, renderH = 0; // the feature's render size
    uint64_t frame = 0;                // monotonically increasing; the SAME counter as IBackend::Frame()
    int64_t tickMs = 0;                // IBackend::NowMs() at the capture
    uint64_t copyView = 0;             // shader resource view of the copy (the core takes its own reference per pass)
    uint64_t device = 0;               // the D3D11 device the copy lives on; 0 = unknown (the device gate is skipped)
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
// The ReShade events the depth binder subscribes, by the ReShade addon_event they map to. The last two only clear the manual
// transform cache (OnTechniquesChanged).
enum class DepthEvent : uint8_t
{
    BeginEffects,         // reshade_begin_effects
    FinishEffects,        // reshade_finish_effects
    DestroyEffectRuntime, // destroy_effect_runtime
    ReloadedEffects,      // reshade_reloaded_effects
    TechniqueState,       // reshade_set_technique_state
    PresetPath,           // reshade_set_current_preset_path
};
const char* DepthEventName(DepthEvent event);

// Whether the capture seam should try to register with a ReShade that loaded after OptiScaler (the load-time attempt runs once
// inside DllMain and misses it). The CALLER makes the retry single-flight (a compare-exchange of the last attempt time before
// calling RegisterAddon) and guards the module scan against access violations.
struct LateRegistrationPlanner
{
    static constexpr int64_t kRetryIntervalMs = 1000;
    static constexpr uint32_t kMaxAttempts = 60; // late attempts in all: about a minute, then ReShade is considered absent
    // The late path subscribes exactly these callbacks (the depth ones, never the HudLess probe, present or window events). The
    // boundary tracking subscribes two of the same ReShade events for its own callbacks.
    static constexpr std::array<DepthEvent, 6> kEvents { DepthEvent::BeginEffects,         DepthEvent::FinishEffects,
                                                         DepthEvent::DestroyEffectRuntime, DepthEvent::ReloadedEffects,
                                                         DepthEvent::TechniqueState,       DepthEvent::PresetPath };

    // `attempted`/`status`: result of the last registration attempt (load time or late); `attempts`: late attempts so far.
    // Retry only when the option is on, an attempt found no ReShade module, fewer than kMaxAttempts late attempts were made
    // and the last attempt is at least kRetryIntervalMs old. Never after success (never register twice), a refusal, a
    // missing export, an exception or while the load-time attempt has not run.
    static bool ShouldRetry(bool enabled, bool attempted, RegisterStatus status, uint32_t attempts, int64_t nowMs,
                            int64_t lastAttemptMs);
};

// ---- the backend --------------------------------------------------------------------------------------------------------
// Everything ReShade- or D3D-specific. Ids and handles are opaque 64-bit values: a runtime id is the effect_runtime pointer, a
// view is an ID3D11ShaderResourceView pointer (resource_view::handle), variables are ReShade's variable handles. Matching of
// names, annotations and definitions is the core's job: the backend returns them raw. Methods return false (or 0) on failure
// and may throw; the core never lets either reach ReShade.
//
// Strings from ReShade are fetched with the size query first (a null buffer), then a buffer of that size, and the terminating
// NUL is not part of the value. Never a fixed-size overload: a truncated "1.05" would read as "1.0" and be taken as neutral.
// The same goes for variable names, the "source" annotation and effect names.
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
enum class DefinitionState : uint8_t
{
    Undefined,
    Defined,
    Error, // the query failed: the core stands down for this pass and asks again at the next one
};

struct IBackend
{
    virtual ~IBackend() = default;

    virtual int64_t NowMs() = 0;  // monotonic milliseconds, the clock of DepthCapture::tickMs
    virtual uint64_t Frame() = 0; // the current frame, from the counter of DepthCapture::frame

    // COM reference counting of a shader resource view.
    virtual void AddRef(uint64_t view) = 0;
    virtual void Release(uint64_t view) = 0;
    // The resource of the view has a depth-family format (D16/D24/D32 and their typeless or single-channel views).
    virtual bool IsDepthView(uint64_t view) = 0;

    // ---- one effect runtime
    virtual bool IsD3D11(uint64_t runtime) = 0;
    // The runtime's device is the capture's device (the backend unwraps ReShade's proxy device and may fall back the way the
    // runtime choice of the HudLess boundary does).
    virtual bool SameDevice(uint64_t runtime, uint64_t captureDevice) = 0;
    virtual bool BackBufferSize(uint64_t runtime, uint32_t& width, uint32_t& height) = 0;

    virtual bool TextureVariables(uint64_t runtime, std::vector<TextureVar>& out) = 0;
    virtual bool TextureBinding(uint64_t runtime, uint64_t variable, uint64_t& srv, uint64_t& srvSrgb) = 0;
    virtual bool UpdateTextureBindings(uint64_t runtime, const char* semantic, uint64_t srv, uint64_t srvSrgb) = 0;

    virtual bool UniformVariables(uint64_t runtime, std::vector<UniformVar>& out) = 0;
    virtual bool GetUniformBool(uint64_t runtime, uint64_t variable, bool& value) = 0;
    virtual bool SetUniformBool(uint64_t runtime, uint64_t variable, bool value) = 0;

    virtual bool Techniques(uint64_t runtime, std::vector<EffectTechnique>& out) = 0;
    // A preprocessor definition: `effect` null = global scope, otherwise that effect's.
    virtual DefinitionState Definition(uint64_t runtime, const char* effect, const char* name, std::string& value) = 0;
};

// ---- the binder ---------------------------------------------------------------------------------------------------------
enum class Outcome : uint8_t
{
    Bound,              // the DEPTH semantic points at the copy until finish_effects
    Disabled,           // feature off
    OtherDevice,        // the runtime is not ours: another device (no status text, see Counter::OtherDevice)
    NotD3D11,           // the runtime is not a D3D11 runtime (no status text)
    ManualTransform,    // a RESHADE_DEPTH_INPUT_* macro is set
    NoDepthInput,       // no texture variable with a known depth input name, or one that cannot be trusted
    NoCapture,          // a depth input exists but the capture side has not published a capture yet ("waiting")
    CaptureUnavailable, // a capture exists but is stale or has no copy
    SizeMismatch,       // the runtime's back buffer is not the capture's output size
    Failed,             // a backend call failed or threw, or a restore from an earlier pass is still pending
};

enum class Counter : uint8_t
{
    OverwrittenByOther,
    StaleCaptures,
    SizeMismatches,
    ManualTransformStandDowns,
    NoDepthInput,
    PassesBound,
    OtherDevice,
    Count
};
inline constexpr size_t kCounters = static_cast<size_t>(Counter::Count);
const char* CounterName(Counter counter);

struct Counters
{
    uint64_t overwrittenByOther = 0; // finish_effects found the binding replaced (by another add-on) and left it
    uint64_t staleCaptures = 0;      // a capture that is too old, from the future, or has no copy (not: no capture yet)
    uint64_t sizeMismatches = 0;
    uint64_t manualTransformStandDowns = 0;
    uint64_t noDepthInput = 0;
    uint64_t passesBound = 0;
    uint64_t otherDevice = 0; // begins of runtimes that are not ours: another device, or not D3D11
};

// Called for the first increment of a counter and every 1000th (ShouldLog), with the new value. May be null.
using LogFn = void (*)(Counter counter, uint64_t count);

class DepthBinder
{
  public:
    explicit DepthBinder(IBackend& backend, LogFn log = nullptr);

    // Starts disabled. Any thread. Disabling does not cut a pass short: finish_effects still puts the binding back.
    void SetEnabled(bool enabled) noexcept;

    // The capture side, after every evaluate (valid = false withdraws the capture). The core takes its reference to
    // capture.copyView under the capture lock, so the owner must keep the view alive until the call that replaces it has
    // returned.
    void PublishCapture(const DepthCapture& capture) noexcept;

    // True if the last begin that found a trusted depth input is at most kConsumerWindowMs old: the capture side only runs
    // while ReShade has an effect that reads depth. A begin with no capture yet records it, which is what opens the gate.
    bool ConsumerSeen(int64_t nowMs) const noexcept;

    // ReShade events (any thread, never throw). `runtime` = the effect_runtime pointer.
    // A begin while a pass of the same runtime is open ends that pass first and starts a new one; the finish of the pass that
    // was ended then finds nothing open (nested begin/finish: the first finish closes the second pass, the second is a no-op).
    Outcome OnBeginEffects(uint64_t runtime) noexcept;
    void OnFinishEffects(uint64_t runtime) noexcept;
    // The runtime and its binding map are gone: the references of an open pass are released, no ReShade call.
    void OnDestroyRuntime(uint64_t runtime) noexcept;
    // Effects were reloaded: forgets the manual transform cache and the trust latch. An open pass is restored first (the
    // variable is looked up again, its handle from begin is stale); if that fails the runtime stays dirty with its references.
    void OnReloadedEffects(uint64_t runtime) noexcept;
    // A technique was switched or the preset changed: forgets the manual transform cache only.
    void OnTechniquesChanged(uint64_t runtime) noexcept;

    // One line for the menu: why the last begin did what it did (last writer wins across runtimes; a runtime that is not ours
    // never writes it). Copy; built here, so the refusal paths do not allocate.
    std::string Status() const noexcept;
    Counters GetCounters() const noexcept;

  private:
    struct SavedBool
    {
        uint64_t variable;
        bool value;
    };
    enum class Trust : uint8_t
    {
        Unknown,   // the original binding of the depth input has not been looked at yet
        Trusted,   // it was empty or a depth view
        Untrusted, // it was something else: a variable with the right name but not the DEPTH semantic
    };
    struct Runtime
    {
        std::optional<bool> manual; // RESHADE_DEPTH_INPUT_* macros non-neutral; known until reload, destroy or a technique change
        Trust trust = Trust::Unknown; // until reload or destroy

        // The pass. open = references held; inPass = between begin and finish; bound = update_texture_bindings(copy) was
        // issued and the binding is not known to be put back. open && !inPass is the "dirty" state.
        bool open = false;
        bool inPass = false;
        bool bound = false;
        uint64_t copy = 0;
        uint64_t variable = 0;                // the depth input variable of the pass (valid until the pass ends)
        uint64_t origSrv = 0, origSrgb = 0;   // the binding before the pass
        bool refSrv = false, refSrgb = false; // a reference to the original view is held
        std::vector<SavedBool> bools;         // bufready_depth values before the pass
    };
    enum class End : uint8_t
    {
        Finish,   // finish_effects: the handles of the pass, counts a replaced binding
        Rollback, // a begin that failed: the handles of the pass
        Retry,    // an earlier pass that never finished, or a restore that is still pending: look everything up again
        Reload,   // reloaded effects: look the variable up again, the uniforms are new objects
        Forget,   // destroy: release only
    };
    enum class StatusKind : uint8_t
    {
        Disabled,
        Waiting,
        Unavailable,
        SizeMismatch,
        Manual,
        NoInput,
        Failed,
        Bound,
    };
    struct StatusInfo
    {
        StatusKind kind = StatusKind::Disabled;
        uint32_t renderW = 0, renderH = 0, outputW = 0, outputH = 0; // Bound only
    };

    Outcome Begin(uint64_t runtime);
    Outcome Bind(uint64_t runtime, Runtime& state);
    void EndPass(uint64_t runtime, Runtime& state, End how) noexcept;
    bool RestoreBinding(uint64_t runtime, Runtime& state, End how) noexcept;
    void RestoreBools(uint64_t runtime, Runtime& state, End how) noexcept;
    void ReleasePass(Runtime& state) noexcept;
    void Forget(uint64_t runtime, bool reload) noexcept;
    bool ManualTransform(uint64_t runtime, bool& manual);
    bool AnyNonNeutral(uint64_t runtime, const char* effect, bool& error);
    DepthCapture TakeCapture(uint64_t& copyView);
    void ReleaseView(uint64_t view) noexcept;
    Outcome Done(Outcome outcome);
    void SetStatus(StatusKind kind, const DepthCapture* capture = nullptr) noexcept;
    void Bump(Counter counter) noexcept;

    IBackend& _backend;
    LogFn _log;
    std::atomic<bool> _enabled { false };
    std::atomic<int64_t> _consumerMs { INT64_MIN }; // INT64_MIN = no begin has found a depth input yet
    std::atomic<uint64_t> _counts[kCounters] {};

    std::recursive_mutex _mutex; // _runtimes
    std::map<uint64_t, Runtime> _runtimes;

    mutable std::mutex _publicMutex; // _capture, _status
    DepthCapture _capture;
    StatusInfo _status;
};
} // namespace ext::reshade::depth
