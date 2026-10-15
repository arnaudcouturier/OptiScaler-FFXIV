#pragma once
// ext-internal: the HDR half of ext::fg::SetResourcePre (ext/hdr/FgHdr.cpp), called from ext/fg/GuideSeams.cpp.

#include "ext/Seams.h"

namespace ext::hdr
{
// DLSS-G / XeFG SetResource, before the guide matcher. Raw scene HUD-less capture (HDR.Mode 1), UI tag drop and the
// PQ HUD-less capture, as the reference build's inline blocks. `storage` receives the capture copy; `input` is
// repointed at it. False = the caller returns false (the UI input was dropped: `_noUi` is set).
bool SetResourceHdr(IFGFeature_Dx12* fg, int fIndex, Dx12Resource*& input, Dx12Resource& storage, fg::Provider provider);
} // namespace ext::hdr
