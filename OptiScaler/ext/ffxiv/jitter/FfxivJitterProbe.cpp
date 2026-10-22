#include "pch.h"

#include "ext/ffxiv/jitter/FfxivJitter.h"
#include "ext/ffxiv/jitter/FfxivJitterCore.h"
#include "ext/ffxiv/jitter/FfxivJitterSourceHooks.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/ProcessMemory.h"
#include "ext/upscaler/UpscalerSeams.h"

#include <Config.h>
#include <State.h>
#include <framegen/IFGFeature_Dx12.h>

#include <atomic>
#include <mutex>

namespace ext::ffxiv::jitter
{
namespace
{
// The core spells the NGX parameter names as literals (it does not include the NGX header); a typo there would turn a parameter into a
// "missing" one and fail open without a sound, so every literal is checked against the header's own macro here.
static_assert(SameText(ParamName(Param::JitterX), NVSDK_NGX_Parameter_Jitter_Offset_X));
static_assert(SameText(ParamName(Param::JitterY), NVSDK_NGX_Parameter_Jitter_Offset_Y));
static_assert(SameText(ParamName(Param::SubrectWidth), NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width));
static_assert(SameText(ParamName(Param::SubrectHeight), NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height));
static_assert(SameText(ParamName(Param::Width), NVSDK_NGX_Parameter_Width));
static_assert(SameText(ParamName(Param::Height), NVSDK_NGX_Parameter_Height));
static_assert(SameText(ParamName(Param::OutWidth), NVSDK_NGX_Parameter_OutWidth));
static_assert(SameText(ParamName(Param::OutHeight), NVSDK_NGX_Parameter_OutHeight));
static_assert(SameText(ParamName(Param::PerfQuality), NVSDK_NGX_Parameter_PerfQualityValue));

struct JitterState
{
    std::mutex mutex;
    Engine engine;
};

JitterState& S()
{
    static JitterState state;
    return state;
}

// The real environment of one step: the bridge's parameter block, the process, the clock and the log.
struct NgxEnvironment
{
    NVSDK_NGX_Parameter* parameters;

    std::unique_lock<std::mutex> Lock() { return std::unique_lock<std::mutex>(S().mutex); }

    // The executable identity is checked once per session (MatchCurrentProcess walks the module headers).
    const GameBuild* Build()
    {
        static const GameBuild* build = MatchCurrentProcess();
        return build;
    }

    std::uintptr_t ModuleBase() { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)); }
    bool ReadMemory(std::uintptr_t address, void* out, std::size_t bytes) { return ext::core::ReadProcessBytes(address, out, bytes); }

    std::optional<float> GetFloat(Param p)
    {
        float v = 0.0f;
        if (parameters->Get(ParamName(p), &v) == NVSDK_NGX_Result_Success)
            return v;
        return std::nullopt;
    }

    std::uint32_t GetUInt(Param p)
    {
        unsigned int v = 0;
        return parameters->Get(ParamName(p), &v) == NVSDK_NGX_Result_Success ? v : 0u;
    }

    std::optional<int> GetInt(Param p)
    {
        int v = 0;
        if (parameters->Get(ParamName(p), &v) == NVSDK_NGX_Result_Success)
            return v;
        unsigned int u = 0;
        if (parameters->Get(ParamName(p), &u) == NVSDK_NGX_Result_Success)
            return static_cast<int>(u);
        return std::nullopt;
    }

    void SetFloat(Param p, float value) { parameters->Set(ParamName(p), value); }

    ForcedQualityAnswer ForcedQuality()
    {
        const auto answer = ext::upscaler::LastQualityAnswer();
        ForcedQualityAnswer out;
        out.present = answer.queries != 0;
        out.quality = answer.quality;
        out.renderWidth = answer.renderWidth;
        out.renderHeight = answer.renderHeight;
        out.displayWidth = answer.displayWidth;
        out.displayHeight = answer.displayHeight;
        return out;
    }

    std::uint64_t NowMs() { return GetTickCount64(); }
    void Log(const std::string& line) { LOG_INFO("{}", line); }
    // The fork's own accessor expression for "frame generation is running" (the same test ext/hudless/HudlessEngine.cpp uses). Called only
    // when a status line is written (every 10 s), on the evaluate thread that already hands the same object the jitter right after the
    // upscaler Evaluate (UpscaleStart).
    std::optional<bool> FrameGenerationActive()
    {
        auto* fg = ::State::Instance().currentFG;
        if (fg == nullptr)
            return false;
        return fg->IsActive() && !fg->IsPaused() && Config::Instance()->IsFGEnabled();
    }
    // State::activeFgInput by its config name (Upscaler, DLSSG, NoFG, ...): only the Upscaler input reads the replaced pair from the block.
    std::string FrameGenerationInput() { return EnumToCode(::State::Instance().activeFgInput); }

    // The source-level jitter fix (FfxivJitterSourceController.h): its key, and the hooks' state (asking installs them when the key is on).
    bool KeyOn() { return ext::Cfg().ffxiv.jitterSource.value_or_default(); }
    source::HooksView Hooks() { return source::hooks::View(); }
};

// The source-level jitter fix's side of the evaluate. Its own lock, never held while the engine runs (the engine takes S().mutex).
struct SourceGlue
{
    std::mutex mutex;
    source::Controller<NgxEnvironment> controller;
};

SourceGlue& G()
{
    static SourceGlue glue;
    return glue;
}

// Set once the key has been on in this session: after that the controller is asked at every evaluate so that it can switch the fix off cleanly
// (and tell the frames that were already drawn on the full lattice) when the key goes off. Never set with the key off all session.
std::atomic<bool> g_sourceEverOn { false };

// One step of the source fix. The result says whether the calibrate-and-scale engine must stand down for this evaluate.
source::StepResult RunSourceFix(NgxEnvironment& env, bool sourceKey, bool correct)
{
    if (sourceKey)
        g_sourceEverOn.store(true, std::memory_order_relaxed);
    SourceGlue& g = G();
    std::lock_guard lock(g.mutex);
    try
    {
        return g.controller.Step(env, source::hooks::State(), correct);
    }
    catch (...)
    {
        // The controller threw (a parameter write failed): it may have written the first half of the pair. The engine stays out of this evaluate if
        // the fix was active; the controller's own put-back already restored the game's X.
        source::StepResult r;
        r.standDown = r.active = g.controller.WasActive();
        return r;
    }
}
} // namespace

void OnUpscaleBoundary(NVSDK_NGX_Parameter* parameters) noexcept
{
    try
    {
        // All keys off, and the source fix never switched on: this is all it costs, three config flag loads and one relaxed atomic load (the core repeats the key gate, which a test pins, so it never
        // runs on its own either).
        const auto& cfg = ext::Cfg().ffxiv;
        const bool probe = cfg.jitterProbe.value_or_default();
        const bool correct = cfg.jitterCorrection.value_or_default();
        const bool sourceKey = cfg.jitterSource.value_or_default();
        if ((!probe && !correct && !sourceKey && !g_sourceEverOn.load(std::memory_order_relaxed)) || parameters == nullptr)
            return;
        NgxEnvironment env { parameters };
        // The source fix first: while it is active it owns the pair, and the calibrate-and-scale engine stands down for this evaluate.
        // Asked only when the key is on or has been on this session: with the key off all session the controller is never reached.
        source::StepResult src;
        if (sourceKey || g_sourceEverOn.load(std::memory_order_relaxed))
            src = RunSourceFix(env, sourceKey, correct);
        const bool correctNow = correct && !src.standDown;
        S().engine.Step(env, probe, correctNow, cfg.jitterScaleOverride.value_or_default(), SourceNote { src.active, src.maxJitter, src.standDown });
    }
    catch (...)
    {
        // Never into the evaluate path. The engine already catches and logs what a step throws; this net is for what it cannot log: a lock
        // that cannot be taken, or the construction of the environment. The game's parameters stay as they are.
    }
}
} // namespace ext::ffxiv::jitter
