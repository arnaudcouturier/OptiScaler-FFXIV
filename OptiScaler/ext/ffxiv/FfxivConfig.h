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
    ffxiv::chara::CharaConfig chara; // ext/ffxiv/chara: [FFXIV] Preview* and [HDR] PreviewNRTrace

    void Bind(config::Binder& b);
};
} // namespace ext
