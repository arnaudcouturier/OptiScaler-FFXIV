#pragma once

namespace ext::game
{
struct TestGame
{
    void* hdr = nullptr;
};

inline TestGame& Current()
{
    static TestGame value;
    return value;
}
} // namespace ext::game
