#pragma once
// ext/fg-internal declarations (not seams): shared between the ext/fg translation units only.

class IFGFeature;

namespace ext::fg
{
// D2: latest observed output frames per real frame of the DLSS-G Dx12 output, 0 = none / stale (> 3 s).
unsigned ObservedMultiplier(IFGFeature* fg);
} // namespace ext::fg
