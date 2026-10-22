#pragma once
// The decision and ordering core of the game-jitter probe and the calibrate-and-scale correction. Pure logic, templated on a small
// environment so that tests/ext/ffxiv/jitter can run it against a fake parameter block, fake process memory, a fake clock and a fake log:
// the rules that must never be broken (nothing runs with both keys off, every read of the game's values happens before the write, no
// write unless a ratio is calibrated) are pinned by tests instead of by reading. FfxivJitterProbe.cpp supplies the real environment and
// keeps OnUpscaleBoundary as a thin wrapper.
//
// The environment `Env` provides (none of it needs to be noexcept: Step catches what the calls throw and logs it once per episode, and
// the wrapper keeps a last silent catch for env.Lock() itself):
//   auto Lock();                                  RAII guard for the engine state (called once per active step, after the key gate)
//   const GameBuild* Build();                     the executable identity (lazy: asked only after the key gate, at most once per step)
//   std::uintptr_t ModuleBase();
//   bool ReadMemory(std::uintptr_t, void*, std::size_t);          process memory, false on any unreadable byte
//   std::optional<float> GetFloat(Param);         parameter block read; nullopt when absent
//   std::uint32_t GetUInt(Param);                 0 when absent
//   std::optional<int> GetInt(Param);
//   void SetFloat(Param, float);                  the ONLY write the core ever makes
//   ForcedQualityAnswer ForcedQuality();          the last answer the fork gave the game's quality query
//   std::uint64_t NowMs();
//   void Log(const std::string&);
//   std::optional<bool> FrameGenerationActive();  asked only when a status line is written
//   std::string FrameGenerationInput();           the active frame-generation input as text ("" = not known); asked with it
//
// Order inside one step (this is the contract the tests pin): key gate, lock, build, [probe-only time precheck], read the NGX jitter
// and the other parameters, limiter, read the game's object, then (correction) the configuration key, the snapshot of the object, the
// scaling write, then the log lines.
//
// The correction in one paragraph: the game applies a fraction of the jitter it reports to NGX. That fraction is learned from snapshots of
// the game's object (FfxivJitterCalibration.h; a snapshot does not have to belong to the evaluate in hand), and once five snapshots agree
// the NGX Jitter.Offset pair of EVERY evaluate is multiplied by it. The pair the core wrote last is remembered, so a block the game did
// not refresh is not scaled twice.

#include "ext/ffxiv/jitter/FfxivJitterCalibration.h"
#include "ext/ffxiv/jitter/FfxivJitterProbe.h"

#include <bit>
#include <cstdint>
#include <exception>
#include <format>
#include <optional>
#include <string>

namespace ext::ffxiv::jitter
{
// The parameters of the NGX block the core reads or writes. ParamName gives the NGX name; the glue checks every literal against the
// NGX header's own macro at compile time, so a typo cannot silently turn into a missing parameter.
enum class Param : std::uint8_t
{
    JitterX,
    JitterY,
    SubrectWidth,
    SubrectHeight,
    Width,
    Height,
    OutWidth,
    OutHeight,
    PerfQuality,
};

constexpr const char* ParamName(Param p)
{
    switch (p)
    {
    case Param::JitterX: return "Jitter.Offset.X";
    case Param::JitterY: return "Jitter.Offset.Y";
    case Param::SubrectWidth: return "DLSS.Render.Subrect.Dimensions.Width";
    case Param::SubrectHeight: return "DLSS.Render.Subrect.Dimensions.Height";
    case Param::Width: return "Width";
    case Param::Height: return "Height";
    case Param::OutWidth: return "OutWidth";
    case Param::OutHeight: return "OutHeight";
    case Param::PerfQuality: return "PerfQualityValue";
    }
    return "";
}

constexpr bool SameText(const char* a, const char* b)
{
    while (*a != '\0' && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}

// What the fork last answered the game's quality query (ext::upscaler::LastQualityAnswer, reduced to what the log line shows).
struct ForcedQualityAnswer
{
    bool present = false; // at least one query was answered
    int quality = 0;
    std::uint32_t renderWidth = 0, renderHeight = 0, displayWidth = 0, displayHeight = 0;
};

class Engine
{
  public:
    // One evaluate. Never throws (except from env.Lock()): an exception anywhere in a step abandons the REST of that step (the learning run
    // and the contradiction candidate are dropped, an adopted ratio is not; a pair whose second Set threw is put back to the game's own
    // X when that Set works, see Scale) and is logged once per episode of exceptions, so a feature that has turned into a no-op leaves a
    // trace. An episode is the same place failing again and again: a step that gets at least as far as the place that threw ends it, a step
    // that returned before that place proves nothing (the status line is built on sample ticks only, the steps between the ticks do not
    // reach it).
    // `manualScale` is [FFXIV] JitterScaleOverride (0 = off), read by the wrapper at every evaluate like the two keys; only used while `correct`.
    // `source` describes the source-level jitter fix at this evaluate (SourceNote): the wrapper passes correct = false while it is active, so the
    // engine never scales on top of the told offset; the probe uses the larger object bound and says so in its line.
    template <class Env> void Step(Env& env, bool probe, bool correct, float manualScale = 0.0f, const SourceNote& source = {})
    {
        // Both keys off: this is the whole cost (the wrapper adds a null test and the config reads). No lock, no build lookup, no clock,
        // no parameter read, no memory read, no log, no state touched.
        if (!probe && !correct)
            return;

        [[maybe_unused]] auto lock = env.Lock();
        _stage = Stage::Start;
        try
        {
            Run(env, probe, correct, manualScale, source);
            if (_stage >= _thrownAt)
                _exceptions.Event(false); // a step that got past the place that threw ends the episode of exceptions
        }
        catch (const std::exception& ex)
        {
            AbandonStep(env, ex.what());
        }
        catch (...)
        {
            AbandonStep(env, "unknown exception");
        }
    }

  private:
    // How far a step got: the stage an exception happened in, and the stage a completed step reached (see Step).
    enum class Stage : std::uint8_t
    {
        Start,  // key gate passed: the build, the clock, the early return of a probe-only step
        Reads,  // the parameter reads, the quality answer and the limiter
        Decide, // the game's object, the calibration, the write, the correction lines
        Sample, // a sample is due: the probe lines, the status line, the report
    };

    // The correction is off in this step (the probe alone is on): whatever the calibration and the episode logs remember about the
    // correction is stale by the time it is switched on again, so the whole calibration is dropped (adopted ratio, runs, remembered
    // counter, the pair last written). Learning starts again when it is switched back on. (Both keys off touches nothing, by design: a
    // pause with both keys off keeps an adopted ratio.)
    void ForgetCorrectionState()
    {
        _cal.Reset();
        _written.Forget();
        _announceScaled = false;
        _staleOpen = false;
        _keyText.clear();
        _manual.reset();
        _ignoredShown = false;
        _notUsable.Close();
    }

    // The step is over: the learning run and the contradiction candidate are dropped (a step that never reached the snapshot has not seen
    // it, and a run must be consecutive), an adopted ratio is not; the exception is reported once per episode.
    template <class Env> void AbandonStep(Env& env, const char* what) noexcept
    {
        if (_stage <= Stage::Decide)
            _cal.AbandonRun(); // an exception after the snapshot was taken (the sample stage) must not throw a correct run away
        if (_thrownAt != _stage)
            _exceptions.Close(); // another place is another episode, even while one is open
        _thrownAt = _stage;
        try
        {
            if (_exceptions.Event(true))
                env.Log(FormatStepException(what));
        }
        catch (...)
        {
            // Logging must never make a failed step worse.
        }
    }

    template <class Env> void Run(Env& env, bool probe, bool correct, float manualScale, const SourceNote& source)
    {
        if (!correct)
            ForgetCorrectionState();

        const GameBuild* build = env.Build();
        if (build == nullptr)
        {
            if (!_buildNoted)
            {
                _buildNoted = true;
                env.Log(std::format("Game jitter: probe and correction idle for this session, {}", GuardName(Guard::BuildMismatch)));
            }
            return;
        }

        const std::uint64_t now = env.NowMs();

        // Probe only: the clock alone says whether a sample can be due. The configuration key (render size, preset) is the one thing that
        // needs the parameters in between, so it is looked at on every 16th evaluate: in a stable configuration that is one pass of nine
        // lookups per 16 evaluates for as long as the session lasts (and none during the first 2 s after a sample, or when the early-sample
        // budget is spent). It is not "no lookups between samples". The correction needs every evaluate and skips this.
        if (!correct && !SampleMayBeDue(now))
            return;

        _stage = Stage::Reads;
        // The values this evaluate carries to NGX, read BEFORE any write: the probe compares against the game's own, and the scaling
        // starts from them. Nothing below is allowed to move these two reads behind the write.
        const std::optional<float> ngxX = env.GetFloat(Param::JitterX);
        const std::optional<float> ngxY = env.GetFloat(Param::JitterY);
        Context ctx;
        ctx.subrectW = env.GetUInt(Param::SubrectWidth);
        ctx.subrectH = env.GetUInt(Param::SubrectHeight);
        ctx.widthParam = env.GetUInt(Param::Width);
        ctx.heightParam = env.GetUInt(Param::Height);
        ctx.outW = env.GetUInt(Param::OutWidth);
        ctx.outH = env.GetUInt(Param::OutHeight);
        ctx.perfQuality = env.GetInt(Param::PerfQuality);
        const ForcedQualityAnswer answer = env.ForcedQuality();
        if (answer.present)
        {
            ctx.forcedQuality = answer.quality;
            ctx.answeredRenderW = answer.renderWidth;
            ctx.answeredRenderH = answer.renderHeight;
            ctx.answeredDisplayW = answer.displayWidth;
            ctx.answeredDisplayH = answer.displayHeight;
        }
        const RenderSize size = ResolveRenderSize(ctx.subrectW, ctx.subrectH, ctx.widthParam, ctx.heightParam);
        const std::uint64_t key = ConfigurationKey(ctx);

        // One sample per interval (and early after a configuration change); the correction works on every evaluate regardless.
        const bool allowed = _limiter.Allow(now, key);
        if (!correct && !allowed)
            return;

        _stage = Stage::Decide;
        auto reader = [&env](std::uintptr_t address, void* out, std::size_t bytes) { return env.ReadMemory(address, out, bytes); };
        RawObject object;
        const Guard readGuard = ReadChain(reader, build, env.ModuleBase(), object, source.active ? source.maxJitter : 0.5f);
        const Analysis analysis = readGuard == Guard::None ? Analyse(object, size) : Analysis {};
        const NgxMatch match = readGuard == Guard::None ? CompareNgx(object, ngxX, ngxY) : NgxMatch::Unavailable;

        if (correct)
            Correct(env, now, key, ctx, ngxX, ngxY, readGuard, analysis, manualScale);

        if (!allowed)
            return;
        _stage = Stage::Sample;

        if (probe)
        {
            const Guard guard = readGuard == Guard::None ? _counter.Check(object.counter) : readGuard;
            if (guard != Guard::None)
            {
                if (_probeFailures.ShouldLog(guard))
                    env.Log(FormatProbeFailure(guard));
            }
            else
            {
                _probeFailures.ShouldLog(Guard::None);
                if (_sampleLines < kMaxSampleLines)
                {
                    ++_sampleLines;
                    env.Log(FormatSample(analysis, match, ngxX, ngxY, ctx, source.active));
                    if (_sampleLines == kMaxSampleLines)
                        env.Log(FormatSampleBudgetUsedUp(kMaxSampleLines));
                }
            }
        }
        // The status line carries the totals since the start, so thinning it (the first 60 lines at every tick, then one per 60 s) loses
        // nothing; the counts are never zeroed.
        // The line is built BEFORE its slot is taken: a failing frame-generation query must not use up the thinned slots without a line.
        if (correct && _statusLog.Ready(now))
        {
            const std::string status = FormatStatus(_counts, _manual ? ManualStateText(*_manual) : StateText(_cal, _keyText.empty() ? std::string("n/a") : _keyText), _cal.counts(),
                                                    _cal.Stale(now), env.FrameGenerationActive(), env.FrameGenerationInput());
            _statusLog.Take(now);
            env.Log(status);
        }

        // The episodes the logs had no budget left for, in either mode, only when the total changed since the last report and thinned the
        // same way. The total is cumulative and `_reportedSuppressed` moves only when a report is written, so a change that comes between
        // two permitted reports is written at the next permitted one.
        const std::uint64_t calibration = static_cast<std::uint64_t>(_learningStarted.Suppressed()) + _adopted.Suppressed() + _ratioChanged.Suppressed() +
                                          _configChanged.Suppressed() + _staleLog.Suppressed() + _staleBackLog.Suppressed() +
                                          _manualInUse.Suppressed() + _manualIgnored.Suppressed();
        const std::uint64_t suppressed = static_cast<std::uint64_t>(_probeFailures.Suppressed()) + _notUsable.Suppressed() +
                                         _firstScaled.Suppressed() + _exceptions.Suppressed() + calibration;
        if (suppressed != _reportedSuppressed && _suppressedLog.Take(now))
        {
            _reportedSuppressed = suppressed;
            env.Log(FormatSuppressed(_probeFailures.Suppressed(), _notUsable.Suppressed(), _firstScaled.Suppressed(), _exceptions.Suppressed(),
                                     calibration));
        }
    }

    // The correction of one evaluate: the configuration key, the snapshot of the object, then the scaling of the NGX pair.
    template <class Env>
    void Correct(Env& env, std::uint64_t now, std::uint64_t key, const Context& ctx, const std::optional<float>& ngxX,
                 const std::optional<float>& ngxY, Guard readGuard, const Analysis& analysis, float manualScale)
    {
        ++_counts.evaluated;

        // The manual scale ([FFXIV] JitterScaleOverride) takes precedence over the calibration: no learning, no snapshots, every evaluate
        // scaled by the value through the same application path. 0 is off (never a scale); an invalid value is ignored, once per value.
        const ManualScaleKind manualKind = ClassifyManualScale(manualScale);
        if (manualKind == ManualScaleKind::Ignored)
        {
            const std::uint32_t bits = std::bit_cast<std::uint32_t>(manualScale);
            if (!_ignoredShown || bits != _ignoredBits)
            {
                _ignoredShown = true;
                _ignoredBits = bits;
                if (_manualIgnored.Occurrence())
                    env.Log(FormatManualScaleIgnored(manualScale));
            }
        }
        else
            _ignoredShown = false;
        const std::optional<float> manual = manualKind == ManualScaleKind::Valid ? std::optional<float>(manualScale) : std::nullopt;
        if (manual.has_value() != _manual.has_value() || (manual && std::bit_cast<std::uint32_t>(*manual) != std::bit_cast<std::uint32_t>(*_manual)))
        {
            // Switched on, changed or switched off: the calibration state is dropped (a manual scale does not learn, and learning starts from
            // scratch when it is switched off), and so is the pair written with the old scale.
            const std::optional<float> previous = _manual;
            _manual = manual;
            _cal.Reset();
            _staleOpen = false;
            _keyText.clear();
            // The first scaled pair is announced (with the read-back) when a manual scale is switched on from off. A change of the value keeps the
            // announcement as it was: a slider drag changes the value at every frame and must not spend the line budget the calibration needs.
            if (!manual)
                _announceScaled = false;
            else if (!previous || ManualScaleIsBaseline(*previous))
                _announceScaled = !ManualScaleIsBaseline(*manual);
            if (manual && _manualInUse.Occurrence())
                env.Log(FormatManualScaleInUse(*manual));
        }
        if (_manual)
        {
            if (ManualScaleIsBaseline(*_manual))
            {
                Apply(env, ngxX, ngxY, std::nullopt); // the baseline: the game's raw pair is put back when the block still holds ours
                ++_counts.idle;
                _notUsable.Event(false);
            }
            else
                Apply(env, ngxX, ngxY, *_manual);
            return;
        }

        // One slot: a different configuration drops the ratio, the runs and the remembered counter (and the remembered pair: the ratio it
        // was written with is gone); the evaluate that sees the new key passes the game's values through and learning starts again.
        const Calibrator::KeyResult keyResult = _cal.UseKey(key);
        if (keyResult != Calibrator::KeyResult::Unchanged)
        {
            _keyText = KeyText(ctx);
            if (keyResult == Calibrator::KeyResult::Changed)
            {
                _announceScaled = false;
                _staleOpen = false;
                if (_configChanged.Occurrence())
                    env.Log(FormatConfigurationChanged(_keyText));
            }
            else if (_learningStarted.Occurrence())
                env.Log(FormatLearningStarted(_keyText));
        }

        const float oldRatio = _cal.ratio();
        const Observation observation = _cal.Observe(readGuard, analysis, now);
        if (observation.change != CalibrationChange::None)
        {
            // A new ratio: the remembered pair stays (the block may still hold it); the next Apply re-derives from the raw pair.
            _announceScaled = _cal.mode() == Mode::Scale;
            _staleOpen = false;
            if (observation.change == CalibrationChange::Adopted)
            {
                if (_adopted.Occurrence())
                    env.Log(FormatAdopted(_cal, _keyText));
            }
            else if (_ratioChanged.Occurrence())
                env.Log(FormatRatioChanged(oldRatio, _cal, _keyText));
        }

        // An adopted ratio keeps being applied when no snapshot arrives (it is a property of the game's formula, not of a frame); the
        // log says that nobody is checking it any more, once per episode, and once when the snapshots are back.
        if (_cal.Stale(now))
        {
            if (!_staleOpen)
            {
                _staleOpen = true;
                if (_staleLog.Occurrence())
                    env.Log(FormatVerificationStale());
            }
        }
        else if (_staleOpen)
        {
            _staleOpen = false;
            if (_staleBackLog.Occurrence())
                env.Log(FormatSnapshotsAgain());
        }

        switch (_cal.mode())
        {
        case Mode::Learning:
            Apply(env, ngxX, ngxY, std::nullopt); // the raw pair is put back when the block still holds ours
            ++_counts.learning;
            _notUsable.Event(false);
            break;
        case Mode::Neutral:
        case Mode::Refused:
            Apply(env, ngxX, ngxY, std::nullopt);
            ++_counts.idle;
            _notUsable.Event(false);
            break;
        case Mode::Scale: Apply(env, ngxX, ngxY, _cal.ratio()); break;
        }
    }

    enum class Applied : std::uint8_t
    {
        Nothing,
        Scaled,
        AlreadyScaled,
        NotUsable
    };

    // Bring the NGX pair of this evaluate to what the state asks for. `ratio` = the adopted ratio (Scale mode) or a manual scale; nullopt = nothing
    // is to be applied (learning, Neutral, Refused, the manual baseline). The pair READ is the game's own unless it equals the pair this core
    // wrote last: then the game did not refresh the block, its raw pair is the one remembered with it, and the desired pair is re-derived from
    // THAT (so a new ratio or a slider drag never compounds, and a state that applies nothing puts the raw pair back).
    template <class Env> Applied Apply(Env& env, const std::optional<float>& ngxX, const std::optional<float>& ngxY, std::optional<float> ratio)
    {
        const bool unrefreshed = _written.has() && ngxX && ngxY && _written.Matches(*ngxX, *ngxY);
        float rawX = 0.0f, rawY = 0.0f;
        float wantX = 0.0f, wantY = 0.0f;
        if (!ratio)
        {
            if (!unrefreshed)
            {
                _written.Forget(); // the game refreshed the block (or nothing was written): nothing to restore
                return Applied::Nothing;
            }
            rawX = wantX = _rawX;
            rawY = wantY = _rawY;
        }
        else
        {
            ScaleDecision d = DecideScale(ngxX, ngxY, *ratio, _written);
            if (d.action == ScaleAction::NotUsable)
            {
                if (!unrefreshed)
                    _written.Forget();
                ++_counts.notUsable;
                if (_notUsable.Event(true))
                    env.Log(FormatNotUsable(d.guard));
                return Applied::NotUsable;
            }
            _notUsable.Event(false);
            if (d.action == ScaleAction::AlreadyScaled)
            {
                rawX = _rawX;
                rawY = _rawY;
                d = DecideScale(rawX, rawY, *ratio, WrittenPair {}); // from the raw pair, with the current ratio
                if (d.action != ScaleAction::Scale || _written.Matches(d.x, d.y))
                {
                    ++_counts.alreadyScaled;
                    return Applied::AlreadyScaled;
                }
            }
            else
            {
                rawX = *ngxX;
                rawY = *ngxY;
            }
            wantX = d.x;
            wantY = d.y;
        }

        // The block is the bridge's own NVSDK_NGX_Parameter (the Evaluate's InParameters, which later goes to the upscaler's Evaluate and
        // to the frame-generation input). What the block is believed to hold after OUR writes is what gets remembered, together with the
        // game's raw pair: nothing before the first Set (a Set that throws leaves the block as it was), the full pair after both Sets (or
        // nothing when the raw pair was restored), and after a failed Y either what it held before (X put back) or the mixed pair (the
        // put-back failed too).
        const WrittenPair before = _written;
        const float beforeRawX = _rawX, beforeRawY = _rawY;
        _written.Forget();
        env.SetFloat(Param::JitterX, wantX);
        try
        {
            env.SetFloat(Param::JitterY, wantY);
        }
        catch (...)
        {
            bool putBack = false;
            try
            {
                env.SetFloat(Param::JitterX, *ngxX);
                putBack = true;
            }
            catch (...)
            {
            }
            if (putBack)
            {
                if (unrefreshed)
                {
                    _written = before;
                    _rawX = beforeRawX;
                    _rawY = beforeRawY;
                }
            }
            else
            {
                _written.Remember(wantX, *ngxY);
                _rawX = rawX;
                _rawY = rawY;
            }
            throw;
        }
        if (!ratio)
        {
            _written.Forget();
            return Applied::Nothing;
        }
        _written.Remember(wantX, wantY);
        _rawX = rawX;
        _rawY = rawY;
        ++_counts.scaled;
        if (_announceScaled)
        {
            _announceScaled = false;
            if (_firstScaled.Occurrence())
            {
                // The first pair of a calibration: ask the block what it holds now, so the line shows what it really took.
                const std::optional<float> storedX = env.GetFloat(Param::JitterX);
                const std::optional<float> storedY = env.GetFloat(Param::JitterY);
                env.Log(FormatFirstScaled(rawX, rawY, wantX, wantY, *ratio, storedX, storedY));
            }
        }
        return Applied::Scaled;
    }

  public:
    // An hour of one line per 10 seconds: a session cannot grow its log without bound.
    static constexpr std::uint32_t kMaxSampleLines = 360;

  private:
    static constexpr std::uint32_t kKeyCheckEvery = 16;

    bool SampleMayBeDue(std::uint64_t now)
    {
        if (_limiter.IntervalElapsed(now))
            return true;
        if (++_sinceKeyCheck < kKeyCheckEvery)
            return false;
        _sinceKeyCheck = 0;
        return _limiter.ChangeWindowOpen(now);
    }

    SampleLimiter _limiter;
    std::uint32_t _sinceKeyCheck = 0; // evaluates since the configuration key was last looked at (probe only)
    CounterTracker _counter;
    EpisodeLog _probeFailures;
    EpisodeLog _notUsable;       // runs of evaluates whose NGX pair could not be scaled
    EpisodeLog _firstScaled;     // the first scaled pair of each calibration (with the read-back)
    EpisodeLog _exceptions;      // runs of steps that ended in an exception, at the same place
    // One event each, one line per occurrence under the 32-line budget (EpisodeLog::Occurrence):
    EpisodeLog _learningStarted, _adopted, _ratioChanged, _configChanged, _staleLog, _staleBackLog, _manualInUse, _manualIgnored;
    Stage _stage = Stage::Start;    // how far the step in progress has got
    Stage _thrownAt = Stage::Start; // where the last exception happened
    bool _buildNoted = false;
    std::uint32_t _sampleLines = 0;
    LineDecimator _statusLog, _suppressedLog; // thinned: the first 60 lines at every sample tick, then one per 60 s
    std::uint64_t _reportedSuppressed = 0; // the total of suppressed episodes the last report showed
    Calibrator _cal;
    WrittenPair _written;     // the pair the core last wrote, as passed to SetFloat (see WrittenPair)
    float _rawX = 0.0f, _rawY = 0.0f; // the game's own pair that _written was derived from (valid while _written.has())
    std::string _keyText;     // the readable form of the configuration key the calibration belongs to
    bool _announceScaled = false; // the next scaled pair is the first of its calibration: log it with the read-back
    bool _staleOpen = false;      // a "verification stale" episode is open
    std::optional<float> _manual; // the manual scale in use (a valid [FFXIV] JitterScaleOverride), none = the calibration decides
    bool _ignoredShown = false;   // an invalid manual scale was reported and has not changed since
    std::uint32_t _ignoredBits = 0;
    CorrectionCounts _counts; // totals since the start of the session
};
} // namespace ext::ffxiv::jitter
