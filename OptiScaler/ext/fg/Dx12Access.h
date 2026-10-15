#pragma once
// ext::Access::FgDx12: the fork's window into the protected/private state of IFGFeature_Dx12 and its derived classes.
// A nested class of the friend `ext::Access` has Access's rights, so the member functions below can name
// `fg._scCommandList` etc. directly. Included only by ext/fg/*.cpp; upstream files never see it.

#include "ext/Seams.h"

#include <framegen/IFGFeature_Dx12.h>

struct ext::Access::FgDx12
{
    static ext::fg::Dx12Ext& Ext(IFGFeature_Dx12& fg) { return *fg._ext; }

    // --- C6: SC command list ownership (Dx12Sc.cpp)
    static bool WaitForSCAllocator(IFGFeature_Dx12& fg, UINT index);
    static bool PrepareSC(IFGFeature_Dx12& fg, UINT index);
    static bool SubmitSC(IFGFeature_Dx12& fg, UINT index);

    // --- B5: guide matcher (GuideMatcher.cpp)
    static ext::fg::GuideState& Guides(IFGFeature_Dx12& fg);
    static void SetPresentationGuideDelay(IFGFeature_Dx12& fg, int age);
    static bool MatchPresentationGuide(IFGFeature_Dx12& fg, Dx12Resource& input, int index);
    static void RetireGuide(IFGFeature_Dx12& fg, ID3D12Resource*& resource);
    static void CollectGuides(IFGFeature_Dx12& fg);
    static void ReleaseGuides(IFGFeature_Dx12& fg);
    static void InvalidateGuides(IFGFeature_Dx12& fg);
    static int GuideAge(IFGFeature_Dx12& fg, int index);

    // --- C8: OptiHDR (hdr/FgHdr.cpp)
    static ID3D12Device* Device(IFGFeature_Dx12& fg) { return fg._device; }
    static bool& NoUi(IFGFeature_Dx12& fg, int index) { return fg._noUi[index]; }
};
