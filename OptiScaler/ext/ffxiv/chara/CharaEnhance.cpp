#include "pch.h"

#include "ext/ffxiv/chara/CharaEnhance.h"
#include "ext/ffxiv/chara/CharaStats.h"

// Spatial anti-aliasing of the equipment preview image is not part of this revision yet: the draw is always left native.
namespace ext::ffxiv::chara::enhance
{
bool Around(const DrawView&, DrawCall)
{
    ++g_stats.equipShaderSeen;
    return false;
}

void Detach() {}
} // namespace ext::ffxiv::chara::enhance
