#pragma once
// Seams of the HDR group (SEAMS-catalog group `hdr` and the HDR rows of `fg`, `dx11bridge`, `present`, `menu`),
// included by ext/Seams.h. State: ext/hdr/*. Everything is inert (one feature-bit test) until OptiHDR engaged.

#include "ext/ExtFeatures.h"

namespace ext::hdr
{
// Out-of-line half of Active(): nightly's `hdrOutputActive` (State), filled for the DX11 bridge by
// ext::dx11bridge::SetColorSpace / Hdr10 Configure. `isHdrActive` of the reference build does not exist any more.
bool OutputIsHdr();

// True while OptiHDR is engaged: the bridge negotiated PQ/BT.2020 on the FG swapchain (feature bit F::Hdr) and
// nightly's `hdrOutputActive` is true. Was Hdr10::Active().
inline bool Active() { return ext::Active(ext::F::Hdr) && OutputIsHdr(); }
} // namespace ext::hdr
