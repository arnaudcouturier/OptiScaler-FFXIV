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

    // [Mipmap] MipmapBiasAuto / MipmapBiasAutoOffset (ext/upscaler/MipBias.h): when on, the mipmap bias override follows
    // log2(render width / display width) of the latest upscaler create, plus the offset (limited to +-3). Default off.
    // Comment block lives in ext/ini/Mipmap.ini.
    CustomOptional<bool> mipBiasAuto { false };
    CustomOptional<float> mipBiasAutoOffset { 0.0f };

    void Bind(config::Binder& b);
    void PostLoad(); // end of config::Load (defined in ext/upscaler/MipBias.cpp): engages the automatic mip bias
};
} // namespace ext
