#pragma once
// Test double of ext/ffxiv/engine/EnginePass.h for the shadow mapping route test: the arming notice is counted and the own-work guard
// keeps a depth the test reads while the route runs (the real guard stops the engine seam from classifying the module's own draws).
#include "ext/ffxiv/engine/EnginePassRules.h"

namespace ext::ffxiv::engine
{
inline int g_armingNotices = 0;
inline int g_ownWorkDepth = 0;
inline void NotifyShadowArmingChanged() { ++g_armingNotices; }
struct OwnWork
{
    OwnWork() { ++g_ownWorkDepth; }
    ~OwnWork() { --g_ownWorkDepth; }
    OwnWork(const OwnWork&) = delete;
    OwnWork& operator=(const OwnWork&) = delete;
};
} // namespace ext::ffxiv::engine
