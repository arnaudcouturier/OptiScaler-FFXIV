#pragma once
// S10 package B: the engine's pure logic (docs/HUDLESS.md sections 5 and 6) -- fallback reasons, the displayed-state
// tracker with hysteresis, the mask-coverage guard, the slot ring with its reader fences and the log line formats.
//
// Header-only and free of D3D, Config, State and the logger so the CPU tests build it with a stub pch.h. The production
// glue (HudlessEngine.cpp) and the decision sequencing (HudlessCore.h) use it.

#include "ext/hudless/HudlessMode.h"

#include <algorithm>
#include <array>
#include <bit>
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
    PausedByPreset,
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
    case Reason::PausedByPreset:
        return "PausedByPreset";
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
    case Reason::PausedByPreset:
        return "paused by the DLSS-G render preset: it turns NVIDIA's UI recomposition off (use Auto or B, or ui_free)";
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
// guard TRIPS after kTripFrames consecutive fresh readbacks over the limit and RELEASES after kReleaseSamples consecutive
// fresh readbacks at or below the release level, limit * kReleaseFraction. While it is tripped the engine does not swap
// the frame generation base (it "probes": the capture and the paste image keep being built, so the readbacks that decide
// the release keep arriving). Samples between the release level and the limit hold the tripped state.
//
// It is driven by PasteStatus::coverageSequence, the ordinal of the paste image the published coverage was read back
// from, so it can tell a FRESH readback from the same stale value read again:
//   - only a new sequence counts as a sample (one per completed readback, which is one per real frame while the producer
//     keeps up); a present without a new sequence neither adds to nor resets the over-limit streak;
//   - "unknown" = kUnknownFrames presents in a row without a new sequence (nothing completed a readback, or the producer
//     stopped producing): it never trips, and the over-limit streak does not survive it;
//   - STARVATION: while tripped, kStarveFrames probed presents without a fresh sample release the guard (fail open: no
//     readbacks means nothing can ever release it otherwise). Presents that did not take a capture are not probed and do
//     not count, so a long loading screen cannot release it;
//   - the sequence seen when the guard starts is stale: a readback left over from an earlier session of the mode is not
//     counted.
// Coverage < 0 or sequence 0 is "unknown yet" and is never a sample.
class CoverageGuard
{
  public:
    static constexpr int kTripFrames = 5;
    static constexpr int kReleaseSamples = 30;
    static constexpr float kReleaseFraction = 0.75f;
    static constexpr int kStarveFrames = 120;
    static constexpr int kUnknownFrames = 8;
    // The density policy ([HudLess] GuardMode = density): trips on kDensityTripBad bad samples among the last kDensityWindow fresh ones
    // (which already covers kDensityTripBad in a row), releases after kDensityReleaseSamples clean ones in a row. Everything else is as above.
    static constexpr int kDensityWindow = 12;
    static constexpr int kDensityTripBad = 3;
    static constexpr int kDensityReleaseSamples = 60;
    // Time limit of the density policy ([HudLess] GuardReleaseHolds, 0 = off): after this many anchor-later presents in a row, each with no
    // sample over the release level since the previous one, the guard lets go although the clean run cannot complete. One summary window
    // (Core::kSummaryPresents) is the upper bound a key value is clamped to.
    static constexpr int kMaxReleaseHolds = kGuardReleaseHoldsMax;

    // limit 0 = guard off. A different policy, release limit or per-frame trip starts the guard over.
    void Configure(float limit, GuardPolicy policy = GuardPolicy::Legacy, int releaseHolds = 0, bool tripOnAnchorLater = false)
    {
        releaseHolds = releaseHolds < 0 ? 0 : (releaseHolds > kMaxReleaseHolds ? kMaxReleaseHolds : releaseHolds);
        _limit = limit;
        _release = limit * kReleaseFraction;
        if (policy != _policy || releaseHolds != _releaseHolds || tripOnAnchorLater != _tripOnAnchorLater)
        {
            _policy = policy;
            _releaseHolds = releaseHolds;
            _tripOnAnchorLater = tripOnAnchorLater;
            Reset();
        }
        if (_limit <= 0.f)
            Reset();
    }

    void Reset()
    {
        _over = 0;
        _clean = 0;
        _starve = 0;
        _unknownRun = 0;
        _tripped = false;
        _seenSequence = 0;
        _baseSet = false;
        _window = 0;
        _releaseDue = false;
        _holdRun = 0;
        _holdWindowClean = true;
    }

    // Once per armed present, before the decision. `coverage` is PasteStatus::coverage (0..1, < 0 unknown),
    // `coverageSequence` PasteStatus::coverageSequence, `probedLastPresent` tells that the previous present took a capture
    // while the guard was tripped (it produced a paste image, so a readback was due).
    void Update(float coverage, uint64_t coverageSequence, bool probedLastPresent = false)
    {
        if (_limit <= 0.f)
            return;

        if (!_baseSet)
        {
            _seenSequence = coverageSequence; // whatever was read back before the guard started is stale
            _baseSet = true;
        }

        if (coverage < 0.f || coverageSequence == 0 || coverageSequence == _seenSequence)
        {
            if (_unknownRun < kUnknownFrames)
                ++_unknownRun;
            if (_unknownRun >= kUnknownFrames)
            {
                _over = 0;
                _window = 0; // the density window does not survive it either
            }
            if (_tripped && probedLastPresent && ++_starve >= kStarveFrames)
                ClearTrip();
            return;
        }

        _seenSequence = coverageSequence;
        _unknownRun = 0;
        _starve = 0;

        if (_tripped)
        {
            if (coverage > _release)
            {
                _clean = 0; // still too much UI area, including the band up to the limit: stay tripped
                _releaseDue = false;
                _holdWindowClean = false; // such a window does not count towards GuardReleaseHolds
            }
            else if (_policy == GuardPolicy::Density)
            {
                // The clean run is complete; the release itself waits for the boundary's answer for this present (ResolveRelease)
                if (++_clean >= kDensityReleaseSamples)
                    _releaseDue = true;
            }
            else if (++_clean >= kReleaseSamples)
                ClearTrip();
            return;
        }

        if (_policy == GuardPolicy::Density)
        {
            const bool bad = coverage > _limit;
            _window = static_cast<uint16_t>(((_window << 1) | (bad ? 1u : 0u)) & ((1u << kDensityWindow) - 1u));
            if (std::popcount(static_cast<unsigned>(_window)) >= kDensityTripBad)
                Trip();
            return;
        }

        if (coverage > _limit)
        {
            if (++_over >= kTripFrames)
                Trip();
        }
        else
        {
            _over = 0;
        }
    }

    // Density policy, once per present after the boundary was asked (or at the end of a present that never got that far, with false):
    // `anchorLater` = this present's frame was anchored after an off-screen UI draw. While tripped such a present keeps the fallback at
    // once, without waiting for its own readback (two presents later), and restarts the clean run. Otherwise a completed clean run
    // releases the guard.
    //   - Not tripped and GuardTripOnAnchorLater: such a present trips the guard itself, so the base of this very frame is not swapped and
    //     its image never reaches the outputs. The trip then ends by the normal release rules, the same as a trip the samples caused.
    //   - Tripped and GuardReleaseHolds = N: the hold counts when no sample since the previous hold was over the release level; the N-th such
    //     hold in a row releases (the time limit for a stream of such frames whose images measure clean, where the clean run never completes).
    // The legacy policy releases inside Update and ignores the fact.
    void ResolveRelease(bool anchorLater)
    {
        if (_policy != GuardPolicy::Density)
            return;
        if (!_tripped)
        {
            if (anchorLater && _tripOnAnchorLater && _limit > 0.f)
                Trip();
            return;
        }
        if (anchorLater)
        {
            _clean = 0;
            _releaseDue = false;
            ++_holds;
            _holdRun = _holdWindowClean ? _holdRun + 1 : 0;
            _holdWindowClean = true; // the window after this hold starts empty
            if (_releaseHolds > 0 && _holdRun >= _releaseHolds)
                ClearTrip(true);
        }
        else if (_releaseDue)
            ClearTrip();
    }

    // True while tripped: the engine must not swap the frame generation base.
    bool Blocking() const { return _tripped; }
    bool Unknown() const { return _limit > 0.f && _unknownRun >= kUnknownFrames; }
    int Trips() const { return _trips; }
    // Every end of a trip: the clean run, the release limit of GuardReleaseHolds, or the starvation release.
    int Releases() const { return _releases; }
    // Of Releases(), those the release limit of GuardReleaseHolds ended.
    int HoldReleases() const { return _holdReleases; }
    // Presents on which an anchor-later frame held a tripped density guard (it restarted the clean run).
    int Holds() const { return _holds; }

  private:
    void Trip()
    {
        _tripped = true;
        _clean = 0;
        _starve = 0;
        _releaseDue = false;
        ++_trips;
    }

    void ClearTrip(bool byReleaseLimit = false)
    {
        ++_releases;
        if (byReleaseLimit)
            ++_holdReleases;
        _tripped = false;
        _over = 0;
        _clean = 0;
        _starve = 0;
        _window = 0;
        _releaseDue = false;
        _holdRun = 0;
        _holdWindowClean = true;
    }

    float _limit = 0.f;
    float _release = 0.f;
    int _over = 0;
    int _clean = 0;
    int _starve = 0;
    int _unknownRun = 0;
    int _trips = 0;
    int _releases = 0;
    int _holdReleases = 0;
    int _holds = 0;
    int _releaseHolds = 0;           // density: GuardReleaseHolds, 0 = off
    int _holdRun = 0;                // density: clean hold windows in a row towards _releaseHolds
    bool _holdWindowClean = true;    // density: no sample over the release level since the last hold or trip
    bool _tripOnAnchorLater = false; // density: GuardTripOnAnchorLater
    GuardPolicy _policy = GuardPolicy::Legacy;
    uint16_t _window = 0;     // density: the last kDensityWindow fresh samples, bit 0 the newest, 1 = over the limit
    bool _releaseDue = false; // density: the clean run is complete, the boundary's answer for the present decides
    bool _tripped = false;
    bool _baseSet = false;
    uint64_t _seenSequence = 0;
};

// The one line written when the density guard becomes active: the numbers it uses, then the two opt-in keys when they are on.
inline std::string DensityGuardLine(int releaseHolds, bool tripOnAnchorLater)
{
    std::string line = std::format("HudLess: coverage guard: density is active (trips on {} bad of the last {} samples, releases after {} "
                                   "clean ones in a row; a frame anchored after off-screen UI draws keeps it tripped",
                                   CoverageGuard::kDensityTripBad, CoverageGuard::kDensityWindow, CoverageGuard::kDensityReleaseSamples);
    if (releaseHolds > 0)
        line += std::format(", or after {} clean holds in a row", releaseHolds);
    if (tripOnAnchorLater)
        line += ", and such a frame trips it at once";
    line += ")";
    return line;
}

// ---- The HDR stage before the paste producer (docs/HUDLESS.md section 8) -------------------------------------------------------------
// Under HDR output the producer builds its image from a PQ copy of the final frame, which needs the slot's HDR stage. The readiness
// check creates the stage only after a finished image exists, so on a present that is not probing nothing created it before the first
// image and the producer found none (no image was ever built; inferred from reading the code, never observed in a game). Probing presents did create it first. [HudLess] HdrPaste = on creates
// it before the producer on every HDR present; off keeps the old rule, because that path (the PQ conversion of the final frame) has not
// run in a game yet.
constexpr bool EnsureHdrStageBeforeProduce(bool hdrOutput, bool probing, bool hdrPaste)
{
    return hdrOutput && (probing || hdrPaste);
}

// The producer step of one present that produces: the stage first when the rule says so (a stage that cannot be made stops the
// present), then the paste image. False when either failed.
template <typename EnsureStage, typename Produce>
bool ProduceWithHdrStage(bool hdrOutput, bool probing, bool hdrPaste, EnsureStage&& ensureStage, Produce&& produce)
{
    const bool stageReady = !EnsureHdrStageBeforeProduce(hdrOutput, probing, hdrPaste) || ensureStage();
    return stageReady && produce();
}

// A stage that cannot be made is not made again on every present (each try recreates resources and writes an error): with HdrPaste = on
// the failure is latched until the HDR output state or the setting changes (or the ring is dropped: Clear). With off nothing is latched.
class HdrStageLatch
{
  public:
    // The key of the latch: a different HDR output state or setting is a new situation, so the failure is forgotten. Called on every
    // present that produces (SDR ones too, otherwise a switch to SDR and back would go unseen).
    void Observe(bool hdrOutput, bool hdrPaste)
    {
        if (_known && (hdrOutput != _hdrOutput || hdrPaste != _hdrPaste))
            _failed = false;
        _hdrOutput = hdrOutput;
        _hdrPaste = hdrPaste;
        _known = true;
    }
    bool MayTry(bool hdrOutput, bool hdrPaste)
    {
        Observe(hdrOutput, hdrPaste);
        return !_failed;
    }
    // True for the first failure since the latch was clear (the caller writes its one line then)
    bool NoteFailure(bool hdrOutput, bool hdrPaste)
    {
        Observe(hdrOutput, hdrPaste);
        const bool first = !_failed;
        _failed = true;
        return first;
    }
    void Clear() { _failed = false; }

  private:
    bool _known = false;
    bool _hdrOutput = false;
    bool _hdrPaste = false;
    bool _failed = false;
};

// Creates the stage for the producer: with hdrPaste off exactly the plain call (the old behaviour, retried whenever it is reached); with
// on a failed creation is latched, so the next presents do not try again, and `onLatched` runs once for it.
template <typename Ensure, typename OnLatched>
bool EnsureStageLatched(HdrStageLatch& latch, bool hdrOutput, bool hdrPaste, Ensure&& ensure, OnLatched&& onLatched)
{
    if (!hdrPaste)
        return ensure();
    if (!latch.MayTry(hdrOutput, hdrPaste))
        return false;
    if (ensure())
        return true;
    if (latch.NoteFailure(hdrOutput, hdrPaste))
        onLatched();
    return false;
}

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
    // Counters that only watch (they never feed a decision), at the end of the summary line
    uint32_t guardTrips = 0;     // times the coverage guard tripped in the window
    uint32_t guardReleases = 0;  // times it ended a trip (clean run, release limit or starvation)
    uint32_t guardHoldReleases = 0; // of those, the ones GuardReleaseHolds' release limit ended
    uint64_t swappedBad = 0;     // swapped bases whose own paste image measured over the limit (counted when its readback arrives)
    uint64_t anchorFrames = 0;   // presents whose frame was anchored after an off-screen UI draw (the boundary's own predicate)
    uint64_t anchorTakes = 0;    // presents that asked the boundary for a frame (what the share is taken of)
    uint32_t anchorLongest = 0;  // longest run of consecutive such presents, inside the window
    uint64_t tagChanges = 0;     // presents whose set of tags sent to the frame generator differs from the previous present's
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
// mask coverage avg 4.1% max 9.7%; paste built 598 pasted 1190 skipped 6; slot waits 0; boundary: ok; guard trips 0 releases 0
// (0 by release limit); swapped bad frames 0; anchor-later 74 of 598 frames (12.4%), longest run 31; tag changes 2
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

    const double share = w.anchorTakes == 0 ? 0.0 : 100.0 * static_cast<double>(w.anchorFrames) / static_cast<double>(w.anchorTakes);
    line += std::format("; guard trips {} releases {} ({} by release limit); swapped bad frames {}; anchor-later {} of {} frames ({:.1f}%), "
                        "longest run {}; tag changes {}",
                        w.guardTrips, w.guardReleases, w.guardHoldReleases, w.swappedBad, w.anchorFrames, w.anchorTakes, share,
                        w.anchorLongest, w.tagChanges);
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
