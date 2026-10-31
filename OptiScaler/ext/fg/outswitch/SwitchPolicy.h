#pragma once
// The preflight of a live output switch: a read-only verdict from an input struct, plus the memory that makes the
// verdict cheap and honest on the following frames.
//
// Nothing here touches a chain. A refusal is a decision, not an action: the caller logs it and returns to its present.
//   Idle     the key is off, or the configured output is the active one. Free (one comparison), never logged.
//   Refuse   the switch must not start. Hard refusals are remembered per target until the game restarts (the menu then
//            says restart, and its "try again" clears the memory); transient ones are retried after kTransientRetryMs
//            because nothing was touched and the cause can pass (a busy GPU, a resize, a lost focus). A preflight
//            refusal costs nothing to look at again, so it neither doubles the pause nor counts toward the give-up:
//            those belong to the refusals raised after the switch started touching things (PauseTransient).
//   Go       every input is good. The caller runs the sequence.
//
// The order of the checks is the order the reason is reported in, so the most basic cause wins (no bridge before no
// provider). Pure: no Windows or upstream include (tests/ext/fg/fg_switch_policy.cpp).

#include "ext/fg/outswitch/SwitchTypes.h"

#include <cstdint>

namespace ext::fg::outswitch
{
// A commit is followed by this pause so a second switch cannot start while the new chain is still being judged.
inline constexpr uint32_t kCooldownMs = 3000;
// A transient refusal (nothing touched) is looked at again after this pause, doubled for every refusal in a row: 1, 2,
// 4, 8, 16 and then 30 s. A cause that keeps coming back therefore costs a decreasing share of the presents instead of a
// barrier, a drain and six log lines every second for the rest of the session.
inline constexpr uint32_t kTransientRetryMs = 1000;
inline constexpr uint32_t kTransientRetryMaxMs = 30000;
// After this many consecutive transient refusals the cause is stored as a hard one, so the menu can say "restart"
// instead of the module stalling for 30 s at a time forever. Ten refusals is two minutes of backoff.
inline constexpr uint32_t kTransientGiveUp = 10;
// A resize within this window refuses the switch: the chains are being rebuilt.
inline constexpr uint32_t kResizeBusyMs = 500;
// A resize from another thread within this window refuses the switch: the thread model is not what the switch assumes.
inline constexpr uint32_t kThreadMixMs = 5000;
// Switches per process. The retired provider shells are capped by it (one shell per switch).
inline constexpr uint32_t kMaxSwitches = 8;

// Everything the verdict needs, filled by the runtime from the live state (read only).
struct PreflightInputs
{
    bool keyOn = false;
    Target target = Target::Plain; // the configured output
    bool targetKnown = true;       // false: DLSS-G, reprojection, a value the switch does not know
    bool activeKnown = true;       // false: the active output is not one of the three
    Target active = Target::Plain;
    bool bridgePresent = false;    // DX11->DX12 bridge, an FG chain on it, the upscaler as input
    bool shuttingDown = false;
    bool windowed = true;
    bool waitable = false;         // DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT on the FG chain
    bool gameHdr = false;          // game-native HDR colour space recorded and the fork does not own the output
    bool optiHdr = false;          // the fork's HDR output is active
    bool secondBridge = false;     // a second bridge exists in this process: the switch owns one window
    bool loadersOk = false;        // the target's libraries load (cached per target)
    bool hooksOk = true;           // the swapchain hook slots are consistent
    bool hooksIdle = true;         // no hook body and no pass-through is inside right now (a read-only gate peek)
    bool canCapture = false;       // descriptor, factory and queue of the live chain are obtainable
    LibraryModel activeModel;      // measured row of the active class (ignored for plain)
    LibraryModel targetModel;      // measured row of the target class (ignored for plain)
    uint32_t activeRefs = 0;       // the live chain's standstill reference count, read now; 0 = not measured
    uint32_t expectedRefs = 0;     // the count the row (or the first present) measured on that chain; 0 = not measured
    bool witnessUsable = true;     // the row in play needs the private-data witness and it could be placed, read only
    // The runtime's permission to create or take down an XeFG chain in this direction (XeFgDirectionAllowed below). True
    // in the core's own tests; the product reads it from its own direction table.
    bool xeFgSwitchAllowed = true;
    uint64_t lastResizeMs = 0;        // 0 = none
    uint64_t lastForeignResizeMs = 0; // a resize on another thread than the present thread; 0 = none
    uint32_t retiredShells = 0;       // provider shells the session already retired (the runtime's retired set)
};

// The XeFG directions a build takes live: into XeFG from FSR FG or from plain, and out of XeFG to either. A direction
// that a real-process run did not survive is set false in the product's table, with its evidence in the report, and the
// preflight then refuses it (Fail::XeFgSwitch) before anything is read. FSR FG <-> plain never asks.
struct XeFgDirections
{
    bool fsrToXe = true;
    bool plainToXe = true;
    bool xeToFsr = true;
    bool xeToPlain = true;
};

inline bool XeFgDirectionAllowed(const XeFgDirections& directions, Target active, Target target)
{
    if (active == target)
        return true; // not a switch: the policy idles on it before it asks
    if (target == Target::XeFg)
        return active == Target::FsrFg ? directions.fsrToXe : directions.plainToXe;
    if (active == Target::XeFg)
        return target == Target::FsrFg ? directions.xeToFsr : directions.xeToPlain;
    return true;
}

enum class VerdictKind : uint8_t
{
    Idle,
    Go,
    Refuse
};

struct Verdict
{
    VerdictKind kind = VerdictKind::Idle;
    Fail reason = Fail::None;
    Severity severity = Severity::None;
    bool log = true; // false: this refusal is the same as the last logged one, keep it out of the log
};

class Policy
{
  public:
    Verdict Evaluate(const PreflightInputs& in, uint64_t nowMs)
    {
        if (!in.keyOn)
            return { VerdictKind::Idle, Fail::KeyOff, Severity::None, false };

        if (!in.targetKnown)
            return Refuse(Target::Plain, Fail::Unsupported, nowMs, Severity::Transient);

        if (in.target == in.active)
            return { VerdictKind::Idle, Fail::AlreadyActive, Severity::None, false };

        // A transient refusal was looked at a moment ago: nothing to do until the pause is over.
        if (nowMs < retryAfterMs_)
            return { VerdictKind::Idle, Fail::None, Severity::None, false };

        if (!in.activeKnown)
            return Refuse(in.target, Fail::Unsupported, nowMs);

        if (!in.bridgePresent)
            return Refuse(in.target, Fail::NoBridge, nowMs);

        // One window, one chain: a second bridge cannot be described by one LiveChain, so it is refused for the session.
        if (in.secondBridge)
            return Refuse(in.target, Fail::SecondBridge, nowMs);

        if (in.shuttingDown)
            return Refuse(in.target, Fail::ShuttingDown, nowMs);

        if (!in.windowed)
            return Refuse(in.target, Fail::Fullscreen, nowMs);

        if (in.waitable)
            return Refuse(in.target, Fail::WaitableCached, nowMs);

        if (in.gameHdr)
            return Refuse(in.target, Fail::GameHdr, nowMs);

        if (in.optiHdr && in.target == Target::FsrFg)
            return Refuse(in.target, Fail::HdrFsr, nowMs);

        // XeFG either side of a switch: when the runtime does not allow this direction, a chain of that class is never
        // created and never taken down. The pure core allows every direction (the simulation and the sequence tests
        // exercise both paths); the product's table names the directions a real process survived.
        if (!in.xeFgSwitchAllowed && (in.active == Target::XeFg || in.target == Target::XeFg))
            return Refuse(in.target, Fail::XeFgSwitch, nowMs);

        if (!HooksUsable(in))
            return Refuse(in.target, Fail::HooksInconsistent, nowMs);

        // A read-only look at the hook set before anything is touched. A body that is still inside (a present blocked in
        // the library, a resize holding a lock) is otherwise found at the unhook, after the old chain is already torn
        // down: that costs the working chain, the provider shell and a budget unit for a call that had nothing to do with
        // the target. Looking is free, so this is a state condition and never remembered.
        if (!in.hooksIdle)
            return Refuse(in.target, Fail::HookBusy, nowMs);

        // A model is needed for the class that goes away and for the class that arrives: the pin release of the old
        // chain follows the old class's row.
        if (!ModelKnown(in.target, in.targetModel) || !ModelKnown(in.active, in.activeModel))
            return Refuse(in.target, Fail::NoModel, nowMs);

        // A row written with witness=honoured promises the interface can be placed on a chain of that class. If the live
        // chain refuses it, the cross-check the teardown leans on does not exist: refuse here, before anything is
        // detached, instead of releasing a pin on the table alone.
        if (!in.witnessUsable &&
            (WitnessNeeded(in.target, in.targetModel) || WitnessNeeded(in.active, in.activeModel)))
            return Refuse(in.target, Fail::WitnessUnavailable, nowMs);

        if (!in.loadersOk)
            return Refuse(in.target, Fail::MissingProvider, nowMs);

        if (!in.canCapture)
            return Refuse(in.target, Fail::NoCapture, nowMs);

        // The live chain is the one the first present measured. A holder that appeared after that (an overlay, a
        // recorder, a module that grabbed the chain) makes the teardown's reference count wrong, and the only place
        // that shows today is the teardown's own cross-check, after the old chain is already gone. Refuse instead:
        // transient, because nothing was touched and a holder can let go again.
        if (in.activeRefs != 0 && in.expectedRefs != 0 && in.activeRefs != in.expectedRefs)
            return Refuse(in.target, Fail::RefMismatch, nowMs);

        if (Remembered(in.target))
        {
            // The cause is already stored: report it once, do not store "remembered" over it, and never pause a hard
            // refusal on a timer (the memory is what keeps the switch away).
            return RefuseUnremembered(in.target, Fail::Remembered, nowMs);
        }

        if (nowMs < cooldownUntilMs_)
            return Refuse(in.target, Fail::Cooling, nowMs);

        // The retired provider shells are capped independently of the budget: a rollback retires two shells (the old
        // one and the dud it created), so the budget alone would not bound the set.
        if (switchesLeft_ == 0 || in.retiredShells >= kMaxSwitches)
            return RefuseUnremembered(in.target, Fail::Budget, nowMs);

        if (Within(nowMs, in.lastResizeMs, kResizeBusyMs))
            return Refuse(in.target, Fail::ResizeBusy, nowMs);

        if (Within(nowMs, in.lastForeignResizeMs, kThreadMixMs))
            return Refuse(in.target, Fail::ThreadMix, nowMs);

        // A fresh attempt: the log dedup starts over. The transient streak does not: an attempt that reaches Go and then
        // refuses inside the sequence is the next refusal in a row, not a fresh start (only a commit resets it). The one
        // exception is a preflight pause that is more than its own cap old when this Go arrives: the condition did not
        // come back for longer than the longest wait, so a row of refusals from earlier in the session is history, not a
        // row. A refusal raised inside the sequence never takes this path: its pause is what the next Go waits for.
        if (briefPause_ && lastTransientMs_ != 0 && nowMs >= lastTransientMs_ &&
            nowMs - lastTransientMs_ >= kTransientRetryMaxMs)
            transientStreak_ = 0;
        logged_ = false;
        return { VerdictKind::Go, Fail::None, Severity::None, false };
    }

    // One unit of the session budget, spent at the first destructive step of a switch. A switch that rolls back has
    // already retired the old provider shell (and the dud), so it has to pay for them: spending only on a commit let a
    // target that always failed grow the retired set without a bound.
    void Spend()
    {
        if (switchesLeft_ > 0)
            --switchesLeft_;
    }

    // A switch committed: the cooldown starts. The budget was already spent when the switch became destructive.
    void Committed(uint64_t nowMs)
    {
        cooldownUntilMs_ = nowMs + kCooldownMs;
        transientStreak_ = 0;
        ++generation_;
    }

    // Per target, until restart or until the menu's "try again". The first cause is the one the menu keeps: a later
    // "this target failed earlier" refusal never overwrites it.
    void Remember(Target target, Fail why)
    {
        // A transient cause is not a property of the target: remembering it would ban the target for the session for
        // something that had already passed (a busy hook body, a holder that let go).
        if (SeverityOf(why) == Severity::Transient)
            return;
        const uint32_t bit = Bit(target);
        const bool known = (failedMask_ & bit) != 0;
        failedMask_ |= bit;
        for (size_t i = 0; i < 3; ++i)
            if (Bit(static_cast<Target>(i)) == bit && !known)
                failedReason_[i] = why;
    }

    // The menu's "try again": the target's memory goes, and so does the backoff that led to it. A give-up the player
    // overrides must not refuse the next attempt again after one more transient cause, and it must not make the player
    // wait out the last pause first.
    void ClearMemory(Target target)
    {
        const uint32_t bit = Bit(target);
        failedMask_ &= ~bit;
        for (size_t i = 0; i < 3; ++i)
            if (Bit(static_cast<Target>(i)) == bit)
                failedReason_[i] = Fail::None;
        transientStreak_ = 0;
        lastTransientMs_ = 0;
        briefPause_ = false;
        retryAfterMs_ = 0;
    }

    bool Remembered(Target target) const { return (failedMask_ & Bit(target)) != 0; }

    Fail RememberedReason(Target target) const
    {
        for (size_t i = 0; i < 3; ++i)
            if (Bit(static_cast<Target>(i)) == Bit(target))
                return failedReason_[i];
        return Fail::None;
    }

    uint32_t SwitchesLeft() const { return switchesLeft_; }
    uint32_t Generation() const { return generation_; }
    uint64_t CooldownUntilMs() const { return cooldownUntilMs_; }

    // A transient refusal raised inside the sequence (a barrier that timed out, a queue that did not prove idle, a hook
    // body that did not leave): the attempt already paid for its barrier and its drain, so the next one waits longer.
    // Without the pause the next present would re-run the whole attempt: a log line, a status write and the barrier or
    // drain cost on every frame. The pause doubles per consecutive refusal up to kTransientRetryMaxMs, and the
    // kTransientGiveUp-th one stores the cause as a hard one so the menu offers a restart instead of a stall forever.
    void PauseTransient(uint64_t nowMs, Target target, Fail why)
    {
        uint32_t delay = kTransientRetryMs;
        for (uint32_t i = 0; i < transientStreak_ && delay < kTransientRetryMaxMs; ++i)
            delay *= 2;
        if (delay > kTransientRetryMaxMs)
            delay = kTransientRetryMaxMs;
        retryAfterMs_ = nowMs + delay;
        lastTransientMs_ = nowMs;
        briefPause_ = false;

        if (transientStreak_ < kTransientGiveUp)
            ++transientStreak_;
        if (transientStreak_ >= kTransientGiveUp)
            Store(target, why); // the cause is a property of the target now, not of the moment
    }

    // A preflight refusal: nothing was touched, the next present can look again for free, so the pause is the base retry
    // with no doubling and no give-up. Counting these toward the give-up would ban a target for a state the player may
    // change a second later (a resize finishing, the window leaving exclusive fullscreen).
    void PauseBrief(uint64_t nowMs)
    {
        retryAfterMs_ = nowMs + kTransientRetryMs;
        lastTransientMs_ = nowMs;
        briefPause_ = true;
    }

    uint64_t RetryAfterMs() const { return retryAfterMs_; }
    uint32_t TransientStreak() const { return transientStreak_; }

    // One line per distinct refusal: the same reason for the same target is logged once, and a fresh Go (a new attempt)
    // starts the count over. The sequence's own refusals use it too (the preflight one is deduplicated by Dedup above),
    // so a barrier or a drain that keeps failing prints one line per attempt and not one per present.
    bool ShouldLog(Target target, Fail why)
    {
        if (logged_ && why == loggedReason_ && target == loggedTarget_)
            return false;
        logged_ = true;
        loggedReason_ = why;
        loggedTarget_ = target;
        return true;
    }

  private:
    static constexpr uint32_t Bit(Target target) { return 1u << static_cast<uint32_t>(target); }

    // Writes the per-target memory without the transient filter: only the give-up of a transient cause that kept coming
    // back uses it, and by then the cause is worth a restart.
    void Store(Target target, Fail why)
    {
        const uint32_t bit = Bit(target);
        const bool known = (failedMask_ & bit) != 0;
        failedMask_ |= bit;
        for (size_t i = 0; i < 3; ++i)
            if (Bit(static_cast<Target>(i)) == bit && !known)
                failedReason_[i] = why;
    }

    static bool ModelKnown(Target target, const LibraryModel& model)
    {
        return target == Target::Plain || model.model != Model::Unknown;
    }

    static bool WitnessNeeded(Target target, const LibraryModel& model)
    {
        return target != Target::Plain && model.witnessHonoured;
    }

    static bool HooksUsable(const PreflightInputs& in)
    {
        // Hooks are only required once a provider class exists at either end: a plain to plain move has none.
        if (in.target == Target::Plain && in.active == Target::Plain)
            return true;
        return in.hooksOk;
    }

    static bool Within(uint64_t nowMs, uint64_t whenMs, uint32_t windowMs)
    {
        return whenMs != 0 && nowMs >= whenMs && nowMs - whenMs < windowMs;
    }

    Verdict Refuse(Target target, Fail why, uint64_t nowMs, Severity forced = Severity::None)
    {
        const Severity severity = forced != Severity::None ? forced : SeverityOf(why);
        if (severity == Severity::Hard)
            Remember(target, why);
        else if (why != Fail::Cooling)
            PauseBrief(nowMs); // Cooling is already a timer of its own
        return Dedup(target, why, severity);
    }

    // A refusal that must not write the per-target memory (the target is already remembered, or the budget is spent:
    // "remembered" is not the cause). Same log dedup as every other refusal: one line per distinct refusal, not one per
    // present.
    Verdict RefuseUnremembered(Target target, Fail why, uint64_t nowMs)
    {
        (void)nowMs;
        return Dedup(target, why, Severity::Hard);
    }

    Verdict Dedup(Target target, Fail why, Severity severity)
    {
        Verdict v { VerdictKind::Refuse, why, severity, true };
        v.log = !(logged_ && why == loggedReason_ && target == loggedTarget_);
        if (v.log)
        {
            logged_ = true;
            loggedReason_ = why;
            loggedTarget_ = target;
        }
        return v;
    }

    uint32_t failedMask_ = 0;
    Fail failedReason_[3] { Fail::None, Fail::None, Fail::None };
    uint32_t switchesLeft_ = kMaxSwitches;
    uint32_t generation_ = 0;
    uint64_t cooldownUntilMs_ = 0;
    uint64_t retryAfterMs_ = 0;
    uint64_t lastTransientMs_ = 0; // when the last pause was set: an old streak expires with it
    bool briefPause_ = false;      // the last pause was a preflight one (kTransientRetryMs, no counting)
    uint32_t transientStreak_ = 0;
    bool logged_ = false;
    Fail loggedReason_ = Fail::None;
    Target loggedTarget_ = Target::Plain;
};
} // namespace ext::fg::outswitch
