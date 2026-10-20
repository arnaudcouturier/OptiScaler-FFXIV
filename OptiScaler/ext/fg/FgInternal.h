#pragma once
// ext/fg-internal declarations (not seams): shared between the ext/fg translation units only.

#include "ext/fg/DynamicMfgControl.h"

class IFGFeature;

namespace sl
{
struct DLSSGOptions;
struct DLSSGState;
}

namespace ext::fg
{
// D2: latest observed output frames per real frame of the DLSS-G Dx12 output, 0 = none / stale (> 3 s).
unsigned ObservedMultiplier(IFGFeature* fg);
// E3: restart the DLSS-G presentation telemetry window (DLSS-G Dx12 activation).
void ResetDlssgPresentStats();

// Dynamic MFG on RTX 40 (DynamicMfgSeams.cpp, logic in DynamicMfgGlue.h). AdjustDlssgOptions: one call per game frame; the
// automatic selection (or the self-test) may change `options`, then the ForceDMFG / Ada-ceiling guard runs on the result.
void AutoMfgAdjustOptions(IFGFeature* fg, sl::DLSSGOptions& options);
// AfterDlssgSetOptions: what GetState said right after this frame's options, for the next tick's kill-switch.
void AutoMfgObserve(bool queryOk, const sl::DLSSGState& state);
// DLSS-G (re)activated (OnActivated): new observation window; the kill-switch's memory stays.
void ResetAutoMfg();
// For the menu card.
AutoMfgStatus AutoMfgGetStatus();

// WS2: the DLSS-G render preset section of the fg_unlocks card (MenuFgPreset.cpp).
void DrawRenderPresetSection();
} // namespace ext::fg
