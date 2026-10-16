#pragma once
// S10 package B: the per-present decision sequence of the engine (docs/HUDLESS.md section 5), written against a small host
// interface so the CPU tests can drive it with a fake IHudlessSource and a fake provider / GPU. The production host
// (HudlessEngine.cpp) implements IEngineHost with the bridge, the frame generator and D3D.
//
// One present, in order (the seams of docs/HUDLESS.md 4.3 call these):
//   PreInteropCopy  -> Core::BeginPresent   early reasons, coverage guard, Take(), capture checks, slot + copy
//   UiFreeFrame     -> Core::DecideBase     paste ready? scene HDR owns the frame? -> swap the FG base
//   PostInteropCopy -> (tags, Produce / Invalidate in the host), Core::Fail on a late failure, Core::EndPresent
// Every present with a mode other than off ends in exactly one of "captured" or one fallback reason (the first failure).

#include "ext/hudless/HudlessLogic.h"
#include "ext/hudless/HudlessSource.h"

#include <string>

namespace ext::hudless
{
// Paste producer counters of the present, read once from UiPaste::Snapshot() by the host.
struct PasteCounters
{
    uint64_t produced = 0;
    uint64_t pasted = 0;
    uint64_t skipped = 0;
    float coverage = -1.f; // newest completed readback, < 0 unknown
};

struct IEngineHost
{
    virtual ~IEngineHost() = default;

    // Frame generation exists, is active and not paused (and enabled in the config).
    virtual bool FrameGenActive() = 0;
    virtual Provider FrameGenProvider() = 0;
    // The bridge's FG swapchain carries OptiHDR's PQ output.
    virtual bool HdrOutput() = 0;
    // Upstream's "Show Detected UI" compare view (State::fgHudlessCompare): it tints what DLSS-G receives, so no paste.
    virtual bool DebugCompareOn() = 0;
    // [HudLess] CoverageGuard, 0 = off.
    virtual float CoverageLimit() = 0;
    virtual IHudlessSource* Source() = 0;
    // Size / format / sample count of the capture against the bridge's backbuffer: None, SizeMismatch,
    // FormatUnsupported, MsaaOrArray or CaptureRejected.
    virtual Reason CheckCapture(const FrameImage& image) = 0;
    // Takes a free slot (never waits) and copies the capture into its shared texture on the D3D11 context: None,
    // SlotBusy or CaptureRejected.
    virtual Reason StageCapture(const FrameImage& image) = 0;
    // A completed paste image exists that matches the staged capture (UiPaste::Ready).
    virtual bool PasteReady() = 0;
    // ISceneHdr supplies a scene for this frame (or an HDR screenshot is pending): the HDR code keeps the frame.
    virtual bool SceneHdrOwnsFrame() = 0;
    virtual void Log(const std::string& line) = 0;
};

// What the paste producer does in PostInteropCopy: exactly one of Produce / Invalidate per ui_free present.
//   Produce     a capture is staged. Also on PasteNotReady frames (otherwise Ready() would never become true) and on every
//               frame whose base was swapped (the UI must come back through the paste).
//   Invalidate  no usable pair this present (any early fallback), or scene HDR owns the frame: stop pasting, the normal
//               base carries its own UI. Never chosen for a swapped base.
enum class PasteAction : uint8_t
{
    None, // not ui_free
    Produce,
    Invalidate,
};

// Plain copy of the engine state for the menu and the tests
struct Status
{
    Mode mode = Mode::Off;
    Phase phase = Phase::Off;
    Reason reason = Reason::None;      // dominant fallback reason in the displayed window (Phase::Fallback)
    Reason lastReason = Reason::None;  // first failure of the last present, None = it used the hud-less image
    uint64_t presents = 0;             // presents with a mode other than off, since start
    uint64_t captured = 0;
    std::array<uint64_t, kReasonCount> reasons {};
    float coverage = -1.f;             // newest completed readback
    bool coverageUnknown = false;      // ui_free, no readback completed for kUnknownFrames presents
    bool coverageTripped = false;
    PasteCounters paste;
    std::string boundary;              // last Take(): "ok", "not detected", the source's reason, "no source"
};

class Core
{
  public:
    static constexpr uint64_t kSummaryPresents = 600;

    explicit Core(IEngineHost& host) : _host(host) {}

    // PreInteropCopy. True when a capture is staged in a slot for this present.
    bool BeginPresent(Mode mode, const PasteCounters& paste)
    {
        _staged = false;
        _swapped = false;
        _reason = Reason::None;
        _mode = mode;
        _paste = paste;

        if (!_announced)
        {
            _announced = true;
            _lastMode = mode;
            _host.Log(ModeLine(mode));
        }
        else if (mode != _lastMode)
        {
            _host.Log(ModeChangeLine(_lastMode, mode));
            _lastMode = mode;
            _tracker.SetOff();
            _guard.Reset();
            _shownPhase = Phase::Off;
            _shownReason = Reason::None;
            _baseline = paste;
            _window = {};
            _sinceSummary = 0;
        }

        if (mode == Mode::Off)
        {
            _active = false;
            _reason = Reason::ModeOff;
            return false;
        }
        _active = true;
        ++_presents;

        if (mode == Mode::UiFree)
        {
            _guard.Configure(_host.CoverageLimit());
            _guard.Update(_presents, paste.coverage, paste.produced);
            if (paste.coverage >= 0.f && paste.produced != _lastSampledProduced)
            {
                _lastSampledProduced = paste.produced;
                _window.coverageSum += static_cast<double>(paste.coverage);
                ++_window.coverageSamples;
                _window.coverageMax = std::max(_window.coverageMax, paste.coverage);
            }
        }

        if (!_host.FrameGenActive())
            return Fail(Reason::NoFrameGeneration);

        const Provider provider = _host.FrameGenProvider();
        if (provider == Provider::None || (mode == Mode::UiFree && provider != Provider::DlssG))
            return Fail(Reason::ProviderUnsupported);

        // Recomposition would need a PQ UI image DLSS-G refuses here, and a hud-less tag converted to PQ: only ui_free
        // is carried over HDR10 output (docs/HUDLESS.md section 8).
        if (mode == Mode::Recomposition && _host.HdrOutput())
            return Fail(Reason::FormatUnsupported);

        IHudlessSource* source = _host.Source();
        if (source == nullptr || !source->Active())
        {
            _boundary = "no source";
            return Fail(Reason::NoSource);
        }

        if (mode == Mode::UiFree)
        {
            if (_host.DebugCompareOn())
                return Fail(Reason::DebugTintOn);
            if (_guard.Blocking())
                return Fail(Reason::MaskCoverage);
        }

        FrameImage image;
        switch (source->Take(image))
        {
        case TakeResult::NoSource:
            _boundary = "no source";
            return Fail(Reason::NoSource);
        case TakeResult::NotThisFrame:
            _boundary = "not detected";
            return Fail(Reason::NoBoundaryThisFrame);
        case TakeResult::Rejected:
        {
            const char* why = source->LastReason();
            _boundary = std::string("rejected: ") + (why != nullptr ? why : "unknown");
            return Fail(Reason::CaptureRejected);
        }
        case TakeResult::Ok:
            _boundary = "ok";
            break;
        }

        if (const Reason check = _host.CheckCapture(image); check != Reason::None)
            return Fail(check);
        if (const Reason stage = _host.StageCapture(image); stage != Reason::None)
            return Fail(stage);

        _staged = true;
        return true;
    }

    // UiFreeFrame, ui_free only. True = swap the frame generation base for the hud-less image.
    bool DecideBase()
    {
        if (!_active || !_staged || _mode != Mode::UiFree || _swapped)
            return _swapped;
        if (!_host.PasteReady())
        {
            Fail(Reason::PasteNotReady);
            return false;
        }
        if (_host.SceneHdrOwnsFrame())
        {
            Fail(Reason::SceneHdrOwnsFrame);
            return false;
        }
        _swapped = true;
        return true;
    }

    // The host found, while swapping the base, that the hud-less image cannot be used after all: undoes the swap.
    bool Reject(Reason reason)
    {
        _swapped = false;
        return Fail(reason);
    }

    // A failure after the staging (a tag the provider refused, a UI image that could not be built, ...). The first
    // failure of a present wins. Always returns false.
    bool Fail(Reason reason)
    {
        if (_reason == Reason::None)
            _reason = reason;
        return false;
    }

    // PostInteropCopy, last. Counts the present, updates the displayed state, writes the state-change and summary lines.
    void EndPresent()
    {
        if (!_active)
            return;

        ++_window.presents;
        ++_sinceSummary;
        if (_reason == Reason::None)
        {
            ++_captured;
            ++_window.captured;
        }
        else
        {
            ++_reasons[static_cast<size_t>(_reason)];
            NoteReason(_window, _reason);
        }

        // Not armed: nothing to capture from or no frame generator at all. Everything else is a visible fallback.
        const bool armed = _reason != Reason::NoFrameGeneration && _reason != Reason::NoSource;
        _tracker.Observe(armed, _reason);
        const Phase phase = _tracker.GetPhase();
        const Reason dominant = phase == Phase::Fallback ? _tracker.Dominant() : Reason::None;
        if (phase != _shownPhase || dominant != _shownReason)
        {
            _host.Log(StateChangeLine(_shownPhase, _shownReason, phase, dominant));
            _shownPhase = phase;
            _shownReason = dominant;
        }

        if (_sinceSummary >= kSummaryPresents)
        {
            _host.Log(SummaryLine(_mode, _shownPhase, _shownReason, _window, _paste.produced - _baseline.produced,
                                  _paste.pasted - _baseline.pasted, _paste.skipped - _baseline.skipped, _boundary));
            _window = {};
            _baseline = _paste;
            _sinceSummary = 0;
        }
    }

    // A blocking wait happened on the game thread (only a recopy-style path can cause one; none exists today).
    void NoteBlockingWait() { ++_window.slotWaits; }

    PasteAction PasteDecision() const
    {
        if (!_active || _mode != Mode::UiFree)
            return PasteAction::None;
        if (!_staged || _reason == Reason::SceneHdrOwnsFrame)
            return PasteAction::Invalidate;
        return PasteAction::Produce;
    }

    bool Staged() const { return _staged; }
    bool Swapped() const { return _swapped; }
    Mode CurrentMode() const { return _mode; }
    Reason PresentReason() const { return _reason; }

    Status Snapshot() const
    {
        Status s;
        s.mode = _lastMode;
        s.phase = _shownPhase;
        s.reason = _shownReason;
        s.lastReason = _reason;
        s.presents = _presents;
        s.captured = _captured;
        s.reasons = _reasons;
        s.coverage = _paste.coverage;
        s.coverageUnknown = _guard.Unknown();
        s.coverageTripped = _guard.Blocking();
        s.paste = _paste;
        s.boundary = _boundary;
        return s;
    }

  private:
    IEngineHost& _host;
    StateTracker _tracker;
    CoverageGuard _guard;
    WindowStats _window;
    std::array<uint64_t, kReasonCount> _reasons {};
    PasteCounters _paste;
    PasteCounters _baseline;
    std::string _boundary = "no source";
    uint64_t _presents = 0;
    uint64_t _captured = 0;
    uint64_t _sinceSummary = 0;
    uint64_t _lastSampledProduced = UINT64_MAX;
    Mode _mode = Mode::Off;
    Mode _lastMode = Mode::Off;
    Phase _shownPhase = Phase::Off;
    Reason _shownReason = Reason::None;
    Reason _reason = Reason::None;
    bool _announced = false;
    bool _active = false;
    bool _staged = false;
    bool _swapped = false;
};
} // namespace ext::hudless
