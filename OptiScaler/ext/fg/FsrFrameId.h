#pragma once
// FSR frame generation requires the frame ID it is given to rise by exactly 1 per dispatched frame: "the frame ID must
// increment by exactly 1 each frame. Any other difference between consecutive frames will reset frame generation logic"
// (FidelityFX frame interpolation API, Configure frame generation). The provider's dispatch selector is shared with every
// other provider (IFGFeature::GetDispatchIndex) and resyncs onto the game's own frame counter when that counter jumps
// forward past the allowed frame-ahead or falls back after a quality change: the ID it hands out then rises by more than 1,
// or falls back, and FSR drops its interpolation history once per resync.
//
// This header keeps the FSR ID in its own space, rising by exactly 1 per dispatch, and remembers the buffer slot each
// dispatch was recorded for, so the provider's callbacks keep indexing the frame that dispatch recorded. Pure: no D3D, no
// config, no logger, so the whole rule is unit-testable (tests/ext/fg/fsr_frame_id.cpp).
//
// Off by default ([FrameGen] FsrMonotonicFrameId, ext/fg/FsrIdSeams.cpp): while it is off the provider passes the game's own
// frame ID and nothing here runs.

#include <array>
#include <atomic>
#include <cstdint>

namespace ext::fg
{
class FsrFrameIds
{
  public:
    // IDs remembered at once. The provider has four frame slots and a dispatch's callbacks arrive within a few frames, so
    // sixteen are far more than can be in flight; an ID that has been overwritten is only ever a callback that nobody waited
    // for, and the caller falls back to its own slot.
    static constexpr std::uint32_t kIds = 16;

    // The ID to give FSR for the dispatch of `gameFrame`, which uses buffer slot `slot`. It rises by exactly 1 per new
    // dispatch, whatever the game's counter does. The same dispatch asking twice (the configure descriptor and the prepare
    // descriptor of one frame) reads the same ID back: only the immediately previous call is treated as that dispatch,
    // because the selector never dispatches one game frame twice in a row.
    std::uint64_t Next(std::uint64_t gameFrame, int slot)
    {
        if (_armed && gameFrame == _lastGameFrame && slot == _lastSlot)
            return _lastId;
        ++_nextId;
        _armed = true;
        _lastGameFrame = gameFrame;
        _lastId = _nextId;
        _lastSlot = slot;
        // One atomic word per entry: the provider's callbacks read it on their own threads, so an entry is never seen half
        // written (the id and its slot always come from the same dispatch).
        _byId[_lastId % kIds].store(Pack(_lastId, slot), std::memory_order_release);
        return _lastId;
    }

    // The buffer slot the dispatch with `id` was recorded for; -1 = not one of ours (a callback for an ID recorded before
    // the seam was switched on, or one that has been overwritten).
    int Slot(std::uint64_t id) const
    {
        const std::uint64_t e = _byId[id % kIds].load(std::memory_order_acquire);
        return id != 0 && (e >> kSlotBits) == id ? static_cast<int>(e & kSlotMask) : -1; // 0 marks an empty entry
    }

    // Forget the recorded dispatches (the provider is being torn down). The counter keeps rising: a fresh FSR context has no
    // history to protect, and a continuing ID can never repeat one it has seen.
    void Reset()
    {
        for (auto& e : _byId)
            e.store(0, std::memory_order_release);
        _armed = false;
        _lastGameFrame = 0;
        _lastId = 0;
        _lastSlot = -1;
    }

  private:
    // An entry packs the id (high bits; 0 = empty, IDs start at 1) and the buffer slot (low 8 bits; the provider has 4).
    static constexpr unsigned kSlotBits = 8;
    static constexpr std::uint64_t kSlotMask = (1u << kSlotBits) - 1u;
    static std::uint64_t Pack(std::uint64_t id, int slot) { return (id << kSlotBits) | (static_cast<std::uint64_t>(slot) & kSlotMask); }

    std::array<std::atomic<std::uint64_t>, kIds> _byId {};
    std::uint64_t _nextId = 0;
    std::uint64_t _lastId = 0;
    std::uint64_t _lastGameFrame = 0;
    int _lastSlot = -1;
    bool _armed = false;
};
} // namespace ext::fg
