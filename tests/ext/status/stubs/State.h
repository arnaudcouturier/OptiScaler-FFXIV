#pragma once
// Test stand-in for the upstream State: the frame-generation codes the status
// collector names, the exe name it reports, and the NGX loader path list. The
// list counts every read so the tests can prove the collector only touches it
// on write-due ticks after start-up settled.

#include <cstdint>
#include <string>
#include <vector>

enum class FGInput : uint32_t
{
    NoFG,
    Upscaler,
    DLSSG,
    NvngxFG,
    FSRFG,
    FSRFG30,
    XeFG,
    ForceXeLL,
};

enum class FGOutput : uint32_t
{
    NoFG,
    FSRFG,
    DLSSG,
    XeFG,
    Reprojection
};

// A list that records how often the collector copies it: production code must
// read it at most on write-due ticks, never on every tick.
inline unsigned g_loaderListReads = 0;

struct LoaderPathList
{
    std::vector<std::wstring> paths;

    operator std::vector<std::wstring>() const
    {
        ++g_loaderListReads;
        return paths;
    }
};

struct State
{
    static State& Instance()
    {
        static State instance;
        return instance;
    }

    std::string gameExe = "ffxiv_dx11.exe";
    FGInput activeFgInput = FGInput::NoFG;
    FGOutput activeFgOutput = FGOutput::NoFG;
    LoaderPathList NVNGX_FeatureInfo_Paths;
};
