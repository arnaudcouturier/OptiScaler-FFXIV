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
// finish of that runtime retries it first.
//
// destroy_effect_runtime does not settle it either: whether ResizeBuffers makes ReShade create a new runtime object or reset the
// same one (the pointer value then comes back, with its binding map) is unverified, and the core does not depend on it. A
// destroy of a runtime that holds nothing forgets it. A destroy of one that still holds the copy (a bound or a dirty pass)
// makes no ReShade call and releases nothing: the entry is kept and marked orphaned. If the pointer comes back, its next begin,
// finish or reload runs the normal retry first (the existing dirty path). If it never does, the references are held until
// Shutdown() or the destructor, which release everything without a ReShade call: a leaked view is acceptable, a view that
// is freed while a map may still hold it is not.
//
// The DEPTH semantic itself cannot be read through the API; a texture variable that is merely NAMED like a depth input may
// not read it at all. So a variable counts as a DEPTH reader for a pass only if, after the bind, it reads back exactly the copy.
// Only the binding that such a variable showed BEFORE the bind is ever written back as the original. If no variable reads the
// copy back the bind is undone with an erase of the entry (update_texture_bindings with empty views): ReShade's own depth is
// lost until Generic Depth selects again, which is visible but safe, and the runtime is not trusted until its effects reload.
// Nothing that the core cannot vouch for is written into ReShade's map, because ReShade frees the views it knows nothing about.
//
// Threading: the capture side calls PublishCapture on the NGX thread. The ReShade events, Status(), GetCounters() and
// GetConsumer() may come from any thread. One recursive mutex guards the per-runtime state and is taken inside each event,
// never held from begin to finish. A second small mutex guards the capture record, the consumer record and the status, so
// PublishCapture and Status() never wait for a ReShade call. Lock order: the runtime mutex, then the capture mutex.
//
// Backend contract: none of the backend calls the core makes fires one of the depth events or OnTechniquesChanged, so a destroy
// or a reload cannot arrive inside one. SetUniformBool can fire reshade_set_uniform_value, which add-ons may veto or answer with
// further API calls; the core does not depend on it taking effect (it checks nothing it set, and puts the old value back
// whatever happened). A reload between begin_effects and finish_effects can arrive, and is handled.

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
inline constexpr int64_t kConsumerWindowMs = 1000; // GetConsumer: the last begin that found a trusted depth input is at most this old
inline constexpr int64_t kLogIntervalMs = 60'000;   // a counter is logged at most this often
inline constexpr int64_t kNeverLogged = INT64_MIN;

// The capture the copy was made from (the capture side fills it after every successful evaluate).
struct DepthCapture
{
    bool valid = false;
    uint32_t outputW = 0, outputH = 0; // NGX Output size, which is also the size of the copy
    uint32_t renderW = 0, renderH = 0; // the feature's render size
    // Monotonically increasing and the SAME counter as IBackend::Frame(). It ticks once per CAPTURE (the capture side ticks it before it
    // publishes), never per effect pass: under frame generation ReShade runs several passes between two evaluates, and all of them see
    // this stamp (delta 0). A capture that stops being made is caught by its age (kMaxCaptureAgeMs), not by this counter.
    uint64_t frame = 0;
    int64_t tickMs = 0;    // IBackend::NowMs() at the capture
    uint64_t copyView = 0; // shader resource view of the copy (the core takes its own reference per pass)
    // The D3D11 device the copy lives on. 0 = no capture yet: the backend then decides for "the game's primary D3D11 device".
    uint64_t device = 0;
};

// valid, at most kMaxCaptureAgeMs old and 0 or kMaxCaptureFrames frames behind `frame`. Signed arithmetic: a capture from the
// future (negative age or frame delta) is stale.
bool IsFresh(const DepthCapture& capture, int64_t nowMs, uint64_t frame);

// One rule for every counter: log its first occurrence, then at most once per kLogIntervalMs (with the count of that moment).
// `lastLoggedMs` is kNeverLogged until the first line. A clock that went backwards logs nothing until the interval has passed
// since the last line; a counter that stopped ticking is simply not asked again (its last occurrences are not logged).
bool ShouldLog(uint64_t count, int64_t nowMs, int64_t lastLoggedMs);

// RESHADE_DEPTH_INPUT_* macro value check: true only if the whole string (surrounding white space removed, one trailing f/F
// allowed) is a number exactly equal to `neutral`. Empty, unparsable, inf/nan, trailing junk and a leading '+' are not neutral.
bool IsNeutralDefinition(std::string_view value, double neutral);

// ---- event subscription, late registration -------------------------------------------------------------------------------------
// The ReShade events the depth binder subscribes, by the ReShade addon_event they map to. TechniqueState and PresetPath only clear
// the manual transform cache (OnTechniquesChanged). InitEffectRuntime is the ordering trick of the load path (see
// LateRegistrationPlanner): its callback subscribes begin and finish, so that they land AFTER the begin_effects of the add-ons that
// register theirs at device creation (REST and ReShade's Generic Depth), which therefore run first and have bound what we read.
enum class DepthEvent : uint8_t
{
    BeginEffects,         // reshade_begin_effects
    FinishEffects,        // reshade_finish_effects
    DestroyEffectRuntime, // destroy_effect_runtime
    ReloadedEffects,      // reshade_reloaded_effects
    TechniqueState,       // reshade_set_technique_state
    PresetPath,           // reshade_set_current_preset_path
    InitEffectRuntime,    // init_effect_runtime
    Count,
};
const char* DepthEventName(DepthEvent event);

// Whether the capture seam should try to register with a ReShade that loaded after OptiScaler (the load-time attempt runs once
// inside DllMain and misses it). The CALLER makes the retry single-flight (a compare-exchange of the last attempt time before
// calling RegisterAddon) and guards the module scan against access violations.
struct LateRegistrationPlanner
{
    static constexpr int64_t kRetryIntervalMs = 1000;
    static constexpr uint32_t kMaxAttempts = 60; // late attempts in all: about a minute, then ReShade is considered absent
    // The late path (a ReShade that loaded after OptiScaler: its runtime exists or is about to, and the add-ons registered long ago)
    // subscribes exactly these callbacks at once (the depth ones, never the HudLess probe, present or window events). Other
    // callbacks of this add-on may sit on the same ReShade events: the HudLess boundary tracking subscribes destroy_effect_runtime,
    // reshade_reloaded_effects, reshade_set_current_preset_path and reshade_set_technique_state with callbacks of its own, and the
    // diagnostics probe begin, finish, destroy, reloaded and preset path with theirs.
    static constexpr std::array<DepthEvent, 6> kEvents { DepthEvent::BeginEffects,         DepthEvent::FinishEffects,
                                                         DepthEvent::DestroyEffectRuntime, DepthEvent::ReloadedEffects,
                                                         DepthEvent::TechniqueState,       DepthEvent::PresetPath };
    // The load path (ReShade was there when OptiScaler loaded) subscribes these at once ...
    static constexpr std::array<DepthEvent, 5> kLoadEvents { DepthEvent::DestroyEffectRuntime, DepthEvent::ReloadedEffects,
                                                             DepthEvent::TechniqueState, DepthEvent::PresetPath,
                                                             DepthEvent::InitEffectRuntime };
    // ... and the init_effect_runtime callback these two, in this order (finish first; begin only if finish and everything else
    // succeeded: a begin without its finish would leave our copy bound). The add-ons that matter registered their begin_effects at
    // device creation, before any effect runtime is initialized, so ours comes after theirs.
    static constexpr std::array<DepthEvent, 2> kRuntimeEvents { DepthEvent::FinishEffects, DepthEvent::BeginEffects };

    // `attempted`/`status`: result of the last registration attempt (load time or late); `attempts`: late attempts so far.
    // Retry only when the option is on, an attempt found no ReShade module, fewer than kMaxAttempts late attempts were made
    // and the last attempt is at least kRetryIntervalMs old. Never after success (never register twice), a refusal, a
    // missing export, an exception or while the load-time attempt has not run.
    static bool ShouldRetry(bool enabled, bool attempted, RegisterStatus status, uint32_t attempts, int64_t nowMs,
                            int64_t lastAttemptMs);
};

// ---- G-buffers realigned like the depth ---------------------------------------------------------------------------------------
// An add-on such as REST (ReshadeEffectShaderToggler) hands the game's G-buffers to ReShade's semantics NORMALS, DECALS, DECALS_WATER,
// MOTIONVECTORS and MATERIAL (update_texture_bindings), and effects read them with plain UV 0..1. When the game renders below the output
// size those textures are output-sized with the picture only in their top-left render window, exactly like the depth. After the add-on's
// own begin_effects the binder reads its binding, remaps the render window to the whole output (the same exact-integer nearest mapping
// as the depth pass, origin 0,0) and binds the copy to the same semantic until finish_effects. Kept apart from the depth: the sources
// are the add-on's views, not a capture, and the copy is made at begin_effects on the present thread (IRemapEngine).
enum class GBuffer : uint8_t
{
    Normals,
    Decals,
    DecalsWater,
    MotionVectors,
    Material,
    Count,
};
inline constexpr size_t kGBuffers = static_cast<size_t>(GBuffer::Count);
const char* GBufferSemantic(GBuffer buffer); // the ReShade semantic: "NORMALS", "DECALS", "DECALS_WATER", "MOTIONVECTORS", "MATERIAL"
const char* GBufferVariable(GBuffer buffer); // the SHORT name effects declare it under: "texNormals", "texDecalNormals", ...
const char* GBufferLabel(GBuffer buffer);    // for the status line: "normals", "decals", "water decals", "motion", "material"

// What a view is, as far as the remap needs it. The handle is untrusted (read from ReShade's binding map): the engine reads it under a
// fault guard and answers ok = false when that fails.
struct ViewDesc
{
    bool ok = false;      // the handle could be read; the rest is meaningful only then
    bool plain2D = false; // a TEXTURE2D view of mip 0 of a one-slice, single-sample 2D texture (anything else is not remapped)
    uint32_t width = 0, height = 0; // of that mip
    uint32_t format = 0;  // DXGI_FORMAT of the VIEW (not of the resource, which may be typeless)
    uint64_t device = 0;  // the D3D11 device of the resource (pointer value): INFORMATION for the log only. Under ReShade's proxy one device shows as two
                          // pointers, so it is never compared: the target is made on the context's device, and the add-on's views come from that runtime
};

// One remap: copy the render window (origin 0,0, renderW x renderH) of the source view, stretched by exact-integer nearest sampling
// (src = min((i*2+1)*render/(output*2), render-1)) to a new texture of outputW x outputH in the same format as the SOURCE VIEW.
struct RemapRequest
{
    uint64_t context = 0;            // the immediate context begin_effects gave (ID3D11DeviceContext*); a non-immediate one is refused
    GBuffer buffer = GBuffer::Normals;
    uint64_t srv = 0, srvSrgb = 0;   // the add-on's bindings (ID3D11ShaderResourceView*, untrusted); srvSrgb may be 0 or equal to srv
    uint32_t renderW = 0, renderH = 0, outputW = 0, outputH = 0;
    uint64_t stamp = 0;              // the depth capture counter: with `srv` it keys the cache (same key = same copy, no draw)
    bool scaleMotion = true;         // MotionVectors only: multiply the vector values by output/render per axis (DERIVED, see the docs)
};
enum class RemapStatus : uint8_t
{
    Ok,                // out holds the copy
    NoContext,         // no (or a deferred) context: keep the add-on's binding
    NotPlain2D,        // the source view is not a plain 2D view
    UnsupportedFormat, // a format the pass does not remap (typeless, integer, sRGB, depth, ...)
    Refused,           // sizes the pass cannot do (zero, beyond the texture limit, render outside the source)
    Failed,            // a resource could not be created or the draw failed; the game's state was put back
};
struct RemapOut
{
    uint64_t srv = 0, srvSrgb = 0; // views of the copy; srvSrgb equals srv unless the format has an sRGB twin. NOT AddRef'd: the owner keeps them
                                   // until its next Remap or Release; whoever binds them takes its own reference first (same thread, no gap)
    uint32_t format = 0;           // DXGI format of the copy's srv
    bool cached = false;           // the key matched: nothing was drawn
};
// The D3D11 side of the realignment (ReShadeGBufferCopy.h). Runs on the thread of begin_effects (the present thread) and nowhere else.
class IRemapEngine
{
  public:
    virtual ~IRemapEngine() = default;
    virtual bool DescribeView(uint64_t view, ViewDesc& out) noexcept = 0;
    virtual RemapStatus Remap(const RemapRequest& request, RemapOut& out) noexcept = 0;
    // Drops every target and resource (the option went off). References the binder took on a copy keep that copy alive.
    virtual void Release() noexcept = 0;
};

// The size rule, per buffer, after the add-on has bound its source. `bbW x bbH` is the effect runtime's back buffer (the output size).
enum class RemapDecision : uint8_t
{
    Remap,             // output-sized source, render window smaller than the output: copy it
    KeepSameSize,      // render == output: nothing to realign
    KeepRenderSized,   // the source is already render-sized: ReShade's UV 0..1 already covers it
    KeepSizeMismatch,  // any other size (or a render window larger than the output)
    KeepUnreadable,    // the source view could not be described or is not a plain 2D view
};
RemapDecision DecideRemap(const ViewDesc& source, uint32_t bbW, uint32_t bbH, uint32_t renderW, uint32_t renderH, uint32_t outputW,
                          uint32_t outputH);
const char* RemapDecisionName(RemapDecision decision);

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
    std::string name; // the SHORT declared name ReShade reports (no namespace, no mangling): "DepthBufferTex", "DepthInputTex"
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

    // Monotonic milliseconds, the clock of DepthCapture::tickMs. The core calls it (and the log hook, see LogFn) with the runtime
    // mutex held, from whatever thread the ReShade event arrives on: the real one must be cheap, lock-free and never wait for ReShade.
    virtual int64_t NowMs() = 0;
    virtual uint64_t Frame() = 0; // the current game frame, from the counter of DepthCapture::frame (one counter, see there)

    // COM reference counting of a shader resource view. The views come from ReShade's binding map, which holds raw pointers: the
    // real backend calls these (and IsDepthView) through the same access violation guard as every other ReShade call.
    virtual void AddRef(uint64_t view) = 0;
    virtual void Release(uint64_t view) = 0;
    // The resource of the view has a depth-family format (D16/D24/D32 and their typeless or single-channel views). The handle
    // is untrusted (it was read from ReShade's map). False, also when the query itself fails, makes the core distrust the depth
    // input of that runtime until its effects reload: it fails closed. A throw is a failed pass, not a verdict.
    virtual bool IsDepthView(uint64_t view) = 0;

    // ---- one effect runtime
    virtual bool IsD3D11(uint64_t runtime) = 0;
    // The runtime's device is the capture's device. The game (and ReShade's runtime) see ReShade's PROXY objects: context->GetDevice() and
    // GetDevice() of the resources created on them answer the proxy, while the runtime's device::get_native() is the real d3d11.dll
    // device; the two pointers never compare equal and nothing in this layer unwraps one into the other, so the live backend compares
    // through the runtime's back buffer resource and falls back to "the only D3D11 runtime" (see LiveDepthBackend::SameDevice).
    // captureDevice 0 means there is no capture yet: the backend then decides for "the game's primary D3D11 device". The core always
    // asks, so a runtime of another device never counts as a consumer.
    virtual bool SameDevice(uint64_t runtime, uint64_t captureDevice) = 0;
    virtual bool BackBufferSize(uint64_t runtime, uint32_t& width, uint32_t& height) = 0;

    virtual bool TextureVariables(uint64_t runtime, std::vector<TextureVar>& out) = 0;
    // Told once per runtime (and again after its effects were reloaded) when its texture variables contain a known depth input: the
    // names of those variables, best first, so the log shows which ones matched (the short declared names). Inert by default; an exception
    // is swallowed by the core.
    virtual void DepthInputsFound(uint64_t runtime, const std::vector<std::string>& names)
    {
        (void)runtime;
        (void)names;
    }
    // Told once per runtime (and again after its effects were reloaded) when its texture variables contain no known depth input: the
    // live backend logs the names that contain "depth", so the first run answers which names real presets use (an MXAO-only preset
    // declares DepthInputTex only). Inert by default; an exception is swallowed by the core.
    virtual void NoDepthCandidate(uint64_t runtime, const std::vector<TextureVar>& textures)
    {
        (void)runtime;
        (void)textures;
    }
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
    NoDepthInput,       // no texture variable with a known depth input name, or none that reads the DEPTH semantic
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
    Failed,
    ErasedUnverified,
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
    uint64_t noDepthInput = 0;       // no depth input, or one that is not trusted
    uint64_t passesBound = 0;
    uint64_t otherDevice = 0;        // begins of runtimes that are not ours: another device, or not D3D11
    uint64_t failed = 0;             // every Outcome::Failed, and every restore that had to be left pending
    // Erases of the DEPTH entry that no readback of this pass vouched for: a pass in which no variable read the copy back, a
    // retry or reload where no re-resolved variable shows it, and the case of no variable at all. Counted once the erase "went
    // through", which is all the backend can say: the live update_texture_bindings returns void, so "went through" means it did
    // not throw or fault (nothing reads the entry back afterwards). Each one may have lost the
    // pick of another add-on (Generic Depth) until it selects again; a deliberate trade-off against leaving the copy in a map
    // that ReShade frees views of. The in-game run shows how often that happens.
    uint64_t erasedUnverified = 0;
};

// Called when ShouldLog says so (first occurrence, then once per kLogIntervalMs), with the current count. May be null. It runs with
// the runtime mutex held, on the thread of the ReShade event (or of the capture side): it must not call back into the binder.
using LogFn = void (*)(Counter counter, uint64_t count);

// What the capture side needs to know before it has a capture: a depth consumer exists, and how big its back buffer is.
struct ConsumerInfo
{
    bool seen = false;       // the last begin that found a trusted depth input is at most kConsumerWindowMs old
    int64_t seenMs = 0;      // when that was (IBackend::NowMs); 0 if never
    uint32_t width = 0, height = 0; // the back buffer of that runtime, as that begin read it; 0 if never
};

// What real ReShade answers for a genuine reader's get_texture_binding after the bind (exactly the copy) is not settled by reading the
// headers; the pass relies on it (a variable counts as a DEPTH reader only if it reads back exactly the copy). The live backend logs the
// readback of the first real bind once, so the in-game run shows what ReShade really returns.
class DepthBinder
{
  public:
    explicit DepthBinder(IBackend& backend, LogFn log = nullptr);
    // Shutdown(): the process or the test is over.
    ~DepthBinder();
    DepthBinder(const DepthBinder&) = delete;
    DepthBinder& operator=(const DepthBinder&) = delete;

    // Releases every reference the binder still holds (orphaned runtimes included) and forgets all runtimes, without a ReShade
    // call: nothing is put back, the maps are assumed gone. It also disables the binder, so nothing binds after the end (a begin
    // answers Disabled until SetEnabled(true)). Any thread; the destructor calls it. Not for a toggle of the option: while a
    // ReShade runtime may be alive, switch it off with SetEnabled(false), which lets finish_effects put the binding back.
    void Shutdown() noexcept;

    // Starts disabled. Any thread. Disabling does not cut a pass short: finish_effects still puts the binding back.
    void SetEnabled(bool enabled) noexcept;

    // The capture side, after every evaluate (valid = false withdraws the capture). The core takes its reference to
    // capture.copyView under the capture lock, so the owner must keep the view alive until the call that replaces it has
    // returned.
    void PublishCapture(const DepthCapture& capture) noexcept;

    // The capture side asks before every evaluate. It captures only while `seen`, and publishes a capture only if the NGX
    // Output size equals width x height: that is the character-preview filter, and the size of the copy. A begin that found a
    // trusted depth input records it (with the runtime's back buffer size) before it looks at the capture, so this works with
    // no capture yet. With several runtimes the most recent such begin wins. A begin whose back buffer size cannot be read does
    // not record anything.
    ConsumerInfo GetConsumer(int64_t nowMs) const noexcept;

    // ReShade events (any thread, never throw). `runtime` = the effect_runtime pointer.
    // A begin while a pass of the same runtime is open ends that pass first and starts a new one; the finish of the pass that
    // was ended then finds nothing open (nested begin/finish: the first finish closes the second pass, the second is a no-op).
    Outcome OnBeginEffects(uint64_t runtime) noexcept;
    void OnFinishEffects(uint64_t runtime) noexcept;
    // A runtime that holds nothing is forgotten. One that holds the copy (a bound or dirty pass) is kept as orphaned, no ReShade
    // call and no reference released: the pointer may come back (a reset that reuses the object, its map surviving), and then
    // its next begin, finish or reload retries first. See the top of this file.
    void OnDestroyRuntime(uint64_t runtime) noexcept;
    // Effects were reloaded: forgets the manual transform cache and the trust latch. An open pass (or a dirty or orphaned
    // runtime) is ended first: the named variables are looked up again, and if one shows the copy the original goes back, if
    // none does the DEPTH entry is ERASED (which can lose another add-on's pick, counted as erasedUnverified); with no
    // variable at all the entry is erased too. Only a failing ReShade call leaves the runtime dirty with its references.
    void OnReloadedEffects(uint64_t runtime) noexcept;
    // A technique was switched or the preset changed: forgets the manual transform cache only. reshade_set_technique_state fires
    // BEFORE the change, so the wiring clears on the preset-path event as well and the next begin asks again; the window in
    // which a begin can still read the old state is narrow by design.
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
        Unknown,   // the binding of the depth input has not been looked at yet
        Trusted,   // it was empty or a depth view, and a bind has not shown otherwise
        Untrusted, // it was something else, or no variable read the copy back: right name, not the DEPTH semantic
    };
    struct Runtime
    {
        std::optional<bool> manual; // RESHADE_DEPTH_INPUT_* macros non-neutral; known until reload, destroy or a technique change
        Trust trust = Trust::Unknown; // until reload or destroy
        bool noCandidateNoted = false; // IBackend::NoDepthCandidate was called (until reload or destroy)
        bool foundNoted = false;       // IBackend::DepthInputsFound was called (until reload or destroy)

        // The pass. open = references held; inPass = between begin and finish; bound = update_texture_bindings(copy) was
        // issued and the binding is not known to be put back. open && !inPass is the "dirty" state.
        bool open = false;
        bool inPass = false;
        bool bound = false;
        // The runtime was destroyed while it held the copy: kept without a ReShade call. Its stored handles may belong to a
        // runtime that is gone, so a finish of it is a retry (everything is looked up again), like a begin or a reload.
        bool orphaned = false;
        uint64_t copy = 0;
        // True once a variable read the copy back after the bind. Only then are `variable` (valid until the pass ends) and
        // `orig*` (what that variable showed before the bind) meaningful; an unverified pass is undone with an erase.
        bool verified = false;
        uint64_t variable = 0;
        uint64_t origSrv = 0, origSrgb = 0;
        bool refSrv = false, refSrgb = false; // a reference to the original view is held
        std::vector<SavedBool> bools;         // bufready_depth values before the pass
    };
    enum class End : uint8_t
    {
        Finish,   // finish_effects: the handles of the pass, counts a replaced binding
        Rollback, // a begin that failed: the handles of the pass
        Retry,    // an earlier pass that never finished, or a restore that is still pending: look everything up again
        Reload,   // reloaded effects: look the variable up again, the uniforms are new objects
    };
    enum class StatusKind : uint8_t
    {
        Disabled,
        Waiting,
        Unavailable,
        SizeMismatch,
        Manual,
        NoInput,
        NotTrusted,
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
    void RecordConsumer(int64_t nowMs, uint32_t width, uint32_t height) noexcept;
    Outcome Done(Outcome outcome);
    void SetStatus(StatusKind kind, const DepthCapture* capture = nullptr) noexcept;
    void Bump(Counter counter) noexcept;

    IBackend& _backend;
    LogFn _log;
    std::atomic<bool> _enabled { false };
    std::atomic<uint64_t> _counts[kCounters] {};
    std::atomic<int64_t> _lastLogged[kCounters] {}; // set to kNeverLogged by the constructor

    std::recursive_mutex _mutex; // _runtimes
    std::map<uint64_t, Runtime> _runtimes;

    mutable std::mutex _publicMutex; // _capture, _consumer, _status
    DepthCapture _capture;
    bool _consumerKnown = false; // a begin has found a trusted depth input
    int64_t _consumerMs = 0;
    uint32_t _consumerWidth = 0, _consumerHeight = 0;
    StatusInfo _status;
};
} // namespace ext::reshade::depth
