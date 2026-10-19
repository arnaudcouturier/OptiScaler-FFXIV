#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/fg/DynamicMfgGlue.h"
#include "ext/fg/FgInternal.h"
#include "ext/fg/dlssg/MfgUnlock.h"

#include <State.h>
#include <Util.h>
#include <framegen/IFGFeature_Dx12.h>
#include <misc/IdentifyGpu.h>
#include <proxies/Streamline_Proxy.h>

#include <mutex>

// Dynamic MFG on RTX 40, the process side of the glue: the tick body and the ForceDMFG / Ada-ceiling guard live in
// DynamicMfgGlue.h (templates over a "world", tested with fakes in tests/ext/fg/dynamic_mfg_glue.cpp); this file is the real
// world (Config, State, the DLSS-G feature, the log) and the objects the menu and the other threads reach. The glue runs per
// game frame from ext::fg::AdjustDlssgOptions, on the same thread as DLSSG_Common::SetOptions; the menu reads the status from
// another thread, hence the mutex. Zero upstream lines: the count is written with set_volatile_value (the saved value of the
// slider stays what the user chose) and the options of this frame are changed in place.

namespace
{
std::mutex g_mutex;
ext::fg::AutoMfgGlue g_glue;

struct GpuFacts
{
    bool known = false; // IdentifyGpu answered with a real vendor
    bool nvidia = false;
    uint32_t arch = 0;
};

// The facts of the primary GPU. IdentifyGpu answers with a default (vendor Invalid) when its adapter list is empty and tries
// again on its next call, so only a real answer is kept; an unknown one is asked again every few seconds, at most
// GpuAsk::kMaxTries times (the glue does nothing meanwhile, see AutoMfgGlue::TickOnce; after the last try the mode stays
// off for the session and one line says so). Called from the SetOptions thread only.
const GpuFacts& Gpu()
{
    static GpuFacts facts;
    static ext::fg::GpuAsk ask;
    if (facts.known || !ask.ShouldAsk(GetTickCount64()))
        return facts;
    const auto gpu = IdentifyGpu::getPrimaryGpu();
    if (gpu.vendorId != VendorId::Invalid)
    {
        ask.Answered();
        facts.known = true;
        facts.nvidia = gpu.vendorId == VendorId::Nvidia;
        facts.arch = static_cast<uint32_t>(gpu.nvidiaArchInfo.architecture_id);
    }
    else if (ask.GaveUp())
        LOG_WARN("AutoMFG: the GPU could not be identified after {} tries, automatic mode stays off for this session",
                 ext::fg::GpuAsk::kMaxTries);
    return facts;
}

// The facts and side channels DynamicMfgGlue.h asks of its world, read from the live process.
struct LiveWorld
{
    IFGFeature* fg;

    Config& config() { return *Config::Instance(); }
    ext::FgConfig& fgConfig() { return ext::Cfg().fg; }
    State& state() { return State::Instance(); }
    uint64_t nowMs() { return GetTickCount64(); }
    // The display refresh is the desktop mode of the window's monitor, 0 = unknown (the glue caches it).
    double readRefreshHz()
    {
        return static_cast<double>(Util::GetActiveRefreshRate(State::Instance().currentSwapchainDesc.OutputWindow));
    }
    bool dx12() { return dynamic_cast<IFGFeature_Dx12*>(fg) != nullptr; }
    bool dlssgOutputActive() { return State::Instance().activeFgOutput == FGOutput::DLSSG; }
    bool gpuKnown() { return Gpu().known; }
    bool nvidia() { return Gpu().nvidia; }
    uint32_t architecture() { return Gpu().arch; }
    int maxGenerated() { return fg->GetMaxInterpolationCount(); }
    bool dmfgSupported() { return fg->GetDMFGSupport(); }
    bool paused() { return fg->IsPaused(); }
    unsigned unlockedMax() { return MfgUnlock::UnlockedMax(); }
    std::unique_lock<std::mutex> lock() { return std::unique_lock<std::mutex>(g_mutex); }
    void log(bool warn, const std::string& text)
    {
        if (warn)
            LOG_WARN("{}", text);
        else
            LOG_INFO("{}", text);
    }
};
} // namespace

namespace ext::fg
{
void AutoMfgAdjustOptions(IFGFeature* fg, sl::DLSSGOptions& options)
{
    LiveWorld world { fg };
    g_glue.Adjust(world, options);
}

void AutoMfgObserve(bool queryOk, const sl::DLSSGState& state)
{
    // The skips (gate closed for good, neither key ever on) and the store are DynamicMfgGlue.h's Observe; only the config
    // object and the lock are read from the world here.
    LiveWorld world { nullptr };
    g_glue.Observe(world, queryOk, state);
}

void ResetAutoMfg()
{
    std::scoped_lock lock(g_mutex);
    g_glue.controller.OnActivated();
}

AutoMfgStatus AutoMfgGetStatus()
{
    std::scoped_lock lock(g_mutex);
    return g_glue.controller.Status();
}
} // namespace ext::fg
