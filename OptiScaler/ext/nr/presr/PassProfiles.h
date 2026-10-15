#pragma once
#include "ext/nr/NrConfig.h"
#include <algorithm>
#include <cmath>

namespace DlssNr::Profiles
{
struct NrPassTuning
{
    float intensity = 1.0f;
    float structure = 1.0f;
    float tone = 0.0f;
    float skin = -1.0f;
    bool autoMask = true;
    bool operator==(const NrPassTuning&) const = default;
};

inline unsigned int PassPreset(const ext::NrConfig& nr, unsigned int pass)
{
    const unsigned int base = std::min(nr.preset.value_or_default(), 3u);

    if (pass == 1 && nr.pass2Preset.has_value())
        return std::min(nr.pass2Preset.value(), 3u);

    if (pass == 2 && nr.pass3Preset.has_value())
        return std::min(nr.pass3Preset.value(), 3u);

    return base;
}

inline unsigned int PassStyle(const ext::NrConfig& nr, unsigned int pass)
{
    const unsigned int base = std::min(nr.style.value_or_default(), 2u);

    if (pass == 1 && nr.pass2Style.has_value())
        return std::min(nr.pass2Style.value(), 2u);

    if (pass == 2 && nr.pass3Style.has_value())
        return std::min(nr.pass3Style.value(), 2u);

    if (pass >= 3 && pass < 30 && nr.extraPasses[pass - 3].style.has_value())
        return std::min(nr.extraPasses[pass - 3].style.value(), 2u);

    return base;
}

inline NrPassTuning PassTuning(const ext::NrConfig& nr, unsigned int pass)
{
    NrPassTuning result { nr.intensity.value_or_default(),
                          nr.localStructure.value_or_default(),
                          pass == 0 ? nr.localTone.value_or_default() : 0.0f,
                          nr.skinStructure.value_or_default(),
                          nr.autoMask.value_or_default() };
    if (pass == 1)
    {
        if (nr.pass2Intensity.has_value())
            result.intensity = nr.pass2Intensity.value();
        if (nr.pass2LocalStructure.has_value())
            result.structure = nr.pass2LocalStructure.value();
        if (nr.pass2LocalTone.has_value())
            result.tone = nr.pass2LocalTone.value();
        if (nr.pass2SkinStructure.has_value())
            result.skin = nr.pass2SkinStructure.value();
        if (nr.pass2AutoMask.has_value())
            result.autoMask = nr.pass2AutoMask.value();
    }
    if (pass == 2)
    {
        if (nr.pass3Intensity.has_value())
            result.intensity = nr.pass3Intensity.value();
        if (nr.pass3LocalStructure.has_value())
            result.structure = nr.pass3LocalStructure.value();
        if (nr.pass3LocalTone.has_value())
            result.tone = nr.pass3LocalTone.value();
        if (nr.pass3SkinStructure.has_value())
            result.skin = nr.pass3SkinStructure.value();
        if (nr.pass3AutoMask.has_value())
            result.autoMask = nr.pass3AutoMask.value();
    }
    if (pass >= 3 && pass < 30)
    {
        const auto& extra = nr.extraPasses[pass - 3];
        if (extra.intensity.has_value()) result.intensity = extra.intensity.value();
        if (extra.structure.has_value()) result.structure = extra.structure.value();
        if (extra.tone.has_value()) result.tone = extra.tone.value();
        if (extra.skin.has_value()) result.skin = extra.skin.value();
        if (extra.autoMask.has_value()) result.autoMask = extra.autoMask.value();
    }
    const auto bounded = [](float value, float fallback, float minimum) {
        return std::isfinite(value) ? std::clamp(value, minimum, 2.0f) : fallback;
    };
    result.intensity = bounded(result.intensity, 1.0f, 0.0f);
    result.structure = bounded(result.structure, 1.0f, 0.0f);
    result.tone = bounded(result.tone, pass == 0 ? 1.0f : 0.0f, 0.0f);
    result.skin = bounded(result.skin, -1.0f, -1.0f);
    return result;
}
}
