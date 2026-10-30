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
//             re-creation. The hold becomes 12 s and probing stops; a second one in a row, or a third in all, stops automatic
//             mode for the session. An earlier one is forgotten only after ten clean switches in a row: counted in switches,
//             not in time, and the mode switches rarely after the first one, so it can stay for hours;
//   broken  - Streamline status != 0, applied count not reached after the second, or presented stuck at 1: one recovery
//             through the existing toggle path to the last good count, then automatic mode stops for the session. A bad
//             status or count that began in the last kBadRunMs of the second is not dropped: the watch stays open (at most
//             kBadRunMs more) until it is convicted or gone.
//
// Stall rule: while the controller's own average of the real interval is above kStallMs (below about 33 real fps) the game is
// stalled (a loading screen, a re-creation by the game), and a stall is not a failed change. No new change starts and the
// self-test does not start; an open watch is not judged and starts over on every stalled tick, so the second that is judged
// is a calm one (kStallCalmMs); a watch that is not judged within kStallGiveUpMs of its switch ends as unknown (no strike, no
// clean switch, the count stays). The self-test waits for kSelfTestCalmMs without a stall and a stall during it ends it as
// interrupted. At normal frame rates nothing of this applies.

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
    int detectedGenerated = 0; // State::dlssgDetectedInterpolationCount (generated frames the plugin evaluates with); 0 = unknown
    // IFGFeature::IsPaused(): the count field reads 0 while frame generation is paused. NOT produced in the DLL today:
    // DLSSG_Dx12::Dispatch returns before SetOptions while the feature is paused or inactive, so no tick runs then. The field
    // stays (harmless, and for a caller that does tick while paused); the protection that really exists against a short
    // pause is the rule that a 0 count has to persist (kWrongSamples samples over kBadRunMs), with the gap rule (kGapMs).
    bool paused = false;
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
    static constexpr int kWrongSamples = 3;         // applied count wrong on this many samples in a row at the end of the watch
    static constexpr int kBadStatusSamples = 3;     // a non-zero Streamline status that persists on this many samples
    // Both runs also have to last this long, from their first sample to the verdict: the samples are game frames, and three
    // of them are 13 ms at 224 fps. A failed change stays failed for the rest of the watch; a blink does not.
    static constexpr uint64_t kBadRunMs = 150;
    static constexpr int kSignatureSamples = DynamicMfg::kSettleSamples; // the re-creation stall is one of the first intervals
                                                    // after the switch: the same ones the selector drops from its average
    static constexpr int kStrikesToStop = 2;        // signatures on this many switches in a row stop automatic mode
    static constexpr int kStrikesInTotalToStop = 3; // so do this many signatures unless ten clean switches in a row followed one of them (a
                                                    // plugin that re-creates DLS-G only on up steps breaks the row every time)
    static constexpr int kForgetAfterClean = 10;    // clean switches after which earlier strikes are forgotten
    // A game stall (a loading screen, a feature re-creation by the game) is not a failed count change. The controller keeps
    // its own average of the real (non-generated) frame interval; above kStallMs (below about 33 real fps) it is stalled.
    static constexpr double kStallMs = 30.0;
    static constexpr uint64_t kStallCalmMs = 1000;    // a watch that was held back by a stall runs again once the average has been at
                                                      // or below kStallMs this long without a break, then it is judged as any other
    static constexpr uint64_t kStallGiveUpMs = 5000;  // a change that could not be judged within this long of its switch ends as unknown
    static constexpr uint64_t kSelfTestCalmMs = 20000; // the self-test starts only after the game has been calm this long
    static constexpr double kStallTauMs = 250.0;      // time constant of that average
    static constexpr double kStallClipMs = 50.0;      // one sample counts at most this much: one long frame (the signature of a
                                                      // re-creation of DLS-G) does not lift an average of up to about 25 ms over
                                                      // kStallMs, a few slow ones in a row do

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
        const bool gap = (lastTick_ != 0 && now >= lastTick_ && now - lastTick_ > kGapMs) || in.paused;
        lastTick_ = now;
        if (restart_ || gap)
        {
            // Activation or a pause: whatever the open watch saw was the toggle path or the pause, not the selector.
            w_ = {};
            sel_.Restart(now);
            lastGood_ = 0;
            realAvg_ = 0.0;
            calm_ = false;
            if (st_.running)
                SelfTestInterrupted(out, now, restart_ ? "frame generation was reactivated" : "the game paused");
            restart_ = false;
        }
        if (externalStall_ || externalStallWatch_)
        {
            // An NR model rebuild owns this hitch, not the game and not the last switch: the first tick restarts the
            // selector (its settle drops exactly the next two intervals) and an open watch starts over on both ticks,
            // so the hitch is never judged as the switch re-creation stall. Neither tick feeds the stall average.
            if (externalStall_)
            {
                externalStall_ = false;
                externalStallWatch_ = true;
                sel_.Restart(now);
            }
            else
                externalStallWatch_ = false;
            if (w_.open)
                w_.StartOver(now);
        }
        else
            UpdateStall(in); // skipped on both ignored ticks above, as before the rewrite
        int current = std::max(1, in.appliedFactor);
        if (lastGood_ == 0)
            lastGood_ = current;
        status_.factor = current;
        out.controls = true;

        if (st_.running && stalled_)
        {
            // A stall is not a failed test. With changes made the test is over, as for a pause (the count in use before it
            // comes back once the game is calm again); with none made it goes back to waiting for a quiet game.
            if (st_.changes > 0)
                SelfTestFinish(out, false, "interrupted: the game stalled", true);
            else
                st_ = {};
        }
        if (in.selfTest && !st_.done && !st_.running)
            SelfTestBegin(in, out, cycleMax, hardMax);

        // Watch of the last switch.
        if (w_.open && stalled_)
        {
            // The game is stalled: a change is not judged on a stalled game (late counts, a long frame and a bad status are
            // what a stall looks like). The watch starts over on every tick of it, so that the second that is judged later
            // is a calm one.
            w_.StartOver(now);
        }
        else if (w_.open)
        {
            ++w_.ticks;
            if (std::isfinite(in.realMs))
            {
                w_.worst = std::max(w_.worst, in.realMs);
                if (w_.ticks <= kSignatureSamples)
                    w_.early = std::max(w_.early, in.realMs);
            }
            if (in.stateValid)
            {
                ++w_.samples;
                if (in.slStatus != 0 && w_.baseStatus == 0) // a status that was already set before the switch is not its fault
                {
                    if (w_.badRun++ == 0)
                        w_.badSince = now;
                    w_.statusCode = in.slStatus;
                }
                else
                    w_.badRun = 0;
                if (in.presented >= 2)
                    w_.seenMulti = true;
            }
            // 0 means "unknown" (the field is zeroed while frame generation pauses), but it is not the requested count either
            if (in.detectedGenerated == w_.to - 1)
                w_.wrongRun = 0;
            else if (w_.wrongRun++ == 0)
                w_.wrongSince = now;
            if (in.detectedGenerated > 0)
                w_.lastDetected = in.detectedGenerated;
            if (now - w_.since >= (w_.stalled ? kStallCalmMs : kWatchMs))
            {
                // A fault that is in progress but too young to convict (it began in the last kBadRunMs) keeps the watch open
                // for at most kBadRunMs more: closing it here would let the next switch take the already faulty status as
                // its baseline, and nothing would ever look at that fault again.
                const bool young = FaultTooYoung(w_, in);
                if (young && w_.graceEnd == 0)
                    w_.graceEnd = now + kBadRunMs;
                if (!young || now >= w_.graceEnd)
                    Verdict(in, out);
            }
        }
        if (w_.open && w_.stalled && now - w_.switchAt >= kStallGiveUpMs)
        {
            // Not judged within kStallGiveUpMs of the switch because of stalls: unknown. No strike (nor a clean switch), the
            // count stays as it is, lastGood_ is not moved (the change was not proven good), and the selector starts over.
            Info(out, std::format("AutoMFG: judging the change at {}x skipped, the game was stalled", w_.to));
            w_ = {};
            sel_.Restart(now);
        }
        if (stopped_) // the verdict just stopped it
        {
            status_.stopped = true;
            return out;
        }

        int next = current;
        bool fromSelector = false;
        bool restoring = false;
        if (restore_ != 0)
        {
            // the self-test is over: back to the count in force before it, as a watched change like any other (not in a stall)
            if (!w_.open && !stalled_)
            {
                if (restore_ != current)
                {
                    next = restore_;
                    restoring = true;
                }
                restore_ = 0;
            }
        }
        else if (st_.running)
            next = SelfTestStep(in, out, now, current);
        else if (stalled_)
        {
            // Nothing new starts while the game stalls. The selector is not asked (it would record a switch that is dropped here)
            // and starts over, so that it holds for its usual time once the game is calm again.
            sel_.Restart(now);
        }
        else
        {
            fromSelector = true;
            // fed while a switch is watched too (the hold keeps it from switching inside the watch)
            const int pick = sel_.Select(in.realMs, target, now, current, hardMax, costly_ == 0);
            if (!w_.open)
                next = pick;
        }
        status_.selfTestRunning = st_.running;

        if (next != current)
        {
            if (fromSelector)
                Line(out, false, FormatSwitch(sel_.Last(), target));
            else if (restoring)
                Line(out, false, std::format("AutoMFG self-test: {}x -> {}x (back to the count in use before the test)",
                                             current, next));
            else
                Line(out, false, std::format("AutoMFG self-test: {}x -> {}x", current, next));
            w_ = {};
            w_.open = true;
            w_.since = now;
            w_.switchAt = now;
            w_.from = current;
            w_.to = next;
            w_.baselineMs = fromSelector ? sel_.Last().avgMs : st_.baselineMs;
            w_.baseStatus = in.stateValid ? in.slStatus : 0;
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
    // Expected stall from outside (an NR model rebuild hitches its frame): the next two frame intervals are
    // ignored. The next open tick restarts the selector (its settle drops exactly those two) and an open watch starts
    // over on both (the hitch is not the switch's); the stall average keeps its clipped value. Pure; the NR pass calls
    // it through NoteModelRebuild. Cleared while the gate is closed, so a rebuild with AutoMFG off cannot swallow the
    // first intervals after it is switched on.
    void NoteExternalStall() { externalStall_ = true; }

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
        double worst = 0.0; // longest interval of the whole watch
        double early = 0.0; // longest of the first kSignatureSamples intervals after the switch
        int ticks = 0;
        double baselineMs = 0.0;
        uint32_t baseStatus = 0; // Streamline status when the switch was made
        int badRun = 0;          // consecutive samples with a status that was 0 before the switch
        uint64_t badSince = 0;   // time of the first sample of that run
        uint32_t statusCode = 0;
        int wrongRun = 0;        // consecutive samples whose applied count is not the requested one (0 included)
        uint64_t wrongSince = 0; // time of the first sample of that run
        int lastDetected = 0;    // last non-zero applied count seen (generated frames)
        bool seenMulti = false;
        int samples = 0;
        uint64_t graceEnd = 0;   // 0, or the time at which a still young fault stops holding the watch open
        uint64_t switchAt = 0;   // time of the switch itself (since moves when a stall starts the watch over)
        bool stalled = false;    // a stall held the watch back at least once

        // A stall: forget what was seen so far, keep what identifies the change (from, to, baselineMs, baseStatus, switchAt).
        void StartOver(uint64_t now)
        {
            since = now;
            worst = early = 0.0;
            ticks = 0;
            badRun = 0;
            badSince = 0;
            statusCode = 0;
            wrongRun = 0;
            wrongSince = 0;
            lastDetected = 0;
            seenMulti = false;
            samples = 0;
            graceEnd = 0;
            stalled = true;
        }
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
        int restoreFactor = 0; // the count in force when the test began
        int costly = 0;
        // the 4x step: real interval over its second half
        bool at4x = false;
        uint64_t at4xSince = 0;
        double sumMs = 0.0;
        int n = 0;
        std::string failure;
    };

    static constexpr int kCycle[kSelfTestSteps] = { 2, 3, 4, 3, 2 };

    // The re-creation signature on two switches in a row, or on three in all. costly_ is forgotten only after
    // kForgetAfterClean clean switches in a row (a strike restarts that count): it is counted in switches, not in time, and
    // after the first strike the selector is in reduced trust (long hold, no probes) and switches rarely, so a strike can
    // stay for hours and later ones add up. Pinned by NineCleanSwitchesDoNotForgetAStrikeButTenDo.
    bool TooManySignatures() const { return streak_ >= kStrikesToStop || costly_ >= kStrikesInTotalToStop; }

    void Line(AutoMfgOut& out, bool warn, std::string text) { out.lines.push_back({ warn, std::move(text) }); }
    void Info(AutoMfgOut& out, std::string text) { Line(out, false, std::move(text)); }
    void Warn(AutoMfgOut& out, std::string text) { Line(out, true, std::move(text)); }

    void GateClosed(AutoMfgGate gate, const AutoMfgTick& in, AutoMfgOut& out)
    {
        // A rebuild while the gate is closed must not eat the first open intervals: the next tick after
        // opening already restarts on the activation path, so a pending external stall is dropped here.
        externalStall_ = externalStallWatch_ = false;
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
                SelfTestFinish(out, false, std::format("interrupted: {}", GateText(gate)), true);
        }
        if (in.selfTest && !st_.done)
            SelfTestFinish(out, false, std::format("not run: {}", GateText(gate)));
        // the self-test left the count at a cycle step: return it to the value in force before the test
        if (restore_ != 0 && in.appliedFactor != restore_)
        {
            out.setFactor = true;
            out.factor = restore_;
            Info(out, std::format("AutoMFG self-test: {}x -> {}x (back to the count in use before the test)",
                                  in.appliedFactor, restore_));
        }
        restore_ = 0;
    }

    // The controller's own average of the real frame interval (an exponential average over time, every sample clipped to
    // kStallClipMs), and from it the stall state and the time the game has been calm for.
    void UpdateStall(const AutoMfgTick& in)
    {
        if (std::isfinite(in.realMs) && in.realMs > 0.0)
        {
            const double s = std::min(in.realMs, kStallClipMs);
            realAvg_ = realAvg_ > 0.0 ? realAvg_ + (1.0 - std::exp(-s / kStallTauMs)) * (s - realAvg_) : s;
        }
        stalled_ = realAvg_ > kStallMs;
        if (stalled_ || realAvg_ <= 0.0)
            calm_ = false;
        else if (!calm_)
        {
            calm_ = true;
            calmSince_ = in.nowMs;
        }
    }
    bool CalmFor(uint64_t now, uint64_t ms) const { return calm_ && now - calmSince_ >= ms; }

    // a count that is not 0 and not the requested one is a real mismatch; 0 ("unknown") has to persist
    static bool CountWrong(const Watch& w, const AutoMfgTick& in)
    {
        return (in.detectedGenerated > 0 && in.detectedGenerated != w.to - 1) ||
               (w.wrongRun >= kWrongSamples && in.nowMs - w.wrongSince >= kBadRunMs);
    }
    static bool StatusBad(const Watch& w, const AutoMfgTick& in)
    {
        return w.badRun >= kBadStatusSamples && in.nowMs - w.badSince >= kBadRunMs;
    }
    // A bad run (status or applied count) is going on and is not yet old enough to be a failure.
    static bool FaultTooYoung(const Watch& w, const AutoMfgTick& in)
    {
        return (w.badRun > 0 || w.wrongRun > 0) && !StatusBad(w, in) && !CountWrong(w, in);
    }

    void Verdict(const AutoMfgTick& in, AutoMfgOut& out)
    {
        const Watch w = w_;
        w_ = {};
        const int requested = w.to - 1;
        const bool countWrong = CountWrong(w, in);
        const bool statusBad = StatusBad(w, in);
        const bool stuck = w.samples >= 5 && !w.seenMulti;
        const double costlyAt = std::max(kCostlyMs, 3.0 * w.baselineMs);
        if (statusBad || countWrong || stuck)
        {
            std::string why;
            if (statusBad)
                why = std::format("Streamline status {:#x}", w.statusCode);
            else if (countWrong)
                why = std::format("applied count {} != requested {} (generated frames)", in.detectedGenerated, requested);
            else
                why = "presented frames stuck at 1";
            // during the self-test the last good count is a cycle step: go back to the one in force before the test
            const int back = st_.running && st_.restoreFactor > 0 ? st_.restoreFactor : lastGood_;
            Warn(out, std::format("AutoMFG: {}, switch {}x -> {}x; going back to {}x, automatic mode stopped", why,
                                  w.from, w.to, back));
            out.recover = true;
            out.factor = back;
            stopped_ = true;
            if (st_.running)
                SelfTestFinish(out, false, std::format("{}", why));
            return;
        }
        if (w.early >= costlyAt)
        {
            // The signature is a long interval right after the switch; a long frame later in the watch is the game's.
            ++costly_;
            ++streak_;
            cleanRun_ = 0;
            sel_.SetHoldMs(DynamicMfg::kReducedTrustHoldMs);
            Warn(out, std::format("AutoMFG: switch {}x -> {}x cost a {:.0f} ms frame (a re-creation of DLS-G looks like "
                                  "this), holding {} s between changes and no more probes",
                                  w.from, w.to, w.early, DynamicMfg::kReducedTrustHoldMs / 1000));
            if (st_.running)
            {
                ++st_.costly;
                st_.worst = std::max(st_.worst, w.worst);
            }
            else if (TooManySignatures())
            {
                stopped_ = true;
                Warn(out, "AutoMFG: changing the count recreates DLSS-G on this setup; automatic MFG stopped");
            }
            lastGood_ = w.to;
            return;
        }
        streak_ = 0; // a clean switch breaks the row
        if (++cleanRun_ >= kForgetAfterClean && costly_ > 0)
        {
            costly_ = 0;
            sel_.SetHoldMs(DynamicMfg::kDefaultHoldMs);
            Info(out, std::format("AutoMFG: {} clean changes since the last long frame, normal hold and probes again",
                                  kForgetAfterClean));
        }
        Info(out, std::format("AutoMFG: {}x settled (worst interval {:.0f} ms, applied {})", w.to, w.worst,
                              w.lastDetected > 0 ? std::format("{}x", w.lastDetected + 1) : std::string("unknown")));
        if (st_.running)
            st_.worst = std::max(st_.worst, w.worst);
        lastGood_ = w.to;
    }

    // --- self-test ---------------------------------------------------------------------------------------------
    void SelfTestBegin(const AutoMfgTick& in, AutoMfgOut& out, int cycleMax, int hardMax)
    {
        if (cycleMax < 4)
        {
            SelfTestFinish(out, false, std::format("not run: the DLSS-G maximum is {}x and the cycle needs 4x, enable "
                                                   "AdaMfgUnlock and restart",
                                                   cycleMax));
            return;
        }
        if (!CalmFor(in.nowMs, kSelfTestCalmMs))
            return; // a loading screen or a hitch while the count changes would be taken for the test's own failure
        st_ = {};
        st_.running = true;
        st_.restoreFactor = std::clamp(in.appliedFactor, 2, std::max(2, hardMax)); // what the selector left in force
        st_.firstAt = in.nowMs + kWarmupMs;
        st_.baselineMs = sel_.AverageMs();
        Info(out, std::format("AutoMFG self-test: started, 2x -> 3x -> 4x -> 3x -> 2x, one change every {} s",
                              kStepMs / 1000));
    }

    void SelfTestInterrupted(AutoMfgOut& out, uint64_t now, const char* why)
    {
        if (st_.changes > 0)
            SelfTestFinish(out, false, std::format("interrupted: {}", why), false); // the user or a pause owns the count now
        else
            st_.firstAt = now + kWarmupMs; // nothing changed yet: start the timeline over
    }

    void SelfTestFinish(AutoMfgOut& out, bool pass, std::string detail, bool restore = false)
    {
        if (st_.done)
            return;
        EmitRefreshField(out, true);
        st_.running = false;
        st_.done = true;
        sel_.Restart(lastTick_);
        if (restore && st_.restoreFactor != 0)
            restore_ = st_.restoreFactor; // applied on the next tick (open: a watched change; closed: at once)
        if (pass)
            Info(out, std::format("AutoMFG self-test: PASS ({} in-place changes, worst interval {:.0f} ms, all applied "
                                  "counts matched, no Streamline errors)",
                                  st_.changes, st_.worst));
        else
            Warn(out, std::format("AutoMFG self-test: FAIL ({})", detail));
        if (TooManySignatures() && !stopped_)
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
                SelfTestFinish(out, pass, why, true);
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
    int costly_ = 0;    // strikes, forgotten after kForgetAfterClean clean switches; > 0 = reduced trust (long hold, no probes)
    int streak_ = 0;    // signatures on consecutive switches
    int cleanRun_ = 0;  // clean switches since the last signature
    int lastGood_ = 0;
    int restore_ = 0;   // self-test over: the count to return to (0 = none)
    bool externalStall_ = false; // an NR model rebuild owns the next tick: restart the selector, restart the watch
    bool externalStallWatch_ = false; // ...and restart the watch once more on the tick after
    double realAvg_ = 0.0;     // the controller's own average of the real frame interval, ms (0 = none yet)
    bool stalled_ = false;     // realAvg_ is above kStallMs
    bool calm_ = false;        // realAvg_ is known and has not been above kStallMs since calmSince_
    uint64_t calmSince_ = 0;
};
} // namespace ext::fg
