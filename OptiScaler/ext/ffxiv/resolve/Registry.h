#pragma once
// The product registry: every seam and binding of the two anchor groups, gathered when it is called.
//
// FROZEN with the other resolver headers: each anchor group owns ONE Anchors*.h file and this header only gathers them. The golden test, the CLI
// and the game all resolve exactly BuildRegistry(), so a seam added to a group shows up everywhere without another edit.
//
// A function, never a namespace-scope object: nothing is built at load time.

#include "AnchorsJitter.h"
#include "AnchorsQuality.h"
#include "ResolveTypes.h"

namespace ext::ffxiv::resolve
{
inline Registry BuildRegistry()
{
    Registry r;
    AddQualityAnchors(r);
    AddJitterAnchors(r);
    return r;
}
} // namespace ext::ffxiv::resolve
