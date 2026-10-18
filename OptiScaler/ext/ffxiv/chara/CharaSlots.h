#pragma once
// Slot and recent-image bookkeeping for the isolated DLSS path: pure state with the clock passed in, so every window and
// rule is tested without a device. A "slot" is the per-preview state (one per preview image, up to 16); the "recent list"
// remembers which preview images a finisher produced lately, so a UI draw that samples one can claim a slot.
//
// Times are milliseconds from any monotonic clock (GetTickCount64 in the game).
//
//   recent entry      : valid for 250 ms after it was last seen
//   slot used         : "live" while used within 250 ms
//   slot idle release : a slot unused for more than 2000 ms (or any slot, when DLSS is switched off) is released, but only
//                       after the GPU query that marks its last work reports done
//   reset flag        : the upscaler history is reset when the slot never evaluated or its last evaluation is older than 250 ms
//   failed            : a failed evaluation or feature creation sticks until the slot is released (no retry); a failed slot is
//                       released kFailedReleaseMs after it failed even while it is touched, so the preview can try again

#include <array>
#include <cstddef>
#include <cstdint>

namespace ext::ffxiv::chara
{
inline constexpr size_t kMaxSlots = 16;
inline constexpr size_t kMaxRecent = 16;
inline constexpr uint64_t kRecentMs = 250;
inline constexpr uint64_t kLiveMs = 250;
inline constexpr uint64_t kResetMs = 250;
inline constexpr uint64_t kIdleReleaseMs = 2000;
inline constexpr uint64_t kFailedReleaseMs = 10000; // a failed slot is released this long after it failed, whatever touches it
inline constexpr int kNoSlot = -1;

// What an event query's GetData answer means: only S_FALSE (1) is "the GPU has not got there yet". S_OK means done, and a failure
// (a lost or removed device never reports S_OK) means no more work will come out of it, so the slot may go.
constexpr bool QueryResultMeansDone(int32_t hresult) { return hresult != 1; }

struct RecentEntry
{
    uintptr_t texture = 0;
    uint64_t tick = 0;
    uint32_t pass = 0;
};

struct SlotState
{
    uintptr_t key = 0; // the finisher output texture the slot belongs to; 0 = free
    uint32_t width = 0;
    uint32_t height = 0;
    bool reversedZ = false; // depth convention at feature creation
    uint64_t lastUse = 0;
    uint64_t lastEvaluate = 0;
    uint32_t evaluatePass = 0;
    uint32_t renders = 0;       // accepted finishers of this preview image so far: the jitter phase of its next render (see JitterPhase)
    bool everEvaluated = false; // the "evaluated OK" flag
    bool failed = false;
    uint64_t failedAt = 0;      // when it failed (0 = not known: never released by the failed-slot rule)
    bool hasFeature = false;    // an upscaler feature exists (counted as active)
    bool queryPending = false;  // GPU work was submitted; release waits for the query to report done

    bool Used() const { return key != 0; }
};

class SlotTable
{
  public:
    // ---- recent list ----
    // Records that `texture` was produced at `now` in pass `pass` (refreshes an existing entry, otherwise takes a free or
    // expired entry, otherwise replaces the oldest).
    void NoteRecent(uintptr_t texture, uint64_t now, uint32_t pass)
    {
        if (texture == 0)
            return;
        RecentEntry* freeEntry = nullptr;
        RecentEntry* oldest = nullptr;
        for (auto& r : _recent)
        {
            if (r.texture == texture)
            {
                r = { texture, now, pass };
                return;
            }
            if (r.texture == 0 || now < r.tick || now - r.tick > kRecentMs)
            {
                if (freeEntry == nullptr)
                    freeEntry = &r;
            }
            else if (oldest == nullptr || r.tick < oldest->tick)
                oldest = &r;
        }
        RecentEntry* target = freeEntry != nullptr ? freeEntry : oldest;
        *target = { texture, now, pass };
    }

    bool RecentContains(uintptr_t texture, uint64_t now) const
    {
        for (const auto& r : _recent)
        {
            if (r.texture == texture && texture != 0 && now >= r.tick && now - r.tick <= kRecentMs)
                return true;
        }
        return false;
    }

    // ---- slots ----
    int Find(uintptr_t texture) const
    {
        if (texture == 0)
            return kNoSlot;
        for (size_t i = 0; i < kMaxSlots; ++i)
        {
            if (_slots[i].key == texture)
                return static_cast<int>(i);
        }
        return kNoSlot;
    }

    // A UI draw sampled `texture`: when it is in the recent list and owns no slot yet, a free slot is claimed for it.
    // Returns the slot index, or kNoSlot (not recent, or every slot is taken).
    int AcquireForConsumer(uintptr_t texture, uint64_t now, uint32_t width, uint32_t height)
    {
        const int existing = Find(texture);
        if (existing != kNoSlot)
            return existing;
        if (!RecentContains(texture, now))
            return kNoSlot;
        for (size_t i = 0; i < kMaxSlots; ++i)
        {
            if (!_slots[i].Used())
            {
                _slots[i] = SlotState {};
                _slots[i].key = texture;
                _slots[i].width = width;
                _slots[i].height = height;
                _slots[i].lastUse = now;
                return static_cast<int>(i);
            }
        }
        return kNoSlot;
    }

    SlotState& Slot(int index) { return _slots[static_cast<size_t>(index)]; }
    const SlotState& Slot(int index) const { return _slots[static_cast<size_t>(index)]; }

    void Touch(int index, uint64_t now) { Slot(index).lastUse = now; }

    // A slot is rebuilt (feature recreated) when the image size or the depth convention changed.
    bool NeedsRebuild(int index, uint32_t width, uint32_t height, bool reversedZ) const
    {
        const SlotState& s = Slot(index);
        return s.width != width || s.height != height || s.reversedZ != reversedZ;
    }

    // The upscaler history is reset when the slot never evaluated or its last evaluation is older than 250 ms.
    bool ShouldReset(int index, uint64_t now) const
    {
        const SlotState& s = Slot(index);
        return !s.everEvaluated || now < s.lastEvaluate || now - s.lastEvaluate > kResetMs;
    }

    void MarkEvaluated(int index, uint64_t now, uint32_t pass)
    {
        SlotState& s = Slot(index);
        s.everEvaluated = true;
        s.lastEvaluate = now;
        s.evaluatePass = pass;
        s.queryPending = true;
    }

    // The slot produced a result in render (pass) `pass`: evaluated OK, not failed since. The consumer draws that result, so an NR result
    // of the same render would never be seen.
    bool EvaluatedInPass(int index, uint32_t pass) const
    {
        const SlotState& s = Slot(index);
        return pass != 0 && s.everEvaluated && !s.failed && s.evaluatePass == pass;
    }

    void MarkFailed(int index, uint64_t now = 0)
    {
        SlotState& s = Slot(index);
        s.failed = true;
        s.failedAt = now;
    }

    // A slot's GPU objects may be released only once the work submitted for it finished (the event query ended after its last
    // evaluate reports done). Clears the pending flag when it did. Serves the idle release and the rebuild of a resized preview.
    template <class QueryDone> bool ReadyForRelease(int index, QueryDone&& queryDone)
    {
        SlotState& s = Slot(index);
        if (!s.queryPending)
            return true;
        if (!queryDone(index))
            return false;
        s.queryPending = false;
        return true;
    }
    void ClearEvaluated(int index) { Slot(index).everEvaluated = false; }

    // Some slot is not failed and was used within 250 ms.
    bool JitterLive(uint64_t now) const
    {
        for (const auto& s : _slots)
        {
            if (s.Used() && !s.failed && now >= s.lastUse && now - s.lastUse <= kLiveMs)
                return true;
        }
        return false;
    }

    // The jitter is applied only when a live slot has already evaluated successfully, so the very first evaluation after a
    // slot is created is not jittered.
    bool JitterApplies(uint64_t now) const
    {
        for (const auto& s : _slots)
        {
            if (s.Used() && !s.failed && s.everEvaluated && now >= s.lastUse && now - s.lastUse <= kLiveMs)
                return true;
        }
        return false;
    }

    size_t ActiveFeatures() const
    {
        size_t n = 0;
        for (const auto& s : _slots)
            n += (s.Used() && s.hasFeature) ? 1 : 0;
        return n;
    }

    // Periodic cleanup. A slot is released when it has been unused for more than 2000 ms, or when `disabled`, and
    // `queryDone(index)` reports that its last GPU work finished (a slot with no pending query releases at once).
    // `release(index)` frees the slot's resources before the slot is cleared. Recent entries older than 250 ms are dropped.
    // Returns the number of slots released.
    template <class QueryDone, class Release>
    size_t Cleanup(uint64_t now, bool disabled, QueryDone&& queryDone, Release&& release)
    {
        size_t released = 0;
        for (size_t i = 0; i < kMaxSlots; ++i)
        {
            SlotState& s = _slots[i];
            if (!s.Used())
                continue;
            const bool idle = now >= s.lastUse && now - s.lastUse > kIdleReleaseMs;
            const bool failedLong = s.failed && s.failedAt != 0 && now >= s.failedAt && now - s.failedAt > kFailedReleaseMs;
            if (!idle && !disabled && !failedLong)
                continue;
            if (!ReadyForRelease(static_cast<int>(i), queryDone))
                continue;
            release(static_cast<int>(i));
            s = SlotState {};
            ++released;
        }
        for (auto& r : _recent)
        {
            if (r.texture != 0 && now >= r.tick && now - r.tick > kRecentMs)
                r = RecentEntry {};
        }
        return released;
    }

    // ---- Jitter phase ----
    // The sub-pixel jitter of a render is chosen when its first geometry draw arrives, before the finisher says which preview image the
    // render is for. Each slot keeps its own render count and the pass takes the phase from the slot it most likely belongs to. A phase
    // taken from the one counter that all previews share would hand each of two alternating previews every second phase, and in base 2
    // every second phase lies in the same half of the pixel in x.
    //
    // The prediction: after the finisher of slot X came the finisher of slot Y the last time, so the render that starts after X's finisher
    // is Y's (strict alternation of previews, a single preview: Y == X). With no history, or when the predicted slot is gone or failed, the
    // caller's shared counter is used, as before.
    int PredictPassSlot() const
    {
        if (_lastFinished == kNoSlot)
            return kNoSlot;
        const int next = _successor[static_cast<size_t>(_lastFinished)];
        const int candidate = next != kNoSlot ? next : _lastFinished;
        const SlotState& s = Slot(candidate);
        return s.Used() && !s.failed ? candidate : kNoSlot;
    }

    uint32_t JitterPhase(uint32_t sharedCounter) const
    {
        const int slot = PredictPassSlot();
        return slot == kNoSlot ? sharedCounter : Slot(slot).renders;
    }

    // The finisher of slot `index` was accepted: the render counts for that slot, and the order of the previews is learned.
    void NoteFinishedRender(int index)
    {
        if (index < 0 || static_cast<size_t>(index) >= kMaxSlots || !Slot(index).Used())
            return;
        ++Slot(index).renders;
        if (_lastFinished != kNoSlot)
            _successor[static_cast<size_t>(_lastFinished)] = index;
        _lastFinished = index;
    }

    void Clear()
    {
        _slots = {};
        _recent = {};
        _lastFinished = kNoSlot;
        _successor.fill(kNoSlot);
    }

  private:
    std::array<SlotState, kMaxSlots> _slots {};
    std::array<RecentEntry, kMaxRecent> _recent {};
    int _lastFinished = kNoSlot;
    std::array<int, kMaxSlots> _successor = [] { std::array<int, kMaxSlots> a {}; a.fill(kNoSlot); return a; }();
};
} // namespace ext::ffxiv::chara
