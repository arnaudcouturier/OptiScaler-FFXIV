#pragma once
// What the game side hands the engine when a real frame finishes, and one plate that moves as a rigid unit. The engine
// pulls the frame finish at the start of its own frame finish (the present seam), so the game side keeps no state the
// engine has to lock: it fills a PlateFrame from its own draw records while the render thread is between draws.
//
// Without a registered finish the engine publishes the frame with no movers, which the pass draws unshifted (the
// endpoint mode). A mover's offsets are already in image pixels.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ext::hudless::plates
{
// One plate that moves as a rigid unit, in image pixels. The pass draws the source rectangle shifted by the per-output
// share of (offX, offY), or unshifted when endpoint is set.
struct Mover
{
    float srcX0 = 0, srcY0 = 0, srcX1 = 0, srcY1 = 0;
    float offX = 0, offY = 0;
    bool endpoint = true;
};
inline constexpr size_t kMaxMovers = 64;

// One real frame's tracked movers.
struct PlateFrame
{
    // Image pixels per vertex unit of the tracker's rectangles (0 or 1 = already in pixels).
    float viewportScale = 1.0f;
    unsigned moverCount = 0;
    Mover movers[kMaxMovers];
};

// The game side's frame finish, called on the render thread when the engine finishes the frame. `user` is whatever was
// registered with it. A null hook means "no movers": the pass draws the image unshifted.
using PlateFinishHook = void (*)(PlateFrame& frame, void* user);

// Registered once at module load by the game side (ffxiv/plates). Null hooks are ignored, so an unregistered engine
// stays in endpoint.
void SetPlateFinishHook(PlateFinishHook hook, void* user);
// The hook the engine calls (null when none is registered) and its user pointer.
PlateFinishHook PlateFinish();
void* PlateFinishUser();

// The outputs the real frame being dispatched was dispatched for (generated frames + 1; 0 = the driver picks the
// count). Written by the frame generation seam from any thread and read once per frame finish: an inline atomic so the
// seam and its test need no plate translation unit.
inline std::atomic<unsigned> g_expectedOutputs { 0 };
inline void NoteMultiplier(unsigned outputsPerFrame)
{
    g_expectedOutputs.store(outputsPerFrame, std::memory_order_relaxed);
}
inline unsigned ExpectedMultiplier() { return g_expectedOutputs.load(std::memory_order_relaxed); }
} // namespace ext::hudless::plates
