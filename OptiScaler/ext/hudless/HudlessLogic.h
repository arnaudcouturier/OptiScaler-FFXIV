#pragma once
// S10 package B: the engine's pure logic (docs/HUDLESS.md sections 5 and 6) -- fallback reasons, the displayed-state
// tracker with hysteresis, the mask-coverage guard, the slot ring with its reader fences and the log line formats.
//
// Header-only and free of D3D, Config, State and the logger so the CPU tests build it with a stub pch.h. The production
// glue (HudlessEngine.cpp) and the decision sequencing (HudlessCore.h) use it.

#include "ext/hudless/HudlessMode.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace ext::hudless
{
// ---- Providers ---------------------------------------------------------------------------------------------------------
enum class Provider : uint8_t
{
    None,
    DlssG, // full engine: tags, recomposition, UI-free base and paste
    FsrFg, // hud-less tag only (untested here)
    XeFg,  // hud-less tag, UI tag only when the SDK/config allows it (untested here)
};

// ---- Fallback reasons (docs/HUDLESS.md section 5), all counted ------------------------------------------------------------
enum class Reason : uint8_t
{
    None = 0, // not a reason: the frame used the hud-less image
    ModeOff,
    NoFrameGeneration,
    ProviderUnsupported,
    NoSource,
    NoBoundaryThisFrame,
    CaptureRejected,
    SizeMismatch,
    FormatUnsupported,
    MsaaOrArray,
    SlotBusy,
    PasteNotReady,
    SceneHdrOwnsFrame,
    MaskCoverage,
    DebugTintOn,
    Count
};
inline constexpr size_t kReasonCount = static_cast<size_t>(Reason::Count);

// The enum name, used in the log lines ("Fallback(MaskCoverage)", "SlotBusy 1")
constexpr const char* ReasonName(Reason reason)
{
    switch (reason)
    {
    case Reason::None:
        return "None";
    case Reason::ModeOff:
        return "ModeOff";
    case Reason::NoFrameGeneration:
        return "NoFrameGeneration";
    case Reason::ProviderUnsupported:
        return "ProviderUnsupported";
    case Reason::NoSource:
        return "NoSource";
    case Reason::NoBoundaryThisFrame:
        return "NoBoundaryThisFrame";
    case Reason::CaptureRejected:
        return "CaptureRejected";
    case Reason::SizeMismatch:
        return "SizeMismatch";
    case Reason::FormatUnsupported:
        return "FormatUnsupported";
    case Reason::MsaaOrArray:
        return "MsaaOrArray";
    case Reason::SlotBusy:
        return "SlotBusy";
    case Reason::PasteNotReady:
        return "PasteNotReady";
    case Reason::SceneHdrOwnsFrame:
        return "SceneHdrOwnsFrame";
    case Reason::MaskCoverage:
        return "MaskCoverage";
    case Reason::DebugTintOn:
        return "DebugTintOn";
    default:
        return "?";
    }
}

// Static text for the menu status line
constexpr const char* ReasonText(Reason reason)
{
    switch (reason)
    {
    case Reason::None:
        return "using the hud-less image";
    case Reason::ModeOff:
        return "mode is off";
    case Reason::NoFrameGeneration:
        return "frame generation is not active";
    case Reason::ProviderUnsupported:
        return "this frame generation provider does not support the selected mode";
    case Reason::NoSource:
        return "no UI boundary detector for this game";
    case Reason::NoBoundaryThisFrame:
        return "no UI boundary seen in this frame";
    case Reason::CaptureRejected:
        return "the UI-free capture was rejected or could not be staged";
    case Reason::SizeMismatch:
        return "the capture size differs from the frame generation backbuffer";
    case Reason::FormatUnsupported:
        return "the capture format cannot be used (HDR10 output, or a format the backbuffer does not share)";
    case Reason::MsaaOrArray:
        return "the capture is multisampled or an array texture";
    case Reason::SlotBusy:
        return "the GPU is still reading the previous hud-less image of this slot";
    case Reason::PasteNotReady:
        return "no UI paste image has completed yet";
    case Reason::SceneHdrOwnsFrame:
        return "scene HDR or an HDR screenshot owns this frame";
    case Reason::MaskCoverage:
        return "the UI mask covers too much of the screen (fade, or a preset changing the capture)";
    case Reason::DebugTintOn:
        return "the Show Detected UI compare view is on";
    default:
        return "";
    }
}

// ---- Displayed state ---------------------------------------------------------------------------------------------------
enum class Phase : uint8_t
{
    Off,       // mode off, frame generation inactive or no source
    Searching, // armed, no capture yet
    Active,    // >= 90 % of the last 60 presents delivered a usable capture
    Fallback,  // armed but not delivering; the dominant reason says why
};

constexpr const char* PhaseName(Phase phase)
{
    switch (phase)
    {
    case Phase::Searching:
        return "Searching";
    case Phase::Active:
        return "Active";
    case Phase::Fallback:
        return "Fallback";
    default:
        return "Off";
    }
}

// "Active", "Fallback(MaskCoverage)"
inline std::string StateText(Phase phase, Reason reason)
{
    if (phase == Phase::Fallback)
        return std::format("Fallback({})", ReasonName(reason));
    return PhaseName(phase);
}

// Hysteresis for the displayed state only (the per-frame decision does not look at it). The window is the last
// kWindow armed presents; a present is "captured" when it used the hud-less image.
//   Searching  -> Active    once >= kEnter of the window captured (at least kMinSamples presents seen)
//   Searching  -> Fallback  after kSearchFrames armed presents without a single capture
//   Active     -> Fallback  when the captured share drops below kLeave
//   Fallback   -> Active    when the share is back at kEnter
//   any        -> Off       immediately when the mode is off, after kOffAfter consecutive unarmed presents otherwise
class StateTracker
{
  public:
    static constexpr int kWindow = 60;
    static constexpr float kEnter = 0.90f;
    static constexpr float kLeave = 0.75f;
    static constexpr int kMinSamples = 10;
    static constexpr int kSearchFrames = 60;
    static constexpr int kOffAfter = 10;

    void Reset()
    {
        *this = StateTracker {};
    }

    // Mode off: immediate, forgets everything.
    void SetOff()
    {
        Reset();
    }

    // One present. `armed` = mode on, frame generation active, source present. `reason` is the first failure of the
    // present (None = it used the hud-less image). Returns true when the displayed state (phase or reason) changed.
    bool Observe(bool armed, Reason reason)
    {
        const Phase before = _phase;
        const Reason beforeReason = Dominant();

        if (!armed)
        {
            if (++_unarmedRun >= kOffAfter && _phase != Phase::Off)
            {
                const bool changed = true;
                Reset();
                _unarmedRun = kOffAfter;
                return changed;
            }
            return false;
        }

        _unarmedRun = 0;
        Push(reason);
        UpdateSticky();

        if (_phase == Phase::Off)
            _phase = Phase::Searching;

        const float share = Share();
        if (reason == Reason::None)
            _everCaptured = true;

        switch (_phase)
        {
        case Phase::Searching:
        case Phase::Fallback:
            if (_everCaptured && _count >= kMinSamples && share >= kEnter)
                _phase = Phase::Active;
            else if (_phase == Phase::Searching && !_everCaptured && _armedFrames >= kSearchFrames)
                _phase = Phase::Fallback;
            else if (_phase == Phase::Searching && _everCaptured && _count >= kMinSamples && share < kEnter)
                _phase = Phase::Fallback;
            break;
        case Phase::Active:
            if (_count >= kMinSamples && share < kLeave)
                _phase = Phase::Fallback;
            break;
        default:
            break;
        }

        return _phase != before || (_phase == Phase::Fallback && Dominant() != beforeReason);
    }

    Phase GetPhase() const { return _phase; }
    // The failure the displayed Fallback state names: the most frequent one in the window, but sticky -- it only switches
    // when another reason leads by kSwitchMargin, so two alternating reasons do not flap the state line.
    Reason Dominant() const { return _sticky; }
    float Share() const { return _count == 0 ? 0.f : static_cast<float>(_counts[0]) / static_cast<float>(_count); }
    int Samples() const { return _count; }

  private:
    static constexpr int kSwitchMargin = 6;

    void UpdateSticky()
    {
        Reason best = Reason::None;
        int bestCount = 0;
        for (size_t i = 1; i < kReasonCount; ++i)
        {
            const int c = _counts[i];
            if (c > bestCount || (c == bestCount && c > 0 && static_cast<Reason>(i) == _latestReason))
            {
                best = static_cast<Reason>(i);
                bestCount = c;
            }
        }
        if (best == Reason::None)
            _sticky = Reason::None;
        else if (_sticky == Reason::None || _counts[static_cast<size_t>(_sticky)] == 0 ||
                 bestCount >= _counts[static_cast<size_t>(_sticky)] + kSwitchMargin)
            _sticky = best;
    }

    void Push(Reason reason)
    {
        ++_armedFrames;
        if (_count == kWindow)
        {
            --_counts[static_cast<size_t>(_ring[_head])];
            --_count;
        }
        _ring[_head] = reason;
        _head = (_head + 1) % kWindow;
        ++_counts[static_cast<size_t>(reason)];
        ++_count;
        if (reason != Reason::None)
            _latestReason = reason;
    }

    std::array<Reason, kWindow> _ring {};
    std::array<int, kReasonCount> _counts {};
    int _head = 0;
    int _count = 0;
    int _armedFrames = 0;
    int _unarmedRun = 0;
    bool _everCaptured = false;
    Phase _phase = Phase::Off;
    Reason _latestReason = Reason::None;
    Reason _sticky = Reason::None;
};

// ---- Mask coverage guard (docs/HUDLESS.md section 5) --------------------------------------------------------------
// Black/white fades, and presets that change pixels before the capture, make the mask cover most of the screen. The
// guard falls back after kTripFrames consecutive frames over the limit and retries after kRetryFrames. Coverage < 0 is
// "unknown" (no readback completed) and never blocks; kUnknownFrames of it in a row reads as "unknown" for the log.
// After a retry the old readbacks are stale, so samples are ignored until kReadbackLag new paste images were produced.
class CoverageGuard
{
  public:
    static constexpr int kTripFrames = 5;
    static constexpr int kRetryFrames = 120;
    static constexpr int kUnknownFrames = 8;
    static constexpr uint64_t kReadbackLag = 3;

    // limit 0 = guard off
    void Configure(float limit)
    {
        _limit = limit;
        if (_limit <= 0.f)
            Reset();
    }

    void Reset()
    {
        _over = 0;
        _unknownRun = 0;
        _tripped = false;
        _trippedAt = 0;
        _baseProduced = 0;
        _baseSet = false;
    }

    // Once per armed present, before the decision. `frame` counts armed presents, `produced` is
    // PasteStatus::produced, `coverage` the newest completed readback (0..1, < 0 unknown).
    void Update(uint64_t frame, float coverage, uint64_t produced)
    {
        if (_limit <= 0.f)
            return;

        if (!_baseSet)
        {
            _baseProduced = produced;
            _baseSet = true;
        }

        if (_tripped)
        {
            if (frame - _trippedAt >= static_cast<uint64_t>(kRetryFrames))
            {
                _tripped = false;
                _over = 0;
                _unknownRun = 0;
                _baseProduced = produced; // readbacks before the retry are stale
            }
            return;
        }

        if (coverage < 0.f || produced < _baseProduced + kReadbackLag)
        {
            ++_unknownRun;
            return;
        }

        _unknownRun = 0;
        if (coverage > _limit)
        {
            if (++_over >= kTripFrames)
            {
                _tripped = true;
                _trippedAt = frame;
                ++_trips;
            }
        }
        else
        {
            _over = 0;
        }
    }

    bool Blocking() const { return _tripped; }
    bool Unknown() const { return _limit > 0.f && _unknownRun >= kUnknownFrames; }
    int Trips() const { return _trips; }

  private:
    float _limit = 0.f;
    int _over = 0;
    int _unknownRun = 0;
    int _trips = 0;
    bool _tripped = false;
    bool _baseSet = false;
    uint64_t _trippedAt = 0;
    uint64_t _baseProduced = 0;
};

// ---- Slot ring with reader fences (docs/HUDLESS.md section 6) --------------------------------------------------------
// A slot is written by the D3D11 side only when every D3D12 queue operation that reads it has COMPLETED. The check is
// a non-blocking GetCompletedValue; there is deliberately no wait anywhere in this class. A fence is identified by an
// opaque pointer; `Completed` is the engine's GetCompletedValue (UINT64_MAX = device removed = never "completed").
struct FenceReader
{
    // Completed value of `fence` (D3D12: ID3D12Fence::GetCompletedValue). UINT64_MAX means the device was removed.
    virtual uint64_t Completed(const void* fence) = 0;

  protected:
    ~FenceReader() = default;
};

template <size_t kSlots, size_t kMaxReaders = 6> class SlotRing
{
  public:
    // A reader entry whose fence value has not completed after this many frames is assumed to belong to work that was
    // never submitted (a paused frame generator drops its pending UI list) and stops pinning the slot. Counted.
    static constexpr uint64_t kStaleFrames = 240;

    // Records that the queue operation signalled by (fence, value) reads `slot`. Same fence: keeps the larger value.
    // False when the slot already tracks kMaxReaders other fences (the caller treats the slot as unusable).
    bool AddReader(size_t slot, const void* fence, uint64_t value, uint64_t frame)
    {
        if (slot >= kSlots || fence == nullptr)
            return false;
        auto& s = _slots[slot];
        for (size_t i = 0; i < s.count; ++i)
        {
            if (s.readers[i].fence == fence)
            {
                s.readers[i].value = std::max(s.readers[i].value, value);
                s.readers[i].frame = frame;
                return true;
            }
        }
        if (s.count == kMaxReaders)
            return false;
        s.readers[s.count++] = { fence, value, frame };
        return true;
    }

    // True while any recorded read has not completed. Entries that completed are dropped; stale ones are dropped and
    // counted.
    bool Busy(size_t slot, uint64_t frame, FenceReader& fences)
    {
        if (slot >= kSlots)
            return true;
        auto& s = _slots[slot];
        bool busy = false;
        for (size_t i = 0; i < s.count;)
        {
            const uint64_t done = fences.Completed(s.readers[i].fence);
            if (done != UINT64_MAX && done >= s.readers[i].value)
            {
                s.readers[i] = s.readers[--s.count];
                continue;
            }
            if (frame >= s.readers[i].frame && frame - s.readers[i].frame >= kStaleFrames)
            {
                ++_staleDrops;
                s.readers[i] = s.readers[--s.count];
                continue;
            }
            busy = true;
            ++i;
        }
        return busy;
    }

    // First free slot starting at `hint` (round robin), or -1 when every slot is still being read.
    int Acquire(size_t hint, uint64_t frame, FenceReader& fences)
    {
        for (size_t n = 0; n < kSlots; ++n)
        {
            const size_t slot = (hint + n) % kSlots;
            if (!Busy(slot, frame, fences))
                return static_cast<int>(slot);
        }
        return -1;
    }

    // Number of fences still pinning the slot (before any completion check)
    size_t Readers(size_t slot) const { return slot < kSlots ? _slots[slot].count : 0; }
    // The i-th recorded reader of the slot (for retiring the slot's resources behind exactly these fences)
    bool Reader(size_t slot, size_t i, const void*& fence, uint64_t& value) const
    {
        if (slot >= kSlots || i >= _slots[slot].count)
            return false;
        fence = _slots[slot].readers[i].fence;
        value = _slots[slot].readers[i].value;
        return true;
    }
    // Drops every record of the slot (its resources were retired on their own fences)
    void Clear(size_t slot)
    {
        if (slot < kSlots)
            _slots[slot].count = 0;
    }
    void ClearAll()
    {
        for (auto& s : _slots)
            s.count = 0;
    }
    uint64_t StaleDrops() const { return _staleDrops; }

  private:
    struct ReaderEntry
    {
        const void* fence = nullptr;
        uint64_t value = 0;
        uint64_t frame = 0;
    };
    struct Slot
    {
        std::array<ReaderEntry, kMaxReaders> readers {};
        size_t count = 0;
    };
    std::array<Slot, kSlots> _slots {};
    uint64_t _staleDrops = 0;
};

// ---- Log lines (docs/HUDLESS.md section 5) ------------------------------------------------------------------------------
struct WindowStats
{
    uint64_t presents = 0;
    uint64_t captured = 0;
    std::array<uint64_t, kReasonCount> reasons {};
    std::array<uint32_t, kReasonCount> firstSeen {}; // 1-based order in which the reasons first occurred in the window
    uint32_t seenCount = 0;
    double coverageSum = 0.0;
    uint64_t coverageSamples = 0;
    float coverageMax = 0.f;
    uint64_t slotWaits = 0; // blocking waits on the game thread (only the stub-free design has none; counted for the log)
};

// Counts one fallback in the window
inline void NoteReason(WindowStats& w, Reason reason)
{
    const size_t i = static_cast<size_t>(reason);
    if (w.reasons[i]++ == 0)
        w.firstSeen[i] = ++w.seenCount;
}

// "SlotBusy 1, NoBoundaryThisFrame 1": by count descending, ties in the order the reasons first occurred
inline std::string FallbackBreakdown(const WindowStats& w)
{
    std::array<size_t, kReasonCount> order {};
    size_t n = 0;
    for (size_t i = 1; i < kReasonCount; ++i)
        if (w.reasons[i] > 0)
            order[n++] = i;
    std::stable_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(n),
                     [&](size_t a, size_t b)
                     { return w.reasons[a] != w.reasons[b] ? w.reasons[a] > w.reasons[b] : w.firstSeen[a] < w.firstSeen[b]; });
    std::string out;
    for (size_t k = 0; k < n; ++k)
    {
        if (k != 0)
            out += ", ";
        out += std::format("{} {}", ReasonName(static_cast<Reason>(order[k])), w.reasons[order[k]]);
    }
    return out;
}

// HudLess: mode ui_free, state Active; 600 presents: 598 captured, 2 fallbacks (SlotBusy 1, NoBoundaryThisFrame 1);
// mask coverage avg 4.1% max 9.7%; paste built 598 pasted 1190 skipped 6; slot waits 0; boundary: ok
inline std::string SummaryLine(Mode mode, Phase phase, Reason reason, const WindowStats& w, uint64_t pasteBuilt,
                               uint64_t pastePasted, uint64_t pasteSkipped, const std::string& boundary)
{
    uint64_t fallbacks = 0;
    for (size_t i = 1; i < kReasonCount; ++i)
        fallbacks += w.reasons[i];

    std::string line = std::format("HudLess: mode {}, state {}; {} presents: {} captured, {} fallbacks", ModeName(mode),
                                   StateText(phase, reason), w.presents, w.captured, fallbacks);
    if (fallbacks > 0)
        line += std::format(" ({})", FallbackBreakdown(w));

    if (w.coverageSamples > 0)
        line += std::format("; mask coverage avg {:.1f}% max {:.1f}%",
                            100.0 * w.coverageSum / static_cast<double>(w.coverageSamples),
                            100.0 * static_cast<double>(w.coverageMax));
    else
        line += "; mask coverage n/a";

    line += std::format("; paste built {} pasted {} skipped {}; slot waits {}; boundary: {}", pasteBuilt, pastePasted,
                        pasteSkipped, w.slotWaits, boundary);
    return line;
}

// HudLess: state Searching -> Active   |   HudLess: state Active -> Fallback(MaskCoverage)
inline std::string StateChangeLine(Phase from, Reason fromReason, Phase to, Reason toReason)
{
    return std::format("HudLess: state {} -> {}", StateText(from, fromReason), StateText(to, toReason));
}

inline std::string ModeLine(Mode mode) { return std::format("HudLess: mode {}", ModeName(mode)); }
inline std::string ModeChangeLine(Mode from, Mode to)
{
    return std::format("HudLess: mode {} -> {}", ModeName(from), ModeName(to));
}
} // namespace ext::hudless
