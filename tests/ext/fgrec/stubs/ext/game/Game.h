#pragma once
// Stand-in for ext/game/Game.h, shadowing it for the seams test only. The
// recorder only reads the lighting pointer (matched build or not) through
// Current(); the test flips it through TestGame().
namespace ext::game
{
struct Game
{
    const void* lighting = nullptr;
};
inline Game& TestGame()
{
    static Game g;
    return g;
}
inline const Game& Current() { return TestGame(); }
} // namespace ext::game
