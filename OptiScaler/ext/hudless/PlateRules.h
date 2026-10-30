#pragma once
// The output pass's rule over the drawable record, before it touches any D3D. Header-only and free of D3D, Config,
// State and the logger so the CPU tests build it directly.
//
// The pass draws the newest record's plate image only when the record says the frame's plate draws were hidden. A
// record whose frame was drawn natively (or declined) sits out: drawing an older hidden image over plates the game
// itself drew is the double draw this rule exists to prevent. A shadow record proves the pass end to end (the submit
// runs with zero instances) without drawing anything.

#include <cstdint>

namespace ext::hudless::plates
{
enum class PlatePrelim
{
    Stale,  // no record, or older than the freshness window: sit out, the game's own plates come back
    Native, // the frame was drawn natively (or declined): sit out, never draw an older image over it
    NoSlot, // the record's copy is not proven complete: sit out, never draw a guess
    Shadow, // the frame's image was kept but the frame was drawn natively: submit zero instances
    Ready,  // the frame was hidden and the image is ready: draw it
};

inline PlatePrelim PrelimPlateRecord(bool fresh, bool native, bool shadow, bool slotReady)
{
    if (!fresh)
        return PlatePrelim::Stale;
    if (native)
        return PlatePrelim::Native;
    if (shadow)
        return PlatePrelim::Shadow;
    if (!slotReady)
        return PlatePrelim::NoSlot;
    return PlatePrelim::Ready;
}
} // namespace ext::hudless::plates
