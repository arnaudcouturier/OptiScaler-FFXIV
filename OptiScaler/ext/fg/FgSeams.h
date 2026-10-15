#pragma once
// Seams of the frame-generation group (SEAMS-catalog groups `fg` and `streamline`), included by ext/Seams.h.
// Per-object state: ext::fg::Dx12Ext (IFGFeature_Dx12::_ext), ext/fg/Dx12Ext.h. Implementations: ext/fg/*.cpp.

#include <d3d12.h>

class IFGFeature_Dx12;

namespace ext::fg
{
// --- C6 (Dx12Sc.cpp)
// DLSSG_Dx12, XeFG_Dx12, FSRFG_Dx12 `Present`: close and submit the SC list in order (monotonic completion fence).
// False = the submission failed (further SC rendering is off): `return false` from Present.
bool SubmitSC(IFGFeature_Dx12* fg, UINT index);
// IFGFeature_Dx12::GetSCCommandList, after the stale lists were discarded: leaves the list at `index` open and ready
// (failed-submission and null guards, allocator fence wait, resets). False -> return nullptr.
bool PrepareSC(IFGFeature_Dx12* fg, UINT index);
} // namespace ext::fg
