#pragma once
// S10 package C: the bookkeeping of the paste image ring as a pure state model (docs/HUDLESS.md sections 6 and 7).
// No D3D12, no clock, no locking: UiPaste.cpp feeds it the completed fence values and the current time and owns the
// mutex, so the rules can be tested exhaustively on the CPU (tests/ext/hudless/ui_paste_ring.cpp).
//
// Rules (docs/HUDLESS.md section 6):
//  * the producer writes a slot only after the slot's previous write AND every paste that read it have completed, and
//    never touches the slot that holds the previous real frame's image (it is the cleanup input of the next pass) or
//    the newest image (the one the consumer is most likely showing); it never waits: no such slot means "skip";
//  * the consumer picks the newest image whose write COMPLETED, at most kStaleImageMs old, and never one older than
//    the last image it pasted (the UI can only move forward in time; pasting the same image again is fine).

#include <array>
#include <cstdint>

namespace ext::hudless::ring
{
constexpr int kSlots = 4;
constexpr double kStaleImageMs = 250.0; // game paused presenting, loading screen, alt-tab
constexpr uint64_t kNever = UINT64_MAX; // a fence value that is never reached (signal failed): the slot is retired

// Completed values of the two fences (the produce fence on the frame-generation queue, the paste fence on the
// presenting queue). UINT64_MAX from GetCompletedValue means device removed and must be mapped to "nothing completed".
struct Fences
{
    uint64_t produced = 0;
    uint64_t pasted = 0;
};

struct Slot
{
    bool allocated = false;     // an image exists for this slot (it may still be in flight)
    bool ready = false;         // holds a produced image that may be pasted
    uint64_t frame = 0;         // real-frame tick of the Produce that wrote it
    uint64_t producedValue = 0; // produce-fence value that completes the write
    uint64_t lastReadValue = 0; // paste-fence value that completes the last paste that read it (0 = never read)
    uint32_t readers = 0;       // pastes between "picked" and "submitted"
    double producedAtMs = 0.0;
};

class Ring
{
  public:
    std::array<Slot, kSlots> slots {};
    uint64_t frame = 0;           // one tick per real frame (Produce or Invalidate)
    int lastWritten = -1;         // slot written on the previous tick, if any
    uint64_t lastPastedFrame = 0; // frame of the newest image the consumer has pasted
    uint32_t epoch = 0;           // bumped by Reset(): a consumer holding a slot index from an older epoch lost it

    // --- producer side (one thread) ---------------------------------------------------------------------------------

    // Starts a real frame. Returns the slot that holds the previous frame's image when it can serve as the cleanup
    // input of this frame's pass (written on the immediately preceding tick), else -1.
    int Tick()
    {
        ++frame;
        const int previous = lastWritten;
        lastWritten = -1;
        if (previous >= 0 && slots[previous].ready && slots[previous].allocated && slots[previous].frame + 1 == frame)
            return previous;
        return -1;
    }

    // Newest ready slot by frame, completed or not; -1 when none.
    int Newest() const
    {
        int best = -1;
        for (int i = 0; i < kSlots; ++i)
            if (slots[i].ready && (best < 0 || slots[i].frame > slots[best].frame))
                best = i;
        return best;
    }

    // The slot to write this frame, or -1 (skip, never wait). Free slots first, then the oldest ready image.
    int PickTarget(int previous, const Fences& done) const
    {
        const int newest = Newest();
        int target = -1;
        for (int i = 0; i < kSlots; ++i)
        {
            const Slot& slot = slots[i];
            if (i == previous || i == newest || slot.readers != 0 || slot.producedValue > done.produced ||
                (slot.allocated && slot.lastReadValue > done.pasted))
            {
                continue;
            }
            if (target < 0 || Better(slot, slots[target]))
                target = i;
        }
        return target;
    }

    void BeginWrite(int s) { slots[s].ready = false; }

    void EndWrite(int s, uint64_t producedValue, double nowMs)
    {
        Slot& slot = slots[s];
        slot.allocated = true;
        slot.ready = true;
        slot.frame = frame;
        slot.producedValue = producedValue;
        slot.producedAtMs = nowMs;
        lastWritten = s;
    }

    // The write was submitted but has no completion token: never reuse or paste this slot again.
    void PoisonWrite(int s)
    {
        slots[s].ready = false;
        slots[s].allocated = true;
        slots[s].producedValue = kNever;
    }

    // Stops pasting until the next write; in-flight bookkeeping (fence values) is kept.
    void InvalidateReady()
    {
        for (auto& slot : slots)
            slot.ready = false;
        lastWritten = -1;
    }

    // The images were handed to the retire list: forget them. `frame` and `lastPastedFrame` keep counting.
    void Reset()
    {
        slots = {};
        lastWritten = -1;
        ++epoch;
    }

    // --- consumer side (any thread, under the owner's lock) ----------------------------------------------------------

    bool Pasteable(const Slot& slot, const Fences& done, double nowMs) const
    {
        return slot.ready && slot.allocated && slot.producedValue <= done.produced &&
               nowMs - slot.producedAtMs <= kStaleImageMs && slot.frame >= lastPastedFrame;
    }

    // The slot to paste, or -1 (skip). Steady timing prefers the image of one real frame behind the newest frame the
    // game submitted: it is almost always complete, so the switch to the next image happens at the same point of every
    // base frame instead of whenever the newest image happens to finish. When no such image qualifies it falls back to
    // the newest qualifying one, because skipping would make the UI blink out of a frame that carries none.
    int PickPaste(const Fences& done, double nowMs, bool steady) const
    {
        const uint64_t allowedFrame = steady ? (frame > 0 ? frame - 1 : 0) : UINT64_MAX;
        int best = -1;
        int bestAllowed = -1;
        for (int i = 0; i < kSlots; ++i)
        {
            const Slot& slot = slots[i];
            if (!Pasteable(slot, done, nowMs))
                continue;
            if (best < 0 || slot.frame > slots[best].frame)
                best = i;
            if (slot.frame <= allowedFrame && (bestAllowed < 0 || slot.frame > slots[bestAllowed].frame))
                bestAllowed = i;
        }
        return steady && bestAllowed >= 0 ? bestAllowed : best;
    }

    bool AnyPasteable(const Fences& done, double nowMs) const { return PickPaste(done, nowMs, false) >= 0; }

    void BeginRead(int s) { ++slots[s].readers; }

    // The paste that read slot `s` was submitted (`value` = the paste-fence value that completes it).
    void EndRead(int s, uint64_t value)
    {
        Slot& slot = slots[s];
        if (slot.readers != 0)
            --slot.readers;
        if (slot.lastReadValue != kNever && (value == kNever || value > slot.lastReadValue))
            slot.lastReadValue = value;
        if (slot.frame > lastPastedFrame)
            lastPastedFrame = slot.frame;
    }

    // The paste was abandoned before it was submitted.
    void AbortRead(int s)
    {
        if (slots[s].readers != 0)
            --slots[s].readers;
    }

  private:
    // Ordering of write targets: never-used or invalidated slots before ready ones, then the oldest image, then a slot
    // that already owns an image (no allocation) before one that needs creating.
    static bool Better(const Slot& a, const Slot& b)
    {
        if (a.ready != b.ready)
            return !a.ready;
        if (a.ready && a.frame != b.frame)
            return a.frame < b.frame;
        return a.allocated && !b.allocated;
    }
};
} // namespace ext::hudless::ring
