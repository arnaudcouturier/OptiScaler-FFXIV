#include "pch.h"

#include "ext/reshade/ReShadeBoundary.h"

#include "ext/core/ExtConfig.h"
#include "ext/reshade/ReShadeAddon.h"
#include "ext/reshade/ReShadeBoundaryCore.h"
#include "ext/reshade/ReShadeEvents.h"

#include <State.h>

#include <atomic>
#include <mutex>

namespace ext::reshade
{
namespace boundary
{
Applier& Instance()
{
    static Applier* applier = new Applier(*NewLiveBackend()); // never destroyed (nothing runs under the loader lock at detach)
    return *applier;
}
} // namespace boundary

namespace
{
// One log line per distinct failure text, at most a handful per process: a failing integration must be visible in the log
// without turning the draw hook into a log writer.
void LogOutcome(ApplyResult result)
{
    static std::atomic<bool> loggedFirstApply { false };
    static std::mutex mutex;
    static std::string seen[8];
    static size_t seenCount = 0;

    if (result == ApplyResult::Applied)
    {
        if (!loggedFirstApply.exchange(true))
            LOG_INFO("ReShade presets: first call of render_effects in place at the UI boundary succeeded (ReShade's own end-of-frame pass is "
                     "skipped on those frames)");
        return;
    }
    if (result != ApplyResult::Failed)
        return;

    const std::string detail = boundary::Instance().GetStats().lastDetail;
    std::lock_guard lock(mutex);
    for (size_t i = 0; i < seenCount; ++i)
        if (seen[i] == detail)
            return;
    if (seenCount < std::size(seen))
        seen[seenCount++] = detail;
    LOG_WARN("ReShade presets at the UI boundary failed: {} (the frame is not used for UI-free frame generation)", detail);
}
} // namespace

ApplyResult ApplyPresetsInPlace(ID3D11DeviceContext* context, ID3D11Resource* renderTarget)
{
    try
    {
        if (State::Instance().isShuttingDown)
            return ApplyResult::NoReShade;
        const PresetsMode mode = ParsePresetsMode(ext::Cfg().reshade.presets.value_or_default());
        const ApplyResult result = boundary::Instance().Apply(mode, Registered(), context, renderTarget);
        LogOutcome(result);
        return result;
    }
    catch (...)
    {
        return ApplyResult::Failed;
    }
}

void FrameEnded()
{
    try
    {
        if (Registered())
            boundary::Instance().FrameEnded();
    }
    catch (...)
    {
    }
}

ApplyStats Stats()
{
    try
    {
        return boundary::Instance().GetStats();
    }
    catch (...)
    {
        return {};
    }
}
} // namespace ext::reshade
