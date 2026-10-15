#pragma once

#include "ext/game/GameInterfaces.h"

namespace ext::game
{
// Selected once at init. Every pointer is null for every game except a matched FFXIV build, so a seam that needs the
// game costs one pointer test and never a per-frame exe name compare.
struct Game
{
    IRenderGeometry* geometry = nullptr;
    INativeQuality* quality = nullptr;
    ISceneLighting* lighting = nullptr;
    ISceneHdr* hdr = nullptr;
    IFgMarkers* fgMarkers = nullptr;
    INgxLifecycle* ngx = nullptr;
    INrQuirks* nrQuirks = nullptr;
};

const Game& Current();
} // namespace ext::game
