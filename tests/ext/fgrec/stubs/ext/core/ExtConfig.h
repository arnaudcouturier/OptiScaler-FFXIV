#pragma once
// Stand-in for ext/core/ExtConfig.h, shadowing it for the seams test only.
// The recorder seams read two keys (framePacing.fgRecorder, fg.autoMfg), so
// the stand-in carries just those; the key on/off behaviour is driven without
// the whole config binder.
namespace ext
{
template <class T> struct Setting
{
    T v {};
    Setting() = default;
    Setting(const T& x) : v(x) {}
    Setting& operator=(const T& x)
    {
        v = x;
        return *this;
    }
    T value_or_default() const { return v; }
};
struct StubFramePacingConfig
{
    Setting<bool> fgRecorder { false };
};
struct StubFgConfig
{
    Setting<bool> autoMfg { false };
};
struct StubConfig
{
    StubFramePacingConfig framePacing;
    StubFgConfig fg;
};
// Counts config reads: the key-off path must barely touch the config (a
// rate-limited re-read), never once per tap.
inline unsigned& CfgReads()
{
    static unsigned n = 0;
    return n;
}
inline StubConfig& Cfg()
{
    ++CfgReads();
    static StubConfig c;
    return c;
}
} // namespace ext
