#include "pch.h"

#include "ext/upscaler/UpscalerSeams.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/game/Game.h"
#include "ext/game/GameInterfaces.h"
#include "ext/upscaler/MipBias.h"
#include "ext/upscaler/NgxThreadTrace.h"

#include <upscalers/IFeature.h>

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

// NVSDK_NGX_DLSS_Feature_Flags_MVLowRes of this create (IFeature::SetInitParameters reads the same parameter): the
// automatic mipmap bias counts the output scaling multiplier only when the motion vectors are low resolution (or render
// equals display), the condition the upscalers themselves use before they enlarge their output.
bool MvLowResFlag(NVSDK_NGX_Parameter* params)
{
    int flags = 0;
    if (params == nullptr || params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags) != NVSDK_NGX_Result_Success)
        return false;

    return (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
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

    // Also unconditional and before every early exit below: the automatic mipmap bias (ext/upscaler/MipBias.h)
    // follows every upscaler feature create, whether or not a preset is forced, native DLAA included.
    mipbias::OnFeatureCreate(width, height, outWidth, outHeight, MvLowResFlag(params));
    ext::upscaler::LogCreateThread(width, height, outWidth, outHeight);

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

namespace
{
// C2/C3's gate is structural (game::Current().geometry != nullptr, never true for any game but a matched FFXIV
// build) plus the RenderSizeFromWidthHeight config flag. CorrectRenderSubrect checks both itself, before touching
// anything, so a closed gate is a complete no-op (not even the extra GetRenderResolution call) -- it does not rely
// on ResolveRenderGeometry's own (necessarily per-call) gating the way the oracle's unconditional CorrectRenderSubrect
// could.
bool RenderGeometryGateOpen()
{
    return game::Current().geometry != nullptr && Cfg().upscaler.renderSizeFromWidthHeight.value_or_default();
}

// Ext-side "has this answer already been logged" state for the two diagnostics below (docs/SEAMS-catalog.md
// "upscaler": no IFeature.h field for the oracle's _reportedEvaluateGeometry). A single last-feature/last-answer
// slot, not a container (SEAMS 3.1 forbids containers on this per-frame path): correct for the common case of one
// feature evaluating render geometry per frame. If two features interleave frame-by-frame (e.g. a dual SR/FG split)
// each can re-log its first answer on every swap instead of logging once per feature -- a cosmetic difference from
// the oracle's per-instance bool, not a behavioural one (the corrected width/height themselves are unaffected).
struct GeometryLogState
{
    const IFeature* feature = nullptr;
    unsigned int width = 0;
    unsigned int height = 0;
};

GeometryLogState& CorrectionLogState()
{
    static GeometryLogState s;
    return s;
}

GeometryLogState& ReportedLogState()
{
    static GeometryLogState s;
    return s;
}
} // namespace

bool ResolveRenderGeometry(IFeature* feature, const NVSDK_NGX_Parameter* InParameters, unsigned int* OutWidth,
                            unsigned int* OutHeight)
{
    auto* geo = game::Current().geometry;
    if (geo == nullptr || feature == nullptr || InParameters == nullptr || OutWidth == nullptr || OutHeight == nullptr)
        return false; // not a matched FFXIV build: upstream's subrect-report-only behaviour, completely untouched

    // U3 H5a (deferred to C2/C3 by C1/C5): unconditional, independent of the config flag below -- mirrors the
    // oracle's EnsureFfxivNativeQuality() call here, through the cached-pointer path ApplyForcedQuality also uses.
    if (auto* quality = game::Current().quality)
        quality->EnsureHooks();

    unsigned int paramWidth = 0, paramHeight = 0;
    const bool haveParams = InParameters->Get(NVSDK_NGX_Parameter_Width, &paramWidth) == NVSDK_NGX_Result_Success &&
                             InParameters->Get(NVSDK_NGX_Parameter_Height, &paramHeight) == NVSDK_NGX_Result_Success;

    const unsigned int subrectWidth = *OutWidth;
    const unsigned int subrectHeight = *OutHeight;
    const bool corrected = haveParams && geo->ResolveRenderExtent(paramWidth, paramHeight, *OutWidth, *OutHeight);

    if (corrected)
    {
        auto& state = CorrectionLogState();
        if (state.feature != feature || state.width != *OutWidth || state.height != *OutHeight)
        {
            state.feature = feature;
            state.width = *OutWidth;
            state.height = *OutHeight;
            LOG_INFO("Evaluate geometry: the game reports a {}x{} subrect but Width/Height of {}x{}. Taking {}x{} "
                     "as the render size -- the subrect is this game's buffer, not its frame.",
                     subrectWidth, subrectHeight, paramWidth, paramHeight, *OutWidth, *OutHeight);
        }
    }

    // Unconditional (U3 H5c), independent of whether a correction fired this call: once per distinct answer, not
    // per frame. `feature`'s render/display size are read before GetRenderResolution's own `_renderWidth =
    // *OutWidth;` assignment runs (this seam is called before that line), so RenderWidth()/RenderHeight() here are
    // still the previous answer.
    {
        auto& state = ReportedLogState();
        if (state.feature != feature || state.width != *OutWidth || state.height != *OutHeight)
        {
            state.feature = feature;
            state.width = *OutWidth;
            state.height = *OutHeight;
            LOG_INFO("Evaluate geometry from the game: render {}x{}. This feature was created for render {}x{} "
                     "display {}x{}.",
                     *OutWidth, *OutHeight, feature->RenderWidth(), feature->RenderHeight(), feature->DisplayWidth(),
                     feature->DisplayHeight());
        }
    }

    return corrected;
}

void CorrectRenderSubrect(IFeature* feature, NVSDK_NGX_Parameter* InParameters)
{
    if (feature == nullptr || InParameters == nullptr || !RenderGeometryGateOpen())
        return; // closed gate: complete no-op, not even the extra GetRenderResolution call below

    unsigned int width = 0, height = 0;
    Access::GetRenderResolution(*feature, InParameters, width, height);

    unsigned int subrectWidth = 0, subrectHeight = 0;
    if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &subrectWidth) ==
            NVSDK_NGX_Result_Success &&
        InParameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &subrectHeight) ==
            NVSDK_NGX_Result_Success &&
        (subrectWidth != width || subrectHeight != height))
    {
        LOG_DEBUG("Correcting the render subrect the game set: {}x{} -> {}x{}", subrectWidth, subrectHeight, width,
                  height);
        InParameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, width);
        InParameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, height);
    }
}
} // namespace ext::upscaler
