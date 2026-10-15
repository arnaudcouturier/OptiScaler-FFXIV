#pragma once

#include <Config.h>

namespace ext
{
struct SceneHdrConfig
{
    CustomOptional<bool> ffxivHdr { false };
    CustomOptional<int> mode { 0 };
};

struct SceneExtConfig
{
    SceneHdrConfig hdr;
};

inline SceneExtConfig& Cfg()
{
    static SceneExtConfig value;
    return value;
}
} // namespace ext
