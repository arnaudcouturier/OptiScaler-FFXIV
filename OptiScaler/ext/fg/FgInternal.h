#pragma once
// ext/fg-internal declarations (not seams): shared between the ext/fg translation units only.

#include "ext/fg/DynamicMfgControl.h"

class IFGFeature;

namespace sl
{
struct DLSSGOptions;
}

namespace ext::fg
{
// D2: latest observed output frames per real frame of the DLSS-G Dx12 output, 0 = none / stale (> 3 s).
unsigned ObservedMultiplier(IFGFeature* fg);
// E3: restart the DLSS-G presentation telemetry window (DLSS-G Dx12 activation).
void ResetDlssgPresentStats();

// Dynamic MFG on RTX 40 (DynamicMfgSeams.cpp). AdjustDlssgOptions: one tick per game frame; true = the automatic
// selection (or the self-test) is in charge of this frame's count, `options` may have been changed.
bool AutoMfgTickFrame(IFGFeature* fg, sl::DLSSGOptions& options);
// AfterDlssgSetOptions: what GetState said right after this frame's options, for the next tick's kill-switch.
void AutoMfgObserve(bool queryOk, uint32_t status, uint32_t presented);
// DLSS-G (re)activated (OnActivated): new observation window; the kill-switch's memory stays.
void ResetAutoMfg();
// For the menu card.
AutoMfgStatus AutoMfgGetStatus();
} // namespace ext::fg
