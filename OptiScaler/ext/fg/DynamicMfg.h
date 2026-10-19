#pragma once
// Dynamic multi-frame generation selector for the DLSS-G output on RTX 40 (docs/FEATURES.md, "Dynamic MFG on RTX 40").
//
// Pure policy: no upstream types, no config, no clock. The caller feeds the real-frame interval (ms) and the time (ms)
// and gets back the factor to use (factor = generated frames + 1, 2..4). It is the sibling of XeFGDynamic.h with the same
// rules, closed against the cost of a step, plus the rules the XeFG selector lacks:
//   - spike clipping: a sample is clipped to 3x the running average, so one 190 ms stall is not a slow scene;
//   - hold: no switch for `holdMs` after a switch, an activation or a bad sample;
//   - fast down: when the presented rate is far above the target the down step needs 0.5 s instead of 2 s;
//   - flap guard: every switch beyond 4 in 60 s doubles the hold again (60 s at most);
//   - probe down: a refresh-bound output pins the presented rate at the refresh, so no rate rule ever steps down. If the
//     presented rate stays at the target for `probeMs` the factor goes one step down as a trial; it is kept when the new
//     presented rate still reaches 0.93x the target and reverted (with the interval doubled) otherwise;
//   - regret guard: an up step that gains less than 4 % presented rate is reverted and refused for 30 s; the gain is judged
//     on the first half second after the step, so a scene that gets heavier meanwhile does not revert a correct step;
// Timing includes frame limiting, like the XeFG selector: with a limiter the real interval is capped, see the tests.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ext::fg
{
class DynamicMfg
{
  public:
    // Rule constants (named so the tests and the docs quote the same numbers).
    static constexpr double kTauMs = 500.0;         // EWMA time constant of the real-frame interval
    static constexpr double kSpikeClip = 3.0;       // a sample is clipped to this many running averages
    static constexpr double kUpBelow = 0.93;        // presented < this x target: step up
    static constexpr double kDownAbove = 1.10;      // one step less would still present more than this x target: step down
    static constexpr double kWideAbove = 1.30;      // presented above this x target: fast down
    static constexpr double kFastKeep = 0.97;       // fast down only if one step less presents at least this x target
    static constexpr double kProbeAtLeast = 0.97;   // presented at or above this x target for probeMs: probe one step down
    static constexpr double kProbeKeep = 0.93;      // a probe is kept when the new presented rate reaches this x target
    static constexpr double kRegretGain = 1.04;     // an up step must gain at least this factor of presented rate
    static constexpr double kMaxSampleMs = 250.0;   // slower samples are pauses (loading), not frame times
    static constexpr uint64_t kUpDwellMs = 1000;
    static constexpr uint64_t kDownDwellMs = 2000;
    static constexpr uint64_t kFastDwellMs = 500;
    static constexpr uint64_t kDefaultHoldMs = 3000;
    static constexpr uint64_t kReducedTrustHoldMs = 12000; // used by the controller after a costly switch
    static constexpr uint64_t kMaxHoldMs = 60000;
    static constexpr int kFlapFree = 4;                    // switches per window that do not raise the hold
    static constexpr uint64_t kFlapWindowMs = 60000;
    static constexpr uint64_t kJudgeMs = 2000;             // a probe / up step is judged this long after the switch
    static constexpr uint64_t kRegretEarlyMs = 500;        // an up step is judged on the average this long after it
    static constexpr uint64_t kProbeBaseMs = 12000;
    static constexpr uint64_t kProbeMaxMs = 96000;
    static constexpr uint64_t kRefuseMs = 30000;

    enum class Why : uint8_t
    {
        None,
        Clamp,        // current factor outside 2..maximum, moved into the range
        Up,
        Down,
        FastDown,
        Probe,        // trial one step down
        ProbeRevert,  // the trial failed, back to the previous factor
        RegretRevert, // the up step gained too little, back to the previous factor
    };

    // What the last switch was, for the log line.
    struct Info
    {
        Why why = Why::None;
        int from = 0, to = 0;
        double avgMs = 0.0;           // running average of the real interval when the decision was taken
        double presented = 0.0;       // presented rate then (real fps x from)
        double presentedBefore = 0.0; // regret revert: the rate before the up step
        uint64_t probeMs = 0;         // probe interval in force
    };

    void SetHoldMs(uint64_t ms) { holdMs_ = std::clamp<uint64_t>(ms, 500, kMaxHoldMs); }
    uint64_t HoldMs() const { return holdMs_; }

    // Full reset (keeps only the hold setting).
    void Reset()
    {
        const uint64_t hold = holdMs_;
        *this = {};
        holdMs_ = hold;
    }

    // Starts a new observation window at `now` (activation, pause, bad sample): average, candidate, trial and timers go,
    // the learned state (flap history, probe interval, refused factor) stays.
    void Restart(uint64_t now)
    {
        avg_ = 0.0;
        cand_ = 0;
        pend_ = {};
        probeRunning_ = false;
        started_ = true;
        lastChange_ = now;
        settle_ = true; // the next interval still holds the transition (a long frame from the toggle path or a re-creation)
    }

    const Info& Last() const { return last_; }
    double AverageMs() const { return avg_; }
    uint64_t ProbeMs() const { return probeMs_; }
    bool TrialPending() const { return pend_.kind != Pend::None; }
    // Switches (probes excluded) inside the flap window ending at `now`.
    int RecentSwitches(uint64_t now) const
    {
        int n = 0;
        for (int i = 0; i < swCount_; ++i)
            if (Since(now, sw_[i]) < kFlapWindowMs)
                ++n;
        return n;
    }
    // The hold in force at `now` (base hold, doubled for every recent switch beyond kFlapFree, capped).
    uint64_t EffectiveHoldMs(uint64_t now) const
    {
        uint64_t hold = holdMs_;
        for (int level = std::max(0, RecentSwitches(now) - kFlapFree); level > 0 && hold < kMaxHoldMs; --level)
            hold *= 2;
        return std::max(holdMs_, std::min<uint64_t>(hold, kMaxHoldMs));
    }

    // `ms` real-frame interval, `target` fps, `current` factor in use, `maximum` highest allowed factor (2..4).
    // `allowProbe`: the probe-down rule may run (the caller turns it off once a switch proved costly).
    int Select(double ms, double target, uint64_t now, int current, int maximum, bool allowProbe)
    {
        maximum = std::clamp(maximum, 2, 4);
        last_ = {};
        if (current < 2 || current > maximum)
        {
            const int fixed = std::clamp(current, 2, maximum);
            Restart(now);
            if (fixed != current)
                last_ = { Why::Clamp, current, fixed, 0.0, 0.0, 0.0, probeMs_ };
            return fixed;
        }
        if (!std::isfinite(ms) || ms <= 0.0 || ms > kMaxSampleMs || !std::isfinite(target) || target <= 0.0)
        {
            // A probe whose verdict is lost here (Restart drops the trial) is not forgotten: the factor stays one step
            // down with nothing proving it harmless, so the back-off doubles as for a failed probe.
            if (pend_.kind == Pend::Probe)
                probeMs_ = std::min(probeMs_ * 2, kProbeMaxMs);
            Restart(now);
            return current;
        }

        if (settle_)
        {
            settle_ = false;
            return current;
        }
        double sample = ms;
        if (avg_ > 0.0)
            sample = std::min(sample, kSpikeClip * avg_);
        const double weight = 1.0 - std::exp(-sample / kTauMs);
        avg_ = avg_ > 0.0 ? avg_ + weight * (sample - avg_) : sample;
        if (!started_)
        {
            started_ = true;
            lastChange_ = now;
            return current;
        }

        const double real = 1000.0 / avg_;
        const double presented = real * current;

        // The average of the first half second after an up step (the first interval is dropped by `settle_`).
        if (pend_.kind == Pend::Regret && pend_.earlyMs == 0.0 && Since(now, pend_.at) >= kRegretEarlyMs)
            pend_.earlyMs = avg_;

        // Verdict on a probe or an up step taken kJudgeMs ago. Nothing else is decided while one is open.
        if (pend_.kind != Pend::None)
        {
            if (current != pend_.to) // somebody else changed the factor: the trial is void
                pend_ = {};
            else if (Since(now, pend_.at) < kJudgeMs)
                return current;
            else
            {
                const Pend p = pend_;
                pend_ = {};
                if (p.kind == Pend::Probe)
                {
                    if (presented >= kProbeKeep * target)
                        probeMs_ = kProbeBaseMs; // worked: the lower factor is the new baseline
                    else
                    {
                        probeMs_ = std::min(probeMs_ * 2, kProbeMaxMs);
                        return Revert(Why::ProbeRevert, p, presented, now, false);
                    }
                }
                else
                {
                    // The step is judged on its own cost: the average of the first kRegretEarlyMs after it. The 2 s average
                    // also holds whatever the scene did meanwhile, and a heavier scene would revert a correct step.
                    const double judged = p.earlyMs > 0.0 ? 1000.0 / p.earlyMs * current : presented;
                    if (judged < kRegretGain * p.presentedBefore)
                    {
                        refuseFactor_ = p.to;
                        refuseUntil_ = now + kRefuseMs;
                        return Revert(Why::RegretRevert, p, judged, now, true);
                    }
                }
            }
        }

        // The rate rules (XeFG's, plus the wide down step).
        int desired = current;
        bool quick = false; // down step with strong evidence: the output is far above the target
        const bool refused = current + 1 == refuseFactor_ && now < refuseUntil_;
        if (current < maximum && !refused && presented < target * kUpBelow)
            desired = current + 1;
        else if (current > 2)
        {
            const double lower = real * (current - 1); // one step less, same real rate (conservative)
            const bool strong = lower > target * kDownAbove;
            const bool wide = presented > target * kWideAbove;
            // `strong` always implies `wide` (presented >= 4/3 x lower), so it takes the fast dwell. The wide rule also
            // makes its own candidate: a limiter caps the real rate, one step less then presents about the target and
            // the plain rule never fires (3x at 112 fps real against a 224 fps target); that weaker evidence keeps the
            // full down dwell.
            if (strong || (wide && lower >= target * kFastKeep))
            {
                desired = current - 1;
                quick = strong && wide;
            }
        }

        // Probe timer: the presented rate sits at the target (a refresh-bound output never rises above it).
        if (allowProbe && current > 2 && presented >= target * kProbeAtLeast)
        {
            if (!probeRunning_)
            {
                probeRunning_ = true;
                probeSince_ = now;
            }
        }
        else
            probeRunning_ = false;

        const uint64_t hold = EffectiveHoldMs(now);
        if (desired == current)
        {
            cand_ = 0;
            if (probeRunning_ && Since(now, probeSince_) >= probeMs_ && Since(now, lastChange_) >= hold)
            {
                last_ = { Why::Probe, current, current - 1, avg_, presented, 0.0, probeMs_ };
                pend_ = { Pend::Probe, current, current - 1, now, presented };
                BeginSwitch(now);
                return current - 1;
            }
            return current;
        }
        if (cand_ != desired)
        {
            cand_ = desired;
            candSince_ = now;
            return current;
        }
        const uint64_t dwell = desired > current ? kUpDwellMs : (quick ? kFastDwellMs : kDownDwellMs);
        if (Since(now, lastChange_) < hold || Since(now, candSince_) < dwell)
            return current;

        last_ = { desired > current ? Why::Up : (quick ? Why::FastDown : Why::Down), current, desired, avg_, presented,
                  0.0, probeMs_ };
        if (desired > current)
            pend_ = { Pend::Regret, current, desired, now, presented };
        probeMs_ = kProbeBaseMs; // a rule switch ends the back-off
        RecordSwitch(now);
        BeginSwitch(now);
        return desired;
    }

  private:
    struct Pend
    {
        enum Kind : uint8_t
        {
            None,
            Probe,
            Regret
        } kind = None;
        int from = 0, to = 0;
        uint64_t at = 0;
        double presentedBefore = 0.0;
        double earlyMs = 0.0; // regret trial: average interval kRegretEarlyMs after the step (0 = not taken yet)
    };

    static uint64_t Since(uint64_t now, uint64_t then) { return now >= then ? now - then : 0; }

    void BeginSwitch(uint64_t now)
    {
        lastChange_ = now;
        cand_ = 0;
        avg_ = 0.0;
        probeRunning_ = false;
        settle_ = true; // the interval right after a switch can hold a re-creation stall: it must not seed the average
    }

    void RecordSwitch(uint64_t now)
    {
        if (swCount_ == kSwitchRing)
        {
            for (int i = 1; i < kSwitchRing; ++i)
                sw_[i - 1] = sw_[i];
            --swCount_;
        }
        sw_[swCount_++] = now;
    }

    int Revert(Why why, const Pend& p, double presented, uint64_t now, bool countsAsSwitch)
    {
        last_ = { why, p.to, p.from, avg_, presented, p.presentedBefore, probeMs_ };
        if (countsAsSwitch)
            RecordSwitch(now);
        BeginSwitch(now);
        return p.from;
    }

    static constexpr int kSwitchRing = 16;

    uint64_t holdMs_ = kDefaultHoldMs;
    double avg_ = 0.0;
    bool started_ = false;
    bool settle_ = false;
    uint64_t lastChange_ = 0;
    int cand_ = 0;
    uint64_t candSince_ = 0;
    uint64_t sw_[kSwitchRing] = {};
    int swCount_ = 0;
    bool probeRunning_ = false;
    uint64_t probeSince_ = 0;
    uint64_t probeMs_ = kProbeBaseMs;
    Pend pend_ {};
    int refuseFactor_ = 0;
    uint64_t refuseUntil_ = 0;
    Info last_ {};
};
} // namespace ext::fg
