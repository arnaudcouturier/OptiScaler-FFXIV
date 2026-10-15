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
// [Hotfix] RenderSizeFromWidthHeight (S7 C2/C3): restores the oracle's adfbea9d gate, default off, for the
// per-axis-min render-geometry correction (ResolveRenderGeometry/CorrectRenderSubrect, docs/SEAMS.md 5). Same ini
// section/key as the oracle's Config::Instance()->RenderSizeFromWidthHeight.
struct UpscalerConfig
{
    CustomOptional<int32_t> forcePerfQuality { -1 };
    CustomOptional<bool> renderSizeFromWidthHeight { false };

    void Bind(config::Binder& b);
};
} // namespace ext
