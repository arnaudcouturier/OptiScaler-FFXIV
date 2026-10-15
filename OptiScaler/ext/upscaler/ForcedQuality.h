#pragma once
// Pure decision logic behind the `upscaler` seams (docs/SEAMS-catalog.md "upscaler": EffectivePerfQuality,
// OnOptimalSettings, ApplyForcedQuality). No upstream types here (no NVNGX, no Config, no logging), so this header
// is cheaply unit-testable on its own (tests/ext/upscaler) without linking anything else.

#include <cstdint>

namespace ext::upscaler
{
// The last answer GetOptimalSettings gave (docs/SEAMS-catalog.md "upscaler" OnOptimalSettings row; was the
// oracle's NVNGX_Parameter.h ForcedQualityStatus -- same field names and types). `queries` is 0 until the first
// answer; `quality` is an NVSDK_NGX_PerfQuality_Value, -1 before the first answer.
struct ForcedQualityStatus
{
    unsigned long long queries = 0;
    int quality = -1;
    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    unsigned int displayWidth = 0;
    unsigned int displayHeight = 0;
};

// Rounding slack for ApplyForcedQuality's "does this create descend from the last answer" test
// (upscalers/IFeature.cpp SetInitParameters in the oracle). 32px absorbs a game's own size rounding without
// matching a whole preset away -- see the oracle's comment there for the full rationale.
inline constexpr unsigned int kForcedQualitySlack = 32;

inline bool WithinSlack(unsigned int a, unsigned int b, unsigned int slack = kForcedQualitySlack)
{
    return (a > b ? a - b : b - a) <= slack;
}

// True when a create at (width, height) render / (outWidth, outHeight) display either has nothing to check against
// yet (no query answered) or matches the last answer within slack on all four dimensions. False = stand down and
// leave the game's own PerfQualityValue alone: relabelling a mismatched create is what the runtime rejects with
// BAD00005 (FFXIV recreates the NGX feature at several points with stale dimensions).
inline bool MatchesLastAnswer(const ForcedQualityStatus& answer, unsigned int width, unsigned int height,
                               unsigned int outWidth, unsigned int outHeight)
{
    if (answer.queries == 0)
        return true; // nothing to check against: the override is all there is

    return WithinSlack(answer.renderWidth, width) && WithinSlack(answer.renderHeight, height) &&
           WithinSlack(answer.displayWidth, outWidth) && WithinSlack(answer.displayHeight, outHeight);
}

// EffectivePerfQuality's decision, isolated from NVSDK_NGX_PerfQuality_Value and Config so it stays a plain int
// function. `asked`/`forced`/`maxValid` are perf-quality values; `forced` < 0 means unset. `dlaaRequired` wins
// unconditionally (native DLAA, not a preset substitution) and returns `maxValid` (DLAA is the enum's last, highest
// value). Sets `invalid` when `forced` is set but out of range (an ini typo); the result is then `asked` and the
// caller should warn once.
inline int EffectivePerfQualityValue(bool dlaaRequired, int forced, int asked, int maxValid, bool& invalid)
{
    invalid = false;

    if (dlaaRequired)
        return maxValid;

    if (forced < 0)
        return asked;

    if (forced > maxValid)
    {
        invalid = true;
        return asked;
    }

    return forced;
}

// ApplyForcedQuality's own gate before it even looks at the last answer: stand down when native DLAA already owns
// the answer elsewhere (do not fight it), the override is unset or out of range, or the game already asked for
// exactly what would be forced (nothing to relabel).
inline bool ShouldApplyForcedQuality(bool dlaaRequired, int forced, int askedPq, int maxValid)
{
    return !dlaaRequired && forced >= 0 && forced <= maxValid && forced != askedPq;
}
} // namespace ext::upscaler
