#pragma once
// Test replacement of ext/hudless/HudlessConfig.h: the engine asks two things of the config, the recomposition mode (a
// preset pause only applies under it) and nothing else. The key and its snapshot live in PlateMode.h, which the engine
// owns here (the config binds them in the wiring package).
#include "ext/hudless/HudlessMode.h"

namespace ext::hudless
{
inline Mode& TestMode()
{
    static Mode mode = Mode::Off;
    return mode;
}

inline Mode CurrentMode()
{
    return TestMode();
}
} // namespace ext::hudless
