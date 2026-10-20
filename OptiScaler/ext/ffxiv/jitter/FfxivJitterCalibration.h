#pragma once
// Calibrate-and-scale: the pure core of [FFXIV] JitterCorrection. No logging, no pch, no Windows calls, no I/O: the engine
// (FfxivJitterCore.h) feeds it snapshots of the game's jitter object and asks what to write, tests/ext/ffxiv/jitter/jitter_calibration.cpp
// runs it on fake snapshots.
//
// Why: in game the picture moves by only 60% of the jitter the game reports to NGX (ratio applied/reported 0.600 on both axes, at DLAA and
// at Performance). The ratio is a property of ONE snapshot of the object (its own jitter and NDC pair), so it needs no proof that the
// object belongs to the evaluate in hand: the ratio is learned from snapshots, and then every evaluate's NGX Jitter.Offset is multiplied
// by it.
//
//   snapshot  -> Calibrator::Observe  (acceptance, learning, adoption, verification, replacement)
//   configuration key -> Calibrator::UseKey (one slot: a change drops everything)
//   NGX pair  -> DecideScale          (usable? already scaled by us? x*r, y*r capped)
//   WrittenPair                       (the pair the engine last wrote, so a block the game did not refresh is not scaled twice)

#include "ext/ffxiv/jitter/FfxivJitterProbe.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>

namespace ext::ffxiv::jitter
{
inline constexpr unsigned kAdoptSnapshots = 5;        // consecutive agreeing snapshots that make a ratio
inline constexpr unsigned kReplaceSnapshots = 5;      // consecutive contradicting snapshots (agreeing with each other) that replace it
inline constexpr float kMinPlausibleRatio = 0.25f;    // below this the ratio is refused (a wrong render size or convention)
inline constexpr float kMaxReportedPixels = 0.55f;    // an NGX jitter above this is not believed (the game reports at most 0.5)
inline constexpr std::uint64_t kStaleMs = 10000;      // an adopted ratio with no accepted snapshot for this long is "verification stale"

enum class Mode : std::uint8_t
{
    Learning, // no ratio yet: the game's values pass through
    Scale,    // a ratio below 1 is adopted: every evaluate is scaled
    Neutral,  // the game already hands NGX what it applies (ratio within the tolerance of 1): nothing is applied
    Refused   // the ratio is implausible (above 1 + tolerance, or below kMinPlausibleRatio): nothing is applied
};

// Neutral if |r - 1| <= tolerance; Scale if kMinPlausibleRatio <= r < 1 - tolerance; Refused otherwise (NaN included). Float arithmetic,
// like Analyse's own verdict, so the two cannot disagree about the edge.
inline Mode ClassifyRatio(float r)
{
    if (std::fabs(r - 1.0f) <= kRatioTolerance)
        return Mode::Neutral;
    if (r >= kMinPlausibleRatio && r < 1.0f - kRatioTolerance)
        return Mode::Scale;
    return Mode::Refused;
}

enum class CalibrationChange : std::uint8_t
{
    None,
    Adopted,  // the first ratio of a learning phase
    Replaced  // an adopted ratio was replaced by a contradiction that held
};

// What one snapshot did.
struct Observation
{
    Guard skipped = Guard::None;  // not accepted: the reason (read failure, size, axis too small, sign, applied size, counter)
    bool accepted = false;
    bool axesDisagree = false;    // accepted, but the two axes give different ratios: no single ratio, not used for learning
    bool agreed = false;          // a ratio is adopted and this snapshot agrees with it
    bool disagreed = false;       // a ratio is adopted and this snapshot contradicts it (axes disagreeing counts)
    CalibrationChange change = CalibrationChange::None;
};

class Calibrator
{
  public:
    struct Counts
    {
        std::uint64_t accepted = 0;  // snapshots that passed every acceptance rule
        std::uint64_t disagreed = 0; // accepted snapshots that contradicted an adopted ratio
        std::uint64_t changes = 0;   // adopted ratios replaced by a contradiction
        std::uint64_t restarts = 0;  // learning restarted by a configuration change
    };

    enum class KeyResult : std::uint8_t
    {
        Unchanged,
        First,   // the first key this calibrator sees (or the first after Reset): nothing is dropped
        Changed  // a different key: the ratio, the runs and the remembered counter were dropped
    };

    // The calibration belongs to one configuration (ConfigurationKey). One slot: a different key drops everything and learning starts again.
    KeyResult UseKey(std::uint64_t key)
    {
        if (!_haveKey)
        {
            _haveKey = true;
            _key = key;
            return KeyResult::First;
        }
        if (key == _key)
            return KeyResult::Unchanged;
        _key = key;
        DropState();
        _relearning = true;
        ++_counts.restarts;
        return KeyResult::Changed;
    }

    // One snapshot of the game's object: `readGuard` is ReadChain's answer, `a` the analysis with this evaluate's render size.
    Observation Observe(Guard readGuard, const Analysis& a, std::uint64_t nowMs)
    {
        Observation o;
        o.skipped = Screen(readGuard, a);
        if (o.skipped != Guard::None)
            return o;

        // The counter is the last rule, so a snapshot turned away by an earlier rule never moves it. A counter that is not above the
        // remembered one (the same camera step again, or a re-created camera that restarts at 0) is not accepted and re-bases.
        const bool increasing = !_haveCounter || a.object.counter > _counter;
        _haveCounter = true;
        _counter = a.object.counter;
        if (!increasing)
        {
            o.skipped = Guard::CounterNotIncreasing;
            return o;
        }

        o.accepted = true;
        ++_counts.accepted;
        _lastAcceptedMs = nowMs;

        const double rx = a.rx, ry = a.ry;
        const bool axesAgree = std::fabs(rx - ry) <= static_cast<double>(kRatioTolerance) * std::max(rx, ry);
        const bool adopted = _mode != Mode::Learning;
        o.axesDisagree = !axesAgree;

        if (!adopted)
        {
            if (!axesAgree)
            {
                _run.Clear(); // no single ratio in this snapshot: it cannot join the run, and the run is not consecutive any more
                return o;
            }
            const double q = (rx + ry) / 2.0;
            if (_run.n > 0 && !Agrees(q, _run.MeanQ()))
                _run.Clear();
            _run.Add(q, rx, ry);
            if (_run.n >= kAdoptSnapshots)
            {
                Adopt(_run);
                o.change = CalibrationChange::Adopted;
            }
            return o;
        }

        if (!axesAgree)
        {
            o.disagreed = true;
            ++_counts.disagreed;
            _candidate.Clear();
            return o;
        }
        const double q = (rx + ry) / 2.0;
        if (Agrees(q, _ratio))
        {
            o.agreed = true;
            _candidate.Clear();
            return o;
        }
        o.disagreed = true;
        ++_counts.disagreed;
        if (_candidate.n > 0 && !Agrees(q, _candidate.MeanQ()))
            _candidate.Clear();
        _candidate.Add(q, rx, ry);
        if (_candidate.n >= kReplaceSnapshots)
        {
            Adopt(_candidate);
            ++_counts.changes;
            o.change = CalibrationChange::Replaced;
        }
        return o;
    }

    // An exception abandoned the step: the learning run and the contradiction candidate are dropped, an adopted ratio is not.
    void AbandonRun() noexcept
    {
        _run.Clear();
        _candidate.Clear();
    }

    // The correction was switched off: everything goes, the configuration key and the "relearning" flag included. The counts stay (they are
    // totals for the session).
    void Reset() noexcept
    {
        DropState();
        _haveKey = false;
        _relearning = false;
    }

    Mode mode() const { return _mode; }
    bool adopted() const { return _mode != Mode::Learning; }
    bool relearning() const { return _relearning; }
    float ratio() const { return _ratio; }
    float ratioX() const { return _ratioX; }
    float ratioY() const { return _ratioY; }
    unsigned adoptedFrom() const { return _adoptedFrom; }
    unsigned runLength() const { return _run.n; }
    const Counts& counts() const { return _counts; }

    // A ratio is adopted but no snapshot was accepted for kStaleMs (a clock that goes backwards is no time).
    bool Stale(std::uint64_t nowMs) const
    {
        return adopted() && nowMs >= _lastAcceptedMs && nowMs - _lastAcceptedMs >= kStaleMs;
    }

  private:
    struct Run
    {
        unsigned n = 0;
        double sumQ = 0.0, sumX = 0.0, sumY = 0.0;
        void Clear() noexcept { *this = Run {}; }
        void Add(double q, double x, double y)
        {
            ++n;
            sumQ += q;
            sumX += x;
            sumY += y;
        }
        double MeanQ() const { return sumQ / n; }
    };

    static bool Agrees(double q, double reference) { return std::fabs(q - reference) <= static_cast<double>(kRatioTolerance) * reference; }

    // Everything the acceptance rules ask except the counter. Guard::None = go on.
    static Guard Screen(Guard readGuard, const Analysis& a)
    {
        if (readGuard != Guard::None)
            return readGuard;
        if (!a.size.trusted || !a.haveApplied)
            return Guard::RenderSizeUnknown;
        if (!a.haveRx || !a.haveRy)
            return Guard::JitterTooSmall;
        if (!(a.rx > 0.0f) || !(a.ry > 0.0f)) // opposite signs, a zero ratio and NaN alike
            return Guard::SignMismatch;
        if (!std::isfinite(a.ax) || !std::isfinite(a.ay) || std::fabs(a.ax) > kMaxAppliedPixels || std::fabs(a.ay) > kMaxAppliedPixels)
            return Guard::AppliedImplausible;
        return Guard::None;
    }

    void Adopt(const Run& run)
    {
        _ratio = static_cast<float>(run.MeanQ());
        _ratioX = static_cast<float>(run.sumX / run.n);
        _ratioY = static_cast<float>(run.sumY / run.n);
        _adoptedFrom = run.n;
        _mode = ClassifyRatio(_ratio);
        _relearning = false;
        _run.Clear();
        _candidate.Clear();
    }

    void DropState() noexcept
    {
        _mode = Mode::Learning;
        _ratio = _ratioX = _ratioY = 0.0f;
        _adoptedFrom = 0;
        _run.Clear();
        _candidate.Clear();
        _haveCounter = false;
        _counter = 0;
    }

    Mode _mode = Mode::Learning;
    float _ratio = 0.0f, _ratioX = 0.0f, _ratioY = 0.0f;
    unsigned _adoptedFrom = 0;
    Run _run, _candidate;
    bool _haveCounter = false;
    std::int32_t _counter = 0;
    bool _haveKey = false, _relearning = false;
    std::uint64_t _key = 0, _lastAcceptedMs = 0;
    Counts _counts;
};

// ---- application ---------------------------------------------------------------------------------------------------------------

// The pair the engine last wrote into the NGX block, exactly as passed to SetFloat. The old correction wrote an absolute value, so writing
// it twice was harmless; a multiplication is not: if the game does not refresh Jitter.Offset before the next evaluate (or the seam runs
// twice for one evaluate) the block still holds OUR result, and scaling it again would shrink the jitter every time.
class WrittenPair
{
  public:
    void Remember(float x, float y) noexcept
    {
        _have = true;
        _x = std::bit_cast<std::uint32_t>(x);
        _y = std::bit_cast<std::uint32_t>(y);
    }
    void Forget() noexcept { _have = false; }
    bool has() const { return _have; }
    // Equal bit for bit on both axes.
    bool Matches(float x, float y) const
    {
        return _have && _x == std::bit_cast<std::uint32_t>(x) && _y == std::bit_cast<std::uint32_t>(y);
    }

  private:
    bool _have = false;
    std::uint32_t _x = 0, _y = 0;
};

enum class ScaleAction : std::uint8_t
{
    Scale,         // write x and y
    NotUsable,     // the NGX pair is missing, not finite or too large: the game's values pass through
    AlreadyScaled  // the pair equals what we wrote last: the game did not refresh the block, nothing is written
};

struct ScaleDecision
{
    ScaleAction action = ScaleAction::NotUsable;
    Guard guard = Guard::None; // why NotUsable
    float x = 0.0f, y = 0.0f;  // the values to write (Scale only)
};

inline float CapApplied(float v) { return std::clamp(v, -kMaxAppliedPixels, kMaxAppliedPixels); }

// `ratio` is the adopted ratio (Scale mode) or a manual scale. Order: usable pair first, then the stale-block comparison, then the
// multiplication (X and Y each by the same ratio), capped at kMaxAppliedPixels.
inline ScaleDecision DecideScale(const std::optional<float>& ngxX, const std::optional<float>& ngxY, float ratio, const WrittenPair& last)
{
    ScaleDecision d;
    if (!ngxX || !ngxY || !std::isfinite(*ngxX) || !std::isfinite(*ngxY))
    {
        d.guard = Guard::NgxJitterUnavailable;
        return d;
    }
    if (std::fabs(*ngxX) > kMaxReportedPixels || std::fabs(*ngxY) > kMaxReportedPixels)
    {
        d.guard = Guard::ReportedOutOfRange;
        return d;
    }
    if (last.Matches(*ngxX, *ngxY))
    {
        d.action = ScaleAction::AlreadyScaled;
        return d;
    }
    d.action = ScaleAction::Scale;
    d.x = CapApplied(*ngxX * ratio);
    d.y = CapApplied(*ngxY * ratio);
    return d;
}
} // namespace ext::ffxiv::jitter
