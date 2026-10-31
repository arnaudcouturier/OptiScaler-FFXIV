#pragma once
// The judgement of a freshly adopted chain: is it healthy, and when does the switch put the old presentation back?
//
// Health is the present result and nothing else. Frame generation being off or paused (a cutscene, a loading screen,
// a menu) is not a failure: punishing it made an earlier design retry a switch every few seconds of every cutscene,
// which is the livelock this rule exists to prevent. Activity is only reported as telemetry.
//
//   probation  the first kProbationPresents presents after the commit, or the first kProbationWindowMs, whichever
//              comes first. A failure here means the chain is not good enough: the game sees S_OK for that present
//              (so it does not react to a one-off error of ours), the failure is recorded, and the next present runs
//              the rollback.
//   watchdog   after probation, a failure is passed to the game unchanged and three in a row trigger the same
//              rollback once per session per target (the per-target memory makes the "once").
//   lost device  never masked, never rolled back: the game has to see removal and handle it itself.
//   not a failure  a present result that reports nothing about the chain: S_OK, a DXGI_STATUS_* code, or
//              DXGI_ERROR_WAS_STILL_DRAWING (a DXGI_PRESENT_DO_NOT_WAIT present whose frame is still in flight,
//              REFERENCES.md [MS-3]). Counting those would roll a healthy chain back because the game did not wait.
//   stall      one present longer than kStallMs during probation counts as a failure. The stall clock starts AFTER the
//              switch: the present that ran the switch also carries the provider's creation, which can take seconds, and
//              timing that as a stall rolled back a chain that never had a bad frame. The first present after a commit is
//              therefore exempt (kStallGracePresents).
//
// Pure: no Windows or upstream include (tests/ext/fg/fg_switch_probation.cpp, fg_switch_com_sim.cpp).

#include "ext/fg/outswitch/SwitchTypes.h"

#include <cstdint>

namespace ext::fg::outswitch
{
inline constexpr uint32_t kProbationPresents = 20;
inline constexpr uint64_t kProbationWindowMs = 8000;
inline constexpr uint32_t kStallMs = 2500;
inline constexpr uint32_t kWatchdogFailures = 3;
// The presents after a commit whose duration is not judged: the first one carries the switch itself.
inline constexpr uint32_t kStallGracePresents = 1;

enum class Health : uint8_t
{
    Inactive,
    Healthy,
    Failed,
    DeviceLost
};

class Probation
{
  public:
    struct Note
    {
        Health health = Health::Inactive;
        bool mask = false;     // the caller answers S_OK instead of this result
        bool rollback = false; // the rollback is due; this present still completes
    };

    // A switch committed: the judgement of the new chain starts now.
    void Begin(uint64_t nowMs)
    {
        *this = Probation {};
        watching_ = true;
        inProbation_ = true;
        startedMs_ = nowMs;
    }

    // The chain is gone (a switch started, or the module gave up): stop judging it.
    void End() { *this = Probation {}; }

    Note Present(uint32_t result, uint32_t presentMs, uint64_t nowMs)
    {
        Note note;
        if (!watching_)
            return note;

        if (DeviceLost(result))
        {
            note.health = Health::DeviceLost;
            watching_ = false;
            inProbation_ = false;
            rollback_ = false;
            lastResult_ = result;
            return note;
        }

        // A result that says nothing about the chain, including DXGI_ERROR_WAS_STILL_DRAWING: not a failure.
        if (!PresentHealthy(result))
        {
            lastResult_ = result;
            if (inProbation_)
            {
                note.health = Health::Failed;
                note.mask = true;
                note.rollback = true;
                lastFail_ = Fail::Probation;
                rollback_ = true;
                ++failed_;
                inProbation_ = false;
                return note;
            }

            note.health = Health::Failed;
            lastFail_ = Fail::Probation;
            if (++consecutive_ >= kWatchdogFailures)
            {
                note.rollback = true;
                rollback_ = true;
                ++failed_;
            }
            return note;
        }

        // The stall clock starts after the switch: the presents that may have carried the switch are exempt.
        if (presentMs > kStallMs && inProbation_ && presents_ >= kStallGracePresents)
        {
            note.health = Health::Failed;
            note.mask = true;
            note.rollback = true;
            lastResult_ = result;
            lastFail_ = Fail::Stall;
            rollback_ = true;
            ++failed_;
            inProbation_ = false;
            return note;
        }

        // A present the chain accepted.
        ++presents_;
        ++healthy_;
        consecutive_ = 0;
        note.health = Health::Healthy;
        if (inProbation_ && (healthy_ >= kProbationPresents || (nowMs >= startedMs_ && nowMs - startedMs_ >= kProbationWindowMs)))
        {
            inProbation_ = false;
            justJudged_ = true;
        }
        return note;
    }

    // Clears the pending rollback once the sequence has run it.
    void ClearRollback() { rollback_ = false; }

    bool RollbackDue() const { return rollback_; }
    bool Watching() const { return watching_; }
    bool InProbation() const { return inProbation_; }
    // True once, on the present that ended probation healthy (the caller logs and publishes it once).
    bool TakeJudged()
    {
        const bool judged = justJudged_;
        justJudged_ = false;
        return judged;
    }
    uint64_t StartedMs() const { return startedMs_; }
    uint32_t Presents() const { return presents_; }
    uint32_t Healthy() const { return healthy_; }
    uint32_t FailureCount() const { return failed_; }
    Fail LastFail() const { return lastFail_; }
    uint32_t LastResult() const { return lastResult_; }

  private:
    bool watching_ = false;    // judging this chain (probation, then the watchdog)
    bool inProbation_ = false; // still inside the first window
    bool justJudged_ = false;  // the present that ended probation healthy
    bool rollback_ = false;
    uint32_t presents_ = 0;
    uint32_t healthy_ = 0;
    uint32_t failed_ = 0;
    uint32_t consecutive_ = 0;
    uint32_t lastResult_ = 0;
    Fail lastFail_ = Fail::None;
    uint64_t startedMs_ = 0;
};
} // namespace ext::fg::outswitch
