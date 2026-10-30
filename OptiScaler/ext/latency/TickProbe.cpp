#include "pch.h"

#include "ext/latency/TickProbe.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/ActiveBuild.h"
#include "ext/latency/TickLease.h"
#include "ext/latency/TickProbeCore.h"

#include <atomic>
#include <mutex>
#include <string>

// The simulation-tick probe's sink: the 10 s line of TickProbeCore.h and the one-off lines about the lease (surface in ext/latency/TickProbe.h,
// rules in ext/latency/TickProbeCore.h and ext/latency/TickLeaseCore.h).
//
// Cost: with the key off every tap is one relaxed load of the mirrored key (and, on a present that follows a key change, one more of the arm state):
// no ini read, no clock, no lock, no exchange. With the key on, one QueryPerformanceCounter read, one counter read, one mutex and the core's
// arithmetic per event - about five events per frame - and one log line per 10 s. Nothing here allocates per event: the Probe is fixed-size and the
// line is built only when a window closes. The mutex is held for a few hundred nanoseconds; the log call happens under it only because the sink may
// be swapped, and it does not block on I/O for long.
//
// The key is mirrored once per config load (ext::AddKeysLoadedHook, so a reload applies live) and once at the first device hook; no per-frame path
// reads it. The arm state also requires the lease (Leased()), which is what makes the probe start counting only after the observer is in the slot:
// the ticks in flight at that moment were never recorded and are dropped by the core, not mis-paired.
namespace ext::latency
{
namespace
{
// QueryPerformanceCounter in microseconds, without the overflow a plain `qpc * 1'000'000` reaches after a few days of uptime.
std::int64_t QpcFrequency()
{
    static const std::int64_t freq = []
    {
        LARGE_INTEGER f {};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 1;
    }();
    return freq;
}

std::int64_t QpcMicros()
{
    LARGE_INTEGER now {};
    QueryPerformanceCounter(&now);
    const std::int64_t freq = QpcFrequency();
    return (now.QuadPart / freq) * 1'000'000 + (now.QuadPart % freq) * 1'000'000 / freq;
}

// The log sink: feeds the pure core and writes the window's line. Owns the process's Probe (there is one sink at a time).
struct LogSink final : IProbeSink
{
    void OnTickBegin(std::int64_t qpcUs, std::uint32_t tid, std::uint32_t frameBefore, bool frameCounter) override
    {
        _probe.TickBegin(qpcUs, tid, frameBefore, frameCounter);
        Due(qpcUs);
    }
    void OnTickEnd(std::int64_t qpcUs, std::uint32_t frameAfter) override
    {
        _probe.TickEnd(qpcUs, frameAfter);
        Due(qpcUs);
    }
    void OnPresentBegin(std::int64_t qpcUs, std::uint32_t tid, std::uint32_t frameAtBegin, bool frameCounter) override
    {
        _probe.PresentBegin(qpcUs, tid, frameAtBegin, frameCounter);
        Due(qpcUs);
    }
    void OnPresentEnd(std::int64_t qpcUs, std::uint32_t frameAtEnd) override
    {
        _probe.PresentEnd(qpcUs, frameAtEnd);
        Due(qpcUs);
    }
    void OnSleepBegin(std::int64_t qpcUs) override
    {
        _probe.SleepBegin(qpcUs);
        Due(qpcUs);
    }
    void OnSleepEnd(std::int64_t qpcUs) override
    {
        _probe.SleepEnd(qpcUs);
        Due(qpcUs);
    }
    void OnNote(const char* text) override { Emit(std::string("Tick probe: ") + (text != nullptr ? text : "?")); }

    // The window rule of FramePacingCore.h: the event that arrives 10 s or more after the window opened carries its summary, and the next event
    // opens the next window.
    void Due(std::int64_t nowUs)
    {
        Summary summary;
        if (_probe.Due(nowUs, summary))
            Emit(FormatSummary(summary));
    }

  private:
    static void Emit(const std::string& line) noexcept
    {
        try
        {
            LOG_INFO("{}", line);
        }
        catch (...)
        {
        }
    }

    Probe _probe;
};

LogSink& Sink()
{
    // Never destroyed: a tick can still arrive while the process tears down, and there is nothing to free.
    static LogSink* sink = new LogSink();
    return *sink;
}

std::atomic<IProbeSink*> g_sink { nullptr };
std::mutex g_mutex;
// The mirrored [FramePacing] TickProbe key: one relaxed load is the whole cost of a tap with the key off.
std::atomic<bool> g_keyOn { false };
// True while the probe records: the key is on, the build's tick anchors are enabled and the lease is installed. Recomputed at a bridge present,
// never read from the config there.
std::atomic<bool> g_armed { false };
std::string g_lastNote; // guarded by g_mutex: one line per distinct text

IProbeSink& Target() noexcept
{
    IProbeSink* sink = g_sink.load(std::memory_order_acquire);
    return sink != nullptr ? *sink : Sink();
}

bool ComputeArmed()
{
    // The mirrored key, then the lease, then the gate: the lease is what makes the observer exist, and the gate is what says the anchors resolved on
    // this build. Reading them here (and not in the taps) keeps a present's cost at one relaxed load with the key off.
    if (!g_keyOn.load(std::memory_order_relaxed))
        return false;
    if (!Leased())
        return false;
    return ext::ffxiv::BuildFor(ext::ffxiv::resolve::Feature::LatencyTick) != nullptr;
}

// A config load ran: mirror the key so a reload applies live without a per-frame ini read. Registered at static init (the UpdateCheck.cpp pattern).
void OnKeysLoaded() { RefreshKeys(); }

[[maybe_unused]] const bool kKeysLoadedRegistered = (ext::AddKeysLoadedHook(&OnKeysLoaded), true);
} // namespace

void SetEventSink(IProbeSink* sink) { g_sink.store(sink, std::memory_order_release); }

bool Armed() { return g_armed.load(std::memory_order_relaxed); }

bool KeyOn() { return g_keyOn.load(std::memory_order_relaxed); }

void RefreshKeys() { g_keyOn.store(ext::Cfg().framePacing.TickProbeOn(), std::memory_order_relaxed); }

void OnBridgePresentBegin()
{
    // The once-per-frame place the arm state is recomputed. With the key off nothing is read but the mirror and the state below, and no exchange
    // happens: this is the whole cost of a present with the probe off.
    if (!g_keyOn.load(std::memory_order_relaxed))
    {
        if (g_armed.load(std::memory_order_relaxed))
        {
            g_armed.store(false, std::memory_order_relaxed);
            Note("paused: the key is off");
        }
        return;
    }
    const bool armed = ComputeArmed();
    if (g_armed.load(std::memory_order_relaxed) != armed)
    {
        g_armed.store(armed, std::memory_order_relaxed);
        if (!armed)
            Note("paused: the lease is not installed or this build's tick is unavailable");
    }
    if (!armed)
        return;
    std::uint32_t frame = 0;
    const bool frameCounter = ReadFrameCounter(frame);
    const std::int64_t now = QpcMicros();
    const std::uint32_t tid = GetCurrentThreadId();
    std::lock_guard lock(g_mutex);
    Target().OnPresentBegin(now, tid, frame, frameCounter);
}

void OnBridgePresentEnd()
{
    if (!g_armed.load(std::memory_order_relaxed))
        return;
    std::uint32_t frame = 0;
    (void) ReadFrameCounter(frame); // a counter that disappeared between the begin and the end leaves 0: the core keeps the begin's value
    const std::int64_t now = QpcMicros();
    std::lock_guard lock(g_mutex);
    Target().OnPresentEnd(now, frame);
}

void OnSleepScopeBegin()
{
    if (!g_armed.load(std::memory_order_relaxed))
        return;
    const std::int64_t now = QpcMicros();
    std::lock_guard lock(g_mutex);
    Target().OnSleepBegin(now);
}

void OnSleepScopeEnd()
{
    if (!g_armed.load(std::memory_order_relaxed))
        return;
    const std::int64_t now = QpcMicros();
    std::lock_guard lock(g_mutex);
    Target().OnSleepEnd(now);
}

void OnTickBegin(std::uint32_t frameBefore, bool frameCounter)
{
    if (!g_armed.load(std::memory_order_relaxed))
        return;
    const std::int64_t now = QpcMicros();
    const std::uint32_t tid = GetCurrentThreadId();
    std::lock_guard lock(g_mutex);
    Target().OnTickBegin(now, tid, frameBefore, frameCounter);
}

void OnTickEnd(std::uint32_t frameAfter)
{
    if (!g_armed.load(std::memory_order_relaxed))
        return;
    const std::int64_t now = QpcMicros();
    std::lock_guard lock(g_mutex);
    Target().OnTickEnd(now, frameAfter);
}

void Note(const char* text)
{
    if (text == nullptr)
        return;
    IProbeSink* sink = g_sink.load(std::memory_order_acquire);
    std::lock_guard lock(g_mutex);
    if (g_lastNote == text)
        return; // the same line twice in a row says nothing new
    g_lastNote = text;
    IProbeSink& target = sink != nullptr ? *sink : Sink();
    try
    {
        target.OnNote(text);
    }
    catch (...)
    {
    }
}
} // namespace ext::latency
