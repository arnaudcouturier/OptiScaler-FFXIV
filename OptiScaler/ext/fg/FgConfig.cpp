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
}
} // namespace ext
