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

// "MB Scale Override" (Config::MipmapBiasScaleOverride): the hooks multiply the game's bias by the override in that mode,
// which turns the automatic (negative) value into a blur, so auto does not engage while it is on.
bool ScaleOverrideOn() { return Config::Instance()->MipmapBiasScaleOverride.value_or_default(); }

// Caller holds the mutex. The config load normally configured the policy long before the first create; if it ran before
// this file's start-up registration (so its hook call was missed), catch up from the stored settings now.
void EnsureConfigured()
{
    auto& policy = CurrentPolicy();
    if (policy.configured)
        return;

    const auto& settings = Cfg().upscaler;
    const Outcome o = Configure(policy, settings.mipBiasAuto.value_or_default(),
                                settings.mipBiasAutoOffset.value_or_default(), ScaleOverrideOn());
    Write(o);

    if (o.warn) // only called from a create, so the logger exists
        LOG_WARN("{}", ScaleModeWarning());
}

// The outcome of a settings change or of the Scale override flipping: one WARN line when auto is blocked by the Scale
// override (once per episode), else the INFO line of the change. Caller holds the mutex.
void LogSetting(const Outcome& o, bool enabled)
{
    if (o.warn)
        LOG_WARN("{}", ScaleModeWarning());
    else if (o.action != Action::None)
        LOG_INFO("{}", SettingLine(o, enabled));
}
} // namespace

void OnConfigLoaded(bool enabled, float offset)
{
    std::scoped_lock lock(PolicyMutex());
    auto& policy = CurrentPolicy();
    const Outcome o = Configure(policy, enabled, offset, ScaleOverrideOn());
    Write(o);

    // No log here (the logger may not exist yet): the warning this load owes is left for the first create or the first
    // look of the menu card. Only the load that started the episode re-arms it; a later load in the same episode (every
    // Config::Reload) must not make the warning come again.
    if (o.warn)
        policy.warned = false;
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
    create.scaleMode = ScaleOverrideOn();

    std::scoped_lock lock(PolicyMutex());
    EnsureConfigured();

    const Outcome o = OnRawCreate(CurrentPolicy(), create);
    Write(o);

    if (o.warn)
        LOG_WARN("{}", ScaleModeWarning());

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
    const Outcome o = Configure(CurrentPolicy(), enabled, offset, ScaleOverrideOn());
    Write(o);
    LogSetting(o, enabled);
}

void OnScaleOverrideMaybeChanged()
{
    const bool scale = ScaleOverrideOn();

    std::scoped_lock lock(PolicyMutex());
    auto& policy = CurrentPolicy();
    if (!policy.configured)
        return;

    // Nothing to do while the policy has seen the current state, except the warning a config load left pending.
    if (policy.scaleMode == scale && !(policy.enabled && scale && !policy.warned))
        return;

    const Outcome o = Configure(policy, policy.enabled, policy.offset, scale);
    Write(o);
    LogSetting(o, policy.enabled);
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
