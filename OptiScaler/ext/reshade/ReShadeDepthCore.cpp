#include "pch.h"

#include "ext/reshade/ReShadeDepthCore.h"
#include "ext/reshade/ReShadeRegister.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <utility>

namespace ext::reshade::depth
{
namespace
{
const char* const kDepthSemantic = "DEPTH";
constexpr std::string_view kBufReadySource = "bufready_depth";

// Texture variables ReShade effects declare for the depth buffer, by priority. A variable that is merely named like "depth"
// may be an internal texture of an effect (it dangles after a reload) and is never used. Some effects (MXAO) declare only
// DepthInputTex. get_texture_variable_name answers the SHORT declared name (measured against ReShade 6.8.0.2155 on 2026-10-25): no
// "ReShade::" prefix for a texture declared in namespace ReShade, no mangled "V__" form (that one only shows in ReShade's compile
// warnings), and a texture declared in any namespace comes back under its short name. So a decoy Foo::DepthBufferTex reads exactly like
// the real input; the readback after the bind (the variable has to show the copy) is the guard, and a decoy that shows something that is
// not a depth view makes the runtime untrusted (fail closed). Matching stays exact (MXAODepth is rejected).
constexpr const char* kDepthInputNames[] = { "DepthBufferTex", "DepthInputTex" };

// The G-buffers the add-on (REST) binds, by their ReShade semantic, the short name effects declare them under (ffxiv_common.fxh:
// `texture texNormals : NORMALS;` inside namespace FFXIV, which ReShade reports without the namespace) and the word for the status line.
constexpr const char* kGBufferSemantics[kGBuffers] = { "NORMALS", "DECALS", "DECALS_WATER", "MOTIONVECTORS", "MATERIAL" };
constexpr const char* kGBufferVariables[kGBuffers] = { "texNormals", "texDecalNormals", "texDecalWaterNormals", "texNativeMotionVectors",
                                                       "texMaterial" };
constexpr const char* kGBufferLabels[kGBuffers] = { "normals", "decals", "water decals", "motion", "material" };

// Slot 0 is the depth, 1 + i is G-buffer i.
const char* SlotSemantic(size_t slot) { return slot == 0 ? kDepthSemantic : kGBufferSemantics[slot - 1]; }

// Every variable with one of the slot's names (the depth: best name first; a G-buffer has one name). Which of them really reads the
// semantic is not known until the copy has been bound and one of them shows it.
std::vector<const TextureVar*> FindInputs(const std::vector<TextureVar>& textures, size_t slot)
{
    std::vector<const TextureVar*> found;
    if (slot == 0)
    {
        for (const char* name : kDepthInputNames)
            for (const TextureVar& t : textures)
                if (t.name == name)
                    found.push_back(&t);
        return found;
    }
    const char* const name = kGBufferVariables[slot - 1];
    for (const TextureVar& t : textures)
        if (t.name == name)
            found.push_back(&t);
    return found;
}

// The ReShade macros that scale or shift the depth input; any of them off its neutral value means the user (or a preset)
// matched ReShade's depth to something, which a copy at the output size would undo.
struct TransformMacro
{
    const char* name;
    double neutral;
};
constexpr TransformMacro kTransformMacros[] = {
    { "RESHADE_DEPTH_INPUT_X_SCALE", 1.0 },        { "RESHADE_DEPTH_INPUT_Y_SCALE", 1.0 },
    { "RESHADE_DEPTH_INPUT_X_OFFSET", 0.0 },       { "RESHADE_DEPTH_INPUT_Y_OFFSET", 0.0 },
    { "RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET", 0.0 }, { "RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET", 0.0 },
};

// Gives a view reference back on every way out unless it was handed on with Take().
struct ViewRef
{
    IBackend& backend;
    uint64_t view = 0;
    ~ViewRef()
    {
        if (view == 0)
            return;
        try
        {
            backend.Release(view);
        }
        catch (...)
        {
        }
    }
    uint64_t Take() { return std::exchange(view, 0); }
};
} // namespace

// ---- pure helpers -------------------------------------------------------------------------------------------------------
bool IsFresh(const DepthCapture& capture, int64_t nowMs, uint64_t frame)
{
    if (!capture.valid)
        return false;
    const int64_t age = nowMs - capture.tickMs;
    const int64_t behind = static_cast<int64_t>(frame - capture.frame);
    return age >= 0 && age <= kMaxCaptureAgeMs && behind >= 0 && behind <= kMaxCaptureFrames;
}

bool IsNeutralDefinition(std::string_view value, double neutral)
{
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; };
    while (!value.empty() && space(value.front()))
        value.remove_prefix(1);
    while (!value.empty() && space(value.back()))
        value.remove_suffix(1);
    if (!value.empty() && (value.back() == 'f' || value.back() == 'F'))
        value.remove_suffix(1);

    // from_chars does not look at the locale. inf and nan parse but never equal a neutral value.
    double parsed = 0.0;
    const char* const end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, parsed);
    return result.ec == std::errc() && result.ptr == end && parsed == neutral;
}

bool ShouldLog(uint64_t count, int64_t nowMs, int64_t lastLoggedMs)
{
    if (count == 0)
        return false;
    if (lastLoggedMs == kNeverLogged)
        return true;
    if (nowMs < lastLoggedMs) // the clock went backwards: wait until it is a whole interval past the last line
        return false;
    // unsigned: the difference of two int64 with nowMs >= lastLoggedMs is exact modulo 2^64, never an overflow
    return static_cast<uint64_t>(nowMs) - static_cast<uint64_t>(lastLoggedMs) >= static_cast<uint64_t>(kLogIntervalMs);
}

const char* DepthEventName(DepthEvent event)
{
    switch (event)
    {
    case DepthEvent::BeginEffects:
        return "reshade_begin_effects";
    case DepthEvent::FinishEffects:
        return "reshade_finish_effects";
    case DepthEvent::DestroyEffectRuntime:
        return "destroy_effect_runtime";
    case DepthEvent::ReloadedEffects:
        return "reshade_reloaded_effects";
    case DepthEvent::TechniqueState:
        return "reshade_set_technique_state";
    case DepthEvent::PresetPath:
        return "reshade_set_current_preset_path";
    case DepthEvent::InitEffectRuntime:
        return "init_effect_runtime";
    case DepthEvent::Count:
        break;
    }
    return "?";
}

const char* CounterName(Counter counter)
{
    switch (counter)
    {
    case Counter::OverwrittenByOther:
        return "overwrittenByOther";
    case Counter::StaleCaptures:
        return "staleCaptures";
    case Counter::SizeMismatches:
        return "sizeMismatches";
    case Counter::ManualTransformStandDowns:
        return "manualTransformStandDowns";
    case Counter::NoDepthInput:
        return "noDepthInput";
    case Counter::PassesBound:
        return "passesBound";
    case Counter::OtherDevice:
        return "otherDevice";
    case Counter::Failed:
        return "failed";
    case Counter::ErasedUnverified:
        return "erasedUnverified";
    case Counter::Kept:
        return "kept";
    case Counter::Count:
        break;
    }
    return "?";
}

// ---- G-buffers -------------------------------------------------------------------------------------------------------------
const char* GBufferSemantic(GBuffer buffer)
{
    const size_t i = static_cast<size_t>(buffer);
    return i < kGBuffers ? kGBufferSemantics[i] : "?";
}
const char* GBufferVariable(GBuffer buffer)
{
    const size_t i = static_cast<size_t>(buffer);
    return i < kGBuffers ? kGBufferVariables[i] : "?";
}
const char* GBufferLabel(GBuffer buffer)
{
    const size_t i = static_cast<size_t>(buffer);
    return i < kGBuffers ? kGBufferLabels[i] : "?";
}

RemapDecision DecideRemap(const ViewDesc& source, uint32_t bbW, uint32_t bbH, uint32_t renderW, uint32_t renderH, uint32_t outputW,
                          uint32_t outputH)
{
    if (!source.ok || !source.plain2D || source.width == 0 || source.height == 0)
        return RemapDecision::KeepUnreadable;
    if (renderW == 0 || renderH == 0 || outputW == 0 || outputH == 0 || renderW > outputW || renderH > outputH)
        return RemapDecision::KeepSizeMismatch;
    if (renderW == outputW && renderH == outputH)
        return RemapDecision::KeepSameSize;
    if (source.width == renderW && source.height == renderH)
        return RemapDecision::KeepRenderSized;
    // the add-on's buffer has the size of the allocation the game makes, which is the size of ReShade's back buffer (and of the depth copy)
    if (source.width == bbW && source.height == bbH && bbW == outputW && bbH == outputH)
        return RemapDecision::Remap;
    return RemapDecision::KeepSizeMismatch;
}

const char* RemapDecisionName(RemapDecision decision)
{
    switch (decision)
    {
    case RemapDecision::Remap:
        return "realigned (render window stretched to the output)";
    case RemapDecision::KeepSameSize:
        return "kept (render size equals the output size)";
    case RemapDecision::KeepRenderSized:
        return "kept (already render-sized)";
    case RemapDecision::KeepSizeMismatch:
        return "kept (size is neither the output's nor the render size)";
    case RemapDecision::KeepUnreadable:
        return "kept (the view could not be read or is not a plain 2D view)";
    case RemapDecision::KeepNoSource:
        return "kept (nothing bound yet)";
    case RemapDecision::KeepAmbiguous:
        return "kept (the variables of that name show different bindings)";
    }
    return "?";
}

bool LateRegistrationPlanner::ShouldRetry(bool enabled, bool attempted, RegisterStatus status, uint32_t attempts, int64_t nowMs,
                                          int64_t lastAttemptMs)
{
    if (!enabled || !attempted || attempts >= kMaxAttempts)
        return false;
    switch (status)
    {
    case RegisterStatus::NoModule:
        return nowMs - lastAttemptMs >= kRetryIntervalMs;
    case RegisterStatus::Ok:
    case RegisterStatus::NoSelf:
    case RegisterStatus::ExportsMissing:
    case RegisterStatus::Refused:
    case RegisterStatus::Exception:
        break;
    }
    return false;
}

// ---- the binder: small members --------------------------------------------------------------------------------------------
DepthBinder::DepthBinder(IBackend& backend, LogFn log) : _backend(backend), _log(log)
{
    for (auto& slot : _lastLogged)
        for (auto& last : slot)
            last.store(kNeverLogged, std::memory_order_relaxed);
}

DepthBinder::~DepthBinder() { Shutdown(); }

// The process or the test is over: whatever is still held (orphaned runtimes, or a restore that never succeeded) is released,
// with no ReShade call. A leaked view would be acceptable; a view freed while a map may still hold it is not, and that is
// what a call into ReShade at this point could not rule out. The binder is disabled for good (until SetEnabled(true)): nothing binds
// after the end, and a begin that arrives anyway is refused instead of opening a pass that nothing will finish.
void DepthBinder::Shutdown() noexcept
{
    SetEnabled(false);
    try
    {
        std::lock_guard lock(_mutex);
        for (auto& entry : _runtimes)
        {
            if (entry.second.pass.open)
                ReleasePass(entry.second.pass);
            for (GSlot& g : entry.second.gb)
                if (g.pass.open)
                    ReleasePass(g.pass);
        }
        _runtimes.clear();
    }
    catch (...)
    {
    }
}

void DepthBinder::SetEnabled(bool enabled) noexcept
{
    if (_enabled.exchange(enabled) != enabled)
        SetStatus(enabled ? StatusKind::Waiting : StatusKind::Disabled);
}

void DepthBinder::PublishCapture(const DepthCapture& capture) noexcept
{
    try
    {
        std::lock_guard lock(_publicMutex);
        _capture = capture;
    }
    catch (...)
    {
    }
}

ConsumerInfo DepthBinder::GetConsumer(int64_t nowMs) const noexcept
{
    ConsumerInfo info;
    try
    {
        std::lock_guard lock(_publicMutex);
        if (_consumerKnown)
        {
            info.seen = nowMs - _consumerMs <= kConsumerWindowMs;
            info.seenMs = _consumerMs;
            info.width = _consumerWidth;
            info.height = _consumerHeight;
        }
    }
    catch (...)
    {
    }
    return info;
}

void DepthBinder::RecordConsumer(int64_t nowMs, uint32_t width, uint32_t height) noexcept
{
    try
    {
        std::lock_guard lock(_publicMutex);
        _consumerKnown = true;
        _consumerMs = nowMs;
        _consumerWidth = width;
        _consumerHeight = height;
    }
    catch (...)
    {
    }
}

std::string DepthBinder::Status() const noexcept
{
    try
    {
        StatusInfo info;
        {
            std::lock_guard lock(_publicMutex);
            info = _status;
        }
        switch (info.kind)
        {
        case StatusKind::Disabled:
            return "Automatic ReShade depth disabled";
        case StatusKind::Waiting:
            return "Waiting for ReShade depth effects";
        case StatusKind::Unavailable:
            return "Scene depth unavailable; generic depth retained";
        case StatusKind::SizeMismatch:
            return "ReShade size differs from the scene depth; generic depth retained";
        case StatusKind::Manual:
            return "Manual ReShade depth transform retained";
        case StatusKind::NoInput:
            return "No depth input found; generic depth retained";
        case StatusKind::NotTrusted:
            return "Depth input not trusted; generic depth retained";
        case StatusKind::Failed:
            return "ReShade call failed; generic depth retained";
        case StatusKind::Bound:
            if (info.renderW == info.outputW && info.renderH == info.outputH)
                return std::format("Automatic scene depth: {}x{}", info.outputW, info.outputH);
            return std::format("Automatic scene depth: {}x{} -> {}x{}", info.renderW, info.renderH, info.outputW, info.outputH);
        }
    }
    catch (...)
    {
    }
    return {};
}

namespace
{
Counters CountersOf(const std::atomic<uint64_t> (&counts)[kCounters])
{
    auto n = [&](Counter c) { return counts[static_cast<size_t>(c)].load(std::memory_order_relaxed); };
    Counters out;
    out.overwrittenByOther = n(Counter::OverwrittenByOther);
    out.staleCaptures = n(Counter::StaleCaptures);
    out.sizeMismatches = n(Counter::SizeMismatches);
    out.manualTransformStandDowns = n(Counter::ManualTransformStandDowns);
    out.noDepthInput = n(Counter::NoDepthInput);
    out.passesBound = n(Counter::PassesBound);
    out.otherDevice = n(Counter::OtherDevice);
    out.failed = n(Counter::Failed);
    out.erasedUnverified = n(Counter::ErasedUnverified);
    out.kept = n(Counter::Kept);
    return out;
}
} // namespace

Counters DepthBinder::GetCounters() const noexcept { return CountersOf(_counts[0]); }
Counters DepthBinder::GetCounters(GBuffer buffer) const noexcept
{
    const size_t index = static_cast<size_t>(buffer);
    return index < kGBuffers ? CountersOf(_counts[1 + index]) : Counters {};
}

void DepthBinder::SetStatus(StatusKind kind, const DepthCapture* capture) noexcept
{
    try
    {
        StatusInfo info;
        info.kind = kind;
        if (capture != nullptr)
        {
            info.renderW = capture->renderW;
            info.renderH = capture->renderH;
            info.outputW = capture->outputW;
            info.outputH = capture->outputH;
        }
        std::lock_guard lock(_publicMutex);
        _status = info;
    }
    catch (...)
    {
    }
}

void DepthBinder::SetRealigned(size_t index, bool on) noexcept
{
    if (index >= kGBuffers)
        return;
    const uint32_t bit = 1u << static_cast<uint32_t>(index);
    if (on)
        _realigned.fetch_or(bit, std::memory_order_relaxed);
    else
        _realigned.fetch_and(~bit, std::memory_order_relaxed);
}

// The count always ticks. The line follows the one rule of ShouldLog, by the backend's clock; a clock that fails costs this line
// only (the next occurrence is asked again), and a hook that throws does not make the occurrence logged twice. `slot` 0 is the depth,
// 1 + i the G-buffers (each with its own count and its own line).
void DepthBinder::Bump(Counter counter, size_t slot) noexcept
{
    const size_t index = static_cast<size_t>(counter);
    const uint64_t count = _counts[slot][index].fetch_add(1, std::memory_order_relaxed) + 1;
    if (slot == 0 ? _log == nullptr : _slotLog == nullptr)
        return;
    try
    {
        const int64_t now = _backend.NowMs();
        int64_t last = _lastLogged[slot][index].load(std::memory_order_relaxed);
        if (!ShouldLog(count, now, last) || !_lastLogged[slot][index].compare_exchange_strong(last, now, std::memory_order_relaxed))
            return;
        if (slot == 0)
            _log(counter, count);
        else
            _slotLog(static_cast<GBuffer>(slot - 1), counter, count);
    }
    catch (...)
    {
    }
}

// The status a refusal leaves. A runtime that is not ours (another device, not D3D11) writes none: with several effect runtimes
// the line would flip with every frame.
Outcome DepthBinder::Done(Outcome outcome)
{
    switch (outcome)
    {
    case Outcome::Disabled:
        SetStatus(StatusKind::Disabled);
        break;
    case Outcome::NoCapture:
        SetStatus(StatusKind::Waiting);
        break;
    case Outcome::CaptureUnavailable:
        SetStatus(StatusKind::Unavailable);
        break;
    case Outcome::SizeMismatch:
        SetStatus(StatusKind::SizeMismatch);
        break;
    case Outcome::ManualTransform:
        SetStatus(StatusKind::Manual);
        break;
    case Outcome::NoDepthInput:
        SetStatus(StatusKind::NoInput);
        break;
    case Outcome::Failed:
        Bump(Counter::Failed);
        SetStatus(StatusKind::Failed);
        break;
    case Outcome::Bound: // sets its own, with the sizes
    case Outcome::OtherDevice:
    case Outcome::NotD3D11:
        break;
    }
    return outcome;
}

void DepthBinder::ReleaseView(uint64_t view) noexcept
{
    try
    {
        _backend.Release(view);
    }
    catch (...)
    {
    }
}

// The newest capture, with a reference to its copy taken under the capture lock: the owner swaps the view under the same lock
// before it drops its own reference, so the view cannot die between reading it and holding it. The reference is only taken for
// a capture that can be bound at all.
DepthCapture DepthBinder::TakeCapture(uint64_t& copyView)
{
    std::lock_guard lock(_publicMutex);
    const DepthCapture capture = _capture;
    if (capture.valid && capture.copyView != 0 && capture.device != 0)
    {
        _backend.AddRef(capture.copyView);
        copyView = capture.copyView;
    }
    return capture;
}

// ---- events -------------------------------------------------------------------------------------------------------------
Outcome DepthBinder::OnBeginEffects(uint64_t runtime, uint64_t context) noexcept
{
    try
    {
        std::lock_guard lock(_mutex);
        return Begin(runtime, context);
    }
    catch (...)
    {
        Bump(Counter::Failed);
        SetStatus(StatusKind::Failed);
        return Outcome::Failed;
    }
}

void DepthBinder::OnFinishEffects(uint64_t runtime) noexcept
{
    try
    {
        std::lock_guard lock(_mutex);
        const auto it = _runtimes.find(runtime);
        if (it == _runtimes.end())
            return;
        Runtime& state = it->second;
        auto howFor = [](const Pass& pass) { return pass.inPass && !pass.orphaned ? End::Finish : End::Retry; };
        EndPass(runtime, state.pass, 0, howFor(state.pass));
        for (size_t k = 0; k < kGBuffers; ++k)
        {
            GSlot& g = state.gb[k];
            const size_t slot = 1 + k;
            // what the add-on's binding was when we put it back: the next begin compares it with what it finds (does the add-on rebind a
            // new view between frames?). Not when another add-on replaced the entry, or the restore did not go through.
            const bool verified = g.pass.open && g.pass.inPass && g.pass.verified;
            const uint64_t srv = g.pass.origSrv, srgb = g.pass.origSrgb;
            const uint64_t overwrittenBefore = _counts[slot][static_cast<size_t>(Counter::OverwrittenByOther)].load(std::memory_order_relaxed);
            EndPass(runtime, g.pass, slot, howFor(g.pass));
            if (verified && !g.pass.open &&
                _counts[slot][static_cast<size_t>(Counter::OverwrittenByOther)].load(std::memory_order_relaxed) == overwrittenBefore)
            {
                g.restoredKnown = true;
                g.restoredSrv = srv;
                g.restoredSrgb = srgb;
            }
        }
    }
    catch (...)
    {
    }
}

void DepthBinder::OnDestroyRuntime(uint64_t runtime) noexcept { Forget(runtime, false); }
void DepthBinder::OnReloadedEffects(uint64_t runtime) noexcept { Forget(runtime, true); }

void DepthBinder::OnTechniquesChanged(uint64_t runtime) noexcept
{
    try
    {
        std::lock_guard lock(_mutex);
        if (const auto it = _runtimes.find(runtime); it != _runtimes.end())
            it->second.manual.reset();
    }
    catch (...)
    {
    }
}

namespace
{
template <class State> bool AnyOpen(const State& state)
{
    if (state.pass.open)
        return true;
    for (const auto& g : state.gb)
        if (g.pass.open)
            return true;
    return false;
}
} // namespace

void DepthBinder::Forget(uint64_t runtime, bool reload) noexcept
{
    try
    {
        std::lock_guard lock(_mutex);
        const auto it = _runtimes.find(runtime);
        if (it == _runtimes.end())
            return;
        Runtime& state = it->second;
        if (!reload)
        {
            // A runtime that holds nothing is simply forgotten. One that still holds the copy (a bound pass, or a restore that is
            // pending) is kept as it is, with its references and no ReShade call: whether the ReShade runtime object is new
            // after a ResizeBuffers (its map empty) or the same one (its map still there) is not known, and both are safe this
            // way. A pointer that comes back runs the normal retry at its next event; one that never does is released by
            // Shutdown().
            if (AnyOpen(state))
            {
                if (state.pass.open)
                    state.pass.orphaned = true;
                for (GSlot& g : state.gb)
                    if (g.pass.open)
                        g.pass.orphaned = true;
            }
            else
                _runtimes.erase(it);
            return;
        }
        // The effects are new: what was looked up is stale. An open pass is restored first (this also repairs a runtime that is
        // dirty or orphaned); if that is not possible the entry stays with its references.
        EndAll(runtime, state, End::Reload);
        state.manual.reset();
        state.trust = Trust::Unknown;
        for (GSlot& g : state.gb)
        {
            g.untrusted = false;
            g.noted = false;
            g.restoredKnown = false;
            g.stableBegins = 0;
        }
        if (!AnyOpen(state))
            _runtimes.erase(it);
    }
    catch (...)
    {
    }
}

Outcome DepthBinder::Begin(uint64_t runtime, uint64_t context)
{
    // A pass an earlier frame never finished, or a restore that is still pending, comes first: before anything reads an
    // "original" and even when the feature has been switched off since.
    if (const auto it = _runtimes.find(runtime); it != _runtimes.end() && AnyOpen(it->second))
        EndAll(runtime, it->second, End::Retry);
    if (!_enabled.load(std::memory_order_relaxed))
    {
        // the option went off: the D3D side drops its remap targets (a copy that ReShade may still hold is kept alive by our references)
        if (_remapsLive.exchange(false))
        {
            try
            {
                _backend.ReleaseRemaps();
            }
            catch (...)
            {
            }
        }
        _realigned.store(0, std::memory_order_relaxed);
        return Done(Outcome::Disabled);
    }

    Runtime& state = _runtimes[runtime];
    if (state.pass.open)
        return Done(Outcome::Failed); // the copy may still be bound: nothing is bound over it

    try
    {
        BindContext bc;
        bc.context = context;
        const Outcome outcome = Bind(runtime, state, bc);
        if (outcome == Outcome::Bound)
            BindGBuffers(runtime, state, bc);
        else if (outcome != Outcome::OtherDevice && outcome != Outcome::NotD3D11)
            _realigned.store(0, std::memory_order_relaxed);
        return outcome;
    }
    catch (...)
    {
        EndPass(runtime, state.pass, 0, End::Rollback);
        _realigned.store(0, std::memory_order_relaxed);
        return Done(Outcome::Failed);
    }
}

Outcome DepthBinder::Bind(uint64_t runtime, Runtime& state, BindContext& bc)
{
    ViewRef copy { _backend };
    const DepthCapture capture = TakeCapture(copy.view);
    bc.capture = capture;

    // ---- is this runtime ours. Also before the first capture: device 0 then means the game's primary device, so a runtime of
    // another device never counts as a consumer.
    if (!_backend.IsD3D11(runtime))
    {
        Bump(Counter::OtherDevice);
        return Done(Outcome::NotD3D11);
    }
    if (!_backend.SameDevice(runtime, capture.device))
    {
        Bump(Counter::OtherDevice);
        return Done(Outcome::OtherDevice);
    }

    // ---- is there a depth input to bind (independent of the capture: a begin that finds one is what starts the capture)
    if (!state.manual)
    {
        bool manual = false;
        if (!ManualTransform(runtime, manual))
            return Done(Outcome::Failed);
        state.manual = manual;
    }
    if (*state.manual)
    {
        Bump(Counter::ManualTransformStandDowns);
        return Done(Outcome::ManualTransform);
    }
    auto notTrusted = [&]
    {
        state.trust = Trust::Untrusted;
        Bump(Counter::NoDepthInput);
        SetStatus(StatusKind::NotTrusted);
        return Outcome::NoDepthInput;
    };
    if (state.trust == Trust::Untrusted)
    {
        Bump(Counter::NoDepthInput);
        SetStatus(StatusKind::NotTrusted);
        return Outcome::NoDepthInput;
    }
    std::vector<TextureVar>& textures = bc.textures;
    if (!_backend.TextureVariables(runtime, textures))
        return Done(Outcome::Failed);
    const std::vector<const TextureVar*> candidates = FindInputs(textures, 0);
    if (candidates.empty())
    {
        if (!state.noCandidateNoted)
        {
            state.noCandidateNoted = true;
            try
            {
                _backend.NoDepthCandidate(runtime, textures);
            }
            catch (...)
            {
            }
        }
        Bump(Counter::NoDepthInput);
        return Done(Outcome::NoDepthInput);
    }
    if (!state.foundNoted)
    {
        state.foundNoted = true;
        try
        {
            std::vector<std::string> names;
            names.reserve(candidates.size());
            for (const TextureVar* candidate : candidates)
                names.push_back(candidate->name);
            _backend.DepthInputsFound(runtime, names);
        }
        catch (...)
        {
        }
    }
    if (state.trust == Trust::Unknown)
    {
        // The DEPTH semantic cannot be read through the API. A variable with the right name but without it shows some other
        // binding: trust the best candidate only if it shows nothing or a depth view. (The bind below checks it for real.)
        uint64_t srv = 0, srgb = 0;
        if (!_backend.TextureBinding(runtime, candidates.front()->handle, srv, srgb))
            return Done(Outcome::Failed);
        auto depthOrEmpty = [&](uint64_t view) { return view == 0 || _backend.IsDepthView(view); };
        if (!depthOrEmpty(srv) || !depthOrEmpty(srgb))
            return notTrusted();
        state.trust = Trust::Trusted;
    }

    // ---- the capture side is told about the consumer and the size of its back buffer before anything depends on a capture.
    // Clock and frame are read after the capture was taken, so it is never "from the future".
    uint32_t width = 0, height = 0;
    if (!_backend.BackBufferSize(runtime, width, height))
        return Done(Outcome::Failed);
    bc.bbW = width;
    bc.bbH = height;
    const int64_t now = _backend.NowMs();
    RecordConsumer(now, width, height);
    if (!capture.valid)
        return Done(Outcome::NoCapture);
    const uint64_t frame = _backend.Frame();
    if (copy.view == 0 || !IsFresh(capture, now, frame))
    {
        Bump(Counter::StaleCaptures);
        return Done(Outcome::CaptureUnavailable);
    }
    if (width != capture.outputW || height != capture.outputH)
    {
        Bump(Counter::SizeMismatches);
        return Done(Outcome::SizeMismatch);
    }

    // ---- what every candidate shows now. One that already shows our copy is a bind that was never put back, not an original:
    // saved and restored later it would leave the copy bound for good.
    struct Reading
    {
        uint64_t handle, srv, srgb;
    };
    std::vector<Reading> before;
    before.reserve(candidates.size());
    for (const TextureVar* c : candidates)
    {
        Reading r { c->handle, 0, 0 };
        if (!_backend.TextureBinding(runtime, r.handle, r.srv, r.srgb))
            return Done(Outcome::Failed);
        if (r.srv == copy.view || r.srgb == copy.view)
            return Done(Outcome::Failed);
        before.push_back(r);
    }

    // ---- bind. From here `state` owns every reference taken and every way out goes through EndPass. Until a variable has read
    // the copy back nothing is known about what the entry held, so an undo is an erase (the original stays empty, unverified).
    Pass& pass = state.pass;
    pass.open = true;
    pass.inPass = true;
    pass.copy = pass.copySrgb = copy.Take();
    pass.verified = false;
    pass.variable = pass.origSrv = pass.origSrgb = 0;
    auto fail = [&]
    {
        EndPass(runtime, pass, 0, End::Rollback);
        return Done(Outcome::Failed);
    };

    pass.bound = true; // set before the call: if it throws after changing the binding, EndPass undoes it
    if (!_backend.UpdateTextureBindings(runtime, kDepthSemantic, pass.copy, pass.copy))
    {
        pass.bound = false; // refused: nothing changed
        return fail();
    }

    // The first candidate that reads exactly the copy back is the DEPTH reader of this pass, and what it showed before the bind
    // is the original. A failed read is a failed pass, not a verdict about the variables.
    const Reading* reader = nullptr;
    for (const Reading& r : before)
    {
        uint64_t srv = 0, srgb = 0;
        if (!_backend.TextureBinding(runtime, r.handle, srv, srgb))
            return fail();
        if (srv == pass.copy && srgb == pass.copy)
        {
            reader = &r;
            break;
        }
    }
    if (reader == nullptr)
    {
        // None of the named variables reads the DEPTH semantic. The bind is undone with an erase (never with what a decoy
        // showed, which ReShade may free), and the runtime is not trusted until its effects reload.
        notTrusted();
        EndPass(runtime, pass, 0, End::Rollback);
        return Outcome::NoDepthInput;
    }
    pass.verified = true;
    pass.variable = reader->handle;
    pass.origSrv = reader->srv;
    pass.origSrgb = reader->srgb;
    if (pass.origSrv != 0)
    {
        _backend.AddRef(pass.origSrv);
        pass.refSrv = true;
    }
    if (pass.origSrgb != 0)
    {
        _backend.AddRef(pass.origSrgb);
        pass.refSrgb = true;
    }

    // Effects that gate on ReShade having found a depth buffer read this uniform; it is forced on for the pass.
    std::vector<UniformVar> uniforms;
    if (!_backend.UniformVariables(runtime, uniforms))
        return fail();
    for (const UniformVar& u : uniforms)
    {
        if (u.source != kBufReadySource)
            continue;
        bool old = false;
        if (!_backend.GetUniformBool(runtime, u.handle, old))
            return fail();
        pass.bools.push_back({ u.handle, old }); // recorded first: a set that threw may still have changed the value
        if (!_backend.SetUniformBool(runtime, u.handle, true))
            return fail();
    }

    Bump(Counter::PassesBound);
    SetStatus(StatusKind::Bound, &capture);
    return Outcome::Bound;
}

// ---- the G-buffer slots -------------------------------------------------------------------------------------------------------
void DepthBinder::BindGBuffers(uint64_t runtime, Runtime& state, const BindContext& bc) noexcept
{
    for (size_t k = 0; k < kGBuffers; ++k)
    {
        try
        {
            BindGBuffer(runtime, state, k, bc);
        }
        catch (...)
        {
            // a slot that throws never turns the depth into Failed: it rolls back on its own and counts on its own
            EndPass(runtime, state.gb[k].pass, 1 + k, End::Rollback);
            SetRealigned(k, false);
            Bump(Counter::Failed, 1 + k);
        }
    }
}

void DepthBinder::BindGBuffer(uint64_t runtime, Runtime& state, size_t k, const BindContext& bc)
{
    GSlot& g = state.gb[k];
    const size_t slot = 1 + k;
    const GBuffer buffer = static_cast<GBuffer>(k);
    if (g.pass.open) // a restore of an earlier pass is still pending (and was retried first): nothing is bound over it
    {
        SetRealigned(k, false);
        Bump(Counter::Failed, slot);
        return;
    }
    const std::vector<const TextureVar*> candidates = FindInputs(bc.textures, slot);
    if (candidates.empty()) // the effects do not declare it: nothing to do, no backend call, no counter
    {
        SetRealigned(k, false);
        return;
    }
    if (g.untrusted)
    {
        SetRealigned(k, false);
        Bump(Counter::NoDepthInput, slot);
        return;
    }

    // ---- what the add-on has bound: every candidate's binding. The semantic is one entry per runtime, so all readers show the same
    // pair; variables of that name that do not read the semantic (an internal texture of an effect) show something else. Different
    // pairs are ambiguous and left alone; the bind below checks the readers for real.
    struct Reading
    {
        uint64_t handle, srv, srgb;
    };
    std::vector<Reading> before;
    before.reserve(candidates.size());
    Reading source { 0, 0, 0 };
    bool haveSource = false, ambiguous = false;
    for (const TextureVar* c : candidates)
    {
        Reading r { c->handle, 0, 0 };
        if (!_backend.TextureBinding(runtime, r.handle, r.srv, r.srgb))
        {
            SetRealigned(k, false);
            Bump(Counter::Failed, slot);
            return;
        }
        before.push_back(r);
        if (r.srv == 0 && r.srgb == 0)
            continue;
        if (!haveSource)
        {
            source = r;
            haveSource = true;
        }
        else if (r.srv != source.srv || r.srgb != source.srgb)
            ambiguous = true;
    }

    // does the add-on rebind a new view between frames? (the log says it once: the order of the callbacks depends on it)
    if (haveSource && g.restoredKnown)
    {
        const bool same = source.srv == g.restoredSrv && source.srgb == g.restoredSrgb;
        try
        {
            if (!same)
            {
                g.stableBegins = 0;
                if (!g.changedTold)
                {
                    g.changedTold = true;
                    _backend.GBufferObserved(buffer, GBufferEvent::ViewChanged);
                }
            }
            else if (++g.stableBegins >= kStableLogAfter && !g.stableTold)
            {
                g.stableTold = true;
                _backend.GBufferObserved(buffer, GBufferEvent::ViewStable);
            }
        }
        catch (...)
        {
        }
    }

    // ---- the decision (the log says every change of it once)
    GBufferNote note;
    note.buffer = buffer;
    note.renderW = bc.capture.renderW;
    note.renderH = bc.capture.renderH;
    note.outputW = bc.capture.outputW;
    note.outputH = bc.capture.outputH;
    auto tell = [&]
    {
        auto same = [](const GBufferNote& a, const GBufferNote& b)
        {
            return a.decision == b.decision && a.status == b.status && a.source.ok == b.source.ok && a.source.plain2D == b.source.plain2D &&
                   a.source.width == b.source.width && a.source.height == b.source.height && a.source.format == b.source.format &&
                   a.renderW == b.renderW && a.renderH == b.renderH && a.outputW == b.outputW && a.outputH == b.outputH;
        };
        if (g.noted && same(g.lastNote, note))
            return;
        g.noted = true;
        g.lastNote = note;
        try
        {
            _backend.GBufferDecided(note);
        }
        catch (...)
        {
        }
    };
    if (!haveSource || source.srv == 0)
        note.decision = RemapDecision::KeepNoSource;
    else if (ambiguous)
        note.decision = RemapDecision::KeepAmbiguous;
    else
    {
        ViewDesc desc;
        if (!_backend.DescribeView(source.srv, desc))
            desc = ViewDesc {};
        note.source = desc;
        note.decision = DecideRemap(desc, bc.bbW, bc.bbH, note.renderW, note.renderH, note.outputW, note.outputH);
    }
    if (note.decision != RemapDecision::Remap)
    {
        tell();
        SetRealigned(k, false);
        Bump(Counter::Kept, slot);
        return;
    }

    // ---- the copy, made now
    RemapRequest request;
    request.context = bc.context;
    request.buffer = buffer;
    request.srv = source.srv;
    request.srvSrgb = source.srgb;
    request.renderW = note.renderW;
    request.renderH = note.renderH;
    request.outputW = note.outputW;
    request.outputH = note.outputH;
    request.stamp = bc.capture.frame;
    RemapOut out;
    const RemapStatus status = _backend.Remap(request, out);
    note.status = status;
    tell();
    if (status != RemapStatus::Ok || out.srv == 0)
    {
        SetRealigned(k, false);
        Bump(status == RemapStatus::Failed || status == RemapStatus::Ok ? Counter::Failed : Counter::Kept, slot);
        return;
    }
    _remapsLive.store(true, std::memory_order_relaxed);
    const uint64_t copyView = out.srv;
    const uint64_t copySrgbView = out.srvSrgb != 0 ? out.srvSrgb : out.srv;
    for (const Reading& r : before)
        if (r.srv == copyView || r.srgb == copySrgbView) // a bind that was never put back is not an original
        {
            SetRealigned(k, false);
            Bump(Counter::Failed, slot);
            return;
        }

    // The pass holds its own references to the views of the copy: the engine drops its own at its next remap or release.
    ViewRef refCopy { _backend };
    _backend.AddRef(copyView);
    refCopy.view = copyView;
    ViewRef refSrgb { _backend };
    if (copySrgbView != copyView)
    {
        _backend.AddRef(copySrgbView);
        refSrgb.view = copySrgbView;
    }

    // ---- bind. From here the pass owns every reference taken and every way out goes through EndPass.
    Pass& pass = g.pass;
    pass.open = true;
    pass.inPass = true;
    pass.copy = refCopy.Take();
    pass.copySrgb = copySrgbView == copyView ? pass.copy : refSrgb.Take();
    pass.verified = false;
    pass.variable = pass.origSrv = pass.origSrgb = 0;
    pass.bools.clear();
    auto fail = [&]
    {
        EndPass(runtime, pass, slot, End::Rollback);
        SetRealigned(k, false);
        Bump(Counter::Failed, slot);
    };
    pass.bound = true; // set before the call: if it throws after changing the binding, EndPass undoes it
    if (!_backend.UpdateTextureBindings(runtime, GBufferSemantic(buffer), pass.copy, pass.copySrgb))
    {
        pass.bound = false; // refused: nothing changed
        fail();
        return;
    }
    const Reading* reader = nullptr;
    for (const Reading& r : before)
    {
        uint64_t srv = 0, srgb = 0;
        if (!_backend.TextureBinding(runtime, r.handle, srv, srgb))
        {
            fail();
            return;
        }
        if (srv == pass.copy && srgb == pass.copySrgb)
        {
            reader = &r;
            break;
        }
    }
    if (reader == nullptr)
    {
        // None of the variables of that name reads the semantic: they are not its readers, and what they showed (an effect's own
        // texture) is never written into the entry. The bind is undone with an erase and the slot is not used again until the effects
        // reload.
        g.untrusted = true;
        Bump(Counter::NoDepthInput, slot);
        EndPass(runtime, pass, slot, End::Rollback);
        SetRealigned(k, false);
        return;
    }
    pass.verified = true;
    pass.variable = reader->handle;
    pass.origSrv = reader->srv;
    pass.origSrgb = reader->srgb;
    if (pass.origSrv != 0)
    {
        _backend.AddRef(pass.origSrv);
        pass.refSrv = true;
    }
    if (pass.origSrgb != 0)
    {
        _backend.AddRef(pass.origSrgb);
        pass.refSrgb = true;
    }
    Bump(Counter::PassesBound, slot);
    SetRealigned(k, true);
}

// ---- ending a pass ------------------------------------------------------------------------------------------------------
// Ends the open pass (if any). The references of the pass are given back only when ReShade holds nothing of ours;
// if the restore fails or cannot be checked the runtime stays dirty (open, no longer in a pass) and keeps them. Every backend
// call is guarded on its own so one failure cannot keep the rest from running.
void DepthBinder::EndPass(uint64_t runtime, Pass& pass, size_t slot, End how) noexcept
{
    if (!pass.open)
        return;
    // An orphan's uniform handles belong to a runtime that is gone (ReShade frees them with the effects): they are not used again, not
    // even to put bufready_depth back. It stays as the pass left it until the effects are loaded again.
    if (slot == 0 && pass.inPass && how != End::Reload && !pass.orphaned)
        RestoreBools(runtime, pass, how);
    if (pass.bound && !RestoreBinding(runtime, pass, slot, how))
    {
        pass.inPass = false;
        Bump(Counter::Failed, slot);
        if (slot == 0)
            SetStatus(StatusKind::Failed);
        else
            SetRealigned(slot - 1, false);
        return;
    }
    ReleasePass(pass);
}

void DepthBinder::EndAll(uint64_t runtime, Runtime& state, End how) noexcept
{
    EndPass(runtime, state.pass, 0, how);
    for (size_t k = 0; k < kGBuffers; ++k)
        EndPass(runtime, state.gb[k].pass, 1 + k, how);
}

void DepthBinder::ReleasePass(Pass& pass) noexcept
{
    ReleaseView(pass.copy);
    if (pass.copySrgb != 0 && pass.copySrgb != pass.copy)
        ReleaseView(pass.copySrgb);
    if (pass.refSrv)
        ReleaseView(pass.origSrv);
    if (pass.refSrgb)
        ReleaseView(pass.origSrgb);
    pass.open = pass.inPass = pass.bound = pass.orphaned = false;
    pass.copy = pass.copySrgb = pass.variable = pass.origSrv = pass.origSrgb = 0;
    pass.refSrv = pass.refSrgb = false;
    pass.bools.clear();
}

// Puts the bufready_depth values back. The handles of the pass are valid at finish; a pass that is ended later (Retry) matches
// the uniforms as they are now, by position among the bufready_depth ones, and leaves them alone if their number changed.
void DepthBinder::RestoreBools(uint64_t runtime, Pass& pass, End how) noexcept
{
    auto put = [&](uint64_t variable, bool value)
    {
        try
        {
            if (!_backend.SetUniformBool(runtime, variable, value))
                SetStatus(StatusKind::Failed);
        }
        catch (...)
        {
            SetStatus(StatusKind::Failed);
        }
    };
    if (how != End::Retry)
    {
        for (const SavedBool& saved : pass.bools)
            put(saved.variable, saved.value);
        return;
    }
    try
    {
        std::vector<UniformVar> uniforms;
        if (!_backend.UniformVariables(runtime, uniforms))
        {
            SetStatus(StatusKind::Failed);
            return;
        }
        std::vector<uint64_t> handles;
        for (const UniformVar& u : uniforms)
            if (u.source == kBufReadySource)
                handles.push_back(u.handle);
        if (handles.size() != pass.bools.size())
            return;
        for (size_t i = 0; i < handles.size(); ++i)
            put(handles[i], pass.bools[i].value);
    }
    catch (...)
    {
        SetStatus(StatusKind::Failed);
    }
}

// Takes the copy out of the semantic's entry. True = ReShade holds nothing of ours any more; false = that could not be checked or
// done (the pass stays dirty). Only a variable that was seen reading the copy back can vouch for what is in the entry:
//  - a pass without such a variable (unverified) erases the entry;
//  - at finish and rollback the variable of the pass is read: if the copy is in it (a half of the pair counts) the original that
//    the variable showed before the bind goes back, otherwise another add-on replaced the entry and its choice stays;
//  - at retry and reload the handle is stale, so the named variables are looked up again: one that shows the copy vouches for
//    the entry and the original goes back; if none does, or there is no variable of those names at all, the entry is erased (a
//    variable looked up now proves nothing, and an erase needs no variable).
// Nothing but the original of a verified pass and "empty" is ever written, since ReShade frees views it knows nothing about.
//
// Every erase that no readback of this pass vouched for is counted (erasedUnverified), and only once the erase went through.
// It can lose the pick of another add-on (Generic Depth, or REST for a G-buffer) until that add-on selects again. A DELIBERATE
// TRADE-OFF: the alternative, leaving the entry alone when a same-named variable no longer shows the copy, would trust a variable that
// may be a decoy without the semantic, and then the copy stays in a map that ReShade frees views of. For a G-buffer slot an
// unverified pass means that no variable of that name reads the semantic (so the erase takes nothing from them), at most from readers
// that go by another name; the slot is then not used again until the effects reload, so it happens once per reload. Whether REST
// rebinds every frame (which repairs it) is not known offline: the counter and the log say.
bool DepthBinder::RestoreBinding(uint64_t runtime, Pass& pass, size_t slot, End how) noexcept
{
    try
    {
        const char* const semantic = SlotSemantic(slot);
        auto holdsCopy = [&](uint64_t srv, uint64_t srgb) { return srv == pass.copy || srgb == pass.copySrgb; };
        auto eraseUnverified = [&]
        {
            if (!_backend.UpdateTextureBindings(runtime, semantic, 0, 0))
                return false;
            Bump(Counter::ErasedUnverified, slot);
            return true;
        };
        if (!pass.verified)
            return eraseUnverified();

        uint64_t srv = 0, srgb = 0;
        if (how == End::Finish || how == End::Rollback)
        {
            if (!_backend.TextureBinding(runtime, pass.variable, srv, srgb))
                return false;
            if (!holdsCopy(srv, srgb))
            {
                if (how == End::Finish)
                    Bump(Counter::OverwrittenByOther, slot);
                return true;
            }
            return _backend.UpdateTextureBindings(runtime, semantic, pass.origSrv, pass.origSrgb);
        }

        std::vector<TextureVar> textures;
        if (!_backend.TextureVariables(runtime, textures))
            return false;
        const std::vector<const TextureVar*> candidates = FindInputs(textures, slot);
        for (const TextureVar* c : candidates)
        {
            if (!_backend.TextureBinding(runtime, c->handle, srv, srgb))
                return false;
            if (holdsCopy(srv, srgb))
                return _backend.UpdateTextureBindings(runtime, semantic, pass.origSrv, pass.origSrgb);
        }
        return eraseUnverified();
    }
    catch (...)
    {
        return false;
    }
}

// ---- manual depth transform ---------------------------------------------------------------------------------------------
// True if any macro of the scope is off its neutral value. `error` is set when a query failed.
bool DepthBinder::AnyNonNeutral(uint64_t runtime, const char* effect, bool& error)
{
    std::string value;
    for (const TransformMacro& macro : kTransformMacros)
    {
        switch (_backend.Definition(runtime, effect, macro.name, value))
        {
        case DefinitionState::Undefined:
            break;
        case DefinitionState::Defined:
            if (!IsNeutralDefinition(value, macro.neutral))
                return true;
            break;
        case DefinitionState::Error:
            error = true;
            return false;
        }
    }
    return false;
}

// False = a query failed (nothing is remembered). `manual` is true if any macro is off its neutral value, globally or for an
// effect that has an enabled technique.
bool DepthBinder::ManualTransform(uint64_t runtime, bool& manual)
{
    bool error = false;
    manual = AnyNonNeutral(runtime, nullptr, error);
    if (error)
        return false;
    if (manual)
        return true;
    std::vector<EffectTechnique> techniques;
    if (!_backend.Techniques(runtime, techniques))
        return false;
    std::vector<std::string> checked;
    for (const EffectTechnique& t : techniques)
    {
        if (!t.enabled || std::find(checked.begin(), checked.end(), t.effect) != checked.end())
            continue;
        checked.push_back(t.effect);
        manual = AnyNonNeutral(runtime, t.effect.c_str(), error);
        if (error)
            return false;
        if (manual)
            return true;
    }
    return true;
}
} // namespace ext::reshade::depth
