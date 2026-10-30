#pragma once
// Test stand-in for the game module: null pointers mean an unmatched build,
// non-null ones a matched one. The quality fake models liveness and the
// preset flag the collector reports.

namespace ext::game
{
struct FakeQuality
{
    bool live = true;
    bool dlaa = false;
    bool Live() { return live; }
    bool DlaaRequired() { return dlaa; }
};

struct Game
{
    const void* geometry = nullptr;
    FakeQuality* quality = nullptr;
    const void* lighting = nullptr;
    const void* hdr = nullptr;
    const void* fgMarkers = nullptr;
    const void* ngx = nullptr;
    const void* nrQuirks = nullptr;
    const void* hudless = nullptr;
};

inline Game g_game;

inline const Game& Current() { return g_game; }
} // namespace ext::game
