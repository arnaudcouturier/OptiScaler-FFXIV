#include "pch.h"

#include "ext/fg/FgConfig.h"

#include "ext/core/ExtConfig.h"

namespace ext
{
void FgConfig::Bind(config::Binder& b)
{
    b.Add("DLSSG", "AdaMfgUnlock", adaMfgUnlock);

    b.Add("DLSSG", "AmpereMfgUnlock", ampereMfgUnlock);
    b.Add("DLSSG", "AmpereMfgMaxFrames", ampereMfgMaxFrames);
    b.Add("DLSSG", "AmpereMfgKernelImage", ampereMfgKernelImage);
    b.Add("DLSSG", "AmpereMfgRouter", ampereMfgRouter);
    b.Add("DLSSG", "AmpereMfgHardwareBilinear", ampereMfgHardwareBilinear);
    b.Add("DLSSG", "AmpereMfgOptimized", ampereMfgOptimized);
    b.Add("DLSSG", "AmpereMfgPreset", ampereMfgPreset);
    b.Add("DLSSG", "AmpereMfgSpoofArchToGame", ampereMfgSpoofArchToGame);
    b.Add("DLSSG", "AmpereMfgLogLevel", ampereMfgLogLevel);

    // D5: upstream members (Config.h) whose ini persistence under [LowLatency] is the fork's. Read after upstream's own
    // [fakenvapi] keys (set_from_config only fills an unset value), out-of-range values are ignored.
    b.AddEnum("LowLatency", "Input", Config::Instance()->LowLatencyInput, ::LowLatencyInput::_);
    b.AddEnum("LowLatency", "Output", Config::Instance()->LowLatencyOutput,
              static_cast<LowLatencyMode>(static_cast<uint32_t>(LowLatencyMode::Reflex) + 1));
}
} // namespace ext
