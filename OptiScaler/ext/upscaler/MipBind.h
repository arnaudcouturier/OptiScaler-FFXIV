#pragma once
// Bind-time mipmap bias: the pure decisions (docs/FEATURES.md "Automatic mipmap bias"). No upstream types, no D3D, no
// logging here, so every rule is unit-tested on its own (tests/ext/upscaler/mip_bind_*), same split as MipBias.h.
//
// The idea: FFXIV caches its sampler states, so a bias written when a sampler is created never reaches the samplers the
// game made before a quality change. Instead the bias is applied when a sampler is BOUND (ext/upscaler/MipBind*.h/.cpp):
// an eligible sampler is swapped for an immutable variant that carries the current target. This file holds
//   - the target: log2(render width / output width) from the sizes of each evaluate, 0 for DLAA;
//   - which samplers are eligible;
//   - the bias of a variant, under the two modes ADD (the game's own bias plus the target, what the manual setting does)
//     and REPLACE (the target alone, used when the game already applies a DLSS bias of its own);
//   - the detector that tells the two modes apart from the original biases the game uses at different ratios.

#include "ext/upscaler/MipBias.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace ext::upscaler::mipbind
{
// D3D11 clamps a sampler's MipLODBias to this range (the same clamp as upstream's CreateSamplerState hook).
inline constexpr double kSamplerMin = -16.0;
inline constexpr double kSamplerMax = 15.99;

// D3D11_FILTER_REDUCTION_TYPE_MASK << D3D11_FILTER_REDUCTION_TYPE_SHIFT: comparison (1), minimum (2), maximum (3).
inline constexpr unsigned kReductionMask = 0x180;

// ---- the target ----------------------------------------------------------------------------------------------------
// The log2 ratio (without the user's offset) of one evaluate. Sizes that cannot be a game scene (outside 20..16384, a
// display under 640 pixels) give nothing, and the caller keeps the previous target. Render at or above the output (DLAA,
// or more pixels than the output) gives 0. `outputScaling` is the multiplier the upscaler really applies (1 otherwise).
inline std::optional<double> BaseFromEvaluate(unsigned renderW, unsigned displayW, double outputScaling = 1.0)
{
    if (renderW < mipbias::kMinExtent || renderW > mipbias::kMaxExtent || displayW < mipbias::kMinExtent ||
        displayW > mipbias::kMaxExtent || displayW < mipbias::kMinDisplayWidth)
        return std::nullopt;

    return mipbias::BiasFromSizes(renderW, displayW, outputScaling);
}

// What the variants carry: the base plus the user's offset, kept inside +-15 (never -0).
inline double Target(double base, double offset) { return mipbias::FinalBias(base, mipbias::ClampOffset(offset)); }

// The render width of an evaluate: the smaller of the render subrect the game reports and the NGX Width parameter (zero
// values do not count). FFXIV reports the buffer's own size as the subrect, which can be larger than what it rendered;
// Width never is larger than the render (ext/upscaler/RenderGeometry.h), so the smaller one is the render size whatever
// the geometry correction flag says. Zero = unknown.
inline unsigned PickRenderWidth(unsigned subrectW, unsigned paramW)
{
    if (subrectW == 0)
        return paramW;
    if (paramW == 0)
        return subrectW;
    return std::min(subrectW, paramW);
}

// The output width of an evaluate: the NGX OutWidth parameter, else the width of the output texture, else the display
// width the feature was created with. Zero = unknown.
inline unsigned PickOutputWidth(unsigned outParamW, unsigned outputTextureW, unsigned createDisplayW)
{
    if (outParamW != 0)
        return outParamW;
    return outputTextureW != 0 ? outputTextureW : createDisplayW;
}

// ---- samplers ------------------------------------------------------------------------------------------------------
// The samplers that take part: the game's own bias is negative, the sampler is mipmapped (upstream's test: MinLOD !=
// MaxLOD) and the filter is an ordinary one (no comparison, minimum or maximum reduction). A NaN bias is not eligible.
inline bool IsEligible(float originalBias, float minLod, float maxLod, unsigned filter)
{
    if (!std::isfinite(originalBias) || !(originalBias < 0.0f))
        return false;
    if (!(minLod != maxLod))
        return false;
    return (filter & kReductionMask) == 0;
}

enum class Mode
{
    Add,     // variant = original + target (what the manual MipmapBiasOverride does)
    Replace, // variant = target (the game already adds a DLSS bias of its own)
};

inline const char* ModeText(Mode m) { return m == Mode::Replace ? "replace" : "add"; }

// The bias an eligible sampler is bound with, inside D3D11's range.
inline float VariantBias(Mode mode, float original, double target)
{
    const double raw = mode == Mode::Replace ? target : static_cast<double>(original) + target;
    if (!std::isfinite(raw))
        return original;

    return static_cast<float>(std::clamp(raw, kSamplerMin, kSamplerMax) + 0.0);
}

// A variant is only worth making when the bias really changes (two floats the driver would treat as the same).
inline bool NeedsVariant(float original, float variant) { return std::fabs(variant - original) >= 1e-6f; }

// ---- add or replace ------------------------------------------------------------------------------------------------
// Many games apply their own DLSS bias (for example -1 + log2(render / output) in steps of 1/16), others keep a constant
// art bias per material. The first one has to be REPLACED by the target (adding would count the ratio twice), the second
// one ADDED to. The detector looks at the originals the game uses: a quality change moves the ratio by `delta`
// (log2 units); if samplers created after it carry a value that is `delta` away from a value the game used before AND was
// not used before, the game scales its bias with the mode, so REPLACE. If enough new samplers appear without such a pair,
// the biases are art values: ADD. Until a decision exists the mode is ADD.
inline constexpr double kMinRatioChange = 0.01;  // a smaller move of the log2 ratio is size rounding, not a mode change
inline constexpr double kPairTolerance = 0.08;   // one 1/16 step of the game's own quantisation plus rounding
inline constexpr double kSameValue = 0.02;       // two originals this close are the same value
inline constexpr unsigned kDecideAfter = 40;     // eligible creations after a change without a pair: constant art biases
inline constexpr size_t kMaxValues = 64;         // distinct original values remembered per set

class ValueSet
{
  public:
    // False when the value was already there (within kSameValue) or the set is full.
    bool Add(float v)
    {
        if (Contains(v) || _n >= _v.size())
            return false;
        _v[_n++] = v;
        return true;
    }

    bool Contains(float v) const
    {
        for (size_t i = 0; i < _n; ++i)
            if (std::fabs(_v[i] - v) <= kSameValue)
                return true;
        return false;
    }

    size_t size() const { return _n; }
    float at(size_t i) const { return _v[i]; }
    void Clear() { _n = 0; }

  private:
    std::array<float, kMaxValues> _v {};
    size_t _n = 0;
};

struct Decision
{
    bool decided = false; // true only on the call that made the decision (the glue logs it once)
    Mode mode = Mode::Add;
    double delta = 0.0;       // log2 ratio change the decision is based on
    float seenBefore = 0.0f;  // Replace: the value the game used before the change
    float seenAfter = 0.0f;   // Replace: the value that follows it
    unsigned created = 0;     // eligible creations looked at after the change
};

class ModeDetector
{
  public:
    enum class Phase
    {
        Baseline, // collecting the originals used at the first ratio
        Watching, // the ratio changed once: comparing what the game creates now
        Decided,
    };

    Mode mode() const { return _mode; }
    Phase phase() const { return _phase; }
    bool decided() const { return _phase == Phase::Decided; }
    bool haveRatio() const { return _haveRatio; }
    double ratio() const { return _ratio; }

    // The base target (log2 ratio, without the offset) of an evaluate. The first call sets the starting ratio; a later
    // move of kMinRatioChange or more is the first mode change, which starts the comparison. A further move while the
    // comparison runs restarts it from the newer ratio, keeping everything seen so far as "before".
    Decision OnRatio(double base)
    {
        if (!std::isfinite(base))
            return {};
        if (!_haveRatio)
        {
            _haveRatio = true;
            _ratio = base;
            return {};
        }
        if (_phase == Phase::Decided || std::fabs(base - _ratio) < kMinRatioChange)
            return {};

        if (_phase == Phase::Watching)
        {
            for (size_t i = 0; i < _after.size(); ++i)
                _before.Add(_after.at(i));
            _after.Clear();
        }

        _delta = base - _ratio;
        _ratio = base;
        _created = 0;
        _phase = Phase::Watching;
        return {};
    }

    // An eligible sampler was created with this original bias (the game's own value).
    Decision OnEligibleCreated(float original)
    {
        if (!std::isfinite(original) || _phase == Phase::Decided)
            return {};

        if (_phase == Phase::Baseline)
        {
            _before.Add(original);
            return {};
        }

        ++_created;
        const bool known = _before.Contains(original);
        _after.Add(original);

        if (!known)
        {
            for (size_t i = 0; i < _before.size(); ++i)
            {
                if (std::fabs(static_cast<double>(original) - (static_cast<double>(_before.at(i)) + _delta)) <=
                    kPairTolerance)
                    return Decide(Mode::Replace, _before.at(i), original);
            }
        }

        if (_created >= kDecideAfter)
            return Decide(Mode::Add, 0.0f, 0.0f);

        return {};
    }

    void Reset() { *this = ModeDetector(); }

  private:
    Decision Decide(Mode mode, float before, float after)
    {
        _phase = Phase::Decided;
        _mode = mode;
        Decision d;
        d.decided = true;
        d.mode = mode;
        d.delta = _delta;
        d.seenBefore = before;
        d.seenAfter = after;
        d.created = _created;
        return d;
    }

    Phase _phase = Phase::Baseline;
    Mode _mode = Mode::Add;
    bool _haveRatio = false;
    double _ratio = 0.0;
    double _delta = 0.0;
    unsigned _created = 0;
    ValueSet _before;
    ValueSet _after;
};

// ---- settings ------------------------------------------------------------------------------------------------------
// [Mipmap] MipmapBiasAuto, its alias AutomaticFFXIV, and the game default: an explicit MipmapBiasAuto wins, then the alias,
// then the default of the game (on for FFXIV, off for every other game).
inline bool EffectiveAuto(std::optional<bool> primary, std::optional<bool> alias, bool gameDefaultOn)
{
    if (primary)
        return *primary;
    if (alias)
        return *alias;
    return gameDefaultOn;
}

// Bind-time substitution runs only when the setting is on and no manual MipmapBiasOverride is set: the manual value always
// wins. (In the override mode of the older module, which writes the volatile override itself, this is not asked.)
inline bool BindShouldRun(bool autoEnabled, bool manualOverrideSet) { return autoEnabled && !manualOverrideSet; }

// ---- text ----------------------------------------------------------------------------------------------------------
namespace detail
{
inline std::string Num4(double v) { return std::format("{:.4f}", std::fabs(v) < 0.00005 ? 0.0 : v); }
inline std::string Off2(double v) { return std::format("{:+.2f}", std::fabs(v) < 0.005 ? 0.0 : v); }
} // namespace detail

// One INFO line per change of the target.
inline std::string TargetLine(unsigned renderW, unsigned displayW, double base, double offset, double target, Mode mode,
                              bool first)
{
    using namespace detail;
    return std::format("Bind-time mip bias: render width {}, output width {}, ratio bias {}, offset {} -> target {} ({} mode{}); "
                       "applies to the samplers bound from now on and to the ones bound now",
                       renderW, displayW, Num4(base), Off2(offset), Num4(target), ModeText(mode),
                       first ? ", first measurement" : "");
}

// The one line when the detector decides.
inline std::string DecisionLine(const Decision& d)
{
    using namespace detail;
    if (d.mode == Mode::Replace)
        return std::format("Bind-time mip bias: the game already follows the quality mode (its bias {} became {} for a ratio "
                           "change of {}), so the target REPLACES the game's own bias",
                           Num4(d.seenBefore), Num4(d.seenAfter), Num4(d.delta));

    return std::format("Bind-time mip bias: the game's biases did not follow the quality change of {} (looked at {} new "
                       "samplers), they are art values, so the target is ADDED to the game's own bias",
                       Num4(d.delta), d.created);
}

// Debug line of MipmapBiasObserve: a distinct original bias seen at a ratio.
inline std::string ObserveLine(double base, float original, size_t distinctAtRatio)
{
    return std::format("Bind-time mip bias observe: ratio bias {} original sampler bias {} ({} distinct at this ratio)",
                       detail::Num4(base), detail::Num4(original), distinctAtRatio);
}

// MipmapBiasObserve: the distinct originals per ratio. Returns a line the first time a value is seen at a ratio.
class Observer
{
  public:
    std::optional<std::string> Note(double base, float original)
    {
        if (!std::isfinite(base) || !std::isfinite(original))
            return std::nullopt;

        Group* g = nullptr;
        for (size_t i = 0; i < _used; ++i)
            if (std::fabs(_groups[i].base - base) < kMinRatioChange)
                g = &_groups[i];
        if (g == nullptr)
        {
            if (_used >= _groups.size())
                return std::nullopt;
            g = &_groups[_used++];
            g->base = base;
        }

        if (!g->values.Add(original))
            return std::nullopt;
        return ObserveLine(base, original, g->values.size());
    }

    void Reset() { *this = Observer(); }

  private:
    struct Group
    {
        double base = 0.0;
        ValueSet values;
    };
    std::array<Group, 8> _groups {};
    size_t _used = 0;
};

// The note under the status line of the card while the bind-time hooks are attached.
inline std::string BindCardNote()
{
    return "Bind-time mode: the sharpening is applied when the game uses a sampler, so a quality change reaches the textures "
           "at once, also on samplers the game made earlier. Add or replace is decided from the game's own sampler biases at "
           "the first quality change (see the log); a Mipmap Bias value set by hand always wins. Only samplers the game made "
           "after the hooks were attached take part, so turn this on at the start for a full effect.";
}

// What the menu card shows.
struct BindStatus
{
    bool installed = false;  // the hooks are in place on the game's context
    bool running = false;    // substitution is on (setting on, no manual value, a target exists)
    bool manualWins = false; // a manual MipmapBiasOverride is set
    bool hasTarget = false;
    double target = 0.0;
    Mode mode = Mode::Add;
    bool decided = false;
    unsigned long long substitutedBinds = 0;
    unsigned long long variants = 0;
    unsigned long long tableFull = 0;
    unsigned long long failures = 0;
    size_t samplers = 0; // eligible samplers known
};

inline std::string BindStatusLine(const BindStatus& s)
{
    using namespace detail;
    if (!s.installed)
        return "Bind-time mode is not active: the value is applied when samplers are created.";
    if (s.manualWins)
        return "A manual Mipmap Bias value is set: it wins, nothing is applied automatically.";
    if (!s.hasTarget)
        return "Waiting for the first upscaler evaluation.";
    return std::format("Target {} ({} mode{}), {} samplers known, {} binds substituted, {} variants made{}{}.", Num4(s.target),
                       ModeText(s.mode), s.decided ? "" : ", not decided yet", s.samplers, s.substitutedBinds, s.variants,
                       s.tableFull != 0 ? std::format(", table full {} times", s.tableFull) : std::string(),
                       s.failures != 0 ? std::format(", {} variant failures", s.failures) : std::string());
}
} // namespace ext::upscaler::mipbind
