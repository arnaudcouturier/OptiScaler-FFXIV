#include "pch.h"

#include "ext/fg/FgConfig.h"

#include "ext/core/ExtConfig.h"

namespace ext
{
void FgConfig::Bind(config::Binder& b)
{
    b.Add("DLSSG", "AdaMfgUnlock", adaMfgUnlock);

    b.Add("DLSSG", "UIRecomposition", uiRecomposition);

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

    // D5: upstream members (Config.h) whose ini persistence under [LowLatency] is the fork's. Read after upstream's own
    // [fakenvapi] keys (set_from_config only fills an unset value), out-of-range values are ignored.
    b.AddEnum("LowLatency", "Input", Config::Instance()->LowLatencyInput, ::LowLatencyInput::_);
    b.AddEnum("LowLatency", "Output", Config::Instance()->LowLatencyOutput,
              static_cast<LowLatencyMode>(static_cast<uint32_t>(LowLatencyMode::Reflex) + 1));
}

void FgConfig::PostLoad()
{
    // Upstream only rejects InterpolationCount < 1; the provider's ceiling is kXeFgMaxInterpolations.
    auto& count = Config::Instance()->FGXeFGInterpolationCount;
    if (count.has_value() && count.value() > kXeFgMaxInterpolations)
        count.reset();

    if (xefgMaxInterpolatedFrames.has_value() &&
        (xefgMaxInterpolatedFrames.value() < 1 || xefgMaxInterpolatedFrames.value() > kXeFgMaxInterpolations))
        xefgMaxInterpolatedFrames.reset();
}
} // namespace ext
