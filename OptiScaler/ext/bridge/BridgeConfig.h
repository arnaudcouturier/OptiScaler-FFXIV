#pragma once
// [Bridge] config keys (docs/SEAMS-catalog.md "bridge"); the comment block players read is
// OptiScaler/ext/ini/Bridge.ini.
//
// Header-only on purpose, like ext/window/WindowConfig.h: several test recipes link ExtConfig.cpp without the feature
// sources, so Bind() lives in ExtConfig.cpp next to the other small config structs and PostLoad() only publishes into
// the cached copy the hand-over reads (ext/bridge/BridgeSeams.h).

#include <Config.h>

#include "ext/bridge/BridgeSeams.h"

namespace ext
{
namespace config
{
class Binder;
}

// [Bridge] FlushAfterSignal: flush the D3D11 immediate context right after the fence Signal of the D3D11 -> D3D12
// hand-over was accepted (SyncDx11ToDx12), so the recorded commands are submitted then instead of at the game's own
// flush. An experiment: default false, which is exactly upstream's call sequence.
struct BridgeConfig
{
    CustomOptional<bool> flushAfterSignal { false };

    void Bind(config::Binder& b);

    // End of every config::Load. The one line for the key is not logged here: the first config load runs before the
    // logger exists (ext/core/Banner.cpp calls bridge::LogStartupState for it).
    void PostLoad() noexcept { bridge::PublishFlushAfterSignal(flushAfterSignal.value_or_default()); }
};
} // namespace ext
