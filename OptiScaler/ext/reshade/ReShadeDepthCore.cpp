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
const char* const kStatusDisabled = "Automatic ReShade depth disabled";
const char* const kStatusWaiting = "Waiting for ReShade depth effects";
const char* const kStatusOtherDevice = "ReShade runs on another device; generic depth retained";
const char* const kStatusNotD3D11 = "ReShade is not using D3D11; generic depth retained";
const char* const kStatusUnavailable = "Scene depth unavailable; generic depth retained";
const char* const kStatusSizeMismatch = "ReShade size differs from the scene depth; generic depth retained";
const char* const kStatusManual = "Manual ReShade depth transform retained";
const char* const kStatusNoInput = "No depth input found; generic depth retained";
const char* const kStatusFailed = "ReShade call failed; generic depth retained";

const char* OutcomeText(Outcome outcome)
{
    switch (outcome)
    {
    case Outcome::Disabled:
        return kStatusDisabled;
    case Outcome::OtherDevice:
        return kStatusOtherDevice;
    case Outcome::NotD3D11:
        return kStatusNotD3D11;
    case Outcome::CaptureUnavailable:
        return kStatusUnavailable;
    case Outcome::SizeMismatch:
        return kStatusSizeMismatch;
    case Outcome::ManualTransform:
        return kStatusManual;
    case Outcome::NoDepthInput:
        return kStatusNoInput;
    case Outcome::Failed:
        return kStatusFailed;
    case Outcome::Bound:
    case Outcome::Duplicate:
        break; // not a refusal: the status is left alone (Bound sets its own)
    }
    return "";
}

std::string BoundText(const DepthCapture& capture)
{
    if (capture.renderW == capture.outputW && capture.renderH == capture.outputH)
        return std::format("Automatic scene depth: {}x{}", capture.outputW, capture.outputH);
    return std::format("Automatic scene depth: {}x{} -> {}x{}", capture.renderW, capture.renderH, capture.outputW, capture.outputH);
}

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
    case Counter::Count:
        break;
    }
    return "?";
}

bool LateRegistrationPlanner::ShouldRetry(bool enabled, bool attempted, RegisterStatus status, int64_t nowMs, int64_t lastAttemptMs)
{
    if (!enabled || !attempted)
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

// ---- the binder ---------------------------------------------------------------------------------------------------------
DepthBinder::DepthBinder(IBackend& backend, LogFn log) : _backend(backend), _log(log), _status(kStatusDisabled) {}

void DepthBinder::SetEnabled(bool enabled)
{
    try
    {
        if (_enabled.exchange(enabled) != enabled)
            SetStatus(enabled ? kStatusWaiting : kStatusDisabled);
    }
    catch (...)
    {
    }
}

void DepthBinder::PublishCapture(const DepthCapture& capture)
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

std::string DepthBinder::Status() const
{
    try
    {
        std::lock_guard lock(_publicMutex);
        return _status;
    }
    catch (...)
    {
        return {};
    }
}

Counters DepthBinder::GetCounters() const
{
    auto n = [&](Counter c) { return _counts[static_cast<size_t>(c)].load(std::memory_order_relaxed); };
    Counters out;
    out.overwrittenByOther = n(Counter::OverwrittenByOther);
    out.staleCaptures = n(Counter::StaleCaptures);
    out.sizeMismatches = n(Counter::SizeMismatches);
    out.manualTransformStandDowns = n(Counter::ManualTransformStandDowns);
    out.noDepthInput = n(Counter::NoDepthInput);
    out.passesBound = n(Counter::PassesBound);
    return out;
}

void DepthBinder::SetStatus(std::string text)
{
    try
    {
        std::lock_guard lock(_publicMutex);
        _status = std::move(text);
    }
    catch (...)
    {
    }
}

void DepthBinder::Bump(Counter counter)
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

Outcome DepthBinder::Done(Outcome outcome)
{
    SetStatus(OutcomeText(outcome));
    return outcome;
}

void DepthBinder::ReleaseView(uint64_t view)
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
Outcome DepthBinder::OnBeginEffects(uint64_t runtime)
{
    try
    {
        std::lock_guard lock(_mutex);
        return Begin(runtime);
    }
    catch (...)
    {
        SetStatus(kStatusFailed);
        return Outcome::Failed;
    }
}

void DepthBinder::OnFinishEffects(uint64_t runtime)
{
    try
    {
        std::lock_guard lock(_mutex);
        if (const auto it = _runtimes.find(runtime); it != _runtimes.end())
            EndPass(runtime, it->second, End::Finish);
    }
    catch (...)
    {
    }
}

void DepthBinder::OnDestroyRuntime(uint64_t runtime) { Forget(runtime); }
void DepthBinder::OnReloadedEffects(uint64_t runtime) { Forget(runtime); }

void DepthBinder::Forget(uint64_t runtime)
{
    try
    {
        std::lock_guard lock(_mutex);
        if (const auto it = _runtimes.find(runtime); it != _runtimes.end())
        {
            EndPass(runtime, it->second, End::Forget);
            _runtimes.erase(it);
        }
    }
    catch (...)
    {
    }
}

Outcome DepthBinder::Begin(uint64_t runtime)
{
    if (!_enabled.load(std::memory_order_relaxed))
    {
        // switched off with a pass still open (its finish never came): put it back now
        if (const auto it = _runtimes.find(runtime); it != _runtimes.end())
            EndPass(runtime, it->second, End::Rollback);
        return Done(Outcome::Disabled);
    }

    Runtime& state = _runtimes[runtime];
    const uint64_t frame = _backend.Frame();
    if (state.hasFrame && state.frame == frame)
        return Outcome::Duplicate;
    state.hasFrame = true;
    state.frame = frame;
    EndPass(runtime, state, End::Rollback); // a pass an earlier frame never finished

    try
    {
        return Bind(runtime, state, frame);
    }
    catch (...)
    {
        EndPass(runtime, state, End::Rollback);
        return Done(Outcome::Failed);
    }
}

Outcome DepthBinder::Bind(uint64_t runtime, Runtime& state, uint64_t frame)
{
    ViewRef copy { _backend };
    const DepthCapture capture = TakeCapture(copy.view);

    // ---- gates: each one leaves ReShade alone
    if (capture.device != 0 && _backend.Device(runtime) != capture.device)
        return Done(Outcome::OtherDevice);
    if (!_backend.IsD3D11(runtime))
        return Done(Outcome::NotD3D11);
    if (copy.view == 0 || !IsFresh(capture, _backend.NowMs(), frame))
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

    // ---- bind. From here `state` owns every reference taken and every way out goes through EndPass.
    state.open = true;
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
    SetStatus(BoundText(capture));
    return Outcome::Bound;
}

// Ends the open pass of `state` (if any) and drops the references it holds. Every backend call is guarded on its own so one
// failure cannot keep the rest from running.
void DepthBinder::EndPass(uint64_t runtime, Runtime& state, End how)
{
    if (!state.open)
        return;
    if (how != End::Forget)
    {
        if (state.bound)
            RestoreBinding(runtime, state, how == End::Finish);
        for (const SavedBool& saved : state.bools)
        {
            try
            {
                if (!_backend.SetUniformBool(runtime, saved.variable, saved.value))
                    SetStatus(kStatusFailed);
            }
            catch (...)
            {
                SetStatus(kStatusFailed);
            }
        }
    }
    ReleaseView(state.copy);
    if (state.refSrv)
        ReleaseView(state.origSrv);
    if (state.refSrgb)
        ReleaseView(state.origSrgb);

    state.open = false;
    state.bound = false;
    state.copy = state.variable = state.origSrv = state.origSrgb = 0;
    state.refSrv = state.refSrgb = false;
    state.bools.clear();
}

// Puts the original binding back, but only if the DEPTH semantic still points at our copy. Another add-on (Generic Depth
// picking a new buffer) may have replaced it during the pass, and its choice wins.
void DepthBinder::RestoreBinding(uint64_t runtime, Runtime& state, bool count)
{
    try
    {
        uint64_t srv = 0, srgb = 0;
        if (!_backend.TextureBinding(runtime, state.variable, srv, srgb))
        {
            SetStatus(kStatusFailed); // cannot tell whose binding it is: leave it
            return;
        }
        if (srv != state.copy || srgb != state.copy)
        {
            if (count)
                Bump(Counter::OverwrittenByOther);
            return;
        }
        if (state.origSrv == state.copy || state.origSrgb == state.copy)
            return; // the original is a copy of ours (a leaked earlier bind): never put that back
        if (!_backend.UpdateTextureBindings(runtime, kDepthSemantic, state.origSrv, state.origSrgb))
            SetStatus(kStatusFailed);
    }
    catch (...)
    {
        SetStatus(kStatusFailed);
    }
}

// ---- manual depth transform ---------------------------------------------------------------------------------------------
bool DepthBinder::AnyNonNeutral(uint64_t runtime, const char* effect)
{
    std::string value;
    for (const TransformMacro& macro : kTransformMacros)
        if (_backend.Definition(runtime, effect, macro.name, value) && !IsNeutralDefinition(value, macro.neutral))
            return true;
    return false;
}

// False = a query failed (nothing is remembered). `manual` is true if any macro is off its neutral value, globally or for an
// effect that has an enabled technique.
bool DepthBinder::ManualTransform(uint64_t runtime, bool& manual)
{
    manual = AnyNonNeutral(runtime, nullptr);
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
        if (AnyNonNeutral(runtime, t.effect.c_str()))
        {
            manual = true;
            return true;
        }
    }
    return true;
}
} // namespace ext::reshade::depth
