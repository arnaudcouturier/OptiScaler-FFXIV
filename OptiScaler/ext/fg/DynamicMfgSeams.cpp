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
    bool nvidia = false;
    uint32_t arch = 0;
};

const GpuFacts& Gpu()
{
    static const GpuFacts facts = []
    {
        const auto gpu = IdentifyGpu::getPrimaryGpu();
        GpuFacts f;
        f.nvidia = gpu.vendorId == VendorId::Nvidia;
        f.arch = static_cast<uint32_t>(gpu.nvidiaArchInfo.architecture_id);
        return f;
    }();
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

void AutoMfgObserve(bool queryOk, uint32_t status, uint32_t presented)
{
    if (!g_glue.EverActive() && !ext::Cfg().fg.autoMfg.value_or_default() && !ext::Cfg().fg.autoMfgSelfTest.value_or_default())
        return;

    std::scoped_lock lock(g_mutex);
    g_glue.observed = { queryOk, status, presented };
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
