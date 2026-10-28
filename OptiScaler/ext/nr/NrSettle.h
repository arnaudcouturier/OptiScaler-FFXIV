#pragma once

// The settle decision of the NR pass. A size change is expensive (the model is rebuilt: a couple of hundred milliseconds with no
// NR on those frames), so a request to change the model's working size must hold steadily for a settle time before it is acted
// on; until then the pass keeps running the layout it already has. Everything here is pure: the pass supplies the applied and the
// requested identity and the clock, and acts on the answer.

#include <cstdint>

namespace DlssNr::Settle
{

constexpr int64_t kSettleMs = 500;

enum class Action
{
    Keep,         // nothing changed
    Wait,         // the working size differs but the request has not held long enough: keep running the applied layout
    HistoryReset, // only the warp changed: adopt it, reset the NR history, no model rebuild
    Rebuild,      // the working size changed and the request held for the settle time
    RebuildNow,   // the frame size changed (or something else forces it): no waiting
};

// What a layout amounts to for this decision: the frame the pass runs on, the size the model works at, and a key that changes
// whenever any other part of the layout (the warp) changes.
struct Identity
{
    uint32_t frameW = 0, frameH = 0, workW = 0, workH = 0;
    uint64_t layoutKey = 0;
    // The size of the ordinary working grid. With spatial compression on, the unpack scratch is sized by it while the model is
    // sized by workW/workH, so a change of only this size must not be taken for a warp-only change.
    uint32_t ordW = 0, ordH = 0;
    bool operator==(const Identity& o) const
    {
        return frameW == o.frameW && frameH == o.frameH && workW == o.workW && workH == o.workH && layoutKey == o.layoutKey &&
               ordW == o.ordW && ordH == o.ordH;
    }
};

// True when the model's working size or the size of the grid its answer is resolved on differs: the cases that need the work-size
// scratch rebuilt (and the settle time), as opposed to a change of the warp alone.
inline bool WorkSizeDiffers(const Identity& a, const Identity& b)
{
    return a.workW != b.workW || a.workH != b.workH || a.ordW != b.ordW || a.ordH != b.ordH;
}

// What the running model's work-size scratch was built for (`built`: the applied identity, with the model's real working size)
// against what is about to run: true when the model's working size or the ordinary grid differs. The pass then rebuilds the
// model and parks the work-size scratch.
inline bool WorkScratchStale(const Identity& built, uint32_t workW, uint32_t workH, uint32_t ordW, uint32_t ordH)
{
    return built.workW != workW || built.workH != workH || built.ordW != ordW || built.ordH != ordH;
}

// The request being waited for and when it was first seen.
struct State
{
    bool pending = false;
    Identity request {};
    int64_t since = 0;
};

// Which scratch a change invalidates: nothing (the warp only), what is sized by the model's working size, or everything
// (the frame size).
enum class ParkScope { None, WorkSize, All };

// `immediate` is for the changes that never wait (a placement or tuning change, a spatial fallback, no model yet): a working-size
// difference is then acted on at once.
inline Action Decide(State& state, const Identity& applied, const Identity& requested, int64_t nowMs, bool immediate = false,
                     int64_t settleMs = kSettleMs)
{
    if (applied == requested)
    {
        state.pending = false;
        return Action::Keep;
    }
    if (applied.frameW != requested.frameW || applied.frameH != requested.frameH)
    {
        state.pending = false;
        return Action::RebuildNow;
    }
    if (!WorkSizeDiffers(applied, requested))
    {
        state.pending = false;
        return Action::HistoryReset;
    }
    if (immediate)
    {
        state.pending = false;
        return Action::RebuildNow;
    }
    if (!state.pending || !(state.request == requested))
    {
        state.pending = true;
        state.request = requested;
        state.since = nowMs;
        return Action::Wait;
    }
    if (nowMs - state.since >= settleMs)
    {
        state.pending = false;
        return Action::Rebuild;
    }
    return Action::Wait;
}

// Only these take the requested layout; Keep and Wait keep the applied one.
inline bool AdoptsRequested(Action a)
{
    return a == Action::HistoryReset || a == Action::Rebuild || a == Action::RebuildNow;
}

// The one place that chooses between what runs and what was asked for. Every consumer (the working size, the model dimensions
// reported to the menu, the fused-resolve choice, the status text, the constants of the pack, guide and unpack dispatches, the
// resolve) reads through the thing this returns.
template <class T> const T& Pick(Action a, const T& applied, const T& requested)
{
    return AdoptsRequested(a) ? requested : applied;
}

inline ParkScope ParkScopeFor(const Identity& applied, const Identity& adopted)
{
    if (applied.frameW != adopted.frameW || applied.frameH != adopted.frameH)
        return ParkScope::All;
    if (WorkSizeDiffers(applied, adopted))
        return ParkScope::WorkSize;
    return ParkScope::None;
}

inline bool ParksFrameScratch(ParkScope s) { return s == ParkScope::All; }
inline bool ParksWorkScratch(ParkScope s) { return s == ParkScope::WorkSize || s == ParkScope::All; }

} // namespace DlssNr::Settle
