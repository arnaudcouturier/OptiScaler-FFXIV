#include "pch.h"

#include "ext/present/PresentSeams.h"

#include "ext/core/ExtConfig.h"
#include "ext/present/HiddenPresentPolicy.h"

#include <State.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

// Frame-pacing CPU trace and the hidden-present switch (ext/present/FramePacingCore.h and HiddenPresentPolicy.h hold the rules).
//
// Cost: with the trace off a scope is one call and a handful of loads (the key, the log settings, the cached exe check). With it on
// it is two steady_clock reads per scope, a thread-local accumulator update and, once per 10 s per thread and phase, one log line.
// No mutex, no allocation (outside the line itself), no GPU call. Nothing here throws into the caller: a failure loses a sample.
namespace ext::present
{
namespace
{
using Clock = std::chrono::steady_clock;

int64_t NowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }

constexpr int64_t kStaleDispatchUs = 1'000'000; // an FG dispatch span older than this never ended (a failed dispatch)

// The exe name is read once: 0 = not known yet (State::gameExe is set early in startup), 1 = ffxiv_dx11.exe, 2 = anything else.
// The exe name, not ext::Current(): the game table only knows the builds it has anchors for, and after a game patch the trace must
// not go silent exactly when its numbers are wanted. None of the timed sites needs those anchors.
std::atomic<int> g_exe { 0 };

int ExeState()
{
    int s = g_exe.load(std::memory_order_relaxed);
    if (s != 0)
        return s;
    const std::string& exe = State::Instance().gameExe;
    if (exe.empty())
        return 0;
    s = _stricmp(exe.c_str(), "ffxiv_dx11.exe") == 0 ? 1 : 2;
    g_exe.store(s, std::memory_order_relaxed);
    return s;
}

// The last decision that was announced (AnnounceKey: reason, file log and level in one number), so a change is announced again.
// Checked with a plain load at every scope; only a changed key reaches the locked exchange (AnnounceChanged).
std::atomic<int> g_announced { -1 };

void Announce(const Decision& d, bool logToFile, int level)
{
    if (!AnnounceChanged(g_announced, AnnounceKey(d.reason, logToFile, level)))
        return;
    const std::string line = DecisionLine(d.reason, logToFile, level);
    LOG_INFO("{}", line);
}

bool TraceOn()
{
    const int exe = ExeState();
    if (exe == 0)
        return false; // too early to tell: stay quiet, nothing is announced yet
    const auto* cfg = Config::Instance();
    const bool toFile = cfg->LogToFile.value_or_default();
    const int level = cfg->LogLevel.value_or_default();
    const Decision d = Decide(ext::Cfg().framePacing.TraceKey(), toFile, level, exe == 1);
    try
    {
        Announce(d, toFile, level);
    }
    catch (...)
    {
    }
    return d.on;
}

struct ThreadState
{
    std::array<Window, static_cast<size_t>(Phase::Count)> windows {};
    Span native;   // NativeBegin .. NativeEnd (the DX11 bridge Present)
    Span dispatch; // FgDispatchBegin .. FgDispatchEnd
};

ThreadState& Tls()
{
    thread_local ThreadState state;
    return state;
}

// Feeds one finished call to the thread's window of the phase and logs the summary when the window closed.
void PacingReport(Phase phase, uint64_t durationUs, int64_t nowUs) noexcept
{
    try
    {
        Summary s;
        if (!Tls().windows[static_cast<size_t>(phase)].Add(durationUs, nowUs, s))
            return;
        const bool showSkipped = phase == Phase::HiddenPresent && ext::Cfg().framePacing.hiddenPresentDoNotWait.value_or_default();
        const std::string line = FormatLine(phase, GetCurrentThreadId(), s, showSkipped);
        LOG_INFO("{}", line);
    }
    catch (...)
    {
    }
}

void NoteSkipped(Phase phase) { Tls().windows[static_cast<size_t>(phase)].NoteSkipped(); }

hidden::Policy& HiddenPolicy()
{
    static hidden::Policy policy;
    return policy;
}
} // namespace

PacingScope::PacingScope(Phase phase, bool counts) noexcept : _phase(phase), _startUs(0), _active(false)
{
    if (counts && TraceOn())
    {
        _active = true;
        _startUs = NowUs();
    }
}

PacingScope::~PacingScope()
{
    if (!_active)
        return;
    const int64_t now = NowUs();
    PacingReport(_phase, now >= _startUs ? static_cast<uint64_t>(now - _startUs) : 0, now);
}

void RecordSample(Phase phase, uint64_t durationUs)
{
    if (TraceOn())
        PacingReport(phase, durationUs, NowUs());
}

void NativeBegin()
{
    if (TraceOn())
        Tls().native.Begin(NowUs());
    else
        Tls().native.Cancel();
}

void NativeEnd()
{
    if (!TraceOn())
    {
        Tls().native.Cancel();
        return;
    }
    const int64_t now = NowUs();
    uint64_t us = 0;
    if (Tls().native.End(now, 0, us))
        PacingReport(Phase::NativePresent, us, now);
}

void FgDispatchBegin()
{
    if (TraceOn())
        Tls().dispatch.Begin(NowUs());
    else
        Tls().dispatch.Cancel();
}

void FgDispatchEnd()
{
    if (!TraceOn())
    {
        Tls().dispatch.Cancel();
        return;
    }
    const int64_t now = NowUs();
    uint64_t us = 0;
    if (Tls().dispatch.End(now, kStaleDispatchUs, us))
        PacingReport(Phase::FgDispatch, us, now);
}

namespace
{
// The two outside dependencies of hidden::SeamPresent: the key, and what each present's report does (the trace's skipped counter and
// the log lines). Nothing here throws into the caller.
struct HiddenHooks
{
    const PacingScope& scope;

    bool KeyOn() const { return ext::Cfg().framePacing.hiddenPresentDoNotWait.value_or_default(); }

    void OnReport(const hidden::Report& report, HRESULT hr) const noexcept
    {
        try
        {
            if (report.outcome == hidden::Outcome::Skipped && scope.Active())
                NoteSkipped(Phase::HiddenPresent);
            if (report.latchedNow)
                LOG_WARN("hidden DX11 present: DXGI rejected DO_NOT_WAIT ({:X}); the flag is switched off for this session", (UINT) hr);
            if (report.episodeStarted)
                LOG_INFO("hidden DX11 present: skipped {} presents in a row (the hidden swapchain is busy); {} skipped in total",
                         report.consecutive, HiddenPolicy().Skipped());
        }
        catch (...)
        {
        }
    }
};
} // namespace

HRESULT HiddenPresent(IDXGISwapChain* swapChain, UINT flags)
{
    PacingScope scope(Phase::HiddenPresent);
    HiddenHooks hooks { scope };
    return hidden::SeamPresent(HiddenPolicy(), swapChain, flags, hooks);
}
} // namespace ext::present
