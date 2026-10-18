#pragma once
// Dynamic MFG on RTX 40: the controller between the live state and the selector. Pure: the glue (DynamicMfgSeams.cpp)
// fills an AutoMfgTick snapshot per SetOptions call and applies what AutoMfgOut says; everything decided here is tested
// with a fake clock (tests/ext/fg/dynamic_mfg_control_unit.cpp).
//
// It owns: the gating table (log the reason once), the activation / gap restarts, the kill-switch that watches every
// switch for one second, and the one-off self-test cycle 2x, 3x, 4x, 3x, 2x.
//
// Kill-switch, after each switch (study 3.2):
//   clean   - worst real-frame interval below the threshold, applied count = requested, Streamline status 0;
//   costly  - a worst interval >= max(100 ms, 3 x the interval before the switch): the signature of a feature
//             re-creation. The hold becomes 12 s and probing stops; a second one stops automatic mode for the session;
//   broken  - Streamline status != 0, applied count not reached after the second, or presented stuck at 1: one recovery
//             through the existing toggle path to the last good count, then automatic mode stops for the session.

#include "ext/fg/DynamicMfg.h"
#include "ext/fg/DynamicMfgGate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace ext::fg
{
// One SetOptions call, as plain data.
struct AutoMfgTick
{
    uint64_t nowMs = 0;
    double realMs = 0.0;     // State::lastFGFrameTime: interval between game presents
    int appliedFactor = 2;   // options.numFramesToGenerate + 1 before the tick
    // config
    bool selfTest = false;   // [DLSSG] AutoMFGSelfTest
    int maxCfg = 4;          // [DLSSG] AutoMFGMax
    double targetCfg = 0.0;  // [DLSSG] AutoMFGTarget
    double limit = 0.0;      // FramerateLimit
    double refresh = 0.0;    // display refresh, Hz, 0 = unknown
    GateInputs gate;         // the gating inputs (stopped is filled in by the controller)
    // what the last AfterDlssgSetOptions saw
    bool stateValid = false; // slDLSSGGetState succeeded
    uint32_t slStatus = 0;   // DLSSGState::status
    uint32_t presented = 0;  // DLSSGState::numFramesActuallyPresented
    int detectedGenerated = 0; // State::dlssgDetectedInterpolationCount (generated frames the plugin evaluates with)
};

struct AutoMfgLine
{
    bool warn = false;
    std::string text;
};

struct AutoMfgOut
{
    bool controls = false;   // the selector (or the self-test) is in charge of this frame's count
    bool setFactor = false;  // switch in place to `factor`
    bool recover = false;    // back to `factor` through the existing toggle path (state.fgChanged)
    int factor = 0;
    std::vector<AutoMfgLine> lines;
};

struct AutoMfgStatus
{
    AutoMfgGate gate = AutoMfgGate::KeyOff;
    int factor = 0;          // factor in force
    double target = 0.0;
    double refresh = 0.0;
    double limit = 0.0;
    int hardMax = 0;
    bool stopped = false;
    bool selfTestRunning = false;
};

// Log texts (pure, so the formats are tested).
inline std::string FormatSwitch(const DynamicMfg::Info& info, double target)
{
    using Why = DynamicMfg::Why;
    if (info.why == Why::Clamp)
        return std::format("AutoMFG: {}x -> {}x (outside the allowed range)", info.from, info.to);
    const double fps = info.avgMs > 0.0 ? 1000.0 / info.avgMs : 0.0;
    std::string reason;
    switch (info.why)
    {
    case Why::Up:
        reason = "up";
        break;
    case Why::Down:
        reason = "down";
        break;
    case Why::FastDown:
        reason = "down, output far above the target";
        break;
    case Why::Probe:
        reason = std::format("probe: output stayed at the target for {} s", info.probeMs / 1000);
        break;
    case Why::ProbeRevert:
        reason = std::format("probe failed, presented {:.0f} is under 0.93 x target, next probe in {} s",
                             info.presented, info.probeMs / 1000);
        break;
    case Why::RegretRevert:
        reason = std::format("step gained too little: presented {:.0f} against {:.0f} before, {}x refused for 30 s",
                             info.presented, info.presentedBefore, info.from);
        break;
    default:
        reason = "?";
        break;
    }
    return std::format("AutoMFG: {}x -> {}x (real {:.1f} ms = {:.0f} fps, presented {:.0f}, target {:.0f}, {})",
                       info.from, info.to, info.avgMs, fps, info.presented, target, reason);
}

class AutoMfgController
{
  public:
    static constexpr uint64_t kGapMs = 1000;        // longer than this between two ticks = a pause
    static constexpr uint64_t kWatchMs = 1000;      // each switch is watched this long
    static constexpr double kCostlyMs = 100.0;      // a worst interval at least this long (and 3 x the usual one)
    static constexpr uint64_t kStepMs = 6000;       // self-test: time between two changes
    static constexpr uint64_t kWarmupMs = 3000;     // self-test: before the first change
    static constexpr double kSelfTestPassMs = 40.0; // self-test: worst interval to call it in-place
    static constexpr int kSelfTestSteps = 5;

    AutoMfgOut Tick(const AutoMfgTick& in)
    {
        AutoMfgOut out;
        GateInputs g = in.gate;
        g.stopped = stopped_;
        const AutoMfgGate gate = EvaluateGate(g);
        status_.gate = gate;
        status_.stopped = stopped_;
        status_.refresh = in.refresh;
        status_.limit = in.limit;
        status_.target = ResolveTarget(in.targetCfg, in.limit, in.refresh);

        if (gate != AutoMfgGate::Open)
        {
            GateClosed(gate, in, out);
            return out;
        }
        const uint64_t now = in.nowMs;
        const int cycleMax = std::min(4, in.gate.maxGenerated + 1);
        const int hardMax = HardMaxFactor(in.maxCfg, in.gate.maxGenerated);
        const double target = status_.target;
        status_.hardMax = hardMax;

        if (lastGate_ != AutoMfgGate::Open)
        {
            lastGate_ = AutoMfgGate::Open;
            restart_ = true;
            Info(out, std::format("AutoMFG: active (highest {}x, target {:.0f} fps, display {:.0f} Hz, limit {:.0f})",
                                  hardMax, target, in.refresh, in.limit));
        }
        const bool gap = lastTick_ != 0 && now >= lastTick_ && now - lastTick_ > kGapMs;
        lastTick_ = now;
        if (restart_ || gap)
        {
            // Activation or a pause: whatever the open watch saw was the toggle path or the pause, not the selector.
            w_ = {};
            sel_.Restart(now);
            lastGood_ = 0;
            if (st_.running)
                SelfTestInterrupted(out, now, restart_ ? "frame generation was reactivated" : "the game paused");
            restart_ = false;
        }
        int current = std::max(1, in.appliedFactor);
        if (lastGood_ == 0)
            lastGood_ = current;
        status_.factor = current;
        out.controls = true;

        if (in.selfTest && !st_.done && !st_.running)
            SelfTestBegin(in, out, cycleMax);

        // Watch of the last switch.
        if (w_.open)
        {
            if (std::isfinite(in.realMs))
                w_.worst = std::max(w_.worst, in.realMs);
            if (in.stateValid)
            {
                ++w_.samples;
                if (in.slStatus != 0)
                {
                    w_.statusBad = true;
                    w_.statusCode = in.slStatus;
                }
                if (in.presented >= 2)
                    w_.seenMulti = true;
            }
            if (now - w_.since >= kWatchMs)
                Verdict(in, out);
        }
        if (stopped_) // the verdict just stopped it
        {
            status_.stopped = true;
            return out;
        }

        int next = current;
        if (st_.running)
            next = SelfTestStep(in, out, now, current);
        else
        {
            // fed while a switch is watched too (the hold keeps it from switching inside the watch)
            const int pick = sel_.Select(in.realMs, target, now, current, hardMax, costly_ == 0);
            if (!w_.open)
                next = pick;
        }
        status_.selfTestRunning = st_.running;

        if (next != current)
        {
            const bool fromSelector = !st_.running;
            if (fromSelector)
                Line(out, false, FormatSwitch(sel_.Last(), target));
            else
                Line(out, false, std::format("AutoMFG self-test: {}x -> {}x", current, next));
            w_ = {};
            w_.open = true;
            w_.since = now;
            w_.from = current;
            w_.to = next;
            w_.baselineMs = fromSelector ? sel_.Last().avgMs : st_.baselineMs;
            if (!fromSelector)
                sel_.Restart(now);
            out.setFactor = true;
            out.factor = next;
            status_.factor = next;
        }
        return out;
    }

    // DLSS-G was (re)activated: a new observation window, any open watch closes without a verdict. What the kill-switch
    // learned (costly count, session stop, reduced trust) stays.
    void OnActivated() { restart_ = true; }

    const AutoMfgStatus& Status() const { return status_; }
    bool Stopped() const { return stopped_; }
    int CostlyCount() const { return costly_; }
    const DynamicMfg& Selector() const { return sel_; }
    bool SelfTestDone() const { return st_.done; }

  private:
    struct Watch
    {
        bool open = false;
        uint64_t since = 0;
        int from = 0, to = 0;
        double worst = 0.0;
        double baselineMs = 0.0;
        bool statusBad = false;
        uint32_t statusCode = 0;
        bool seenMulti = false;
        int samples = 0;
    };

    struct SelfTest
    {
        bool running = false;
        bool done = false;
        uint64_t firstAt = 0; // time of the first step
        int step = 0;         // next step to take
        int changes = 0;
        double worst = 0.0;
        double baselineMs = 0.0;
        int costly = 0;
        // the 4x step: real interval over its second half
        bool at4x = false;
        uint64_t at4xSince = 0;
        double sumMs = 0.0;
        int n = 0;
        std::string failure;
    };

    static constexpr int kCycle[kSelfTestSteps] = { 2, 3, 4, 3, 2 };

    void Line(AutoMfgOut& out, bool warn, std::string text) { out.lines.push_back({ warn, std::move(text) }); }
    void Info(AutoMfgOut& out, std::string text) { Line(out, false, std::move(text)); }
    void Warn(AutoMfgOut& out, std::string text) { Line(out, true, std::move(text)); }

    void GateClosed(AutoMfgGate gate, const AutoMfgTick& in, AutoMfgOut& out)
    {
        const bool wasOpen = lastGate_ == AutoMfgGate::Open;
        if (gate != lastGate_)
        {
            if (gate == AutoMfgGate::KeyOff)
            {
                if (wasOpen)
                    Info(out, "AutoMFG: switched off, the multiplier stays where it is");
            }
            else
                Warn(out, std::format("AutoMFG: inactive, {}", GateText(gate)));
            lastGate_ = gate;
        }
        if (wasOpen)
        {
            w_ = {};
            if (st_.running)
                SelfTestFinish(out, false, std::format("interrupted: {}", GateText(gate)));
        }
        if (in.selfTest && !st_.done)
            SelfTestFinish(out, false, std::format("not run: {}", GateText(gate)));
    }

    void Verdict(const AutoMfgTick& in, AutoMfgOut& out)
    {
        const Watch w = w_;
        w_ = {};
        const int requested = w.to - 1;
        const bool countWrong = in.detectedGenerated != requested;
        const bool stuck = w.samples >= 5 && !w.seenMulti;
        const double costlyAt = std::max(kCostlyMs, 3.0 * w.baselineMs);
        if (w.statusBad || countWrong || stuck)
        {
            std::string why;
            if (w.statusBad)
                why = std::format("Streamline status {:#x}", w.statusCode);
            else if (countWrong)
                why = std::format("applied count {} != requested {} (generated frames)", in.detectedGenerated, requested);
            else
                why = "presented frames stuck at 1";
            Warn(out, std::format("AutoMFG: {}, switch {}x -> {}x; going back to {}x, automatic mode stopped", why,
                                  w.from, w.to, lastGood_));
            out.recover = true;
            out.factor = lastGood_;
            stopped_ = true;
            if (st_.running)
                SelfTestFinish(out, false, std::format("{}", why));
            return;
        }
        if (w.worst >= costlyAt)
        {
            ++costly_;
            sel_.SetHoldMs(DynamicMfg::kReducedTrustHoldMs);
            Warn(out, std::format("AutoMFG: switch {}x -> {}x cost a {:.0f} ms frame (a re-creation of DLS-G looks like "
                                  "this), holding {} s between changes and no more probes",
                                  w.from, w.to, w.worst, DynamicMfg::kReducedTrustHoldMs / 1000));
            if (st_.running)
            {
                ++st_.costly;
                st_.worst = std::max(st_.worst, w.worst);
            }
            else if (costly_ >= 2)
            {
                stopped_ = true;
                Warn(out, "AutoMFG: changing the count recreates DLSS-G on this setup; automatic MFG stopped");
            }
            lastGood_ = w.to;
            return;
        }
        Info(out, std::format("AutoMFG: {}x settled (worst interval {:.0f} ms, applied {}x)", w.to, w.worst, w.to));
        if (st_.running)
            st_.worst = std::max(st_.worst, w.worst);
        lastGood_ = w.to;
    }

    // --- self-test ---------------------------------------------------------------------------------------------
    void SelfTestBegin(const AutoMfgTick& in, AutoMfgOut& out, int cycleMax)
    {
        if (cycleMax < 4)
        {
            SelfTestFinish(out, false, std::format("not run: the DLSS-G maximum is {}x and the cycle needs 4x, enable "
                                                   "AdaMfgUnlock and restart",
                                                   cycleMax));
            return;
        }
        st_ = {};
        st_.running = true;
        st_.firstAt = in.nowMs + kWarmupMs;
        st_.baselineMs = sel_.AverageMs();
        Info(out, std::format("AutoMFG self-test: started, 2x -> 3x -> 4x -> 3x -> 2x, one change every {} s",
                              kStepMs / 1000));
    }

    void SelfTestInterrupted(AutoMfgOut& out, uint64_t now, const char* why)
    {
        if (st_.changes > 0)
            SelfTestFinish(out, false, std::format("interrupted: {}", why));
        else
            st_.firstAt = now + kWarmupMs; // nothing changed yet: start the timeline over
    }

    void SelfTestFinish(AutoMfgOut& out, bool pass, std::string detail)
    {
        if (st_.done)
            return;
        EmitRefreshField(out, true);
        st_.running = false;
        st_.done = true;
        sel_.Restart(lastTick_);
        if (pass)
            Info(out, std::format("AutoMFG self-test: PASS ({} in-place changes, worst interval {:.0f} ms, all applied "
                                  "counts matched, no Streamline errors)",
                                  st_.changes, st_.worst));
        else
            Warn(out, std::format("AutoMFG self-test: FAIL ({})", detail));
        if (costly_ >= 2 && !stopped_)
        {
            stopped_ = true;
            Warn(out, "AutoMFG: changing the count recreates DLSS-G on this setup; automatic MFG stopped");
        }
    }

    // The 4x step answers whether the output is refresh-bound: real interval against 4 / refresh.
    void EmitRefreshField(AutoMfgOut& out, bool force)
    {
        if (!st_.at4x || st_.n == 0)
            return;
        if (!force && st_.n < 10)
            return;
        const double ms = st_.sumMs / st_.n;
        st_.at4x = false;
        if (!(status_.refresh > 0.0))
        {
            Info(out, std::format("AutoMFG self-test: at 4x real interval {:.1f} ms, display refresh unknown", ms));
            return;
        }
        const double bound = 4000.0 / status_.refresh;
        const char* verdict = ms < bound * 0.95   ? "faster than 4/refresh, not refresh-bound"
                              : ms <= bound * 1.05 ? "refresh-bound"
                                                   : "GPU-bound";
        Info(out, std::format("AutoMFG self-test: at 4x real interval {:.1f} ms vs 4/refresh {:.1f} ms -> {}", ms, bound,
                              verdict));
    }

    int SelfTestStep(const AutoMfgTick& in, AutoMfgOut& out, uint64_t now, int current)
    {
        // the second half of the 4x step is what is measured
        if (st_.at4x && now >= st_.at4xSince && std::isfinite(in.realMs) && in.realMs > 0.0)
        {
            st_.sumMs += in.realMs;
            ++st_.n;
        }
        if (st_.step >= kSelfTestSteps)
        {
            if (!w_.open) // the last watch has its verdict
            {
                const bool pass = st_.costly == 0 && st_.changes >= kSelfTestSteps - 1 && st_.worst < kSelfTestPassMs;
                std::string why;
                if (st_.costly > 0)
                    why = std::format("re-created: worst interval {:.0f} ms; a count change re-creates DLSS-G on this "
                                      "setup, every change costs a long frame",
                                      st_.worst);
                else if (st_.worst >= kSelfTestPassMs)
                    why = std::format("worst interval {:.0f} ms, expected under {:.0f}: slow but no re-creation signature",
                                      st_.worst, kSelfTestPassMs);
                else
                    why = std::format("only {} changes happened", st_.changes);
                SelfTestFinish(out, pass, why);
            }
            return current;
        }
        if (w_.open || now < st_.firstAt + uint64_t(st_.step) * kStepMs)
            return current;
        EmitRefreshField(out, true);
        const int want = kCycle[st_.step];
        ++st_.step;
        if (want == 4)
        {
            st_.at4x = true;
            st_.at4xSince = now + kStepMs / 2;
            st_.sumMs = 0.0;
            st_.n = 0;
        }
        if (want == current)
            return current; // already there (the slider was at 2x): no change, no watch
        ++st_.changes;
        return want;
    }

    DynamicMfg sel_;
    Watch w_;
    SelfTest st_;
    AutoMfgStatus status_;
    AutoMfgGate lastGate_ = AutoMfgGate::KeyOff;
    uint64_t lastTick_ = 0;
    bool restart_ = true;
    bool stopped_ = false;
    int costly_ = 0;
    int lastGood_ = 0;
};
} // namespace ext::fg
