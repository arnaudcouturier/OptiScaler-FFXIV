#pragma once
// Test stand-in for the feature config aggregate: only the keys the status
// collector and writer read are modelled. Every Cfg() call is counted so the
// tests can prove the present path reads the key at most once per scope.

namespace ext
{
struct BoolKey
{
    bool value = false;
    bool value_or_default() const { return value; }
};

struct IntKey
{
    int value = 0;
    int value_or_default() const { return value; }
};

struct FfxivConfig
{
};

struct NrConfig
{
    BoolKey enabled;
};

struct HdrConfig
{
    BoolKey ffxivHdr;
    IntKey mode;
};

struct StatusConfig
{
    BoolKey fileEnabled;
};

struct ExtConfig
{
    FfxivConfig ffxiv;
    NrConfig nr;
    HdrConfig hdr;
    StatusConfig status;
};

inline ExtConfig g_cfg;
inline unsigned g_cfgReads = 0;

inline ExtConfig& Cfg()
{
    ++g_cfgReads;
    return g_cfg;
}
} // namespace ext
