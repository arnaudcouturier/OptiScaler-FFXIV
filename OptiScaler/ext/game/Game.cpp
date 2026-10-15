#include "pch.h"

#include "ext/game/Game.h"

namespace ext::game
{
const Game& Current()
{
    // No game module yet: everything is null. ext/ffxiv selects its module here once it is ported.
    static const Game none {};
    return none;
}
} // namespace ext::game
