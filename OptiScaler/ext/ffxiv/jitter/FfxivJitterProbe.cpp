#include "pch.h"

#include "ext/ffxiv/jitter/FfxivJitter.h"
#include "ext/ffxiv/jitter/FfxivJitterCore.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/ProcessMemory.h"
#include "ext/upscaler/UpscalerSeams.h"

#include <Config.h>
#include <State.h>
#include <framegen/IFGFeature_Dx12.h>

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
};
} // namespace

void OnUpscaleBoundary(NVSDK_NGX_Parameter* parameters) noexcept
{
    try
    {
        // Both keys off: this is all it costs (the core repeats the key gate, which a test pins, so it never runs on its own either).
        const auto& cfg = ext::Cfg().ffxiv;
        const bool probe = cfg.jitterProbe.value_or_default();
        const bool correct = cfg.jitterCorrection.value_or_default();
        if ((!probe && !correct) || parameters == nullptr)
            return;
        NgxEnvironment env { parameters };
        S().engine.Step(env, probe, correct, cfg.jitterScaleOverride.value_or_default());
    }
    catch (...)
    {
        // Never into the evaluate path. The engine already catches and logs what a step throws; this net is for what it cannot log: a lock
        // that cannot be taken, or the construction of the environment. The game's parameters stay as they are.
    }
}
} // namespace ext::ffxiv::jitter
