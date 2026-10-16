#include "pch.h"

#include "ext/reshade/ReShadeProbe.h"

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
        {
            std::lock_guard lock(_mutex);
            Entry& e = FindOrCreateLocked(runtime);
            e.alive = true;
            if (e.generation == 0)
                e.generation = 1;
            if (ok)
                e.info = info;
            e.info.id = runtime;
            lines = ok ? FormatRuntime(e.info, "reshade_reloaded_effects")
                       : std::vector<std::string> { std::format("[F1] reshade_reloaded_effects runtime {}: description "
                                                               "failed", Hex(runtime)) };
        }
        WriteLines(lines);
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
} // namespace ext::reshade::probe
