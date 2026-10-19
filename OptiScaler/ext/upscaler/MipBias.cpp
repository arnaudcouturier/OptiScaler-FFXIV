#include "pch.h"

#include "ext/upscaler/MipBias.h"

#include "ext/core/ExtConfig.h"

#include <State.h>

#include <mutex>

// Glue of the automatic mipmap bias (docs/FEATURES.md "Automatic mipmap bias"). The decisions are in MipBias.h (pure,
// unit-tested); this file only reads the config, writes upstream's volatile `Config::MipmapBiasOverride` (which the
// three sampler hooks already apply, additive by default) and logs.
namespace ext::upscaler::mipbias
{
namespace
{
// Touched from the thread that creates the feature, the config load and the menu: a handful of times per session.
std::mutex& PolicyMutex()
{
    static std::mutex m;
    return m;
}

Policy& CurrentPolicy()
{
    static Policy policy;
    return policy;
}

// Caller holds the mutex.
void Write(const Outcome& o)
{
    if (!Apply(Config::Instance()->MipmapBiasOverride, o))
        return;

    // The menu's "created since the last change" readout (State::lastMipBias / lastMipBiasMax) starts over.
    if (o.changed)
    {
        State::Instance().lastMipBias = 100.0f;
        State::Instance().lastMipBiasMax = -100.0f;
    }
}

// Caller holds the mutex. The config load normally configured the policy long before the first create; if it ran before
// this file's start-up registration (so its hook call was missed), catch up from the stored settings now.
void EnsureConfigured()
{
    auto& policy = CurrentPolicy();
    if (policy.configured)
        return;

    const auto& settings = Cfg().upscaler;
    Write(Configure(policy, settings.mipBiasAuto.value_or_default(), settings.mipBiasAutoOffset.value_or_default()));
}
} // namespace

void OnConfigLoaded(bool enabled, float offset)
{
    std::scoped_lock lock(PolicyMutex());
    Write(Configure(CurrentPolicy(), enabled, offset));
}

void OnFeatureCreate(unsigned width, unsigned height, unsigned outWidth, unsigned outHeight, bool lowResMvFlag)
{
    const auto* config = Config::Instance();

    RawCreate create;
    create.width = width;
    create.height = height;
    create.outWidth = outWidth;
    create.outHeight = outHeight;
    create.extendedLimits = config->ExtendedLimits.value_or_default();
    create.outputScalingEnabled = config->OutputScalingEnabled.value_or_default();
    create.outputScalingMultiplier = static_cast<double>(config->OutputScalingMultiplier.value_or_default());
    create.lowResMv =
        EffectiveLowResMv(lowResMvFlag, config->DisplayResolution.has_value(), config->DisplayResolution.value_or(false));

    std::scoped_lock lock(PolicyMutex());
    EnsureConfigured();

    const Outcome o = OnRawCreate(CurrentPolicy(), create);
    Write(o);

    if (!o.log)
        return; // auto is off: the sizes are remembered, nothing is logged

    if (o.accepted)
        LOG_INFO("{}", CreateLine(o));
    else
        LOG_INFO("{}", IgnoredLine(o));
}

void OnSettingChanged()
{
    const auto& settings = Cfg().upscaler;
    const bool enabled = settings.mipBiasAuto.value_or_default();
    const float offset = settings.mipBiasAutoOffset.value_or_default();

    std::scoped_lock lock(PolicyMutex());
    const Outcome o = Configure(CurrentPolicy(), enabled, offset);
    Write(o);

    if (o.action != Action::None)
        LOG_INFO("{}", SettingLine(o, enabled));
}

Status CurrentStatus()
{
    std::scoped_lock lock(PolicyMutex());
    return StatusOf(CurrentPolicy());
}

namespace
{
// Registers the config-load hook (the one the upscaler config's post-load step calls) when the DLL's static
// initialisers run, the way the menu cards register themselves; the config is read later, from Config::Instance().
[[maybe_unused]] const bool kLoadHookRegistered = (UpscalerConfig::onLoaded = &OnConfigLoaded, true);
} // namespace
} // namespace ext::upscaler::mipbias
