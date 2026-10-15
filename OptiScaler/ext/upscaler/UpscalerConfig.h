#pragma once

#include <Config.h>

namespace ext
{
namespace config
{
class Binder;
}

// [QualityOverrides] ForcePerfQuality (docs/SEAMS-catalog.md "upscaler"). -1 = leave the game's own choice alone,
// 0..5 = an NVSDK_NGX_PerfQuality_Value to force instead. Same ini section/key as the oracle's
// Config::Instance()->ForcePerfQuality, so existing inis are not broken; comment block lives in
// ext/ini/QualityOverrides.ini (the upstream OptiScaler.ini template is never edited directly).
//
// RenderSizeFromWidthHeight (the geometry gate, C2/C3) is a later wave's key; it does not live here yet.
struct UpscalerConfig
{
    CustomOptional<int32_t> forcePerfQuality { -1 };

    void Bind(config::Binder& b);
};
} // namespace ext
