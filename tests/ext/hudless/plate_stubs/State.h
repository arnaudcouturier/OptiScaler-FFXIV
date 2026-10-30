#pragma once
// Only what the native nameplate engine takes from the product State.h: the shutdown flag and the active frame generator.
#include <cstdint>

class ScopedSkipHeapCapture
{
};

enum class FGOutput
{
    None,
    DLSSG,
    FSRFG,
    XeFG,
};

class State
{
  public:
    static State& Instance()
    {
        static State state;
        return state;
    }

    bool isShuttingDown = false;
    bool fgHudlessCompare = false;
    FGOutput activeFgOutput = FGOutput::DLSSG;
    uint32_t frameCount = 0;
};
