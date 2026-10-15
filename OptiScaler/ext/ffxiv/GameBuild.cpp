#include "pch.h"

#include "ext/ffxiv/GameBuild.h"

namespace ext::ffxiv
{
const GameBuild* MatchBuild(std::uint32_t timeDateStamp, std::uint32_t sizeOfImage)
{
    if (timeDateStamp == kCurrentBuild.timeDateStamp && sizeOfImage == kCurrentBuild.sizeOfImage)
        return &kCurrentBuild;
    return nullptr;
}
} // namespace ext::ffxiv
