#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/fg/DynamicMfgControl.h"
#include "ext/fg/FgInternal.h"

#include <State.h>
#include <Util.h>
#include <framegen/IFGFeature_Dx12.h>
#include <misc/IdentifyGpu.h>
#include <proxies/Streamline_Proxy.h>

#include <atomic>
#include <mutex>

// Dynamic MFG on RTX 40, the glue: reads the live state into an AutoMfgTick, hands it to the controller
// (DynamicMfgControl.h, which holds every decision and is tested with fakes) and applies the answer. Called per game
// frame from ext::fg::AdjustDlssgOptions, so it runs on the same thread as DLSSG_Common::SetOptions; the menu reads the
// status from another thread, hence the mutex. Zero upstream lines: the count is written with set_volatile_value (the
// saved value of the slider stays what the user chose) and the options of this frame are changed in place.

namespace
{
std::mutex g_mutex;
ext::fg::AutoMfgController g_controller;
std::atomic<bool> g_everActive { false };

// What the last AfterDlssgSetOptions saw (it runs after the tick of the same frame, so the tick reads the frame before).
struct Observation
{
    bool valid = false;
    uint32_t status = 0;
    uint32_t presented = 0;
} g_observed;

// The display refresh is the desktop mode of the window's monitor: read every 5 s, 0 = unknown.
double CachedRefresh(uint64_t now)
{
    static uint64_t lastRead = 0;
    static double refresh = 0.0;
    if (lastRead == 0 || now - lastRead >= 5000)
    {
        lastRead = now;
        refresh = static_cast<double>(Util::GetActiveRefreshRate(State::Instance().currentSwapchainDesc.OutputWindow));
    }
    return refresh;
}

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
} // namespace

namespace ext::fg
{
bool AutoMfgTickFrame(IFGFeature* fg, sl::DLSSGOptions& options)
{
    auto& cfg = ext::Cfg().fg;
    const bool keyOn = cfg.autoMfg.value_or_default();
    const bool selfTest = cfg.autoMfgSelfTest.value_or_default();
    // Cheap exit for everybody who never turned it on.
    if (!keyOn && !selfTest && !g_everActive.load(std::memory_order_relaxed))
        return false;

    auto* config = Config::Instance();
    auto& state = State::Instance();
    const uint64_t now = GetTickCount64();

    AutoMfgTick in;
    in.nowMs = now;
    in.realMs = state.lastFGFrameTime;
    in.appliedFactor = static_cast<int>(options.numFramesToGenerate) + 1;
    in.selfTest = selfTest;
    in.maxCfg = cfg.autoMfgMax.value_or_default();
    in.targetCfg = cfg.autoMfgTarget.value_or_default();
    in.limit = config->FramerateLimit.value_or_default();
    in.refresh = CachedRefresh(now);
    in.gate.keyOn = keyOn;
    in.gate.dx12 = dynamic_cast<IFGFeature_Dx12*>(fg) != nullptr;
    in.gate.dlssgOutput = state.activeFgOutput == FGOutput::DLSSG;
    in.gate.nvidia = Gpu().nvidia;
    in.gate.archId = Gpu().arch;
    in.gate.followsGame = config->FGFollowsGame();
    in.gate.dynamicMode = options.mode == sl::DLSSGMode::eDynamic;
    in.gate.maxGenerated = fg->GetMaxInterpolationCount();
    in.detectedGenerated = state.dlssgDetectedInterpolationCount;

    AutoMfgOut out;
    {
        std::scoped_lock lock(g_mutex);
        in.stateValid = g_observed.valid;
        in.slStatus = g_observed.status;
        in.presented = g_observed.presented;
        out = g_controller.Tick(in);
    }
    if (out.controls)
        g_everActive.store(true, std::memory_order_relaxed);

    for (const auto& line : out.lines)
    {
        if (line.warn)
            LOG_WARN("{}", line.text);
        else
            LOG_INFO("{}", line.text);
    }

    if (out.setFactor || out.recover)
    {
        const int generated = std::max(1, out.factor - 1);
        // In memory only: Save Settings keeps storing the value the user chose.
        config->FGDLSSGInterpolationCount.set_volatile_value(generated);
        options.numFramesToGenerate = static_cast<uint32_t>(generated);
        if (out.recover)
            state.fgChanged = true; // the existing toggle path, once
    }
    return out.controls;
}

void AutoMfgObserve(bool queryOk, uint32_t status, uint32_t presented)
{
    if (!g_everActive.load(std::memory_order_relaxed) && !ext::Cfg().fg.autoMfg.value_or_default() &&
        !ext::Cfg().fg.autoMfgSelfTest.value_or_default())
        return;

    std::scoped_lock lock(g_mutex);
    g_observed = { queryOk, status, presented };
}

void ResetAutoMfg()
{
    std::scoped_lock lock(g_mutex);
    g_controller.OnActivated();
}

AutoMfgStatus AutoMfgGetStatus()
{
    std::scoped_lock lock(g_mutex);
    return g_controller.Status();
}
} // namespace ext::fg
