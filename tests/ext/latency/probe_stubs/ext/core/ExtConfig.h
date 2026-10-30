#pragma once
// Stand-in for ext/core/ExtConfig.h in the tick-probe tests: the one [FramePacing] key the probe mirrors, and the load hook it registers. The test
// calls PostLoad() to stand in for a config reload; the real binder (and the real hook array) is covered by the frame-pacing config test.
#include <cstddef>

namespace ext
{
struct FramePacingProbeKey
{
    bool on = false;
    bool TickProbeOn() const { return on; }
};

struct ExtConfigProbeStub
{
    FramePacingProbeKey framePacing;
    void PostLoad(); // runs the registered load hooks, like the product's ExtConfig::PostLoad
};

ExtConfigProbeStub& Cfg();

using KeysLoadedHook = void (*)();
inline constexpr std::size_t kMaxKeysLoadedHooks = 4;
void AddKeysLoadedHook(KeysLoadedHook hook);
} // namespace ext
