#pragma once
// Dynamic MFG on RTX 40, the glue between the process and the controller (DynamicMfgControl.h). It reads the live state
// into an AutoMfgTick, hands it to the controller, applies the answer to the DLS-G options of this frame, and then runs the
// ForceDMFG / Ada-ceiling guard on the result. It was DynamicMfgSeams.cpp's tick body and DlssgSeams.cpp's ApplyDlssgGuard;
// moved here as templates over a "world" so the same lines run in the DLL (DynamicMfgSeams.cpp: Config, State, IFGFeature)
// and in tests/ext/fg/dynamic_mfg_glue.cpp (fakes, the real CustomOptional and the real FgConfig).
//
// What a World provides (facts and side channels only; the decisions and the writes stay in this file):
//   config()      the upstream Config: FramerateLimit, FGDLSSGInterpolationCount, FGFollowsGame()
//   fgConfig()    ext::FgConfig: autoMfg, autoMfgSelfTest, autoMfgMax, autoMfgTarget
//   state()       the upstream State: lastFGFrameTime, dlssgDetectedInterpolationCount, fgChanged
//   nowMs()       the clock, in ms
//   readRefreshHz()  the raw display refresh of the window's monitor (a system call: only read through the 5 s cache)
//   dx12(), dlssgOutputActive(), nvidia(), architecture(), maxGenerated(), dmfgSupported(), paused(), unlockedMax()
//   lock()        a guard held while the controller runs (the menu reads its status from another thread)
//   log(warn, text)
// Options is sl::DLSSGOptions (mode, numFramesToGenerate).

#include "ext/fg/DynamicMfgControl.h"
#include "ext/fg/DynamicMfgGate.h"

#include <sl_dlss_g.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace ext::fg
{
// What the last AfterDlssgSetOptions saw (it runs after the tick of the same frame, so the tick reads the frame before).
struct AutoMfgObservation
{
    bool valid = false;
    uint32_t status = 0;
    uint32_t presented = 0;
};

class AutoMfgGlue
{
  public:
    static constexpr uint64_t kRefreshCacheMs = 5000;

    AutoMfgController controller; // guarded by World::lock()
    AutoMfgObservation observed;  // guarded by World::lock()

    bool EverActive() const { return everActive_.load(std::memory_order_relaxed); }
    // A closed gate that cannot reopen was seen (the observation of the DLS-G state is not needed then).
    bool ClosedForGood() const { return closedForGood_.load(std::memory_order_relaxed); }

    // One tick per game frame, before the guard. True = the selector (or the self-test) is in charge of this frame's count.
    // Fail open: whatever goes wrong in here, the options stay the game's and the answer is false.
    template <class World, class Options> bool Tick(World& w, Options& options)
    {
        const bool keyOn = w.fgConfig().autoMfg.value_or_default();
        const bool selfTest = w.fgConfig().autoMfgSelfTest.value_or_default();
        // Cheap exit for everybody who never turned it on.
        if (!keyOn && !selfTest && !EverActive())
            return false;
        // A closed gate that cannot reopen (RTX 50, not NVIDIA, not D3D12, stopped by the kill-switch): the tick has said
        // what it had to say once (the inactive line, the self-test result); until the keys change there is nothing to do.
        if (ClosedForGood() && keyOn == latchedKeyOn_ && selfTest == latchedSelfTest_)
            return false;

        bool controls = false;
        try
        {
            controls = TickOnce(w, options, keyOn, selfTest);
            failing_ = false;
        }
        catch (...)
        {
            if (!std::exchange(failing_, true)) // one line per episode of failures, not one per frame
                LogQuietly(w, true, "AutoMFG: an error stopped the automatic multiplier for this frame, the game's count is used");
            return false;
        }
        return controls;
    }

    // The DLSSGOptions guard (DynamicMfgGate.h has the rules): where Streamline reports dynamic MFG as unsupported the
    // forced dynamic mode falls back to the fixed one, and on a card whose ceiling the Ada unlock opened an automatic choice
    // never uses more than 3 generated frames. The clamp is for Streamline only: the configured count (the slider the user
    // set) is not written, so turning ForceDMFG off gives the manual 5x/6x back with nothing to restore.
    template <class World, class Options> void Guard(World& w, Options& options, bool autoMfgControls)
    {
        OptionsGuardIn in;
        in.dynamicMode = options.mode == sl::DLSSGMode::eDynamic;
        in.dmfgSupported = w.dmfgSupported();
        in.adaUnlockActive = w.unlockedMax() > static_cast<unsigned>(kAutoMaxGenerated);
        in.autoMfgControls = autoMfgControls;
        in.generated = static_cast<int>(options.numFramesToGenerate);

        const auto out = GuardDlssgOptions(in);
        if (out.dynamicToOn)
        {
            options.mode = sl::DLSSGMode::eOn;
            if (!std::exchange(warnedDynamic_, true))
                LogQuietly(w, true,
                           "DLSSG: Force Dynamic MFG is on but Streamline reports dynamic multi frame generation as "
                           "unsupported; using the fixed mode with the configured count");
        }
        if (out.ceilingClamped)
        {
            if (!std::exchange(warnedClamp_, true))
            {
                try
                {
                    w.log(true, std::format("DLSSG: automatic multi frame generation is limited to {}x on this card, {} "
                                            "generated frames were requested; using {}",
                                            kAutoMaxGenerated + 1, options.numFramesToGenerate, out.generated));
                }
                catch (...)
                {
                }
            }
            options.numFramesToGenerate = static_cast<uint32_t>(out.generated);
        }
    }

    // What AdjustDlssgOptions runs: the tick first (it may change the count of this frame and says whether it did), then the
    // guard on the result.
    template <class World, class Options> void Adjust(World& w, Options& options)
    {
        const bool autoControls = Tick(w, options);
        Guard(w, options, autoControls);
    }

  private:
    template <class World, class Options> bool TickOnce(World& w, Options& options, bool keyOn, bool selfTest)
    {
        auto& cfg = w.fgConfig();
        auto& config = w.config();
        auto& state = w.state();

        AutoMfgTick in;
        in.nowMs = w.nowMs();
        in.realMs = state.lastFGFrameTime;
        in.appliedFactor = static_cast<int>(options.numFramesToGenerate) + 1;
        in.selfTest = selfTest;
        in.maxCfg = cfg.autoMfgMax.value_or_default();
        in.targetCfg = cfg.autoMfgTarget.value_or_default();
        in.limit = config.FramerateLimit.value_or_default();
        in.refresh = Refresh(w, in.nowMs);
        in.gate.keyOn = keyOn;
        in.gate.dx12 = w.dx12();
        in.gate.dlssgOutput = w.dlssgOutputActive();
        in.gate.nvidia = w.nvidia();
        in.gate.archId = w.architecture();
        in.gate.followsGame = config.FGFollowsGame();
        // Force Dynamic MFG only counts where Streamline supports it: otherwise the guard turns the mode into the fixed one.
        in.gate.dynamicMode = options.mode == sl::DLSSGMode::eDynamic && w.dmfgSupported();
        in.gate.maxGenerated = w.maxGenerated();
        in.detectedGenerated = state.dlssgDetectedInterpolationCount;
        in.paused = w.paused();

        AutoMfgOut out;
        AutoMfgGate gate = AutoMfgGate::KeyOff;
        {
            [[maybe_unused]] auto lock = w.lock();
            in.stateValid = observed.valid;
            in.slStatus = observed.status;
            in.presented = observed.presented;
            out = controller.Tick(in);
            gate = controller.Status().gate;
        }
        // Latched only after a full tick has seen the closed gate (the inactive line, the self-test result and the restore
        // of the count all happen inside that tick).
        closedForGood_.store(IsPermanentlyClosed(gate), std::memory_order_relaxed);
        latchedKeyOn_ = keyOn;
        latchedSelfTest_ = selfTest;
        if (out.controls)
            everActive_.store(true, std::memory_order_relaxed);

        // Applied before anything is formatted or logged: a failure further down must not lose a switch the controller
        // already committed to (its watch would then see the old count and call it a broken change).
        if (out.setFactor || out.recover)
        {
            const int generated = std::max(1, out.factor - 1);
            // In memory only: Save Settings keeps storing the value the user chose. Written in Frame Gen "Default" too: the
            // configured count is not used there (Config::FGInterpolationCount), but a self-test restore must not leave
            // the test's last step behind for when the user leaves Default.
            config.FGDLSSGInterpolationCount.set_volatile_value(generated);
            // In Default the game owns this frame's count.
            if (!in.gate.followsGame)
                options.numFramesToGenerate = static_cast<uint32_t>(generated);
            if (out.recover)
                state.fgChanged = true; // the existing toggle path, once
        }

        for (const auto& line : out.lines)
            LogQuietly(w, line.warn, line.text);
        return out.controls;
    }

    // The display refresh, read every kRefreshCacheMs. 0 or 1 Hz is what Windows reports for "unspecified": unknown (0).
    template <class World> double Refresh(World& w, uint64_t now)
    {
        if (refreshReadAt_ == 0 || now - refreshReadAt_ >= kRefreshCacheMs)
        {
            refreshReadAt_ = now;
            refresh_ = UsableRefresh(w.readRefreshHz());
        }
        return refresh_;
    }

    template <class World> static void LogQuietly(World& w, bool warn, const char* text)
    {
        try
        {
            w.log(warn, std::string(text));
        }
        catch (...)
        {
        }
    }

    template <class World> static void LogQuietly(World& w, bool warn, const std::string& text)
    {
        try
        {
            w.log(warn, text);
        }
        catch (...)
        {
        }
    }

    std::atomic<bool> everActive_ { false };
    uint64_t refreshReadAt_ = 0;
    double refresh_ = 0.0;
    std::atomic<bool> closedForGood_ { false };
    bool latchedKeyOn_ = false;
    bool latchedSelfTest_ = false;
    bool failing_ = false;
    bool warnedDynamic_ = false;
    bool warnedClamp_ = false;
};
} // namespace ext::fg
