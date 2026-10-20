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

    // [Mipmap] MipmapBiasAuto / MipmapBiasAutoOffset (ext/upscaler/MipBias.h, MipBind.h): when on, the mipmap bias follows
    // log2(render width / display width), plus the offset (limited to +-3). On by default for FFXIV, off for other games.
    // Comment block lives in ext/ini/Mipmap.ini.
    // No default on purpose: an explicit false must survive a save (the game default is on for FFXIV), so the key stays auto until set.
    CustomOptional<bool, NoDefault> mipBiasAuto;
    // [Mipmap] AutomaticFFXIV: accepted as an alias of MipmapBiasAuto (the key wins when both are set).
    CustomOptional<bool, NoDefault> mipBiasAutoAlias;
    // [Mipmap] MipmapBiasObserve (debug, off): log the distinct original sampler biases per upscaling ratio.
    CustomOptional<bool> mipBiasObserve { false };
    CustomOptional<float> mipBiasAutoOffset { 0.0f };

    void Bind(config::Binder& b);
    // End of config::Load. Hands the two mip bias keys to ext/upscaler/MipBias.cpp, which registers `onLoaded` at start-up.
    // A pointer rather than a call so that ExtConfig.cpp keeps no link dependency on the glue (it needs Config.h and
    // State.h): the unit tests that build ExtConfig.cpp alone find the pointer null and nothing happens.
    using LoadedHook = void (*)(float offset);
    static inline LoadedHook onLoaded = nullptr;

    void PostLoad()
    {
        if (onLoaded != nullptr)
            onLoaded(mipBiasAutoOffset.value_or_default());
    }
};
} // namespace ext
