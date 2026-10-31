#include "pch.h"

#include "ext/fg/FgConfig.h"

#include "ext/core/ExtConfig.h"
#include "ext/fg/FrameLimitSanity.h"

#include <cmath>

namespace ext
{
void FgConfig::Bind(config::Binder& b)
{
    b.Add("DLSSG", "AdaMfgUnlock", adaMfgUnlock);

    b.Add("DLSSG", "UIRecomposition", uiRecomposition);
    b.Add("DLSSG", "RenderPreset", renderPreset);

    b.Add("DLSSG", "AutoMFG", autoMfg);
    b.Add("DLSSG", "AutoMFGMax", autoMfgMax);
    b.Add("DLSSG", "AutoMFGTarget", autoMfgTarget);
    b.Add("DLSSG", "AutoMFGSelfTest", autoMfgSelfTest);

    b.Add("DLSSG", "AmpereMfgUnlock", ampereMfgUnlock);
    b.Add("DLSSG", "AmpereMfgMaxFrames", ampereMfgMaxFrames);
    b.Add("DLSSG", "AmpereMfgKernelImage", ampereMfgKernelImage);
    b.Add("DLSSG", "AmpereMfgRouter", ampereMfgRouter);
    b.Add("DLSSG", "AmpereMfgHardwareBilinear", ampereMfgHardwareBilinear);
    b.Add("DLSSG", "AmpereMfgOptimized", ampereMfgOptimized);
    b.Add("DLSSG", "AmpereMfgPreset", ampereMfgPreset);
    b.Add("DLSSG", "AmpereMfgSpoofArchToGame", ampereMfgSpoofArchToGame);
    b.Add("DLSSG", "AmpereMfgLogLevel", ampereMfgLogLevel);

    // D3: fork keys of the [XeFG] section (also written by the SaveXeFG quick-save: section name "XeFG").
    b.Add("XeFG", "DynamicMFG", xefgDynamic);
    b.Add("XeFG", "DynamicTargetFPS", xefgDynamicTarget);
    b.Add("XeFG", "UnlockMFG", xefgUnlock);
    b.Add("XeFG", "MaxInterpolatedFrames", xefgMaxInterpolatedFrames);
    b.Add("XeFG", "ExtraPacing", xefgExtraPacing);
    b.Add("XeFG", "ResetOnUpscalerChange", xefgResetOnUpscalerChange);

    // FSR output: a frame ID that rises by exactly 1 per dispatch (see FgConfig.h).
    b.Add("FrameGen", "FsrMonotonicFrameId", fsrMonotonicFrameId);

    // D5: upstream members (Config.h) whose ini persistence under [LowLatency] is the fork's. Read after upstream's own
    // [fakenvapi] keys (set_from_config only fills an unset value), out-of-range values are ignored.
    b.AddEnum("LowLatency", "Input", Config::Instance()->LowLatencyInput, ::LowLatencyInput::_);
    b.AddEnum("LowLatency", "Output", Config::Instance()->LowLatencyOutput,
              static_cast<LowLatencyMode>(static_cast<uint32_t>(LowLatencyMode::Reflex) + 1));

    // [FrameGen]: the live output switch and its developer measurement.
    b.Add("FrameGen", "FGOutputHotSwap", outputHotSwap);
    b.Add("FrameGen", "FGOutputSwitchProbe", outputSwitchProbe);
    b.Add("FrameGen", "FGOutputSwitchScript", outputSwitchScript);
    b.Add("FrameGen", "FGOutputSwitchFault", outputSwitchFault);
}

void FgConfig::PostLoad()
{
    // Upstream only rejects InterpolationCount < 1; the provider's ceiling is kXeFgMaxInterpolations.
    auto& count = Config::Instance()->FGXeFGInterpolationCount;
    if (count.has_value() && count.value() > kXeFgMaxInterpolations)
        count.reset();

    // WS2: the render preset is 0 (Auto) or 1..3 (A, B, C); anything else means unset.
    if (renderPreset.has_value() && (renderPreset.value() < 0 || renderPreset.value() > 3))
        renderPreset.reset();

    // Dynamic MFG: the multiplier range is 2x..4x, the target 0 (auto) or a frame rate. Anything else means unset.
    if (autoMfgMax.has_value() && (autoMfgMax.value() < 2 || autoMfgMax.value() > 4))
        autoMfgMax.reset();
    if (autoMfgTarget.has_value() &&
        (!std::isfinite(autoMfgTarget.value()) || autoMfgTarget.value() < 0.0f || autoMfgTarget.value() > 1000.0f))
        autoMfgTarget.reset();

    if (xefgMaxInterpolatedFrames.has_value() &&
        (xefgMaxInterpolatedFrames.value() < 1 || xefgMaxInterpolatedFrames.value() > kXeFgMaxInterpolations))
        xefgMaxInterpolatedFrames.reset();

    // QS: upstream's [Framerate] FramerateLimit accepts whatever the ini parses (nan, inf, -5). The limiter converts
    // 1e6 / limit to an unsigned integer, which is undefined for those. Unset means "no cap" and Save writes `auto`.
    // This covers ini-borne values only (this runs at load): the VRR calculator's "Set as FPS Limit" assigns the value at run
    // time and is not sanitised here (see FrameLimitSanity.h).
    auto& limit = Config::Instance()->FramerateLimit;
    if (limit.has_value() && !present::IsUsableFrameLimit(limit.value()))
        limit.reset();
}
} // namespace ext
