#pragma once
// The switch's monotonic clock, shared by everything that records a moment the switch compares against (the hook
// detach wait, the resize record in ext/hdr/BridgeHdr.cpp). Defined once, in HookRebind.cpp.
#include <cstdint>

namespace ext::fg::outswitch
{
uint64_t NowMs();
void SleepMs(uint32_t ms);
} // namespace ext::fg::outswitch
