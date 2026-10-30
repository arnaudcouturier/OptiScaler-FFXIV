#pragma once
// Test replacement of ext/hudless/HudlessEngine.h: only the recomposition query the engine reads for the preset pause.
#include <cstdint>

namespace ext::hudless
{
struct Recomposition
{
    bool enabled = false;
    uint32_t format = 0;
    const char* formatSource = "test";
};

inline Recomposition& TestRecomposition()
{
    static Recomposition recomp;
    return recomp;
}

inline Recomposition QueryRecomposition()
{
    return TestRecomposition();
}
} // namespace ext::hudless
