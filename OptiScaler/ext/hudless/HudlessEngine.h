#pragma once
// The HUD-less engine's public face for the menu and the DLSS-G option seam (docs/HUDLESS.md sections 5, 9.1, 11). The
// frame flow itself is driven by the seams in HudlessSeams.h; the decision logic is HudlessCore.h.

#include "ext/hudless/HudlessCore.h"

#include <cstdint>

namespace ext::hudless
{
// Copy of the engine's state, safe from any thread (the menu).
Status GetStatus();

// What DLSS-G's feature creation needs (ext/fg/dlssg/DlssgSeams.cpp AdjustUiRecomposition). `enabled` is mode driven
// (Mode == recomposition and a game source exists), not "a frame was delivered": nvngx_dlssg reads it when the feature
// is created, before any capture exists. `format` is the DXGI_FORMAT to declare for the hud-less buffer, 0 = leave the
// default; `formatSource` says where it came from for the log ("last capture", "bridge buffer", "default").
struct Recomposition
{
    bool enabled = false;
    uint32_t format = 0;
    const char* formatSource = "default";
};
Recomposition QueryRecomposition();

// Shown in the menu next to the state: the displayed phase and reason as one line, e.g. "Fallback(MaskCoverage)"
std::string StatusLine(const Status& status);
} // namespace ext::hudless
