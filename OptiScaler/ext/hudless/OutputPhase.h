#pragma once
// The output phase model: which in-between position of two real frames an output frame shows. Pure logic (no D3D, no
// OS) so the frame generator's batch counting can be unit-tested without a game.
//
// The game thread closes the previous batch at every real present and opens the next one; the presenting thread asks
// for every output which in-batch index it is. A batch whose output count does not equal the expected multiplier (a
// lagging output attributed to the next batch, a bunching present, a multiplier change) never shifts: that batch and
// the next one are drawn unshifted and the model re-locks. The multiplier is unknown (0) while the driver picks the
// count per frame: never shift on unknown.
//
// s = (k + c) / M for the k-th output (0-based) of a batch of M, with c from [HudLess] PlatePhaseOffset: c = 1 when
// the real frame presents last in its batch (generated frames first), 0 when it presents first.
#include <cstdint>
#include <mutex>

namespace ext::hudless
{
class OutputPhase
{
  public:
    // Consecutive consistent batches before an output may shift.
    static constexpr unsigned kLockBatches = 8;
    // Larger multipliers are not observed (and never 6x): treat them as unknown.
    static constexpr unsigned kMaxMultiplier = 8;

    // Game thread, once per real present: closes the previous batch, opens the next one with `multiplier` (0 when
    // the driver picks the count). A later NoteMultiplier amends the open batch until its first output.
    void OnPost(unsigned multiplier)
    {
        std::lock_guard lock(mutex_);
        if (expectedM_ > 0 && outputs_ == expectedM_)
        {
            if (lockCount_ < kLockBatches)
                ++lockCount_;
        }
        else
        {
            lockCount_ = 0;
        }
        locked_ = lockCount_ >= kLockBatches;
        ++serial_;
        expectedM_ = Known(multiplier);
        outputs_ = 0;
    }

    // Frame-generator thread, per real dispatch: the expected outputs of the batch being dispatched. Amends the open
    // batch while none of its outputs presented yet.
    void NoteMultiplier(unsigned multiplier)
    {
        std::lock_guard lock(mutex_);
        if (outputs_ == 0 && serial_ > 0)
            expectedM_ = Known(multiplier);
    }

    struct Output
    {
        uint64_t serial = 0; // the open batch (1-based; 0 before the first present)
        unsigned k = 0;      // the output's in-batch index (0-based)
        unsigned m = 0;      // the batch's expected multiplier (0 = unknown)
        bool shift = false;  // the caller may shift this output by s
        float s = 1.0f;      // the share of the last frame-to-frame movement that belongs to this output
    };

    // Presenting thread, once per output.
    Output OnOutput(int phaseC)
    {
        std::lock_guard lock(mutex_);
        const unsigned k = outputs_++;
        Output out { serial_, k, expectedM_, false, 1.0f };
        if (!locked_ || expectedM_ == 0 || k >= expectedM_)
            return out;
        out.shift = true;
        out.s = (static_cast<float>(k) + static_cast<float>(phaseC)) / static_cast<float>(expectedM_);
        if (out.s < 0.0f)
            out.s = 0.0f;
        else if (out.s > 1.0f)
            out.s = 1.0f;
        return out;
    }

    bool Locked() const
    {
        std::lock_guard lock(mutex_);
        return locked_;
    }

  private:
    static unsigned Known(unsigned multiplier) { return multiplier > 0 && multiplier <= kMaxMultiplier ? multiplier : 0; }

    mutable std::mutex mutex_;
    uint64_t serial_ = 0;
    unsigned expectedM_ = 0;
    unsigned outputs_ = 0;
    unsigned lockCount_ = 0;
    bool locked_ = false;
};
} // namespace ext::hudless
