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
// DepthInputTex.
constexpr const char* kDepthInputNames[] = { "ReShade::DepthBufferTex", "DepthBufferTex", "DepthInputTex" };

const TextureVar* FindDepthInput(const std::vector<TextureVar>& textures)
{
    for (const char* name : kDepthInputNames)
        for (const TextureVar& t : textures)
            if (t.name == name)
                return &t;
    return nullptr;
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

bool ShouldLog(uint64_t count) { return count == 1 || (count != 0 && count % 1000 == 0); }

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
    case Counter::Count:
        break;
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
DepthBinder::DepthBinder(IBackend& backend, LogFn log) : _backend(backend), _log(log) {}

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

bool DepthBinder::ConsumerSeen(int64_t nowMs) const noexcept
{
    const int64_t last = _consumerMs.load(std::memory_order_relaxed);
    return last != INT64_MIN && nowMs - last <= kConsumerWindowMs;
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

Counters DepthBinder::GetCounters() const noexcept
{
    auto n = [&](Counter c) { return _counts[static_cast<size_t>(c)].load(std::memory_order_relaxed); };
    Counters out;
    out.overwrittenByOther = n(Counter::OverwrittenByOther);
    out.staleCaptures = n(Counter::StaleCaptures);
    out.sizeMismatches = n(Counter::SizeMismatches);
    out.manualTransformStandDowns = n(Counter::ManualTransformStandDowns);
    out.noDepthInput = n(Counter::NoDepthInput);
    out.passesBound = n(Counter::PassesBound);
    out.otherDevice = n(Counter::OtherDevice);
    return out;
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

void DepthBinder::Bump(Counter counter) noexcept
{
    const uint64_t count = _counts[static_cast<size_t>(counter)].fetch_add(1, std::memory_order_relaxed) + 1;
    if (_log == nullptr || !ShouldLog(count))
        return;
    try
    {
        _log(counter, count);
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
Outcome DepthBinder::OnBeginEffects(uint64_t runtime) noexcept
{
    try
    {
        std::lock_guard lock(_mutex);
        return Begin(runtime);
    }
    catch (...)
    {
        SetStatus(StatusKind::Failed);
        return Outcome::Failed;
    }
}

void DepthBinder::OnFinishEffects(uint64_t runtime) noexcept
{
    try
    {
        std::lock_guard lock(_mutex);
        if (const auto it = _runtimes.find(runtime); it != _runtimes.end())
            EndPass(runtime, it->second, it->second.inPass ? End::Finish : End::Retry);
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
            EndPass(runtime, state, End::Forget);
            _runtimes.erase(it);
            return;
        }
        // The effects are new: what was looked up is stale. An open pass is restored first (this also repairs a runtime that
        // is dirty); if that is not possible the entry stays with its references until the runtime is destroyed.
        EndPass(runtime, state, End::Reload);
        state.manual.reset();
        state.trust = Trust::Unknown;
        if (!state.open)
            _runtimes.erase(it);
    }
    catch (...)
    {
    }
}

Outcome DepthBinder::Begin(uint64_t runtime)
{
    // A pass an earlier frame never finished, or a restore that is still pending, comes first: before anything reads an
    // "original" and even when the feature has been switched off since.
    if (const auto it = _runtimes.find(runtime); it != _runtimes.end() && it->second.open)
        EndPass(runtime, it->second, End::Retry);
    if (!_enabled.load(std::memory_order_relaxed))
        return Done(Outcome::Disabled);

    Runtime& state = _runtimes[runtime];
    if (state.open)
        return Done(Outcome::Failed); // the copy may still be bound: nothing is bound over it

    try
    {
        return Bind(runtime, state);
    }
    catch (...)
    {
        EndPass(runtime, state, End::Rollback);
        return Done(Outcome::Failed);
    }
}

Outcome DepthBinder::Bind(uint64_t runtime, Runtime& state)
{
    ViewRef copy { _backend };
    const DepthCapture capture = TakeCapture(copy.view);

    // ---- is this runtime ours
    if (!_backend.IsD3D11(runtime))
    {
        Bump(Counter::OtherDevice);
        return Done(Outcome::NotD3D11);
    }
    if (capture.device != 0 && !_backend.SameDevice(runtime, capture.device))
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
    if (state.trust == Trust::Untrusted)
    {
        Bump(Counter::NoDepthInput);
        return Done(Outcome::NoDepthInput);
    }
    std::vector<TextureVar> textures;
    if (!_backend.TextureVariables(runtime, textures))
        return Done(Outcome::Failed);
    const TextureVar* input = FindDepthInput(textures);
    if (input == nullptr)
    {
        Bump(Counter::NoDepthInput);
        return Done(Outcome::NoDepthInput);
    }
    uint64_t srv = 0, srgb = 0;
    if (!_backend.TextureBinding(runtime, input->handle, srv, srgb))
        return Done(Outcome::Failed);
    if (state.trust == Trust::Unknown)
    {
        // The DEPTH semantic cannot be read through the API. A variable with the right name but without it would show some
        // other binding, which is not an original to put back: trust the variable only if it shows nothing or a depth view.
        auto depthOrEmpty = [&](uint64_t view) { return view == 0 || _backend.IsDepthView(view); };
        if (!depthOrEmpty(srv) || !depthOrEmpty(srgb))
        {
            state.trust = Trust::Untrusted;
            Bump(Counter::NoDepthInput);
            return Done(Outcome::NoDepthInput);
        }
        state.trust = Trust::Trusted;
    }

    // ---- the capture. Clock and frame are read after the capture was taken, so it is never "from the future".
    const int64_t now = _backend.NowMs();
    _consumerMs.store(now, std::memory_order_relaxed);
    if (!capture.valid)
        return Done(Outcome::NoCapture);
    const uint64_t frame = _backend.Frame();
    if (copy.view == 0 || !IsFresh(capture, now, frame))
    {
        Bump(Counter::StaleCaptures);
        return Done(Outcome::CaptureUnavailable);
    }
    uint32_t width = 0, height = 0;
    if (!_backend.BackBufferSize(runtime, width, height) || width != capture.outputW || height != capture.outputH)
    {
        Bump(Counter::SizeMismatches);
        return Done(Outcome::SizeMismatch);
    }
    // A binding that already holds our copy is a bind that was never put back, not an original: saved and restored later it
    // would leave the copy bound for good.
    if (srv == copy.view || srgb == copy.view)
        return Done(Outcome::Failed);

    // ---- bind. From here `state` owns every reference taken and every way out goes through EndPass.
    state.open = true;
    state.inPass = true;
    state.copy = copy.Take();
    state.variable = input->handle;
    state.origSrv = srv;
    state.origSrgb = srgb;
    if (srv != 0)
    {
        _backend.AddRef(srv);
        state.refSrv = true;
    }
    if (srgb != 0)
    {
        _backend.AddRef(srgb);
        state.refSrgb = true;
    }
    auto fail = [&]
    {
        EndPass(runtime, state, End::Rollback);
        return Done(Outcome::Failed);
    };

    state.bound = true; // set before the call: if it throws after changing the binding, EndPass finds out whether it is ours
    if (!_backend.UpdateTextureBindings(runtime, kDepthSemantic, state.copy, state.copy))
    {
        state.bound = false; // refused: nothing changed
        return fail();
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
        state.bools.push_back({ u.handle, old }); // recorded first: a set that threw may still have changed the value
        if (!_backend.SetUniformBool(runtime, u.handle, true))
            return fail();
    }

    Bump(Counter::PassesBound);
    SetStatus(StatusKind::Bound, &capture);
    return Outcome::Bound;
}

// ---- ending a pass ------------------------------------------------------------------------------------------------------
// Ends the open pass of `state` (if any). The references of the pass are given back only when ReShade holds nothing of ours;
// if the restore fails or cannot be checked the runtime stays dirty (open, no longer in a pass) and keeps them. Every backend
// call is guarded on its own so one failure cannot keep the rest from running.
void DepthBinder::EndPass(uint64_t runtime, Runtime& state, End how) noexcept
{
    if (!state.open)
        return;
    if (how != End::Forget)
    {
        if (state.inPass && how != End::Reload)
            RestoreBools(runtime, state, how);
        if (state.bound && !RestoreBinding(runtime, state, how))
        {
            state.inPass = false;
            SetStatus(StatusKind::Failed);
            return;
        }
    }
    ReleasePass(state);
}

void DepthBinder::ReleasePass(Runtime& state) noexcept
{
    ReleaseView(state.copy);
    if (state.refSrv)
        ReleaseView(state.origSrv);
    if (state.refSrgb)
        ReleaseView(state.origSrgb);
    state.open = state.inPass = state.bound = false;
    state.copy = state.variable = state.origSrv = state.origSrgb = 0;
    state.refSrv = state.refSrgb = false;
    state.bools.clear();
}

// Puts the bufready_depth values back. The handles of the pass are valid at finish; a pass that is ended later (Retry) matches
// the uniforms as they are now, by position among the bufready_depth ones, and leaves them alone if their number changed.
void DepthBinder::RestoreBools(uint64_t runtime, Runtime& state, End how) noexcept
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
        for (const SavedBool& saved : state.bools)
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
        if (handles.size() != state.bools.size())
            return;
        for (size_t i = 0; i < handles.size(); ++i)
            put(handles[i], state.bools[i].value);
    }
    catch (...)
    {
        SetStatus(StatusKind::Failed);
    }
}

// Puts the original binding back, but only if the DEPTH semantic still points at our copy. Another add-on (Generic Depth
// picking a new buffer) may have replaced it, and its choice wins. True = ReShade holds nothing of ours any more; false = the
// binding could not be checked or restored.
bool DepthBinder::RestoreBinding(uint64_t runtime, Runtime& state, End how) noexcept
{
    try
    {
        uint64_t variable = state.variable;
        if (how == End::Retry || how == End::Reload)
        {
            // the handle from begin may be stale: find the variable again, by name
            std::vector<TextureVar> textures;
            if (!_backend.TextureVariables(runtime, textures))
                return false;
            const TextureVar* input = FindDepthInput(textures);
            if (input == nullptr)
                return false;
            variable = input->handle;
        }
        uint64_t srv = 0, srgb = 0;
        if (!_backend.TextureBinding(runtime, variable, srv, srgb))
            return false;
        if (srv != state.copy || srgb != state.copy)
        {
            if (how == End::Finish)
                Bump(Counter::OverwrittenByOther);
            return true;
        }
        return _backend.UpdateTextureBindings(runtime, kDepthSemantic, state.origSrv, state.origSrgb);
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
