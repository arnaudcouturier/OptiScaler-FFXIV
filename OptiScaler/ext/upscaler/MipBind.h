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
#include <optional>

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

// The render width of an evaluate: the game's geometry answer when there is one (the width after the fork's render
// subrect correction), else the subrect the game reported, else the NGX Width parameter. Zero = unknown.
inline unsigned PickRenderWidth(unsigned resolvedW, unsigned subrectW, unsigned paramW)
{
    if (resolvedW != 0)
        return resolvedW;
    return subrectW != 0 ? subrectW : paramW;
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
} // namespace ext::upscaler::mipbind
