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
#include <format>
#include <optional>
#include <string>

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

// ---- text ----------------------------------------------------------------------------------------------------------------------

// A short readable form of the configuration the calibration belongs to (no hash): the render size, the output size and the quality value.
inline std::string KeyText(const Context& c)
{
    auto size = [](std::uint32_t w, std::uint32_t h) { return w != 0 && h != 0 ? std::format("{}x{}", w, h) : std::string("n/a"); };
    const RenderSize render = ResolveRenderSize(c.subrectW, c.subrectH, c.widthParam, c.heightParam);
    return std::format("render {}, output {}, quality {}", render.trusted ? size(render.width, render.height) : std::string("n/a"),
                       size(c.outW, c.outH), c.perfQuality ? std::to_string(*c.perfQuality) : std::string("n/a"));
}

inline std::string FormatLearningStarted(const std::string& key)
{
    return std::format("Jitter correction: learning the game's jitter ratio for {}; the game's values pass through until {} agreeing snapshots are seen",
                       key, kAdoptSnapshots);
}

// The line of a ratio that was adopted (Calibrator::mode() is the class of the adopted ratio).
inline std::string FormatAdopted(const Calibrator& c, const std::string& key)
{
    switch (c.mode())
    {
    case Mode::Scale:
        return std::format("Jitter correction: calibrated ratio {:.3f}/{:.3f} from {} snapshots for {}; scaling the reported jitter", c.ratioX(),
                           c.ratioY(), c.adoptedFrom(), key);
    case Mode::Neutral:
        return std::format("Jitter correction: the game already hands DLSS the jitter it applies (ratio {:.3f}/{:.3f} from {} snapshots for {}); "
                           "nothing is applied",
                           c.ratioX(), c.ratioY(), c.adoptedFrom(), key);
    default:
        return std::format("Jitter correction: ratio {:.3f}/{:.3f} from {} snapshots for {} refused (a ratio above 1.02 or below 0.25 is not believed); "
                           "nothing is applied, the game's values pass through",
                           c.ratioX(), c.ratioY(), c.adoptedFrom(), key);
    }
}

// A contradiction held and replaced the adopted ratio.
inline std::string FormatRatioChanged(float oldRatio, const Calibrator& c, const std::string& key)
{
    const char* tail = c.mode() == Mode::Scale     ? "scaling the reported jitter"
                       : c.mode() == Mode::Neutral ? "the game now hands DLSS what it applies, nothing is applied"
                                                   : "ratio refused, nothing is applied";
    return std::format("Jitter correction: ratio changed after {} contradicting snapshots: was {:.3f}, now {:.3f}/{:.3f} for {}; {}", c.adoptedFrom(),
                       oldRatio, c.ratioX(), c.ratioY(), key, tail);
}

inline std::string FormatConfigurationChanged(const std::string& key)
{
    return std::format("Jitter correction: configuration changed to {}; calibration dropped, learning again", key);
}

inline std::string FormatVerificationStale()
{
    return std::format("Jitter correction: verification stale, no usable jitter snapshot for {} s; keeping the current state", kStaleMs / 1000);
}

inline std::string FormatSnapshotsAgain() { return "Jitter correction: jitter snapshots are arriving again, verification is fresh"; }

inline std::string FormatNotUsable(Guard g)
{
    return std::format("Jitter correction: jitter pair not usable, game values passed through unchanged: {}", GuardName(g));
}

// The first pair a calibration scaled. `readX/readY` are what the parameter block returned when it was asked again after the write (nullopt =
// the read failed): the line shows the intended values and the stored ones side by side, so a block that did not take the write is visible.
inline std::string FormatFirstScaled(float reportedX, float reportedY, float scaledX, float scaledY, float ratio, const std::optional<float>& readX,
                                     const std::optional<float>& readY)
{
    const std::string stored = readX && readY ? std::format("({:.4f},{:.4f})", *readX, *readY) : std::string("unavailable");
    return std::format("Jitter correction: first scaled pair of this calibration: reported ({:.4f},{:.4f}) scaled to ({:.4f},{:.4f}) by ratio {:.3f}; "
                       "parameter block read back: {}",
                       reportedX, reportedY, scaledX, scaledY, ratio, stored);
}

// What happened to the evaluates since the correction started (totals for the whole session, never reset: the status lines are thinned, so each
// one has to carry everything).
struct CorrectionCounts
{
    std::uint64_t evaluated = 0, scaled = 0;
    std::uint64_t learning = 0;      // passed through while no ratio was adopted
    std::uint64_t notUsable = 0;     // passed through because the NGX pair was missing, not finite or too large
    std::uint64_t alreadyScaled = 0; // not scaled because the block still held our last result (the game did not refresh it)
    std::uint64_t idle = 0;          // nothing to apply: the game is consistent, the ratio was refused, or the manual scale is 1.0
};

// The state field of the status line, with the configuration it belongs to ("n/a" before any evaluate).
inline std::string StateText(const Calibrator& c, const std::string& key)
{
    switch (c.mode())
    {
    case Mode::Scale: return std::format("calibrated ratio {:.3f} for {}", c.ratio(), key);
    case Mode::Neutral: return std::format("consistent (ratio {:.3f}), nothing applied, for {}", c.ratio(), key);
    case Mode::Refused: return std::format("refused (ratio {:.3f}), nothing applied, for {}", c.ratio(), key);
    default:
        return c.relearning() ? std::format("dropped after a configuration change, learning again for {}", key) : std::format("learning for {}", key);
    }
}

// `frameGeneration`: whether the fork's frame generation is running (nullopt = could not be told). The scaled pair feeds it too, but only
// through the Upscaler input (the others get their jitter elsewhere), so `input` (State::activeFgInput as text, empty = not known) is printed
// next to it.
inline std::string FormatStatus(const CorrectionCounts& n, const std::string& state, const Calibrator::Counts& s, bool stale,
                                const std::optional<bool>& frameGeneration, const std::string& input)
{
    const std::string inputText = input.empty() ? std::string() : std::format(" (input {})", input);
    return std::format("Jitter correction: on; totals since the start of the session: {} evaluates seen, {} scaled, {} passed through while learning, "
                       "{} passed through because the jitter pair was not usable, {} already scaled (block not refreshed), {} left alone (nothing to apply); "
                       "state: {}; snapshots: {} accepted, {} disagreed with the ratio, {} ratio changes, {} restarts after a configuration change; "
                       "verification {}; frame generation active {}{}",
                       n.evaluated, n.scaled, n.learning, n.notUsable, n.alreadyScaled, n.idle, state, s.accepted, s.disagreed, s.changes, s.restarts,
                       stale ? "stale" : "fresh", frameGeneration ? (*frameGeneration ? "yes" : "no") : "unknown", inputText);
}

// ---- the manual scale ([FFXIV] JitterScaleOverride) ---------------------------------------------------------------------------------------

// A fixed scale for experiments, used instead of the learned ratio. 0 is OFF and is never applied as a scale (a zeroed jitter would break the
// image); a value from kMinPlausibleRatio to 1.0 is valid; anything else (NaN, infinite, negative, below 0.25, above 1.0) is ignored and the
// calibration goes on as if the key were 0.
enum class ManualScaleKind : std::uint8_t
{
    Off,
    Valid,
    Ignored
};

inline ManualScaleKind ClassifyManualScale(float v)
{
    if (v == 0.0f) // also -0.0
        return ManualScaleKind::Off;
    if (!std::isfinite(v) || v < kMinPlausibleRatio || v > 1.0f)
        return ManualScaleKind::Ignored;
    return ManualScaleKind::Valid;
}

// A valid value within the tolerance of 1.0 is the baseline (what the game does anyway): nothing is written.
inline bool ManualScaleIsBaseline(float v) { return std::fabs(v - 1.0f) <= kRatioTolerance; }

inline std::string FormatManualScaleInUse(float v) { return std::format("Jitter correction: manual scale {:.3f} in use, calibration off", v); }

inline std::string FormatManualScaleIgnored(float v)
{
    return std::format("Jitter correction: manual scale value {:.3f} ignored (valid values are 0.25 to 1.0, 0 turns it off); the calibration goes on", v);
}

// The state field of the status line while a manual scale is in use.
inline std::string ManualStateText(float v) { return std::format("manual scale {:.3f} (calibration off)", v); }
} // namespace ext::ffxiv::jitter
