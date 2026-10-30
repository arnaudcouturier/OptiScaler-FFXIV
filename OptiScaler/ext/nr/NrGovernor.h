#pragma once

// NR frame-rate governor over model-resolution tiers. Pure: the pass feeds one Input per evaluation and applies the
// returned cap; nothing here reads config, the clock or the GPU, so simulated-time tests drive Tick directly.
//
// The governor never writes a config key. It outputs a RUN-TIME cap (0 = no cap, exactly the user's WorkingScale;
// otherwise the tier fraction) that the pass applies to the scales from EffectiveScales in one place. The user value
// is the ceiling: the governor only ever steps below it.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace DlssNr::Governor
{
// Model-resolution tiers the governor steps through, high to low, as fractions of the frame. The top state is no cap
// at all (exactly the user's WorkingScale, including one above 100%); the first step down goes to the highest tier
// strictly below the user value (a user at 150% steps to 100%, a user at 85% to 80%).
inline constexpr float kTiers[] = { 1.0f, 0.90f, 0.80f, 0.70f, 0.60f, 0.50f };
inline constexpr int kTierCount = 6;
// At least 3 s below the band before stepping down; at least 10 s above it before probing one tier up. The up dwell
// doubles on every failed probe, up to the cap, and returns to the base on a ceiling change.
inline constexpr uint64_t kStepDownDwellMs = 3000;
inline constexpr uint64_t kUpDwellBaseMs = 10000;
inline constexpr uint64_t kUpDwellMaxMs = 96000;
// Step down only below 95% of the target: an auto target (the lower of the frame limit and the refresh) pins the
// displayed rate at the target, and 99% of it is holding, not falling behind. The number sits between AutoMFG's 0.93
// shortfall rule (step the multiplier up below it) and its 0.97 at-target rule (a probe only holds at or above it).
inline constexpr double kDownBelow = 0.95;
// The up dwell accrues at or above the probe-keep line, not above a band: an auto target IS the
// displayed-rate cap, so a held rate sits at the target and can never clear 105% of it. The probe
// verdict below (keep at 0.97x, else revert with the dwell doubled) is what stops a climb the tier
// cannot hold.
// A probe up is kept only when the rate holds at or above 97% of the target over the judge window after the new size
// renders: the same 0.97 AutoMFG asks of a probe down or a fast down before it trusts doing with less.
inline constexpr double kProbeKeep = 0.97;
// The verdict window after the probed size is applied (AutoMFG judges a switch over the same 2 s).
inline constexpr uint64_t kProbeJudgeMs = 2000;
// After the governor's own step, a ceiling change, or any applied size change the pass reports, samples are dropped
// for 1 s: the 500 ms settle plus the rebuild hitch (about 0.2 s) and margin. Counted in ms, not evaluations: pre-SR
// plus post-SR ticks twice per frame.
inline constexpr uint64_t kDiscardMs = 1000;
// The average is never seeded from one raw sample (the first sample after a step can be the rebuild hitch itself):
// the next 500 ms of samples seed it through the bilateral clip below, and no step is decided meanwhile.
inline constexpr uint64_t kSeedMs = 500;
// The displayed-rate signal is an exponential moving average with about a second of memory.
inline constexpr double kSmoothTauMs = 1000.0;
// One sample counts at most 3x the running average and at least a third of it: a rebuild hitch is not a slower scene,
// and a lone fast frame after one is not a faster scene either.
inline constexpr double kSpikeClip = 3.0;
// Slower samples are pauses (loading screens), not frame times, and never move the governor.
inline constexpr double kMaxSampleMs = 250.0;
// No timestamp is ever 0-as-unset: the simulated clock starts at 0, so every "since" uses this instead.
inline constexpr uint64_t kUnset = UINT64_MAX;
// While a feature owns presents, the present counter not advancing across this many ticks is a stale signal.
// The pass ticks up to twice per frame (pre-SR plus post-SR), so four ticks without one present advancing is
// two missed frames: genuinely stale, while one hitch never trips it.
inline constexpr unsigned kFgStaleTicks = 4;
// Longer than this between two pass ticks is a pause (focus loss, bridge suspended, loading): the AutoMFG
// kGapMs rule. No step is ever decided on timers that sat through one.
inline constexpr uint64_t kPassGapMs = 1000;

// Why the governor holds its tier or stands down; the status line quotes the text.
enum class Reason : uint8_t
{
    Off,            // the Governor key is off: the user value runs untouched
    Full,           // at full resolution, uncapped (the user value, or the top tier while it holds)
    Holding,        // holding the tier inside the band
    BelowTarget,    // below the target but the down dwell has not run out yet
    AboveTarget,    // at or above the probe-keep line but the up dwell has not run out yet
    AtFloor,        // at the 50% floor and still below the target: nothing further to give
    BelowRange,     // the user ceiling is below the 50% floor: uncapped, the governor cannot step
    TexturesPending, // the previous step's retired textures are still waiting for the GPU: holding
    AutoMfg,        // AutoMFG is on: two controllers on one frame rate would fight
    PerAxis,        // a per-axis key is set: the governor only steps the uniform scale
    Spatial,        // spatial compression is on: the governor only steps the ordinary path
    NoTarget,       // no target: neither GovernorTargetFps nor a limit/refresh to take it from
    NoSignal,       // no (or stale) frame-time signal on this path
    WaitingForCount, // a feature is actively generating but its count is not observed yet (selected but idle
                     // reads as off: with nothing generated the displayed rate is the present rate)
    UnsupportedFg,  // a non-DLSS-G frame-generation output is active: no count reader here
};

inline const char* ReasonText(Reason reason)
{
    switch (reason)
    {
    case Reason::Off: return "off";
    case Reason::Full: return "full resolution";
    case Reason::Holding: return "holding";
    case Reason::BelowTarget: return "below target";
    case Reason::AboveTarget: return "above target";
    case Reason::AtFloor: return "at the 50% floor";
    case Reason::BelowRange: return "below the 50% floor";
    case Reason::TexturesPending: return "waiting for the previous size to release";
    case Reason::AutoMfg: return "AutoMFG is on";
    case Reason::PerAxis: return "a per-axis key is set";
    case Reason::Spatial: return "spatial compression is on";
    case Reason::NoTarget: return "no target";
    case Reason::NoSignal: return "no frame-time signal";
    case Reason::WaitingForCount: return "waiting for the frame-generation count";
    case Reason::UnsupportedFg: return "frame generation is not DLSS-G";
    }
    return "unknown";
}

struct Input
{
    uint64_t nowMs = 0;          // simulated or real clock, ms
    double intervalMs = 0.0;     // present interval (State::lastFGFrameTime), ms; <= 0 or not finite = no signal
    uint64_t fgFrame = 0;        // State::fgLastFrame: advances with presents; read only while fgFeature is set
    bool fgFeature = false;      // an FG feature owns presents (State::currentFG != nullptr)
    double multiplier = 0.0;     // displayed frames per present (DLSS-G count + 1); 0 = not observed yet
    bool fgActive = false;       // a feature is actively generating (IsActive and not paused), else the multiplier is 1
    bool unsupportedFg = false;  // the active output is not DLSS-G (no count reader here)
    double targetFps = 0.0;      // ResolveTarget of GovernorTargetFps, the frame limit and the refresh; 0 = unknown
    float userCeiling = 1.0f;    // the user's WorkingScale (sanitized): the governor never steps above it
    bool governorOn = false;     // [DlssNr] Governor
    bool autoMfgOn = false;      // [DLSSG] AutoMFG: on stands the governor down, even with its gate closed
    bool perAxisSet = false;     // WorkingScaleX or WorkingScaleY follows its own value
    bool spatialOn = false;      // spatial compression is on
    bool texturesReleased = true; // the retirements queued by the last size change have run
    uint64_t appliedSeq = 0;     // the pass's parked-retirements mark: a change means a size change was parked
    unsigned appliedWorkW = 0;   // the running model width: a change means a new size renders
    unsigned appliedWorkH = 0;   // the running model height
};

struct Output
{
    float cap = 0.0f;          // the run-time cap: 0 = no cap (exactly the user value), else the tier fraction
    int tierPct = 0;           // the tier as a whole percent (100/90/80/70/60/50), 0 when uncapped
    double measuredFps = 0.0;  // the smoothed displayed rate, 0 while nothing valid has been seen
    double targetFps = 0.0;    // the target this tick decided against
    uint64_t upDwellMs = kUpDwellBaseMs; // the up dwell in force (grows on failed probes, resets on ceiling change)
    Reason reason = Reason::Off;
};

// One status line for the NR card: the tier, the target, the measured rate, and why it stands down if it does.
inline std::string StatusLine(const Output& out)
{
    if (out.reason == Reason::Off)
        return "Governor: off";
    const char* why = nullptr;
    switch (out.reason)
    {
    case Reason::AutoMfg: why = "stands down: AutoMFG is on"; break;
    case Reason::PerAxis: why = "stands down: a per-axis key is set"; break;
    case Reason::Spatial: why = "stands down: spatial compression is on"; break;
    case Reason::NoTarget: why = "stands down: no target (set GovernorTargetFps or a frame limit)"; break;
    case Reason::NoSignal: why = "stands down: no frame-time signal on this path"; break;
    case Reason::WaitingForCount: why = "stands down: frame generation is running but its count is not observed yet"; break;
    case Reason::UnsupportedFg: why = "stands down: frame generation is not DLSS-G"; break;
    case Reason::BelowRange: why = "the user resolution is below the 50% floor"; break;
    case Reason::TexturesPending: why = "waiting for the previous size to release"; break;
    case Reason::AtFloor: why = "at the 50% floor"; break;
    default: break; // Full, Holding, BelowTarget, AboveTarget: the numbers speak
    }
    char text[192] = {};
    if (out.tierPct > 0)
        std::snprintf(text, sizeof text, "Governor: %d%% (target %.0f fps, measured %.0f fps)%s%s", out.tierPct,
                      out.targetFps, out.measuredFps, why != nullptr ? "; " : "", why != nullptr ? why : "");
    else if (out.measuredFps > 0.0)
        std::snprintf(text, sizeof text, "Governor: full resolution (target %.0f fps, measured %.0f fps)%s%s",
                      out.targetFps, out.measuredFps, why != nullptr ? "; " : "", why != nullptr ? why : "");
    else
        std::snprintf(text, sizeof text, "Governor: full resolution (target %.0f fps)%s%s", out.targetFps,
                      why != nullptr ? "; " : "", why != nullptr ? why : "");
    return text;
}

class Governor
{
  public:
    Output Tick(const Input& in)
    {
        Output out;
        out.targetFps = in.targetFps;
        out.upDwellMs = upDwellMs_;
        if (!in.governorOn)
        {
            Reset();
            out.reason = Reason::Off;
            out.upDwellMs = upDwellMs_;
            return out;
        }
        const float ceiling =
            std::isfinite(in.userCeiling) && in.userCeiling > 0.0f ? in.userCeiling : 1.0f;
        if (!haveCeiling_ || ceiling != ceiling_)
            AdoptCeiling(ceiling, in.nowMs);
        // A real stand-down resets to the top state: another controller owns the frame rate, or the governor must
        // not touch the scale at all. Every other stand-down below only freezes the tier and pauses the timers.
        Reason down = Reason::Full;
        if (in.autoMfgOn)
            down = Reason::AutoMfg;
        else if (in.perAxisSet)
            down = Reason::PerAxis;
        else if (in.spatialOn)
            down = Reason::Spatial;
        if (down != Reason::Full)
        {
            StandDown(down, in.nowMs);
            out.reason = reason_;
            return out;
        }
        if (in.unsupportedFg)
        {
            Freeze(in.nowMs, Reason::UnsupportedFg);
            return Finish(out, in.nowMs);
        }
        if (in.fgActive && !(in.multiplier > 0.0))
        {
            Freeze(in.nowMs, Reason::WaitingForCount);
            return Finish(out, in.nowMs);
        }
        if (!(in.targetFps > 0.0))
        {
            Freeze(in.nowMs, Reason::NoTarget);
            return Finish(out, in.nowMs);
        }
        // The pass did not tick for over a second (focus pause, bridge suspended, loading): no step from
        // stale timers. Every timer shifts past the gap like a transient freeze, then the transition window
        // drops the pre-pause average so the rate reseeds from what renders after it.
        if (lastTick_ != kUnset && in.nowMs > lastTick_ && in.nowMs - lastTick_ > kPassGapMs)
        {
            ShiftTimers(in.nowMs - lastTick_);
            StartDiscard(in.nowMs);
            reason_ = Reason::NoSignal;
            return Finish(out, in.nowMs);
        }
        // Signal freshness. The frame-time signal freezes on exactly one live path: the D3D11 wrap path stops
        // writing lastFGFrameTime while an FG feature owns presents, while presents keep flowing. A timestamp
        // witness cannot tell that from the game path (D3D11 through the bridge, DLSS-G): there the output
        // presents tick several times per game frame, so any timestamp delta disagrees with the game-frame
        // signal and a timestamp rule would read stale every tick and pin the governor. The counter instead:
        // while a feature owns presents, a present counter that did not advance across ticks means the signal
        // went stale. With no feature there is no check: an invalid interval is already no signal.
        if (in.fgFeature)
        {
            if (!haveFgFrame_)
            {
                haveFgFrame_ = true;
                fgFrameSeen_ = in.fgFrame;
                fgStaleTicks_ = 0;
            }
            else if (in.fgFrame != fgFrameSeen_)
            {
                fgFrameSeen_ = in.fgFrame;
                fgStaleTicks_ = 0;
            }
            else if (++fgStaleTicks_ >= kFgStaleTicks)
            {
                // A loading hitch is not a slower scene: hold the tier, pause the timers, say so.
                Freeze(in.nowMs, Reason::NoSignal);
                return Finish(out, in.nowMs);
            }
        }
        else
        {
            haveFgFrame_ = false;
            fgStaleTicks_ = 0;
        }
        const double interval = in.intervalMs;
        const bool inRange = std::isfinite(interval) && interval > 0.0 && interval <= kMaxSampleMs;
        if (!inRange)
        {
            // A loading hitch is not a slower scene: hold the tier, pause the timers, say so.
            Freeze(in.nowMs, Reason::NoSignal);
            return Finish(out, in.nowMs);
        }
        // The pass reports when a size change is actually applied: the parked-retirements mark advanced (a work-size
        // park, including a slider or hotkey rebuild) or the running model size changed. From that point the samples
        // belong to the transition, so the smoothing restarts and nothing is decided until the new size seeds it.
        if (haveApplied_ &&
            (in.appliedSeq != appliedSeq_ || in.appliedWorkW != appliedWorkW_ || in.appliedWorkH != appliedWorkH_))
            OnApplied(in.nowMs);
        appliedSeq_ = in.appliedSeq;
        appliedWorkW_ = in.appliedWorkW;
        appliedWorkH_ = in.appliedWorkH;
        haveApplied_ = true;
        if (discardUntil_ != kUnset)
        {
            if (in.nowMs < discardUntil_)
                return Finish(out, in.nowMs); // the transition owns these samples: the tier and average hold
            discardUntil_ = kUnset;
            BeginSeed(in.nowMs);
        }
        const double rate = (in.fgActive ? in.multiplier : 1.0) * 1000.0 / interval;
        if (seeding_)
        {
            SeedSample(rate);
            if (in.nowMs >= seedUntil_)
            {
                if (seedN_ == 0)
                    seedUntil_ = in.nowMs + kSeedMs; // a pause inside the seed: wait for real samples
                else
                {
                    avg_ = seedMean_;
                    avgAt_ = in.nowMs;
                    seeding_ = false;
                    seedUntil_ = kUnset;
                    if (probeOpen_ && probeJudgeFrom_ == kUnset)
                        probeJudgeFrom_ = in.nowMs; // the judge runs on the applied size, not the transition
                }
            }
            return Finish(out, in.nowMs);
        }
        Feed(in.nowMs, rate);
        // The VRAM gate holds in every state, including the top one: it counts only the retirements the last size
        // change parked, so a hotkey or slider rebuild parks the governor too until the old size releases.
        // Intentional: the average freezes with the tier while it holds. Past the transition window a still
        // depressed rate measures the draining queued work, not the tier, so the verdict waits for the release
        // instead of stepping on it; fences that never resolve freeze the tier (the fail-safe).
        if (!in.texturesReleased)
        {
            Freeze(in.nowMs, Reason::TexturesPending);
            return Finish(out, in.nowMs);
        }
        if (probeOpen_ && probeJudgeFrom_ != kUnset && in.nowMs - probeJudgeFrom_ >= kProbeJudgeMs)
        {
            if (avg_ >= kProbeKeep * in.targetFps)
            {
                // The higher tier holds the target: it is the new baseline, and the next probe waits the base dwell.
                probeOpen_ = false;
                probeJudgeFrom_ = kUnset;
                upDwellMs_ = kUpDwellBaseMs;
                belowSince_ = aboveSince_ = kUnset;
            }
            else
            {
                // The probe failed: back to the tier it came from, and the next probe waits twice as long.
                upDwellMs_ = std::min(upDwellMs_ * 2, kUpDwellMaxMs);
                const int back = probeFrom_;
                probeOpen_ = false;
                probeJudgeFrom_ = kUnset;
                StepTo(back, in.nowMs, false);
                return Finish(out, in.nowMs);
            }
        }
        const double lo = kDownBelow * in.targetFps;
        const double keep = kProbeKeep * in.targetFps;
        if (avg_ < lo)
        {
            aboveSince_ = kUnset;
            if (probeOpen_)
            {
                // An open probe owns the verdict: a down step now would judge the transition, not the tier.
                reason_ = Reason::BelowTarget;
                return Finish(out, in.nowMs);
            }
            if (belowSince_ == kUnset)
                belowSince_ = in.nowMs;
            if (tier_ >= 0 && tier_ + 1 >= kTierCount)
            {
                reason_ = Reason::AtFloor;
                return Finish(out, in.nowMs);
            }
            reason_ = Reason::BelowTarget;
            if (in.nowMs - belowSince_ >= kStepDownDwellMs &&
                (dwellStart_ == kUnset || in.nowMs - dwellStart_ >= kStepDownDwellMs))
            {
                if (tier_ < 0)
                {
                    const int next = FirstStepForCeiling(ceiling);
                    if (next < 0)
                        reason_ = Reason::BelowRange;
                    else
                        StepTo(next, in.nowMs, false);
                }
                else
                    StepTo(tier_ + 1, in.nowMs, false);
            }
        }
        else if (avg_ >= keep)
        {
            belowSince_ = kUnset;
            if (aboveSince_ == kUnset)
                aboveSince_ = in.nowMs;
            if (tier_ < 0)
                reason_ = Reason::Full;
            else
            {
                reason_ = Reason::AboveTarget;
                if (!probeOpen_ && in.nowMs - aboveSince_ >= upDwellMs_ &&
                    (dwellStart_ == kUnset || in.nowMs - dwellStart_ >= upDwellMs_))
                {
                    // Probe one tier up: kept only if the applied size still holds the target (the verdict above).
                    // Stepping above the ceiling is going home: the top state is exactly the user value.
                    const int up = (tier_ == 0 || kTiers[tier_ - 1] > ceiling + 1e-6f) ? -1 : tier_ - 1;
                    StepTo(up, in.nowMs, true);
                }
            }
        }
        else
        {
            // Between stepping down (below 95%) and probing up (at or above 97%): hold, banking neither dwell.
            belowSince_ = aboveSince_ = kUnset;
            reason_ = tier_ < 0 ? Reason::Full : Reason::Holding;
        }
        return Finish(out, in.nowMs);
    }

    void Reset()
    {
        tier_ = -1;
        ceiling_ = 1.0f;
        haveCeiling_ = false;
        avg_ = 0.0;
        avgAt_ = kUnset;
        discardUntil_ = kUnset;
        seeding_ = false;
        seedUntil_ = kUnset;
        seedMean_ = 0.0;
        seedN_ = 0;
        dwellStart_ = kUnset;
        belowSince_ = aboveSince_ = kUnset;
        lastTick_ = kUnset;
        probeOpen_ = false;
        probeFrom_ = -1;
        probeJudgeFrom_ = kUnset;
        upDwellMs_ = kUpDwellBaseMs;
        appliedSeq_ = 0;
        appliedWorkW_ = appliedWorkH_ = 0;
        haveApplied_ = false;
        fgFrameSeen_ = 0;
        haveFgFrame_ = false;
        fgStaleTicks_ = 0;
        reason_ = Reason::Off;
    }

    // Highest tier strictly below `ceiling`, the 50% floor tier when the ceiling is exactly 50% (it equals the
    // ceiling, and stepping to it is still stepping down), or -1 when the ceiling is below the 50% floor.
    static int FirstStepForCeiling(float ceiling)
    {
        for (int i = 0; i < kTierCount; ++i)
            if (kTiers[i] < ceiling - 1e-6f)
                return i;
        if (ceiling >= kTiers[kTierCount - 1] - 1e-6f)
            return kTierCount - 1;
        return -1;
    }

  private:
    // Every timer (dwells, discard, seed, probe judge) pauses by shifting past a gap the governor sat out.
    void ShiftTimers(uint64_t gap)
    {
        auto shift = [gap](uint64_t& t) {
            if (t != kUnset)
                t += gap;
        };
        shift(dwellStart_);
        shift(belowSince_);
        shift(aboveSince_);
        shift(discardUntil_);
        shift(seedUntil_);
        shift(probeJudgeFrom_);
        shift(avgAt_);
    }

    // A transient stand-down: the tier and the cap stay, every timer pauses past the gap, and the average
    // freezes. Only the real stand-downs below reset to the top state.
    void Freeze(uint64_t now, Reason reason)
    {
        if (lastTick_ != kUnset && now > lastTick_)
            ShiftTimers(now - lastTick_);
        reason_ = reason;
    }

    void StandDown(Reason reason, uint64_t now)
    {
        tier_ = -1;
        avg_ = 0.0;
        avgAt_ = kUnset;
        probeOpen_ = false;
        probeFrom_ = -1;
        probeJudgeFrom_ = kUnset;
        upDwellMs_ = kUpDwellBaseMs;
        dwellStart_ = kUnset;
        belowSince_ = aboveSince_ = kUnset;
        lastTick_ = now;
        StartDiscard(now);
        reason_ = reason;
    }

    // A ceiling change clamps the tier at once and restarts everything: the smoothing, the transition window and the
    // dwell timers, and the probe back-off (a new ceiling is a new regime).
    void AdoptCeiling(float ceiling, uint64_t now)
    {
        ceiling_ = ceiling;
        haveCeiling_ = true;
        if (tier_ >= 0 && kTiers[tier_] > ceiling + 1e-6f)
        {
            tier_ = -1;
            for (int i = 0; i < kTierCount; ++i)
            {
                if (kTiers[i] <= ceiling + 1e-6f)
                {
                    tier_ = i;
                    break;
                }
            }
        }
        avg_ = 0.0;
        avgAt_ = kUnset;
        probeOpen_ = false;
        probeFrom_ = -1;
        probeJudgeFrom_ = kUnset;
        upDwellMs_ = kUpDwellBaseMs;
        dwellStart_ = now;
        lastTick_ = now;
        StartDiscard(now);
    }

    // A reported size change was parked or a new size renders: the transition owns the next second of samples.
    void OnApplied(uint64_t now) { StartDiscard(now); }

    void StepTo(int tier, uint64_t now, bool isProbe)
    {
        probeFrom_ = isProbe ? tier_ : -1;
        probeOpen_ = isProbe;
        probeJudgeFrom_ = kUnset;
        tier_ = tier;
        dwellStart_ = now;
        belowSince_ = aboveSince_ = kUnset;
        avgAt_ = kUnset;
        StartDiscard(now);
    }

    void StartDiscard(uint64_t now)
    {
        discardUntil_ = now + kDiscardMs;
        seeding_ = false;
        seedUntil_ = kUnset;
        seedMean_ = 0.0;
        seedN_ = 0;
        belowSince_ = aboveSince_ = kUnset;
        probeJudgeFrom_ = kUnset;
    }

    void BeginSeed(uint64_t now)
    {
        seeding_ = true;
        seedUntil_ = now + kSeedMs;
        seedMean_ = 0.0;
        seedN_ = 0;
    }

    void SeedSample(double sample)
    {
        if (!(sample > 0.0) || !std::isfinite(sample))
            return;
        // The window is past the transition, so its own running mean is the clip reference: one stray sample cannot
        // own the seed, and the lone-sample seed the old code did is impossible (an empty window extends instead).
        if (seedN_ > 0)
            sample = std::clamp(sample, seedMean_ / kSpikeClip, seedMean_ * kSpikeClip);
        seedMean_ += (sample - seedMean_) / (double) (seedN_ + 1);
        ++seedN_;
    }

    void Feed(uint64_t now, double sample)
    {
        if (!(sample > 0.0) || !std::isfinite(sample))
            return;
        if (!(avg_ > 0.0) || !std::isfinite(avg_))
        {
            // No live average and no seed running: seed from a window, never from this lone sample.
            BeginSeed(now);
            SeedSample(sample);
            return;
        }
        sample = std::clamp(sample, avg_ / kSpikeClip, avg_ * kSpikeClip);
        const double dt = (avgAt_ != kUnset && now >= avgAt_) ? (double) (now - avgAt_) : 0.0;
        const double weight = 1.0 - std::exp(-std::min(dt, 5000.0) / kSmoothTauMs);
        avg_ += weight * (sample - avg_);
        avgAt_ = now;
    }

    Output Finish(Output& out, uint64_t now)
    {
        out.cap = tier_ < 0 ? 0.0f : kTiers[tier_];
        out.tierPct = tier_ < 0 ? 0 : (int) std::lround(kTiers[tier_] * 100.0f);
        out.measuredFps = avg_;
        out.reason = reason_;
        out.upDwellMs = upDwellMs_;
        lastTick_ = now;
        return out;
    }

    int tier_ = -1; // -1 = top (no cap), else an index into kTiers
    float ceiling_ = 1.0f;
    bool haveCeiling_ = false;
    double avg_ = 0.0; // the smoothed displayed rate
    uint64_t avgAt_ = kUnset;
    uint64_t discardUntil_ = kUnset; // the transition owns samples before this time
    bool seeding_ = false;           // the post-transition window that seeds the average
    uint64_t seedUntil_ = kUnset;
    double seedMean_ = 0.0;
    uint64_t seedN_ = 0;
    uint64_t dwellStart_ = kUnset; // the last step (or ceiling change): no step banks while frozen
    uint64_t belowSince_ = kUnset, aboveSince_ = kUnset;
    uint64_t lastTick_ = kUnset; // the previous tick: transient gaps shift the timers past it
    bool probeOpen_ = false;     // an up step under judgment: down steps wait for the verdict
    int probeFrom_ = -1;         // the tier the open probe came from
    uint64_t probeJudgeFrom_ = kUnset; // the verdict window runs on the applied size, from here
    uint64_t upDwellMs_ = kUpDwellBaseMs;
    uint64_t appliedSeq_ = 0; // the last parked-retirements mark seen
    unsigned appliedWorkW_ = 0, appliedWorkH_ = 0;
    bool haveApplied_ = false;
    uint64_t fgFrameSeen_ = 0;  // the last present counter seen while a feature owns presents
    bool haveFgFrame_ = false;  // a counter value was adopted (re-adopted whenever the feature returns)
    unsigned fgStaleTicks_ = 0; // consecutive ticks without the counter advancing
    Reason reason_ = Reason::Off;
};
} // namespace DlssNr::Governor
