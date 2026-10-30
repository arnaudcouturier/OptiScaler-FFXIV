#pragma once
// Native nameplates, game side: decides which of the game's draw calls are nameplate draws, asks the plate engine for a
// slot at the frame's first one, and draws those triangles into the slot instead of (or in addition to) the game's own
// target. The engine side (ext/hudless/UiPlates.h) owns the slots, the records and the output pass.
//
// Threads: the draws and the present seam run on the game's render thread; all of this state is that thread's.
//
// Key off: one relaxed load and one branch per draw, nothing else (InterceptIndexed is inline and tests the bit first).

#include "ext/ExtFeatures.h"

#include <cstdint>

struct ID3D11DeviceContext;

namespace ext::ffxiv::plates
{
namespace detail
{
// Out-of-line: the header keeps the per-draw cost to one atomic load while the key is off.
bool InterceptIndexedSlow(ID3D11DeviceContext* context, unsigned count, unsigned start, int baseVertex, void* original);
} // namespace detail

// Called from the game's DrawIndexed hook, for the game's own immediate context only, before every other consumer of the
// draw. True = the plate triangles were drawn into the frame's plate image and the native draw must be skipped (the
// frame is hidden). False = draw the draw natively: it is not a plate draw, or this frame is not hidden, or the redirect
// refused (fail closed). `original` is the real DrawIndexed the hook replaced; the redirect draws through it so the
// plate image never re-enters the hooks.
inline bool InterceptIndexed(ID3D11DeviceContext* context, unsigned count, unsigned start, int baseVertex, void* original)
{
    if (!ext::Active(ext::F::Plate))
        return false;
    return detail::InterceptIndexedSlow(context, count, start, baseVertex, original);
}

// Counters for the log and the diagnostics probe (cumulative while the key is armed).
struct Counters
{
    uint64_t drawsSeen = 0;      // indexed draws on the game's context while the key was on
    uint64_t drawsMatched = 0;   // ... that matched every plate-draw condition
    uint64_t reserveAsks = 0;    // frames that asked the engine for a slot
    uint64_t framesDeclined = 0; // ... that the engine refused: the frame stays fully native
    uint64_t drawsShadowed = 0;  // plate draws also drawn into the image of a frame the game draws itself (warm-up)
    uint64_t drawsHidden = 0;    // plate draws drawn into the image only (the game's own are skipped)
    uint64_t drawsFallback = 0;  // plate draws the redirect refused: the game drew them, the frame may be hidden
    uint64_t framesFinished = 0; // frames the engine finished through this module's hook
};
Counters Snapshot();
} // namespace ext::ffxiv::plates
