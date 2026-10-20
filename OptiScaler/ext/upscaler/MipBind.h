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
#include <cstring>
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
//   ADD:     the game's bias plus the target now (`baseNow` plus the offset): what the manual setting does.
//   REPLACE: the game already puts the quality mode into its own bias (for FFXIV `-1 + log2(ratio)`), so only the CHANGE of the
//            ratio since the sampler was made is applied (`baseNow - baseCreate`), plus the offset. A sampler made at the
//            current mode is bound as the game made it (plus the offset), a stale one gets exactly the game's new value, and
//            nothing of the game's own constant part (the -1) is dropped.
// `baseCreate` is the base target (log2 ratio, no offset) in force when the sampler was made, 0 (native resolution) when
// that was not known.
inline float VariantBias(Mode mode, float original, double baseNow, double baseCreate, double offset)
{
    if (!std::isfinite(baseNow) || !std::isfinite(baseCreate))
        return original;

    const double change = mode == Mode::Replace ? baseNow - baseCreate : baseNow;
    const double raw = static_cast<double>(original) + Target(change, offset);
    if (!std::isfinite(raw))
        return original;

    return static_cast<float>(std::clamp(raw, kSamplerMin, kSamplerMax) + 0.0);
}

// A variant is only worth making when the bias really changes (two floats the driver would treat as the same).
inline bool NeedsVariant(float original, float variant) { return std::fabs(variant - original) >= 1e-6f; }

// ---- a steady target -----------------------------------------------------------------------------------------------
// Size noise, a second upscaler feature at another size, or a window being dragged must not rebuild the variants at every
// evaluate. The gate (1) follows one anchor, the output width of the first evaluate: an evaluate whose output differs from it
// by more than 10 % belongs to another feature and is ignored until it has been the only one for two seconds or 120 evaluates
// in a row (then the anchor moves: a resolution or window change), or until a BIGGER output has shown up three times within two
// seconds (the first evaluate belonged to a small feature, a preview window: the main scene is the larger one); (2) takes a new base only when it moved by at least one
// step of 1/64 in log2 (about 1 % of the width); (3) takes at most four new bases per second.
inline constexpr double kBaseStep = 1.0 / 64.0;
inline constexpr double kAnchorTolerance = 0.10;
inline constexpr unsigned kReanchorAfterMs = 2000;
inline constexpr unsigned kReanchorEvaluates = 120;
inline constexpr unsigned kLargerEvaluates = 3;
inline constexpr unsigned kLargerWindowMs = 2000;
inline constexpr unsigned kMinApplyIntervalMs = 250;

struct GateResult
{
    enum class Kind
    {
        Ignored,   // another feature's evaluate
        Unchanged, // inside the dead band
        Deferred,  // moved, but the last change was less than 250 ms ago: the next evaluate asks again
        Applied,   // take `base`
    };
    Kind kind = Kind::Ignored;
    double base = 0.0;
    bool first = false;       // the first base ever taken
    bool anchorMoved = false; // the output size changed for good
};

class TargetGate
{
  public:
    GateResult OnEvaluate(unsigned outputW, double base, std::uint64_t nowMs)
    {
        GateResult r;
        r.base = base;
        if (outputW == 0 || !std::isfinite(base))
            return r;

        if (!_haveAnchor)
        {
            _haveAnchor = true;
            _anchor = outputW;
            _lastMatchMs = nowMs;
            return Apply(r, nowMs);
        }

        const double relative = std::fabs(static_cast<double>(outputW) - static_cast<double>(_anchor)) / static_cast<double>(_anchor);
        if (relative > kAnchorTolerance)
        {
            ++_deviant;
            // A bigger output than the anchor's is the main scene when the anchor was captured by a small feature (a preview
            // window that evaluated first): after three such evaluates within two seconds the anchor moves up to it.
            bool bigger = false;
            if (outputW > _anchor)
            {
                if (nowMs - _lastLargerMs > kLargerWindowMs)
                    _larger = 0;
                _lastLargerMs = nowMs;
                bigger = ++_larger >= kLargerEvaluates;
            }
            if (!bigger && _deviant < kReanchorEvaluates && nowMs - _lastMatchMs < kReanchorAfterMs)
                return r; // Ignored
            _larger = 0;

            _anchor = outputW; // the output really changed
            _deviant = 0;
            _lastMatchMs = nowMs;
            r.anchorMoved = true;
            r.kind = GateResult::Kind::Unchanged;
            if (!_haveApplied || std::fabs(base - _applied) >= kBaseStep)
                return Apply(r, nowMs, true);
            return r;
        }

        _deviant = 0;
        _lastMatchMs = nowMs;
        _anchor = outputW; // a slow resize is followed
        r.kind = GateResult::Kind::Unchanged;
        if (_haveApplied && std::fabs(base - _applied) < kBaseStep)
            return r;
        return Apply(r, nowMs);
    }

    bool haveApplied() const { return _haveApplied; }
    double applied() const { return _applied; }
    unsigned anchor() const { return _anchor; }
    void Reset() { *this = TargetGate(); }

  private:
    GateResult Apply(GateResult r, std::uint64_t nowMs, bool force = false)
    {
        if (_haveApplied && !force && nowMs - _lastApplyMs < kMinApplyIntervalMs)
        {
            r.kind = GateResult::Kind::Deferred;
            return r;
        }
        r.first = !_haveApplied;
        r.kind = GateResult::Kind::Applied;
        _haveApplied = true;
        _applied = r.base;
        _lastApplyMs = nowMs;
        return r;
    }

    bool _haveAnchor = false;
    bool _haveApplied = false;
    unsigned _anchor = 0;
    unsigned _deviant = 0;
    unsigned _larger = 0;
    std::uint64_t _lastLargerMs = 0;
    double _applied = 0.0;
    std::uint64_t _lastMatchMs = 0;
    std::uint64_t _lastApplyMs = 0;
};

// ---- add or replace ------------------------------------------------------------------------------------------------
// Many games apply their own DLSS bias (FFXIV appears to add -1 + log2(render / output) in steps of 1/16 to one family of
// samplers), others keep a constant art bias per material. For the first the quality mode is already in the game's bias, so only
// the change since the sampler was made is applied (REPLACE, MipBind.h VariantBias); for the second the target is ADDED. The
// detector tells them apart from what the game creates, each observation being (family, bias, base target in force):
//   - a FAMILY is everything of a sampler description except the bias (filter, address modes, anisotropy, comparison, border,
//     LOD range): only samplers of one family can be a tracking pair;
//   - a PAIR is two distinct samplers of one family made at base targets at least 0.25 apart, whose biases differ by the same
//     amount as the targets (within 0.08: one step of the game's 1/16 quantisation plus rounding), i.e. whose INTERCEPT
//     (bias - base: -1 for FFXIV's tracking family) agrees. One pair makes the mode REPLACE provisionally. It is confirmed and
//     final by a second family that shows a pair of its own, or by a chain: a sampler that pairs with older ones made at two
//     different targets (DLAA, Quality, Performance). Pairs that agree on the intercept never contradict each other. A sampler
//     that pairs with older ones whose intercepts DISAGREE with each other is ambiguous: ADD, and the detection is re-opened
//     by the next observation made at a base target 0.25 or more away from that one;
//   - re-creates of a sampler that is already known (the runtime returns the existing object) are not observations;
//   - ADD is never final: 40 distinct samplers made at other ratios without a pair log "no pair seen" once, and a pair that
//     arrives later still makes the mode REPLACE. Only a confirmed REPLACE is final.
inline constexpr double kMinRatioChange = 0.01;  // grouping of the observe log: ratios closer than this are one ratio
inline constexpr double kMinPairDelta = 0.25;    // a pair needs a change of the target at least this large (log2 units)
inline constexpr double kPairTolerance = 0.08;   // one 1/16 step of the game's own quantisation plus rounding
inline constexpr double kSameValue = 0.02;       // two originals this close are the same value
inline constexpr unsigned kNoPairAfter = 40;     // distinct samplers made at other ratios without a pair: "no pair seen"
inline constexpr size_t kMaxValues = 64;         // distinct original values remembered per observe-log ratio
inline constexpr size_t kMaxObservations = 128;  // distinct samplers the detector remembers

// FNV-1a over the description fields that are not the bias.
inline std::uint32_t FamilyHash(unsigned filter, unsigned addressU, unsigned addressV, unsigned addressW, unsigned anisotropy,
                                unsigned comparison, const float border[4], float minLod, float maxLod)
{
    std::uint32_t h = 2166136261u;
    const auto mix = [&h](std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
        {
            h ^= (v >> (8 * i)) & 0xFFu;
            h *= 16777619u;
        }
    };
    const auto bits = [](float f)
    {
        std::uint32_t v = 0;
        std::memcpy(&v, &f, sizeof(v));
        return v;
    };
    mix(filter);
    mix(addressU);
    mix(addressV);
    mix(addressW);
    mix(anisotropy);
    mix(comparison);
    for (int i = 0; i < 4; ++i)
        mix(bits(border[i]));
    mix(bits(minLod));
    mix(bits(maxLod));
    return h;
}

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
    enum class Kind
    {
        None,
        ReplaceProvisional, // a pair was seen: REPLACE until confirmed
        ReplaceConfirmed,   // final
        AddAmbiguous,       // two different pairs: ADD (re-opened later)
        AddNoPair,          // informational: many samplers at other ratios, no pair; ADD stays, nothing is locked
    };
    Kind kind = Kind::None;
    Mode mode = Mode::Add;    // the mode after this call
    double delta = 0.0;       // the change of the base target the pair spans
    float seenBefore = 0.0f;  // the older bias of the pair
    float seenAfter = 0.0f;   // the newer bias of the pair
    unsigned distinct = 0;    // distinct samplers looked at so far
    unsigned candidates = 0;  // pairs the observation formed

    bool any() const { return kind != Kind::None; }
};

class ModeDetector
{
  public:
    enum class State
    {
        Default,     // ADD, no pair yet
        Provisional, // REPLACE after one pair
        Final,       // REPLACE confirmed
        Ambiguous,   // ADD after two pairs; re-opened by an observation at another ratio
    };

    Mode mode() const { return _mode; }
    State state() const { return _state; }
    bool final() const { return _state == State::Final; }
    size_t observations() const { return _n; }

    // One distinct sampler: its family, the bias the game gave it and the base target in force when it was made.
    Decision OnObserved(std::uint32_t family, float bias, double base)
    {
        if (!std::isfinite(bias) || !std::isfinite(base))
            return {};

        for (size_t i = 0; i < _n; ++i)
            if (_obs[i].family == family && std::fabs(_obs[i].bias - bias) <= kSameValue)
                return {}; // already known

        if (_n >= _obs.size())
            return {}; // full: later samplers are not looked at

        // A tracking sampler has a constant intercept (bias - base: -1 for FFXIV), so a pair is two samplers of one family at
        // targets 0.25 or more apart whose intercepts agree. Candidates are the older samplers this one pairs with.
        const double intercept = static_cast<double>(bias) - base;
        std::array<double, 8> candidateIntercepts {};
        std::array<double, 8> candidateBases {};
        unsigned candidates = 0;
        bool otherRatio = false;
        float pairedWith = 0.0f;
        double pairedDelta = 0.0;
        for (size_t i = 0; i < _n; ++i)
        {
            const double dBase = base - _obs[i].base;
            if (std::fabs(dBase) < kMinPairDelta)
                continue;
            otherRatio = true;
            if (_obs[i].family == family && std::fabs(intercept - (static_cast<double>(_obs[i].bias) - _obs[i].base)) <= kPairTolerance)
            {
                if (candidates == 0)
                {
                    pairedWith = _obs[i].bias;
                    pairedDelta = dBase;
                }
                if (candidates < candidateIntercepts.size())
                {
                    candidateIntercepts[candidates] = static_cast<double>(_obs[i].bias) - _obs[i].base;
                    candidateBases[candidates] = _obs[i].base;
                }
                ++candidates;
            }
        }
        _obs[_n++] = { family, bias, base };
        if (otherRatio)
            ++_otherRatio;

        Decision d;
        d.distinct = static_cast<unsigned>(_n);
        d.candidates = candidates;
        d.mode = _mode;
        d.delta = pairedDelta;
        d.seenBefore = pairedWith;
        d.seenAfter = bias;

        if (_state == State::Final)
            return {};

        if (_state == State::Ambiguous)
        {
            if (std::fabs(base - _ambiguousBase) < kMinPairDelta)
                return {};
            _state = State::Default; // another ratio: look again
        }

        if (candidates > 0)
        {
            const size_t known = std::min<size_t>(candidates, candidateIntercepts.size());
            double lo = candidateIntercepts[0], hi = candidateIntercepts[0];
            for (size_t i = 1; i < known; ++i)
            {
                lo = std::min(lo, candidateIntercepts[i]);
                hi = std::max(hi, candidateIntercepts[i]);
            }
            if (hi - lo > kPairTolerance)
                return Ambiguous(d, base); // the sampler pairs with older ones that disagree with each other: cannot tell

            // A consistent pair or chain. It is CONFIRMED by a chain of three targets (this sampler pairs with older ones made
            // at two targets 0.25 apart, e.g. DLAA, Quality and Performance) or by a second family with a pair of its own.
            unsigned clusters = 0;
            std::array<double, 8> reps {};
            for (size_t i = 0; i < known; ++i)
            {
                bool found = false;
                for (unsigned k = 0; k < clusters; ++k)
                    found = found || std::fabs(reps[k] - candidateBases[i]) < kMinPairDelta;
                if (!found)
                    reps[clusters++] = candidateBases[i];
            }
            const bool newFamily = AddPairFamily(family);
            const bool chain = clusters >= 2;
            if (chain || (_state == State::Provisional && newFamily))
            {
                _state = State::Final;
                _mode = Mode::Replace;
                d.kind = Decision::Kind::ReplaceConfirmed;
                d.mode = _mode;
                return d;
            }
            if (_state == State::Default)
            {
                _state = State::Provisional;
                _mode = Mode::Replace;
                d.kind = Decision::Kind::ReplaceProvisional;
                d.mode = _mode;
                return d;
            }
            return {}; // another pair of the same family at the same targets: nothing new
        }

        if (_state == State::Default && !_noPairLogged && _otherRatio >= kNoPairAfter)
        {
            _noPairLogged = true;
            d.kind = Decision::Kind::AddNoPair;
            return d;
        }
        return {};
    }

    void Reset() { *this = ModeDetector(); }

  private:
    struct Observation
    {
        std::uint32_t family = 0;
        float bias = 0.0f;
        double base = 0.0;
    };

    Decision Ambiguous(Decision d, double base)
    {
        _state = State::Ambiguous;
        _mode = Mode::Add;
        _ambiguousBase = base;
        d.kind = Decision::Kind::AddAmbiguous;
        d.mode = _mode;
        return d;
    }

    // True when this family had no consistent pair before.
    bool AddPairFamily(std::uint32_t family)
    {
        for (size_t i = 0; i < _pairFamilyCount; ++i)
            if (_pairFamilies[i] == family)
                return false;
        if (_pairFamilyCount < _pairFamilies.size())
            _pairFamilies[_pairFamilyCount++] = family;
        return true;
    }

    State _state = State::Default;
    Mode _mode = Mode::Add;
    std::array<Observation, kMaxObservations> _obs {};
    size_t _n = 0;
    unsigned _otherRatio = 0;
    std::array<std::uint32_t, 8> _pairFamilies {};
    size_t _pairFamilyCount = 0;
    bool _noPairLogged = false;
    double _ambiguousBase = 0.0;
};

// Warns once when the hooks look to sit on the wrong context: the substitution is on and samplers are known, but after many
// evaluates not one bind was substituted.
class LatchWatch
{
  public:
    static constexpr unsigned kEvaluates = 600;

    bool OnEvaluate(bool active, size_t samplers, std::uint64_t substitutedBinds)
    {
        if (_warned || _satisfied)
            return false;
        if (substitutedBinds > 0)
        {
            _satisfied = true;
            return false;
        }
        if (!active || samplers == 0)
            return false;
        if (++_count < kEvaluates)
            return false;
        _warned = true;
        return true;
    }

    void Reset() { *this = LatchWatch(); }

  private:
    unsigned _count = 0;
    bool _warned = false;
    bool _satisfied = false;
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

// The line when the detector decides (or declines to).
inline std::string DecisionLine(const Decision& d)
{
    using namespace detail;
    switch (d.kind)
    {
    case Decision::Kind::ReplaceProvisional:
        return std::format("Bind-time mip bias: the game already follows the quality mode (one sampler family went from bias {} to "
                           "{} while the ratio target moved by {}), so only the CHANGE of the ratio since a sampler was made is "
                           "applied (REPLACE mode, provisional until a second sampler family or a further quality change confirms it)",
                           Num4(d.seenBefore), Num4(d.seenAfter), Num4(d.delta));
    case Decision::Kind::ReplaceConfirmed:
        return std::format("Bind-time mip bias: REPLACE mode confirmed (after {} distinct samplers)", d.distinct);
    case Decision::Kind::AddAmbiguous:
        return std::format("Bind-time mip bias: {} different sampler pairs fit a change of the ratio, so the game's bias cannot be "
                           "told from art values; staying in ADD mode (the target is added to the game's own bias) and looking "
                           "again at the next change of the ratio",
                           d.candidates >= 2 ? d.candidates : 2u);
    case Decision::Kind::AddNoPair:
        return std::format("Bind-time mip bias: {} samplers made at other ratios and none follows the quality mode, they look like "
                           "art values; ADD mode (the target is added to the game's own bias), still watching",
                           d.distinct);
    default:
        return {};
    }
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
           "at once, also on samplers the game made earlier. Add or replace is decided from the game's own sampler biases when the "
           "quality changes (see the log); a Mipmap Bias value set by hand always wins, and takes over again as soon as it is "
           "cleared.";
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
    bool decided = false; // REPLACE confirmed
    unsigned long long substitutedBinds = 0;
    unsigned long long lazy = 0;     // samplers first seen at a bind
    unsigned long long notReady = 0; // binds of a sampler whose variant was not ready
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
                       ModeText(s.mode), s.decided ? ", confirmed" : "", s.samplers, s.substitutedBinds, s.variants,
                       s.tableFull != 0 ? std::format(", table full {} times", s.tableFull) : std::string(),
                       s.failures != 0 ? std::format(", {} variant failures", s.failures) : std::string());
}
} // namespace ext::upscaler::mipbind
