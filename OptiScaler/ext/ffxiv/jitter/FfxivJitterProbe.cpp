#include "pch.h"

#include "ext/ffxiv/jitter/FfxivJitter.h"
#include "ext/ffxiv/jitter/FfxivJitterProbe.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/ProcessMemory.h"
#include "ext/upscaler/UpscalerSeams.h"

#include <mutex>

namespace ext::ffxiv::jitter
{
namespace
{
struct State
{
    std::mutex mutex;
    SampleLimiter limiter;
    CounterTracker counter;
    EpisodeLog probeFailures;
    EpisodeLog passThrough;
    bool replacing = false;
    bool buildNoted = false;
    std::uint64_t evaluated = 0, replaced = 0, passed = 0; // since the last status line
};

State& S()
{
    static State state;
    return state;
}

// The executable identity is checked once per session (MatchCurrentProcess walks the module headers).
const GameBuild* Build()
{
    static const GameBuild* build = MatchCurrentProcess();
    return build;
}

std::optional<float> GetFloat(const NVSDK_NGX_Parameter* p, const char* name)
{
    float v = 0.0f;
    if (p->Get(name, &v) == NVSDK_NGX_Result_Success)
        return v;
    return std::nullopt;
}

std::uint32_t GetUInt(const NVSDK_NGX_Parameter* p, const char* name)
{
    unsigned int v = 0;
    return p->Get(name, &v) == NVSDK_NGX_Result_Success ? v : 0u;
}

std::optional<int> GetInt(const NVSDK_NGX_Parameter* p, const char* name)
{
    int v = 0;
    if (p->Get(name, &v) == NVSDK_NGX_Result_Success)
        return v;
    unsigned int u = 0;
    if (p->Get(name, &u) == NVSDK_NGX_Result_Success)
        return static_cast<int>(u);
    return std::nullopt;
}

void Run(const NVSDK_NGX_Parameter* parameters, bool probe, bool correct)
{
    const GameBuild* build = Build();
    State& st = S();
    std::lock_guard lock(st.mutex);

    if (build == nullptr)
    {
        if (!st.buildNoted)
        {
            st.buildNoted = true;
            LOG_INFO("Game jitter: probe and correction idle for this session, {}", GuardName(Guard::BuildMismatch));
        }
        return;
    }

    // The values this evaluate carries to NGX, read BEFORE any replacement: the probe compares against the game's own.
    const std::optional<float> ngxX = GetFloat(parameters, NVSDK_NGX_Parameter_Jitter_Offset_X);
    const std::optional<float> ngxY = GetFloat(parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y);
    Context ctx;
    ctx.subrectW = GetUInt(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width);
    ctx.subrectH = GetUInt(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height);
    ctx.widthParam = GetUInt(parameters, NVSDK_NGX_Parameter_Width);
    ctx.heightParam = GetUInt(parameters, NVSDK_NGX_Parameter_Height);
    ctx.outW = GetUInt(parameters, NVSDK_NGX_Parameter_OutWidth);
    ctx.outH = GetUInt(parameters, NVSDK_NGX_Parameter_OutHeight);
    ctx.perfQuality = GetInt(parameters, NVSDK_NGX_Parameter_PerfQualityValue);
    const auto answer = ext::upscaler::LastQualityAnswer();
    if (answer.queries != 0)
    {
        ctx.forcedQuality = answer.quality;
        ctx.answeredRenderW = answer.renderWidth;
        ctx.answeredRenderH = answer.renderHeight;
        ctx.answeredDisplayW = answer.displayWidth;
        ctx.answeredDisplayH = answer.displayHeight;
    }
    const RenderSize size = ResolveRenderSize(ctx.subrectW, ctx.subrectH, ctx.widthParam, ctx.heightParam);

    auto reader = [](std::uintptr_t address, void* out, std::size_t bytes) { return ext::core::ReadProcessBytes(address, out, bytes); };
    RawObject object;
    const Guard readGuard = ReadChain(reader, build, reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)), object);
    const Analysis analysis = readGuard == Guard::None ? Analyse(object, size) : Analysis {};
    const NgxMatch match = readGuard == Guard::None ? CompareNgx(object, ngxX, ngxY) : NgxMatch::Unavailable;

    if (correct)
    {
        ++st.evaluated;
        const CorrectionDecision decision = DecideCorrection(readGuard, analysis, match);
        if (decision.replace)
        {
            // The block is the game's own non-const NVSDK_NGX_Parameter (IFeature_Dx11wDx12::Evaluate's InParameters, which later goes to
            // the upscaler's Evaluate and to the frame-generation input); the seam only declares it const. The game sets Jitter.Offset
            // again at its next evaluate, so nothing persists past this frame.
            auto* writable = const_cast<NVSDK_NGX_Parameter*>(parameters);
            writable->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, decision.ax);
            writable->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, decision.ay);
            ++st.replaced;
            if (!st.replacing)
            {
                st.replacing = true;
                LOG_INFO("{}", FormatCorrection(analysis, decision));
            }
            st.passThrough.Close();
        }
        else
        {
            ++st.passed;
            st.replacing = false;
            if (st.passThrough.ShouldLog(decision.guard))
                LOG_INFO("{}", FormatPassThrough(decision.guard));
        }
    }

    if (!st.limiter.Allow(GetTickCount64(), ConfigurationKey(ctx)))
        return;

    if (probe)
    {
        const Guard guard = readGuard == Guard::None ? st.counter.Check(object.counter) : readGuard;
        if (guard != Guard::None)
        {
            if (st.probeFailures.ShouldLog(guard))
                LOG_INFO("{}", FormatProbeFailure(guard));
        }
        else
        {
            st.probeFailures.ShouldLog(Guard::None);
            LOG_INFO("{}", FormatSample(analysis, match, ngxX, ngxY, ctx));
        }
    }
    if (correct)
    {
        LOG_INFO("Jitter correction: on; since the last status line {} evaluates seen, {} replaced by the applied jitter, {} passed through unchanged",
                 st.evaluated, st.replaced, st.passed);
        st.evaluated = st.replaced = st.passed = 0;
    }
}
} // namespace

void OnUpscaleBoundary(const NVSDK_NGX_Parameter* parameters) noexcept
{
    try
    {
        // Both keys off: this is all it costs.
        const auto& cfg = ext::Cfg().ffxiv;
        const bool probe = cfg.jitterProbe.value_or_default();
        const bool correct = cfg.jitterCorrection.value_or_default();
        if ((!probe && !correct) || parameters == nullptr)
            return;
        Run(parameters, probe, correct);
    }
    catch (...)
    {
        // Never into the evaluate path: the game's parameters stay as they are.
    }
}
} // namespace ext::ffxiv::jitter
