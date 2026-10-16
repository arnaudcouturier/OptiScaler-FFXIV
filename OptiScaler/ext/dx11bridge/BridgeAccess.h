#pragma once
// ext::Access::Bridge: the fork's window into the private state of Dx11wDx12SC (with_dx12/dx11_with_dx12_sc.h, friend
// `ext::Access`). A nested class of the friend has its rights. Included only by ext/dx11bridge/*.cpp and ext/hdr/*.cpp;
// upstream files never see it.

#include "ext/Seams.h"

#include <upscalers/IFeature_Dx11wDx12.h>
#include <with_dx12/dx11_with_dx12_sc.h>

struct ext::Access::Bridge
{
    static ext::dx11bridge::BridgeExt& Ext(Dx11wDx12SC& sc)
    {
        if (!sc._ext)
            sc._ext = std::make_unique<ext::dx11bridge::BridgeExt>();
        return *sc._ext;
    }
    static bool HdrOutput(const Dx11wDx12SC& sc) { return sc._ext && sc._ext->hdrOutput; }

    // C4: lazily creates the per-feature forced-quality colour scaler state (upscalers/IFeature_Dx11wDx12.h `_ext`).
    static ext::dx11bridge::BridgeState& Ext(IFeature_Dx11wDx12& feature)
    {
        if (!feature._ext)
            feature._ext = std::make_unique<ext::dx11bridge::BridgeState>();
        return *feature._ext;
    }

    // C4 (Fix 2): ext::dx11bridge::PrepareUpscalerColor needs this frame's actual render size, not the (one-frame-
    // stale) IFeature_Dx11wDx12::RenderWidth()/RenderHeight() public accessor -- it resolves it itself through
    // ext::upscaler::ResolveRenderGeometry, which needs the same feature identity CorrectRenderSubrect passes
    // moments later (dx12Feature.get(), not `&feature`) so UpscalerSeams.cpp's per-feature geometry-log dedup state
    // isn't bounced between two different pointers for what is really one logical feature.
    static IFeature* InnerFeature(IFeature_Dx11wDx12& feature) { return feature.dx12Feature.get(); }

    static IDXGISwapChain4* FgSwapChain(Dx11wDx12SC& sc) { return sc._fgSwapChain; }
    static DXGI_FORMAT BufferFormat(const Dx11wDx12SC& sc) { return sc._bufferFormat; }
    static ID3D12Device* Dx12Device(Dx11wDx12SC& sc) { return sc._dx12Device; }
    static ID3D12GraphicsCommandList* CopyList(Dx11wDx12SC& sc, UINT slot) { return sc._copyCommandLists[slot]; }
    static HWND Window(Dx11wDx12SC& sc) { return sc._handle; }
    static IFGFeature_Dx12* Fg(Dx11wDx12SC& sc) { return sc._fg; }
};
