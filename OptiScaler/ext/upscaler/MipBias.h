#pragma once
// Automatic mipmap bias: the pure core (docs/FEATURES.md "Automatic mipmap bias"). No upstream types here (no Config,
// no State, no logging), so it is unit-testable on its own (tests/ext/upscaler/mip_bias_*), same split as
// ForcedQuality.h.
//
// The idea: upstream already applies `[Mipmap] MipmapBiasOverride` to every sampler the game creates (additive by
// default). That number is the one thing a player has to work out by hand for the upscaling ratio in use:
//     bias = log2(render width / display width)        (the menu's mipmap calculator, "Mipmap Bias" card)
// This file computes it from the sizes of every upscaler feature create; MipBias.cpp publishes it as a volatile
// override so the existing hooks apply it unchanged.
//
// Source policy: follow the latest ACCEPTED create. Before any create the bias is 0 (plus the offset); each accepted
// create sets render/display and the bias follows it. A create that is not a game scene (zero or absurd sizes, a
// display narrower than 640 pixels, a portrait display such as the character preview's 576x960 -> 1152x1920) is
// ignored and leaves the value alone. The forced-quality setting is deliberately NOT an input: the policy could not
// tell a login screen in native DLAA from a DLAA the player chose (study "STUDY-mip-presets", section 3.2).

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace ext::upscaler::mipbias
{
// A new value is published (and logged as "changed") only when it moves by at least this much.
inline constexpr double kMinChange = 0.001;
// Config::Reload resets any MipmapBiasOverride outside +-15, so the final value never leaves that range.
inline constexpr double kBiasLimit = 15.0;
// Range of the [Mipmap] MipmapBiasAutoOffset key and of the menu slider.
inline constexpr double kOffsetLimit = 3.0;
// A display narrower than this is not a game scene.
inline constexpr unsigned kMinDisplayWidth = 640;
// IFeature::SetInitParameters zeroes any size under 20 or over 16384 before it uses it ("Thanks to Crytek").
inline constexpr unsigned kMinExtent = 20;
inline constexpr unsigned kMaxExtent = 16384;

struct Sizes
{
    unsigned renderW = 0;
    unsigned renderH = 0;
    unsigned displayW = 0;
    unsigned displayH = 0;
};

enum class Reject
{
    None,
    OutOfRange,  // a size is missing, under 20 or over 16384
    TinyDisplay, // display narrower than kMinDisplayWidth
    Portrait,    // display taller than wide
};

inline const char* RejectText(Reject reason)
{
    switch (reason)
    {
    case Reject::OutOfRange:
        return "a size is missing or outside 20..16384";
    case Reject::TinyDisplay:
        return "display narrower than 640 pixels";
    case Reject::Portrait:
        return "display taller than wide, not the main scene";
    default:
        return "";
    }
}

struct CreateCheck
{
    Reject reject = Reject::None;
    Sizes sizes {}; // only meaningful when reject == None
};

namespace detail
{
inline unsigned Extent(unsigned v) { return (v > kMaxExtent || v < kMinExtent) ? 0u : v; }
} // namespace detail

// The render and display size of a feature create, read exactly as IFeature::SetInitParameters does (the seam sees
// the NGX parameters before its range clamp, so the clamp is repeated here): without extended limits the smaller of
// each pair is the render size and the larger the display size, per axis (a game may hand Width/Height and
// OutWidth/OutHeight over swapped); with extended limits Width/Height are the render size as given, even when they
// exceed the display. A create that is not a game scene is rejected, in this order: sizes out of range, display
// narrower than kMinDisplayWidth, display taller than wide.
inline CreateCheck CreateSizes(unsigned width, unsigned height, unsigned outWidth, unsigned outHeight,
                               bool extendedLimits)
{
    width = detail::Extent(width);
    height = detail::Extent(height);
    outWidth = detail::Extent(outWidth);
    outHeight = detail::Extent(outHeight);

    Sizes s;
    if (!extendedLimits)
    {
        s.displayW = std::max(width, outWidth);
        s.displayH = std::max(height, outHeight);
        s.renderW = std::min(width, outWidth);
        s.renderH = std::min(height, outHeight);
    }
    else
    {
        s.displayW = outWidth;
        s.displayH = outHeight;
        s.renderW = width;
        s.renderH = height;
    }

    if (s.renderW == 0 || s.renderH == 0 || s.displayW == 0 || s.displayH == 0)
        return { Reject::OutOfRange, {} };
    if (s.displayW < kMinDisplayWidth)
        return { Reject::TinyDisplay, {} };
    if (s.displayH > s.displayW)
        return { Reject::Portrait, {} };

    return { Reject::None, s };
}

// The menu calculator's formula: log2(render width / display width), where the display width is multiplied by the
// output scaling multiplier when output scaling is on (`outputScaling` is 1 otherwise; zero, negative and NaN count
// as 1). Render at or above the display gives 0: more pixels than the output need no negative bias.
inline double BiasFromSizes(unsigned renderW, unsigned displayW, double outputScaling = 1.0)
{
    if (!(outputScaling > 0.0) || !std::isfinite(outputScaling))
        outputScaling = 1.0;
    if (renderW == 0 || displayW == 0)
        return 0.0;

    const double ratio = static_cast<double>(renderW) / (static_cast<double>(displayW) * outputScaling);
    if (!(ratio < 1.0))
        return 0.0;

    return std::log2(ratio);
}

// The output scaling multiplier the upscaler really applies. Upstream (upscalers/dlss/DLSSFeature.cpp:63 and the FSR / XeSS
// equivalents) enlarges its output target only when OutputScalingEnabled && (LowResMV() || RenderWidth() == DisplayWidth());
// in every other case the output has the display size and the multiplier must not enter the bias. Zero, negative, NaN
// and infinite multipliers count as 1.
inline double EffectiveOutputScaling(bool enabled, double multiplier, bool lowResMv, bool renderEqualsDisplay)
{
    if (!enabled || !(lowResMv || renderEqualsDisplay))
        return 1.0;

    return (multiplier > 0.0 && std::isfinite(multiplier)) ? multiplier : 1.0;
}

// The "LowResMV" of the feature being created, as IFeature::SetInitParameters decides it: the user's DisplayResolution
// override when there is one (display resolution motion vectors = not low res), else the MVLowRes create flag.
inline bool EffectiveLowResMv(bool createFlagLowRes, bool haveDisplayResolution, bool displayResolution)
{
    return haveDisplayResolution ? !displayResolution : createFlagLowRes;
}

inline double ClampOffset(double offset)
{
    if (!std::isfinite(offset))
        return 0.0;

    return std::clamp(offset, -kOffsetLimit, kOffsetLimit);
}

// What the override holds: the formula plus the user's offset, kept inside the range upstream accepts. Never -0.
inline double FinalBias(double base, double offset)
{
    const double sum = base + offset;
    if (!std::isfinite(sum))
        return 0.0;

    return std::clamp(sum, -kBiasLimit, kBiasLimit) + 0.0;
}

// No new value, no "changed": a move under kMinChange is not published. A value that is not a number never is.
inline bool ShouldPublish(double oldValue, double newValue)
{
    if (!std::isfinite(newValue))
        return false;
    if (!std::isfinite(oldValue))
        return true;

    return std::fabs(newValue - oldValue) >= kMinChange;
}

// ---- the policy: a small state machine; the glue (MipBias.cpp) turns its Outcomes into writes and log lines -----------
struct Policy
{
    bool configured = false; // Configure ran at least once (the glue catches up if the config load got there first)
    bool enabled = false;  // [Mipmap] MipmapBiasAuto
    double offset = 0.0;   // [Mipmap] MipmapBiasAutoOffset, already clamped
    bool engaged = false;  // the override currently holds this policy's value
    bool haveSizes = false;
    Sizes sizes {};        // the last ACCEPTED create (remembered even while auto is off)
    double outputScaling = 1.0;
    double published = 0.0; // the final bias the override holds while engaged
    // [Mipmap] MipmapBiasScaleOverride ("MB Scale Override"): the hooks then multiply the game's own bias by the override
    // instead of adding to it, so a negative automatic value would blur. Auto does not engage while it is on.
    bool scaleMode = false;
    bool warned = false;    // the warning for the current scale-mode episode was already given
};

enum class Action
{
    None,
    Engage,  // take the override over: write `value` unconditionally (first engagement, bypasses the threshold)
    Publish, // keep the override at `value`: written only if it moved or the override was emptied meanwhile
    Restore, // give the override back to the user's ini value
};

struct Outcome
{
    Action action = Action::None;
    double value = 0.0;    // Engage/Publish: the final bias the override should hold
    double was = 0.0;      // what the policy held before this outcome
    bool changed = false;  // the value moved by kMinChange or more (or was just engaged)
    bool accepted = false; // OnCreate: the create was taken into account
    Reject reject = Reject::None; // OnCreate: why it was not
    bool log = false;      // OnCreate: the glue logs this outcome (auto is on and not blocked)
    bool blocked = false;  // auto is on but cannot engage because the Scale override is on
    bool warn = false;     // blocked, and this is the first outcome of the episode: the glue logs one WARN line
    bool haveSizes = false;
    Sizes sizes {};        // accepted: render/display of the create; rejected: Width, Height, OutWidth, OutHeight as given
    double outputScaling = 1.0;
    double ratio = 1.0;    // display width * output scaling / render width
    double offset = 0.0;
};

namespace detail
{
inline double RatioOf(const Sizes& s, double outputScaling)
{
    if (s.renderW == 0)
        return 1.0;

    return static_cast<double>(s.displayW) * outputScaling / static_cast<double>(s.renderW);
}

inline double CleanScaling(double outputScaling)
{
    return (outputScaling > 0.0 && std::isfinite(outputScaling)) ? outputScaling : 1.0;
}

inline double BaseBias(const Policy& p)
{
    return p.haveSizes ? BiasFromSizes(p.sizes.renderW, p.sizes.displayW, p.outputScaling) : 0.0;
}

inline void Describe(Outcome& o, const Policy& p)
{
    o.haveSizes = p.haveSizes;
    o.sizes = p.sizes;
    o.outputScaling = p.outputScaling;
    o.ratio = RatioOf(p.sizes, p.outputScaling);
    o.offset = p.offset;
}

// Shared tail of Configure/OnCreate for an enabled policy: engage on first use, else publish when it moved.
inline void Follow(Outcome& o, Policy& p)
{
    const double target = FinalBias(BaseBias(p), p.offset);
    o.was = p.published;

    if (!p.engaged)
    {
        p.engaged = true;
        p.published = target;
        o.action = Action::Engage;
        o.value = target;
        o.changed = true;
        return;
    }

    o.changed = ShouldPublish(p.published, target);
    if (o.changed)
        p.published = target;
    o.action = Action::Publish;
    o.value = p.published;
}

// What the policy should do now: follow when auto is on and the Scale override is off; otherwise give the override back
// if it holds this policy's value, and (auto on, Scale on) say so once per episode.
inline void Reconcile(Outcome& o, Policy& p)
{
    if (p.enabled && !p.scaleMode)
    {
        p.warned = false;
        Follow(o, p);
        return;
    }

    if (p.engaged)
    {
        o.was = p.published;
        p.engaged = false;
        p.published = 0.0;
        o.action = Action::Restore;
        o.changed = true;
    }

    o.blocked = p.enabled && p.scaleMode;
    if (o.blocked && !p.warned)
    {
        p.warned = true;
        o.warn = true;
    }
    else if (!o.blocked)
    {
        p.warned = false;
    }
}
} // namespace detail

// The settings changed: config load (it runs again on every Config::Reload, so it is idempotent), the menu checkbox,
// the menu slider. Turning auto on engages the override at once from the last accepted create (or from 0 plus the
// offset when there is none); turning it off restores the user's value; a changed offset republishes.
// `scaleMode` is the Scale override ("MB Scale Override"): while it is on, auto does not engage (see Policy::scaleMode).
// `scaleMode` has no default on purpose: a caller that forgot to hand over the live Scale override would compile and engage auto in
// Scale mode (a blur); the glue pins every call site (tests/ext/upscaler/mip_bias_seam_test.cpp).
inline Outcome Configure(Policy& p, bool enabled, double offset, bool scaleMode)
{
    Outcome o;
    p.configured = true;
    p.enabled = enabled;
    p.offset = ClampOffset(offset);
    p.scaleMode = scaleMode;
    detail::Describe(o, p);
    detail::Reconcile(o, p);
    return o;
}

// An upscaler feature was created (IFeature::SetInitParameters, through ApplyForcedQuality). Raw NGX parameters in.
// The sizes are remembered even while auto is off, so ticking the checkbox mid-session applies the current ratio.
inline Outcome OnCreate(Policy& p, unsigned width, unsigned height, unsigned outWidth, unsigned outHeight,
                        bool extendedLimits, double outputScaling, bool scaleMode)
{
    Outcome o;
    const bool modeChanged = p.scaleMode != scaleMode;
    p.scaleMode = scaleMode;
    o.log = p.enabled && !p.scaleMode;

    const auto check = CreateSizes(width, height, outWidth, outHeight, extendedLimits);
    if (check.reject != Reject::None)
    {
        o.reject = check.reject;
        o.sizes = { width, height, outWidth, outHeight };
        o.outputScaling = p.outputScaling;
        o.offset = p.offset;
        if (modeChanged)
            detail::Reconcile(o, p); // the Scale override flipped since the last event: engage or give back now
        return o;
    }

    p.haveSizes = true;
    p.sizes = check.sizes;
    p.outputScaling = detail::CleanScaling(outputScaling);

    o.accepted = true;
    detail::Describe(o, p);
    detail::Reconcile(o, p);

    return o;
}

// What the glue reads for one create: the raw NGX sizes plus the settings that decide the output scaling. OnRawCreate is
// everything the glue does to them besides taking the config, the lock and the log, so it is tested as a whole.
struct RawCreate
{
    unsigned width = 0;
    unsigned height = 0;
    unsigned outWidth = 0;
    unsigned outHeight = 0;
    bool extendedLimits = false;
    bool outputScalingEnabled = false;
    double outputScalingMultiplier = 1.0;
    bool lowResMv = false; // already resolved by EffectiveLowResMv
    bool scaleMode = false; // [Mipmap] MipmapBiasScaleOverride
};

inline Outcome OnRawCreate(Policy& p, const RawCreate& c)
{
    const auto check = CreateSizes(c.width, c.height, c.outWidth, c.outHeight, c.extendedLimits);
    const bool renderEqualsDisplay = check.reject == Reject::None && check.sizes.renderW == check.sizes.displayW;
    const double scaling = EffectiveOutputScaling(c.outputScalingEnabled, c.outputScalingMultiplier, c.lowResMv,
                                                  renderEqualsDisplay);

    return OnCreate(p, c.width, c.height, c.outWidth, c.outHeight, c.extendedLimits, scaling, c.scaleMode);
}

// ---- text: the log lines the in-game check reads and the menu card ---------------------------------------------------
namespace detail
{
// Values that round to zero print as 0, never as -0.
inline double Tidy(double v, double halfStep) { return std::fabs(v) < halfStep ? 0.0 : v; }
inline std::string Bias4(double v) { return std::format("{:.4f}", Tidy(v, 0.00005)); }
inline std::string Bias3(double v) { return std::format("{:.3f}", Tidy(v, 0.0005)); }
inline std::string Signed2(double v) { return std::format("{:+.2f}", Tidy(v, 0.005)); }
} // namespace detail

// One INFO line for every accepted create while auto is on. "bias" is the value the override holds: the formula plus
// the offset. Every create being logged makes a stale-size create show up as flapping.
inline std::string CreateLine(const Outcome& o)
{
    using namespace detail;
    std::string line = std::format("Automatic mipmap bias: create render {}x{}, display {}x{}", o.sizes.renderW,
                                   o.sizes.renderH, o.sizes.displayW, o.sizes.displayH);
    if (o.outputScaling != 1.0)
        line += std::format(" (x{:.2f} output scaling)", o.outputScaling);

    line += std::format(", ratio {:.3f} -> bias {}", o.ratio, Bias4(o.value));
    if (o.changed)
        line += std::format(" (was {}, offset {}); applies to samplers created from now on", Bias4(o.was),
                            Signed2(o.offset));
    else
        line += std::format(" (unchanged, offset {})", Signed2(o.offset));

    return line;
}

// A create that was not taken into account (while auto is on), with the sizes exactly as the game gave them.
inline std::string IgnoredLine(const Outcome& o)
{
    return std::format("Automatic mipmap bias: ignored create {}x{} -> {}x{}: {}", o.sizes.renderW, o.sizes.renderH,
                       o.sizes.displayW, o.sizes.displayH, RejectText(o.reject));
}

// The one WARN line of a Scale-override episode (auto is on, "MB Scale Override" is on): nothing is applied.
inline std::string ScaleModeWarning()
{
    return "Automatic mipmap bias: not applied while MB Scale Override is on (Scale multiplies the game's own bias by the "
           "value, so a sharpening value would blur); the manual setting stays in force. Turn Scale Override off to use "
           "the automatic bias";
}

// The red line of the menu card while auto is on and the Scale override blocks it; empty otherwise.
inline std::string BlockedLine(bool blocked)
{
    return blocked ? "Automatic bias needs additive mode; MB Scale Override is on." : "";
}

// The note under the card: how the automatic value meets the upstream buttons and modes, and the known limits.
inline std::string CardNote()
{
    return "While this is on, \"Set\" and \"Reset\" in the Mipmap Bias card above are overwritten at the next quality change. "
           "Fixed replaces the game's own per-sampler biases with the single value; Scale cannot be used with it. "
           "Turn it off from the menu while no level is loading, or edit the ini: the texture hooks read the value without "
           "a lock. A screen turned to portrait is not followed, because a display taller than wide is ignored.";
}

// The line for the menu checkbox / slider (Configure's outcome); `on` is the new state of the checkbox.
inline std::string SettingLine(const Outcome& o, bool on)
{
    using namespace detail;
    if (!on)
        return "Automatic mipmap bias: off, the manual setting is back in force";

    if (!o.haveSizes)
        return std::format("Automatic mipmap bias: on, offset {}, bias {} until the next upscaler create",
                           Signed2(o.offset), Bias4(o.value));

    return std::format("Automatic mipmap bias: on, offset {}, bias {} from the last create (render {}x{}, display "
                       "{}x{})",
                       Signed2(o.offset), Bias4(o.value), o.sizes.renderW, o.sizes.renderH, o.sizes.displayW,
                       o.sizes.displayH);
}

struct Status
{
    bool enabled = false;
    bool haveSizes = false;
    Sizes sizes {};
    double outputScaling = 1.0;
    double ratio = 1.0;
    double bias = 0.0; // what the override holds (formula plus offset)
    double offset = 0.0;
    bool blocked = false; // auto is on but the Scale override stops it from engaging
};

inline Status StatusOf(const Policy& p)
{
    Status s;
    s.enabled = p.enabled;
    s.blocked = p.enabled && p.scaleMode;
    s.haveSizes = p.haveSizes;
    s.sizes = p.sizes;
    s.outputScaling = p.outputScaling;
    s.ratio = detail::RatioOf(p.sizes, p.outputScaling);
    s.bias = p.engaged ? p.published : 0.0;
    s.offset = p.offset;
    return s;
}

// First status line of the menu card.
inline std::string StatusLine(const Status& s)
{
    using namespace detail;
    if (!s.haveSizes)
        return std::format("No upscaler created yet: bias {} until then, offset {}.", Bias3(s.bias),
                           Signed2(s.offset));

    std::string scaling;
    if (s.outputScaling != 1.0)
        scaling = std::format(", output scaling x{:.2f}", s.outputScaling);

    return std::format("Ratio {:.2f}x ({}x{} to {}x{}{}): bias {}, offset {}.", s.ratio, s.sizes.renderW,
                       s.sizes.renderH, s.sizes.displayW, s.sizes.displayH, scaling, Bias3(s.bias),
                       Signed2(s.offset));
}

// Second status line: the min/max of the biases the sampler hooks created since the last change
// (State::lastMipBias / lastMipBiasMax, reset to 100 / -100).
inline std::string CreatedLine(float minBias, float maxBias)
{
    if (minBias > maxBias)
        return "No texture sampler created since the last change.";

    return std::format("Created since the last change: min {:.2f} / max {:.2f}", minBias, maxBias);
}

// ---- applying an Outcome to the override -----------------------------------------------------------------------------
// `Optional` is upstream's CustomOptional<float, NoDefault> (Config::MipmapBiasOverride) in the product, and a fake with
// the same members in the test (this header must not include Config.h): set_volatile_value(float), value_for_config(),
// operator=(std::optional<float>), has_value(), value(). Returns whether the override was written.
//
//  - Engage: the override becomes the policy's value. CustomOptional::set_volatile_value keeps the user's ini value
//    aside the first time (it is what Save() writes, and what value_for_config() returns), so a second Engage (the
//    config load runs on every Config::Reload) never overwrites it.
//  - Publish: written only if the override is empty (the user pressed "Reset" in the card above) or differs from the
//    policy's value by kMinChange or more. A value the user stored with "Set" is replaced the same way; the "Set" value
//    then becomes the one that is kept aside.
//  - Restore: the user's ini value goes back, through operator= so that the volatile flag is cleared (reset() would
//    leave it set). value_for_config() is the saved value while volatile and the current value otherwise, so a value
//    the user set by hand after auto engaged is kept.
template <class Optional> bool Apply(Optional& override_, const Outcome& o)
{
    switch (o.action)
    {
    case Action::Engage:
        override_.set_volatile_value(static_cast<float>(o.value));
        return true;

    case Action::Publish:
        if (override_.has_value() && !ShouldPublish(static_cast<double>(override_.value()), o.value))
            return false;

        override_.set_volatile_value(static_cast<float>(o.value));
        return true;

    case Action::Restore:
        override_ = override_.value_for_config();
        return true;

    default:
        return false;
    }
}

// ---- the glue (MipBias.cpp) -----------------------------------------------------------------------------------------
// Entry points, all thread-safe (one mutex; they run a handful of times per session).

// ExtConfig::PostLoad, through UpscalerConfig::PostLoad and its onLoaded pointer (registered by MipBias.cpp at start-up):
// after the ini was read. Runs again on every Config::Reload.
// Engages the override at once when auto is on. Does not log (it can run before the logger exists).
void OnConfigLoaded(float offset);

// ext::upscaler::ApplyForcedQuality, first statement after EnsureHooks(): every upscaler feature create, with the
// NGX Width, Height, OutWidth, OutHeight as the game gave them, and whether the DLSS create flags say the motion vectors
// are low resolution (NVSDK_NGX_DLSS_Feature_Flags_MVLowRes; the output scaling multiplier counts only then, or when
// render == display, see EffectiveOutputScaling).
void OnFeatureCreate(unsigned width, unsigned height, unsigned outWidth, unsigned outHeight, bool lowResMvFlag);

// The menu card changed the checkbox or the offset (they are already stored in ext::Cfg().upscaler).
void OnSettingChanged();

// The menu card calls this each time it is drawn (and nothing else does: the module has no per-frame hook, so a tick made while the
// card is not on screen is handed over by the next feature create): "MB Scale Override" can be ticked without any create or setting
// change in between. Acts (give the override back, or take it again, and log) only when the Scale override differs from what the
// policy last saw, or a blocked policy still owes its warning.
void OnScaleOverrideMaybeChanged();

// A copy of the policy for the menu card.
Status CurrentStatus();
} // namespace ext::upscaler::mipbias
