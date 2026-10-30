#pragma once
// The three plate image slots and their states. Pure logic (no D3D, no OS) over the slot ring's reader fences, so the
// reservation rule can be unit-tested without a GPU; the engine owns the textures and creates them.
//
// Free -> Reserved (the first accepted plate draw of a frame asked for it) -> Staged (the frame finished) -> Drawable
// (the record was promoted) -> Draining (a newer record replaced it) -> Free (every reader fence completed).
//
// A slot is written only when it is Free: not drawable, not staged, not reserved, not pinned by the output pass, and
// every fence of the queue operations that read it has completed. A Reserved or Staged slot older than
// kReclaimMs is reclaimed at the next reservation, which covers a present that never reached the engine's frame
// finish. Pins and reader fences are an overlay, not states.

#include "ext/hudless/HudlessLogic.h" // SlotRing, FenceReader

#include <cstddef>
#include <cstdint>

namespace ext::hudless::plates
{
enum class SlotState : uint8_t
{
    Free,
    Reserved,
    Staged,
    Drawable,
    Draining,
};

class SlotTable
{
  public:
    static constexpr size_t kSlots = 3;
    // A reservation that never reached the frame finish is taken back after this long.
    static constexpr uint64_t kReclaimMs = 500;
    using Ring = SlotRing<kSlots>;

    // A read the pass proved with the record copy: the slot may not be reserved again until it is dropped, and a pin
    // taken before a reset never releases a slot of the new generation.
    struct Pin
    {
        int slot = -1;
        uint64_t generation = 0;
        explicit operator bool() const { return slot >= 0; }
    };

    // Every slot free, pins dropped, readers cleared, new generation (release, device change, resize).
    void Reset()
    {
        ++generation_;
        ring_.ClearAll();
        for (auto& slot : slots_)
            slot = {};
    }

    uint64_t Generation() const { return generation_; }

    // The slot a frame may draw into: Free, unpinned, every reader fence completed. A stale reservation is reclaimed
    // first. Round robin from `hint` so a slot that just drained is not preferred. -1 when every slot is in use.
    int Reserve(size_t hint, uint64_t nowMs, uint64_t frame, FenceReader& fences)
    {
        ReclaimStale(nowMs);
        for (size_t n = 0; n < kSlots; ++n)
        {
            const size_t index = (hint + n) % kSlots;
            auto& slot = slots_[index];
            if (slot.state != SlotState::Free || slot.pins > 0)
                continue;
            if (ring_.Busy(index, frame, fences))
                continue;
            slot.state = SlotState::Reserved;
            slot.sinceMs = nowMs;
            return static_cast<int>(index);
        }
        return -1;
    }

    // The frame's image is complete (the engine's frame finish). Reserved -> Staged.
    bool Stage(int slot)
    {
        return Transition(slot, SlotState::Reserved, SlotState::Staged);
    }

    // The record was promoted: the outputs may draw this slot. Staged -> Drawable.
    bool Promote(int slot)
    {
        return Transition(slot, SlotState::Staged, SlotState::Drawable);
    }

    // A newer record replaced this one: keep the textures until every reader fence completed.
    bool Retire(int slot)
    {
        return Transition(slot, SlotState::Drawable, SlotState::Draining);
    }

    // A frame that was reserved and then not drawn (declined, or the image never finished): back to Free at once.
    void Release(int slot)
    {
        if (!Valid(slot))
            return;
        slots_[static_cast<size_t>(slot)].state = SlotState::Free;
        slots_[static_cast<size_t>(slot)].sinceMs = 0;
    }

    // Draining -> Free once nothing reads the slot any more. Called by the reserve path and the frame finish.
    void FreeDrained(uint64_t frame, FenceReader& fences)
    {
        for (size_t i = 0; i < kSlots; ++i)
        {
            if (slots_[i].state != SlotState::Draining || slots_[i].pins > 0)
                continue;
            if (!ring_.Busy(i, frame, fences))
                slots_[i].state = SlotState::Free;
        }
    }

    // Reclaims a Reserved or Staged slot that never reached the frame finish. Returns how many were reclaimed.
    unsigned ReclaimStale(uint64_t nowMs)
    {
        unsigned reclaimed = 0;
        for (auto& slot : slots_)
        {
            if ((slot.state == SlotState::Reserved || slot.state == SlotState::Staged) && nowMs >= slot.sinceMs &&
                nowMs - slot.sinceMs >= kReclaimMs)
            {
                slot.state = SlotState::Free;
                slot.sinceMs = 0;
                ++reclaimed;
            }
        }
        staleReclaims_ += reclaimed;
        return reclaimed;
    }

    unsigned StaleReclaims() const { return staleReclaims_; }

    // The pass reads this slot until it drops the pin. Only a Drawable or Draining slot may be pinned: everything else
    // is either not published yet or already handed back.
    Pin TakePin(int slot)
    {
        if (!Valid(slot))
            return {};
        auto& entry = slots_[static_cast<size_t>(slot)];
        if (entry.state != SlotState::Drawable && entry.state != SlotState::Draining)
            return {};
        ++entry.pins;
        return Pin { slot, generation_ };
    }

    // False when the table was reset (release, device change) after the pin was taken: the pin was dropped with it.
    bool DropPin(const Pin& pin)
    {
        if (!Valid(pin.slot) || pin.generation != generation_)
            return false;
        auto& entry = slots_[static_cast<size_t>(pin.slot)];
        if (entry.pins == 0)
            return false;
        --entry.pins;
        return true;
    }

    // The fence of one queue operation that reads the slot (the produce fence at promote, the pass fence at the end of
    // its draw). False when the slot tracks too many fences: the caller must treat the slot as unusable.
    bool AddReader(int slot, const void* fence, uint64_t value, uint64_t frame)
    {
        return Valid(slot) && ring_.AddReader(static_cast<size_t>(slot), fence, value, frame);
    }
    bool Busy(int slot, uint64_t frame, FenceReader& fences)
    {
        return !Valid(slot) || ring_.Busy(static_cast<size_t>(slot), frame, fences);
    }
    size_t Readers(int slot) const { return Valid(slot) ? ring_.Readers(static_cast<size_t>(slot)) : 0; }
    bool Reader(int slot, size_t i, const void*& fence, uint64_t& value) const
    {
        return Valid(slot) && ring_.Reader(static_cast<size_t>(slot), i, fence, value);
    }
    void DropReaders(int slot)
    {
        if (Valid(slot))
            ring_.Clear(static_cast<size_t>(slot));
    }

    SlotState State(int slot) const { return Valid(slot) ? slots_[static_cast<size_t>(slot)].state : SlotState::Free; }
    unsigned Pins(int slot) const { return Valid(slot) ? slots_[static_cast<size_t>(slot)].pins : 0; }
    size_t ReadersUnchecked(int slot) const { return ring_.Readers(static_cast<size_t>(slot)); }

  private:
    struct Entry
    {
        SlotState state = SlotState::Free;
        unsigned pins = 0;
        uint64_t sinceMs = 0; // entered Reserved / Staged (the 500 ms reclaim)
    };

    static bool Valid(int slot) { return slot >= 0 && static_cast<size_t>(slot) < kSlots; }

    bool Transition(int slot, SlotState from, SlotState to)
    {
        if (!Valid(slot) || slots_[static_cast<size_t>(slot)].state != from)
            return false;
        slots_[static_cast<size_t>(slot)].state = to;
        // The reclaim window runs from the reservation: staging keeps its stamp, a published slot no longer needs one.
        if (to == SlotState::Drawable)
            slots_[static_cast<size_t>(slot)].sinceMs = 0;
        return true;
    }

    Entry slots_[kSlots] {};
    Ring ring_;
    uint64_t generation_ = 1;
    unsigned staleReclaims_ = 0;
};
} // namespace ext::hudless::plates
