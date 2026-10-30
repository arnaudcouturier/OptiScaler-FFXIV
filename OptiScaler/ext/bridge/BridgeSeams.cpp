#include "pch.h"

#include "ext/bridge/BridgeSeams.h"

namespace ext::bridge
{
void LogStartupState() noexcept
{
    try
    {
        // Off is the default and needs no line: a run with the experiment off looks exactly like an upstream run.
        if (!FlushAfterSignal())
            return;

        LOG_INFO("Bridge FlushAfterSignal on: the D3D11 context is flushed after each D3D11 -> D3D12 fence Signal "
                 "(experiment, off in the ini by default)");
    }
    catch (...)
    {
        // A log line must never take the game down.
    }
}
} // namespace ext::bridge
