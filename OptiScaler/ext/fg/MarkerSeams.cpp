#include "pch.h"

#include "ext/Seams.h"
#include "ext/game/Game.h"

#include <framegen/IFGFeature.h>

// D2, C5: the game module's FG markers (ext::game::IFgMarkers, null for every game but FFXIV). A pointer test per call.

namespace ext::fg
{
void OnUpscaleStart(IFGFeature* fg)
{
    if (auto* markers = game::Current().fgMarkers)
        markers->OnUpscale(fg->FrameCount());
}

void MarkersBeforePresent(uint32_t frameId)
{
    if (auto* markers = game::Current().fgMarkers)
        markers->BeforePresent(frameId);
}

void MarkersNextFrame(uint64_t frame)
{
    if (auto* markers = game::Current().fgMarkers)
        markers->NextFrame(frame);
}
} // namespace ext::fg
