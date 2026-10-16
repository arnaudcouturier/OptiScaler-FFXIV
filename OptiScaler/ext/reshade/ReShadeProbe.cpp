#include "pch.h"

#include "ext/reshade/ReShadeProbe.h"

#include "ext/core/HooksBypass.h"

#include <format>

namespace ext::reshade::probe
{
namespace
{
size_t Idx(EventKind k) { return static_cast<size_t>(k); }
thread_local bool t_inProbeCall = false;
} // namespace

bool InProbeCall() { return t_inProbeCall; }
ProbeCallScope::ProbeCallScope() { t_inProbeCall = true; }
ProbeCallScope::~ProbeCallScope() { t_inProbeCall = false; }

Session::Session(IBackend& backend, IReport& report) : _backend(backend), _report(report), _ledger(backend.QpcFrequency()) {}

void Session::WriteLines(const std::vector<std::string>& lines)
{
    for (const auto& l : lines)
        _report.Line(l);
}

void Session::Note(const std::string& line) { _report.Line(line); }

void Session::Begin(const std::string& registrationLine)
{
    _report.Line(std::format("[F1] {}", registrationLine));
    _report.Line("[F1] Events are logged below. Normal frames are logged for the first 120 frames of each runtime, then one "
                 "summary line per 600 frames.");
}

Session::Entry* Session::FindLocked(uint64_t runtime)
{
    for (auto& e : _runtimes)
    {
        if (e.info.id == runtime && (e.alive || e.generation != 0))
            return &e;
    }
    return nullptr;
}

const Session::Entry* Session::FindLocked(uint64_t runtime) const
{
    for (const auto& e : _runtimes)
    {
        if (e.info.id == runtime && (e.alive || e.generation != 0))
            return &e;
    }
    return nullptr;
}

Session::Entry& Session::FindOrCreateLocked(uint64_t runtime)
{
    if (Entry* e = FindLocked(runtime))
        return *e;
    for (auto& e : _runtimes)
    {
        if (!e.alive && e.generation == 0)
        {
            e.info.id = runtime;
            return e;
        }
    }
    // More runtimes than slots: recycle the last one.
    _runtimes.back() = Entry {};
    _runtimes.back().info.id = runtime;
    return _runtimes.back();
}

void Session::OnInitRuntime(uint64_t runtime)
{
    try
    {
        _counts[Idx(EventKind::InitRuntime)]++;
        RuntimeInfo info;
        const bool ok = _backend.Describe(runtime, false, info);

        std::vector<std::string> lines;
        {
            std::lock_guard lock(_mutex);
            Entry& e = FindOrCreateLocked(runtime);
            e.generation++;
            e.alive = true;
            if (ok)
                e.info = info;
            e.info.id = runtime;
            lines = ok ? FormatRuntime(e.info, std::format("init_effect_runtime #{}", e.generation))
                       : std::vector<std::string> { std::format("[F1] init_effect_runtime #{} runtime {}: description "
                                                               "failed (see log)", e.generation, Hex(runtime)) };
        }
        WriteLines(lines);
    }
    catch (...)
    {
    }
}

void Session::OnDestroyRuntime(uint64_t runtime)
{
    try
    {
        _counts[Idx(EventKind::DestroyRuntime)]++;
        {
            std::lock_guard lock(_mutex);
            if (Entry* e = FindLocked(runtime))
                e->alive = false;
            _ledger.Forget(runtime);
        }
        _report.Line(std::format("[F1] destroy_effect_runtime: runtime {} (ReShade reuses the pointer value when it "
                                 "recreates the runtime after ResizeBuffers)", Hex(runtime)));

        if (_state.load() == ProbeState::AwaitFrameEnd)
        {
            bool mine = false;
            {
                std::lock_guard lock(_mutex);
                mine = _pending.runtime == runtime;
                if (mine)
                    _pending.effectsStateChanged = false; // the runtime is gone, nothing to restore
            }
            if (mine)
                GiveUpPending("the runtime was destroyed before its frame ended");
        }
    }
    catch (...)
    {
    }
}

void Session::OnReloadedEffects(uint64_t runtime)
{
    try
    {
        _counts[Idx(EventKind::ReloadedEffects)]++;
        RuntimeInfo info;
        const bool ok = _backend.Describe(runtime, true, info);

        std::vector<std::string> lines;
        std::string verdict;
        {
            std::lock_guard lock(_mutex);
            Entry& e = FindOrCreateLocked(runtime);
            e.alive = true;
            if (e.generation == 0)
                e.generation = 1;
            if (ok)
                e.info = info;
            e.info.id = runtime;
            if (ok)
            {
                size_t enabledCount = 0;
                for (const auto& t : e.info.techniques)
                    enabledCount += t.enabled ? 1 : 0;
                if (!_f1Verdict.exchange(true))
                    verdict = std::format("VERDICT F1: registered; runtime {} api={} hwnd={} back buffer {}x{}; {} techniques, {} enabled; "
                                          "effects {}; preset \"{}\"", Hex(runtime), ApiName(e.info.api), Hex(e.info.hwnd),
                                          e.info.backBufferWidth, e.info.backBufferHeight, e.info.techniques.size(), enabledCount,
                                          e.info.effectsEnabled ? "enabled" : "DISABLED", e.info.presetPath);
            }
            lines = ok ? FormatRuntime(e.info, "reshade_reloaded_effects")
                       : std::vector<std::string> { std::format("[F1] reshade_reloaded_effects runtime {}: description "
                                                               "failed", Hex(runtime)) };
        }
        WriteLines(lines);
        if (!verdict.empty())
            _report.Line(verdict);
    }
    catch (...)
    {
    }
}

void Session::OnPresetPath(uint64_t runtime, const char* path)
{
    try
    {
        _counts[Idx(EventKind::PresetPath)]++;
        const std::string p = path != nullptr ? path : "";
        {
            std::lock_guard lock(_mutex);
            if (Entry* e = FindLocked(runtime))
                e->info.presetPath = p;
        }
        _report.Line(std::format("[F1] reshade_set_current_preset_path: runtime {} preset \"{}\"", Hex(runtime), p));
    }
    catch (...)
    {
    }
}

void Session::OnEffects(EffectsEvent event)
{
    try
    {
        _counts[Idx(event.begin ? EventKind::BeginEffects : EventKind::FinishEffects)]++;
        std::lock_guard lock(_mutex);
        _ledger.OnEffects(event);
    }
    catch (...)
    {
    }
}

void Session::OnReshadePresent(uint64_t runtime)
{
    try
    {
        _counts[Idx(EventKind::ReshadePresent)]++;
        _framesSeen++;
        std::vector<std::string> lines;
        {
            std::lock_guard lock(_mutex);
            lines = _ledger.OnFrameEnd(runtime);
        }
        WriteLines(lines);

        // The armed probe is driven by the frame boundary of the runtime too.
        const ProbeState state = _state.load();
        if (state == ProbeState::Armed)
        {
            bool timedOut = false;
            {
                std::lock_guard lock(_mutex);
                timedOut = ++_armedFrames >= kArmTimeoutFrames;
            }
            if (timedOut)
            {
                ProbeState expected = ProbeState::Armed;
                if (_state.compare_exchange_strong(expected, ProbeState::Idle))
                {
                    const std::string msg = std::format(
                        "probe disarmed: no boundary call within {} ReShade frames (ProbeAtBoundary is not wired in this "
                        "build, or the boundary detector never fired)", kArmTimeoutFrames);
                    _report.Line("[F2] " + msg);
                    SetResult(msg);
                }
            }
        }
        else if (state == ProbeState::AwaitFrameEnd)
        {
            bool mine = false, giveUp = false;
            {
                std::lock_guard lock(_mutex);
                mine = _pending.runtime == runtime;
                giveUp = !mine && ++_pending.framesWaited >= kAwaitTimeoutFrames;
            }
            if (mine)
                FinishFrame(runtime);
            else if (giveUp)
                GiveUpPending("the probed runtime delivered no reshade_present");
        }
    }
    catch (...)
    {
    }
}

void Session::OnQueuePresent() { _counts[Idx(EventKind::QueuePresent)]++; }

uint32_t Session::AliveRuntimes() const
{
    std::lock_guard lock(_mutex);
    uint32_t n = 0;
    for (const auto& e : _runtimes)
        n += e.alive ? 1 : 0;
    return n;
}

bool Session::Runtime(uint64_t runtime, RuntimeInfo& out) const
{
    std::lock_guard lock(_mutex);
    const Entry* e = FindLocked(runtime);
    if (e == nullptr)
        return false;
    out = e->info;
    return true;
}

EventCounts Session::Counts() const
{
    EventCounts c;
    for (size_t i = 0; i < kEventKinds; i++)
        c.n[i] = _counts[i].load(std::memory_order_relaxed);
    return c;
}

SessionStatus Session::Status(const std::string& reportFolder, bool reportFailed) const
{
    SessionStatus s;
    s.counts = Counts();
    s.aliveRuntimes = AliveRuntimes();
    s.framesSeen = _framesSeen.load(std::memory_order_relaxed);
    s.reportFolder = reportFolder;
    s.reportFailed = reportFailed;
    return s;
}

// ---- the armed probe ------------------------------------------------------------------------------------------------
const char* SkipMethodName(SkipMethod method)
{
    return method == SkipMethod::RenderEffectsOnly ? "render_effects only (documented implicit skip)"
                                                   : "render_effects + set_effects_state(false) until reshade_present";
}

std::vector<uint64_t> Session::AliveIds() const
{
    std::lock_guard lock(_mutex);
    std::vector<uint64_t> ids;
    for (const auto& e : _runtimes)
    {
        if (e.alive)
            ids.push_back(e.info.id);
    }
    return ids;
}

void Session::SetResult(const std::string& line)
{
    std::lock_guard lock(_mutex);
    _lastResult = line;
}

void Session::Fail(const std::string& why, bool latchInert)
{
    _report.Line("[F2] probe FAILED: " + why);
    _report.Line("VERDICT F2: not answered (" + why + ")");
    SetResult("FAILED: " + why);
    if (latchInert)
    {
        _inert = true;
        _report.Line("[F2] the probe is latched INERT for the rest of this process (an exception escaped the render_effects "
                     "call; restart the game before trying again)");
    }
    _state.store(ProbeState::Idle);
}

Session::ProbeStatus Session::ProbeStatusCopy() const
{
    ProbeStatus s;
    s.state = _state.load();
    s.inert = _inert.load();
    std::lock_guard lock(_mutex);
    s.method = _method;
    s.runs = _runs;
    s.lastResult = _lastResult;
    return s;
}

bool Session::Arm(SkipMethod method, std::string* why)
{
    auto refuse = [&](const char* reason)
    {
        if (why != nullptr)
            *why = reason;
        return false;
    };
    if (_inert.load())
        return refuse("latched inert after an exception in an earlier probe (see reshade-probe.txt); restart the game");

    ProbeState expected = ProbeState::Idle;
    if (!_state.compare_exchange_strong(expected, ProbeState::Armed))
        return refuse("a probe is already armed or still running");

    {
        std::lock_guard lock(_mutex);
        _method = method;
        _armedFrames = 0;
    }
    _report.Line(std::format("[F2] probe ARMED, skip method: {}. Waiting for the boundary call (ProbeAtBoundary).",
                             SkipMethodName(method)));
    return true;
}

void Session::Disarm()
{
    ProbeState expected = ProbeState::Armed;
    if (_state.compare_exchange_strong(expected, ProbeState::Idle))
        _report.Line("[F2] probe disarmed by the user");
}

void Session::GiveUpPending(const char* why)
{
    Pending p;
    {
        std::lock_guard lock(_mutex);
        p = _pending;
    }
    if (p.effectsStateChanged)
    {
        const bool restored = _backend.SetEffectsState(p.runtime, p.previousEffectsState);
        _report.Line(std::format("[F3] effects state restore after give-up: {}", restored ? "done" : "FAILED"));
    }
    _report.Line(std::format("[F3] frame end not observed: {}", why));
    _report.Line(std::format("VERDICT F3: not answered ({})", why));
    SetResult(std::string("F3 not answered: ") + why);
    _state.store(ProbeState::Idle);
}

void Session::ProbeAtBoundary(void* context)
{
    ProbeState expected = ProbeState::Armed;
    if (!_state.compare_exchange_strong(expected, ProbeState::Running))
        return; // the per-frame cost when not armed: one load and a failed compare

    // Everything below issues D3D11 work on the immediate context (inspect, staging copies, ReShade's own passes): none of
    // it may look like game work to the draw hooks (lighting capture hooks CopyResource and every draw).
    ext::core::BypassScope bypass;
    try
    {
        SkipMethod method;
        uint32_t run;
        {
            std::lock_guard lock(_mutex);
            method = _method;
            run = ++_runs;
        }
        const double usPerTick = 1e6 / double(_backend.QpcFrequency());
        const int64_t t0 = _backend.Qpc();

        _report.Line(std::format("[F2] ===== probe run {} at the boundary call, method: {} =====", run, SkipMethodName(method)));
        _report.Line("[F3] skip methods NOT tried on purpose: set_technique_state per technique (it can mark the preset modified, and "
                     "ReShade.ini may have AutoSavePreset=1, so a crash in between could rewrite the user's preset) and event "
                     "vetoes (reshade_begin_effects / reshade_finish_effects return void: no event can veto a pass).");
        const EventCounts counts = Counts();
        _report.Line("[F1] events delivered so far: " + FormatEventCounts(counts));
        if (counts.Total() == 0)
        {
            _report.Line("[F1] WARNING: registered but ReShade delivered NO event at all. Either the registration made in "
                         "DllMain was too early for this ReShade (it searches for add-ons later, at the first device "
                         "creation) or this build restricts the events. Next registration point to try: the existing "
                         "ext::game::OnD3D11DeviceHooked seam (no new upstream line).");
            return Fail("no ReShade event was ever delivered", false);
        }

        const std::vector<uint64_t> alive = AliveIds();
        if (alive.empty())
            return Fail("no live effect runtime (init_effect_runtime delivered none, or all were destroyed)", false);

        std::string why;
        const uint64_t runtime = _backend.PickRuntime(context, alive, why);
        _report.Line("[F1] runtime selection: " + why);
        if (runtime == 0)
            return Fail("no runtime matches this D3D11 context", false);

        RuntimeInfo info;
        if (!_backend.Describe(runtime, true, info))
            return Fail("describing the runtime failed", false);
        WriteLines(FormatRuntime(info, "probe frame"));
        if (info.api != Api::D3D11)
            return Fail(std::format("the runtime is {}, not D3D11", ApiName(info.api)), false);
        if (!info.effectsEnabled)
            return Fail("ReShade effects are disabled (get_effects_state() is false): nothing to apply; enable them and arm again", false);

        const BoundaryInfo b = _backend.Inspect(context, runtime);
        _report.Line(std::format("[F2] boundary target: context={} reshade-immediate-list-native={} resource={} {}x{} fmt={} samples={} "
                                 "array={} is-runtime-back-buffer={}; rtv={} rtv_srgb={} ({})",
                                 Hex(b.context), Hex(b.cmdListNative), Hex(b.resource), b.width, b.height, b.format, b.samples,
                                 b.arraySize, b.isRuntimeBackBuffer ? "yes" : "no", Hex(b.rtv), Hex(b.rtvSrgb), b.note));
        if (!b.ok)
            return Fail("boundary target refused: " + b.refusal, false);

        const DepthReport depth = _backend.QueryDepth(runtime, b);
        WriteLines(depth.lines);
        _report.Line(std::format(
            "VERDICT F4: {} (inferred from texture-variable bindings; the ReShade API has no direct depth-detection query)",
            !depth.anyDepthBound ? std::string("no depth-format texture binding found (see [F4] lines)")
            : depth.sizeMatchesBackBuffer
                ? "a depth-format texture of the back buffer size is bound: " + depth.summary
                : "a depth-format texture is bound but its size differs from the back buffer (render-resolution depth? effects "
                  "would sample misaligned depth): " + depth.summary));

        _backend.SnapshotBefore(context, b);
        FrameRecord open0;
        {
            std::lock_guard lock(_mutex);
            open0 = _ledger.Open(runtime);
        }

        const int64_t t1 = _backend.Qpc();
        CallOutcome out;
        {
            ProbeCallScope scope;
            out = _backend.RenderEffects(context, runtime, b);
        }
        const int64_t t2 = _backend.Qpc();

        const PixelDiff diff = _backend.SnapshotAfterAndDiff(context, b);
        FrameRecord open1;
        {
            std::lock_guard lock(_mutex);
            open1 = _ledger.Open(runtime);
        }

        _report.Line(std::format("[F2] render_effects: {}{}; pipeline state restored: {} ({}); passes fired inside the call: "
                                 "begin={} finish={}",
                                 out.ok ? "returned normally" : "DID NOT RUN/COMPLETE", out.what.empty() ? "" : " - " + out.what,
                                 out.stateRestored ? "yes" : "NO", out.stateNote, open1.probeBegin - open0.probeBegin,
                                 open1.probeFinish - open0.probeFinish));
        if (diff.valid)
        {
            _report.Line(std::format("[F2] render target before/after: sampled {} pixels, changed {:.2f}%, mean abs delta {:.3f}/255, max "
                                     "delta {} ({})",
                                     diff.sampled, diff.changedFraction * 100.0, diff.meanAbs, diff.maxDelta, diff.note));
        }
        else
            _report.Line("[F2] render target before/after: not available (" + diff.note + ")");

        if (out.exception)
        {
            _report.Line(std::format("[F2] EXCEPTION in render_effects: code {:#x} {}", out.code, out.what));
            return Fail("render_effects raised an exception", true);
        }
        if (!out.ok)
            return Fail("render_effects was not executed: " + out.what, false);

        // F3, method 2: kill the end-of-frame pass explicitly for the rest of this frame.
        bool changed = false, previous = true;
        if (method == SkipMethod::AlsoDisableEffects)
        {
            if (_backend.GetEffectsState(runtime, previous))
                changed = _backend.SetEffectsState(runtime, false);
            _report.Line(std::format("[F3] set_effects_state(false) for the rest of the frame: {} (previous state {})",
                                     changed ? "applied" : "FAILED", previous ? "enabled" : "disabled"));
        }

        const int64_t t3 = _backend.Qpc();
        const double callUs = double(t2 - t1) * usPerTick;
        const double totalUs = double(t3 - t0) * usPerTick;
        _report.Line(std::format("[F5] CPU time (command submission only; GPU time is not measured): render_effects call {:.0f} us; whole "
                                 "probe {:.0f} us, of which staging readbacks {:.0f} us (net {:.0f} us)",
                                 callUs, totalUs, diff.readbackUs, totalUs - diff.readbackUs));
        _report.Line(std::format("VERDICT F5: render_effects took {:.0f} us of CPU on the boundary thread (submission only, one call per "
                                 "probe frame)", callUs));
        _report.Line(std::format("VERDICT F2: render_effects returned normally in place on the boundary target; {}; pipeline "
                                 "state restored: {}. Corruption of the game's own rendering can only be judged by eye (flicker, "
                                 "garbage, missing HUD in the frames after the probe): report what you see.",
                                 !diff.valid ? "pixel comparison unavailable"
                                 : diff.changedFraction > 0
                                     ? std::format("the target CHANGED ({:.2f}% of sampled pixels)", diff.changedFraction * 100.0)
                                     : std::string("the target did NOT change (effects empty, or in-place rendering is not taking effect)"),
                                 out.stateRestored ? "yes" : "NO"));

        {
            std::lock_guard lock(_mutex);
            _pending = Pending {};
            _pending.runtime = runtime;
            _pending.method = method;
            _pending.effectsStateChanged = changed;
            _pending.previousEffectsState = previous;
            _pending.outcome = out;
            _pending.diff = diff;
            _pending.callUs = callUs;
            _pending.totalUs = totalUs;
        }
        SetResult(std::format("run {}: render_effects ok in {:.0f} us, target changed {}; waiting for the frame end", run, callUs,
                              diff.valid ? std::format("{:.1f}%", diff.changedFraction * 100.0) : std::string("n/a")));
        _state.store(ProbeState::AwaitFrameEnd);
    }
    catch (...)
    {
        Fail("exception inside the probe logic", true);
    }
}

void Session::FinishFrame(uint64_t runtime)
{
    try
    {
        Pending p;
        FrameRecord f;
        uint32_t run = 0;
        {
            std::lock_guard lock(_mutex);
            p = _pending;
            f = _ledger.LastClosed(runtime);
            run = _runs;
        }

        _report.Line(std::format("[F3] probe frame closed by reshade_present: passes inside render_effects begin={} finish={}; "
                                 "end-of-frame passes after it begin={} finish={}",
                                 f.probeBegin, f.probeFinish, f.begin, f.finish));
        const bool skipped = f.begin == 0 && f.finish == 0;
        const bool uniformsOnly = !skipped && f.rtv == 0;
        _report.Line(std::format("VERDICT F3 (method: {}): ReShade's own end-of-frame effect pass {} for the probed frame{}",
                                 SkipMethodName(p.method),
                                 skipped ? "was SKIPPED" : uniformsOnly ? "ran with a NULL render target view" : "STILL RAN",
                                 skipped ? ""
                                 : uniformsOnly
                                     ? " (uniform update only, nothing rendered: this is how render_effects documents its skip; "
                                       "counts as SKIPPED for the picture)"
                                     : std::format(" (rtv {}: effects would be applied twice; try the other skip method)", Hex(f.rtv))));
        if (f.probeBegin == 0)
            _report.Line("[F3] note: reshade_begin_effects did not fire for the probe's own call (the header says the events "
                         "are delivered 'in other add-ons'; our own callbacks may not see our call, so the skip verdict "
                         "above only counts passes after the call)");

        std::string restoreText = "no state change was made";
        if (p.effectsStateChanged)
        {
            const bool set = _backend.SetEffectsState(p.runtime, p.previousEffectsState);
            bool now = !p.previousEffectsState;
            const bool read = _backend.GetEffectsState(p.runtime, now);
            const bool exact = set && read && now == p.previousEffectsState;
            restoreText = std::format("effects state restored to {}: {}", p.previousEffectsState ? "enabled" : "disabled",
                                      exact ? "verified" : "NOT VERIFIED");
            _report.Line("[F3] " + restoreText);
        }

        SetResult(std::format("run {}: render_effects {:.0f} us, end-of-frame pass {} ({}); {}", run, p.callUs,
                              skipped ? "skipped" : "STILL RAN", SkipMethodName(p.method), restoreText));
        _state.store(ProbeState::Idle);
    }
    catch (...)
    {
        _state.store(ProbeState::Idle);
    }
}
} // namespace ext::reshade::probe
