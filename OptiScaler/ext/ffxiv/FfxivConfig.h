#pragma once

#include <Config.h>

#include "ext/ffxiv/chara/CharaConfig.h"

namespace ext
{
namespace config
{
class Binder;
}

struct FfxivConfig
{
    CustomOptional<bool> companionHudReplacement { false };
    CustomOptional<bool> companionHudInterpolation { false };
    CustomOptional<bool> jitterProbe { false };      // [FFXIV] JitterProbe: read-only log of the game's applied versus reported jitter (ext/ffxiv/jitter)
    CustomOptional<bool> jitterCorrection { false }; // [FFXIV] JitterCorrection: experimental, hands the upscaler the jitter the game applies
    ffxiv::chara::CharaConfig chara; // ext/ffxiv/chara: [FFXIV] Preview* and [HDR] PreviewNRTrace

    void Bind(config::Binder& b);
};
} // namespace ext
