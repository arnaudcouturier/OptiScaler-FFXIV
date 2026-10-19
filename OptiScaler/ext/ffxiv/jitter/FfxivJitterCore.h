#pragma once
// The decision and ordering core of the game-jitter probe and correction. Pure logic, templated on a small environment so that
// tests/ext/ffxiv/jitter/jitter_glue.cpp can run it against a fake parameter block, fake process memory, a fake clock and a fake log:
// the rules that must never be broken (nothing runs with both keys off, every read of the game's values happens before the write, no
// write unless every guard passed) are pinned by tests instead of by reading. FfxivJitterProbe.cpp supplies the real environment and
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
// and the other parameters, limiter, read the game's object, decide, write (correction only), then the log lines.

#include "ext/ffxiv/jitter/FfxivJitterProbe.h"

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
    // One evaluate. Never throws (except from env.Lock()): an exception anywhere in a step abandons the REST of that step (a write only
    // follows a complete proof, so whatever was not reached was not written; a pair whose second Set threw is put back to the game's own
    // X when that Set works, see Run, so the pair that goes on to the upscaler is the game's) and is logged once per episode of
    // exceptions, so a feature that has turned into a no-op leaves a trace. An episode is the same place failing again and again: a step
    // that gets at least as far as the place that threw ends it, a step that returned before that place proves nothing (the status line
    // is built on sample ticks only, the steps between the ticks do not reach it).
    template <class Env> void Step(Env& env, bool probe, bool correct)
    {
        // Both keys off: this is the whole cost (the wrapper adds a null test and the config reads). No lock, no build lookup, no clock,
        // no parameter read, no memory read, no log, no state touched.
        if (!probe && !correct)
            return;

        [[maybe_unused]] auto lock = env.Lock();
        _stage = Stage::Start;
        try
        {
            Run(env, probe, correct);
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
        Decide, // the game's object, the decision, the write, the correction lines
        Sample, // a sample is due: the probe lines, the status line, the report
    };

    // The correction is off in this step (the probe alone is on): whatever the same-frame rules and the episode logs remember about the
    // correction is stale by the time it is switched on again, so it is dropped. (Both keys off touches nothing, by design.)
    void ForgetCorrectionState()
    {
        _frames = FrameRules {};
        _replacing.Close();
        _passThrough.Close();
    }

    // The step is over: whatever the same-frame rules remembered is no longer "the previous evaluate" (a step that never reached them
    // has not been seen), so a thrown step forgets like a failed read, and the exception is reported once per episode.
    template <class Env> void AbandonStep(Env& env, const char* what) noexcept
    {
        _frames = FrameRules {};
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

    template <class Env> void Run(Env& env, bool probe, bool correct)
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
        // The values this evaluate carries to NGX, read BEFORE any replacement: the probe compares against the game's own, and the
        // correction decides on them. Nothing below is allowed to move these two reads behind the write.
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

        // One sample per interval (and early after a configuration change); the correction decides on every evaluate regardless.
        const bool allowed = _limiter.Allow(now, ConfigurationKey(ctx));
        if (!correct && !allowed)
            return;

        _stage = Stage::Decide;
        auto reader = [&env](std::uintptr_t address, void* out, std::size_t bytes) { return env.ReadMemory(address, out, bytes); };
        RawObject object;
        const Guard readGuard = ReadChain(reader, build, env.ModuleBase(), object);
        const Analysis analysis = readGuard == Guard::None ? Analyse(object, size) : Analysis {};
        const NgxMatch match = readGuard == Guard::None ? CompareNgx(object, ngxX, ngxY) : NgxMatch::Unavailable;

        if (correct)
        {
            ++_counts.evaluated;
            const CorrectionDecision proven = DecideCorrection(readGuard, analysis, match);
            // The same-frame rules look at every evaluate and need the previous one. An evaluate the proof rejected (for any reason,
            // not only an unreadable object) is not a reference: it makes the next evaluate a first frame again. Only a replacement
            // the rules refuse is counted as held back.
            const Guard frame = _frames.Observe(proven.guard, analysis);
            const CorrectionDecision decision = ApplyFrameRules(proven, frame);
            if (proven.replace && !decision.replace)
            {
                if (frame == Guard::CounterNotIncreasing)
                    ++_counts.heldCounter;
                else if (frame == Guard::RatioNotSteady)
                    ++_counts.heldRatio;
                else
                    ++_counts.heldFirst;
            }
            if (decision.replace)
            {
                // The block is the bridge's own NVSDK_NGX_Parameter (the Evaluate's InParameters, which later goes to the upscaler's
                // Evaluate and to the frame-generation input). The game sets Jitter.Offset again at its next evaluate.
                env.SetFloat(Param::JitterX, decision.ax);
                try
                {
                    env.SetFloat(Param::JitterY, decision.ay);
                }
                catch (...)
                {
                    // Half written: X is corrected, Y is the game's. Put the game's own X back (best effort, a failing Set here is
                    // swallowed) so that the pair that goes on to the upscaler is not a mixed one, then let Step abandon the evaluate.
                    // A replacement needs the NGX jitter to match, so ngxX is set here.
                    if (ngxX)
                    {
                        try
                        {
                            env.SetFloat(Param::JitterX, *ngxX);
                        }
                        catch (...)
                        {
                        }
                    }
                    throw;
                }
                ++_counts.replaced;
                if (_replacing.Event(true))
                {
                    // The first replacement of an episode: ask the block what it holds now, so the line shows what it really took.
                    const std::optional<float> storedX = env.GetFloat(Param::JitterX);
                    const std::optional<float> storedY = env.GetFloat(Param::JitterY);
                    env.Log(FormatCorrection(analysis, decision, storedX, storedY));
                }
                _passThrough.Close();
            }
            else
            {
                ++_counts.passed;
                if (!decision.undetermined) // a frame too small to tell changes no episode
                {
                    _replacing.Event(false);
                    if (_passThrough.ShouldLog(decision.guard))
                        env.Log(FormatPassThrough(decision.guard));
                }
            }
        }

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
                    env.Log(FormatSample(analysis, match, ngxX, ngxY, ctx));
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
            const std::string status = FormatStatus(_counts, env.FrameGenerationActive(), env.FrameGenerationInput());
            _statusLog.Take(now);
            env.Log(status);
        }

        // The episodes the three logs had no budget left for, in either mode, only when the total changed since the last report and
        // thinned the same way. The total is cumulative and `_reportedSuppressed` moves only when a report is written, so a change that
        // comes between two permitted reports is written at the next permitted one.
        const std::uint64_t suppressed = static_cast<std::uint64_t>(_probeFailures.Suppressed()) + _passThrough.Suppressed() +
                                         _replacing.Suppressed() + _exceptions.Suppressed();
        if (suppressed != _reportedSuppressed && _suppressedLog.Take(now))
        {
            _reportedSuppressed = suppressed;
            env.Log(FormatSuppressed(_probeFailures.Suppressed(), _passThrough.Suppressed(), _replacing.Suppressed(),
                                     _exceptions.Suppressed()));
        }
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
    EpisodeLog _passThrough;
    EpisodeLog _replacing; // runs of evaluates in which the correction replaced the values
    EpisodeLog _exceptions; // runs of steps that ended in an exception, at the same place
    Stage _stage = Stage::Start;    // how far the step in progress has got
    Stage _thrownAt = Stage::Start; // where the last exception happened
    bool _buildNoted = false;
    std::uint32_t _sampleLines = 0;
    LineDecimator _statusLog, _suppressedLog; // thinned: the first 60 lines at every sample tick, then one per 60 s
    std::uint64_t _reportedSuppressed = 0; // the total of suppressed episodes the last report showed
    FrameRules _frames;
    StatusCounts _counts; // totals since the start of the session
};
} // namespace ext::ffxiv::jitter
