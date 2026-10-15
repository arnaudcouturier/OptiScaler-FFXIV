#pragma once
// Link to the hud-less source engine (C11, owner S10, ext/hudless). The FG code only needs these four facts to decide
// DLSS-G UI recomposition (DlssgSeams.cpp). Until the engine exists Query() reports "no source", which keeps
// recomposition off, as the reference build does without its add-on.
//
// S10: replace the body of Query() (HudlessEngine::SourceActive(), the format of the last delivered hud-less image) and
// bind the two [FrameGen] keys named below in the engine's config struct, then delete HudlessLink.cpp.

#include <cstdint>

namespace ext::fg::hudless
{
struct Link
{
    bool sourceActive = false; // a hud-less source delivered a frame (was ExternalHudless::Active())
    uint32_t lastFormat = 0;   // DXGI_FORMAT of the last delivered hud-less image, 0 = none
    bool freeFrameGen = true;  // [FrameGen] ExternalUIFreeFrameGen
    bool uiFromHudless = true; // [FrameGen] ExternalUIFromHudless
};

Link Query();
} // namespace ext::fg::hudless
