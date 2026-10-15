#pragma once
// ext::Access::FgDx12: the fork's window into the protected/private state of IFGFeature_Dx12 and its derived classes.
// A nested class of the friend `ext::Access` has Access's rights, so the member functions below can name
// `fg._scCommandList` etc. directly. Included only by ext/fg/*.cpp; upstream files never see it.

#include "ext/Seams.h"

#include <framegen/IFGFeature_Dx12.h>

struct ext::Access::FgDx12
{
    // --- C6: SC command list ownership (Dx12Sc.cpp)
    static bool WaitForSCAllocator(IFGFeature_Dx12& fg, UINT index);
    static bool PrepareSC(IFGFeature_Dx12& fg, UINT index);
    static bool SubmitSC(IFGFeature_Dx12& fg, UINT index);

    static ext::fg::Dx12Ext& Ext(IFGFeature_Dx12& fg) { return *fg._ext; }
};
