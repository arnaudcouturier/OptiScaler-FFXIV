#pragma once
// ext-internal C10 entry points (ext/hdr/scene/SceneHooks.cpp).

#include "ext/game/GameInterfaces.h"

namespace ext::hdr
{
// The ISceneHdr implementation of the FFXIV scene capture. A game module sets ext::game::Game::hdr to it (S7).
game::ISceneHdr& SceneProvider();
// Fills ext::nr::SetSceneHdrHooks; called once by HdrConfig::PostLoad. Everything behind the hooks is inert until a game
// module provides ISceneHdr and OptiHDR runs in scene mode.
void InstallSceneHooks();
} // namespace ext::hdr
