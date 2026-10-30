#pragma once
// Stand-in for the product State.h, shadowing it for the seams test only. The
// recorder seams read the FG output enum and four frame/exe fields; the enum
// order matches the product header (fgrec_nopaths pins it), so the provider
// mapping is tested against the real values.
#include <cstdint>
#include <string>

enum class FGOutput : uint32_t
{
    NoFG,
    FSRFG,
    DLSSG,
    XeFG,
    Reprojection
};

class State
{
  public:
    static State& Instance()
    {
        static State instance;
        return instance;
    }

    std::string gameExe;
    uint64_t fgLastFrame = 0;
    uint64_t frameCount = 0;
    FGOutput activeFgOutput = FGOutput::NoFG;
};
