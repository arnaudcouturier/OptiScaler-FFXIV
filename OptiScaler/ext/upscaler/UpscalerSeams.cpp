#include "pch.h"

#include "ext/upscaler/UpscalerSeams.h"

#include "ext/core/ExtConfig.h"
#include "ext/game/Game.h"

#include <mutex>

namespace ext::upscaler
{
namespace
{
static_assert(static_cast<int>(NVSDK_NGX_PerfQuality_Value_DLAA) == 5,
              "ForcePerfQuality's ini range (0..5) and the menu's preset table assume DLAA is value 5");
constexpr int kMaxPerfQuality = static_cast<int>(NVSDK_NGX_PerfQuality_Value_DLAA);

bool NativeDlaaRequired()
{
    auto* quality = game::Current().quality;
    return quality != nullptr && quality->DlaaRequired();
}

// Written from the thread the game's GetOptimalSettings callback runs on, read from the overlay and from
// ApplyForcedQuality: a mutex, not a scatter of atomics (touched a handful of times per session, same as the
// oracle's g_qualityAnswerMutex).
std::mutex& AnswerMutex()
{
    static std::mutex m;
    return m;
}

ForcedQualityStatus& Answer()
{
    static ForcedQualityStatus answer;
    return answer;
}
} // namespace

NVSDK_NGX_PerfQuality_Value EffectivePerfQuality(NVSDK_NGX_PerfQuality_Value asked)
{
    const bool dlaaRequired = NativeDlaaRequired();
    const int forced = Cfg().upscaler.forcePerfQuality.value_or_default();

    bool invalid = false;
    const int effective =
        EffectivePerfQualityValue(dlaaRequired, forced, static_cast<int>(asked), kMaxPerfQuality, invalid);

    if (invalid)
        LOG_WARN("ForcePerfQuality is {}, which is not a quality value -- leaving the game's choice alone", forced);
    else if (effective != static_cast<int>(asked))
        LOG_DEBUG("ForcePerfQuality: answering {} where the game asked for {}", effective, (int) asked);

    return static_cast<NVSDK_NGX_PerfQuality_Value>(effective);
}

void OnOptimalSettings(NVSDK_NGX_PerfQuality_Value quality, unsigned int renderWidth, unsigned int renderHeight,
                        unsigned int displayWidth, unsigned int displayHeight)
{
    std::scoped_lock lock(AnswerMutex());
    auto& answer = Answer();
    ++answer.queries;
    answer.quality = static_cast<int>(quality);
    answer.renderWidth = renderWidth;
    answer.renderHeight = renderHeight;
    answer.displayWidth = displayWidth;
    answer.displayHeight = displayHeight;
}

ForcedQualityStatus LastQualityAnswer()
{
    std::scoped_lock lock(AnswerMutex());
    return Answer();
}

bool ApplyForcedQuality(NVSDK_NGX_Parameter* params, unsigned int width, unsigned int height, unsigned int outWidth,
                         unsigned int outHeight, int& pq)
{
    // Unconditional and first: a user who never touches the preset setting still gets the native-quality hooks
    // installed (lazy, call_once) as soon as the first feature is created, matching the oracle's
    // EnsureFfxivNativeQuality() at the top of SetInitParameters regardless of whether a preset is forced.
    if (auto* quality = game::Current().quality)
        quality->EnsureHooks();

    const bool dlaaRequired = NativeDlaaRequired();
    const int forced = Cfg().upscaler.forcePerfQuality.value_or_default();

    if (!ShouldApplyForcedQuality(dlaaRequired, forced, pq, kMaxPerfQuality))
        return false;

    const auto answer = LastQualityAnswer();
    if (!MatchesLastAnswer(answer, width, height, outWidth, outHeight))
    {
        LOG_WARN("Leaving PerfQualityValue at the game's {}: this feature is being built for {}x{} -> {}x{}, but "
                 "the last optimal-settings answer was {}x{} -> {}x{} for quality {}. Forcing {} onto dimensions it "
                 "did not produce is what the runtime rejects. The override applies again once the game asks.",
                 pq, width, height, outWidth, outHeight, answer.renderWidth, answer.renderHeight, answer.displayWidth,
                 answer.displayHeight, answer.quality, forced);
        return false;
    }

    LOG_INFO("PerfQualityValue overrided by user: {} (game asked for {})", forced, pq);
    pq = forced;
    if (params != nullptr)
        params->Set(NVSDK_NGX_Parameter_PerfQualityValue, pq);
    return true;
}
} // namespace ext::upscaler
