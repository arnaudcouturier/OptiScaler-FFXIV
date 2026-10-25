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

// Every variable with one of the names, best name first. Which of them really reads the DEPTH semantic is not known until the
// copy has been bound and one of them shows it.
std::vector<const TextureVar*> FindDepthInputs(const std::vector<TextureVar>& textures)
{
    std::vector<const TextureVar*> found;
    for (const char* name : kDepthInputNames)
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
DepthBinder::DepthBinder(IBackend& backend, LogFn log) : _backend(backend), _log(log)
{
    for (auto& last : _lastLogged)
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
            if (entry.second.open)
                ReleasePass(entry.second);
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
    out.failed = n(Counter::Failed);
    out.erasedUnverified = n(Counter::ErasedUnverified);
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

// The count always ticks. The line follows the one rule of ShouldLog, by the backend's clock; a clock that fails costs this line
// only (the next occurrence is asked again), and a hook that throws does not make the occurrence logged twice.
void DepthBinder::Bump(Counter counter) noexcept
{
    const size_t index = static_cast<size_t>(counter);
    const uint64_t count = _counts[index].fetch_add(1, std::memory_order_relaxed) + 1;
    if (_log == nullptr)
        return;
    try
    {
        const int64_t now = _backend.NowMs();
        int64_t last = _lastLogged[index].load(std::memory_order_relaxed);
        if (!ShouldLog(count, now, last) || !_lastLogged[index].compare_exchange_strong(last, now, std::memory_order_relaxed))
            return;
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
Outcome DepthBinder::OnBeginEffects(uint64_t runtime) noexcept
{
    try
    {
        std::lock_guard lock(_mutex);
        return Begin(runtime);
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
        if (const auto it = _runtimes.find(runtime); it != _runtimes.end())
            EndPass(runtime, it->second, it->second.inPass && !it->second.orphaned ? End::Finish : End::Retry);
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
            // A runtime that holds nothing is simply forgotten. One that still holds the copy (a bound pass, or a restore that is
            // pending) is kept as it is, with its references and no ReShade call: whether the ReShade runtime object is new
            // after a ResizeBuffers (its map empty) or the same one (its map still there) is not known, and both are safe this
            // way. A pointer that comes back runs the normal retry at its next event; one that never does is released by
            // Shutdown().
            if (state.open)
                state.orphaned = true;
            else
                _runtimes.erase(it);
            return;
        }
        // The effects are new: what was looked up is stale. An open pass is restored first (this also repairs a runtime that
        // is dirty or orphaned); if that is not possible the entry stays with its references.
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
    std::vector<TextureVar> textures;
    if (!_backend.TextureVariables(runtime, textures))
        return Done(Outcome::Failed);
    const std::vector<const TextureVar*> candidates = FindDepthInputs(textures);
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
    state.open = true;
    state.inPass = true;
    state.copy = copy.Take();
    state.verified = false;
    state.variable = state.origSrv = state.origSrgb = 0;
    auto fail = [&]
    {
        EndPass(runtime, state, End::Rollback);
        return Done(Outcome::Failed);
    };

    state.bound = true; // set before the call: if it throws after changing the binding, EndPass undoes it
    if (!_backend.UpdateTextureBindings(runtime, kDepthSemantic, state.copy, state.copy))
    {
        state.bound = false; // refused: nothing changed
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
        if (srv == state.copy && srgb == state.copy)
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
        EndPass(runtime, state, End::Rollback);
        return Outcome::NoDepthInput;
    }
    state.verified = true;
    state.variable = reader->handle;
    state.origSrv = reader->srv;
    state.origSrgb = reader->srgb;
    if (state.origSrv != 0)
    {
        _backend.AddRef(state.origSrv);
        state.refSrv = true;
    }
    if (state.origSrgb != 0)
    {
        _backend.AddRef(state.origSrgb);
        state.refSrgb = true;
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
    // An orphan's uniform handles belong to a runtime that is gone (ReShade frees them with the effects): they are not used again, not
    // even to put bufready_depth back. It stays as the pass left it until the effects are loaded again.
    if (state.inPass && how != End::Reload && !state.orphaned)
        RestoreBools(runtime, state, how);
    if (state.bound && !RestoreBinding(runtime, state, how))
    {
        state.inPass = false;
        Bump(Counter::Failed);
        SetStatus(StatusKind::Failed);
        return;
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
    state.open = state.inPass = state.bound = state.orphaned = false;
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

// Takes the copy out of the DEPTH entry. True = ReShade holds nothing of ours any more; false = that could not be checked or
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
// It can lose the pick of another add-on (Generic Depth) until that add-on selects again. A DELIBERATE TRADE-OFF: the
// alternative, leaving the entry alone when a same-named variable no longer shows the copy, would trust a variable that may be
// a decoy without the DEPTH semantic, and then the copy stays in a map that ReShade frees views of.
bool DepthBinder::RestoreBinding(uint64_t runtime, Runtime& state, End how) noexcept
{
    try
    {
        auto holdsCopy = [&](uint64_t srv, uint64_t srgb) { return srv == state.copy || srgb == state.copy; };
        auto eraseUnverified = [&]
        {
            if (!_backend.UpdateTextureBindings(runtime, kDepthSemantic, 0, 0))
                return false;
            Bump(Counter::ErasedUnverified);
            return true;
        };
        if (!state.verified)
            return eraseUnverified();

        uint64_t srv = 0, srgb = 0;
        if (how == End::Finish || how == End::Rollback)
        {
            if (!_backend.TextureBinding(runtime, state.variable, srv, srgb))
                return false;
            if (!holdsCopy(srv, srgb))
            {
                if (how == End::Finish)
                    Bump(Counter::OverwrittenByOther);
                return true;
            }
            return _backend.UpdateTextureBindings(runtime, kDepthSemantic, state.origSrv, state.origSrgb);
        }

        std::vector<TextureVar> textures;
        if (!_backend.TextureVariables(runtime, textures))
            return false;
        const std::vector<const TextureVar*> candidates = FindDepthInputs(textures);
        for (const TextureVar* c : candidates)
        {
            if (!_backend.TextureBinding(runtime, c->handle, srv, srgb))
                return false;
            if (holdsCopy(srv, srgb))
                return _backend.UpdateTextureBindings(runtime, kDepthSemantic, state.origSrv, state.origSrgb);
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
