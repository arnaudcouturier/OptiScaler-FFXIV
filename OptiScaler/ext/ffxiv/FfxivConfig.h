#pragma once

#include <Config.h>

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

    void Bind(config::Binder& b);
};
} // namespace ext
