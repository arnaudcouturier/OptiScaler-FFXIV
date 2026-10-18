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
    bool enabled = false;  // [Mipmap] MipmapBiasAuto
    double offset = 0.0;   // [Mipmap] MipmapBiasAutoOffset, already clamped
    bool engaged = false;  // the override currently holds this policy's value
    bool haveSizes = false;
    Sizes sizes {};        // the last ACCEPTED create (remembered even while auto is off)
    double outputScaling = 1.0;
    double published = 0.0; // the final bias the override holds while engaged
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
    bool log = false;      // OnCreate: the glue logs this outcome (auto is on)
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
} // namespace detail

// The settings changed: config load (it runs again on every Config::Reload, so it is idempotent), the menu checkbox,
// the menu slider. Turning auto on engages the override at once from the last accepted create (or from 0 plus the
// offset when there is none); turning it off restores the user's value; a changed offset republishes.
inline Outcome Configure(Policy& p, bool enabled, double offset)
{
    Outcome o;
    p.enabled = enabled;
    p.offset = ClampOffset(offset);
    detail::Describe(o, p);

    if (!enabled)
    {
        if (p.engaged)
        {
            o.was = p.published;
            p.engaged = false;
            p.published = 0.0;
            o.action = Action::Restore;
            o.changed = true;
        }
        return o;
    }

    detail::Follow(o, p);
    return o;
}

// An upscaler feature was created (IFeature::SetInitParameters, through ApplyForcedQuality). Raw NGX parameters in.
// The sizes are remembered even while auto is off, so ticking the checkbox mid-session applies the current ratio.
inline Outcome OnCreate(Policy& p, unsigned width, unsigned height, unsigned outWidth, unsigned outHeight,
                        bool extendedLimits, double outputScaling)
{
    Outcome o;
    o.log = p.enabled;

    const auto check = CreateSizes(width, height, outWidth, outHeight, extendedLimits);
    if (check.reject != Reject::None)
    {
        o.reject = check.reject;
        o.sizes = { width, height, outWidth, outHeight };
        o.outputScaling = p.outputScaling;
        o.offset = p.offset;
        return o;
    }

    p.haveSizes = true;
    p.sizes = check.sizes;
    p.outputScaling = detail::CleanScaling(outputScaling);

    o.accepted = true;
    detail::Describe(o, p);
    if (p.enabled)
        detail::Follow(o, p);

    return o;
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
};

inline Status StatusOf(const Policy& p)
{
    Status s;
    s.enabled = p.enabled;
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
} // namespace ext::upscaler::mipbias
