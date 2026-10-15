#pragma once
// B5: state of the provider-neutral presentation-guide matcher (Dx12Ext::guides). Private to ext/fg/*.cpp.
//
// When a game module asks for a guide age N (ext::game::IFgMarkers::PresentationGuideDelay), the incoming Depth and
// Velocity inputs of the FG provider are replaced by the scene-matched snapshot taken N frames ago, together with the
// camera/jitter metadata captured with it. The snapshots are copies recorded on the FG UI command list, so
// they stay valid until their retirement fence passes.
//
// NR-side consumer (S2b): DlssNr::ConsumePresentationGuideDelay() is what the game module's
// IFgMarkers::PresentationGuideDelay() forwards to; nothing in ext/fg depends on NR.

#include "ext/fg/TemporalContinuity.h"

#include <framegen/IFGFeature_Dx12.h>

#include <vector>

namespace ext::fg
{
struct GuideSnapshot
{
    ID3D12Resource* resource = nullptr;
    Dx12Resource view {};
    UINT64 frame = 0;
    bool valid = false;
};

struct GuideMetadata
{
    float values[9] {};
    float vectors[4][3] {};
    double delta = 0;
    UINT reset = 0;
};

struct RetiredGuide
{
    ID3D12Resource* resource;
    ID3D12Fence* fence;
    UINT64 value;
};

struct GuideState
{
    GuideMetadata metadata[BUFFER_COUNT] {};
    GuideSnapshot snapshots[BUFFER_COUNT][2] {};
    int age[BUFFER_COUNT] { -1, -1, -1, -1 };
    std::vector<RetiredGuide> retired;
    SuccessfulFrames sceneHistory;
    UINT64 resetFrame = UINT64_MAX;
};
} // namespace ext::fg
