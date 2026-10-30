#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/fg/Dx12Access.h"

// FSR FG frame IDs (rule: ext/fg/FsrFrameId.h). The provider's dispatch selector resyncs onto the game's frame counter, so
// the ID it hands out jumps whenever the game skipped a frame or its counter fell back; FSR resets its interpolation history
// on any gap. With [FrameGen] FsrMonotonicFrameId on, FSR is given an ID from its own space that rises by exactly 1 per
// dispatch, and the two provider callbacks ask this seam which buffer slot the dispatch behind their ID used.
//
// Off (the default) both calls answer exactly what the provider did before: the game's own frame ID, and the slot derived
// from it. Not called by the Vulkan provider or by DLSS-G / XeFG.

namespace ext::fg
{
namespace
{
bool MonotonicIdsOn() { return ext::Cfg().fg.fsrMonotonicFrameId.value_or_default(); }
} // namespace

std::uint64_t FsrFrameId(IFGFeature_Dx12* fg, std::uint64_t gameFrame, int slot)
{
    if (fg == nullptr || !MonotonicIdsOn())
        return gameFrame;
    return Access::FgDx12::Ext(*fg).fsrFrameIds.Next(gameFrame, slot);
}

int FsrFrameIndex(IFGFeature_Dx12* fg, std::uint64_t id)
{
    if (fg != nullptr && MonotonicIdsOn())
    {
        const int slot = Access::FgDx12::Ext(*fg).fsrFrameIds.Slot(id);
        if (slot >= 0)
            return slot;
    }
    // Fail open: the provider's own rule, which is the only answer while the key is off and the right one for an ID this
    // object never recorded.
    return static_cast<int>(id % BUFFER_COUNT);
}
} // namespace ext::fg
