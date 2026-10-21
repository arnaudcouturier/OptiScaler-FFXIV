#pragma once
// Pure core of the frame-pacing CPU trace (docs/FEATURES.md "Frame pacing trace"): the phases, the per-phase accumulator, the 10 s
// reporting window, the one-span tracker, the log line text and the decision whether the trace is on. No upstream type, no clock
// and no logger of its own (times are passed in), so every rule is unit-testable (tests/ext/present/frame_pacing_core_test.cpp).
// The clock, the thread-local windows and the logging are in ext/present/FramePacingTrace.cpp.
//
// CPU time only: queued GPU work is not GPU time, so the numbers show where the render and present threads wait.
// The line format and the phase names follow the reference build's trace; the thread id is added (its accumulators are per thread).

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace ext::present
{
// What is timed. The order is the order of kPhaseNames.
enum class Phase : unsigned
{
    NativePresent,  // the whole DX11 bridge Present (ext::dx11bridge::PresentScope)
    HiddenPresent,  // the auxiliary present on the hidden DX11 swapchain (ext::present::HiddenPresent)
    FgDispatch,     // DLSS-G Dispatch: options and constants (ext::present::FgDispatchBegin/End)
    ReflexSleep,    // the Reflex sleep of the FG present path (FG_Hooks.cpp)
    OutputPresent,  // LocalPresent: overlay plus the present on the visible swapchain
    SceneNr,        // scene NR recording (DlssNr_Dx12::Dispatch, lock wait included)
    PreviewNr,      // character preview NR recording time (RecordSample)
    Count
};

inline constexpr std::array<const char*, static_cast<size_t>(Phase::Count)> kPhaseNames { "native present", "hidden DX11 present",
                                                                                          "FG dispatch",    "Reflex sleep",
                                                                                          "output present", "scene NR recording",
                                                                                          "preview NR" };

inline const char* PhaseName(Phase p)
{
    const auto i = static_cast<size_t>(p);
    return i < kPhaseNames.size() ? kPhaseNames[i] : "?";
}

// Thresholds are inclusive (>=), in microseconds. The window length is the same unit.
inline constexpr uint64_t kOver8Us = 8000;
inline constexpr uint64_t kOver16Us = 16000;
inline constexpr uint64_t kOver33Us = 33000;
inline constexpr int64_t kWindowUs = 10'000'000;

struct Accumulator
{
    uint64_t calls = 0, sumUs = 0, maxUs = 0, over8 = 0, over16 = 0, over33 = 0;

    void Add(uint64_t us)
    {
        ++calls;
        sumUs += us;
        if (us > maxUs)
            maxUs = us;
        over8 += us >= kOver8Us ? 1 : 0;
        over16 += us >= kOver16Us ? 1 : 0;
        over33 += us >= kOver33Us ? 1 : 0;
    }
};

// What one closed window reports. `skipped` counts hidden presents that DXGI refused to wait for (hidden DX11 present only).
struct Summary
{
    Accumulator acc;
    uint64_t skipped = 0;
};

// One phase on one thread. The first sample opens the window; the sample that arrives 10 s or more after it closes the window,
// is part of its summary, and the next sample opens a new one. A clock that went backwards restarts the window at that sample.
class Window
{
  public:
    // Records one sample that finished at `nowUs`. Returns true and fills `out` when the window closed.
    bool Add(uint64_t durationUs, int64_t nowUs, Summary& out)
    {
        if (open_ && nowUs < startUs_)
        {
            acc_ = Accumulator {};
            skipped_ = 0;
            open_ = false;
        }
        if (!open_)
        {
            open_ = true;
            startUs_ = nowUs;
        }
        acc_.Add(durationUs);
        if (nowUs - startUs_ < kWindowUs)
            return false;

        out.acc = acc_;
        out.skipped = skipped_;
        acc_ = Accumulator {};
        skipped_ = 0;
        open_ = false;
        return true;
    }

    void NoteSkipped() { ++skipped_; }

  private:
    Accumulator acc_;
    uint64_t skipped_ = 0;
    int64_t startUs_ = 0;
    bool open_ = false;
};

// One open span per thread (begin ... end in different functions). Begin restarts it, End consumes it.
class Span
{
  public:
    void Begin(int64_t nowUs)
    {
        startUs_ = nowUs;
        open_ = true;
    }

    // Forgets an open span (the trace went off while it was open).
    void Cancel() { open_ = false; }

    // True and the duration when a span was open, did not run backwards and is not older than `maxAgeUs` (0 = no limit).
    // The span is closed either way.
    bool End(int64_t nowUs, int64_t maxAgeUs, uint64_t& durationUs)
    {
        if (!open_)
            return false;
        open_ = false;
        if (nowUs < startUs_)
            return false;
        const int64_t d = nowUs - startUs_;
        if (maxAgeUs > 0 && d > maxAgeUs)
            return false;
        durationUs = static_cast<uint64_t>(d);
        return true;
    }

  private:
    int64_t startUs_ = 0;
    bool open_ = false;
};

// "Frame pacing CPU [<phase>] tid <n>: calls N, mean x.xxx ms, max x.xxx ms, >=8ms a, >=16ms b, >=33ms c" and, when asked for,
// ", skipped k". An empty summary does not divide by zero.
inline std::string FormatLine(Phase phase, uint32_t tid, const Summary& s, bool showSkipped)
{
    const double meanMs = s.acc.calls != 0 ? static_cast<double>(s.acc.sumUs) / static_cast<double>(s.acc.calls) / 1000.0 : 0.0;
    const double maxMs = static_cast<double>(s.acc.maxUs) / 1000.0;
    char buf[320];
    int n = std::snprintf(buf, sizeof(buf),
                          "Frame pacing CPU [%s] tid %u: calls %llu, mean %.3f ms, max %.3f ms, >=8ms %llu, >=16ms %llu, >=33ms %llu",
                          PhaseName(phase), static_cast<unsigned>(tid), static_cast<unsigned long long>(s.acc.calls), meanMs, maxMs,
                          static_cast<unsigned long long>(s.acc.over8), static_cast<unsigned long long>(s.acc.over16),
                          static_cast<unsigned long long>(s.acc.over33));
    std::string line(buf, n > 0 ? static_cast<size_t>(n < static_cast<int>(sizeof(buf)) ? n : static_cast<int>(sizeof(buf)) - 1) : 0);
    if (showSkipped)
        line += ", skipped " + std::to_string(s.skipped);
    return line;
}

// Whether the trace is on and why. The rule of the key [FramePacing] Trace:
//   not the FFXIV exe              -> off, whatever the key says (the sites also serve other DX11 games)
//   true                           -> on at any log level
//   false                          -> off
//   unset (auto)                   -> on only with file logging at Debug or Trace (level <= 1), the reference build's rule
enum class Reason
{
    KeyOn,
    DebugLog,
    KeyOff,
    AutoOff,
    NotFfxiv
};

struct Decision
{
    bool on = false;
    Reason reason = Reason::NotFfxiv;
};

inline Decision Decide(std::optional<bool> key, bool logToFile, int logLevel, bool isFfxivExe)
{
    if (!isFfxivExe)
        return { false, Reason::NotFfxiv };
    if (key.has_value())
        return *key ? Decision { true, Reason::KeyOn } : Decision { false, Reason::KeyOff };
    if (logToFile && logLevel <= 1)
        return { true, Reason::DebugLog };
    return { false, Reason::AutoOff };
}

inline std::string ReasonText(Reason r, bool logToFile, int logLevel)
{
    switch (r)
    {
    case Reason::KeyOn:
        return "on (key true)";
    case Reason::DebugLog:
        return "on (Debug log)";
    case Reason::KeyOff:
        return "off (key false)";
    case Reason::AutoOff:
        return logToFile ? "off (key unset, log level " + std::to_string(logLevel) + ")" : std::string("off (key unset, no file log)");
    case Reason::NotFfxiv:
        break;
    }
    return "off (not FFXIV)";
}

// The line the trace logs when its state is first known (and again when it changes).
inline std::string DecisionLine(Reason r, bool logToFile, int logLevel)
{
    return "Frame pacing trace: " + ReasonText(r, logToFile, logLevel);
}
} // namespace ext::present
