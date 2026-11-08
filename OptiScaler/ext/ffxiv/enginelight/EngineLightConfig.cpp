#include "pch.h"

#include "ext/ffxiv/enginelight/EngineLight.h"

#include "ext/core/ExtConfig.h"

// Engine light controls: the keys' side of the module (EngineLight.h). The module's own file (EngineLightDraw.cpp) never reads the config, so the tests that link it need no ini
// reader: this file hands the keys to Configure at every config load (the hook pointer ExtConfig.cpp keeps no link dependency on) and after every change in the card.
namespace ext::ffxiv::enginelight
{
Params ParamsFromConfig()
{
    const auto& cfg = ext::Cfg().ffxiv;
    Params p;
    p.master = cfg.engineLight.value_or_default();
    p.referenceAB = cfg.engineLightReferenceAB.value_or_default();
    p.moonIntensity = cfg.engineLightMoon.value_or_default();
    p.moonTint[0] = cfg.engineLightMoonTintR.value_or_default();
    p.moonTint[1] = cfg.engineLightMoonTintG.value_or_default();
    p.moonTint[2] = cfg.engineLightMoonTintB.value_or_default();
    p.ambientIntensity = cfg.engineLightAmbient.value_or_default();
    p.ambientTint[0] = cfg.engineLightAmbientTintR.value_or_default();
    p.ambientTint[1] = cfg.engineLightAmbientTintG.value_or_default();
    p.ambientTint[2] = cfg.engineLightAmbientTintB.value_or_default();
    p.waterMoon = cfg.engineLightWaterMoon.value_or_default();
    p.waterAmbient = cfg.engineLightWaterAmbient.value_or_default();
    p.waterTint[0] = cfg.engineLightWaterTintR.value_or_default();
    p.waterTint[1] = cfg.engineLightWaterTintG.value_or_default();
    p.waterTint[2] = cfg.engineLightWaterTintB.value_or_default();
    p.lampIntensity = cfg.engineLightLamps.value_or_default();
    p.exposureMode = cfg.engineLightExposure.value_or_default();
    p.exposureManual = cfg.engineLightExposureManual.value_or_default();
    p.forceNightWeight = cfg.engineLightForceNightWeight.value_or_default();
    return Sanitize(p);
}

void Publish() { Configure(ParamsFromConfig()); }

void OnConfigLoaded(bool) { Configure(ParamsFromConfig()); }

namespace
{
// The registration of the config hook, set when this DLL's static initialisers run: the [FFXIV] EngineLight key arms the seam's feature bit at every config load.
struct RegisterConfig
{
    RegisterConfig() { ext::FfxivConfig::engineLightOnLoaded = &ext::ffxiv::enginelight::OnConfigLoaded; }
} g_registerConfig;
} // namespace
} // namespace ext::ffxiv::enginelight
