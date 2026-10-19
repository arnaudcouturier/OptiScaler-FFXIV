#pragma once
// The decision and ordering core of the game-jitter probe and correction. Pure logic, templated on a small environment so that
// tests/ext/ffxiv/jitter/jitter_glue.cpp can run it against a fake parameter block, fake process memory, a fake clock and a fake log:
// the rules that must never be broken (nothing runs with both keys off, every read of the game's values happens before the write, no
// write unless every guard passed) are pinned by tests instead of by reading. FfxivJitterProbe.cpp supplies the real environment and
// keeps OnUpscaleBoundary as a thin wrapper.
//
// The environment `Env` provides (all noexcept-free; the wrapper catches):
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
//
// Order inside one step (this is the contract the tests pin): key gate, lock, build, [probe-only time precheck], read the NGX jitter
// and the other parameters, limiter, read the game's object, decide, write (correction only), then the log lines.

#include "ext/ffxiv/jitter/FfxivJitterProbe.h"

#include <cstdint>
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
    template <class Env> void Step(Env& env, bool probe, bool correct)
    {
        // Both keys off: this is the whole cost (the wrapper adds a null test and the config reads). No lock, no build lookup, no clock,
        // no parameter read, no memory read, no log.
        if (!probe && !correct)
            return;

        [[maybe_unused]] auto lock = env.Lock();
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

        // Probe only: one sample per interval, and nothing is read in between. The correction needs every evaluate.
        const bool allowed = _limiter.Allow(now, ConfigurationKey(ctx));
        if (!correct && !allowed)
            return;

        auto reader = [&env](std::uintptr_t address, void* out, std::size_t bytes) { return env.ReadMemory(address, out, bytes); };
        RawObject object;
        const Guard readGuard = ReadChain(reader, build, env.ModuleBase(), object);
        const Analysis analysis = readGuard == Guard::None ? Analyse(object, size) : Analysis {};
        const NgxMatch match = readGuard == Guard::None ? CompareNgx(object, ngxX, ngxY) : NgxMatch::Unavailable;

        if (correct)
        {
            ++_counts.evaluated;
            const CorrectionDecision proven = DecideCorrection(readGuard, analysis, match);
            // The same-frame rules look at every evaluate that read the object (they need the previous one), but only a replacement
            // they refuse is counted as held back.
            const Guard frame = _frames.Observe(readGuard, analysis);
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
                env.SetFloat(Param::JitterY, decision.ay);
                ++_counts.replaced;
                if (_replacing.Event(true))
                    env.Log(FormatCorrection(analysis, decision));
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
                env.Log(FormatSample(analysis, match, ngxX, ngxY, ctx));
            }
        }
        if (correct)
        {
            env.Log(FormatStatus(_counts));
            if (_replacing.Suppressed() != 0 || _passThrough.Suppressed() != 0)
                env.Log(std::format("Jitter correction: log line budget used up, {} further episodes were not logged",
                                    _replacing.Suppressed() + _passThrough.Suppressed()));
            _counts = StatusCounts {};
        }
    }

  private:
    SampleLimiter _limiter;
    CounterTracker _counter;
    EpisodeLog _probeFailures;
    EpisodeLog _passThrough;
    EpisodeLog _replacing; // runs of evaluates in which the correction replaced the values
    bool _buildNoted = false;
    FrameRules _frames;
    StatusCounts _counts; // since the last status line
};
} // namespace ext::ffxiv::jitter
