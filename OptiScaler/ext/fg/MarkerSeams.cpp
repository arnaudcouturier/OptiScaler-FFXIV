#include "pch.h"

#include "ext/Seams.h"
#include "ext/game/Game.h"

#include <framegen/IFGFeature.h>

// D2, C5: the game module's FG markers (ext::game::IFgMarkers, null for every game but FFXIV). A pointer test per call.
// (fg::OnUpscaleStart is in GuideSeams.cpp: it also hands the guide delay to the matcher.)

namespace ext::fg
{
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
