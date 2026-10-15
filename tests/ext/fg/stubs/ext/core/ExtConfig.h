#pragma once
// Stand-in for ext::Cfg().fg: only the XeFG fields the unlock and pacing headers read
#include "ext/fg/FgConfig.h"
namespace ext
{
template <class T> struct Setting
{
    T v;
    T value_or_default() const { return v; }
};
struct StubFgConfig
{
    Setting<bool> xefgUnlock { true }, xefgExtraPacing { false };
    Setting<int> xefgMaxInterpolatedFrames { 7 };
};
struct StubConfig
{
    StubFgConfig fg;
};
inline StubConfig& Cfg()
{
    static StubConfig c;
    return c;
}
} // namespace ext
