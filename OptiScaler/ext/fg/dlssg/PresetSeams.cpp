#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/fg/FgInternal.h"
#include "ext/fg/dlssg/PresetLogRelay.h"
#include "ext/fg/dlssg/PresetModule.h"
#include "ext/fg/dlssg/PresetRequest.h"
#include "ext/fg/dlssg/PresetStatus.h"
#include "ext/fg/dlssg/PresetSwitch.h"

#include <State.h>
#include <proxies/Streamline_Proxy.h>

#include <mutex>

// WS2: the live DLSS-G render preset change on the FG dispatch thread (DLSSG_Dx12::Dispatch calls ext::fg::PresetSwitchTick before
// it sets the options of the frame). The pure parts are PresetRequest.h and PresetSwitch.h, the hooks into NVIDIA's runtime are
// PresetModule.cpp.

namespace
{
using namespace ext::fg::preset;

std::mutex g_statusMutex;
Status g_status;
int g_reportedFailure = -1; // the target whose failure was already logged

Switcher& TheSwitcher()
{
    static Switcher switcher(Requests());
    return switcher;
}

void Publish(const Status& status)
{
    std::lock_guard lock(g_statusMutex);
    g_status = status;
}
} // namespace

namespace ext::fg::preset
{
Status GetStatus()
{
    Status copy;
    {
        std::lock_guard lock(g_statusMutex);
        copy = g_status;
    }
    // the request state is atomic and the module text has its own lock, so these are always current
    auto& state = Requests();
    copy.configured = ext::Cfg().fg.renderPreset.value_or_default();
    copy.active = state.Active();
    copy.boot = state.Boot();
    copy.moduleText = module::StatusText();
    return copy;
}
} // namespace ext::fg::preset

namespace ext::fg
{
bool PresetSwitchTick(IFGFeature* fg, const sl::ViewportHandle& viewport)
{
    FlushCandidateLines(); // unrecognised NVIDIA preset lines, queued by the log sink

    auto& state = Requests();
    auto& switcher = TheSwitcher();
    auto& cfg = ext::Cfg().fg;
    const int configured = cfg.renderPreset.value_or_default();

    const auto available = CheckAvailable(cfg.ampereMfgUnlock.value_or_default(),
                                          State::Instance().activeFgNvngx != FGNvngxReplacement::None,
                                          State::Instance().activeFgOutput == FGOutput::DLSSG);
    Status status;
    status.unavailable = available;
    status.switches = switcher.Generation();
    status.fail = switcher.LastFail();
    status.restartNeeded = switcher.RestartNeeded(configured);

    // Nothing to do: the request in use is the configured one, or the control is off, or this target already failed.
    if (available != Unavailable::None || configured == state.Active() || status.restartNeeded)
    {
        Publish(status);
        return false;
    }

    SwitchSteps steps;
    steps.prepare = [] { return module::Ensure(); };
    steps.off = [&]
    {
        if (StreamlineProxy::DLSSGSetOptions() == nullptr)
            return false;
        sl::DLSSGOptions off {};
        off.mode = sl::DLSSGMode::eOff;
        off.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
        return StreamlineProxy::DLSSGSetOptions()(viewport, off) == sl::Result::eOk;
    };
    steps.release = [&]
    {
        return StreamlineProxy::FreeResources() != nullptr &&
               StreamlineProxy::FreeResources()(sl::kFeatureDLSS_G, viewport) == sl::Result::eOk;
    };
    const int previous = state.Active();
    steps.activated = [&]
    {
        ReadoutState().BeginRequest();
        ext::fg::OnDeactivating(fg); // history, guide snapshots and the observed multiplier restart from nothing
        ResetDlssgPresentStats();
    };

    const auto result = switcher.Apply(configured, steps);
    status.switches = switcher.Generation();
    status.fail = switcher.LastFail();
    status.restartNeeded = switcher.RestartNeeded(configured);
    Publish(status);

    if (result == SwitchResult::Switched)
    {
        LOG_INFO("DLSS-G render preset: {} -> {}; frame generation was switched off and its resources released, it resumes on the "
                 "next frame with fresh history",
                 Name(previous), Name(configured));
        return true; // skip this frame's dispatch
    }
    if (result == SwitchResult::RestartNeeded && g_reportedFailure != configured)
    {
        g_reportedFailure = configured;
        LOG_WARN("DLSS-G render preset: {} cannot be applied while the game runs ({}); {} stays in use, restart needed", Name(configured),
                 FailText(switcher.LastFail()), Name(state.Active()));
    }
    else if (result != SwitchResult::RestartNeeded)
        g_reportedFailure = -1;
    return false;
}
} // namespace ext::fg
