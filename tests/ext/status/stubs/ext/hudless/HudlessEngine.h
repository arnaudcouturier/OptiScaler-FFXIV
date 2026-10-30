#pragma once
// Test stand-in for the hudless engine face: shares the mode/phase stand-ins
// above so mode and state stay consistent in one place.

#include "ext/hudless/HudlessConfig.h"

#include <string>

namespace ext::hudless
{
inline Status EngineStatus() { return g_status; }
} // namespace ext::hudless
